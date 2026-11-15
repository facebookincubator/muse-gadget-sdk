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

"""The TriviaPOD runs the StopWatch's board file and its controls."""

import re
import unittest
from pathlib import Path

ESP32 = Path(__file__).resolve().parents[1]
MUSE = ESP32 / "components/muse"


class TriviaPodSharesStopWatch(unittest.TestCase):
    def test_one_board_file_for_both(self):
        cmake = (MUSE / "CMakeLists.txt").read_text()
        self.assertIn('elseif(CONFIG_MUSE_BOARD_M5STACK_STOPWATCH_FAMILY)\n'
                      '        list(APPEND srcs "boards/board_m5stack_stopwatch.c"', cmake)
        self.assertFalse((MUSE / "boards/board_m5stack_triviapod.c").exists())
        board = (MUSE / "boards/board_m5stack_stopwatch.c").read_text()
        self.assertIn('#if CONFIG_MUSE_BOARD_M5STACK_TRIVIAPOD\n    .name = "M5Stack TriviaPOD",', board)

    def test_family_covers_both_boards(self):
        kconfig = (MUSE / "Kconfig").read_text()
        self.assertIn("default y if MUSE_BOARD_M5STACK_STOPWATCH || MUSE_BOARD_M5STACK_TRIVIAPOD", kconfig)

    def test_shared_controls_key_on_the_family(self):
        # A TriviaPOD build swaps keys like the StopWatch (board file), so the
        # shared push-to-talk call controls must apply to it too.
        for name in ("muse_ui.c", "muse_voice.c", "muse_settings.c", "muse_input.c", "muse_settings_ui.c"):
            with self.subTest(file=name):
                src = (MUSE / name).read_text()
                self.assertIsNone(re.search(r"CONFIG_MUSE_BOARD_M5STACK_STOPWATCH\b(?!_)", src))
                self.assertIn("CONFIG_MUSE_BOARD_M5STACK_STOPWATCH_FAMILY", src)

    def test_overlay_selects_the_triviapod(self):
        overlay = (ESP32 / "devices/sdkconfig.muse-m5stack-triviapod").read_text()
        self.assertIn("CONFIG_MUSE_BOARD_M5STACK_TRIVIAPOD=y", overlay)
        self.assertIn('CONFIG_ESPTOOLPY_FLASHSIZE="32MB"', overlay)

    def test_overlay_keeps_the_stopwatch_ram_headroom(self):
        # Same chip, PSRAM and board file: without these, the 8 KB setup tasks
        # and link.register run short of internal RAM as on the StopWatch.
        stopwatch = (ESP32 / "devices/sdkconfig.muse-m5stack-stopwatch").read_text()
        overlay = (ESP32 / "devices/sdkconfig.muse-m5stack-triviapod").read_text()
        for line in ("CONFIG_HOMEHUB_SETUP_TASKS_IN_PSRAM=y", "CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=1024"):
            with self.subTest(line=line):
                self.assertIn(line, stopwatch)
                self.assertIn(line, overlay)


if __name__ == "__main__":
    unittest.main()
