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
"""Compile the Core2 v1.1 backlight cap against PMU, BSP and timer fakes."""
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile
import unittest

from test_core2_buttons import function

ROOT = Path(__file__).resolve().parents[1]


class Core2Backlight(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        board = (ROOT / "components/muse/boards/board_m5stack_core2.c").read_text()
        defines = "\n".join(re.findall(r"^#define BACKLIGHT_.*", board, re.M))
        state = re.search(r"^static int s_bright_req.*", board, re.M)[0]
        source = r'''
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#define ESP_OK 0
#define TAG "board"
#define ESP_LOGI(tag, ...) ((void)(tag))
#define ESP_LOGW(tag, ...) ((void)(tag))
static int64_t now_us;
static int vsys_mv, set_pct = -1, sets, set_error;
static int64_t esp_timer_get_time(void) { return now_us; }
static int muse_pmu_vsys_mv(void) { return vsys_mv; }
static int bsp_display_brightness_set(int pct) {
    if (set_error) return set_error;
    set_pct = pct;
    ++sets;
    return ESP_OK;
}
''' + defines + "\n" + state + "\n" + "\n".join(
            function(board, name) for name in ("backlight_apply", "backlight_track_vsys")) + r'''
/* One BACKLIGHT_WINDOW of once-a-second VSYS samples. */
static void window(int mv) {
    vsys_mv = mv;
    for (int i = 0; i < BACKLIGHT_WINDOW; i++) {
        backlight_track_vsys();
        now_us += 1000000;
    }
}
static void low_battery_caps_and_recovers(void) {
    s_bright_req = 100;
    backlight_apply();
    assert(set_pct == 100);
    window(3180); /* Measured on a near-empty v1.1: BLDO1 must sit at or below 2.9 V. */
    assert(set_pct == 50);
    assert(500 + 100 * (20 + 8 * set_pct / 100) <= 3180 - BACKLIGHT_MARGIN_MV);
    window(4100); /* Healthy VSYS lifts the cap. */
    assert(set_pct == 100);
}
static void cap_never_raises_the_request(void) {
    s_bright_req = 30;
    backlight_apply();
    window(4100);
    assert(set_pct == 30);
    window(3180);
    assert(set_pct == 30);
}
static void off_stays_off(void) {
    window(3180);
    s_bright_req = 0;
    backlight_apply();
    assert(set_pct == 0);
    window(4100);
    assert(set_pct == 0);
    window(2500); /* Below the BLDO1 floor: still dimmest, never off while on. */
    s_bright_req = 100;
    backlight_apply();
    assert(set_pct == 1);
}
static void dip_inside_window_counts(void) {
    s_bright_req = 100;
    backlight_apply();
    vsys_mv = 4100;
    for (int i = 0; i < BACKLIGHT_WINDOW; i++) {
        vsys_mv = i == 3 ? 3180 : 4100;
        backlight_track_vsys();
        now_us += 1000000;
    }
    assert(set_pct == 50);
}
static void failed_set_retries(void) {
    s_bright_req = 100;
    set_error = -1;
    backlight_apply();
    assert(set_pct == -1);
    set_error = 0;
    backlight_apply();
    assert(set_pct == 100 && sets == 1);
    backlight_apply();
    assert(sets == 1); /* No I2C write when nothing changed. */
}
int main(int argc, char **argv) {
    assert(argc == 2);
    switch (atoi(argv[1])) {
    case 0: low_battery_caps_and_recovers(); break;
    case 1: cap_never_raises_the_request(); break;
    case 2: off_stays_off(); break;
    case 3: dip_inside_window_counts(); break;
    case 4: failed_set_retries(); break;
    default: return 2;
    }
    return 0;
}
'''
        c = Path(cls.tmp.name) / "core2_backlight.c"
        c.write_text(source)
        cls.binary = c.with_suffix("")
        subprocess.run([*shlex.split(os.environ.get("CC", "cc")), "-std=c11", "-Wall",
                        "-Wextra", "-Werror", str(c), "-o", str(cls.binary)],
                       check=True, capture_output=True)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def test_low_battery_caps_and_recovers(self):
        subprocess.run([str(self.binary), "0"], check=True)

    def test_cap_never_raises_the_request(self):
        subprocess.run([str(self.binary), "1"], check=True)

    def test_off_stays_off(self):
        subprocess.run([str(self.binary), "2"], check=True)

    def test_dip_inside_the_window_sets_the_cap(self):
        subprocess.run([str(self.binary), "3"], check=True)

    def test_failed_set_retries_and_unchanged_is_skipped(self):
        subprocess.run([str(self.binary), "4"], check=True)


if __name__ == "__main__":
    unittest.main()
