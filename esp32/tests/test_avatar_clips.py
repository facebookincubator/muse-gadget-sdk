# Copyright (c) Meta Platforms, Inc. and affiliates.
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at http://www.apache.org/licenses/LICENSE-2.0
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
"""Validate the production clip reader/converter with hostile packs and real H.264."""
import importlib.util
import base64
import hashlib
import json
import sys
import types
from unittest import mock
import os
from pathlib import Path
import shlex
import shutil
import struct
import subprocess
import tempfile
import unittest
import zlib
ROOT=Path(__file__).resolve().parents[1]
MUSE=ROOT/'components/muse'
spec=importlib.util.spec_from_file_location('avatar_clips',ROOT/'tools/muse/avatar_clips.py')
clips=importlib.util.module_from_spec(spec);spec.loader.exec_module(clips)

HARNESS=r'''
#include "muse_avatar_pack.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void put32(unsigned char *p,uint32_t n) {for(int i=0;i<4;i++)p[i]=(unsigned char)(n>>(8*i));}
static void repair(unsigned char *p,size_t n) {
    put32(p+24,muse_avatar_pack_crc(p+64,n-64));put32(p+60,muse_avatar_pack_crc(p,60));
}
int main(int argc,char **argv) {
    assert(argc==2);FILE *f=fopen(argv[1],"rb");assert(f);fseek(f,0,SEEK_END);long length=ftell(f);rewind(f);
    assert(length>64 && length<MUSE_AVATAR_PACK_MAX);size_t n=(size_t)length;
    unsigned char *p=malloc(n),*copy=malloc(n);assert(p&&copy&&fread(p,1,n,f)==n);fclose(f);
    unsigned char key[32]={0};muse_avatar_pack_t pack;
    assert(muse_avatar_pack_parse(p,n,key,&pack));assert(pack.coverage==7 && pack.width==240 && pack.fps==16);
    for(unsigned k=0;k<3;k++) {uint32_t cursor=0,len,count=0;const uint8_t *data;
        while(muse_avatar_pack_frame(&pack,k,&cursor,&data,&len)) {assert(len>=6 && data>=p && data+len<=p+n);count++;}
        assert(count==pack.clip[k].frames);
    }
    for(size_t i=0;i<n;i++)assert(!muse_avatar_pack_parse(p,i,key,&pack));
    key[0]=1;assert(!muse_avatar_pack_parse(p,n,key,&pack));key[0]=0;
    for(size_t i=0;i<n;i++) {memcpy(copy,p,n);copy[i]^=0x80;assert(!muse_avatar_pack_parse(copy,n,key,&pack));}
    /* Repaired outer CRCs force malformed clip/frame lengths past the first checks. */
    uint32_t values[]={0,1,0x7fffffff,0xffffffff};
    for(unsigned i=0;i<4;i++) for(unsigned pos=68;pos<=84;pos+=4) {
        memcpy(copy,p,n);put32(copy+pos,values[i]);repair(copy,n);assert(!muse_avatar_pack_parse(copy,n,key,&pack));
    }
    memcpy(copy,p,n);copy[12]=239;repair(copy,n);assert(!muse_avatar_pack_parse(copy,n,key,&pack));
    memcpy(copy,p,n);copy[16]=25;repair(copy,n);assert(!muse_avatar_pack_parse(copy,n,key,&pack));
    free(copy);free(p);puts("PASS clip pack bounds, CRCs, frame walk, truncation, source binding");
}
'''
DECODE=r'''
#include "muse_avatar_pack.h"
#include "muse_vdec.h"
#include "muse_vstream.h"
#include <string.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
int main(int argc,char **argv) {
    assert(argc==2);FILE *f=fopen(argv[1],"rb");assert(f);fseek(f,0,SEEK_END);size_t n=(size_t)ftell(f);rewind(f);
    unsigned char *p=malloc(n);assert(p&&fread(p,1,n,f)==n);fclose(f);unsigned char key[32]={0};muse_avatar_pack_t pack;
    assert(muse_avatar_pack_parse(p,n,key,&pack));unsigned total=0;
    for(unsigned k=0;k<MUSE_AVATAR_CLIPS;k++) if(pack.coverage&(1u<<k)) {
        for(int loop=0;loop<2;loop++) {
            muse_vdec_t *d=muse_vdec_open(MUSE_VIDEO_CODEC_H264,pack.width,pack.height);assert(d);muse_vdec_limit_refs(d,1);
            const uint8_t *data;uint32_t cursor=0,len,count=0;
            while(muse_avatar_pack_frame(&pack,k,&cursor,&data,&len)) {
                uint8_t *scratch=malloc(len);assert(scratch);memcpy(scratch,data,len);
                muse_pic_t pic;int rc=muse_vdec_decode(d,scratch,len,&pic);free(scratch);
                if(rc!=MUSE_VDEC_PICTURE) fprintf(stderr,"clip %u loop %d frame %u rc %d\n",k,loop,count,rc);
                assert(rc==MUSE_VDEC_PICTURE);
                assert(pic.w==pack.width && pic.h==pack.height && pic.y && pic.u && pic.v);
                count++;
            }
            assert(count==pack.clip[k].frames);total+=count;muse_vdec_close(d);
        }
    }
    free(p);printf("PASS actual tinyh264 decoded %u frames including loop resets\n",total);
}
'''

class AvatarClips(unittest.TestCase):
    def test_console_reply_after_log_prefix(self):
        class Board:
            def read_line(self,deadline):
                return 'log tail @avatar_install ok'
        self.assertEqual(clips.receive(Board(),'@avatar_install '),'ok')
    def test_install_rejects_stale_source_before_upload(self):
        commands=[]
        class Board:
            def __init__(self,port):pass
            def __enter__(self):return self
            def __exit__(self,*args):pass
            def status(self):return {'board':'fake'}
            def write(self,data):pass
            def write_line(self,command):commands.append(command)
            def read_line(self,deadline):return '@avatar_assets '+json.dumps({'source':'ff'*32})
        with tempfile.TemporaryDirectory() as tmp,mock.patch.dict(sys.modules,{'chat':types.SimpleNamespace(Board=Board)}):
            path=Path(tmp)/'old.mav';path.write_bytes(bytes(64))
            with self.assertRaisesRegex(ValueError,'Avatar changed'):
                clips.install('fake',path)
        self.assertEqual(commands,['avatar.assets'])
    def compile(self,tmp,source,decoder=False):
        src=Path(tmp)/'harness.c';src.write_text(source);exe=Path(tmp)/'harness'
        command=[*shlex.split(os.environ.get('CC','cc')),'-std=c11','-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer',
            '-I',str(MUSE),'-I',str(ROOT/'components/muse_vstream/include'),'-I',str(ROOT/'components/tinyh264/src')]
        if decoder:
            (Path(tmp)/'sdkconfig.h').write_text('#define CONFIG_MUSE_VIDEO_VP8 0\n')
            command+=['-I',tmp,'-w','-fno-sanitize=shift-base',str(MUSE/'muse_vdec.c')]+[str(p) for p in sorted((ROOT/'components/tinyh264/src').glob('*.c'))]
        else:command+=['-Wall','-Wextra','-Werror']
        subprocess.run(command+[str(src),str(MUSE/'muse_avatar_pack.c'),'-o',str(exe)],check=True,capture_output=True)
        return exe
    def run_harness(self,exe,path):
        result=subprocess.run([str(exe),str(path)],capture_output=True,text=True,timeout=60,
            env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0:halt_on_error=1'})
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
    def test_pack_bounds_and_corruption(self):
        with tempfile.TemporaryDirectory() as tmp:
            unit=b'\x00\x00\x00\x01\x09\x10\x00\x00\x01\x65abc'
            data=clips.make_pack(bytes(32),{k:[unit,unit] for k in ('idle','waking','working')})
            path=Path(tmp)/'test.mav';path.write_bytes(data)
            self.run_harness(self.compile(tmp,HARNESS),path)
    def test_real_conversion_and_decoder(self):
        if not shutil.which('ffmpeg'):self.skipTest('ffmpeg required for actual conversion integration')
        with tempfile.TemporaryDirectory() as tmp:
            src=Path(tmp)/'input.mp4'
            subprocess.run(['ffmpeg','-nostdin','-v','error','-f','lavfi','-i','testsrc2=size=720x720:rate=24',
                '-t','1','-an','-c:v','libx264','-profile:v','high','-pix_fmt','yuv420p',str(src)],check=True,capture_output=True)
            units=clips.convert_clip(src)
            self.assertEqual(len(units),16)
            path=Path(tmp)/'real.mav';path.write_bytes(clips.make_pack(bytes(32),{'idle':units,'working':units}))
            self.run_harness(self.compile(tmp,DECODE,True),path)
    def test_usb_export_busy_retry_and_exact_range_eof(self):
        data=bytes(range(256))*32
        class Board:
            def __init__(self,port):self.lines=[];self.busy=True
            def __enter__(self):return self
            def __exit__(self,*args):pass
            def write(self,data):pass
            def status(self):return {'device':{'link':{'paired':True,'state':'Online'}}}
            def write_line(self,command):
                if command=='avatar.assets':
                    self.lines.append('@avatar_assets '+json.dumps({'source':'00'*32,'coverage':1}))
                    return
                offset=int(command.split(',')[1])
                if self.busy:
                    self.busy=False;self.lines.append('@avatar_export_error busy');return
                chunk=data[offset:offset+4096]
                self.lines.append('@avatar_asset '+json.dumps({'kind':0,'offset':offset,'status':206 if chunk else 416,'len':len(chunk),'crc':zlib.crc32(chunk)}))
                self.lines.extend('@avatar_bytes '+base64.b64encode(chunk[i:i+360]).decode() for i in range(0,len(chunk),360))
                self.lines.append('@avatar_end')
            def read_line(self,end):
                if not self.lines:raise AssertionError('Export requested more input than the transcript supplies')
                return self.lines.pop(0)
        with tempfile.TemporaryDirectory() as tmp, mock.patch.dict(sys.modules,{'chat':types.SimpleNamespace(Board=Board)}), mock.patch.object(clips.time,'sleep'):
            clips.export_clips('fake',tmp,['idle'])
            self.assertEqual((Path(tmp)/'idle.mp4').read_bytes(),data)
            manifest=json.loads((Path(tmp)/'manifest.json').read_text())
            self.assertEqual(manifest['files']['idle']['sha256'],hashlib.sha256(data).hexdigest())
            self.assertEqual((Path(tmp)/'idle.mp4').stat().st_mode & 0o777,0o600)
    def test_conversion_rejects_mixed_export_generation(self):
        with tempfile.TemporaryDirectory() as tmp:
            directory=Path(tmp)
            (directory/'idle.mp4').write_bytes(b'new avatar')
            (directory/'manifest.json').write_text(json.dumps({'source':'00'*32,'files':{'idle':{'bytes':10,'sha256':hashlib.sha256(b'old avatar').hexdigest()}}}))
            with self.assertRaisesRegex(ValueError,'manifest'):
                clips.convert(directory,directory/'output.mav',['idle'],240,16,'ffmpeg')

    def test_bounded_identity_variant_fields(self):
        cjson=ROOT/'managed_components/espressif__cjson/cJSON'
        if not (cjson/'cJSON.c').exists():self.skipTest('Configure IDF to fetch pinned cJSON')
        harness=r"""
#include "avatar_clip_refs.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void) {
    avatar_clip_refs_t refs;
    const char *good="{\"ok\":true,\"result\":{\"avatar\":{\"image_exists\":true,\"image_path\":\"workspace/a.webp\",\"assets_revision\":\"r1\",\"video_variants\":{\"idle\":\"workspace/idle.mp4\",\"working\":\"workspace/work.mp4\",\"secret\":\"ignored\"}}}}";
    assert(avatar_clip_refs_parse(good,strlen(good),&refs));
    assert(!strcmp(refs.path[MUSE_CLIP_IDLE],"workspace/idle.mp4"));
    assert(!refs.path[MUSE_CLIP_WAKING][0]);assert(!strcmp(refs.path[MUSE_CLIP_WORKING],"workspace/work.mp4"));
    const char *missing="{\"ok\":true,\"result\":{\"avatar\":{\"image_exists\":true,\"image_path\":\"a.webp\",\"video_variants\":{\"idle\":42}}}}";
    assert(!avatar_clip_refs_parse(missing,strlen(missing),&refs));
    char large[1024],path[514];memset(path,'a',512);path[512]=0;
    snprintf(large,sizeof(large),"{\"ok\":true,\"result\":{\"avatar\":{\"image_exists\":true,\"image_path\":\"a.webp\",\"video_variants\":{\"idle\":\"%s\"}}}}",path);
    assert(!avatar_clip_refs_parse(large,strlen(large),&refs));
    assert(!avatar_clip_refs_parse("{}[]",4,&refs));
    puts("PASS bounded identity animation references and missing variants");
}
"""
        with tempfile.TemporaryDirectory() as tmp:
            src=Path(tmp)/'refs.c';src.write_text(harness);exe=Path(tmp)/'refs'
            subprocess.run([*shlex.split(os.environ.get('CC','cc')),'-std=c11','-Wall','-Wextra','-Werror','-Wno-deprecated-declarations','-fsanitize=address,undefined',
                '-I',str(ROOT/'main'),'-I',str(MUSE),'-I',str(cjson),str(src),str(ROOT/'main/avatar_clip_refs.c'),
                str(ROOT/'main/avatar_identity.c'),str(cjson/'cJSON.c'),'-o',str(exe)],check=True)
            result=subprocess.run([str(exe)],capture_output=True,text=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0:halt_on_error=1'})
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)

    def test_reference_limit_rejects_before_picture_allocation(self):
        from test_video_untrusted import sps,pps,idr
        harness=r"""
#include "muse_vdec.h"
#include "muse_vstream.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc,char **argv) {
    assert(argc==2);FILE *f=fopen(argv[1],"rb");assert(f);fseek(f,0,SEEK_END);size_t n=(size_t)ftell(f);rewind(f);
    uint8_t *data=malloc(n),*scratch=malloc(n);assert(data&&scratch&&fread(data,1,n,f)==n);fclose(f);
    for(int limit=0;limit<=1;limit++) {
        muse_vdec_t *d=muse_vdec_open(MUSE_VIDEO_CODEC_H264,32,32);assert(d);muse_vdec_limit_refs(d,(unsigned)limit);
        memcpy(scratch,data,n);muse_pic_t pic;int result=muse_vdec_decode(d,scratch,(uint32_t)n,&pic);
        assert(limit?result!=MUSE_VDEC_PICTURE:result==MUSE_VDEC_PICTURE);muse_vdec_close(d);
    }
    free(data);free(scratch);puts("PASS opt-in reference bound; default decoder still accepts the stream");
}
"""
        with tempfile.TemporaryDirectory() as tmp:
            path=Path(tmp)/'two-refs.h264';path.write_bytes(sps(2,2,num_ref_frames=2)+pps()+idr(4))
            self.run_harness(self.compile(tmp,harness,True),path)

    def test_converter_rejects_invalid_bounds(self):
        for edge,fps in ((241,16),(240,25),(0,16),(240,0)):
            with self.assertRaises(ValueError):clips.make_pack(bytes(32),{'idle':[b'123456']},edge,fps)
        with self.assertRaises(ValueError):clips.make_pack(bytes(31),{'idle':[b'123456']})
        with self.assertRaises(ValueError):clips.access_units(b'not h264')
        with self.assertRaises(ValueError):clips.make_pack(bytes(32),{'idle':[b'a'*32769]})

if __name__=='__main__':unittest.main()
