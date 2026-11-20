# Copyright (c) Meta Platforms, Inc. and affiliates.
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at http://www.apache.org/licenses/LICENSE-2.0
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
"""Run the production worker with a virtual clock/decoder and real bounded packs."""
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest
from test_avatar_clips import clips
from test_muse_ptt_ui import LINK_GC

ROOT = Path(__file__).resolve().parents[1]
MUSE = ROOT / 'components/muse'
FAKES = r'''
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdlib.h>
typedef void *TaskHandle_t;
typedef void *TimerHandle_t;
typedef unsigned TickType_t;
typedef int BaseType_t;
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(p) ((void)(p))
#define portEXIT_CRITICAL(p) ((void)(p))
#define pdPASS 1
#define pdTRUE 1
#define pdMS_TO_TICKS(ms) (ms)
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define MALLOC_CAP_INTERNAL 4
#define MUSE_VIDEO_CODEC_H264 1
typedef enum {MUSE_MODE_BOOT,MUSE_MODE_IDLE,MUSE_MODE_LISTENING,MUSE_MODE_THINKING,MUSE_MODE_SPEAKING,MUSE_MODE_ERROR,MUSE_MODE_OFF} muse_mode_t;
typedef struct {muse_mode_t mode;float t,mode_t,level,happy,working;} muse_pose_t;
muse_mode_t muse_state_mode(float *time);
bool muse_state_asleep(void);
bool muse_voice_in_call(void);
int64_t esp_timer_get_time(void);
void *heap_caps_malloc(size_t bytes,unsigned caps);
void *heap_caps_calloc(size_t count,size_t bytes,unsigned caps);
void heap_caps_free(void *ptr);
size_t heap_caps_get_free_size(unsigned caps);
void vTaskDelay(TickType_t ticks);
void vTaskDeleteWithCaps(TaskHandle_t task);
BaseType_t xTaskCreatePinnedToCoreWithCaps(void (*fn)(void *),const char *name,unsigned stack,void *arg,unsigned priority,TaskHandle_t *task,int core,unsigned caps);
TimerHandle_t xTimerCreate(const char *name,TickType_t ticks,BaseType_t repeat,void *id,void (*fn)(TimerHandle_t));
BaseType_t xTimerStart(TimerHandle_t timer,TickType_t ticks);
BaseType_t xTimerStop(TimerHandle_t timer,TickType_t ticks);
'''
HARNESS = r'''
#include <assert.h>
#include <string.h>
#include "motion.c"
static int64_t clock_us=1000000,decode_cost=20000,starts[64];
static int scenario,phase,pause_ticks,allocations,opens,closes,decoded;
static unsigned frozen_revision;
static muse_avatar_source_t selected=MUSE_AVATAR_PERSONAL;
static muse_mode_t mode=MUSE_MODE_IDLE;
static void (*queued)(void *);
struct muse_vdec {int width,height;};
static uint8_t y_plane[240*240],uv_plane[120*120];
muse_mode_t muse_state_mode(float *time) {(void)time;return mode;}
bool muse_state_asleep(void) {return false;}
bool muse_voice_in_call(void) {return false;}
int64_t esp_timer_get_time(void) {return clock_us;}
muse_avatar_source_t muse_avatar_source(void) {return selected;}
bool muse_avatar_personal_allowed(void) {return selected==MUSE_AVATAR_PERSONAL;}
void muse_avatar_release(const muse_avatar_image_t *image) {assert(image && image->release);image->release(image);}
bool muse_avatar_files_mount(void) {return true;}
bool muse_avatar_files_space(size_t bytes) {(void)bytes;return true;}
void *heap_caps_malloc(size_t bytes,unsigned caps) {(void)caps;void *p=malloc(bytes);if(p)allocations++;if(scenario==6 && bytes==240*240*2)clock_us+=100000;return p;}
void *heap_caps_calloc(size_t n,size_t bytes,unsigned caps) {(void)caps;void *p=calloc(n,bytes);if(p)allocations++;return p;}
void heap_caps_free(void *p) {if(p){allocations--;free(p);}}
size_t heap_caps_get_free_size(unsigned caps) {(void)caps;return scenario==4 && phase==1?32*1024:64*1024*1024;}
void vTaskDeleteWithCaps(TaskHandle_t task) {assert(!task);}
BaseType_t xTaskCreatePinnedToCoreWithCaps(void (*fn)(void *),const char *name,unsigned stack,void *arg,unsigned priority,TaskHandle_t *task,int core,unsigned caps) {
    (void)name;(void)arg;(void)priority;(void)task;assert(stack==6144 && core==0 && (caps&MALLOC_CAP_INTERNAL) && !queued);queued=fn;return pdPASS;
}
muse_vdec_t *muse_vdec_open(uint8_t codec,int w,int h) {
    assert(codec==MUSE_VIDEO_CODEC_H264);opens++;muse_vdec_t *d=malloc(sizeof(*d));assert(d);d->width=w;d->height=h;return d;
}
void muse_vdec_limit_refs(muse_vdec_t *d,unsigned refs) {assert(d && refs==1);}
void muse_vdec_close(muse_vdec_t *d) {if(d){closes++;free(d);}}
muse_vdec_result_t muse_vdec_decode(muse_vdec_t *d,const uint8_t *data,uint32_t len,muse_pic_t *pic) {
    assert(d && data && len);assert(decoded<64);starts[decoded++]=clock_us;
    if(phase==3) {assert(s_frame && s_frame->view.revision==frozen_revision);phase=4;}
    clock_us+=decode_cost;memset(y_plane,128,sizeof(y_plane));memset(uv_plane,128,sizeof(uv_plane));
    *pic=(muse_pic_t){y_plane,uv_plane,uv_plane,d->width,d->width/2,d->width,d->height};return MUSE_VDEC_PICTURE;
}
void vTaskDelay(TickType_t ticks) {
    clock_us+=(int64_t)ticks*1000;
    if(!phase && atomic_load(&s_frames)==3) {
        frozen_revision=s_frame->view.revision;phase=1;
        if(scenario==3)mode=MUSE_MODE_SPEAKING;
        if(scenario==4)atomic_store(&s_want,MUSE_CLIP_WORKING);
        if(scenario==5)atomic_store(&s_installing,true);
    }
    if(phase==1 && scenario!=2 && scenario!=6) {
        assert(s_frame && s_frame->view.revision==frozen_revision);
        if(ticks==40 && scenario==0 && ++pause_ticks==3)phase=2;
    }
    if(phase!=1 || scenario==2 || scenario==3 || scenario==4 || scenario==6)atomic_store(&s_last_ui,clock_us);
    else atomic_store(&s_last_ui,clock_us-201000);
    if(atomic_load(&s_frames)>=6 && (scenario==0 || scenario==2 || scenario==6 || phase>=3))selected=MUSE_AVATAR_BUILTIN;
}
static void start_worker(void) {
    muse_pose_t pose={.mode=mode,.happy=1};muse_avatar_motion_tick(&pose);
    assert(queued);void (*fn)(void *)=queued;queued=NULL;fn(NULL);
}
int main(int argc,char **argv) {
    assert(argc==2);scenario=atoi(argv[1]);if(scenario==2)decode_cost=70000;
    uint8_t key[32]={0};muse_avatar_motion_bind(key,key);
    start_worker();assert(opens==closes && !atomic_load(&s_running));
    if(scenario==0) {assert(pause_ticks==3 && opens==1 && decoded==6 && !s_frame && !allocations);}
    if(scenario==2) {assert(decoded==6 && starts[1]-starts[0]<80000 && atomic_load(&s_late)>0 && !allocations);}
    if(scenario==6) {assert(decoded>6 && atomic_load(&s_skipped)>0 && !allocations);}
    if(scenario==4) {
        assert(s_frame && atomic_load(&s_retry_after)>clock_us && decoded==3);
        muse_avatar_motion_enable(false);assert(!s_frame && !allocations);
    }
    if(scenario==1 || scenario==3 || scenario==5) {
        assert(s_frame && s_frame->view.revision==frozen_revision && atomic_load(&s_loaded)==0);
        const muse_avatar_image_t *held=muse_avatar_motion_acquire();assert(held==&s_frame->view);muse_avatar_release(held);
        if(scenario==3 || scenario==5) {
            muse_pose_t voice={.mode=scenario==3?MUSE_MODE_SPEAKING:MUSE_MODE_IDLE};muse_avatar_motion_tick(&voice);assert(!queued && s_frame);
            atomic_store(&s_installing,false);
            mode=MUSE_MODE_IDLE;phase=3;start_worker();assert(!s_frame && !allocations && opens==closes);
        } else {
            atomic_store(&s_want,MUSE_CLIP_WORKING); /* retained frame bridges a new clip's warm-up */
            held=muse_avatar_motion_acquire();assert(held);
            muse_avatar_motion_bind(NULL,NULL);assert(!muse_avatar_motion_acquire() && allocations==2);
            muse_avatar_release(held);assert(!allocations); /* pinned reader outlives invalidation */
        }
    }
    puts("PASS motion worker: continuity, bounded retention, pinned lifetime, voice suspension, no added late-frame delay");
}
'''

class AvatarMotionRuntime(unittest.TestCase):
    def test_worker_transitions_and_frame_pacing(self):
        with tempfile.TemporaryDirectory() as tmp:
            stage = Path(tmp)
            (stage / 'fakes.h').write_text(FAKES)
            for name in ('muse_pixel.h', 'muse_state.h', 'muse_voice.h', 'muse_vstream.h', 'esp_heap_caps.h', 'esp_timer.h',
                         'freertos/FreeRTOS.h', 'freertos/task.h', 'freertos/timers.h', 'freertos/idf_additions.h'):
                path = stage / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text('#include "fakes.h"\n')
            for name in ('muse_avatar_motion.h', 'muse_avatar.h', 'muse_avatar_render.h', 'muse_avatar_pack.h', 'muse_vdec.h'):
                shutil.copyfile(MUSE / name, stage / name)
            (stage / 'sdkconfig.h').write_text('#define CONFIG_MUSE_AVATAR_ANIMATIONS 1\n')
            source = (MUSE / 'muse_avatar_motion.c').read_text().replace('"/files/.muse-avatar-v1/motion.dat"', '"' + str(stage / 'pack.dat') + '"')
            (stage / 'motion.c').write_text(source)
            (stage / 'test.c').write_text(HARNESS)
            unit = b'\x00\x00\x00\x01\x09\x10\x00\x00\x01\x65abc'
            (stage / 'pack.dat').write_bytes(bytes(32) + clips.make_pack(bytes(32), {'idle': [unit] * 8, 'working': [unit] * 8}))
            exe = str(stage / 'motion')
            subprocess.run([*shlex.split(os.environ.get('CC', 'cc')), '-std=c11', '-D_POSIX_C_SOURCE=200809L',
                '-Wall', '-Wextra', '-Werror', '-Wno-unused-function', '-ffunction-sections', '-fdata-sections', *LINK_GC,
                '-fsanitize=address,undefined', '-I', tmp, str(stage / 'test.c'), str(MUSE / 'muse_avatar_pack.c'), '-o', exe], check=True)
            for scenario in range(7):
                with self.subTest(scenario=scenario):
                    subprocess.run([exe, str(scenario)], check=True, timeout=10,
                        env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0:halt_on_error=1'})
