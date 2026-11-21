# Copyright (c) Meta Platforms, Inc. and affiliates.
# SPDX-License-Identifier: Apache-2.0
"""Validate the animated roller native's handle/type/range checks."""
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
typedef void *wasm_exec_env_t;
typedef struct { int type; void *obj; } slot_t;
static slot_t slot={W_ROLLER,0};
static slot_t *obj_arg(int obj) { return obj==1 ? &slot : NULL; }
static uint32_t lv_roller_get_option_count(void *obj) { (void)obj;return 60; }
enum {LV_ANIM_OFF,LV_ANIM_ON};
static int last_index=-1,last_animation=-1;
static void lv_roller_set_selected(void *obj,uint32_t index,int animation) { (void)obj;last_index=index;last_animation=animation; }
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
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            p=Path(tmp); (p/'test.c').write_text(code)
            subprocess.run([*shlex.split(os.environ.get('CC','cc')),'-std=c11','-Wall','-Wextra','-Werror', '-I',str(ROOT/'components/muse_apps/sdk'),str(p/'test.c'),'-o',str(p/'test')],check=True)
            subprocess.run([str(p/'test')],check=True)
