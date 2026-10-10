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

"""The SenseCAP Watcher's battery rest: CPU power-down in light sleep with its
retention memory taken at boot, the internal RAM moved to PSRAM to pay for it,
and the codecs suspended whenever the board rests (or the voice task can't
start)."""

from __future__ import annotations

import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MUSE = ROOT / "components" / "muse"
OVERLAY = ROOT / "devices" / "sdkconfig.muse-sensecap-watcher"
INPUT = MUSE / "muse_input.c"


def settings(path: Path) -> dict[str, str]:
    """CONFIG_X=v lines, and "# CONFIG_X is not set" as "n"."""
    out = {}
    for line in path.read_text().splitlines():
        if m := re.fullmatch(r"(CONFIG_\w+)=(.*)", line):
            out[m[1]] = m[2]
        elif m := re.fullmatch(r"# (CONFIG_\w+) is not set", line):
            out[m[1]] = "n"
    return out


def body(source: str, signature: str) -> str:
    """The braces after `signature`."""
    start = source.index("{", source.index(signature))
    depth = 0
    for i in range(start, len(source)):
        depth += {"{": 1, "}": -1}.get(source[i], 0)
        if depth == 0:
            return source[start:i + 1]
    raise AssertionError(f"unbalanced {signature}")


class WatcherSleepTest(unittest.TestCase):
    def test_overlay(self) -> None:
        s = settings(OVERLAY)
        self.assertEqual(s.get("CONFIG_PM_POWER_DOWN_CPU_IN_LIGHT_SLEEP"), "y")
        self.assertEqual(s.get("CONFIG_PM_RESTORE_CACHE_TAGMEM_AFTER_LIGHT_SLEEP"), "y")
        self.assertEqual(s.get("CONFIG_MUSE_STATIC_BUFFERS_IN_PSRAM"), "y")
        # What that needs, and what was measured with it.
        for key in ("CONFIG_PM_ENABLE", "CONFIG_FREERTOS_USE_TICKLESS_IDLE", "CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY",
                    "CONFIG_ESP32S3_DATA_CACHE_64KB", "CONFIG_BT_CTRL_MODEM_SLEEP",
                    "CONFIG_BT_CTRL_MAIN_XTAL_PU_DURING_LIGHT_SLEEP", "CONFIG_PM_LIGHT_SLEEP_CALLBACKS"):
            self.assertEqual(s.get(key), "y", key)
        # Floating every pin in light sleep measured 0.10 mA worse.
        self.assertEqual(s.get("CONFIG_ESP_SLEEP_GPIO_RESET_WORKAROUND"), "n")
        self.assertNotIn("CONFIG_PM_SLP_DISABLE_GPIO", s)

    def test_buffers_in_psram(self) -> None:
        kconfig = (MUSE / "Kconfig").read_text()
        option = kconfig[kconfig.index("config MUSE_STATIC_BUFFERS_IN_PSRAM"):]
        option = option[:option.index("\n    config ", 1)]
        self.assertIn("depends on MUSE_ENABLED && SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY", option)
        self.assertIn("default n", option)   # other boards keep them internal
        lf = (MUSE / "linker.lf").read_text()
        self.assertIn("bss -> extern_ram", lf)
        self.assertIn("common -> extern_ram", lf)
        mapped = lf[lf.index("if MUSE_STATIC_BUFFERS_IN_PSRAM = y:"):lf.index("else:")]
        self.assertIn("muse_pixel (muse_extram_bss)", mapped)
        self.assertIn("muse_audio (muse_extram_bss)", mapped)
        self.assertIn('LDFRAGMENTS "linker.lf"', (MUSE / "CMakeLists.txt").read_text())

    def test_resting_suspends_both_codecs(self) -> None:
        voice = (MUSE / "muse_voice.c").read_text()
        self.assertIn("muse_audio_power(!rest);", body(voice, "static void set_resting(bool rest)"))
        audio = (MUSE / "muse_audio.c").read_text()
        off = body(audio, "void muse_audio_power(bool on)")
        off = off[off.index("} else {"):]
        self.assertIn("esp_codec_dev_close(s_spk);", off)
        self.assertIn("esp_codec_dev_close(s_mic);", off)

    def test_codecs_suspended_without_the_voice_task(self) -> None:
        voice = (MUSE / "muse_voice.c").read_text()
        start = body(voice, "esp_err_t muse_voice_start(QueueHandle_t queue)")
        failed = start[start.index("xTaskCreatePinnedToCoreWithCaps(voice_task"):]
        failed = body(failed, "!= pdPASS)")
        self.assertIn("muse_audio_power(false);", failed)

    def test_retention_taken_early(self) -> None:
        # Before Link's radios and Muse's display: app_main starts Muse's glue
        # before app_run(), and the glue takes the memory before its tasks.
        main = (ROOT / "main" / "main.c").read_text()
        self.assertLess(main.index("muse_glue_start();"), main.index("app_run();"))
        glue = body((ROOT / "main" / "muse_glue.c").read_text(), "void muse_glue_start(void)")
        self.assertLess(glue.index("muse_input_power_init();"), glue.index("xTaskCreatePinnedToCore(boot_task"))
        init = body(INPUT.read_text(), "void muse_input_power_init(void)")
        self.assertIn("esp_sleep_cpu_retention_init()", init)
        self.assertIn('esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "muse_awake", &s_awake)', init)
        # The lock first, then light sleep on for good.
        self.assertLess(init.index("esp_pm_lock_acquire(s_awake);"), init.index("esp_pm_configure(&pm);"))
        self.assertIn(".light_sleep_enable = true,", init)
        self.assertIn("muse_battery_note_cpu_pd(s_cpu_pd);", init)

    def test_fallback(self) -> None:
        init = body(INPUT.read_text(), "void muse_input_power_init(void)")
        failed = body(init, "if (!s_cpu_pd)")
        for call in ("esp_pm_lock_release(s_awake);", "esp_pm_lock_delete(s_awake);",
                     "esp_sleep_cpu_retention_deinit();", "ESP_LOGW("):
            self.assertIn(call, failed)
        self.assertEqual(init.count("ESP_LOGW("), 1)   # once, at boot
        # Without the memory set_cpu_low() is what it was: light sleep on only while low.
        low = body(INPUT.read_text(), "static void set_cpu_low(bool low)")
        self.assertIn(".light_sleep_enable = low,", low)

    def test_lock_pairing(self) -> None:
        low = body(INPUT.read_text(), "static void set_cpu_low(bool low)")
        retained = body(low, "if (s_cpu_pd)")
        # With the memory, light sleep is never configured off (that frees it).
        self.assertIn("pm.light_sleep_enable = true;", retained)
        acquire = low.index("esp_pm_lock_acquire(s_awake);")
        release = low.index("esp_pm_lock_release(s_awake);")
        configure = low.index("esp_pm_configure(&pm);")
        self.assertLess(acquire, configure)    # awake: the lock before the full clock
        self.assertLess(configure, release)    # low: the lock goes after the low clock
        self.assertIn("if (!low && !s_awake_held)", low)
        self.assertIn("if (s_cpu_pd && low && s_awake_held && err == ESP_OK)", low)
        self.assertEqual(low.count("s_awake_held = true;"), 1)
        self.assertEqual(low.count("s_awake_held = false;"), 1)
        # Only the input task changes the clock: no other esp_pm_configure in Muse or Link.
        for path in list(MUSE.glob("*.c")) + list(MUSE.glob("boards/*.c")) + list((ROOT / "main").glob("*.c*")):
            if path.name != "muse_input.c":
                self.assertNotIn("esp_pm_configure(", path.read_text(), path.name)

    def test_deep_sleep_disarms_the_timer(self) -> None:
        # Light sleep stays configured, which leaves the timer wake armed.
        off = body(INPUT.read_text(), "static void power_off(void)")
        self.assertLess(off.index("esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);"),
                        off.index("muse_board->power_off()"))
        watcher = body((MUSE / "boards" / "board_sensecap_watcher.c").read_text(), "static void sleep_until_wheel(void)")
        self.assertLess(watcher.index("esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);"),
                        watcher.index("esp_deep_sleep_start();"))

if __name__ == "__main__":
    unittest.main()
