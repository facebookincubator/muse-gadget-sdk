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

"""The LilyGO T-Display-S3 AMOLED 1.75's SY6970 charger setup and its wiring
into the board lists."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
MUSE = ROOT / 'components/muse'
BOARD = MUSE / 'boards/board_lilygo_s3_amoled_175.c'


def function(source, name):
    match = re.search(r"static (?:esp_err_t|void) " + name + r"\([^)]*\)\n\{", source)
    brace = source.index("{", match.start())
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end]


class LilygoS3Amoled175(unittest.TestCase):
    def test_adc_start_keeps_the_rest_of_reg02(self):
        # REG02 also holds input source detection and ICO: set only
        # CONV_START and CONV_RATE, as LilyGO's own driver does.
        init = function(BOARD.read_text(), 'init')
        self.assertNotRegex(init, r'sy6970_write\(SY6970_REG_ADC,\s*0x[0-9A-Fa-f]+\)')
        self.assertIn('sy6970_read(SY6970_REG_ADC, &adc)', init)
        self.assertIn('sy6970_write(SY6970_REG_ADC, adc | 0xC0)', init)

    def test_boot_reconnects_the_battery(self):
        # power_off() opens the BATFET; on USB that ends in deep sleep, and
        # the next boot has to close it again or the cell never charges.
        source = BOARD.read_text()
        off = function(source, 'power_off')
        self.assertIn('batfet | 0x20', off)
        # Only on battery: with input power the cell has to keep charging.
        # A failed status read must not count as "on battery".
        self.assertIn("on_battery = sy6970_read(SY6970_REG_STATUS, &status) == ESP_OK && !(status & 0x04);", off)
        self.assertIn('if (on_battery && sy6970_read(SY6970_REG_BATFET, &batfet) == ESP_OK)', off)
        self.assertIn('sy6970_write(SY6970_REG_BATFET, batfet & ~0x20)', function(source, 'init'))

    def test_board_is_wired_into_the_lists(self):
        self.assertIn('lilygo_s3_amoled_175', (MUSE / 'Kconfig').read_text())
        self.assertIn('boards/board_lilygo_s3_amoled_175.c', (MUSE / 'CMakeLists.txt').read_text())
        components = (MUSE / 'idf_component.yml').read_text()
        self.assertIn('waveshare/esp_lcd_touch_cst9217', components)
        self.assertIn('\\"lilygo_s3_amoled_175\\"', components)
        self.assertIn('lilygo175) profile=lilygo-s3-amoled-175', (ROOT / 'tools/muse/board.sh').read_text())
        overlay = (ROOT / 'devices/sdkconfig.muse-lilygo-s3-amoled-175').read_text()
        self.assertIn('build-muse-lilygo-s3-amoled-175/sdkconfig', overlay)


if __name__ == '__main__':
    unittest.main()
