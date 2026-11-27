# Copyright (c) Meta Platforms, Inc. and affiliates.
# SPDX-License-Identifier: Apache-2.0
"""Real matte, alpha blitter, pinned-image lifetimes and cache retry logic."""
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile
import unittest
from test_avatar_context import function

ROOT=Path(__file__).resolve().parents[1]
MUSE=ROOT/'components/muse'

KERNEL=r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "muse_avatar_matte.h"
#include "muse_avatar_render.h"
#include "muse_pixel_art.h"
static uint16_t pixels[256],original[256];static uint8_t alpha[256];static uint32_t queue[256];
static void outlined(void) {
 for(unsigned i=0;i<256;i++)pixels[i]=0xffff;
 for(int y=4;y<12;y++)for(int x=4;x<12;x++)if(x==4 || x==11 || y==4 || y==11)pixels[y*16+x]=0;
 memcpy(original,pixels,sizeof(pixels));
}
int main(void) {
 muse_avatar_matte_stats_t stats;outlined();
 assert(muse_avatar_white_candidate(pixels,16,16));
 assert(muse_avatar_white_mask(pixels,16,16,alpha,queue,256,&stats));assert(stats.applied && stats.removed==192);
 assert(alpha[0]==0 && alpha[6*16+6]==255 && alpha[4*16+4]==255);
 muse_avatar_matte_decontaminate(pixels,alpha,256);
 for(unsigned i=0;i<256;i++)assert(alpha[i]==255?pixels[i]==original[i]:pixels[i]==0);
 for(unsigned i=0;i<256;i++)pixels[i]=0xffff;
 assert(muse_avatar_white_mask(pixels,16,16,alpha,queue,256,&stats) && !stats.applied);
 for(unsigned i=0;i<256;i++)assert(alpha[i]==255);
 outlined();pixels[0]=0x001f;
 assert(!muse_avatar_white_candidate(pixels,16,16));
 assert(muse_avatar_white_mask(pixels,16,16,alpha,queue,256,&stats) && !stats.applied);
 for(unsigned i=0;i<256;i++)assert(alpha[i]==255);
 memset(alpha,0xaa,sizeof(alpha));assert(!muse_avatar_white_mask(pixels,16,16,alpha,queue,255,&stats));
 for(unsigned i=0;i<256;i++)assert(alpha[i]==0xaa);
 assert(!muse_avatar_white_mask(pixels,0,16,alpha,queue,256,&stats));
 assert(!muse_avatar_white_candidate(pixels,467,16));assert(!muse_avatar_white_candidate(NULL,16,16));
 for(unsigned i=0;i<256;i++)pixels[i]=0xffff;
 pixels[6*16+6]=0;pixels[6*16+5]=(28u<<11)|(57u<<5)|28u;
 assert(muse_avatar_white_mask(pixels,16,16,alpha,queue,256,&stats) && stats.applied);
 assert(alpha[6*16+5]==24 && alpha[6*16+6]==255 && stats.softened==1);
 muse_avatar_matte_decontaminate(pixels,alpha,256);assert(pixels[6*16+5]==0);
 uint16_t *one=calloc(128*128,2),*two=calloc(128*128,2),*strip=calloc(128*7,2);assert(one && two && strip);
 for(unsigned i=0;i<256;i++) {pixels[i]=(uint16_t)(i*251u);alpha[i]=(uint8_t)(i*17u);}
 muse_avatar_pixels_t image={pixels,16,16};
 for(int filter=0;filter<3;filter++)for(int canvas=32;canvas<=128;canvas*=2) {
  muse_avatar_transform_t t={950,970,3,(muse_avatar_filter_t)filter};
  assert(muse_avatar_blit_alpha_transform(&image,alpha,canvas,&t,0x1234,one,canvas,0,canvas-1,0,canvas-1));
  for(int y=0;y<canvas;y+=7) {int last=y+6<canvas?y+6:canvas-1;
   assert(muse_avatar_blit_alpha_transform(&image,alpha,canvas,&t,0x1234,strip,canvas,0,canvas-1,y,last));
   for(int r=0;r<=last-y;r++)assert(!memcmp(strip+r*canvas,one+(y+r)*canvas,(size_t)canvas*2));
  }
  assert(muse_avatar_blit_transform(&image,canvas,&t,0x1234,one,canvas,0,canvas-1,0,canvas-1));
  assert(muse_avatar_blit_alpha_transform(&image,NULL,canvas,&t,0x1234,two,canvas,0,canvas-1,0,canvas-1));
  assert(!memcmp(one,two,(size_t)canvas*canvas*2));
 }
 uint16_t values[4]={0xffff,0xf800,0,0xffff},out[4];uint8_t a[4]={0,128,255,255};
 muse_avatar_pixels_t tiny={values,2,2};muse_avatar_transform_t t={1000,1000,0,MUSE_AVATAR_NEAREST};
 assert(muse_avatar_blit_alpha_transform(&tiny,a,2,&t,0x001f,out,2,0,1,0,1));
 assert(out[0]==0x001f && out[1]==0x800f && out[2]==0 && out[3]==0xffff);
 assert(!muse_avatar_blit_alpha_transform(&tiny,a,2,&t,0,out,1,0,1,0,1));
 assert(muse_pixel_pack_crc("123456789",9)==0xcbf43926u);
 assert(muse_pixel_crc_append(muse_pixel_pack_crc("ab",2),"cdef",4)==muse_pixel_pack_crc("abcdef",6));
 assert(muse_pixel_crc_append(42,NULL,0)==42);free(one);free(two);free(strip);
 puts("matte: enclosed whites, skip gates, soft edge, alpha/opaque strips and CRC passed");
}
'''

LIFETIME=r'''
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "muse_avatar.h"
#include "muse_avatar_matte.h"
#include "muse_pixel_art.h"
#define portENTER_CRITICAL(p) ((void)0)
#define portEXIT_CRITICAL(p) ((void)0)
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define CACHE_A "synthetic-a"
#define CACHE_B "synthetic-b"
#define mkdir(path,mode) ((void)0)
static atomic_uint s_source=MUSE_AVATAR_PERSONAL,s_generation=1,s_content_epoch=1,s_metadata_epoch,s_cache_writes;
static atomic_bool s_binding_blocked;
static bool s_image_saved;static unsigned s_revision,s_cache_sequence;static int s_cache_slot=-1;
static uint8_t s_binding[32];
static unsigned calls,fail_at,invalidate_at,live,objects,frame_releases;
static bool fail_object;
static void motion_binding(const void *p) {(void)p;}
muse_avatar_source_t muse_avatar_source(void) {return (muse_avatar_source_t)atomic_load(&s_source);}
static void *heap_caps_calloc(size_t n,size_t size,unsigned caps) {(void)caps;if(fail_object){fail_object=false;return NULL;}objects++;return calloc(n,size);}
static void heap_caps_free(void *p) {if(p){assert(objects);objects--;free(p);}}
static void *request_alloc(size_t n) {return heap_caps_calloc(1,n,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);}
#define AVATAR_WORKSPACE (16u*1024u)
static size_t allocation_budget(size_t pending) {(void)pending;return 4u*1024u*1024u;}
static void *muse_webp_malloc(size_t n) {
 calls++;if(calls==fail_at)return NULL;
 if(calls==invalidate_at){atomic_fetch_add(&s_generation,1);atomic_fetch_add(&s_content_epoch,1);atomic_store(&s_binding_blocked,true);}
 size_t *p=malloc(sizeof(size_t)+n);assert(p);*p=0xabcdef;live++;return p+1;
}
static void muse_webp_free(void *data) {if(data){size_t *p=(size_t *)data-1;assert(*p==0xabcdef && live);live--;free(p);}}
static uint32_t avatar_cache_crc(const void *p,size_t n) {return muse_pixel_pack_crc(p,n);}
static bool busy(void) {return false;}
bool muse_avatar_files_mount(void) {return true;}
typedef struct {unsigned sequence;} avatar_cache_header_t;
typedef struct {char path[16];} avatar_identity_t;
static const uint16_t *expected_raw;static unsigned writes;
static void avatar_cache_header_init(avatar_cache_header_t *h,unsigned sequence,int w,int height,const uint8_t *binding,const avatar_identity_t *id,const void *p) {
 (void)binding;(void)id;assert(w==16 && height==16 && p==expected_raw && expected_raw[0]==0xffff);h->sequence=sequence;
}
static bool avatar_cache_write_while(const char *path,const avatar_cache_header_t *h,const void *p,bool (*proceed)(void *),void *ctx) {
 assert(!strcmp(path,CACHE_A) || !strcmp(path,CACHE_B));assert(h->sequence && p==expected_raw && proceed(ctx));writes++;return true;
}
static void cached_identity(const avatar_identity_t *id) {(void)id;}
'''
LIFE_MAIN=r'''
static image_t *fresh(void) {
 uint16_t *p=muse_webp_malloc(512);assert(p);
 for(unsigned i=0;i<256;i++)p[i]=0xffff;
 for(int y=4;y<12;y++)for(int x=4;x<12;x++)if(x==4 || x==11 || y==4 || y==11)p[y*16+x]=0;
 return new_image(p,16,16,atomic_load(&s_content_epoch));
}
static void frame_release(const muse_avatar_image_t *p) {(void)p;frame_releases++;}
int main(void) {
 image_t *image=fresh();assert(image && image->alpha && !image->matte_pending);
 assert(image->raw_pixels[0]==0xffff && image->view.data.pixels[0]==0 && image->alpha[6*16+6]==255);
 expected_raw=image->raw_pixels;avatar_identity_t id={{0}};
 assert(cache_save(image,&id,atomic_load(&s_generation)) && writes==1 && atomic_load(&s_cache_writes)==1);
 publish(image);const muse_avatar_image_t *held=muse_avatar_acquire();assert(held && muse_avatar_image_alpha(held)[0]==0);
 publish(fresh());assert(muse_avatar_image_alpha(held)[6*16+6]==255);muse_avatar_release(held);publish(NULL);assert(!live && !objects);
 fail_object=true;assert(!fresh() && !live && !objects);
 for(unsigned failure=1;failure<=3;failure++) {
  fail_at=calls+1+failure;image=fresh();assert(image && image->matte_pending && !image->alpha && image->view.data.pixels==image->raw_pixels);
  image->pixel_key_valid=true;memset(image->pixel_key,0x23,32);publish(image);fail_at=0;
  assert(retry_matte(atomic_load(&s_generation)) && s_image->alpha && !s_image->matte_pending);
  assert(s_image->pixel_key_valid && s_image->pixel_key[0]==0x23 && s_image->raw_pixels[0]==0xffff);
  publish(NULL);assert(!live && !objects);
 }
 for(unsigned failure=1;failure<=4;failure++) {
  fail_at=calls+2;image=fresh();assert(image && image->matte_pending);publish(image);
  fail_at=calls+failure;assert(!retry_matte(atomic_load(&s_generation)) && s_image==image && image->matte_pending);
  fail_at=0;assert(retry_matte(atomic_load(&s_generation)));publish(NULL);assert(!live && !objects);
 }
 fail_at=calls+2;image=fresh();assert(image && image->matte_pending);publish(image);fail_at=0;
 held=muse_avatar_acquire();assert(held);unsigned generation=atomic_load(&s_generation);invalidate_at=calls+1;
 assert(!retry_matte(generation) && s_image==image && !muse_avatar_acquire());invalidate_at=0;
 assert(held->data.pixels[0]==0xffff);muse_avatar_release(held);publish(NULL);assert(!live && !objects);
 atomic_store(&s_binding_blocked,false);
 uint16_t frame_pixels[4]={0};muse_avatar_image_t frame={.data={frame_pixels,2,2},.release=frame_release};
 assert(!muse_avatar_image_alpha(&frame));muse_avatar_release(&frame);assert(frame_releases==1);
 puts("matte lifecycle: raw cache, refcounts, allocation recovery, revocation and opaque clips passed");
}
'''


class AvatarMatte(unittest.TestCase):
    def compile_run(self,code,sources=(),defines=''):
        with tempfile.TemporaryDirectory() as tmp:
            stage=Path(tmp);(stage/'sdkconfig.h').write_text(defines);(stage/'test.c').write_text(code)
            exe=stage/'test'
            subprocess.run([*shlex.split(os.environ.get('CC','cc')),'-std=c11','-O1','-g','-Wall','-Wextra','-Werror',
                            '-fsanitize=address,undefined','-I',str(stage),'-I',str(MUSE),
                            str(stage/'test.c'),*[str(p) for p in sources],'-o',str(exe)],check=True)
            subprocess.run([str(exe)],check=True,timeout=30)

    def test_kernel_alpha_renderer_and_incremental_crc(self):
        self.compile_run(KERNEL,[MUSE/'muse_avatar_matte.c',MUSE/'muse_avatar_render.c',MUSE/'muse_pixel_art.c'])

    def test_actual_image_lifetime_cache_and_retries(self):
        source=(ROOT/'main/avatar_sync.c').read_text()
        image_type=re.search(r'typedef struct \{ muse_avatar_image_t view;[^\n]+\} image_t;',source).group()
        names=('const muse_avatar_image_t *muse_avatar_acquire(', 'void muse_avatar_release(',
               'static void publish(', 'const uint8_t *muse_avatar_image_alpha(', 'static image_t *new_image(',
               'static bool cancelled(', 'static bool cache_proceed(', 'static bool cache_save(', 'static bool retry_matte(')
        body='\n'.join(function(source,name) for name in names)
        self.compile_run(LIFETIME+image_type+'\nstatic image_t *s_image;\n'+body+LIFE_MAIN,
                         [MUSE/'muse_avatar_matte.c',MUSE/'muse_pixel_art.c'],
                         '#define CONFIG_MUSE_AVATAR_WHITE_MATTE 1\n#define CONFIG_MUSE_PIXEL_AVATAR 1\n#define CONFIG_MUSE_AVATAR_ANIMATIONS 1\n')

    def test_actual_cache_load_backoff_is_not_marked_done_after_failure(self):
        source=(ROOT/'main/avatar_sync.c').read_text()
        start='if (s_have_binding && !loaded && !busy() && esp_timer_get_time()>=next_cache)'
        block=function(source,start)
        code=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
static bool s_have_binding=true,loaded,working;static unsigned attempts,cache_retry=1,failures=2;
static int64_t clock_us,next_cache;
static bool busy(void) {return working;}
static int64_t esp_timer_get_time(void) {return clock_us;}
static bool cache_load(void) {attempts++;return attempts>failures;}
static void step(void) {
'''+block+r'''
}
int main(void) {
 step();assert(attempts==1 && !loaded && next_cache==1000000);
 clock_us=500000;step();assert(attempts==1);
 clock_us=1000000;step();assert(attempts==2 && !loaded && next_cache==3000000);
 clock_us=3000000;step();assert(attempts==3 && loaded);
 step();assert(attempts==3);
 loaded=false;attempts=0;failures=1000;next_cache=clock_us=0;cache_retry=1;
 for(int i=0;i<20;i++){step();assert(!loaded && cache_retry<=60 && next_cache-clock_us<=60000000);clock_us=next_cache;}
 unsigned before=attempts;working=true;step();assert(attempts==before);working=false;s_have_binding=false;step();assert(attempts==before);
}
'''
        self.compile_run(code)


if __name__=='__main__':
    unittest.main()
