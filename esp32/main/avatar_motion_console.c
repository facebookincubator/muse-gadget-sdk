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
#include "avatar_motion_console.h"
#include "avatar_sync.h"
#include "avatar_request.h"
#include "avatar_clip_refs.h"
#include "muse_avatar_motion.h"
#include "muse_state.h"
#include "muse_voice.h"
#include "noise_control.h"
#include "vm_file_path.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "mbedtls/base64.h"
#include "mbedtls/md.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static atomic_bool s_busy;
static avatar_clip_refs_t *s_refs;
static uint8_t s_key[32];
static int s_kind;
static unsigned s_offset;
static void *alloc(size_t n) {return heap_caps_malloc(n,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);}
static bool stop(uintptr_t unused) {
    (void)unused;
    muse_mode_t mode=muse_state_mode(NULL);
    return !noise_ctrl_is_connected() || muse_state_asleep() || muse_voice_in_call() ||
        mode==MUSE_MODE_LISTENING || mode==MUSE_MODE_SPEAKING || mode==MUSE_MODE_THINKING;
}
static void yield(void) {vTaskDelay(pdMS_TO_TICKS(20));}
static const avatar_request_ops_t ops={noise_ctrl_req_open,noise_ctrl_req_cancel,alloc,heap_caps_free,esp_timer_get_time,yield,stop};
static bool source_key(const avatar_identity_t *id,uint8_t key[32]) {
    mbedtls_md_context_t ctx;mbedtls_md_init(&ctx);
    bool ok=!mbedtls_md_setup(&ctx,mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),0) && !mbedtls_md_starts(&ctx) &&
        !mbedtls_md_update(&ctx,(const uint8_t *)id->path,strlen(id->path)+1) &&
        !mbedtls_md_update(&ctx,(const uint8_t *)id->revision,strlen(id->revision)) && !mbedtls_md_finish(&ctx,key);
    mbedtls_md_free(&ctx);return ok;
}
static bool same_source(void) {
    uint8_t key[32],binding[32];
    return avatar_sync_motion_keys(key,binding) && !memcmp(key,s_key,32);
}
static void export_task(void *arg) {
    (void)arg;
    const char *error="unavailable"; char detail[96];
    if(s_kind==-2) {
        const muse_avatar_image_t *image=muse_avatar_motion_acquire();
        if(image && image->data.width>0 && image->data.width<=240 && image->data.height>0 && image->data.height<=240) {
            const uint8_t *bytes=(const uint8_t *)image->data.pixels;
            size_t size=(size_t)image->data.width*image->data.height*2;
            printf("@avatar_frame {\"width\":%d,\"height\":%d,\"revision\":%u,\"bytes\":%u,\"crc\":%u}\n",
                image->data.width,image->data.height,(unsigned)image->revision,(unsigned)size,(unsigned)muse_avatar_pack_crc(bytes,size));
            for(size_t off=0;off<size;off+=144) {
                size_t n=size-off;if(n>144)n=144;unsigned char encoded[193];size_t length;
                if(mbedtls_base64_encode(encoded,sizeof(encoded),&length,bytes+off,n))break;
                printf("@avatar_bytes %.*s\n",(int)length,encoded);
            }
            printf("@avatar_end\n");error=NULL;
        }
        muse_avatar_release(image);
    } else if(s_kind<0) {
        if(!s_refs)s_refs=alloc(sizeof(*s_refs));
        avatar_response_t *r=avatar_request_get("/identity",NULL,8192,0,&ops);
        bool ok=s_refs && r && avatar_clip_refs_parse((const char *)r->body,r->len,s_refs);
        if(!ok) {
            snprintf(detail,sizeof(detail),"metadata_status_%d_len_%u_mode_%d_sleep_%d_call_%d_online_%d",r?r->status:0,r?(unsigned)r->len:0,
                (int)muse_state_mode(NULL),(int)muse_state_asleep(),(int)muse_voice_in_call(),(int)noise_ctrl_is_connected());error=detail;
        }
        avatar_request_release(r);
        if(ok) {
            if(source_key(&s_refs->identity,s_key) && same_source()) {
                unsigned mask=0;char hex[65];
                for(unsigned i=0;i<MUSE_AVATAR_CLIPS;i++) if(s_refs->path[i][0])mask|=1u<<i;
                for(unsigned i=0;i<32;i++)sprintf(hex+i*2,"%02x",s_key[i]);
                printf("@avatar_assets {\"source\":\"%s\",\"coverage\":%u,\"path_classes\":[",hex,mask);
                for(unsigned i=0;i<MUSE_AVATAR_CLIPS;i++) {
                    const char *p=s_refs->path[i];
                    const char *kind=!p[0]?"missing":strstr(p,"://")?"url":p[0]=='/'?"absolute":p[0]=='~'?"home":strchr(p,'/')?"relative":"basename";
                    printf("%s\"%s\"",i?",":"",kind);
                }
                printf("]}\n");error=NULL;
            } else error="refresh_still_first";
        }
    } else if(s_refs && same_source() && s_kind<MUSE_AVATAR_CLIPS && s_refs->path[s_kind][0] && s_offset<2*1024*1024) {
        char range[64];snprintf(range,sizeof(range),"bytes=%u-%u",s_offset,s_offset+4095);
        const char *headers[]={"Range",range,NULL};
        char *url=vm_file_request_path(s_refs->path[s_kind]);
        avatar_response_t *r=url?avatar_request_get_status(url,headers,4096,0,&ops):NULL;free(url);
        if(r && same_source() && (r->status==206 || r->status==416 || (r->status==200 && !s_offset))) {
            size_t size=r->status==416?0:r->len;
            printf("@avatar_asset {\"kind\":%d,\"offset\":%u,\"status\":%d,\"len\":%u,\"crc\":%u}\n",s_kind,s_offset,r->status,(unsigned)size,(unsigned)muse_avatar_pack_crc(r->body,size));
            for(size_t off=0;off<size;off+=144) {
                size_t n=size-off;if(n>144)n=144;
                unsigned char encoded[193];size_t length;
                if(mbedtls_base64_encode(encoded,sizeof(encoded),&length,r->body+off,n))break;
                printf("@avatar_bytes %.*s\n",(int)length,encoded);
            }
            printf("@avatar_end\n");error=NULL;
        }
        if(error && r) {
            snprintf(detail,sizeof(detail),"http_%d_len_%u",r->status,(unsigned)r->len);error=detail;
        } else if(error) error="request_failed";
        avatar_request_release(r);
    } else error="stale_or_missing_asset";
    if(error)printf("@avatar_export_error %s mode_%d_sleep_%d_call_%d_online_%d\n",error,
        (int)muse_state_mode(NULL),(int)muse_state_asleep(),(int)muse_voice_in_call(),(int)noise_ctrl_is_connected());
    fflush(stdout);atomic_store(&s_busy,false);vTaskDeleteWithCaps(NULL);
}
bool avatar_motion_console(const char *line) {
    /* Explicit maintenance activity keeps the inactivity timer alive, but a
     * status poll never wakes the display or overrides deliberate sleep. */
    if(!strncmp(line,"avatar.motion.",14) || !strcmp(line,"avatar.assets") || !strncmp(line,"avatar.asset=",13)) muse_state_poke();
    if(!strcmp(line,"avatar.motion")) {muse_avatar_motion_status();return true;}
    if(!strcmp(line,"avatar.motion=0") || !strcmp(line,"avatar.motion=1")) {
        muse_avatar_motion_enable(line[14]=='1');muse_avatar_motion_status();return true;
    }
    if(!strncmp(line,"avatar.motion.preview=",22)) {
        char *end;unsigned long clip=strtoul(line+22,&end,10);
        bool ok=end!=line+22 && !*end && clip<MUSE_AVATAR_CLIPS && muse_avatar_motion_preview((unsigned)clip);
        printf("@avatar_preview %s\n",ok?"ok":"error");return true;
    }
    if(!strcmp(line,"avatar.motion.abort")) {muse_avatar_motion_install_abort();printf("@avatar_install ok\n");return true;}
    if(!strcmp(line,"avatar.motion.commit")) {printf("@avatar_install %s\n",muse_avatar_motion_install_commit()?"ok":"error");return true;}
    if(!strncmp(line,"avatar.motion.begin=",20)) {
        char *end;unsigned long size=strtoul(line+20,&end,10);
        bool ok=end!=line+20 && !*end && size<=MUSE_AVATAR_PACK_MAX && muse_avatar_motion_install_begin(size);
        printf("@avatar_install %s\n",ok?"ok":"error");return true;
    }
    if(!strncmp(line,"avatar.motion.chunk=",20)) {
        char *end;unsigned long offset=strtoul(line+20,&end,10);uint8_t bytes[600];size_t size;
        bool ok=end!=line+20 && *end==':' && offset<=MUSE_AVATAR_PACK_MAX &&
            strlen(end+1)<=800 && !mbedtls_base64_decode(bytes,sizeof(bytes),&size,(const uint8_t *)end+1,strlen(end+1)) &&
            muse_avatar_motion_install_chunk(offset,bytes,size);
        printf("@avatar_install %s\n",ok?"ok":"error");return true;
    }
    bool assets=!strcmp(line,"avatar.assets"),frame=!strcmp(line,"avatar.motion.frame");
    if(!assets && !frame && strncmp(line,"avatar.asset=",13))return false;
    bool expected=false;
    if(!atomic_compare_exchange_strong(&s_busy,&expected,true)) {printf("@avatar_export_error busy\n");return true;}
    s_kind=frame?-2:-1;s_offset=0;
    if(!assets && !frame) {
        unsigned kind,off;char tail;
        if(sscanf(line+13,"%u,%u%c",&kind,&off,&tail)!=2 || kind>=MUSE_AVATAR_CLIPS || off%4096 || off>=2*1024*1024) {
            atomic_store(&s_busy,false);printf("@avatar_export_error invalid\n");return true;
        }
        s_kind=(int)kind;s_offset=off;
    }
    if(xTaskCreateWithCaps(export_task,"avatar_export",6144,NULL,2,NULL,MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)!=pdPASS) {
        atomic_store(&s_busy,false);printf("@avatar_export_error memory\n");
    }
    return true;
}
