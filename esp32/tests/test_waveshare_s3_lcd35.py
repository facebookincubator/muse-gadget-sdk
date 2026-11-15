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
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import tempfile
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

    def test_battery_through_the_shared_reader(self):
        # The rails first, then muse_pmu takes the chip: it reports no
        # battery as -1, skips an unready gauge and reads the voltage.
        init = function(self.source, 'init')
        self.assertLess(init.index('pmic_init()'), init.index('muse_pmu_init(s_i2c, false)'))
        self.assertLess(init.index('i2c_master_bus_rm_device(s_pmic)'), init.index('muse_pmu_init(s_i2c, false)'))
        self.assertIn('muse_pmu_read_power(out)', function(self.source, 'read_power'))

    def test_landscape_layout_fits(self):
        # muse_ui_start()'s own layout choice, compiled: at 480x320 the full
        # layout (laid out for 466 px) would put the status line and state
        # above the screen, so this panel takes the compact one. Taller and
        # wider panels keep theirs.
        cc = shlex.split(os.environ.get('CC', 'cc'))
        if not cc or not shutil.which(cc[0]):
            self.skipTest('C compiler unavailable')
        w = int(re.search(r'#define LCD_H_RES (\d+)', self.source).group(1))
        h = int(re.search(r'#define LCD_V_RES (\d+)', self.source).group(1))
        self.assertIn('.width = LCD_H_RES,', self.source)
        self.assertIn('.height = LCD_V_RES,', self.source)
        start = function((MUSE / 'muse_ui.c').read_text(), 'muse_ui_start')
        choice = re.search(r'    bool short_landscape = [^;]+;\n    s_small = [^;]+;\n', start).group(0)
        code = ('#include <stdbool.h>\n#include <stdio.h>\n#include <stdlib.h>\n'
                'int main(int argc, char **argv) {\n    (void)argc;\n'
                '    int s_w = atoi(argv[1]), s_h = atoi(argv[2]);\n    bool s_small;\n'
                + choice + '    printf("%d", s_small);\n    return 0;\n}\n')
        with tempfile.TemporaryDirectory() as tmp:
            src, exe = Path(tmp) / 't.c', Path(tmp) / 't'
            src.write_text(code)
            subprocess.run(cc + ['-std=c11', '-Wall', '-Werror', str(src), '-o', str(exe)], check=True)
            small = lambda sw, sh: subprocess.run([str(exe), str(sw), str(sh)], capture_output=True,
                                                  text=True, check=True).stdout == '1'
            self.assertTrue(small(w, h))
            self.assertTrue(small(320, 240))      # BOX-3
            self.assertFalse(small(320, 480))     # JC3248W535, portrait
            self.assertFalse(small(800, 480))     # LCD-7
            self.assertFalse(small(466, 466))

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

    def test_powers_the_panel_like_the_full_ui(self):
        # The panel's rails are off until the AXP2101 is set up; the status
        # build doesn't run the board file's init(), so it does the same.
        led = (ROOT / 'main/led_status.c').read_text()
        pair = r'\{(0x[0-9a-f]+), ([^}]+)\}'
        status = re.findall(pair, led[led.index('s_pmic_seq[][2] = {'):led.index('};', led.index('s_pmic_seq[][2] = {'))])
        board = BOARD.read_text()
        start = board.index('seq[][2] = {')
        ui = [(r, re.sub(r'\s*/\*.*', '', v).strip().rstrip(',')) for r, v in re.findall(pair, board[start:board.index('};', start)])]
        self.assertEqual(len(status), 14)
        self.assertEqual([(r, v.strip()) for r, v in status], ui)
        init = function(led, 'exp_lcd_init')
        self.assertLess(init.index('pmic_rails_init(bus)'), init.index('EXP_ADDR'))

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
