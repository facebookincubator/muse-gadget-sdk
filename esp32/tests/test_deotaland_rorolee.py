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

"""The RoRoLee's quarter turn and the menu's wake caption, compiled on the host."""
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
MUSE = ROOT / 'components/muse'


def function(source, name):
    match = re.search(r"static (?:esp_err_t|void) " + name + r"\([^)]*\)\n\{", source)
    start = match.start()
    brace = source.index("{", start)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def build_and_run(test, code):
    path = Path(test.tmp.name) / 'prog.c'
    path.write_text(code)
    cc = shlex.split(os.environ.get('CC', 'cc'))
    subprocess.run(cc + ['-std=c11', '-Wall', '-Wextra', '-Werror', str(path), '-o', str(path.with_suffix(''))],
                   check=True, capture_output=True, text=True)
    subprocess.run([str(path.with_suffix(''))], check=True)


class DeotalandRoRoLee(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)

    def test_draw_turned_maps_ui_onto_the_portrait_panel(self):
        board = (MUSE / 'boards/board_deotaland_rorolee.c').read_text()
        code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
typedef int esp_err_t;
typedef void *esp_lcd_panel_handle_t;
typedef struct { bool last; int ready; } lv_display_t;
#define ESP_OK 0
#define LCD_W 120
#define LCD_H 240
#define UI_W LCD_H
#define UI_H LCD_W
static uint16_t s_frame[LCD_W * LCD_H];
static int frames;
static esp_err_t send_frame(void) { ++frames; return ESP_OK; }
static bool lv_display_flush_is_last(lv_display_t *d) { return d->last; }
static void lv_display_flush_ready(lv_display_t *d) { ++d->ready; }
''' + function(board, 'draw_turned') + r'''
/* A pixel's value names its UI position. */
static uint16_t at(int x, int y) { return (uint16_t)(y * UI_W + x); }
static uint16_t px[UI_W * UI_H];
static void area(lv_display_t *d, int x1, int y1, int x2, int y2) {
    const int w = x2 - x1;
    for (int y = y1; y < y2; y++)
        for (int x = x1; x < x2; x++)
            px[(y - y1) * w + (x - x1)] = at(x, y);
    assert(draw_turned(d, NULL, x1, y1, x2, y2, px, NULL) == ESP_OK);
}
/* UI (x, y) lands on panel column y, row 239 - x. */
static uint16_t panel(int x, int y) { return s_frame[(LCD_H - 1 - x) * LCD_W + y]; }
int main(void) {
    lv_display_t d = {0};
    memset(s_frame, 0xff, sizeof(s_frame));
    /* Two offset areas of one refresh: only the last sends the frame. */
    area(&d, 3, 5, 20, 9);
    assert(frames == 0 && d.ready == 1);
    d.last = true;
    area(&d, 200, 100, 240, 120);
    assert(frames == 1 && d.ready == 2);
    for (int y = 5; y < 9; y++)
        for (int x = 3; x < 20; x++)
            assert(panel(x, y) == at(x, y));
    for (int y = 100; y < 120; y++)
        for (int x = 200; x < 240; x++)
            assert(panel(x, y) == at(x, y));
    assert(panel(2, 5) == 0xffff && panel(20, 5) == 0xffff && panel(3, 9) == 0xffff);
    /* The whole screen, corners included, stays inside the copy. */
    area(&d, 0, 0, UI_W, UI_H);
    assert(frames == 2);
    assert(s_frame[(LCD_H - 1) * LCD_W] == at(0, 0));
    assert(s_frame[LCD_W - 1] == at(UI_W - 1, UI_H - 1));
    assert(s_frame[0] == at(UI_W - 1, 0));
    assert(s_frame[LCD_W * LCD_H - 1] == at(0, UI_H - 1));
    for (int y = 0; y < UI_H; y++)
        for (int x = 0; x < UI_W; x++)
            assert(panel(x, y) == at(x, y));
    return 0;
}
'''
        build_and_run(self, code)

    def test_menu_power_page_names_the_wake_button(self):
        menu = (MUSE / 'muse_menu.c').read_text()
        case = re.search(r'    case VIEW_POWER: \{\n.*?\n        break;\n    \}\n', menu, re.S)[0]
        code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
typedef enum { VIEW_POWER } view_t;
static const char *s_title = "title", *s_page = "page", *s_hint_down = "down";
static char page[96];
static void set_text(const char *l, const char *text) { if (l == s_page) snprintf(page, sizeof(page), "%s", text); }
static void select_hint(const char *t) { (void)t; }
static struct { bool keyboard; const char *aux_button, *wake_button; } board, *muse_board = &board;
static void show(view_t view) {
    switch (view) {
''' + case + r'''
    }
}
int main(void) {
    board.aux_button = "vol-"; board.wake_button = "boot";
    show(VIEW_POWER);
    assert(strcmp(page, "Turn Muse off?\n\nPress the boot button to turn it back on.") == 0);
    board.wake_button = NULL;
    show(VIEW_POWER);
    assert(strcmp(page, "Turn Muse off?\n\nPress the vol- button to turn it back on.") == 0);
    board.keyboard = true;
    show(VIEW_POWER);
    assert(strcmp(page, "Turn Muse off?\n\nPress the GO button to turn it back on.") == 0);
    return 0;
}
'''
        build_and_run(self, code)

    def test_board_wakes_on_the_button_it_names(self):
        board = (MUSE / 'boards/board_deotaland_rorolee.c').read_text()
        self.assertIn('.wake_button = "boot"', board)
        self.assertIn('esp_sleep_enable_ext0_wakeup(TALK_GPIO, 0)', board)
        self.assertRegex(board, r'#define TALK_GPIO GPIO_NUM_0\b')


if __name__ == '__main__':
    unittest.main()
