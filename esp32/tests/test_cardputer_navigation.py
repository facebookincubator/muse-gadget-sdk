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

"""Compile the production keyboard decoder/menu handlers against small host fakes."""
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def function(source, name):
    match = re.search(r"static (?:unsigned|void|int) " + name + r"\([^)]*\)\n\{", source)
    start = match.start()
    brace = source.index("{", start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


class CardputerNavigation(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        menu = (ROOT / 'components/muse/muse_menu.c').read_text()
        board = (ROOT / 'components/muse/boards/board_m5stack_cardputer_adv.c').read_text()
        header = (ROOT / 'components/muse/muse_board.h').read_text()
        menu_header = (ROOT / 'components/muse/muse_menu.h').read_text()
        defines = '\n'.join(re.findall(r'^#define MUSE_BTN_.*', header, re.M))
        enums = re.search(r'typedef enum \{.*?\} muse_menu_key_t;', menu_header, re.S)[0]
        items = re.search(r'typedef enum \{.*?\} item_t;', menu, re.S)[0]
        views = re.search(r'typedef enum \{\s*VIEW_CLOSED.*?\} view_t;', menu, re.S)[0]
        source = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#define ESP_OK 0
#define KEY_IRQ 11
''' + defines + '\n' + enums + '\n' + items + '\n' + views + r'''
static struct { bool pressed; } s_talk;
static bool s_space, s_talk_down;
static uint8_t raw, status;
static int irq = 1;
static void muse_gpio_button_poll(void *p) { (void)p; }
static int gpio_get_level(int p) { (void)p; return irq; }
static int key_read(uint8_t reg, uint8_t *v) {
    *v = reg == 2 ? status : raw;
    if (reg == 4) raw = 0;
    return ESP_OK;
}
static int key_write(uint8_t r, uint8_t v) { (void)r; (void)v; return ESP_OK; }
static view_t s_view;
static int s_sel, activated = -1, direction, power_count, reset_count;
static void refresh(void) {}
static void open_menu(void) { s_view = VIEW_LIST; s_sel = 0; }
static void muse_menu_close(void) { s_view = VIEW_CLOSED; }
static void show(view_t v) { s_view = v; }
static void activate(int item, int dir) { activated = item; direction = dir; }
static void muse_input_request_power_off(void) { ++power_count; }
static void muse_link_reset_setup(void) { ++reset_count; }
static void muse_state_set_caption(const char *p) { (void)p; }
''' + function(board, 'poll_buttons') + '\n' + function(menu, 'handle') + '\n' + function(menu, 'value_step') + r'''
static unsigned key(uint8_t event) { irq = 0; raw = event; return poll_buttons(); }
static void keyboard_test(void) {
    /* Actual TCA8418 events for the key legends on the ADV. */
    const uint8_t keys[] = {1,57,58,54,64,67};
    const unsigned bits[] = {MUSE_BTN_ESCAPE,MUSE_BTN_UP,MUSE_BTN_DOWN,
                            MUSE_BTN_LEFT,MUSE_BTN_RIGHT,MUSE_BTN_ENTER};
    for (int i = 0; i < 6; ++i) {
        assert(key(keys[i] | 0x80) == bits[i]);
        assert(key(keys[i]) == 0);
    }
    assert(key(3 | 0x80) == 0); /* Fn doesn't consume arrow presses. */
    assert(key(57 | 0x80) == MUSE_BTN_UP);
    assert(key(57) == 0);
    assert(key(3) == 0);
    assert(key(68 | 0x80) == MUSE_BTN_TALK_PRESS);
    s_talk.pressed = true;
    assert(key(68) == 0);  /* GO still held: don't terminate recording. */
    s_talk.pressed = false;
    assert(key(0) == MUSE_BTN_TALK_RELEASE);
    assert(key(68 | 0x80) == MUSE_BTN_TALK_PRESS);
    status = 8;  /* FIFO overflow releases held Space. */
    assert(key(0) == MUSE_BTN_TALK_RELEASE);
    status = 0;
    assert(key(42 | 0x80) == 0); /* Unmapped character has no action. */
}
static void menu_test(void) {
    handle(MUSE_MENU_BACK); assert(s_view == VIEW_LIST);
    handle(MUSE_MENU_UP); assert(s_sel == ITEM_COUNT - 1);
    handle(MUSE_MENU_DOWN); assert(s_sel == 0);
    handle(MUSE_MENU_RIGHT); assert(activated == 0 && direction == 1);
    handle(MUSE_MENU_LEFT); assert(activated == 0 && direction == -1);
    activated = -1; s_sel = ITEM_RESET;
    handle(MUSE_MENU_RIGHT); assert(activated == -1);
    handle(MUSE_MENU_LEFT); assert(activated == -1);
    handle(MUSE_MENU_BACK); assert(s_view == VIEW_CLOSED);
    handle(MUSE_MENU_SELECT); assert(s_view == VIEW_LIST);
    for (int v = VIEW_POWER; v <= VIEW_RESET; ++v) {
        const muse_menu_key_t keys[] = {MUSE_MENU_UP,MUSE_MENU_DOWN,MUSE_MENU_LEFT,MUSE_MENU_RIGHT,MUSE_MENU_BACK};
        for (int i = 0; i < 5; ++i) {
            s_view = v; handle(keys[i]);
            assert(power_count == 0 && reset_count == 0);
        }
    }
    s_view = VIEW_POWER; handle(MUSE_MENU_SELECT); assert(power_count == 1);
    s_view = VIEW_RESET; handle(MUSE_MENU_SELECT); assert(reset_count == 1);
    s_view = VIEW_STATUS; handle(MUSE_MENU_BACK); assert(s_view == VIEW_LIST);
    s_view = VIEW_BATTERY; handle(MUSE_MENU_BACK); assert(s_view == VIEW_LIST);
}
static void values_test(void) {
    const int steps[] = {10,25,50,75,100};
    assert(value_step(steps,5,50,-1) == 25);
    assert(value_step(steps,5,50,1) == 75);
    assert(value_step(steps,5,10,-1) == 100);
    assert(value_step(steps,5,100,1) == 10);
    assert(value_step(steps,5,60,-1) == 50);
    assert(value_step(steps,5,60,1) == 75);
}
int main(int argc, char **argv) {
    assert(argc == 2);
    switch (atoi(argv[1])) {
    case 0: keyboard_test(); break;
    case 1: menu_test(); break;
    case 2: values_test(); break;
    default: return 2;
    }
    return 0;
}
'''
        c = Path(cls.tmp.name) / 'navigation.c'
        c.write_text(source)
        cls.binary = c.with_suffix('')
        subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-Wall', '-Wextra', '-Werror',
                        str(c), '-o', str(cls.binary)], check=True, capture_output=True)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def test_keyboard_legends_and_overlapping_talk_keys(self):
        subprocess.run([str(self.binary), '0'], check=True)

    def test_navigation_never_accidentally_confirms_destructive_actions(self):
        subprocess.run([str(self.binary), '1'], check=True)

    def test_bidirectional_values(self):
        subprocess.run([str(self.binary), '2'], check=True)
