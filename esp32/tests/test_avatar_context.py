# Copyright (c) Meta Platforms, Inc. and affiliates.
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at http://www.apache.org/licenses/LICENSE-2.0
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
"""Compile owning backend functions and interleave invalidation with late publishes."""
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

def function(source, signature):
    start = source.index(signature)
    body = source.index('{', start)
    depth = 1
    end = body + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

class AvatarContext(unittest.TestCase):
    def test_clear_pairing_and_late_image_publication(self):
        source = (ROOT / 'main/avatar_sync.c').read_text()
        image_type = re.search(r'typedef struct \{ muse_avatar_image_t view;[^\n]+\} image_t;', source).group()
        functions = '\n'.join(function(source, signature) for signature in (
            'void muse_avatar_release(', 'static void publish(', 'static image_t *new_image(',
            'uint32_t muse_avatar_content_epoch(', 'void muse_avatar_clear(', 'void muse_avatar_pairing_changed('))
        harness = r'''
#include <assert.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include "muse_avatar.h"
#define portENTER_CRITICAL(guard) ((void)0)
#define portEXIT_CRITICAL(guard) ((void)0)
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define JOB_CLEAR 1
#define JOB_BINDING 2
static atomic_uint s_source,s_jobs,s_generation=1,s_content_epoch=1,s_metadata_epoch;
static atomic_bool s_binding_blocked;
static bool s_image_saved;
static unsigned s_revision,worker_starts;
static void worker_kick(void) {worker_starts++;}
static void motion_binding(const void *identity) {(void)identity;}
muse_avatar_source_t muse_avatar_source(void) {return (muse_avatar_source_t)atomic_load(&s_source);}
static void *heap_caps_calloc(size_t n,size_t size,unsigned caps) {(void)caps;return calloc(n,size);}
static void heap_caps_free(void *p) {free(p);}
static void *request_alloc(size_t n) {return heap_caps_calloc(1,n,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);}
static void muse_webp_free(void *p) {free(p);}
static uint32_t avatar_cache_crc(const void *p,size_t bytes) {(void)p;(void)bytes;return 1;}
'''
        main = r'''
static image_t *fresh(uint32_t epoch) {
    return new_image(calloc(16,sizeof(uint16_t)),4,4,epoch);
}
int main(void) {
    assert(muse_avatar_content_epoch()==0);
    uint32_t first=atomic_load(&s_content_epoch);
    publish(fresh(first));assert(muse_avatar_content_epoch()==first);
    atomic_store(&s_source,MUSE_AVATAR_BUILTIN);
    publish(NULL);assert(muse_avatar_content_epoch()==first); /* preserve derived metadata through source switch */
    muse_avatar_clear();assert(muse_avatar_content_epoch()==0);
    publish(fresh(first));assert(muse_avatar_content_epoch()==0); /* old read finishes after clear */
    uint32_t second=atomic_load(&s_content_epoch);
    publish(fresh(second));assert(muse_avatar_content_epoch()==second);
    muse_avatar_pairing_changed();assert(muse_avatar_content_epoch()==0);
    publish(fresh(second));assert(muse_avatar_content_epoch()==0);
    atomic_store(&s_binding_blocked,false);assert(muse_avatar_content_epoch()==0); /* binding check alone cannot revalidate old pixels */
    uint32_t third=atomic_load(&s_content_epoch);
    publish(fresh(third));assert(muse_avatar_content_epoch()==third);
    publish(NULL);assert(muse_avatar_content_epoch()==third);
    assert(worker_starts==1);
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            stage = Path(tmp)
            (stage / 'sdkconfig.h').write_text('')
            (stage / 'context.c').write_text(harness + image_type + '\nstatic image_t *s_image;\n' + functions + main)
            exe = str(stage / 'context')
            subprocess.run([*shlex.split(os.environ.get('CC', 'cc')), '-std=c11', '-Wall', '-Wextra', '-Werror',
                '-fsanitize=address,undefined', '-I', tmp, '-I', str(ROOT / 'components/muse'),
                str(stage / 'context.c'), '-o', exe], check=True)
            subprocess.run([exe], check=True)
