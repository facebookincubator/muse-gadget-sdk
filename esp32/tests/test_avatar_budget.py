# Copyright (c) Meta Platforms, Inc. and affiliates.
# SPDX-License-Identifier: Apache-2.0
"""Executable avatar refresh/cache/matte admission under a counted PSRAM heap.

Owning sync functions are compiled from production, together with the real
bounded libwebp decoder/allocator, request ownership, cache format and matte.
Only transport, identity metadata, scheduler and the ESP heap are synthetic.
No device, network request, private asset, or production files volume is used.
"""
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

from PIL import Image, ImageDraw
from test_avatar_context import function
from pixel_test_sanitizers import ASAN_OPTIONS

ROOT = Path(__file__).resolve().parents[1]

HEAP_H = r'''
#pragma once
#include <stddef.h>
#define MALLOC_CAP_SPIRAM 1u
#define MALLOC_CAP_8BIT 2u
#define MALLOC_CAP_INTERNAL 4u
void *heap_caps_malloc(size_t n, unsigned caps);
void *heap_caps_calloc(size_t n, size_t bytes, unsigned caps);
void heap_caps_free(void *p);
size_t heap_caps_get_free_size(unsigned caps);
'''

PRELUDE = r'''
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "muse_avatar.h"
#include "muse_avatar_matte.h"
#include "avatar_webp.h"
#include "avatar_request.h"
#include "avatar_identity.h"
#include "avatar_cache.h"
#define portENTER_CRITICAL(p) ((void)0)
#define portEXIT_CRITICAL(p) ((void)0)
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define pdMS_TO_TICKS(n) (n)
/* Cache filenames are private temporary fixtures. Never create /files. */
static int test_mkdir(const char *path,unsigned mode) {assert(!strcmp(path,"/files/.muse-avatar-v1") && mode==0700);return 0;}
#define mkdir(path,mode) test_mkdir(path,mode)
#define MUSE_MODE_LISTENING 1
#define MUSE_MODE_SPEAKING 2
#define MUSE_MODE_THINKING 3
typedef int muse_mode_t;
#define MIB (1024u*1024u)
static atomic_uint s_source=MUSE_AVATAR_PERSONAL,s_generation=1,s_content_epoch=1,s_metadata_epoch;
static atomic_bool s_binding_blocked;
static atomic_size_t s_last_peak;
static atomic_int s_last_ms;
static atomic_uint s_file_reads,s_identity_reads,s_bytes_received,s_cache_writes,s_cache_loads;
static atomic_uint s_path_id,s_revision_id,s_cached_path_id,s_cached_revision_id;
static _Atomic(const char *) s_status="test";
static unsigned s_revision,s_cache_sequence;
static int s_cache_slot=-1;
static bool s_image_saved;
static uint8_t s_binding[32];
static avatar_identity_t s_identity;
static avatar_identity_t next_identity={"synthetic/avatar.webp","B"};
static uint8_t *fixture;
static size_t fixture_len;
static size_t heap_total=8u*MIB,heap_used,heap_other,heap_peak;
static unsigned heap_blocks,alloc_calls;
static size_t fail_payload,owner_bytes;
static bool fail_encoded,fail_queue_after_owner,post_pressure;
static const muse_avatar_image_t *release_on_free;
typedef union { max_align_t align; size_t bytes; } heap_header_t;
typedef union { max_align_t align; size_t bytes; } webp_header_t;
void *heap_caps_malloc(size_t n,unsigned caps) {
    assert(caps==(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));alloc_calls++;
    if(fail_encoded && n==fixture_len) {fail_encoded=false;return NULL;}
    if(fail_payload && n==fail_payload+sizeof(webp_header_t)) {fail_payload=0;return NULL;}
    if(n>heap_total-heap_used-heap_other)return NULL;
    heap_header_t *h=malloc(sizeof(*h)+n);assert(h);h->bytes=n;
    heap_used+=n;heap_blocks++;if(heap_used>heap_peak)heap_peak=heap_used;
    if(fail_queue_after_owner && n==owner_bytes) {fail_queue_after_owner=false;fail_payload=1024;}
    if(post_pressure) {post_pressure=false;heap_other=heap_total-heap_used-AVATAR_WEBP_RESERVE_BYTES+1;}
    return h+1;
}
void *heap_caps_calloc(size_t n,size_t bytes,unsigned caps) {
    if(bytes && n>SIZE_MAX/bytes)return NULL;
    void *p=heap_caps_malloc(n*bytes,caps);if(p)memset(p,0,n*bytes);return p;
}
void heap_caps_free(void *p) {
    if (!p) {
        return;
    }
    heap_header_t *h=(heap_header_t *)p-1;
    assert(heap_used>=h->bytes && heap_blocks);heap_used-=h->bytes;heap_blocks--;free(h);
}
size_t heap_caps_get_free_size(unsigned caps) {
    if(caps&(MALLOC_CAP_INTERNAL))return 64u*1024u;
    assert(caps==MALLOC_CAP_SPIRAM);
    size_t free=heap_total-heap_used-heap_other;
    if(release_on_free) {const muse_avatar_image_t *held=release_on_free;release_on_free=NULL;muse_avatar_release(held);}
    return free;
}
static void free_psram(size_t n) {assert(n<=heap_total-heap_used);heap_other=heap_total-heap_used-n;}
static muse_mode_t muse_state_mode(void *p) {(void)p;return 0;}
static bool muse_voice_in_call(void) {return false;}
static bool image_fetch_busy(void) {return false;}
static bool noise_tx_has_dma_headroom(void *p) {(void)p;return true;}
static bool noise_tx_has_contiguous_dma_headroom(void) {return true;}
static bool muse_state_asleep(void) {return false;}
static int64_t esp_timer_get_time(void) {return 1;}
static void vTaskDelay(unsigned ms) {(void)ms;}
static void motion_binding(const avatar_identity_t *p) {(void)p;}
static char *vm_file_request_path(const char *p) {size_t n=strlen(p)+1;char *s=malloc(n);assert(s);memcpy(s,p,n);return s;}
static size_t noise_ctrl_req_max_body(void) {return 4096;}
static int64_t noise_ctrl_req_open(const char *method,const char *path,const char *const *headers,bool quiet,avatar_request_cb cb,void *ctx) {
    assert(!strcmp(method,"GET") && quiet);
    if(!strcmp(path,"/identity"))cb(ctx,200,(const uint8_t *)"identity",8,true);
    else {
        unsigned offset,end;assert(headers && !strcmp(headers[0],"Range"));
        assert(sscanf(headers[1],"bytes=%u-%u",&offset,&end)==2 && offset<fixture_len);
        size_t n=fixture_len-offset;if(n>(size_t)end-offset+1)n=(size_t)end-offset+1;
        cb(ctx,206,fixture+offset,n,true);
    }
    return 1;
}
static void noise_ctrl_req_cancel(int64_t id) {(void)id;assert(!"completed synthetic request was cancelled");}
int avatar_identity_parse(const char *json,size_t n,avatar_identity_t *out) {assert(n==8 && !memcmp(json,"identity",8));*out=next_identity;return 1;}
bool muse_avatar_files_mount(void) {return true;}
static void cached_identity(const avatar_identity_t *id) {(void)id;}
'''

MAIN = r'''
static uint16_t *raw(void) {
    uint16_t *p=muse_webp_malloc(512);assert(p);
    for(unsigned i=0;i<256;i++)p[i]=0xffff;
    for(int y=4;y<12;y++)for(int x=4;x<12;x++)if(x==4 || x==11 || y==4 || y==11)p[y*16+x]=0;
    return p;
}
static image_t *seed(void) {
    avatar_webp_set_budget(AVATAR_PEAK);
    image_t *image=new_image(raw(),16,16,1);assert(image);publish(image);
    s_identity=(avatar_identity_t){"synthetic/avatar.webp","A"};
    assert(cache_save(image,&s_identity,1));s_image_saved=true;return image;
}
static void disk_matches(const uint16_t *raw) {
    avatar_cache_header_t h;uint16_t bytes[256];
    assert(avatar_cache_read(s_cache_slot?CACHE_B:CACHE_A,s_binding,&h,bytes,sizeof(bytes)));
    assert(h.bytes==sizeof(bytes) && !memcmp(bytes,raw,sizeof(bytes)));
}
int main(int argc,char **argv) {
    assert(argc==3);owner_bytes=sizeof(image_t);
    FILE *f=fopen(argv[2],"rb");assert(f);assert(!fseek(f,0,SEEK_END));long n=ftell(f);rewind(f);
    assert(n>12);fixture_len=(size_t)n;fixture=malloc(fixture_len);assert(fixture);
    assert(fread(fixture,1,fixture_len,f)==fixture_len);fclose(f);
    const char *which=argv[1];
    if(!strcmp(which,"three_mib")) {
        free_psram(3u*MIB);assert(sync_once(1));
        assert(s_image && s_image->view.data.width==16 && s_image->view.data.height==16);
#if CONFIG_MUSE_AVATAR_WHITE_MATTE
        assert(s_image->alpha && !s_image->matte_pending && s_image->raw_pixels[0]==0xffff && s_image->alpha[0]==0);
#endif
        assert(atomic_load(&s_bytes_received)==fixture_len && heap_caps_get_free_size(MALLOC_CAP_SPIRAM)>=AVATAR_RESERVE);
        assert(atomic_load(&s_last_peak)<=AVATAR_PEAK);disk_matches(s_image->raw_pixels);
    } else if(!strcmp(which,"reserve")) {
        image_t *old=seed();const muse_avatar_image_t *held=muse_avatar_acquire();assert(held);
        free_psram(AVATAR_RESERVE+AVATAR_WORKSPACE);
        size_t live=avatar_webp_live_bytes();assert(!sync_once(1) && s_image==old && avatar_webp_live_bytes()==live);
        assert(!atomic_load(&s_bytes_received));disk_matches(old->raw_pixels);
        free_psram(AVATAR_RESERVE);assert(!allocation_budget(0));assert(!muse_webp_malloc(1));
        muse_avatar_release(held);
    } else if(!strcmp(which,"held_cap")) {
        image_t *old=seed();const muse_avatar_image_t *held=muse_avatar_acquire();assert(held);
        size_t live=avatar_webp_live_bytes();void *extra=muse_webp_malloc(AVATAR_PEAK-live-sizeof(webp_header_t)-64);assert(extra);
        free_psram(3u*MIB);assert(!sync_once(1) && s_image==old && held->data.width==16);disk_matches(old->raw_pixels);
        muse_webp_free(extra);free_psram(3u*MIB);assert(sync_once(1));assert(held->data.width==16);
        muse_avatar_release(held);
    } else if(!strcmp(which,"cache")) {
        image_t *old=seed();uint16_t original[256];memcpy(original,old->raw_pixels,512);publish(NULL);
        free_psram(AVATAR_RESERVE);assert(!cache_load() && !s_image && !avatar_webp_live_bytes());
        free_psram(3u*MIB);assert(cache_load() && s_image && !memcmp(original,s_image->raw_pixels,512));disk_matches(original);
#if CONFIG_MUSE_AVATAR_WHITE_MATTE
        assert(s_image->alpha && !s_image->matte_pending);
#endif
    } else if(!strcmp(which,"heap_failure") || !strcmp(which,"decoder_failure")) {
        image_t *old=seed();const muse_avatar_image_t *held=muse_avatar_acquire();assert(held);
        size_t live=avatar_webp_live_bytes();free_psram(3u*MIB);
        if(!strcmp(which,"heap_failure"))fail_encoded=true;else fail_payload=512;
        assert(!sync_once(1) && s_image==old && avatar_webp_live_bytes()==live);disk_matches(old->raw_pixels);
        assert(sync_once(1) && held->data.width==16);muse_avatar_release(held);
    } else if(!strcmp(which,"post_pressure")) {
        image_t *old=seed();size_t live=avatar_webp_live_bytes();free_psram(3u*MIB);
        assert(allocation_budget(AVATAR_WORKSPACE));post_pressure=true;
        assert(!muse_webp_malloc(128) && avatar_webp_live_bytes()==live && s_image==old);disk_matches(old->raw_pixels);
        free_psram(3u*MIB);post_pressure=true;assert(!request_alloc(128));free_psram(3u*MIB);
    } else if(!strcmp(which,"release_sampling")) {
        image_t *old=seed();const muse_avatar_image_t *held=muse_avatar_acquire();assert(held);publish(NULL);
        size_t before=heap_used;free_psram(3u*MIB);release_on_free=held;
        size_t limit=allocation_budget(AVATAR_WORKSPACE);
        assert(!avatar_webp_live_bytes() && heap_used<before && !release_on_free);
        assert(limit==(3u*MIB-AVATAR_RESERVE-AVATAR_WORKSPACE<AVATAR_PEAK-AVATAR_WORKSPACE?
            3u*MIB-AVATAR_RESERVE-AVATAR_WORKSPACE:AVATAR_PEAK-AVATAR_WORKSPACE));
        (void)old;
#if CONFIG_MUSE_AVATAR_WHITE_MATTE
    } else if(!strcmp(which,"matte_retry")) {
        avatar_webp_set_budget(AVATAR_PEAK);fail_payload=1024;
        image_t *old=new_image(raw(),16,16,1);assert(old && old->matte_pending);publish(old);
        const muse_avatar_image_t *held=muse_avatar_acquire();assert(held);
        free_psram(AVATAR_RESERVE);assert(!retry_matte(1) && s_image==old && old->raw_pixels[0]==0xffff);
        free_psram(3u*MIB);assert(retry_matte(1) && s_image->alpha && !s_image->matte_pending);
        assert(held->data.pixels[0]==0xffff);muse_avatar_release(held);
    } else if(!strcmp(which,"replacement_matte_failure")) {
        image_t *old=seed();size_t live=avatar_webp_live_bytes();free_psram(3u*MIB);fail_queue_after_owner=true;
        assert(!sync_once(1) && s_image==old && old->alpha && avatar_webp_live_bytes()==live);disk_matches(old->raw_pixels);
        assert(sync_once(1));
#endif
    } else assert(!"unknown case");
    publish(NULL);free(fixture);assert(!avatar_webp_live_bytes() && !heap_used && !heap_blocks);
    printf("PASS %s peak=%zu cap=%u\n",which,heap_peak,(unsigned)AVATAR_PEAK);
}
'''


class AvatarBudget(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix='avatar-budget-')
        cls.addClassCleanup(cls.tmp.cleanup)
        cls.stage = Path(cls.tmp.name)
        (cls.stage / 'esp_heap_caps.h').write_text(HEAP_H)
        (cls.stage / 'sdkconfig.h').write_text('')
        source = (ROOT / 'main/avatar_sync.c').read_text()
        budget = source[source.index('#if CONFIG_MUSE_AVATAR_WHITE_MATTE\n#define AVATAR_PEAK'):
                        source.index('#if CONFIG_SPIRAM_FETCH_INSTRUCTIONS')]
        image_type = re.findall(r'typedef struct \{ muse_avatar_image_t view;[^\n]+\} image_t;', source)[1]
        names = ('const muse_avatar_image_t *muse_avatar_acquire(', 'void muse_avatar_release(',
                 'static void publish(', 'static image_t *new_image(', 'static bool busy(',
                 'static bool cancelled(', 'static void *request_alloc(size_t n) {',
                 'static bool request_stop(', 'static void request_yield(void) {', 'static avatar_response_t *get(',
                 'static uint8_t *fetch_webp(', 'static bool cache_load(', 'static bool cache_proceed(',
                 'static bool cache_save(', 'static bool sync_once(')
        functions = '\n'.join(function(source, name) for name in names)
        functions += '\n#if CONFIG_MUSE_AVATAR_WHITE_MATTE\n' + function(source, 'static bool retry_matte(') + '\n#endif\n'
        cache_defs = '\n'.join(f'#define CACHE_{name} "{cls.stage / file}"' for name, file in (
            ('A', 'a.dat'), ('B', 'b.dat'), ('META', 'metadata.dat'), ('META_TMP', 'metadata.tmp')))
        code = PRELUDE + '\n' + budget + '\n' + cache_defs + '\n' + image_type + '\nstatic image_t *s_image;\n'
        code += 'static void *request_alloc(size_t n);\n' + functions + MAIN
        (cls.stage / 'budget.c').write_text(code)
        fixture = Image.new('RGB', (16, 16), 'white')
        ImageDraw.Draw(fixture).rectangle((4, 4, 11, 11), fill='blue', outline='black')
        cls.fixture = cls.stage / 'synthetic.webp'
        fixture.save(cls.fixture, 'WEBP', lossless=True)
        fixture.close()
        repo = str(ROOT).replace('\\', '/')
        cmake = f'''cmake_minimum_required(VERSION 3.24)
project(avatar_budget_tests C)
set(CMAKE_C_STANDARD 11)
foreach(matte 0 1)
  add_executable(budget_${{matte}} budget.c "{repo}/main/avatar_webp.c" "{repo}/main/avatar_cache.c"
                 "{repo}/main/avatar_request.c" "{repo}/components/muse/muse_avatar_matte.c")
  target_include_directories(budget_${{matte}} PRIVATE "{cls.stage}" "{repo}/main" "{repo}/components/muse")
  target_compile_definitions(budget_${{matte}} PRIVATE ESP_PLATFORM=1 CONFIG_MUSE_AVATAR_WHITE_MATTE=${{matte}} _POSIX_C_SOURCE=200809L)
  target_compile_options(budget_${{matte}} PRIVATE -O1 -g -Wall -Wextra -Werror -Wno-unused-function -Wno-unused-variable -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(budget_${{matte}} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
  include("{repo}/cmake/avatar_webp.cmake")
  muse_avatar_add_webp(budget_${{matte}})
  target_link_libraries(budget_${{matte}} PRIVATE m)
endforeach()
'''
        (cls.stage / 'CMakeLists.txt').write_text(cmake)
        tool = shutil.which('cmake')
        if not tool:
            raise AssertionError('cmake is required for the actual bounded decoder regression')
        args = [tool, '-S', str(cls.stage), '-B', str(cls.stage / 'build')]
        if os.environ.get('AVATAR_WEBP_SOURCE_DIR'):
            args += ['-DFETCHCONTENT_SOURCE_DIR_MUSE_AVATAR_WEBP=' + os.environ['AVATAR_WEBP_SOURCE_DIR'],
                     '-DFETCHCONTENT_FULLY_DISCONNECTED=ON']
        for command in (args, [tool, '--build', str(cls.stage / 'build'), '-j', '4']):
            built = subprocess.run(command, capture_output=True, text=True)
            if built.returncode:
                raise AssertionError(built.stdout + built.stderr)

    def run_case(self, case, matte=1):
        ran = subprocess.run([str(self.stage / 'build' / f'budget_{matte}'), case, str(self.fixture)],
                             capture_output=True, text=True, timeout=30,
                             env={**os.environ, 'ASAN_OPTIONS': ASAN_OPTIONS,
                                  'UBSAN_OPTIONS': 'halt_on_error=1:print_stacktrace=1'})
        self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
        self.assertIn('PASS ' + case, ran.stdout)

    def test_three_mib_free_accepts_real_small_webp_with_matte(self):
        self.run_case('three_mib')

    def test_feature_off_retains_original_lower_cap(self):
        self.run_case('three_mib', matte=0)

    def test_reserve_refusal_preserves_live_and_durable_image(self):
        self.run_case('reserve')

    def test_held_allocations_still_count_against_maximum_before_replacement(self):
        self.run_case('held_cap')

    def test_cache_restore_uses_effective_budget_and_retries_without_raw_changes(self):
        self.run_case('cache')

    def test_real_encoded_allocation_failure_preserves_prior_image(self):
        self.run_case('heap_failure')

    def test_real_decoder_allocation_failure_preserves_prior_image(self):
        self.run_case('decoder_failure')

    def test_pressure_after_admission_rolls_back_allocations_without_taking_reserve(self):
        self.run_case('post_pressure')

    def test_live_release_during_sampling_is_not_double_counted(self):
        self.run_case('release_sampling')

    def test_matte_retry_keeps_pinned_raw_until_success(self):
        self.run_case('matte_retry')

    def test_failed_replacement_matte_retains_previous_mask_and_cache(self):
        self.run_case('replacement_matte_failure')
