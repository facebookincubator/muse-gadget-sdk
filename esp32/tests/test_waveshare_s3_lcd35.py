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

"""The Waveshare ESP32-S3-Touch-LCD-3.5, in both builds: the full UI
(sdkconfig.muse-waveshare-s3-lcd-35) and Link's status screen
(sdkconfig.waveshare-s3-lcd-35). Its ST7796 driver comes in only for those
builds, CI builds both, LVGL's PSRAM buffers go out by DMA from PSRAM, and the
tools agree on the board's alias."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
MUSE = ROOT / 'components/muse'
BOARD = MUSE / 'boards/board_waveshare_s3_lcd_35.c'
BOARD_ID = 'waveshare_s3_lcd_35'
ALIAS = 's3lcd35'


def dependency(manifest, name):
    m = re.search(r'^  ' + re.escape(name) + r':\n((?:    .*\n)+)', manifest, re.M)
    return m.group(1) if m else None


def function(source, name):
    match = re.search(r'\b' + name + r'\([^)]*\)\s*\{', source)
    start, pos, depth = match.start(), match.end(), 1
    while depth:
        depth += (source[pos] == '{') - (source[pos] == '}')
        pos += 1
    return source[start:pos]


class WiredOnlyForThisBoard(unittest.TestCase):
    def test_ui_build_pulls_the_panel_driver(self):
        manifest = (MUSE / 'idf_component.yml').read_text()
        for dep in ('espressif/esp_lcd_st7796', 'espressif/esp_lvgl_adapter'):
            block = dependency(manifest, dep)
            self.assertIsNotNone(block, dep)
            rules = re.findall(r'- if: (.+)', block)
            self.assertEqual(len(rules), 1, dep)
            self.assertIn('MUSE_BOARD_ID', rules[0])
            self.assertIn('\\"%s\\"' % BOARD_ID, rules[0], dep)

    def test_status_build_uses_a_compatible_driver_range(self):
        # Both manifests resolve into one dependencies.lock, so main's range
        # must not exclude the one the Waveshare P4 3.5 needs.
        muse = dependency((MUSE / 'idf_component.yml').read_text(), 'espressif/esp_lcd_st7796')
        main = dependency((ROOT / 'main/idf_component.yml').read_text(), 'espressif/esp_lcd_st7796')
        self.assertIsNotNone(main)
        version = re.compile(r'version: (".*")')
        self.assertEqual(version.search(main).group(1), version.search(muse).group(1))
        self.assertIn('target == esp32s3', main)
        cmake = (ROOT / 'main/CMakeLists.txt').read_text()
        self.assertRegex(cmake, r'if\(CONFIG_HOMEHUB_LED_BACKEND_WAVESHARE_S3_ST7796\)\n'
                                r'\s+list\(APPEND GADGET_REQUIRES esp_lcd_st7796\)')

    def test_board_source_only_for_this_board(self):
        cmake = (MUSE / 'CMakeLists.txt').read_text()
        self.assertRegex(cmake, r'elseif\(CONFIG_MUSE_BOARD_WAVESHARE_S3_LCD_35\)\n'
                                r'\s+list\(APPEND srcs "boards/board_waveshare_s3_lcd_35.c"\)')
        kconfig = (MUSE / 'Kconfig').read_text()
        self.assertRegex(kconfig, r'config MUSE_BOARD_WAVESHARE_S3_LCD_35\n.*\n\s+depends on IDF_TARGET_ESP32S3')
        self.assertIn('default "%s" if MUSE_BOARD_WAVESHARE_S3_LCD_35' % BOARD_ID, kconfig)

    def test_ci_builds_both(self):
        ci = (ROOT.parent / '.github/workflows/esp32.yml').read_text()
        self.assertIn('overlays: ";devices/sdkconfig.waveshare-s3-lcd-35"', ci)
        self.assertIn('overlays: ";devices/sdkconfig.muse;devices/sdkconfig.muse-waveshare-s3-lcd-35"', ci)


class Board(unittest.TestCase):
    def setUp(self):
        self.source = BOARD.read_text()

    def test_psram_draw_buffers_go_out_by_dma_from_psram(self):
        # ESP-IDF v6.0.3: without psram_dma_direct each chunk is copied into
        # an internal bounce buffer, the RAM the I2S DMA descriptors need.
        start = function(self.source, 'display_start')
        self.assertIn('.use_psram = true,', start)
        self.assertIn('.flags.psram_dma_direct = 1,', start)

    def test_no_battery_reads_as_none(self):
        power = function(self.source, 'read_power')
        self.assertIn('out->battery_pct = -1;', power)
        self.assertIn('bool battery = s1 & (1u << 3);', power)
        self.assertIn('pct <= 100', power)

    def test_boot_is_talk(self):
        self.assertIn('muse_gpio_button_init(&s_talk, TALK_GPIO)', self.source)
        self.assertRegex(self.source, r'#define TALK_GPIO GPIO_NUM_0\b')
        self.assertIn('.talk_button = "boot",', self.source)


class StatusScreen(unittest.TestCase):
    def test_backend_draws_from_internal_dma_memory(self):
        led = (ROOT / 'main/led_status.c').read_text()
        start = led.index('#elif CONFIG_HOMEHUB_LED_BACKEND_WAVESHARE_S3_ST7796\n// Waveshare')
        block = led[start:led.index('#endif', start)]
        self.assertIn('#define LCD_BUF_CAPS     MALLOC_CAP_DMA', block)
        scale = int(re.search(r'#define LCD_ANIM_SCALE\s+(\d+)', block).group(1))
        width = int(re.search(r'#define LCD_H_RES\s+(\d+)', block).group(1))
        self.assertLessEqual(56 * scale, width)

    def test_overlay_selects_the_backend(self):
        overlay = (ROOT / 'devices/sdkconfig.waveshare-s3-lcd-35').read_text()
        self.assertIn('CONFIG_HOMEHUB_LED_BACKEND_WAVESHARE_S3_ST7796=y', overlay)
        self.assertIn('CONFIG_SPIRAM_MODE_OCT=y', overlay)


class Tools(unittest.TestCase):
    def test_alias_everywhere(self):
        board_sh = (ROOT / 'tools/muse/board.sh').read_text()
        self.assertRegex(board_sh, r'\n\s+%s\) profile=waveshare-s3-lcd-35;\s+target=esp32s3 ;;' % ALIAS)
        self.assertIn('|%s}' % ALIAS, board_sh)
        ports = (ROOT / 'tools/muse/ports.py').read_text()
        self.assertIn('"%s": USJ,' % ALIAS, ports)
        self.assertRegex(ports, r'COMMANDS = \(.*"%s"' % ALIAS)
        avatar = (ROOT / 'tools/muse/avatar.py').read_text()
        name = re.search(r'\.name = "([^"]+)"', BOARD.read_text()).group(1)
        self.assertIn('"%s": "%s",' % (name, ALIAS), avatar)
        self.assertRegex(avatar, r'CHAT_BOARDS = \(.*"%s"' % ALIAS)

    def test_status_build_helper(self):
        board_sh = (ROOT / 'tools/board.sh').read_text()
        self.assertRegex(board_sh, r'\n  [a-z0-9|-]*\bwaveshare-s3-lcd-35\)\n\s+TARGET=esp32s3\n')


if __name__ == '__main__':
    unittest.main()
