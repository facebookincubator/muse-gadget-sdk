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

"""The TriviaPOD gets the StopWatch's call controls."""

import re
import unittest
from pathlib import Path

MUSE = Path(__file__).resolve().parents[1] / "components/muse"


class TriviaPodControls(unittest.TestCase):
    def test_family_covers_both_boards(self):
        kconfig = (MUSE / "Kconfig").read_text()
        self.assertIn("config MUSE_BOARD_M5STACK_STOPWATCH_FAMILY\n        bool\n"
                      "        default y if MUSE_BOARD_M5STACK_STOPWATCH || MUSE_BOARD_M5STACK_TRIVIAPOD", kconfig)

    def test_shared_controls_key_on_the_family(self):
        # With live calls on, the TriviaPOD's board file swaps its keys like the
        # StopWatch's, so the shared push-to-talk call controls must apply too.
        for name in ("muse_ui.c", "muse_voice.c", "muse_settings.c", "muse_input.c", "muse_settings_ui.c"):
            with self.subTest(file=name):
                src = (MUSE / name).read_text()
                self.assertIsNone(re.search(r"CONFIG_MUSE_BOARD_M5STACK_STOPWATCH\b(?!_)", src))
                self.assertIn("CONFIG_MUSE_BOARD_M5STACK_STOPWATCH_FAMILY", src)


if __name__ == "__main__":
    unittest.main()
