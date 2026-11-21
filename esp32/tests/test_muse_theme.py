"""Exercise the standalone palette, independently of avatar support."""
import os,subprocess,tempfile,shlex
from pathlib import Path
import unittest
ROOT=Path(__file__).resolve().parents[1]
class Theme(unittest.TestCase):
 def test_palette_and_contrast(self):
  source=r"""
#include "muse_theme_policy.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
 muse_theme_palette_t p=muse_theme_palette(MUSE_THEME_AUTO,false,0xffffff);
 assert(!p.light && !p.background && p.original_dark);
 unsigned colors[]={0x8b84a8,0xf2efff,0xa77dff,0x6ff0bf,0xffb45c,0xff5c5c};
 for(unsigned i=0;i<6;i++)assert(muse_theme_map(&p,colors[i],true)==colors[i]);
 unsigned bg[]={0xffffff,0xf7f3eb,0x777777,0x101825,0x754898,0x050505};
 for(unsigned k=0;k<6;k++) {
  p=muse_theme_palette(MUSE_THEME_AUTO,true,bg[k]);assert(p.background==bg[k]);
  for(unsigned i=0;i<6;i++)assert(muse_theme_contrast(muse_theme_map(&p,colors[i],true),bg[k])>=4.49f);
 }
 p=muse_theme_palette(MUSE_THEME_DARK,true,0xffffff);assert(!p.light && p.background==0);
 p=muse_theme_palette(MUSE_THEME_LIGHT,true,0);assert(p.light && p.background==0xffffff);
 assert(muse_theme_rgb565(0xffffff)==0xffff && muse_theme_rgb565(0)==0);
 puts("PASS standalone palette, contrast, overrides and RGB565");
}
"""
  with tempfile.TemporaryDirectory() as tmp:
   p=Path(tmp);(p/'test.c').write_text(source)
   subprocess.run([*shlex.split(os.environ.get('CC','cc')),'-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
    '-I',str(ROOT/'components/muse'),str(p/'test.c'),str(ROOT/'components/muse/muse_theme_policy.c'),'-lm','-o',str(p/'test')],check=True)
   subprocess.run([str(p/'test')],check=True)
