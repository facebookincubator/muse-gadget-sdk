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

"""The VIEWE SmartRing-Plus: its vendored BSP stays out of other boards' builds,
and touch, its only reachable control, keeps working with the screen dark."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
MUSE = ROOT / 'components/muse'
BOARD = MUSE / 'boards/board_viewe_smartring_plus.c'


def board_struct(source):
    start = source.index('static const muse_board_t s_board = {')
    return source[start:source.index('};', start)]


class SmartRingPlusBsp(unittest.TestCase):
    def test_bsp_is_outside_the_discovered_component_dirs(self):
        # ESP-IDF builds every component under components/ for every target,
        # so the S3-only BSP must live elsewhere.
        found = sorted(str(p.relative_to(ROOT)) for p in (ROOT / 'components').glob('*/**/smartring_plus.h'))
        self.assertEqual(found, [], 'the SmartRing-Plus BSP would build for every target')

    def test_bsp_is_a_board_gated_override(self):
        manifest = (MUSE / 'idf_component.yml').read_text()
        m = re.search(r'^  viewesmart/smartring_plus:\n((?:    .*\n)+)', manifest, re.M)
        self.assertIsNotNone(m, 'viewesmart/smartring_plus dependency missing')
        block = m.group(1)
        path = re.search(r'override_path: "([^"]+)"', block).group(1)
        bsp = (MUSE / path).resolve()
        self.assertTrue((bsp / 'CMakeLists.txt').is_file(), bsp)
        self.assertTrue((bsp / 'idf_component.yml').is_file(), bsp)
        self.assertNotEqual(bsp.parent, (ROOT / 'components').resolve())
        rules = re.findall(r'- if: (.+)', block)
        self.assertEqual(rules, ['"$CONFIG{MUSE_BOARD_ID} == \\"viewe_smartring_plus\\""'])


class SmartRingPlusTouch(unittest.TestCase):
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


if __name__ == '__main__':
    unittest.main()
