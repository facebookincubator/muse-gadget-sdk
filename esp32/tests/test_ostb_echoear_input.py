# Copyright (c) 2026 Mihir Jadhav. SPDX-License-Identifier: Apache-2.0
"""Compile the actual touch decoder and input-edge path with host C checks."""
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def function(source, name):
    match = re.search(r'(?:static )?void ' + name + r'\([^)]*\)\s*\{', source)
    start, pos, depth = match.start(), match.end(), 1
    while depth:
        depth += (source[pos] == '{') - (source[pos] == '}')
        pos += 1
    return source[start:pos]


class EchoEarInputTest(unittest.TestCase):
    def test_packet_bounds_and_touch_edges(self):
        cc = shlex.split(os.environ.get('CC', 'cc'))
        if not cc or not shutil.which(cc[0]):
            self.skipTest('C compiler unavailable')
        source = (ROOT / 'components/muse/muse_input.c').read_text()
        code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include "ostb_echoear_touch.h"
#define MUSE_BTN_TALK_PRESS 1u
#define MUSE_BTN_TALK_RELEASE 2u
#define MUSE_PTT_DOWN 1
#define MUSE_PTT_UP 2
#define MUSE_MENU_SELECT 1
static atomic_uint s_touch_talk_events;
static bool s_talk_down, pending_pair;
static int down, up, confirmed;
static struct { const char *talk_button; bool keyboard, touch_talk; } board = {"screen", false, true};
typedef int lv_event_t;
#define LV_EVENT_PRESSED 1
#define LV_EVENT_RELEASED 2
#define LV_EVENT_PRESS_LOST 3
#define MUSE_LINK_CONFIRM 1
static int link_state;
static int lv_event_get_code(lv_event_t *e) { return *e; }
static int muse_link_state(void) { return link_state; }
#define muse_board (&board)
static bool muse_link_talk_press(void) { if (pending_pair) { confirmed++; return true; } return false; }
static bool muse_state_asleep(void) { return false; }
static bool muse_menu_is_open(void) { return false; }
static void muse_state_poke(void) {}
static void muse_menu_key(int key) { (void)key; }
static void set_asleep(bool sleep, const char *why) { (void)sleep; (void)why; }
static void post(int type, bool wake) { (void)wake; if (type==MUSE_PTT_DOWN) down++; else up++; }
'''
        code += function(source, 'muse_input_touch_talk') + '\n' + function(source, 'talk_button')
        ui = (ROOT / 'components/muse/muse_ui.c').read_text()
        code += '\n' + function(ui, 'on_touch_mic') + '\n' + function(ui, 'on_any_press')
        code += r'''
int main(void) {
    uint16_t x=9,y=8;
    uint8_t packet[6]={1,1,103,1,103,0};
    assert(ostb_echoear_touch_decode(packet,&x,&y) && x==359 && y==359);
    packet[2]=104; assert(!ostb_echoear_touch_decode(packet,&x,&y) && x==359);
    packet[2]=103;packet[4]=104;assert(!ostb_echoear_touch_decode(packet,&x,&y));
    packet[0]=0;assert(!ostb_echoear_touch_decode(packet,&x,&y) && y==359);
    packet[0]=2;assert(!ostb_echoear_touch_decode(packet,&x,&y));
    muse_input_touch_talk(true);talk_button(atomic_exchange(&s_touch_talk_events,0));
    assert(down==1 && up==0 && s_talk_down);
    muse_input_touch_talk(false);talk_button(atomic_exchange(&s_touch_talk_events,0));
    assert(up==1 && !s_talk_down);
    muse_input_touch_talk(true);muse_input_touch_talk(false);
    talk_button(atomic_exchange(&s_touch_talk_events,0));
    assert(down==2 && up==2 && atomic_load(&s_touch_talk_events)==0);
    pending_pair=true;muse_input_touch_talk(true);muse_input_touch_talk(false);
    talk_button(atomic_exchange(&s_touch_talk_events,0));
    assert(confirmed==1 && down==2 && up==2 && !s_talk_down);
    pending_pair=false;
    lv_event_t event=LV_EVENT_PRESSED;on_touch_mic(&event);
    talk_button(atomic_exchange(&s_touch_talk_events,0));assert(down==3 && s_talk_down);
    event=LV_EVENT_PRESS_LOST;on_touch_mic(&event);
    talk_button(atomic_exchange(&s_touch_talk_events,0));assert(up==3 && !s_talk_down);
    event=LV_EVENT_PRESSED;on_touch_mic(&event);
    event=LV_EVENT_RELEASED;on_touch_mic(&event);
    talk_button(atomic_exchange(&s_touch_talk_events,0));assert(down==4 && up==4);
    on_any_press(&event);assert(atomic_load(&s_touch_talk_events)==0);
    pending_pair=true;link_state=MUSE_LINK_CONFIRM;on_any_press(&event);
    talk_button(atomic_exchange(&s_touch_talk_events,0));
    assert(confirmed==2 && down==4 && up==4 && !s_talk_down);
    board.touch_talk=false;on_any_press(&event);assert(atomic_load(&s_touch_talk_events)==0);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as d:
            path = Path(d)
            (path / 'check.c').write_text(code)
            subprocess.run(cc + ['-std=c11', '-Wall', '-Wextra', '-Werror',
                            '-I', str(ROOT / 'components/muse/boards'),
                            str(path / 'check.c'), '-o', str(path / 'check')], check=True)
            subprocess.run([str(path / 'check')], check=True)


if __name__ == '__main__':
    unittest.main()
