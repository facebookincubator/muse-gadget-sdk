# Copyright (c) Meta Platforms, Inc. and affiliates.
# SPDX-License-Identifier: Apache-2.0
"""Connect independently compiled sprite packs to the real C parser/renderer."""
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile
import unittest
from pixel_test_sanitizers import ASAN_OPTIONS

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT.parent))
try:
    from avatar_pixel import compile_image,validate_pack
    from avatar_pixel.tests.fixtures import avatar,encode
except ImportError:
    compile_image=None

HARNESS=r'''
#include "muse_pixel_art.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
int main(int argc,char **argv) {
 assert(argc==2);FILE *f=fopen(argv[1],"rb");assert(f);fseek(f,0,SEEK_END);long size=ftell(f);rewind(f);
 assert(size>64 && size<=MUSE_PIXEL_PACK_MAX);uint8_t *bytes=malloc((size_t)size);assert(bytes && fread(bytes,1,(size_t)size,f)==(size_t)size);fclose(f);
 muse_pixel_pack_t pack;assert(muse_pixel_pack_parse(bytes,(size_t)size,&pack));assert(pack.colors<=24);
 uint16_t row[320];
 for(unsigned mode=0;mode<7;mode++) {
  muse_pixel_pose_t pose={.time_ms=1435,.happy=500,.level=200,.mode=(uint8_t)mode,.working=true};
  for(int y=0;y<320;y++)assert(muse_pixel_art_blit(&pack,320,&pose,0,row,320,0,319,y,y));
 }
 free(bytes);puts("PASS compiler pack consumed by C with all local states");
}
'''


@unittest.skipIf(compile_image is None,'Pillow and the local avatar_pixel package are required')
class PixelCompilerIntegration(unittest.TestCase):
    def test_diverse_compiler_packs_in_production_c_renderer(self):
        with tempfile.TemporaryDirectory() as tmp:
            stage=Path(tmp);(stage/'test.c').write_text(HARNESS);exe=stage/'compiler-c-test'
            subprocess.run([*shlex.split(os.environ.get('CC','cc')),'-std=c11','-O1','-g',
                            '-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
                            '-I',str(ROOT/'components/muse'),str(stage/'test.c'),
                            str(ROOT/'components/muse/muse_pixel_art.c'),'-o',str(exe)],check=True)
            for kind in ('robot','animal','plant','logo'):
                for profile in (64,96):
                    with self.subTest(kind=kind,profile=profile):
                        image=avatar(kind)
                        try: result=compile_image(encode(image),profile=profile,colors=24,dither=True)
                        finally: image.close()
                        self.assertEqual(validate_pack(result.pack)['width'],profile)
                        asset=stage/(kind+str(profile)+'.mspix');asset.write_bytes(result.pack)
                        subprocess.run([str(exe),str(asset)],check=True,timeout=30,
                                       env={**os.environ,'ASAN_OPTIONS':ASAN_OPTIONS})


if __name__=='__main__':
    unittest.main()
