# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""Run the actual motion classifier, runtime and I2C driver against host fakes."""
import os
import json
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
MUSE = ROOT / "components/muse"


class MotionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        tmp = Path(cls.tmp.name)
        (tmp / "freertos").mkdir()
        (tmp / "sdkconfig.h").write_text("#define CONFIG_MUSE_MOTION_GESTURES 1\n")
        (tmp / "esp_err.h").write_text(
            '#include "' + str(ROOT / "tests/pmu_fakes/esp_err.h") + '"\n'
            '#define ESP_ERR_NOT_FOUND 0x105\n#define ESP_ERR_NOT_FINISHED 0x10c\n')
        (tmp / "freertos/FreeRTOS.h").write_text("#define pdMS_TO_TICKS(ms) (ms)\n")
        (tmp / "freertos/task.h").write_text("void vTaskDelay(int ms);\n")
        (tmp / "esp_log.h").write_text("#define ESP_LOGW(...) ((void)0)\n")
        (tmp / "muse_settings.h").write_text(
            "#include <stdbool.h>\nbool muse_settings_lift_wake(void);\n"
            "bool muse_settings_face_down_quiet(void);\n")
        (tmp / "muse_board.h").write_text(
            '#pragma once\n#include "esp_err.h"\n#include "muse_motion_core.h"\n'
            'typedef struct { esp_err_t (*motion_enable)(bool); '
            'esp_err_t (*motion_read)(muse_motion_sample_t *); void *mic_init; } muse_board_t;\n'
            'extern const muse_board_t *muse_board;\nbool muse_state_asleep(void);\n')
        # Copy verbatim so quoted board/settings includes resolve to the test
        # interfaces; classifier and driver compile directly from the tree.
        (tmp / "muse_motion.c").write_text((MUSE / "muse_motion.c").read_text())
        audio = (MUSE / "muse_audio.c").read_text()
        (tmp / "motion_audio_write.inc").write_text(
            audio[audio.index("esp_err_t muse_audio_write_chained("):audio.index("float muse_audio_dbfs(")])
        cls.binary = tmp / "motion"
        built = subprocess.run([
            *shlex.split(os.environ.get("CC", "cc")), "-std=c11", "-Wall", "-Wextra", "-Werror",
            "-Wno-unused-parameter", "-fsanitize=undefined,address",
            "-I", str(tmp), "-I", str(MUSE), "-I", str(MUSE / "boards"),
            "-I", str(ROOT / "tests/pmu_fakes"), str(ROOT / "tests/motion_harness.c"),
            str(MUSE / "muse_motion_core.c"), str(tmp / "muse_motion.c"),
            str(MUSE / "boards/qmi8658_motion.c"), "-lm", "-o", str(cls.binary),
        ], capture_output=True, text=True)
        if built.returncode:
            raise AssertionError(built.stdout + built.stderr)

    def test_face_up_pickup_gradual_tilt_noise_stale_samples_and_clock_rollover(self):
        subprocess.run([str(self.binary), "core"], check=True)

    def test_opt_in_quiet_recovery_poll_budget_and_sensor_failure(self):
        subprocess.run([str(self.binary), "runtime"], check=True)

    def test_sensor_identity_configuration_signed_samples_and_i2c_errors(self):
        subprocess.run([str(self.binary), "driver"], check=True)

    def test_actual_playback_mutes_temporarily_and_preserves_saved_speaker_setting(self):
        subprocess.run([str(self.binary), "audio"], check=True)

    def test_sleep_cancels_an_old_pickup_and_the_next_pickup_wakes_once(self):
        result = subprocess.check_output([str(self.binary), "sleep"], text=True)
        status = json.loads(result.removeprefix("@motion "))
        self.assertTrue(status["asleep"])
        self.assertEqual(status["wakes"], 1)

    def test_lowering_landing_bumps_and_aborted_pickups_do_not_wake(self):
        subprocess.run([str(self.binary), "lowering"], check=True)

    def replay(self, name):
        rows = (ROOT / "tests" / ("motion_" + name + "_capture.csv")).read_text().splitlines()
        data = "\n".join(row.replace(",", " ") for row in rows if not row.startswith("#"))
        result = subprocess.check_output([str(self.binary), "replay"], input=data, text=True)
        return [int(ms) for ms in result.splitlines()]

    def test_captured_failed_pickup_wakes_before_the_acceleration_burst(self):
        wakes = self.replay("pickup")
        self.assertEqual(len(wakes), 1)
        self.assertLessEqual(wakes[0], 850)  # first physical acceleration burst is at 853 ms

    def test_captured_putdown_does_not_wake_after_landing(self):
        self.assertEqual(self.replay("putdown"), [])

    def test_isolated_physical_lowering_is_not_normalized_into_a_pickup(self):
        self.assertEqual(self.replay("lowering"), [])

    def test_table_contact_after_a_long_lowering_does_not_rearm_midair(self):
        self.assertEqual(self.replay("landing"), [])

    def test_physical_cycle_ignores_landing_but_wakes_on_the_next_upward_start(self):
        wakes = self.replay("cycle")
        self.assertEqual(len(wakes), 1)
        self.assertGreaterEqual(wakes[0], 16500)  # actual repickup starts at 16.55 s
        self.assertLessEqual(wakes[0], 16800)
