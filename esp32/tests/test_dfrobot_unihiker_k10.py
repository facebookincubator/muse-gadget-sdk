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

"""The UNIHIKER K10's wake button, LED breathing and build wiring."""
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
MUSE = ROOT / 'components/muse'
BOARD = MUSE / 'boards/board_dfrobot_unihiker_k10.c'


def function(source, name):
    match = re.search(r"static (?:esp_err_t|void) " + name + r"\([^)]*\)\n\{", source)
    start = match.start()
    brace = source.index("{", start)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


class DFRobotUnihikerK10(unittest.TestCase):
    def setUp(self):
        self.board = BOARD.read_text()

    def test_power_off_wakes_on_the_button_the_menu_names(self):
        # A and B are on the I/O expander, which can't wake the chip: the menu
        # must not tell people to press B.
        self.assertIn('.wake_button = "BOOT"', self.board)
        power_off = function(self.board, 'power_off')
        self.assertIn('esp_sleep_enable_ext0_wakeup(GPIO_NUM_0, 0)', power_off)
        # GPIO0 is the I2S bit clock until power off hands it back to BOOT.
        self.assertLess(power_off.index('i2s_channel_disable(s_tx)'), power_off.index('gpio_config('))
        self.assertLess(power_off.index('gpio_config('), power_off.index('esp_sleep_enable_ext0_wakeup'))
        self.assertLess(power_off.index('esp_lcd_panel_disp_on_off(s_panel, false)'),
                        power_off.index('esp_deep_sleep_start()'))

    def test_boot_wake_returns_the_bit_clock_pin_to_digital(self):
        # After an EXT0 wake GPIO0 stays an RTC IO, and I2S setup doesn't undo
        # that: init (which runs before audio_init) must, or audio is dead.
        init = function(self.board, 'init')
        self.assertLess(init.index('rtc_gpio_deinit(GPIO_NUM_0)'), init.index('i2c_new_master_bus'))

    def test_led_breathing_stays_smooth_after_weeks_of_uptime(self):
        line = re.search(r'^\s*float phase = .*;$', self.board, re.M)
        self.assertIsNotNone(line, 'breathing phase must be computed on its own line')
        level = re.search(r'^\s*float level = pulse \? (.*) : 1\.0f;$', self.board, re.M)[1]
        code = r'''
#include <math.h>
#include <stdint.h>
#include <stdio.h>
static int64_t now;
static int64_t esp_timer_get_time(void) { return now; }
int main(void) {
    const int64_t day = 86400LL * 1000000LL;
    int distinct = 0;
    float prev = -1.0f, worst = 0.0f;
    for (now = 60 * day; now < 60 * day + 2000000; now += 60000) {
''' + line[0] + r'''
        float level = ''' + level + r''';
        if (prev >= 0.0f) {
            float d = fabsf(level - prev);
            if (d > worst) worst = d;
            distinct += d > 1e-4f;
        }
        prev = level;
    }
    printf("%d %f\n", distinct, worst);
    return !(distinct >= 25 && worst < 0.2f);
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'phase.c'
            path.write_text(code)
            cc = shlex.split(os.environ.get('CC', 'cc'))
            built = subprocess.run(cc + ['-std=c11', '-Wall', '-Werror', str(path), '-o', str(path.with_suffix('')),
                                         '-lm'], capture_output=True, text=True)
            self.assertEqual(built.returncode, 0, built.stderr)
            run = subprocess.run([str(path.with_suffix(''))], capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, 'coarse breathing after 60 days: ' + run.stdout)

    def test_board_is_wired_into_build_tools_and_ci(self):
        kconfig = (MUSE / 'Kconfig').read_text()
        self.assertIn('config MUSE_BOARD_DFROBOT_UNIHIKER_K10', kconfig)
        self.assertIn('default "dfrobot_unihiker_k10" if MUSE_BOARD_DFROBOT_UNIHIKER_K10', kconfig)
        self.assertIn('boards/board_dfrobot_unihiker_k10.c', (MUSE / 'CMakeLists.txt').read_text())
        deps = (MUSE / 'idf_component.yml').read_text()
        for component in ('espressif/led_strip', 'espressif/esp_lvgl_adapter', 'espressif/esp_lcd_ili9341'):
            block = re.split(r'\n  \S', deps.split('\n  ' + component + ':', 1)[1], maxsplit=1)[0]
            self.assertIn('dfrobot_unihiker_k10', block, component)

        overlay = 'devices/sdkconfig.muse-dfrobot-unihiker-k10'
        self.assertIn('CONFIG_MUSE_BOARD_DFROBOT_UNIHIKER_K10=y', (ROOT / overlay).read_text())
        self.assertRegex((ROOT / 'tools/muse/board.sh').read_text(),
                         r'\n\s+k10\)\s+profile=dfrobot-unihiker-k10;\s+target=esp32s3 ;;')
        ports = (ROOT / 'tools/muse/ports.py').read_text()
        self.assertIn('"k10": USJ', ports)
        self.assertRegex(ports, r'COMMANDS = \([^)]*"k10"')
        name = re.search(r'\.name = "([^"]+)"', self.board)[1]
        avatar = (ROOT / 'tools/muse/avatar.py').read_text()
        self.assertIn('"%s": "k10"' % name, avatar)
        self.assertRegex(avatar, r'CHAT_BOARDS = \([^)]*"k10"')

        workflow = (ROOT.parent / '.github/workflows/esp32.yml').read_text()
        self.assertRegex(workflow, r'- board: muse-dfrobot-unihiker-k10\n\s+target: esp32s3\n\s+overlays: '
                                   r'";devices/sdkconfig\.muse;' + re.escape(overlay) + '"')


if __name__ == '__main__':
    unittest.main()
