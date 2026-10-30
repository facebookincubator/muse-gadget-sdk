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

"""The Waveshare ESP32-S3-Touch-LCD-1.9's wiring into the board list, and the
shared Kconfig and component rules it must not disturb."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
MUSE = ROOT / 'components/muse'
BOARD = MUSE / 'boards/board_waveshare_s3_lcd19.c'


def dependencies():
    """Each managed dependency's name and `if` rules, in file order."""
    deps, current = [], None
    for line in (MUSE / 'idf_component.yml').read_text().splitlines():
        top = re.match(r'^  ([\w./-]+):\s*$', line)
        if top:
            current = (top.group(1), [])
            deps.append(current)
            continue
        rule = re.match(r'^\s+- if:\s*(.+)$', line)
        if rule and current:
            current[1].append(rule.group(1))
    return deps


def board_choice():
    """(config name, its depends-on lines) for each MUSE_BOARD choice entry."""
    text = (MUSE / 'Kconfig').read_text()
    choice = text[text.index('choice MUSE_BOARD'):text.index('endchoice')]
    entries = re.split(r'^\s*config (MUSE_BOARD_\w+)\s*$', choice, flags=re.M)[1:]
    return [(name, re.findall(r'^\s*depends on (.+)$', body, flags=re.M))
            for name, body in zip(entries[::2], entries[1::2])]


class WaveshareS3Lcd19(unittest.TestCase):
    def test_no_dependency_is_declared_twice(self):
        names = [name for name, _ in dependencies()]
        self.assertEqual(len(names), len(set(names)), names)

    def test_ili9341_stays_on_the_fnk0104b(self):
        # Rules are ANDed: a second rule that excludes fnk0104b drops the
        # FNK0104B's panel driver.
        rules = dict(dependencies())['espressif/esp_lcd_ili9341']
        self.assertEqual(rules, ['"$CONFIG{MUSE_BOARD_ID} == \\"fnk0104b\\""'])

    def test_lcd19_gets_its_panel_touch_and_lvgl_components(self):
        deps = dict(dependencies())
        for name in ('espressif/esp_lcd_sh8601', 'espressif/esp_lcd_touch_cst816s',
                     'espressif/esp_lvgl_adapter'):
            self.assertEqual(len(deps[name]), 1, name)
            self.assertIn('\\"waveshare_s3_lcd19\\"', deps[name][0], name)

    def test_every_board_is_guarded_by_its_target(self):
        for name, depends in board_choice():
            if name == 'MUSE_BOARD_NONE':
                continue
            with self.subTest(board=name):
                self.assertEqual(len(depends), 1, depends)
                self.assertRegex(depends[0], r'^IDF_TARGET_ESP32\w*$')
        boards = dict(board_choice())
        self.assertEqual(boards['MUSE_BOARD_GUITION_JC3248W535'], ['IDF_TARGET_ESP32S3'])
        self.assertEqual(boards['MUSE_BOARD_WAVESHARE_S3_LCD19'], ['IDF_TARGET_ESP32S3'])

    def test_no_power_off_or_panel_sleep_it_cannot_do(self):
        source = BOARD.read_text()
        # No PMU or latch: leaving power_off out hides Settings' Power off
        # (muse_board.h) instead of offering one that always fails.
        self.assertNotRegex(source, r'\.power_off\s*=')
        self.assertNotIn('ESP_ERR_NOT_SUPPORTED', source)
        # esp_lcd_sh8601 has no disp_sleep, so the call could only fail.
        self.assertNotIn('esp_lcd_panel_disp_sleep', source)
        self.assertRegex(source, r'\.panel_sleep\s*=\s*NULL')


if __name__ == '__main__':
    unittest.main()
