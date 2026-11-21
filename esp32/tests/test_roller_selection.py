# Copyright (c) Meta Platforms, Inc. and affiliates.
# SPDX-License-Identifier: Apache-2.0
"""Validate the animated roller native's checks, and its selections on an infinite roller."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[1]
class RollerSelection(unittest.TestCase):
    def test_selection_checks_and_animation(self):
        source = (ROOT/'components/muse_apps/muse_apps_ui.c').read_text()
        start = source.index('int32_t gadget_ui_roller_select(')
        function = source[start:source.index('\n}\n',start)+3].replace('GADGET_', '')
        code = r'''
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include "gadget.h"
#include <stdbool.h>
#include <stdlib.h>
typedef void *wasm_exec_env_t;
typedef struct { bool infinite; } roller_t;
typedef struct { int type; void *obj; void *ext; } slot_t;
static roller_t infinite={true};
static slot_t slot={W_ROLLER,0,0};
static slot_t *obj_arg(int obj) { return obj==1 ? &slot : NULL; }
enum {LV_ANIM_OFF,LV_ANIM_ON};
/* LVGL 9.5.0's index arithmetic (lv_roller.c): an infinite roller holds PAGES
 * copies of its N options. A select is read on the page shown, and only one
 * without animation (or an animation's end) moves back to the middle page. */
#define N 60
#define PAGES 3
static uint32_t id=N*(PAGES/2);
static int last_index=-1,last_animation=-1;
static uint32_t lv_roller_get_option_count(void *obj) { (void)obj;return N; }
static uint32_t lv_roller_get_selected(void *obj) { (void)obj;return slot.ext ? id%N : id; }
static void lv_roller_set_selected(void *obj,uint32_t index,int animation) {
 (void)obj;last_index=index;last_animation=animation;
 uint32_t count=slot.ext ? N*PAGES : N;
 if(slot.ext && index<N) {
  uint32_t page=id/N,shown=id-page*N;
  int32_t signed_index=index;
  if((uint32_t)abs((int16_t)(shown-index))>N/2) signed_index+=shown>index ? N : -N;
  index=signed_index+N*page;
 }
 id=index<count ? index : count-1;
 if(slot.ext && animation==LV_ANIM_OFF) id=id%N+N*(PAGES/2);
}
''' + function + r'''
int main(void) {
 assert(gadget_ui_roller_select(0,0,0,1)==ERR_HANDLE);
 slot.type=W_LABEL; assert(gadget_ui_roller_select(0,1,0,1)==ERR_UNSUPPORTED); slot.type=W_ROLLER;
 assert(gadget_ui_roller_select(0,1,-1,1)==ERR_ARG);
 assert(gadget_ui_roller_select(0,1,60,1)==ERR_ARG);
 assert(gadget_ui_roller_select(0,1,0,2)==ERR_ARG);
 assert(last_index==-1);
 assert(gadget_ui_roller_select(0,1,59,1)==0 && last_index==59 && last_animation==LV_ANIM_ON);
 assert(gadget_ui_roller_select(0,1,0,0)==0 && last_index==0 && last_animation==LV_ANIM_OFF);
 /* Infinite: selections made before the last animation ended still land on
  * the option asked for, across the wrap in both directions. */
 slot.ext=&infinite; id=N*(PAGES/2);
 const int jumps[]={59,30,1,58,0,31,59};
 for(int i=0;i<7;i++) assert(gadget_ui_roller_select(0,1,jumps[i],1)==0 && lv_roller_get_selected(0)==(uint32_t)jumps[i]);
 for(int i=1;i<=2*N;i++) assert(gadget_ui_roller_select(0,1,(59+i)%N,1)==0 && lv_roller_get_selected(0)==(uint32_t)((59+i)%N));
 for(int i=1;i<=2*N;i++) assert(gadget_ui_roller_select(0,1,(59+2*N-i)%N,1)==0 && lv_roller_get_selected(0)==(uint32_t)((59+2*N-i)%N));
 assert(last_animation==LV_ANIM_ON);
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            p=Path(tmp); (p/'test.c').write_text(code)
            subprocess.run([*shlex.split(os.environ.get('CC','cc')),'-std=c11','-Wall','-Wextra','-Werror', '-I',str(ROOT/'components/muse_apps/sdk'),str(p/'test.c'),'-o',str(p/'test')],check=True)
            subprocess.run([str(p/'test')],check=True)
