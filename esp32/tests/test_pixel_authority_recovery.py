# Copyright (c) Meta Platforms, Inc. and affiliates.
# SPDX-License-Identifier: Apache-2.0
"""Executable production sync/cache authority regression on synthetic data only.

Uses the real cache format/read/write and owning sync functions. Cloud responses,
WebP fetch/decode, and digest are fakes; this is generation-lifecycle evidence,
not SHA-256 or network evidence. The pixel backend's rejection behavior is
separately exercised by test_pixel_backend.
"""
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile
import unittest
from test_avatar_context import function
from pixel_test_sanitizers import ASAN_OPTIONS

ROOT = Path(__file__).resolve().parents[1]
MUSE = ROOT / 'components/muse'

PRELUDE = r'''
#include <assert.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "muse_avatar.h"
#include "muse_pixel_avatar.h"
#include "avatar_identity.h"
#include "avatar_request.h"
#include "avatar_webp.h"
#include "avatar_cache.h"
#define portENTER_CRITICAL(p) ((void)0)
#define portEXIT_CRITICAL(p) ((void)0)
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define JOB_REFRESH 1u
#define JOB_CLEAR 2u
#define JOB_BINDING 4u
#define AVATAR_PEAK (1536*1024)
#define ESP_OK 0
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define MBEDTLS_MD_SHA256 1
typedef struct {uint32_t hash;} mbedtls_md_context_t;
static atomic_uint s_source=MUSE_AVATAR_PERSONAL,s_generation=1,s_content_epoch=1,s_metadata_epoch,s_jobs;
static atomic_bool s_binding_blocked;
static atomic_uint s_path_id,s_revision_id,s_cached_path_id,s_cached_revision_id,s_cache_loads,s_cache_writes,s_identity_reads;
static atomic_size_t s_last_peak;
static atomic_int s_last_ms;
static _Atomic(const char *) s_status="test";
static uint8_t s_binding[32];
static unsigned s_revision,s_cache_sequence;
static int s_cache_slot=-1;
static bool s_image_saved;
static uint32_t s_pixel_authority_epoch;
static uint8_t s_pixel_authority_key[32];
static bool s_pixel_authority_valid;
static int fail_digest,fail_alloc,fail_mount,downloads,cache_failures,key_a_calls;
static uint8_t pixel_key[32],digest_a[32],digest_b[32];
static bool pixel_known,pixel_required,pixel_visible;
static avatar_identity_t id_a={"synthetic/a.webp","revision-A"},id_b={"synthetic/b.webp","revision-B"};
static avatar_identity_t s_identity;
static void worker_kick(void) {}
static void mbedtls_md_init(mbedtls_md_context_t *p) {p->hash=1;}
static void mbedtls_md_free(mbedtls_md_context_t *p) {(void)p;}
static const void *mbedtls_md_info_from_type(int type) {assert(type==1);return (void *)1;}
static int mbedtls_md_setup(mbedtls_md_context_t *p,const void *info,int hmac) {(void)p;(void)info;(void)hmac;return fail_digest?-1:0;}
static int mbedtls_md_starts(mbedtls_md_context_t *p) {p->hash=1;return 0;}
static int mbedtls_md_update(mbedtls_md_context_t *p,const uint8_t *b,size_t n) {while(n--)p->hash=p->hash*33+*b++;return 0;}
static int mbedtls_md_finish(mbedtls_md_context_t *p,uint8_t out[32]) {for(unsigned i=0;i<32;i++)out[i]=(uint8_t)(p->hash>>(8*(i%4)));return 0;}
void muse_pixel_avatar_identity(const uint8_t *key) {
    if(!key)return;
    memcpy(pixel_key,key,32);pixel_required=pixel_known=true;
    pixel_visible=!memcmp(key,digest_a,32);
    if(pixel_visible)key_a_calls++;
}
void muse_pixel_avatar_identity_unavailable(void) {pixel_required=true;pixel_known=pixel_visible=false;}
void muse_pixel_avatar_invalidate(void) {pixel_required=pixel_known=pixel_visible=false;}
void muse_pixel_avatar_clear(void) {muse_pixel_avatar_invalidate();}
void muse_pixel_avatar_source_changed(void) {pixel_visible=false;}
static void motion_binding(const avatar_identity_t *id);
static void *heap_caps_calloc(size_t n,size_t size,unsigned caps) {(void)caps;return calloc(n,size);}
static void heap_caps_free(void *p) {free(p);}
void *muse_webp_malloc(size_t size) {if(fail_alloc){fail_alloc--;return NULL;}return malloc(size);}
void muse_webp_free(void *p) {free(p);}
bool muse_avatar_files_mount(void) {if(fail_mount){fail_mount--;return false;}return true;}
static bool busy(void) {return false;}
static int64_t esp_timer_get_time(void) {return 1;}
static avatar_response_t response={(uint8_t *)"identity-B",10,200,false};
static avatar_response_t *get(const char *path,const char *const *headers,size_t cap,unsigned gen) {(void)headers;(void)cap;(void)gen;assert(!strcmp(path,"/identity"));return &response;}
void avatar_request_release(avatar_response_t *r) {assert(r==&response);}
int avatar_identity_parse(const char *json,size_t len,avatar_identity_t *out) {assert(len==10 && !memcmp(json,"identity-B",10));*out=id_b;return 1;}
static uint8_t *fetch_webp(const char *path,unsigned gen,size_t *len) {(void)gen;(void)len;assert(!strcmp(path,id_b.path));downloads++;return NULL;}
bool avatar_webp_decode(const uint8_t *b,size_t n,int max,size_t budget,avatar_webp_result_t *out) {(void)b;(void)n;(void)max;(void)budget;(void)out;assert(!"download failed before decoding");return false;}
static bool cache_save(void *image,const avatar_identity_t *id,unsigned gen) {(void)image;(void)id;(void)gen;assert(!"download failed before save");return false;}
static void cached_identity(const avatar_identity_t *id) {(void)id;}
static int muse_settings_set_avatar_source(muse_avatar_source_t source) {(void)source;return ESP_OK;}
'''

MAIN = r'''
static void motion_binding(const avatar_identity_t *id) {pixel_identity(id);}
muse_avatar_source_t muse_avatar_source(void) {return (muse_avatar_source_t)atomic_load(&s_source);}
int main(int argc,char **argv) {
    assert(argc==2);bool digest_failure=!strcmp(argv[1],"digest_failure");
    assert(pixel_identity_key(&id_a,digest_a) && pixel_identity_key(&id_b,digest_b) && memcmp(digest_a,digest_b,32));
    uint16_t raw[16];for(unsigned i=0;i<16;i++)raw[i]=(uint16_t)(i*101);
    avatar_cache_header_t h;avatar_cache_header_init(&h,1,4,4,s_binding,&id_a,raw);
    assert(avatar_cache_write(CACHE_A,&h,raw));
    /* Exact failure/recovery order: first cached still A is temporarily
     * unavailable, online metadata B validates, B's image fetch fails, then
     * the original durable cache A becomes readable. */
    fail_mount=1;assert(!cache_load() && !s_image);cache_failures++;
    fail_digest=digest_failure;
    assert(!sync_once(atomic_load(&s_generation)) && downloads==1);
    assert(s_pixel_authority_epoch==atomic_load(&s_content_epoch));
    if(digest_failure)assert(!s_pixel_authority_valid && pixel_required && !pixel_known);
    else assert(s_pixel_authority_valid && pixel_known && !memcmp(pixel_key,digest_b,32));
    fail_digest=0;
    assert(cache_load() && s_image && !memcmp(s_image->raw_pixels,raw,sizeof(raw)));
    uint8_t still_key[32];assert(muse_avatar_copy_image_key(&s_image->view,still_key) && !memcmp(still_key,digest_a,32));
    assert(key_a_calls==0 && !pixel_visible);
    if(digest_failure)assert(pixel_required && !pixel_known);
    else assert(pixel_known && !memcmp(pixel_key,digest_b,32));
    /* A pinned still and its cache key remain A, while B's online authority
     * survives temporary still loss and Personal -> Built-in -> Personal. */
    muse_avatar_select(MUSE_AVATAR_BUILTIN);publish(NULL);
    muse_avatar_select(MUSE_AVATAR_PERSONAL);assert(cache_load());
    assert(key_a_calls==0 && !pixel_visible);
    if(digest_failure) {
        assert(!sync_once(atomic_load(&s_generation)));
        assert(pixel_known && !memcmp(pixel_key,digest_b,32));
    }
    /* Digest-independent allocation failure cannot erase B's authority. */
    publish(NULL);fail_alloc=1;assert(!cache_load());assert(cache_load());
    assert(key_a_calls==0 && !pixel_visible);
    /* Real owner/content boundaries revoke authority by moving the content
     * epoch; cached identity may bootstrap offline only in that new epoch. */
    muse_avatar_pairing_changed();assert(!pixel_required);
    atomic_store(&s_binding_blocked,false);publish(NULL);assert(cache_load());
    assert(pixel_visible && key_a_calls==1 && !memcmp(pixel_key,digest_a,32));
    muse_avatar_clear();publish(NULL);assert(!pixel_required);assert(cache_load());assert(pixel_visible);
    publish(NULL);assert(cache_failures==1);puts("PASS authoritative metadata survives failed download, late real cache recovery, source switches and digest failure");
}
'''


class PixelAuthorityRecovery(unittest.TestCase):
    def test_online_generation_is_not_replaced_by_late_cached_still(self):
        self.run_case('recovery')

    def test_digest_failure_after_valid_metadata_fails_closed_through_cache_recovery(self):
        self.run_case('digest_failure')

    def run_case(self, case):
        source = (ROOT / 'main/avatar_sync.c').read_text()
        image_type = re.search(r'typedef struct \{ muse_avatar_image_t view;[^\n]+pixel_key_valid; \} image_t;', source).group()
        signatures = (
            'static bool pixel_identity_key(', 'static void pixel_identity(',
            'static void pixel_authoritative_identity(', 'static bool set_image_pixel_key(',
            'bool muse_avatar_copy_image_key(', 'void muse_avatar_release(',
            'static void publish(', 'static image_t *new_image(',
            'static bool cancelled(', 'static bool cache_load(', 'static bool sync_once(',
            'void muse_avatar_select(', 'void muse_avatar_clear(', 'void muse_avatar_pairing_changed(',
        )
        functions = '\n'.join(function(source, sig) for sig in signatures)
        with tempfile.TemporaryDirectory(prefix='pixel-authority-recovery-') as tmp:
            stage = Path(tmp)
            (stage / 'sdkconfig.h').write_text('#define CONFIG_MUSE_PIXEL_AVATAR 1\n')
            cache_defs = '\n'.join(f'#define CACHE_{name} "{stage / file}"' for name, file in (
                ('A', 'a.dat'), ('B', 'b.dat'), ('META', 'metadata.dat'), ('META_TMP', 'metadata.tmp')))
            code = PRELUDE + '\n' + cache_defs + '\n' + image_type + '\nstatic image_t *s_image;\n' + functions + MAIN
            (stage / 'test.c').write_text(code)
            exe = stage / 'authority-test'
            built = subprocess.run([
                *shlex.split(os.environ.get('CC', 'cc')), '-std=c11', '-D_POSIX_C_SOURCE=200809L',
                '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
                '-Wno-unused-variable', '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                '-I', str(stage), '-I', str(MUSE), '-I', str(ROOT / 'main'),
                str(stage / 'test.c'), str(ROOT / 'main/avatar_cache.c'), '-o', str(exe),
            ], capture_output=True, text=True)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            ran = subprocess.run([str(exe), case], capture_output=True, text=True, timeout=30,
                env={**os.environ, 'ASAN_OPTIONS': ASAN_OPTIONS,
                     'UBSAN_OPTIONS': 'halt_on_error=1'})
            self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
            self.assertIn('PASS authoritative metadata', ran.stdout)


if __name__ == '__main__':
    unittest.main()
