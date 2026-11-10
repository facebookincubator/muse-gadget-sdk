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
import os
import re
import shlex
import shutil
import subprocess
import tempfile
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


def function(source, signature):
    start = source.index(signature)
    pos, depth = source.index('{', start) + 1, 1
    while depth:
        depth += (source[pos] == '{') - (source[pos] == '}')
        pos += 1
    return source[start:pos]


class Box2Keys(unittest.TestCase):
    # The keys' own code, compiled against a fake expander and clock: Q and L
    # read low pressed and M high, on USB and on battery (checked on a unit),
    # and a key held at boot is a press already under way.
    HARNESS = r'''
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
typedef int esp_err_t;
#define ESP_OK 0
#define XL9555_IN0 0x00
#define MUSE_BTN_TALK_PRESS 0x01
#define MUSE_BTN_TALK_RELEASE 0x02
#define MUSE_BTN_SPEAKER_PRESS 0x04
#define MUSE_BTN_SPEAKER_RELEASE 0x08
#define MUSE_BTN_POWER_PRESS 0x10
#define MUSE_BTN_POWER_RELEASE 0x20
#define MUSE_BTN_POWER_LONG 0x40
static uint8_t s_port0;
static bool s_fail;
static int64_t s_now_us;
static esp_err_t xl_read2(uint8_t reg, uint8_t val[2])
{
    (void)reg;
    if (s_fail) {
        return -1;
    }
    val[0] = s_port0;
    val[1] = 0xff;
    return ESP_OK;
}
static int64_t esp_timer_get_time(void)
{
    return s_now_us;
}
@KEYS@
static unsigned run(uint8_t port0, int ms)
{
    unsigned ev = 0;
    s_port0 = port0;
    for (int t = 0; t < ms; t += 10) {
        s_now_us += 10000;
        ev |= poll_expander();
    }
    return ev;
}
static unsigned tap(uint8_t down, uint8_t up)
{
    unsigned ev = run(down, 100);
    return ev | run(up, 100);
}
int main(void)
{
    keys_at_boot(0x3f);   /* Q held: it woke the box */
    printf("q_boot_held %x\n", run(0x3f, 3000));
    printf("q_boot_release %x\n", run(0x7f, 100));
    printf("q_press %x\n", run(0x3f, 100));
    printf("q_hold %x\n", run(0x3f, 2100));
    printf("q_release %x\n", run(0x7f, 100));
    keys_at_boot(0xff);   /* M held: it switched the box on */
    printf("m_boot_held %x\n", run(0xff, 1000));
    printf("m_boot_release %x\n", run(0x7f, 100));
    printf("m_tap %x\n", tap(0xff, 0x7f));
    keys_at_boot(0x5f);   /* L held */
    printf("l_boot_held %x\n", run(0x5f, 1000));
    printf("l_boot_release %x\n", run(0x7f, 100));
    keys_at_boot(0x7f);   /* nothing held */
    printf("q_tap %x\n", tap(0x3f, 0x7f));
    printf("l_tap %x\n", tap(0x5f, 0x7f));
    printf("q_press_then_fail %x\n", run(0x3f, 100));
    s_fail = true;        /* Q let go while the expander doesn't answer */
    printf("q_failed %x\n", run(0x7f, 2500));
    s_fail = false;
    printf("q_recovered %x\n", run(0x7f, 100));
    return 0;
}
'''

    def test_key_held_at_boot_is_not_a_press(self):
        cc = shlex.split(os.environ.get('CC', 'cc'))
        if not cc or not shutil.which(cc[0]):
            self.skipTest('C compiler unavailable')
        source = BOARD.read_text()
        defines = '\n'.join(re.findall(r'^#define (?:XL_KEY_[LQM]|POWER_HOLD_US) .*$', source, re.M))
        keys = re.search(r'typedef struct \{\n    bool pressed;\n    uint8_t stable;\n\} xl_key_t;\n'
                         r'static xl_key_t s_m, s_l, s_q;\n', source).group(0)
        state = 'static int64_t s_q_down_us;\nstatic bool s_q_long;\n'
        code = '\n\n'.join(function(source, sig) for sig in (
            'static bool m_down(', 'static bool l_down(', 'static bool q_down(', 'static void keys_at_boot(',
            'static unsigned debounce(', 'static unsigned poll_expander('))
        with tempfile.TemporaryDirectory() as tmp:
            src, exe = Path(tmp) / 'keys.c', Path(tmp) / 'keys'
            src.write_text(self.HARNESS.replace('@KEYS@', '\n'.join((defines, keys, state, code))))
            subprocess.run(cc + ['-std=c11', '-Wall', '-Werror', '-Wno-unused-function', str(src), '-o', str(exe)],
                           check=True)
            out = dict(line.split() for line in subprocess.run([str(exe)], capture_output=True, text=True,
                                                               check=True).stdout.splitlines())
        ev = {k: int(v, 16) for k, v in out.items()}
        self.assertEqual(ev['q_boot_held'], 0)           # no press, and no power-off hold
        self.assertEqual(ev['q_boot_release'], 0x20)     # the held press ends, nothing more
        self.assertEqual(ev['q_press'], 0x10)
        self.assertEqual(ev['q_hold'], 0x40)             # a real hold still powers off
        self.assertEqual(ev['q_release'], 0x20)
        self.assertEqual(ev['m_boot_held'], 0)
        self.assertEqual(ev['m_boot_release'], 0x02)
        self.assertEqual(ev['m_tap'], 0x03)
        self.assertEqual(ev['l_boot_held'], 0)
        self.assertEqual(ev['l_boot_release'], 0x08)
        self.assertEqual(ev['q_tap'], 0x30)
        self.assertEqual(ev['l_tap'], 0x0c)
        # Q let go while reads fail: no power-off hold once they recover.
        self.assertEqual(ev['q_press_then_fail'], 0x10)
        self.assertEqual(ev['q_failed'], 0)
        self.assertEqual(ev['q_recovered'], 0x20)

    def test_power_off_waits_for_the_keys_only_so_long(self):
        source = BOARD.read_text()
        off = function(source, 'static esp_err_t power_off(void)')
        self.assertIn('waited < KEYS_UP_WAIT_MS', off)
        self.assertNotIn('do {', off)
        self.assertNotIn('s_keys_idle', source)


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
