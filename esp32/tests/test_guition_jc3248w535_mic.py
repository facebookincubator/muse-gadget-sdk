# Copyright (c) Meta Platforms, Inc. and affiliates.
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy at http://www.apache.org/licenses/LICENSE-2.0
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""The Guition JC3248W535's added-mic options, read from the sources.

A configuration from before the mic choice set only
CONFIG_MUSE_JC3248W535_MIC_GPIO. Kconfig drops a value whose dependencies
are unmet, so the GPIO must not depend on the choice, and the choice must
pick I2S when that GPIO is set.
"""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
KCONFIG = ROOT / "components/muse/Kconfig"
BOARD = ROOT / "components/muse/boards/board_guition_jc3248w535.c"
END = re.compile(r"^\s*(?:(?:menu)?config|choice|endchoice|menu|endmenu|if|endif|comment|source)\b", re.M)


def entry(text, head):
    start = re.search(r"^\s*" + head + r"\s*$", text, re.M)
    if start is None:
        raise AssertionError(head + " not found")
    stop = END.search(text, start.end())
    return text[start.start():stop.start() if stop else len(text)]


class GuitionMicConfigTests(unittest.TestCase):
    def setUp(self):
        self.kconfig = KCONFIG.read_text(encoding="utf-8")
        self.board = BOARD.read_text(encoding="utf-8")

    def test_gpio_survives_without_the_choice(self):
        gpio = entry(self.kconfig, "config MUSE_JC3248W535_MIC_GPIO")
        self.assertRegex(gpio, r"depends on MUSE_BOARD_GUITION_JC3248W535\s")
        self.assertNotIn("MUSE_JC3248W535_MIC_I2S", gpio)
        self.assertNotIn("MUSE_JC3248W535_MIC_PDM", gpio)
        self.assertIn("range -1 48", gpio)
        self.assertRegex(gpio, r"default -1\b")

    def test_choice_picks_i2s_for_a_set_gpio(self):
        choice = entry(self.kconfig, "choice MUSE_JC3248W535_MIC")
        defaults = re.findall(r"^\s*default (.+?)\s*$", choice, re.M)
        self.assertEqual(defaults, [
            "MUSE_JC3248W535_MIC_I2S if MUSE_JC3248W535_MIC_GPIO >= 0",
            "MUSE_JC3248W535_MIC_NONE",
        ])

    def test_pdm_mixes_slots_through_mic_slot(self):
        self.assertRegex(
            self.board,
            r"#if CONFIG_MUSE_JC3248W535_MIC_PDM\n(?:\s*/?\*.*\n)*\s*\.mic_slot = -1,\n#else\n\s*\.mic_slot = 0,",
        )
        self.assertRegex(self.board, r"#define MIC_GAIN_OFFSET_DB 0\n")
        self.assertNotIn("MIC_GAIN_OFFSET_DB 6", self.board)

    def test_touch_talk_beside_boot(self):
        self.assertIn(".touch_talk = true,", self.board)
        self.assertIn('.talk_button = "boot",', self.board)


if __name__ == "__main__":
    unittest.main()
