/* Copyright (c) Meta Platforms, Inc. and affiliates.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at http://www.apache.org/licenses/LICENSE-2.0
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "muse_avatar_motion.h"
#include "muse_vdec.h"
#include "muse_vstream.h"
#include "muse_state.h"
#include "muse_voice.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "freertos/idf_additions.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define PACK_FILE "/files/.muse-avatar-v1/motion.dat"
#define PACK_TMP "/files/.muse-avatar-v1/motion.tmp"
#define RESERVE (1024*1024)
typedef struct { muse_avatar_image_t view; unsigned refs; int clip; unsigned epoch; } frame_t;
static portMUX_TYPE s_guard=portMUX_INITIALIZER_UNLOCKED;
static uint8_t s_source[32],s_binding[32];
static bool s_bound;
static unsigned s_epoch=1;
static frame_t *s_frame;
static atomic_bool s_enabled=true,s_running,s_failed,s_installing;
static atomic_int s_want=-1;
static atomic_uint s_preview;
static atomic_int_fast64_t s_preview_until;
static atomic_int_fast64_t s_last_ui;
static atomic_uint s_ui_frames;
static atomic_int_fast64_t s_retry_after;
static uint32_t s_seen_revision;
void muse_avatar_motion_seen(uint32_t revision) {
    if(revision!=s_seen_revision) {s_seen_revision=revision;atomic_fetch_add(&s_ui_frames,1);}
}
static atomic_uint s_frames,s_errors,s_dec_us,s_peak_us,s_loaded,s_allocated,s_late,s_decoded,s_skipped;
static atomic_uint s_coverage;
static uint8_t *s_upload;
static size_t s_upload_size,s_upload_offset;
static uint8_t s_upload_source[32],s_upload_binding[32];
static unsigned s_upload_epoch;
static atomic_flag s_upload_guard=ATOMIC_FLAG_INIT;
static TimerHandle_t s_upload_timer;
static int64_t s_upload_deadline;
static void upload_lock(void) {while(atomic_flag_test_and_set(&s_upload_guard))vTaskDelay(1);}
static void upload_unlock(void) {atomic_flag_clear(&s_upload_guard);}
static bool voice_busy(void) {
    muse_mode_t mode=muse_state_mode(NULL);
    return muse_voice_in_call() || mode==MUSE_MODE_LISTENING || mode==MUSE_MODE_SPEAKING || mode==MUSE_MODE_THINKING;
}
static uint32_t s_revision=0x80000000u;

static bool keys(uint8_t source[32],uint8_t binding[32],unsigned *epoch) {
    portENTER_CRITICAL(&s_guard);
    bool ok=s_bound;
    if(ok) { memcpy(source,s_source,32);memcpy(binding,s_binding,32); }
    *epoch=s_epoch;
    portEXIT_CRITICAL(&s_guard);return ok;
}
static bool current(unsigned epoch) {
    portENTER_CRITICAL(&s_guard);bool ok=s_bound && s_epoch==epoch;portEXIT_CRITICAL(&s_guard);
    return ok && atomic_load(&s_enabled) && muse_avatar_personal_allowed();
}
static void release_frame(const muse_avatar_image_t *view) {
    frame_t *f=(frame_t *)view;
    portENTER_CRITICAL(&s_guard);bool last=--f->refs==0;portEXIT_CRITICAL(&s_guard);
    if(last) { heap_caps_free((void *)f->view.data.pixels);heap_caps_free(f); }
}
static void publish(frame_t *f) {
    portENTER_CRITICAL(&s_guard);frame_t *old=s_frame;s_frame=f;portEXIT_CRITICAL(&s_guard);
    if(old) release_frame(&old->view);
}
const muse_avatar_image_t *muse_avatar_motion_acquire(void) {
    if(!muse_avatar_personal_allowed()) return NULL;
    portENTER_CRITICAL(&s_guard);
    frame_t *f=s_frame;
    if(!s_bound || !atomic_load(&s_enabled) || !f || f->epoch!=s_epoch) f=NULL;
    if(f) f->refs++;
    portEXIT_CRITICAL(&s_guard);return f?&f->view:NULL;
}
void muse_avatar_motion_bind(const uint8_t source[32],const uint8_t binding[32]) {
    frame_t *old=NULL;
    portENTER_CRITICAL(&s_guard);
    bool bound=source && binding;
    if(bound!=s_bound || (bound && (memcmp(source,s_source,32)||memcmp(binding,s_binding,32)))) {
        s_bound=bound;s_epoch++;
        old=s_frame;s_frame=NULL;
        if(bound) {memcpy(s_source,source,32);memcpy(s_binding,binding,32);}
        atomic_store(&s_failed,false);atomic_store(&s_coverage,0);
    }
    portEXIT_CRITICAL(&s_guard);
    if(old)release_frame(&old->view);
}
void muse_avatar_motion_enable(bool enabled) { atomic_store(&s_enabled,enabled); if(!enabled) {atomic_store(&s_want,-1);publish(NULL);} else {atomic_store(&s_failed,false);atomic_store(&s_retry_after,0);} }
static uint8_t *load_pack(const uint8_t source[32],const uint8_t binding[32],muse_avatar_pack_t *p) {
    if(!muse_avatar_files_mount()) return NULL;
    FILE *f=fopen(PACK_FILE,"rb");if(!f)return NULL;
    uint8_t stored_binding[32];
    bool ok=!fseek(f,0,SEEK_END);long length=ok?ftell(f):-1;
    ok=ok && length>96 && length<=MUSE_AVATAR_PACK_MAX+32 && !fseek(f,0,SEEK_SET);
    size_t size=ok?(size_t)length-32:0;
    if(ok) ok=fread(stored_binding,1,32,f)==32 && !memcmp(stored_binding,binding,32);
    uint8_t *data=ok && heap_caps_get_free_size(MALLOC_CAP_SPIRAM)>size+RESERVE?
        heap_caps_malloc(size,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT):NULL;
    ok=data && fread(data,1,size,f)==size && !ferror(f);
    fclose(f);
    if(ok) ok=muse_avatar_pack_parse(data,size,source,p);
    if(!ok) {heap_caps_free(data);return NULL;}
    return data;
}
static uint16_t rgb(int y,int u,int v) {
    int c=298*(y-16),r=(c+409*(v-128)+128)>>8,g=(c-100*(u-128)-208*(v-128)+128)>>8,b=(c+516*(u-128)+128)>>8;
    r=r<0?0:r>255?255:r;g=g<0?0:g>255?255:g;b=b<0?0:b>255?255:b;
    return (uint16_t)(((r&248)<<8)|((g&252)<<3)|(b>>3));
}
static frame_t *make_frame(const muse_pic_t *pic,int w,int h,int clip,unsigned epoch) {
    if(!pic->y || !pic->u || !pic->v || pic->w!=w || pic->h!=h || pic->y_stride<w || pic->c_stride<w/2) return NULL;
    frame_t *f=heap_caps_calloc(1,sizeof(*f),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    uint16_t *pixels=f?heap_caps_malloc((size_t)w*h*2,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT):NULL;
    if(!pixels) {heap_caps_free(f);return NULL;}
    for(int y=0;y<h;y++) for(int x=0;x<w;x++)
        pixels[(size_t)y*w+x]=rgb(pic->y[(size_t)y*pic->y_stride+x],pic->u[(size_t)(y/2)*pic->c_stride+x/2],pic->v[(size_t)(y/2)*pic->c_stride+x/2]);
    f->view=(muse_avatar_image_t){.data={pixels,w,h},.revision=++s_revision,.release=release_frame};
    f->refs=1;f->clip=clip;f->epoch=epoch;return f;
}
static void run(void *arg) {
    (void)arg;
    uint8_t source[32],binding[32];unsigned epoch=0;
    muse_avatar_pack_t pack={0};uint8_t *bytes=NULL,*scratch=NULL;
    muse_vdec_t *dec=NULL;int clip=-1;uint32_t cursor=0,frame=0;
    int64_t due=0,paused_since=0;
    bool idle_stop=false;
    for(;;) {
        unsigned wanted_epoch;bool bound=keys(source,binding,&wanted_epoch);
        if(!bound || !atomic_load(&s_enabled) || muse_avatar_source()!=MUSE_AVATAR_PERSONAL) break;
        if(wanted_epoch!=epoch) {
            muse_vdec_close(dec);dec=NULL;heap_caps_free(bytes);bytes=NULL;clip=-1;
            atomic_store(&s_loaded,0);atomic_store(&s_allocated,0);epoch=wanted_epoch;
            bytes=load_pack(source,binding,&pack);
            if(!bytes) {atomic_store(&s_failed,true);break;}
            atomic_store(&s_coverage,pack.coverage);atomic_store(&s_loaded,(unsigned)pack.size);
        }
        int want=atomic_load(&s_want);
        int64_t now=esp_timer_get_time();
        muse_mode_t mode=muse_state_mode(NULL);
        bool paused=muse_state_asleep() || now-atomic_load(&s_last_ui)>200000 || want<0 ||
                    mode==MUSE_MODE_LISTENING || mode==MUSE_MODE_SPEAKING || mode==MUSE_MODE_OFF || atomic_load(&s_installing);
        if(paused) {
            if(!paused_since)paused_since=now;
            /* Freeze in place during a short trip to Settings. Retain the
             * decoder cursor, and do not expose the different still image. */
            if(now-paused_since>5000000 || muse_state_asleep()) {idle_stop=true;break;}
            vTaskDelay(pdMS_TO_TICKS(40));continue;
        }
        if(paused_since)due=now;
        paused_since=0;
        if(want>=MUSE_AVATAR_CLIPS || !(pack.coverage&(1u<<want))) {vTaskDelay(pdMS_TO_TICKS(40));continue;}
        if(want!=clip || !dec) {
            bool different=want!=clip;
            muse_vdec_close(dec);dec=NULL;
            if(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)<RESERVE+768*1024 ||
               heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)<48*1024) break;
            if(!scratch) scratch=heap_caps_malloc(MUSE_AVATAR_FRAME_MAX,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
            if(!scratch) {atomic_store(&s_failed,true);break;}
            dec=muse_vdec_open(MUSE_VIDEO_CODEC_H264,pack.width,pack.height);
            if(!dec) {atomic_store(&s_failed,true);break;}
            muse_vdec_limit_refs(dec,1);
            clip=want;cursor=0;frame=0;if(different)due=0;
        }
        const uint8_t *data;uint32_t size;
        if(frame==pack.clip[clip].frames) {
            /* No prediction across loop boundaries; every clip starts with an IDR. */
            if(!pack.clip[clip].loop) {atomic_store(&s_want,MUSE_CLIP_IDLE);continue;}
            muse_vdec_close(dec);dec=NULL;continue;
        }
        if(!muse_avatar_pack_frame(&pack,(unsigned)clip,&cursor,&data,&size)) {atomic_fetch_add(&s_errors,1);atomic_store(&s_failed,true);break;}
        int64_t started=esp_timer_get_time();muse_pic_t pic;
        /* tinyh264 unescapes NAL bytes in place. The pack stays immutable,
         * including across loops and CRC validation. */
        memcpy(scratch,data,size);
        muse_vdec_result_t result=muse_vdec_decode(dec,scratch,size,&pic);frame++;
        if(result!=MUSE_VDEC_PICTURE) {atomic_fetch_add(&s_errors,1);atomic_store(&s_failed,true);break;}
        atomic_fetch_add(&s_decoded,1);
        int64_t decoded_at=esp_timer_get_time();
        if(!due)due=decoded_at;
        if(decoded_at>due+1000000/pack.fps) {
            /* Preserve every H.264 prediction reference, but omit RGB output
             * for an overdue picture so the clip does not become slow motion. */
            atomic_fetch_add(&s_skipped,1);due+=1000000/pack.fps;
            vTaskDelay(pdMS_TO_TICKS(1));continue;
        }
        frame_t *f=make_frame(&pic,pack.width,pack.height,clip,epoch);
        if(!f) {atomic_store(&s_failed,true);break;}
        unsigned elapsed=(unsigned)(esp_timer_get_time()-started);
        atomic_store(&s_dec_us,elapsed);if(elapsed>atomic_load(&s_peak_us)) atomic_store(&s_peak_us,elapsed);
        /* Keep the source timeline. The next iteration can decode an overdue
         * reference without converting it, rather than stretching the clip. */
        if(esp_timer_get_time()>due)atomic_fetch_add(&s_late,1);
        while(esp_timer_get_time()<due && current(epoch) && atomic_load(&s_want)==clip)
            vTaskDelay(pdMS_TO_TICKS(2));
        if(current(epoch) && atomic_load(&s_want)==clip) {publish(f);atomic_fetch_add(&s_frames,1);}
        else release_frame(&f->view);
        due+=1000000/pack.fps;
        atomic_store(&s_allocated,(unsigned)pack.size+(unsigned)pack.width*pack.height*2);
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    /* Long hidden/voice/sleep pauses release decoding resources but retain one
     * bounded RGB frame to bridge the next decoder warm-up. */
    bool retained=current(epoch);
    if(!retained)publish(NULL);
    muse_vdec_close(dec);heap_caps_free(bytes);heap_caps_free(scratch);
    atomic_store(&s_loaded,0);atomic_store(&s_allocated,0);atomic_store(&s_retry_after,retained && idle_stop?0:esp_timer_get_time()+1000000);atomic_store(&s_running,false);
    vTaskDeleteWithCaps(NULL);
}
bool muse_avatar_motion_preview(unsigned clip) {
    if(clip>=MUSE_AVATAR_CLIPS || !(atomic_load(&s_coverage)&(1u<<clip)))return false;
    atomic_store(&s_preview,clip);atomic_store(&s_preview_until,esp_timer_get_time()+8000000);return true;
}
void muse_avatar_motion_tick(const muse_pose_t *pose) {
    if(!atomic_load(&s_enabled) || muse_avatar_source()!=MUSE_AVATAR_PERSONAL) {atomic_store(&s_want,-1);return;}
    unsigned coverage=atomic_load(&s_coverage);
    int clip=pose->mode==MUSE_MODE_BOOT && (coverage&(1u<<MUSE_CLIP_WAKING))?MUSE_CLIP_WAKING:
        pose->mode==MUSE_MODE_THINKING && pose->working>0 && (coverage&(1u<<MUSE_CLIP_WORKING))?MUSE_CLIP_WORKING:MUSE_CLIP_IDLE;
    if(pose->mode==MUSE_MODE_IDLE && pose->happy<=0.01f && esp_timer_get_time()<atomic_load(&s_preview_until)) clip=(int)atomic_load(&s_preview);
    atomic_store(&s_want,clip);atomic_store(&s_last_ui,esp_timer_get_time());
    if(pose->mode==MUSE_MODE_LISTENING || pose->mode==MUSE_MODE_SPEAKING || pose->mode==MUSE_MODE_OFF || atomic_load(&s_installing))return;
    portENTER_CRITICAL(&s_guard);bool bound=s_bound;portEXIT_CRITICAL(&s_guard);
    if(bound && !atomic_load(&s_failed) && esp_timer_get_time()>=atomic_load(&s_retry_after)) {
        bool expected=false;
        if(atomic_compare_exchange_strong(&s_running,&expected,true) &&
            /* Match the SDK video decoder: core 0 leaves the UI/audio core
             * free, rather than letting first FPU use pin this task to it. */
            xTaskCreatePinnedToCoreWithCaps(run,"avatar_motion",6144,NULL,1,NULL,0,MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)!=pdPASS)
            atomic_store(&s_running,false);
    }
}
void muse_avatar_motion_status(void) {
    upload_lock();unsigned upload=(unsigned)s_upload_size;upload_unlock();
    portENTER_CRITICAL(&s_guard);
    unsigned retained=s_frame?(unsigned)s_frame->view.data.width*s_frame->view.data.height*2:0;
    unsigned revision=s_frame?s_frame->view.revision:0;
    portEXIT_CRITICAL(&s_guard);
    printf("@avatar_motion {\"enabled\":%s,\"running\":%s,\"failed\":%s,\"coverage\":%u,\"clip\":%d,\"frames\":%u,\"ui_frames\":%u,\"decode_us\":%u,\"peak_decode_us\":%u,\"errors\":%u,\"pack_bytes\":%u,\"pack_frame_bytes\":%u,\"upload_bytes\":%u,\"retained_bytes\":%u,\"frame_revision\":%u,\"late_frames\":%u,\"decoded_frames\":%u,\"skipped_frames\":%u,\"parity_complete\":false}\n",
        atomic_load(&s_enabled)?"true":"false",atomic_load(&s_running)?"true":"false",atomic_load(&s_failed)?"true":"false",
        atomic_load(&s_coverage),atomic_load(&s_want),atomic_load(&s_frames),atomic_load(&s_ui_frames),atomic_load(&s_dec_us),atomic_load(&s_peak_us),atomic_load(&s_errors),atomic_load(&s_loaded),atomic_load(&s_allocated),upload,retained,revision,atomic_load(&s_late),atomic_load(&s_decoded),atomic_load(&s_skipped));
}
static void upload_release(void) {
    heap_caps_free(s_upload);s_upload=NULL;s_upload_size=s_upload_offset=0;
    atomic_store(&s_installing,false);
    if(s_upload_timer)xTimerStop(s_upload_timer,0);
}
static void upload_expire(TimerHandle_t timer) {
    (void)timer;
    /* The timer task never waits behind a flash commit or frees a buffer
     * being copied by the serial task. A later tick can reclaim it. */
    if(atomic_flag_test_and_set(&s_upload_guard))return;
    if(s_upload && (esp_timer_get_time()>s_upload_deadline || !current(s_upload_epoch))) upload_release();
    upload_unlock();
}
void muse_avatar_motion_install_abort(void) {upload_lock();upload_release();upload_unlock();}
bool muse_avatar_motion_install_begin(size_t size) {
    upload_lock();upload_release();
    bool ok=size>=64 && size<=MUSE_AVATAR_PACK_MAX && !voice_busy() &&
        keys(s_upload_source,s_upload_binding,&s_upload_epoch) && muse_avatar_files_mount() &&
        heap_caps_get_free_size(MALLOC_CAP_SPIRAM)>=size+RESERVE && muse_avatar_files_space(size+32+64*1024);
    if(ok && !s_upload_timer)s_upload_timer=xTimerCreate("avatar_upload",pdMS_TO_TICKS(5000),pdTRUE,NULL,upload_expire);
    if(ok && s_upload_timer)s_upload=heap_caps_malloc(size,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    ok=ok && s_upload;
    if(ok) {
        s_upload_size=size;s_upload_deadline=esp_timer_get_time()+30000000;
        atomic_store(&s_installing,true);
        ok=xTimerStart(s_upload_timer,0)==pdPASS;
    }
    if(!ok)upload_release();
    upload_unlock();return ok;
}
bool muse_avatar_motion_install_chunk(size_t off,const uint8_t *data,size_t size) {
    upload_lock();
    bool ok=s_upload && off==s_upload_offset && data && size && size<=s_upload_size-s_upload_offset && current(s_upload_epoch);
    if(ok) {memcpy(s_upload+off,data,size);s_upload_offset+=size;s_upload_deadline=esp_timer_get_time()+30000000;}
    upload_unlock();return ok;
}
bool muse_avatar_motion_install_commit(void) {
    upload_lock();
    muse_avatar_pack_t pack;
    bool complete=s_upload && s_upload_offset==s_upload_size;
    bool context=current(s_upload_epoch),voice=voice_busy();
    bool ok=complete && context && !voice && muse_avatar_pack_parse(s_upload,s_upload_size,s_upload_source,&pack);
    const char *reason=!complete?"incomplete":!context?"context_changed":voice?"voice_busy":"invalid_pack";
    FILE *f=NULL;
    if(ok) {reason="open_failed";mkdir("/files/.muse-avatar-v1",0700);f=fopen(PACK_TMP,"wb");ok=f!=NULL;}
    if(ok) {reason="write_failed";ok=fwrite(s_upload_binding,1,32,f)==32 && fwrite(s_upload,1,s_upload_size,f)==s_upload_size && !fflush(f) && !fsync(fileno(f));}
    if(f && fclose(f)) {ok=false;reason="close_failed";}
    if(ok) {context=current(s_upload_epoch);reason=context?"rename_failed":"context_changed";ok=context && !rename(PACK_TMP,PACK_FILE);}
    if(!ok) {unlink(PACK_TMP);printf("@avatar_install_error %s\n",reason);fflush(stdout);}
    upload_release();upload_unlock();
    if(ok) {
        portENTER_CRITICAL(&s_guard);s_epoch++;portEXIT_CRITICAL(&s_guard);
        atomic_store(&s_failed,false);
    }
    return ok;
}
void muse_avatar_motion_clear(void) {
    /* Upload RAM is owned only by the serial task. Binding invalidation
     * rejects its next chunk/commit without freeing it on another task. */
    if(muse_avatar_files_mount()) {unlink(PACK_FILE);unlink(PACK_TMP);}
    portENTER_CRITICAL(&s_guard);s_epoch++;portEXIT_CRITICAL(&s_guard);
    atomic_store(&s_failed,false);atomic_store(&s_coverage,0);
}
