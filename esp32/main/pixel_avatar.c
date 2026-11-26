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
#include "sdkconfig.h"
#if CONFIG_MUSE_PIXEL_AVATAR
#include "muse_pixel_avatar.h"
#include "muse_avatar.h"
#include "muse_state.h"
#include "muse_settings.h"
#include "muse_voice.h"
#include "config_store.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "mbedtls/md.h"
#include "mbedtls/base64.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/idf_additions.h"
#include <stdatomic.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Parent integration supplies this key from the caller's pinned still image,
 * not from mutable global identity state. The duplicate declaration also lets
 * a backend-only host harness use the interface before copying the new header. */
extern bool muse_avatar_copy_image_key(const muse_avatar_image_t *image,uint8_t out[32]);

#define PIXEL_FILE "/files/.muse-avatar-v1/pixel.dat"
#define PIXEL_TMP "/files/.muse-avatar-v1/pixel.tmp"
#define CACHE_HEADER 48
#define JOB_RELOAD 1u
#define JOB_CLEAR 2u
#define RESERVE (1024*1024)
#define OWNER_TEXT_MAX 4096
#define CACHE_WAIT_MS 5000

typedef struct { muse_pixel_avatar_t view; uint8_t owner[32]; unsigned refs; } model_t;
typedef enum { OWNER_INVALID, OWNER_VALID, OWNER_UNAVAILABLE } owner_result_t;
typedef struct { unsigned delay_s; int64_t next; } worker_state_t;
static portMUX_TYPE s_guard=portMUX_INITIALIZER_UNLOCKED;
static model_t *s_model;
static uint8_t s_owner[32],s_identity[32];
static bool s_have_owner,s_owner_known,s_have_identity,s_identity_required,s_clear_pending;
static unsigned s_epoch=1,s_clear_epoch;
static _Atomic(SemaphoreHandle_t) s_cache_mutex;
static atomic_uint s_revision;
static atomic_bool s_selected,s_ready,s_running,s_exporting;
static atomic_uint s_jobs,s_cache_loads,s_installs;
static atomic_uint s_start_delay=1;
static atomic_int_fast64_t s_start_after;
static _Atomic(const char *) s_status="Not installed";
static uint8_t *s_upload;
static size_t s_upload_size,s_upload_offset;
static uint8_t s_upload_owner[32],s_upload_identity[32];
static unsigned s_upload_epoch;
static int64_t s_upload_deadline;
static atomic_flag s_upload_guard=ATOMIC_FLAG_INIT;
static void upload_lock(void) {while(atomic_flag_test_and_set(&s_upload_guard))vTaskDelay(1);}
static void upload_unlock(void) {atomic_flag_clear(&s_upload_guard);}
static void kick(void);
static void upload_release(void) {
    heap_caps_free(s_upload);s_upload=NULL;s_upload_size=s_upload_offset=0;
}
static bool busy(void) {
    muse_mode_t mode=muse_state_mode(NULL);
    return muse_voice_in_call() || mode==MUSE_MODE_LISTENING || mode==MUSE_MODE_SPEAKING || mode==MUSE_MODE_THINKING;
}
/* Only tasks performing persistence take this mutex. UI acquire/release and
 * invalidation use s_guard briefly, never wait for filesystem I/O. The sole
 * nested order is upload_guard -> cache_mutex -> s_guard; the worker never
 * takes upload_guard while holding cache_mutex. */
static bool cache_lock(void) {
    SemaphoreHandle_t mutex=atomic_load(&s_cache_mutex);
    if(!mutex) {
        SemaphoreHandle_t fresh=xSemaphoreCreateMutex();
        if(!fresh)return false;
        SemaphoreHandle_t expected=NULL;
        if(atomic_compare_exchange_strong(&s_cache_mutex,&expected,fresh))mutex=fresh;
        else {vSemaphoreDelete(fresh);mutex=expected;}
    }
    return xSemaphoreTake(mutex,pdMS_TO_TICKS(CACHE_WAIT_MS))==pdTRUE;
}
static void cache_unlock(void) {xSemaphoreGive(atomic_load(&s_cache_mutex));}
/* Stored pairing and explicit endpoint/VM settings define ownership. No live
 * VM token, resolved VM state, or cloud request participates in this digest.
 * Allocation/crypto failure is UNKNOWN, not proof that pairing was erased. */
/* A journal's presence (including lookup failure) means the raw keys may be
 * provisional or only partly rolled back. Do not invoke the committed token
 * getters here: those may apply a legacy journal and write flash recursively.
 * Both journal marker set/erase paths invalidate our epoch after NVS closes. */
static bool owner_journals_closed(void) {
    return config_key_lookup("rp_open")==CONFIG_KEY_NOT_FOUND &&
        config_key_lookup("tok_update")==CONFIG_KEY_NOT_FOUND;
}
static owner_result_t owner_key_read(uint8_t out[32]) {
    if(!owner_journals_closed())return OWNER_UNAVAILABLE;
    if(!config_is_provisioned() || !config_setup_complete())
        return owner_journals_closed()?OWNER_INVALID:OWNER_UNAVAILABLE;
    char *value=heap_caps_calloc(1,OWNER_TEXT_MAX,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!value)return OWNER_UNAVAILABLE;
    mbedtls_md_context_t md;mbedtls_md_init(&md);
    bool ok=!mbedtls_md_setup(&md,mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),0) && !mbedtls_md_starts(&md);
    const char domain[]="muse-pixel-owner-v1";
    if(ok)ok=!mbedtls_md_update(&md,(const uint8_t *)domain,sizeof(domain));
    const char *keys[]={"refresh_token","node_id","noise_host","vm_url"};
    for(unsigned i=0;ok && i<4;i++) {
        value[0]=0;bool found=config_get_str(keys[i],value,OWNER_TEXT_MAX);
        if(i<2 && (!found || !value[0])) {ok=false;break;}
        ok=!mbedtls_md_update(&md,(const uint8_t *)value,strlen(value)+1);
    }
    if(ok) {muse_settings_hatch_host(value);ok=!mbedtls_md_update(&md,(const uint8_t *)value,strlen(value)+1);}
    if(ok) {muse_settings_hatch_vm(value);ok=!mbedtls_md_update(&md,(const uint8_t *)value,strlen(value)+1);}
    if(ok) {muse_settings_hatch_token(value);ok=!mbedtls_md_update(&md,(const uint8_t *)value,strlen(value)+1);}
    if(ok)ok=!mbedtls_md_finish(&md,out);
    memset(value,0,OWNER_TEXT_MAX);heap_caps_free(value);mbedtls_md_free(&md);
    /* The worker also rejects an epoch change, so even a journal that opened
     * and closed entirely within this probe cannot publish its old snapshot. */
    return ok && owner_journals_closed()?OWNER_VALID:OWNER_UNAVAILABLE;
}
static void release_model(model_t *model) {
    if(!model)return;
    portENTER_CRITICAL(&s_guard);bool last=--model->refs==0;portEXIT_CRITICAL(&s_guard);
    if(last) {heap_caps_free((void *)model->view.pack.data);heap_caps_free(model);}
}
void muse_pixel_avatar_release(const muse_pixel_avatar_t *view) {release_model((model_t *)view);}
/* s_guard held; caller releases the displaced ownership reference outside it. */
static model_t *replace_locked(model_t *model) {model_t *old=s_model;s_model=model;return old;}
static bool matches_locked(const model_t *model) {
    return !s_clear_pending && s_have_owner && model && !memcmp(model->owner,s_owner,32) &&
        (!s_identity_required || (s_have_identity && !memcmp(model->view.pack.source_key,s_identity,32)));
}
static bool context_locked(unsigned epoch,const uint8_t owner[32]) {
    return epoch==s_epoch && !s_clear_pending && s_have_owner && !memcmp(owner,s_owner,32) &&
        muse_avatar_source()==MUSE_AVATAR_PERSONAL;
}
/* Cache mutex is held, but invalidation can revoke visibility immediately.
 * Validation and pointer replacement are one operation under s_guard. */
static bool publish_current(model_t *model,unsigned epoch,bool install) {
    portENTER_CRITICAL(&s_guard);
    bool ok=context_locked(epoch,model->owner) && matches_locked(model) && atomic_load(&s_selected);
    model_t *old=NULL;
    if(ok) {old=replace_locked(model);if(install)s_epoch++;}
    portEXIT_CRITICAL(&s_guard);
    release_model(old);return ok;
}
const muse_pixel_avatar_t *muse_pixel_avatar_acquire(void) {
    if(!atomic_load(&s_selected) || muse_avatar_source()!=MUSE_AVATAR_PERSONAL)return NULL;
    portENTER_CRITICAL(&s_guard);
    model_t *model=atomic_load(&s_selected) && muse_avatar_source()==MUSE_AVATAR_PERSONAL && matches_locked(s_model)?s_model:NULL;
    if(model)model->refs++;
    portEXIT_CRITICAL(&s_guard);return model?&model->view:NULL;
}
static model_t *new_model(uint8_t *bytes,size_t size,const uint8_t owner[32]) {
    muse_pixel_pack_t parsed;
    if(!muse_pixel_pack_parse(bytes,size,&parsed))return NULL;
    model_t *model=heap_caps_calloc(1,sizeof(*model),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!model)return NULL;
    model->view.pack=parsed;model->view.revision=atomic_fetch_add(&s_revision,1)+1;model->refs=1;memcpy(model->owner,owner,32);return model;
}
static void put32(uint8_t *p,uint32_t v) {for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(v>>(8*i));}
static uint32_t get32(const uint8_t *p) {return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
/* All callers hold cache_mutex: an open old inode can never outlive a newer
 * commit and subsequently republish its model. */
static model_t *cache_load_locked(const uint8_t owner[32]) {
    if(!muse_avatar_files_mount())return NULL;
    FILE *f=fopen(PIXEL_FILE,"rb");if(!f)return NULL;
    uint8_t header[CACHE_HEADER];
    bool ok=fread(header,1,sizeof(header),f)==sizeof(header) && !memcmp(header,"MSPCACH1",8) && !memcmp(header+8,owner,32);
    size_t size=ok?get32(header+40):0;
    ok=ok && size>=64 && size<=MUSE_PIXEL_PACK_MAX && heap_caps_get_free_size(MALLOC_CAP_SPIRAM)>=size+RESERVE;
    uint8_t *bytes=ok?heap_caps_malloc(size,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT):NULL;
    ok=bytes && fread(bytes,1,size,f)==size && fgetc(f)==EOF && !ferror(f) && get32(header+44)==muse_pixel_pack_crc(bytes,size);
    fclose(f);model_t *model=ok?new_model(bytes,size,owner):NULL;
    if(!model)heap_caps_free(bytes);
    return model;
}
/* Prepare bytes first. The final generation check is the acceptance boundary
 * of a successful atomic rename. A revocation BEFORE acceptance cancels and
 * preserves pixel.dat. A revocation AFTER acceptance is ordered after that
 * commit: it hides the model, but must not retroactively report "failed" after
 * a successful durable replacement. No post-rename failure is fabricated.
 * Clear/restore/other commits are serialized by cache_mutex. */
static bool cache_save_locked(const uint8_t *bytes,size_t size,const uint8_t owner[32],unsigned epoch) {
    if(!muse_avatar_files_mount() || !muse_avatar_files_space(size+CACHE_HEADER+64*1024))return false;
    mkdir("/files/.muse-avatar-v1",0700);
    uint8_t header[CACHE_HEADER]={0};memcpy(header,"MSPCACH1",8);memcpy(header+8,owner,32);
    put32(header+40,(uint32_t)size);put32(header+44,muse_pixel_pack_crc(bytes,size));
    FILE *f=fopen(PIXEL_TMP,"wb");if(!f)return false;
    bool ok=fwrite(header,1,sizeof(header),f)==sizeof(header) && fwrite(bytes,1,size,f)==size && !fflush(f) && !fsync(fileno(f));
    if(fclose(f))ok=false;
    bool idle=!busy();
    portENTER_CRITICAL(&s_guard);
    bool accepted=idle && context_locked(epoch,owner) && s_have_identity && !memcmp(bytes+24,s_identity,32);
    portEXIT_CRITICAL(&s_guard);
    ok=ok && accepted && !rename(PIXEL_TMP,PIXEL_FILE);
    if(!ok)unlink(PIXEL_TMP);
    return ok;
}
void muse_pixel_avatar_identity(const uint8_t source[32]) {
    /* NULL means the still fallback is temporarily absent. Only explicit owner
     * invalidation/clear may erase authoritative generation knowledge. */
    if(!source)return;
    portENTER_CRITICAL(&s_guard);
    bool changed=!s_have_identity || memcmp(source,s_identity,32);
    model_t *old=NULL;
    if(changed) {
        s_identity_required=true;s_have_identity=true;memcpy(s_identity,source,32);s_epoch++;
        if(!matches_locked(s_model))old=replace_locked(NULL);
    }
    portEXIT_CRITICAL(&s_guard);release_model(old);
    if(changed) {atomic_fetch_or(&s_jobs,JOB_RELOAD);kick();}
}
void muse_pixel_avatar_identity_unavailable(void) {
    /* Validated online metadata with an uncomputable generation is different
     * from boot without metadata: do not reopen any owner-only cached pack. */
    portENTER_CRITICAL(&s_guard);
    bool changed=!s_identity_required || s_have_identity;
    s_identity_required=true;s_have_identity=false;
    model_t *old=replace_locked(NULL);if(changed)s_epoch++;
    portEXIT_CRITICAL(&s_guard);release_model(old);
    if(changed) {atomic_fetch_or(&s_jobs,JOB_RELOAD);kick();}
}
void muse_pixel_avatar_journal_changed(void) {
    /* A journal transition revokes snapshots/uploads, but is not a committed
     * owner change. Keep the prior owner digest/identity so a rollback cannot
     * erase an authoritative online mismatch and reopen its stale cache. */
    portENTER_CRITICAL(&s_guard);
    s_have_owner=false;s_epoch++;model_t *old=replace_locked(NULL);
    portEXIT_CRITICAL(&s_guard);release_model(old);
    atomic_fetch_or(&s_jobs,JOB_RELOAD);kick();
}
void muse_pixel_avatar_invalidate(void) {
    portENTER_CRITICAL(&s_guard);
    s_have_owner=false;s_owner_known=false;s_have_identity=false;s_identity_required=false;s_epoch++;model_t *old=replace_locked(NULL);
    portEXIT_CRITICAL(&s_guard);release_model(old);
    atomic_fetch_or(&s_jobs,JOB_RELOAD);kick();
}
void muse_pixel_avatar_source_changed(void) {
    portENTER_CRITICAL(&s_guard);s_epoch++;model_t *old=replace_locked(NULL);portEXIT_CRITICAL(&s_guard);
    release_model(old);atomic_fetch_or(&s_jobs,JOB_RELOAD);kick();
}
void muse_pixel_avatar_clear(void) {
    portENTER_CRITICAL(&s_guard);
    s_clear_pending=true;s_have_identity=false;s_identity_required=false;s_clear_epoch=++s_epoch;model_t *old=replace_locked(NULL);
    atomic_fetch_or(&s_jobs,JOB_CLEAR|JOB_RELOAD);
    portEXIT_CRITICAL(&s_guard);release_model(old);kick();
}
static bool remove_cache_file(const char *path) {return !unlink(path) || errno==ENOENT;}
/* A single production iteration is also exercised by a fake scheduler in host
 * tests. It is not a test-only implementation or an alternate restore path. */
static void worker_step(worker_state_t *state) {
    portENTER_CRITICAL(&s_guard);
    unsigned jobs=atomic_exchange(&s_jobs,0),probe_epoch=s_epoch,clear_epoch=s_clear_epoch;
    portEXIT_CRITICAL(&s_guard);
    if(jobs&JOB_RELOAD) {state->next=0;state->delay_s=1;}
    uint8_t owner[32];owner_result_t have=owner_key_read(owner);
    if(!cache_lock()) {atomic_fetch_or(&s_jobs,jobs);atomic_store(&s_status,"Storage busy");return;}
    if(jobs&JOB_CLEAR) {
        bool cleared=muse_avatar_files_mount() && remove_cache_file(PIXEL_FILE) && remove_cache_file(PIXEL_TMP);
        if(!cleared) {
            atomic_fetch_or(&s_jobs,JOB_CLEAR);atomic_store(&s_status,"Storage unavailable");cache_unlock();return;
        }
        portENTER_CRITICAL(&s_guard);
        if(clear_epoch==s_clear_epoch)s_clear_pending=false;
        portEXIT_CRITICAL(&s_guard);
    }
    portENTER_CRITICAL(&s_guard);
    bool valid_probe=probe_epoch==s_epoch && !s_clear_pending;
    bool changed=false;model_t *old=NULL;
    if(valid_probe && have!=OWNER_UNAVAILABLE) {
        bool valid=have==OWNER_VALID;
        changed=valid!=s_have_owner || (valid && memcmp(owner,s_owner,32));
        if(changed) {
            if(!valid || (s_owner_known && memcmp(owner,s_owner,32))) {
                s_have_identity=false;s_identity_required=false;
            }
            s_have_owner=valid;s_owner_known=valid;
            if(valid)memcpy(s_owner,owner,32);
            s_epoch++;old=replace_locked(NULL);
        }
    }
    unsigned epoch=s_epoch;
    bool loaded=matches_locked(s_model);
    bool can_load=s_have_owner && !s_clear_pending && atomic_load(&s_selected) && muse_avatar_source()==MUSE_AVATAR_PERSONAL;
    if(!atomic_load(&s_selected) || muse_avatar_source()!=MUSE_AVATAR_PERSONAL) {
        if(!old)old=replace_locked(NULL);
        loaded=false;
    }
    portEXIT_CRITICAL(&s_guard);release_model(old);
    if(!valid_probe || have==OWNER_UNAVAILABLE) {
        atomic_store(&s_status,valid_probe?"Memory pressure":"Owner changed");cache_unlock();return;
    }
    if(changed) {state->next=0;state->delay_s=1;}
    if(can_load && !loaded && !busy() && esp_timer_get_time()>=state->next) {
        model_t *model=cache_load_locked(owner);
        if(model) {
            if(publish_current(model,epoch,false)) {atomic_fetch_add(&s_cache_loads,1);loaded=true;}
            else release_model(model);
        }
        state->next=esp_timer_get_time()+(int64_t)state->delay_s*1000000;
        if(state->delay_s<30)state->delay_s=state->delay_s<15?state->delay_s*2:30;
    }
    atomic_store(&s_status,have==OWNER_INVALID?"Waiting for pairing":loaded?"Ready":"No matching pixel pack");
    cache_unlock();
}
static bool current_upload(void) {
    portENTER_CRITICAL(&s_guard);
    bool ok=context_locked(s_upload_epoch,s_upload_owner) && s_have_identity && !memcmp(s_identity,s_upload_identity,32);
    portEXIT_CRITICAL(&s_guard);return ok;
}
static void expire_upload(void) {
    if(!atomic_flag_test_and_set(&s_upload_guard)) {
        if(s_upload && (esp_timer_get_time()>s_upload_deadline || !current_upload()))upload_release();
        upload_unlock();
    }
}
static void worker(void *arg) {
    (void)arg;worker_state_t state={1,0};
    for(;;) {
        worker_step(&state);
        expire_upload();
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
static void kick(void) {
    if(!atomic_load(&s_ready) || esp_timer_get_time()<atomic_load(&s_start_after))return;
    bool expected=false;
    if(!atomic_compare_exchange_strong(&s_running,&expected,true))return;
    if(xTaskCreateWithCaps(worker,"pixel_avatar",6144,NULL,2,NULL,MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)!=pdPASS) {
        unsigned delay=atomic_load(&s_start_delay);
        atomic_store(&s_start_after,esp_timer_get_time()+(int64_t)delay*1000000);
        atomic_store(&s_start_delay,delay<15?delay*2:30);
        atomic_store(&s_running,false);atomic_store(&s_status,"No memory");
    } else {atomic_store(&s_start_delay,1);atomic_store(&s_start_after,0);}
}
void muse_pixel_avatar_poll(void) {kick();}
void muse_pixel_avatar_start(void) {
    char selected[8]={0};config_get_str("pixel_style",selected,sizeof(selected));
    atomic_store(&s_selected,!strcmp(selected,"1"));atomic_store(&s_ready,true);kick();
}
static bool parse_hex(const char *text,uint8_t out[32]) {
    if(strlen(text)!=64)return false;
    for(unsigned i=0;i<32;i++) {
        unsigned v=0;
        for(unsigned j=0;j<2;j++) {
            char c=text[2*i+j];unsigned n=c>='0'&&c<='9'?(unsigned)(c-'0'):c>='a'&&c<='f'?(unsigned)(c-'a'+10):c>='A'&&c<='F'?(unsigned)(c-'A'+10):16;
            if(n>15)return false;
            v=v*16+n;
        }
        out[i]=(uint8_t)v;
    }
    return true;
}
static void status(void) {
    portENTER_CRITICAL(&s_guard);model_t *m=s_model;bool matching=matches_locked(m);
    unsigned width=m?m->view.pack.width:0,height=m?m->view.pack.height:0,colors=m?m->view.pack.colors:0;
    unsigned bytes=m?(unsigned)m->view.pack.size:0,revision=m?m->view.revision:0;portEXIT_CRITICAL(&s_guard);
    upload_lock();unsigned uploading=(unsigned)s_upload_size;upload_unlock();
    printf("@pixel {\"enabled\":%s,\"matching\":%s,\"status\":\"%s\",\"width\":%u,\"height\":%u,\"colors\":%u,\"bytes\":%u,\"revision\":%u,\"cache_loads\":%u,\"installs\":%u,\"upload_bytes\":%u}\n",
        atomic_load(&s_selected)?"true":"false",matching?"true":"false",atomic_load(&s_status),width,height,colors,bytes,revision,atomic_load(&s_cache_loads),atomic_load(&s_installs),uploading);
}
static bool begin(size_t size,const uint8_t source[32]) {
    upload_lock();upload_release();
    portENTER_CRITICAL(&s_guard);
    bool ok=!s_clear_pending && s_have_owner && s_have_identity && muse_avatar_source()==MUSE_AVATAR_PERSONAL && !memcmp(source,s_identity,32);
    if(ok) {s_upload_epoch=s_epoch;memcpy(s_upload_owner,s_owner,32);memcpy(s_upload_identity,s_identity,32);}
    portEXIT_CRITICAL(&s_guard);
    ok=ok && size>=64 && size<=MUSE_PIXEL_PACK_MAX && !busy() && muse_avatar_files_mount() &&
        muse_avatar_files_space(size+CACHE_HEADER+64*1024) && heap_caps_get_free_size(MALLOC_CAP_SPIRAM)>size+RESERVE;
    if(ok)s_upload=heap_caps_malloc(size,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    ok=ok && s_upload;
    if(ok) {s_upload_size=size;s_upload_deadline=esp_timer_get_time()+30000000;}
    upload_unlock();return ok;
}
static bool commit(void) {
    upload_lock();muse_pixel_pack_t pack;
    bool ok=s_upload && s_upload_offset==s_upload_size && current_upload() && !busy() &&
        muse_pixel_pack_parse(s_upload,s_upload_size,&pack) && !memcmp(pack.source_key,s_upload_identity,32);
    model_t *model=ok?new_model(s_upload,s_upload_size,s_upload_owner):NULL;
    ok=model && cache_lock();
    if(ok) {
        ok=cache_save_locked(s_upload,s_upload_size,s_upload_owner,s_upload_epoch);
        if(ok) {
            /* Accepted durable replacement owns the upload bytes. Late owner
             * revocation can prevent publication, never fabricate disk failure. */
            s_upload=NULL;s_upload_size=s_upload_offset=0;
            if(!publish_current(model,s_upload_epoch,true))release_model(model);
            atomic_fetch_add(&s_installs,1);atomic_store(&s_status,"Installed");
        }
        cache_unlock();
    }
    if(!ok) {
        if(model) {model->view.pack.data=NULL;heap_caps_free(model);}
        upload_release();
    }
    upload_unlock();return ok;
}
static bool export_current(unsigned epoch,const uint8_t key[32]) {
    portENTER_CRITICAL(&s_guard);
    bool ok=epoch==s_epoch && !s_clear_pending && s_have_owner && s_have_identity && !memcmp(key,s_identity,32) &&
        muse_avatar_source()==MUSE_AVATAR_PERSONAL;
    portEXIT_CRITICAL(&s_guard);return ok;
}
static void export_image(void *arg) {
    (void)arg;
    portENTER_CRITICAL(&s_guard);unsigned epoch=s_epoch;portEXIT_CRITICAL(&s_guard);
    const muse_avatar_image_t *image=muse_avatar_acquire();uint8_t identity[32];
    bool ok=image && image->data.pixels && muse_avatar_copy_image_key(image,identity) && export_current(epoch,identity) &&
        image->data.width>=1 && image->data.height>=1 && image->data.width<=MUSE_AVATAR_MAX_EDGE && image->data.height<=MUSE_AVATAR_MAX_EDGE;
    if(!ok)printf("@pixel_export_error unavailable\n");
    else {
        size_t rgb_size=(size_t)image->data.width*image->data.height*2;
        const uint8_t *bytes=(const uint8_t *)image->data.pixels,*available_alpha=NULL;
#if CONFIG_MUSE_AVATAR_WHITE_MATTE
        available_alpha=muse_avatar_image_alpha(image);
#endif
        const uint8_t *alpha=(uintptr_t)arg?available_alpha:NULL;
        size_t alpha_size=alpha?rgb_size/2:0,size=rgb_size+alpha_size;
        uint32_t crc=muse_pixel_pack_crc(bytes,rgb_size);
        if(alpha)crc=muse_pixel_crc_append(crc,alpha,alpha_size);
        char key[65];for(unsigned i=0;i<32;i++)snprintf(key+2*i,3,"%02x",identity[i]);
        printf("@pixel_image {\"width\":%d,\"height\":%d,\"bytes\":%u,\"alpha_bytes\":%u,\"alpha_available\":%s,\"crc\":%u,\"source_key\":\"%s\"}\n",image->data.width,image->data.height,(unsigned)size,(unsigned)alpha_size,available_alpha?"true":"false",(unsigned)crc,key);
        for(size_t off=0;off<size;off+=144) {
            if(!export_current(epoch,identity)) {ok=false;break;}
            size_t n=size-off;if(n>144)n=144;uint8_t chunk[144];size_t copied=0;
            if(off<rgb_size) {copied=rgb_size-off;if(copied>n)copied=n;memcpy(chunk,bytes+off,copied);}
            if(copied<n)memcpy(chunk+copied,alpha+(off+copied-rgb_size),n-copied);
            unsigned char text[193];size_t length=0;
            if(mbedtls_base64_encode(text,sizeof(text),&length,chunk,n)) {ok=false;break;}
            printf("@pixel_bytes %.*s\n",(int)length,text);
        }
        if(ok && export_current(epoch,identity))printf("@pixel_end\n");
        else printf("@pixel_export_error context_changed\n");
    }
    muse_avatar_release(image);fflush(stdout);atomic_store(&s_exporting,false);vTaskDeleteWithCaps(NULL);
}
#if CONFIG_LV_USE_SNAPSHOT
bool muse_pixel_avatar_test_boot_offline(void) {
    char off[2]={0};
    return config_get_str("pix_test_off",off,sizeof(off)) && off[0]=='1';
}
#endif
bool muse_pixel_avatar_console(const char *line) {
    if(!strcmp(line,"pixel")) {status();return true;}
#if CONFIG_LV_USE_SNAPSHOT
    if(!strcmp(line,"pixel.test_wifi=0") || !strcmp(line,"pixel.test_wifi=1")) {
        bool on=line[16]=='1';char saved[2]={0};const char *value=on?"0":"1";
        bool ok=config_set_str("pix_test_off",value) &&
            config_get_str("pix_test_off",saved,sizeof(saved)) && !strcmp(saved,value);
        if(ok)muse_settings_set_wifi_on(on);
        printf("@pixel_test_wifi %s\n",ok?"ok":"error");return true;
    }
#endif
    if(!strcmp(line,"pixel.on") || !strcmp(line,"pixel.off")) {
        const char *value=!strcmp(line,"pixel.on")?"1":"0";char saved[8]={0};
        bool ok=config_set_str("pixel_style",value) && config_get_str("pixel_style",saved,sizeof(saved)) && !strcmp(saved,value);
        if(ok) {
            bool selected=value[0]=='1';
            bool changed=atomic_exchange(&s_selected,selected)!=selected;
            if(changed)muse_pixel_avatar_source_changed();else kick();
            muse_state_set_asleep(false);muse_state_poke();
        }
        printf("@pixel_setting %s\n",ok?"ok":"error");return true;
    }
    if(!strcmp(line,"pixel.abort")) {upload_lock();upload_release();upload_unlock();printf("@pixel_install ok\n");return true;}
    if(!strcmp(line,"pixel.commit")) {printf("@pixel_install %s\n",commit()?"ok":"error");return true;}
    if(!strncmp(line,"pixel.begin=",12)) {
        char *end;unsigned long size=strtoul(line+12,&end,10);uint8_t source[32];
        bool ok=end!=line+12 && *end==',' && size<=MUSE_PIXEL_PACK_MAX && parse_hex(end+1,source) && begin((size_t)size,source);
        printf("@pixel_install %s\n",ok?"ok":"error");return true;
    }
    if(!strncmp(line,"pixel.chunk=",12)) {
        char *end;unsigned long offset=strtoul(line+12,&end,10);uint8_t bytes[600];size_t size=0;
        bool ok=end!=line+12 && *end==':' && offset<=MUSE_PIXEL_PACK_MAX && strlen(end+1)<=800 &&
            !mbedtls_base64_decode(bytes,sizeof(bytes),&size,(const uint8_t *)end+1,strlen(end+1));
        upload_lock();ok=ok && s_upload && offset==s_upload_offset && size && size<=s_upload_size-s_upload_offset && current_upload();
        if(ok) {memcpy(s_upload+offset,bytes,size);s_upload_offset+=size;s_upload_deadline=esp_timer_get_time()+30000000;}
        upload_unlock();printf("@pixel_install %s\n",ok?"ok":"error");return true;
    }
    if(!strcmp(line,"pixel.assets")) {
        portENTER_CRITICAL(&s_guard);bool have=s_have_owner && s_have_identity && !s_clear_pending;uint8_t key[32];if(have)memcpy(key,s_identity,32);portEXIT_CRITICAL(&s_guard);
        if(have && muse_avatar_source()==MUSE_AVATAR_PERSONAL) {char hex[65];for(unsigned i=0;i<32;i++)snprintf(hex+2*i,3,"%02x",key[i]);printf("@pixel_assets {\"source_key\":\"%s\"}\n",hex);}
        else printf("@pixel_export_error identity_unavailable\n");
        return true;
    }
    if(!strcmp(line,"pixel.image") || !strcmp(line,"pixel.image_alpha")) {
        bool with_alpha=!strcmp(line,"pixel.image_alpha");
        bool expected=false;
        if(!atomic_compare_exchange_strong(&s_exporting,&expected,true)) {printf("@pixel_export_error busy\n");return true;}
        muse_state_set_asleep(false);muse_state_poke();
        if(xTaskCreateWithCaps(export_image,"pixel_export",4096,(void *)(uintptr_t)with_alpha,2,NULL,MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)!=pdPASS) {
            atomic_store(&s_exporting,false);printf("@pixel_export_error memory\n");
        }
        return true;
    }
    return false;
}
#endif
