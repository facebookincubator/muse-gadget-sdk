# Copyright (c) Meta Platforms, Inc. and affiliates.
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at http://www.apache.org/licenses/LICENSE-2.0
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
"""Run the real indexed-pack parser and strip renderer against hostile assets."""
import os
from pathlib import Path
import shlex
import struct
import subprocess
import tempfile
import unittest
import zlib

ROOT = Path(__file__).resolve().parents[1]
MUSE = ROOT / 'components/muse'


def pack_bytes(edge=64):
    palette = struct.pack('<HHH', 0, 0xF800, 0x07E0)
    indices = bytes((x // 8 + y // 8) % 3 for y in range(edge) for x in range(edge))
    alpha = bytes(0 if x < 4 or y < 4 else 128 if x < 8 else 255
                  for y in range(edge) for x in range(edge))
    payload = palette + indices + alpha
    header = struct.pack('<8sIHHHHI32sII', b'MSPIX01\0', 64 + len(payload), edge, edge,
                         3, 0, 15, bytes(range(32)), zlib.crc32(payload), 0)
    return header[:60] + struct.pack('<I', zlib.crc32(header[:60])) + payload


HARNESS = r'''
#include "muse_pixel_art.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void put32(uint8_t *p,uint32_t v) {for(int i=0;i<4;i++)p[i]=(uint8_t)(v>>(i*8));}
static void repair(uint8_t *p,size_t n) {
    put32(p+56,muse_pixel_pack_crc(p+64,n-64));put32(p+60,muse_pixel_pack_crc(p,60));
}
int main(int argc,char **argv) {
    assert(argc==2);FILE *f=fopen(argv[1],"rb");assert(f);fseek(f,0,SEEK_END);size_t n=(size_t)ftell(f);rewind(f);
    uint8_t *data=malloc(n),*copy=malloc(n+1);assert(data && copy && fread(data,1,n,f)==n);fclose(f);
    muse_pixel_pack_t pack;assert(muse_pixel_pack_parse(data,n,&pack));assert(pack.width==64 && pack.height==64 && pack.colors==3);
    for(size_t size=0;size<n;size++)assert(!muse_pixel_pack_parse(data,size,&pack));
    for(size_t i=0;i<n;i++) {memcpy(copy,data,n);copy[i]^=128;assert(!muse_pixel_pack_parse(copy,n,&pack));}
    memcpy(copy,data,n);copy[n]=0;assert(!muse_pixel_pack_parse(copy,n+1,&pack));
    memcpy(copy,data,n);copy[18]=1;repair(copy,n);assert(!muse_pixel_pack_parse(copy,n,&pack));
    memcpy(copy,data,n);put32(copy+20,31);repair(copy,n);assert(!muse_pixel_pack_parse(copy,n,&pack));
    memcpy(copy,data,n);copy[12]=129;repair(copy,n);assert(!muse_pixel_pack_parse(copy,n,&pack));
    memcpy(copy,data,n);copy[64+6]=3;repair(copy,n);assert(!muse_pixel_pack_parse(copy,n,&pack));
    memcpy(copy,data,n);memset(copy+64+6+4096,0,4096);repair(copy,n);assert(!muse_pixel_pack_parse(copy,n,&pack));
    assert(muse_pixel_pack_parse(data,n,&pack));
    const int edge=320;uint16_t *whole=malloc((edge*edge+2)*sizeof(*whole));
    uint16_t *pieces=malloc((edge*edge+2)*sizeof(*pieces));assert(whole && pieces);
    for(unsigned mode=0;mode<7;mode++) for(unsigned time=0;time<4000;time+=127) {
        muse_pixel_pose_t pose={time,500,200,(uint8_t)mode,true,false};muse_pixel_geometry_t g;
        assert(muse_pixel_geometry(&pack,edge,&pose,&g));assert(g.left>=0 && g.top>=0 && g.left+g.width<=edge && g.top+g.height<=edge);
        whole[0]=whole[edge*edge+1]=0xAAAA;pieces[0]=pieces[edge*edge+1]=0xBBBB;
        assert(muse_pixel_art_blit(&pack,edge,&pose,0xFFFF,whole+1,edge,0,edge-1,0,edge-1));
        for(int y=0;y<edge;y+=16)assert(muse_pixel_art_blit(&pack,edge,&pose,0xFFFF,pieces+1+y*edge,edge,0,edge-1,y,y+15));
        assert(!memcmp(whole+1,pieces+1,edge*edge*sizeof(*whole)));
        assert(whole[0]==0xAAAA && whole[edge*edge+1]==0xAAAA && pieces[0]==0xBBBB && pieces[edge*edge+1]==0xBBBB);
    }
    muse_pixel_pose_t pose={0,0,0,1,false,false};muse_pixel_geometry_t g;assert(muse_pixel_geometry(&pack,edge,&pose,&g));
    assert(muse_pixel_art_blit(&pack,edge,&pose,0xFFFF,whole,edge,0,edge-1,0,edge-1));
    assert(whole[g.top*edge+g.left]==0xFFFF); /* real zero alpha, not keyed black */
    int sx=32,sy=32,x=g.left+sx*g.width/64,y=g.top+sy*g.height/64;
    unsigned source_x=(unsigned)(x-g.left)*64/(unsigned)g.width,source_y=(unsigned)(y-g.top)*64/(unsigned)g.height;
    if(pack.indices[source_y*64+source_x]==0)assert(whole[y*edge+x]==0); /* opaque black details survive */
    assert(!muse_pixel_art_blit(&pack,edge,&pose,0,whole,edge,-1,319,0,319));
    assert(!muse_pixel_art_blit(&pack,edge,&pose,0,whole,319,0,319,0,319));
    pose.mode=7;assert(!muse_pixel_geometry(&pack,edge,&pose,&g));pose.mode=1;pose.level=1001;assert(!muse_pixel_geometry(&pack,edge,&pose,&g));
    free(whole);free(pieces);free(data);free(copy);
    puts("PASS pixel pack integrity, semantic bounds, alpha/black handling, all local states, and strip equivalence");
}
'''


class PixelArt(unittest.TestCase):
    def test_parser_and_local_strip_renderer(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / 'asset.msp').write_bytes(pack_bytes())
            (root / 'test.c').write_text(HARNESS)
            executable = root / 'pixel-test'
            subprocess.run([*shlex.split(os.environ.get('CC', 'cc')), '-std=c11', '-O1', '-g',
                            '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                            '-fno-omit-frame-pointer', '-I', str(MUSE), str(root / 'test.c'),
                            str(MUSE / 'muse_pixel_art.c'), '-o', str(executable)], check=True)
            subprocess.run([str(executable), str(root / 'asset.msp')], check=True, timeout=60,
                           env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0:halt_on_error=1'})


if __name__ == '__main__':
    unittest.main()
