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

"""Compile the Waveshare round boards' hold-the-face talk against LVGL fakes."""
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
BOARDS = ("board_waveshare_s3_146.c", "board_waveshare_s3_143c.c")


def function(source, name):
    match = re.search(r"static (?:unsigned|int|void) " + name + r"\([^)]*\)\n\{", source)
    start = match.start()
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


HARNESS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
%(defines)s
typedef uint32_t TickType_t;
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
#define LV_INDEV_STATE_RELEASED 0
#define LV_INDEV_STATE_PRESSED 1
typedef struct { int unused; } lv_indev_t;
typedef struct { int unused; } lv_obj_t;
typedef struct { int unused; } muse_gpio_button_t;
static muse_gpio_button_t s_boot;
static lv_indev_t indev, *s_touch_indev = &indev;
static lv_obj_t tileview;
static TickType_t now;
static bool pressed, scrolling, face = true;
static TickType_t xTaskGetTickCount(void) { return now; }
static unsigned muse_gpio_button_poll(muse_gpio_button_t *b) { (void)b; return 0; }
static int lv_indev_get_state(lv_indev_t *i) { (void)i; return pressed; }
__attribute__((unused)) static lv_obj_t *lv_indev_get_scroll_obj(lv_indev_t *i) { (void)i; return scrolling ? &tileview : NULL; }
static bool muse_ui_face_active(void) { return face; }
%(poll)s
static unsigned step(TickType_t ms) { now += ms; return poll_buttons(); }
static void hold_talks(void) {
    pressed = true;
    assert(step(10) == 0);
    assert(step(400) == 0);
    assert(step(100) == MUSE_BTN_TALK_PRESS);
    assert(step(10) == 0);
    pressed = false;
    assert(step(10) == MUSE_BTN_TALK_RELEASE);
}
static void slow_swipe_never_talks(void) {
    pressed = true;
    assert(step(10) == 0);
    scrolling = true;        /* LVGL took the drag before the threshold. */
    assert(step(200) == 0);
    face = false;            /* Settings tile now in view. */
    scrolling = false;       /* Finger rests on the new tile past 500 ms. */
    assert(step(600) == 0);
    face = true;             /* Even dragged back onto the face. */
    assert(step(600) == 0);
    pressed = false;
    assert(step(10) == 0);   /* No talk started, so no release either. */
    pressed = true;          /* The next press is a fresh hold. */
    assert(step(10) == 0);
    assert(step(500) == MUSE_BTN_TALK_PRESS);
    pressed = false;
    assert(step(10) == MUSE_BTN_TALK_RELEASE);
}
static void off_face_press_never_talks(void) {
    face = false;
    pressed = true;
    assert(step(10) == 0);
    face = true;
    assert(step(1000) == 0);
    pressed = false;
    assert(step(10) == 0);
}
int main(int argc, char **argv) {
    assert(argc == 2);
    switch (atoi(argv[1])) {
    case 0: hold_talks(); break;
    case 1: slow_swipe_never_talks(); break;
    case 2: off_face_press_never_talks(); break;
    default: return 2;
    }
    return 0;
}
'''


class WaveshareTouchHold(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cc = shlex.split(os.environ.get("CC", "cc"))
        if not cc or not shutil.which(cc[0]):
            raise unittest.SkipTest("C compiler unavailable")
        cls.tmp = tempfile.TemporaryDirectory()
        header = (ROOT / "components/muse/muse_board.h").read_text()
        btn = "\n".join(re.findall(r"^#define MUSE_BTN_.*", header, re.M))
        cls.binaries = {}
        for name in BOARDS:
            board = (ROOT / "components/muse/boards" / name).read_text()
            hold = re.search(r"^#define TOUCH_HOLD_MS .*", board, re.M)[0]
            c = Path(cls.tmp.name) / name
            c.write_text(HARNESS % {"defines": btn + "\n" + hold,
                                    "poll": function(board, "poll_buttons")})
            binary = c.with_suffix("")
            subprocess.run([*cc, "-std=c11", "-Wall", "-Wextra", "-Werror",
                            str(c), "-o", str(binary)], check=True, capture_output=True)
            cls.binaries[name] = binary

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def run_case(self, case):
        for name, binary in self.binaries.items():
            with self.subTest(board=name):
                subprocess.run([str(binary), case], check=True)

    def test_hold_on_face_talks_and_release_ends(self):
        self.run_case("0")

    def test_slow_swipe_off_face_never_talks(self):
        self.run_case("1")

    def test_press_starting_off_face_never_talks(self):
        self.run_case("2")
