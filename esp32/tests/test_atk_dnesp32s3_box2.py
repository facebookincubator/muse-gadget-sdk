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

"""The ALIENTEK ATK-DNESP32S3-BOX2: wired into the build, the tools and CI
like the other boards with the full UI, its screen lit again after a wake
from deep sleep, and its third-party panel table listed with its licence."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
MUSE = ROOT / 'components/muse'
BOARD = MUSE / 'boards/board_atk_dnesp32s3_box2.c'
OVERLAY = 'devices/sdkconfig.muse-atk-dnesp32s3-box2'


class Box2Wiring(unittest.TestCase):
    def test_ci_builds_it(self):
        ci = (ROOT.parent / '.github/workflows/esp32.yml').read_text()
        self.assertIn('board: muse-atk-dnesp32s3-box2\n', ci)
        self.assertIn('"' + ';devices/sdkconfig.muse;' + OVERLAY + '"', ci)

    def test_board_source_only_for_this_board(self):
        cmake = (MUSE / 'CMakeLists.txt').read_text()
        self.assertRegex(cmake, r'elseif\(CONFIG_MUSE_BOARD_ATK_DNESP32S3_BOX2\)\n\s+list\(APPEND srcs "boards/board_atk_dnesp32s3_box2.c"\)')
        kconfig = (MUSE / 'Kconfig').read_text()
        self.assertRegex(kconfig, r'config MUSE_BOARD_ATK_DNESP32S3_BOX2\n(?:.*\n)*?\s+depends on IDF_TARGET_ESP32S3')
        self.assertIn('default "atk_dnesp32s3_box2" if MUSE_BOARD_ATK_DNESP32S3_BOX2', kconfig)
        manifest = (MUSE / 'idf_component.yml').read_text()
        self.assertIn('\\"atk_dnesp32s3_box2\\"', manifest)

    def test_overlay_selects_the_board(self):
        overlay = (ROOT / OVERLAY).read_text()
        self.assertIn('CONFIG_IDF_TARGET="esp32s3"', overlay)
        self.assertIn('CONFIG_MUSE_BOARD_ATK_DNESP32S3_BOX2=y', overlay)
        self.assertIn('CONFIG_SPIRAM_MODE_OCT=y', overlay)

    def test_tools_know_it(self):
        board_sh = (ROOT / 'tools/muse/board.sh').read_text()
        self.assertRegex(board_sh, r'\n\s+atk-box2\)\s+profile=atk-dnesp32s3-box2;\s+target=esp32s3 ;;')
        ports = (ROOT / 'tools/muse/ports.py').read_text()
        self.assertIn('"atk-box2": USJ,', ports)
        self.assertRegex(ports, r'COMMANDS = \([^)]*"atk-box2"')
        avatar = (ROOT / 'tools/muse/avatar.py').read_text()
        self.assertIn('"ALIENTEK ATK-DNESP32S3-BOX2": "atk-box2",', avatar)
        self.assertIn('.name = "ALIENTEK ATK-DNESP32S3-BOX2",', BOARD.read_text())


class Box2Power(unittest.TestCase):
    def test_backlight_hold_released_at_boot(self):
        # power_off() on USB holds the backlight low through deep sleep. The
        # hold outlives the wake, so init() has to release it or the PWM can't
        # light the screen again (board_ai_passport.c does the same).
        source = BOARD.read_text()
        off = source[source.index('static esp_err_t power_off(void)'):]
        self.assertIn('gpio_hold_en(LCD_BL);', off)
        self.assertIn('gpio_deep_sleep_hold_en();', off)
        init = source[source.index('static esp_err_t init(void)'):source.index('static bool read_flip')]
        self.assertIn('gpio_hold_dis(LCD_BL);', init)
        self.assertIn('gpio_deep_sleep_hold_dis();', init)
        self.assertLess(init.index('gpio_hold_dis(LCD_BL);'), init.index('i2c_new_master_bus'))


class Box2ThirdParty(unittest.TestCase):
    def test_panel_table_licence_is_listed(self):
        self.assertTrue((ROOT / 'devices/licenses/xiaozhi-esp32.txt').read_text().startswith('MIT License'))
        for readme in (ROOT.parent / 'README.md', ROOT / 'README.md'):
            text = readme.read_text()
            self.assertIn('board_atk_dnesp32s3_box2.c', text, readme)
            self.assertIn('xiaozhi-esp32.txt', text, readme)

    def test_rotation_owns_madctl(self):
        # The vendor table's MADCTL (0x36) would undo swap_xy and the mirrors
        # that turn the picture with the box.
        source = BOARD.read_text()
        table = source[source.index('tuning[] = {'):source.index('};', source.index('tuning[] = {'))]
        self.assertNotRegex(table, r'\{ 0x36,')
        self.assertIn('esp_lcd_panel_swap_xy(s_panel, true);', source)


if __name__ == '__main__':
    unittest.main()
