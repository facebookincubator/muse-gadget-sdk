# Copyright (c) Meta Platforms, Inc. and affiliates.
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy at http://www.apache.org/licenses/LICENSE-2.0
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
"""Compile actual diagnostic BSP lifecycle/hold functions against C fakes.

No hardware or IDF APIs run. Fakes model failed managed-wrapper opens, ignored
low-level disable errors, retained external codecs, and reset/torn pad journals.
"""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
BSP = ROOT / "components/muse/boards/board_sensecap_watcher.c"


def function_body(source, signature):
    start = source.index(signature)
    first = source.index("{", start)
    while ";" in source[start:first]:
        start = source.index(signature, start + len(signature))
        first = source.index("{", start)
    depth = 0
    for index in range(first, len(source)):
        depth += (source[index] == "{") - (source[index] == "}")
        if not depth:
            return source[start:index + 1], index + 1
    raise AssertionError(f"Unclosed function {signature}")


class WatcherPowerBspTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix="watcher-power-bsp-")
        directory = Path(cls.tmp.name)
        source = BSP.read_text()
        marker_start = source.index("#define PTEST_HOLD_MAGIC")
        _, marker_end = function_body(source, "bool muse_watcher_ptest_deep_holds_owned(void)")
        pieces = [source[marker_start:marker_end]]
        for signature in (
            "static esp_err_t ptest_codec_read(",
            "static esp_err_t ptest_adc_discover(void)",
            "static esp_err_t ptest_codec_snapshot(void)",
            "static esp_err_t ptest_verify_suspended(void)",
            "static esp_err_t ptest_audio_close(void)",
            "static esp_err_t ptest_audio_initialize(void)",
            "static esp_err_t ptest_audio_open(void)",
            "esp_err_t muse_watcher_ptest_prepare_deep(bool held)",
            "esp_err_t muse_watcher_ptest_release_deep_holds(void)",
        ):
            pieces.append(function_body(source, signature)[0])
        (directory / "watcher_power_bsp_functions.inc").write_text("\n\n".join(pieces))
        cls.binary = directory / "harness"
        compilation = subprocess.run(
            [os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
             "-I", str(ROOT / "tests/watcher_power_test_fakes"), "-I", str(directory),
             str(ROOT / "tests/watcher_power_bsp_harness.c"), "-o", str(cls.binary)],
            capture_output=True, text=True,
        )
        if compilation.returncode:
            raise AssertionError(compilation.stderr)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def scenario(self, mode):
        result = subprocess.run([str(self.binary), str(mode)], capture_output=True, text=True, check=True, timeout=5)
        self.assertEqual(result.stderr, "")
        return json.loads(result.stdout)

    def test_partial_constructor_failure_checked_shutdown_and_poison(self):
        self.assertNotEqual(self.scenario(0)["operation"], 0)

    def test_failed_wrapper_open_closes_nonnull_wrappers_and_poison(self):
        self.assertEqual(self.scenario(1)["close_calls"], 2)

    def test_constructor_only_state_is_not_forced_suspended(self):
        self.assertEqual(self.scenario(2)["force_calls"], 0)

    def test_warm_boot_without_wrappers_checked_then_allows_fresh_init(self):
        self.assertEqual(self.scenario(3)["force_calls"], 1)

    def test_ignored_wrapper_disable_error_caught_and_poisoned(self):
        self.assertNotEqual(self.scenario(4)["cleanup"], 0)

    def test_parked_audio_is_rejected_before_constructor(self):
        self.assertNotEqual(self.scenario(5)["operation"], 0)

    def test_reset_after_marker_before_first_hold_acquisition(self):
        self.assertNotEqual(self.scenario(6)["operation"], 0)

    def test_torn_large_journal_and_failed_release_retain_ownership(self):
        self.assertEqual(self.scenario(7)["cleanup"], 0)

    def test_partial_open_after_previous_stop_still_disables_owned_i2s(self):
        self.assertEqual(self.scenario(12)["operation"], 0)

    def test_repeated_resting_close_does_not_disable_stopped_i2s(self):
        self.assertEqual(self.scenario(11)["operation"], 0)

    def test_chip_value_mismatch_is_evidence_not_failure(self):
        self.assertEqual(self.scenario(9)["operation"], 0)

    def test_codec_i2c_transport_failure_still_fails_and_marks_missing_byte(self):
        self.assertEqual(self.scenario(10)["operation"], 263)

    def test_sticky_warm_proof_survives_constructor_reset_and_idle_reset(self):
        self.assertEqual(self.scenario(8)["force_calls"], 2)

    def test_hold_release_precedes_uart_and_restoration_has_readback(self):
        source = (ROOT / "components/muse/muse_watcher_power_test.c").read_text()
        startup, _ = function_body(source, "void muse_watcher_power_test_run(void)")
        self.assertLess(startup.index("muse_watcher_ptest_release_deep_holds()"), startup.index("uart_driver_install("))
        finish, _ = function_body(source, "static void finish_run(")
        self.assertIn("s_run.restore_error = fw_readback(&restored)", finish)
        readback, _ = function_body(BSP.read_text(), "esp_err_t muse_watcher_ptest_readback(")
        self.assertIn("ptest_codec_snapshot()", readback)
        self.assertNotIn("s_ptest_audio_attempted && s_ptest_codec_state == MUSE_PTEST_CODEC_SUSPENDED", readback)


if __name__ == "__main__":
    unittest.main()
