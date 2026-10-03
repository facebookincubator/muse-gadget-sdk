# Copyright (c) 2026 Patrick McDowell
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

"""Compile production Core2 input handlers against touch and PMU fakes."""
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def function(source, name):
    match = re.search(r"static (?:unsigned|int|void) " + name + r"\([^)]*\)\n\{", source)
    start = match.start()
    brace = source.index("{", start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


class Core2Buttons(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        board = (ROOT / "components/muse/boards/board_m5stack_core2.c").read_text()
        header = (ROOT / "components/muse/muse_board.h").read_text()
        defines = "\n".join(re.findall(r"^#define MUSE_BTN_.*", header, re.M))
        pek_define = re.search(r"^#define AXP_PEK_STATUS .*", board, re.M)[0]
        source = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#define ESP_OK 0
''' + defines + "\n" + pek_define + r'''
static bool s_talk_pressed;
static void *s_axp;
static int zone, read_error, clear_error, shutdowns, clears;
static uint8_t latched;
static int tp_zone(void) { return zone; }
static int axp_read(uint8_t reg, uint8_t *out, size_t len) {
    assert(reg == AXP_PEK_STATUS && len == 1);
    *out = latched;
    return read_error;
}
static int i2c_master_transmit(void *dev, const uint8_t *data, size_t len, int timeout) {
    (void)dev; (void)timeout;
    assert(len == 2 && data[0] == AXP_PEK_STATUS);
    assert((data[1] & ~0x03) == 0); /* Leave other IRQ sources alone. */
    if (clear_error) return clear_error;
    latched &= ~data[1];
    ++clears;
    return ESP_OK;
}
static void muse_input_request_power_off(void) { ++shutdowns; }
#define BSP_LCD_H_RES 320
#define BSP_LCD_V_RES 240
#define LV_INDEV_STATE_RELEASED 0
#define LV_INDEV_STATE_PRESSED 1
typedef struct { int x, y; } lv_point_t;
typedef struct { int unused; } lv_indev_t;
typedef struct { lv_point_t point; int state; } lv_indev_data_t;
static lv_point_t s_touch_point;
static lv_indev_data_t touch_data;
static void fake_touch_read(lv_indev_t *indev, lv_indev_data_t *data) {
    (void)indev;
    *data = touch_data;
}
static void (*s_touch_read)(lv_indev_t *, lv_indev_data_t *) = fake_touch_read;
''' + "\n".join(function(board, name) for name in ("pek_read", "poll_buttons", "touch_read")) + r'''
static void key_events(void) {
    latched = 2; /* AXP192 short-press event, not a held level. */
    assert(poll_buttons() == (MUSE_BTN_AUX_PRESS | MUSE_BTN_AUX_RELEASE));
    assert(clears == 1 && shutdowns == 0);
    assert(poll_buttons() == 0);
    latched = 1; /* PMU long-press event requests clean shutdown. */
    assert(poll_buttons() == 0 && shutdowns == 1);
    assert(poll_buttons() == 0 && shutdowns == 1);
    latched = 3; /* Long press takes priority over a stale short event. */
    assert(poll_buttons() == 0 && shutdowns == 2);
    latched = 0x80; /* Unrelated IRQ bit is neither cleared nor a key. */
    assert(poll_buttons() == 0 && latched == 0x80 && shutdowns == 2);
}
static void touch_edges(void) {
    zone = 1;
    assert(poll_buttons() == MUSE_BTN_TALK_PRESS);
    assert(poll_buttons() == 0); /* Holding BtnB doesn't repeat presses. */
    zone = -1;
    assert(poll_buttons() == 0); /* I2C failure doesn't end a recording. */
    zone = 0;
    assert(poll_buttons() == MUSE_BTN_TALK_RELEASE);
    assert(poll_buttons() == 0);
    zone = 1; latched = 2;
    assert(poll_buttons() == (MUSE_BTN_TALK_PRESS | MUSE_BTN_AUX_PRESS | MUSE_BTN_AUX_RELEASE));
}
static void pmu_errors(void) {
    latched = 1; read_error = -1;
    assert(poll_buttons() == 0 && shutdowns == 0 && clears == 0);
    read_error = 0; clear_error = -1;
    assert(poll_buttons() == 0 && shutdowns == 0 && clears == 0);
    clear_error = 0;
    assert(poll_buttons() == 0 && shutdowns == 1 && clears == 1);
    assert(poll_buttons() == 0 && shutdowns == 1);
}
static void screen_and_strip(void) {
    lv_indev_t indev = {0};
    lv_indev_data_t out;
    touch_data = (lv_indev_data_t){{319, 239}, LV_INDEV_STATE_PRESSED};
    touch_read(&indev, &out);
    assert(out.state == LV_INDEV_STATE_PRESSED && out.point.x == 319 && out.point.y == 239);
    touch_data = (lv_indev_data_t){{160, 277}, LV_INDEV_STATE_PRESSED};
    touch_read(&indev, &out);
    assert(out.state == LV_INDEV_STATE_RELEASED && out.point.x == 319 && out.point.y == 239);
    touch_data.state = LV_INDEV_STATE_RELEASED; /* BSP retains the strip point. */
    touch_read(&indev, &out);
    assert(out.state == LV_INDEV_STATE_RELEASED && out.point.y == 239);
    touch_data = (lv_indev_data_t){{100, 120}, LV_INDEV_STATE_PRESSED};
    touch_read(&indev, &out);
    assert(out.state == LV_INDEV_STATE_PRESSED && out.point.x == 100 && out.point.y == 120);
    touch_data = (lv_indev_data_t){{320, 120}, LV_INDEV_STATE_PRESSED};
    touch_read(&indev, &out);
    assert(out.state == LV_INDEV_STATE_RELEASED && out.point.x == 100);
}
int main(int argc, char **argv) {
    assert(argc == 2);
    switch (atoi(argv[1])) {
    case 0: key_events(); break;
    case 1: touch_edges(); break;
    case 2: pmu_errors(); break;
    case 3: screen_and_strip(); break;
    default: return 2;
    }
    return 0;
}
'''
        c = Path(cls.tmp.name) / "core2_buttons.c"
        c.write_text(source)
        cls.binary = c.with_suffix("")
        subprocess.run([*shlex.split(os.environ.get("CC", "cc")), "-std=c11", "-Wall",
                        "-Wextra", "-Werror", str(c), "-o", str(cls.binary)],
                       check=True, capture_output=True)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def test_latched_short_and_long_power_events(self):
        subprocess.run([str(self.binary), "0"], check=True)

    def test_touch_hold_release_and_read_failure(self):
        subprocess.run([str(self.binary), "1"], check=True)

    def test_pmu_errors_never_trigger_unacknowledged_shutdown(self):
        subprocess.run([str(self.binary), "2"], check=True)

    def test_touch_strip_is_excluded_from_lvgl(self):
        subprocess.run([str(self.binary), "3"], check=True)
