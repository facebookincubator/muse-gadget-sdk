# Copyright (c) Meta Platforms, Inc. and affiliates.
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
# http://www.apache.org/licenses/LICENSE-2.0
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
"""Bounded background detection and the independent theme adapter."""
import os,subprocess,shlex,tempfile
from pathlib import Path
import unittest
ROOT=Path(__file__).resolve().parents[1]
class Background(unittest.TestCase):
 def test_detection_padding_and_revision_cache(self):
  source=r"""
#include "muse_avatar_background.h"
#include "muse_avatar.h"
#include "muse_avatar_theme.h"
#include "muse_theme.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
static uint16_t pixels[64*64];
static muse_avatar_image_t image={.data={pixels,64,64},.revision=1};
static bool selected,valid,loading;
static uint32_t content_epoch=1;
static uint32_t hint;
static unsigned refs;
uint32_t muse_avatar_content_epoch(void) {return content_epoch;}
muse_avatar_source_t muse_avatar_source(void) {return selected?MUSE_AVATAR_PERSONAL:MUSE_AVATAR_BUILTIN;}
const muse_avatar_image_t *muse_avatar_acquire(void) {if(!selected || loading)return NULL;refs++;return &image;}
void muse_avatar_release(const muse_avatar_image_t *p) {if(p){assert(p==&image && refs);refs--;}}
void muse_theme_set_hint(bool v,uint32_t rgb) {valid=v;hint=rgb;}
static void fill(uint16_t c) {for(unsigned i=0;i<64*64;i++)pixels[i]=c;}
int main(void) {
 uint32_t bg;unsigned confidence;
 fill(0xffff);assert(muse_avatar_background_estimate(&image.data,&bg,&confidence) && bg==0xffffff && confidence==100);
 fill(0);assert(muse_avatar_background_estimate(&image.data,&bg,&confidence) && bg==0);
 fill(0xf800);assert(muse_avatar_background_estimate(&image.data,&bg,&confidence) && bg==0xff0000);
 fill(0xffff);for(int y=32;y<64;y++)for(int x=32;x<64;x++)pixels[y*64+x]=0x001f;
 assert(muse_avatar_background_estimate(&image.data,&bg,&confidence) && bg==0xffffff && confidence>=80);
 for(int y=0;y<64;y++)for(int x=32;x<64;x++)pixels[y*64+x]=0;
 assert(!muse_avatar_background_estimate(&image.data,&bg,&confidence));
 muse_avatar_pixels_t bad=image.data;bad.width=0;assert(!muse_avatar_background_estimate(&bad,&bg,NULL));
 bad.width=467;assert(!muse_avatar_background_estimate(&bad,&bg,NULL));
 fill(0xffff);pixels[32*64+32]=0;
 uint16_t out[128*128];assert(muse_avatar_blit_background(&image.data,128,950,0xffff,out,128,0,127,0,127));
 assert(out[0]==0xffff && out[64*128+64]==0); /* black features are never keyed out */
 selected=true;content_epoch=0;muse_avatar_theme_tick();assert(!valid && !refs); /* pending invalidation, even before first sample */
 content_epoch=1;muse_avatar_theme_tick();assert(valid && hint==0xffffff && !refs);
 fill(0);muse_avatar_theme_tick();assert(valid && hint==0xffffff && !refs); /* stable revision */
 image.revision++;muse_avatar_theme_tick();assert(valid && hint==0 && !refs);
 selected=false;muse_avatar_theme_tick();assert(!valid && !refs);
 selected=true;loading=true;muse_avatar_theme_tick();assert(valid && hint==0 && !refs);
 content_epoch=0;muse_avatar_theme_tick();assert(!valid && !refs); /* clear/account change */
 loading=false;muse_avatar_theme_tick();assert(!valid && !refs); /* old image pending cleanup */
 fill(0xffff);image.revision++;content_epoch=2;muse_avatar_theme_tick();assert(valid && hint==0xffffff && !refs);
 for(unsigned i=0;i<1000;i++) {
  selected=false;muse_avatar_theme_tick();assert(!valid && !refs);
  selected=true;loading=true;muse_avatar_theme_tick();assert(valid && hint==0xffffff && !refs);
  loading=false;muse_avatar_theme_tick();assert(valid && hint==0xffffff && !refs);
 }
 puts("PASS background: white/dark/color, confidence, bounds, non-destructive padding, revision cache");
}
"""
  with tempfile.TemporaryDirectory() as tmp:
   p=Path(tmp);(p/'test.c').write_text(source);(p/'sdkconfig.h').write_text('#define CONFIG_MUSE_UI_THEME 1\n')
   if not (ROOT/'components/muse/muse_theme.h').exists():
    (p/'muse_theme.h').write_text('#include <stdbool.h>\n#include <stdint.h>\nvoid muse_theme_set_hint(bool valid,uint32_t rgb);\n')
   subprocess.run([*shlex.split(os.environ.get('CC','cc')),'-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
    '-I',tmp,'-I',str(ROOT/'components/muse'),str(p/'test.c'),
    *[str(ROOT/'components/muse'/f) for f in ['muse_avatar_background.c','muse_avatar_render.c','muse_avatar_theme.c']],'-o',str(p/'test')],check=True)
   subprocess.run([str(p/'test')],check=True)
