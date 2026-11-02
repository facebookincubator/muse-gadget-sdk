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

"""The VIEWE SmartRing-Plus: built without VIEWE's BSP but keeping what the
BSP-based port was reviewed for. Its components build only for this board,
touch (its only reachable control) wakes a dark screen, and the battery
arithmetic VIEWE's BSP used is compiled and checked on the host."""
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
BOARD = MUSE / 'boards/board_viewe_smartring_plus.c'
INIT = MUSE / 'boards/viewe_smartring_display_init.h'


def board_struct(source):
    start = source.index('static const muse_board_t s_board = {')
    return source[start:source.index('};', start)]


def function(source, name):
    match = re.search(r'\b' + name + r'\([^)]*\)\s*\{', source)
    start, pos, depth = match.start(), match.end(), 1
    while depth:
        depth += (source[pos] == '{') - (source[pos] == '}')
        pos += 1
    return source[start:pos]


class SmartRingPlusIsolation(unittest.TestCase):
    def test_no_vendored_bsp(self):
        self.assertFalse((ROOT / 'components_vendor/viewesmart__smartring_plus').exists())
        manifest = (MUSE / 'idf_component.yml').read_text()
        self.assertNotIn('viewesmart/smartring_plus', manifest)
        self.assertNotIn('bsp/', BOARD.read_text())

    def test_components_only_for_this_board(self):
        manifest = (MUSE / 'idf_component.yml').read_text()
        for dep in ('espressif/esp_lcd_st77916', 'espressif/esp_lcd_touch_cst816s', 'espressif/esp_lvgl_adapter'):
            m = re.search(r'^  ' + re.escape(dep) + r':\n((?:    .*\n)+)', manifest, re.M)
            self.assertIsNotNone(m, dep)
            rules = re.findall(r'- if: (.+)', m.group(1))
            self.assertEqual(len(rules), 1, dep)
            self.assertIn('MUSE_BOARD_ID', rules[0])
            self.assertIn('\\"viewe_smartring_plus\\"', rules[0], dep)

    def test_board_source_only_for_this_board(self):
        cmake = (MUSE / 'CMakeLists.txt').read_text()
        self.assertRegex(cmake, r'elseif\(CONFIG_MUSE_BOARD_VIEWE_SMARTRING_PLUS\)\n\s+list\(APPEND srcs "boards/board_viewe_smartring_plus.c"\)')
        kconfig = (MUSE / 'Kconfig').read_text()
        self.assertRegex(kconfig, r'config MUSE_BOARD_VIEWE_SMARTRING_PLUS\n(?:.*\n)*?\s+depends on IDF_TARGET_ESP32S3')

    def test_ci_builds_it(self):
        ci = (ROOT.parent / '.github/workflows/esp32.yml').read_text()
        self.assertIn('devices/sdkconfig.muse-viewe-smartring-plus', ci)


class SmartRingPlusBoard(unittest.TestCase):
    def setUp(self):
        self.source = BOARD.read_text()
        self.board = board_struct(self.source)

    def test_uses_the_shared_touch_talk_flag(self):
        self.assertRegex(self.board, r'\.touch_talk = true,')
        self.assertNotIn('touch_talk(bool', self.source)

    def test_touch_still_wakes_a_dark_screen(self):
        # display_pause stops LVGL and with it touch, leaving only buttons to
        # wake the screen; BOOT is inside the case.
        self.assertNotRegex(self.board, r'\.display_pause\s*=')
        self.assertNotIn('esp_lv_adapter_pause', self.source)

    def test_no_power_off_without_a_soft_switch(self):
        self.assertNotRegex(self.board, r'\.power_off\s*=')

    def test_boot_still_talks(self):
        self.assertIn('muse_gpio_button_init(&s_talk, TALK_GPIO)', self.source)
        self.assertIn('return muse_gpio_button_poll(&s_talk);', function(self.source, 'poll_buttons'))

    def test_internal_ram_draw_buffers(self):
        start = function(self.source, 'display_start')
        self.assertIn('ESP_LV_ADAPTER_DISPLAY_SPI_WITHOUT_PSRAM_DEFAULT_CONFIG', start)
        self.assertIn('disp_cfg.profile.use_psram = false;', start)
        self.assertIn('disp_cfg.profile.require_double_buffer = true;', start)
        self.assertRegex(self.source, r'#define DRAW_BUF_LINES 10\b')

    def test_panel_keeps_the_vendor_table_inversion(self):
        # VIEWE's B-panel table ends with INVON; a later invert_color(false)
        # would send INVOFF and invert every colour.
        table = INIT.read_text()
        self.assertRegex(table, r'\{0x21, \(uint8_t\[\]\)\{\}, 0, 0\}')
        self.assertNotIn('esp_lcd_panel_invert_color', self.source)

    def test_touch_controller_is_reset(self):
        # Without the reset pulse a CST816 left asleep by a soft restart may
        # not answer, and the board would come up without touch.
        start = function(self.source, 'display_start')
        self.assertIn('.rst_gpio_num = TOUCH_RST', start)
        self.assertRegex(self.source, r'#define TOUCH_RST 40\b')
        self.assertIn('esp_lv_adapter_register_touch', start)

    def test_v1_reads_a_calibrated_voltage(self):
        # adc_oneshot_get_calibrated_result() takes a calibration handle;
        # the scheme is created once and the raw average converted with it.
        self.assertNotIn('adc_oneshot_get_calibrated_result', self.source)
        self.assertIn('adc_cali_create_scheme_curve_fitting', function(self.source, 'adc_setup'))
        self.assertIn('adc_cali_raw_to_voltage(s_cali', function(self.source, 'read_power'))

    def test_v2_configures_the_charger_before_use(self):
        setup = function(self.source, 'axp_setup')
        for reg, value in (('0x94', 'SMARTRING_AXP_ALDO3_CODE'), ('0x64', 'SMARTRING_AXP_CV_CODE'),
                           ('0x61', 'SMARTRING_AXP_IPRE_CODE'), ('0x62', 'SMARTRING_AXP_ICC_CODE'),
                           ('0x63', 'SMARTRING_AXP_ITERM_CODE')):
            self.assertIn('axp_write(%s, %s)' % (reg, value), setup)
        self.assertIn('axp_set_bits(0x90, 0x04)', setup)   # ALDO3 on
        self.assertIn('axp_set_bits(0x18, 0x0A)', setup)   # charger and gauge


class SmartRingPlusPower(unittest.TestCase):
    def test_battery_arithmetic(self):
        cc = shlex.split(os.environ.get('CC', 'cc'))
        if not cc or not shutil.which(cc[0]):
            self.skipTest('C compiler unavailable')
        code = r"""
#include <assert.h>
#include "viewe_smartring_power.h"
int main(void) {
    assert(smartring_ocv_pct(3000) == 0 && smartring_ocv_pct(3300) == 0);
    assert(smartring_ocv_pct(3700) == 40 && smartring_ocv_pct(3725) == 44);
    assert(smartring_ocv_pct(4200) == 100 && smartring_ocv_pct(4400) == 100);
    for (int mv = 3300, last = 0; mv <= 4200; mv += 5) {
        int p = smartring_ocv_pct(mv);
        assert(p >= last && p <= 100);
        last = p;
    }
    /* AXP2101 codes, from its datasheet as VIEWE's BSP computes them. */
    assert(SMARTRING_AXP_ICC_CODE == 9 && 200 + 100 * (SMARTRING_AXP_ICC_CODE - 8) == 300);
    assert(SMARTRING_AXP_IPRE_CODE * 25 == 75 && SMARTRING_AXP_ITERM_CODE == (0x10 | 1));
    assert(SMARTRING_AXP_ALDO3_CODE * 100 + 500 == 3300 && SMARTRING_AXP_CV_CODE == 3);

    smartring_v1_t s = {0};
    int mv = 0, pct = -2;
    bool chg = true;
    assert(!smartring_v1_update(&s, 1000, &mv, &pct, &chg) && pct == -2);
    assert(smartring_v1_update(&s, 3700, &mv, &pct, &chg) && mv == 3700 && pct == 40 && !chg);
    /* USB in: the rail jumps; charging after two readings, cell estimated below the rail. */
    assert(smartring_v1_update(&s, 4400, &mv, &pct, &chg) && !chg && pct == 93);
    assert(smartring_v1_update(&s, 4400, &mv, &pct, &chg) && chg && pct == 93);
    /* Hysteresis: 4.2 V is still charging; two readings under 4.15 V end it. */
    assert(smartring_v1_update(&s, 4200, &mv, &pct, &chg) && chg);
    assert(smartring_v1_update(&s, 4100, &mv, &pct, &chg) && chg);
    assert(smartring_v1_update(&s, 4100, &mv, &pct, &chg) && !chg && pct == 93);
    /* A single spike doesn't flip it. */
    assert(smartring_v1_update(&s, 4300, &mv, &pct, &chg) && !chg);
    assert(smartring_v1_update(&s, 4000, &mv, &pct, &chg) && !chg && pct == 82);

    bool usb = false;
    smartring_axp_decode(0x28, 0x20, 57, 0x0E, 0xD8, &mv, &pct, &chg, &usb);
    assert(mv == 3800 && pct == 57 && chg && usb);
    smartring_axp_decode(0x08, 0x40, 250, 0xCF, 0xA0, &mv, &pct, &chg, &usb);
    assert(mv == 4000 && pct == 100 && !chg && !usb);
    smartring_axp_decode(0x20, 0x00, 0, 0x0E, 0xD8, &mv, &pct, &chg, &usb);
    assert(mv == 0 && pct == -1 && !chg && usb);
    return 0;
}
"""
        with tempfile.TemporaryDirectory() as tmp:
            src, exe = Path(tmp) / 't.c', Path(tmp) / 't'
            src.write_text(code)
            subprocess.run(cc + ['-std=c11', '-Wall', '-Werror', '-I', str(MUSE / 'boards'), str(src), '-o', str(exe)],
                           check=True)
            subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    unittest.main()
