# Copyright (c) Meta Platforms, Inc. and affiliates.
# SPDX-License-Identifier: Apache-2.0
"""Production backend races with real pthread locks and private synthetic FS.

The digest is deliberately fake: these are lifecycle/transaction tests, not
SHA-256 evidence. No device, credentials, network, or private assets are used.
"""
import os
from pathlib import Path
import shlex
import shutil
import struct
import subprocess
import tempfile
import unittest
import zlib
from test_pixel_art import pack_bytes
from pixel_test_sanitizers import ASAN_OPTIONS
from test_muse_ptt_ui import LINK_GC

ROOT = Path(__file__).resolve().parents[1]
MUSE = ROOT / 'components/muse'
FAKES = r'''
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <pthread.h>
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define MALLOC_CAP_INTERNAL 4
#define pdPASS 1
#define pdTRUE 1
#define pdMS_TO_TICKS(n) (n)
#define portMUX_INITIALIZER_UNLOCKED PTHREAD_MUTEX_INITIALIZER
#define portENTER_CRITICAL(p) test_enter_guard(p)
#define portEXIT_CRITICAL(p) test_exit_guard(p)
typedef pthread_mutex_t portMUX_TYPE;
typedef struct test_semaphore *SemaphoreHandle_t;
typedef void *TaskHandle_t;
typedef enum {MUSE_MODE_BOOT,MUSE_MODE_IDLE,MUSE_MODE_LISTENING,MUSE_MODE_THINKING,MUSE_MODE_SPEAKING,MUSE_MODE_ERROR,MUSE_MODE_OFF} muse_mode_t;
void test_enter_guard(portMUX_TYPE *p);
void test_exit_guard(portMUX_TYPE *p);
SemaphoreHandle_t xSemaphoreCreateMutex(void);
void vSemaphoreDelete(SemaphoreHandle_t p);
int xSemaphoreTake(SemaphoreHandle_t p,unsigned timeout);
void xSemaphoreGive(SemaphoreHandle_t p);
void *heap_caps_malloc(size_t n,unsigned caps);
void *heap_caps_calloc(size_t count,size_t n,unsigned caps);
void heap_caps_free(void *p);
size_t heap_caps_get_free_size(unsigned caps);
int64_t esp_timer_get_time(void);
void vTaskDelay(unsigned ticks);
void vTaskDeleteWithCaps(TaskHandle_t task);
int xTaskCreateWithCaps(void (*fn)(void *),const char *name,unsigned stack,void *arg,unsigned priority,TaskHandle_t *task,unsigned caps);
bool config_is_provisioned(void);
bool config_setup_complete(void);
typedef enum {CONFIG_KEY_LOOKUP_ERROR=-1,CONFIG_KEY_NOT_FOUND=0,CONFIG_KEY_FOUND=1} config_key_lookup_t;
config_key_lookup_t config_key_lookup(const char *key);
bool config_get_str(const char *key,char *out,size_t cap);
bool config_set_str(const char *key,const char *value);
void muse_settings_hatch_host(char *out);
void muse_settings_hatch_vm(char *out);
void muse_settings_hatch_token(char *out);
void muse_settings_set_wifi_on(bool on);
muse_mode_t muse_state_mode(float *t);
bool muse_voice_in_call(void);
void muse_state_set_asleep(bool on);
void muse_state_poke(void);
#define MBEDTLS_MD_SHA256 1
typedef struct {uint32_t hash;} mbedtls_md_context_t;
void mbedtls_md_init(mbedtls_md_context_t *md);
void mbedtls_md_free(mbedtls_md_context_t *md);
const void *mbedtls_md_info_from_type(int type);
int mbedtls_md_setup(mbedtls_md_context_t *md,const void *info,int hmac);
int mbedtls_md_starts(mbedtls_md_context_t *md);
int mbedtls_md_update(mbedtls_md_context_t *md,const uint8_t *data,size_t size);
int mbedtls_md_finish(mbedtls_md_context_t *md,uint8_t out[32]);
int mbedtls_base64_encode(unsigned char *out,size_t cap,size_t *n,const uint8_t *input,size_t size);
int mbedtls_base64_decode(uint8_t *out,size_t cap,size_t *n,const uint8_t *input,size_t size);
size_t test_fread(void *p,size_t size,size_t n,FILE *f);
int test_fsync(int fd);
int test_rename(const char *a,const char *b);
int test_unlink(const char *p);
int test_printf(const char *format,...);
'''
HARNESS = r'''
#include <assert.h>
#include <errno.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#define fread test_fread
#define fsync test_fsync
#define rename test_rename
#define unlink test_unlink
#define printf test_printf
#include "backend.c"
#undef fread
#undef fsync
#undef rename
#undef unlink
#undef printf

enum {H_NONE,H_OWNER,H_READ,H_FSYNC,H_RENAME,H_KEY,H_CHUNK,H_UNLINK};
static pthread_mutex_t hook_mutex=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t hook_cond=PTHREAD_COND_INITIALIZER;
static int armed_hook,hit_hook;
static bool release_hook;
static atomic_int lock_attempts,allocations,fail_malloc,fail_owner,fail_model,fail_fsync,fail_rename,fail_mount,fail_start,fail_mutex;
static atomic_int start_attempts,image_refs,export_headers,export_ends,export_chunks,wifi_calls,config_writes;
static atomic_bool paired=true,setup=true,personal=true,style=true,wifi_on=true;
static atomic_int_fast64_t clock_us;
static _Thread_local unsigned guard_depth;
static _Thread_local bool one_worker;
static void (*queued_worker)(void *);
static uint8_t *asset_a,*asset_b;
static size_t asset_size;
static char last_control[512],last_header[512];
static uint16_t image_pixels[32*32];
static muse_avatar_image_t image_a={.data={image_pixels,32,32},.revision=1};
static muse_avatar_image_t image_b={.data={image_pixels,32,32},.revision=2};
static _Atomic(const muse_avatar_image_t *) canonical;
static uint8_t key_a[32],key_b[32];
static bool take_failure(atomic_int *value) {int n=atomic_load(value);while(n>0){if(atomic_compare_exchange_weak(value,&n,n-1))return true;}return false;}
static struct timespec deadline(void) {struct timespec t;assert(!clock_gettime(CLOCK_REALTIME,&t));t.tv_sec+=5;return t;}
static void checkpoint(int which) {
    assert(!guard_depth);
    pthread_mutex_lock(&hook_mutex);
    if(armed_hook==which) {
        armed_hook=H_NONE;hit_hook=which;pthread_cond_broadcast(&hook_cond);
        struct timespec until=deadline();
        while(!release_hook)assert(!pthread_cond_timedwait(&hook_cond,&hook_mutex,&until));
    }
    pthread_mutex_unlock(&hook_mutex);
}
static void arm(int which) {pthread_mutex_lock(&hook_mutex);armed_hook=which;hit_hook=0;release_hook=false;pthread_mutex_unlock(&hook_mutex);}
static void wait_hit(int which) {pthread_mutex_lock(&hook_mutex);struct timespec until=deadline();while(hit_hook!=which)assert(!pthread_cond_timedwait(&hook_cond,&hook_mutex,&until));pthread_mutex_unlock(&hook_mutex);}
static void unblock(void) {pthread_mutex_lock(&hook_mutex);release_hook=true;pthread_cond_broadcast(&hook_cond);pthread_mutex_unlock(&hook_mutex);}
static void wait_locks(int count) {pthread_mutex_lock(&hook_mutex);struct timespec until=deadline();while(atomic_load(&lock_attempts)<count)assert(!pthread_cond_timedwait(&hook_cond,&hook_mutex,&until));pthread_mutex_unlock(&hook_mutex);}
void test_enter_guard(portMUX_TYPE *p) {assert(!guard_depth);assert(!pthread_mutex_lock(p));guard_depth++;}
void test_exit_guard(portMUX_TYPE *p) {assert(guard_depth==1);guard_depth--;assert(!pthread_mutex_unlock(p));}
struct test_semaphore {pthread_mutex_t lock;};
SemaphoreHandle_t xSemaphoreCreateMutex(void) {assert(!guard_depth);if(take_failure(&fail_mutex))return NULL;SemaphoreHandle_t p=calloc(1,sizeof(*p));assert(p && !pthread_mutex_init(&p->lock,NULL));return p;}
void vSemaphoreDelete(SemaphoreHandle_t p) {assert(!guard_depth && !pthread_mutex_destroy(&p->lock));free(p);}
int xSemaphoreTake(SemaphoreHandle_t p,unsigned timeout) {
    assert(!guard_depth && timeout==5000);
    pthread_mutex_lock(&hook_mutex);atomic_fetch_add(&lock_attempts,1);pthread_cond_broadcast(&hook_cond);pthread_mutex_unlock(&hook_mutex);
    assert(!pthread_mutex_lock(&p->lock));return pdTRUE;
}
void xSemaphoreGive(SemaphoreHandle_t p) {assert(!guard_depth && !pthread_mutex_unlock(&p->lock));}
void *heap_caps_malloc(size_t n,unsigned caps) {assert(!guard_depth && (caps&MALLOC_CAP_8BIT));if(take_failure(&fail_malloc))return NULL;void *p=malloc(n);if(p)atomic_fetch_add(&allocations,1);return p;}
void *heap_caps_calloc(size_t count,size_t n,unsigned caps) {
    assert(!guard_depth && (caps&MALLOC_CAP_8BIT));
    if((n==4096 && take_failure(&fail_owner)) || (n==sizeof(model_t) && take_failure(&fail_model)))return NULL;
    void *p=calloc(count,n);if(p)atomic_fetch_add(&allocations,1);return p;
}
void heap_caps_free(void *p) {assert(!guard_depth);if(p){atomic_fetch_sub(&allocations,1);free(p);}}
size_t heap_caps_get_free_size(unsigned caps) {(void)caps;return 16*1024*1024;}
int64_t esp_timer_get_time(void) {return atomic_load(&clock_us);}
void vTaskDelay(unsigned n) {assert(!guard_depth);atomic_fetch_add(&clock_us,(int64_t)n*1000);if(one_worker && n>=1000)pthread_exit(NULL);sched_yield();}
void vTaskDeleteWithCaps(TaskHandle_t task) {assert(!task && !guard_depth);}
int xTaskCreateWithCaps(void (*fn)(void *),const char *name,unsigned stack,void *arg,unsigned priority,TaskHandle_t *task,unsigned caps) {
    assert(!guard_depth);(void)arg;(void)priority;(void)task;assert(caps&MALLOC_CAP_INTERNAL);
    if(!strcmp(name,"pixel_avatar")) {assert(stack==6144);atomic_fetch_add(&start_attempts,1);if(take_failure(&fail_start))return 0;queued_worker=fn;}
    return pdPASS;
}
bool config_is_provisioned(void) {return atomic_load(&paired);}
bool config_setup_complete(void) {return atomic_load(&setup);}
config_key_lookup_t config_key_lookup(const char *key) {(void)key;return CONFIG_KEY_NOT_FOUND;}
static atomic_bool test_offline;
static atomic_uint test_flag_writes;
bool config_get_str(const char *key,char *out,size_t cap) {
    assert(!guard_depth);
    const char *value=!strcmp(key,"refresh_token")?"synthetic-credential":!strcmp(key,"node_id")?"synthetic-node":!strcmp(key,"pixel_style")?(atomic_load(&style)?"1":"0"):!strcmp(key,"pix_test_off")?(atomic_load(&test_offline)?"1":"0"):"";
    assert(strlen(value)<cap);strcpy(out,value);return true;
}
bool config_set_str(const char *key,const char *value) {
    assert(!guard_depth);
    if(!strcmp(key,"pix_test_off")) {atomic_store(&test_offline,value[0]=='1');atomic_fetch_add(&test_flag_writes,1);return true;}
    assert(!strcmp(key,"pixel_style"));atomic_fetch_add(&config_writes,1);atomic_store(&style,value[0]=='1');return true;
}
void muse_settings_hatch_host(char *out) {strcpy(out,"synthetic-host");}
void muse_settings_hatch_vm(char *out) {out[0]=0;}
void muse_settings_hatch_token(char *out) {out[0]=0;}
void muse_settings_set_wifi_on(bool on) {assert(!guard_depth);atomic_store(&wifi_on,on);atomic_fetch_add(&wifi_calls,1);}
muse_mode_t muse_state_mode(float *t) {(void)t;return MUSE_MODE_IDLE;}
bool muse_voice_in_call(void) {return false;}
void muse_state_set_asleep(bool on) {(void)on;}
void muse_state_poke(void) {}
muse_avatar_source_t muse_avatar_source(void) {return atomic_load(&personal)?MUSE_AVATAR_PERSONAL:MUSE_AVATAR_BUILTIN;}
const muse_avatar_image_t *muse_avatar_acquire(void) {const muse_avatar_image_t *p=atomic_load(&canonical);if(p)atomic_fetch_add(&image_refs,1);return p;}
void muse_avatar_release(const muse_avatar_image_t *p) {if(p)atomic_fetch_sub(&image_refs,1);}
bool muse_avatar_copy_image_key(const muse_avatar_image_t *p,uint8_t out[32]) {checkpoint(H_KEY);if(p==&image_a)memcpy(out,key_a,32);else if(p==&image_b)memcpy(out,key_b,32);else return false;return true;}
bool muse_avatar_files_mount(void) {assert(!guard_depth);return !take_failure(&fail_mount);}
bool muse_avatar_files_space(size_t size) {assert(!guard_depth && size<200000);return true;}
/* Fake deterministic digest, deliberately NOT cryptographic validation. */
void mbedtls_md_init(mbedtls_md_context_t *md) {md->hash=1;}
void mbedtls_md_free(mbedtls_md_context_t *md) {(void)md;}
const void *mbedtls_md_info_from_type(int type) {assert(type==1);return (void *)1;}
int mbedtls_md_setup(mbedtls_md_context_t *md,const void *info,int hmac) {(void)md;(void)info;(void)hmac;return 0;}
int mbedtls_md_starts(mbedtls_md_context_t *md) {md->hash=1;return 0;}
int mbedtls_md_update(mbedtls_md_context_t *md,const uint8_t *data,size_t size) {while(size--)md->hash=md->hash*33+*data++;return 0;}
int mbedtls_md_finish(mbedtls_md_context_t *md,uint8_t out[32]) {for(unsigned i=0;i<32;i++)out[i]=(uint8_t)(md->hash>>(8*(i%4)));checkpoint(H_OWNER);return 0;}
static const char alphabet[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
int mbedtls_base64_encode(unsigned char *out,size_t cap,size_t *n,const uint8_t *input,size_t size) {
    size_t want=4*((size+2)/3);if(cap<want+1)return -1;size_t at=0;
    for(size_t i=0;i<size;i+=3) {unsigned v=(unsigned)input[i]<<16;if(i+1<size)v|=(unsigned)input[i+1]<<8;if(i+2<size)v|=input[i+2];out[at++]=alphabet[v>>18];out[at++]=alphabet[(v>>12)&63];out[at++]=i+1<size?alphabet[(v>>6)&63]:'=';out[at++]=i+2<size?alphabet[v&63]:'=';}
    out[at]=0;*n=at;return 0;
}
int mbedtls_base64_decode(uint8_t *out,size_t cap,size_t *n,const uint8_t *input,size_t size) {
    if (size%4) {
        return -1;
    }
    size_t at=0;
    for(size_t i=0;i<size;i+=4) {unsigned v=0,pad=0;for(unsigned j=0;j<4;j++){const char *p=strchr(alphabet,input[i+j]);if(input[i+j]=='='){v<<=6;pad++;}else if(p && !pad)v=(v<<6)|(unsigned)(p-alphabet);else return -1;}
        unsigned bytes=3-pad;if(pad>2 || at+bytes>cap)return -1;out[at++]=(uint8_t)(v>>16);if(bytes>1)out[at++]=(uint8_t)(v>>8);if(bytes>2)out[at++]=(uint8_t)v;
    }*n=at;return 0;
}
size_t test_fread(void *p,size_t size,size_t n,FILE *f) {assert(!guard_depth);size_t got=fread(p,size,n,f);if(n==asset_size)checkpoint(H_READ);return got;}
int test_fsync(int fd) {assert(!guard_depth);checkpoint(H_FSYNC);if(take_failure(&fail_fsync)){errno=EIO;return -1;}return fsync(fd);}
int test_rename(const char *a,const char *b) {assert(!guard_depth);checkpoint(H_RENAME);if(take_failure(&fail_rename)){errno=EIO;return -1;}return rename(a,b);}
int test_unlink(const char *p) {assert(!guard_depth);checkpoint(H_UNLINK);return unlink(p);}
int test_printf(const char *format,...) {
    assert(!guard_depth);char text[512];va_list ap;va_start(ap,format);int n=vsnprintf(text,sizeof(text),format,ap);va_end(ap);assert(n>=0 && (size_t)n<sizeof(text));
    if(!strncmp(text,"@pixel_image ",13)){strcpy(last_header,text);atomic_fetch_add(&export_headers,1);}
    if(!strncmp(text,"@pixel_bytes ",13)){atomic_fetch_add(&export_chunks,1);checkpoint(H_CHUNK);}
    if(!strcmp(text,"@pixel_end\n"))atomic_fetch_add(&export_ends,1);
    strcpy(last_control,text);return n;
}
static void read_asset(const char *path,uint8_t **out,size_t *size) {FILE *f=fopen(path,"rb");assert(f && !fseek(f,0,SEEK_END));long n=ftell(f);assert(n>64 && n<65536);rewind(f);*out=malloc((size_t)n);assert(*out && fread(*out,1,(size_t)n,f)==(size_t)n);fclose(f);*size=(size_t)n;}
static void disk_is(const uint8_t *wanted) {FILE *f=fopen(PIXEL_FILE,"rb");assert(f);uint8_t header[48];assert(fread(header,1,48,f)==48);uint8_t *b=malloc(asset_size);assert(b && fread(b,1,asset_size,f)==asset_size && fgetc(f)==EOF);assert(!memcmp(b,wanted,asset_size));free(b);fclose(f);}
static void live_is(const uint8_t *wanted) {const muse_pixel_avatar_t *p=muse_pixel_avatar_acquire();assert(p && p->pack.size==asset_size && !memcmp(p->pack.data,wanted,asset_size));muse_pixel_avatar_release(p);}
static void upload(const uint8_t *bytes) {
    char key[65],command[900];for(unsigned i=0;i<32;i++)snprintf(key+2*i,3,"%02x",bytes[24+i]);
    snprintf(command,sizeof(command),"pixel.begin=%u,%s",(unsigned)asset_size,key);assert(muse_pixel_avatar_console(command) && s_upload && s_upload_size==asset_size);
    for(size_t at=0;at<asset_size;at+=256) {size_t len=asset_size-at;if(len>256)len=256;unsigned char encoded[345];size_t n;
        assert(!mbedtls_base64_encode(encoded,sizeof(encoded),&n,bytes+at,len));snprintf(command,sizeof(command),"pixel.chunk=%u:%s",(unsigned)at,encoded);
        assert(muse_pixel_avatar_console(command) && s_upload_offset==at+len);
    }
}
static void seed(void) {muse_pixel_avatar_start();muse_pixel_avatar_identity(key_a);worker_state_t state={1,0};worker_step(&state);upload(asset_a);assert(commit());live_is(asset_a);disk_is(asset_a);}
static void drop_and_select(void) {assert(muse_pixel_avatar_console("pixel.off"));assert(!muse_pixel_avatar_acquire());assert(muse_pixel_avatar_console("pixel.on"));assert(!s_model);}
struct job {worker_state_t state;bool ok;atomic_bool done;};
static void *worker_job(void *p) {struct job *j=p;worker_step(&j->state);expire_upload();atomic_store(&j->done,true);return NULL;}
static void *commit_job(void *p) {struct job *j=p;j->ok=commit();atomic_store(&j->done,true);return NULL;}
static void *export_job(void *p) {(void)p;export_image(NULL);return NULL;}
static void *queued_job(void *p) {(void)p;one_worker=true;queued_worker(NULL);return NULL;}
static void cleanup(void) {
    upload_lock();upload_release();upload_unlock();portENTER_CRITICAL(&s_guard);model_t *old=replace_locked(NULL);portEXIT_CRITICAL(&s_guard);release_model(old);
    SemaphoreHandle_t m=atomic_exchange(&s_cache_mutex,NULL);if(m)vSemaphoreDelete(m);
    assert(!atomic_load(&allocations) && !atomic_load(&image_refs));
}
int main(int argc,char **argv) {
    assert(argc==4);const char *which=argv[1];size_t b_size;
    read_asset(argv[2],&asset_a,&asset_size);read_asset(argv[3],&asset_b,&b_size);assert(b_size==asset_size);
    memcpy(key_a,asset_a+24,32);memcpy(key_b,key_a,32);key_b[0]^=1;
    if(!strcmp(which,"offline") || !strcmp(which,"start_retry")) {
        if(!strcmp(which,"start_retry"))atomic_store(&fail_start,1);
        muse_pixel_avatar_start();assert(!s_have_identity);
        if(!strcmp(which,"start_retry")) {
            assert(!atomic_load(&s_running) && atomic_load(&start_attempts)==1);
            for (int i=0;i<10;i++) {
                muse_pixel_avatar_poll();
            }
            assert(atomic_load(&start_attempts)==1);
            atomic_store(&clock_us,1000000);muse_pixel_avatar_poll();assert(atomic_load(&start_attempts)==2 && atomic_load(&s_running));
            pthread_t t;assert(!pthread_create(&t,NULL,queued_job,NULL));assert(!pthread_join(t,NULL));live_is(asset_a);
        } else {
            worker_state_t state={1,0};atomic_store(&fail_mount,1);worker_step(&state);assert(!s_model);
            atomic_store(&clock_us,1000000);atomic_store(&fail_malloc,1);worker_step(&state);assert(!s_model);
            atomic_store(&clock_us,3000000);atomic_store(&fail_model,1);worker_step(&state);assert(!s_model);
            atomic_store(&clock_us,7000000);worker_step(&state);live_is(asset_a);assert(!s_have_identity);
        }
    } else if(!strcmp(which,"diagnostic")) {
#if CONFIG_LV_USE_SNAPSHOT
        assert(muse_pixel_avatar_console("pixel.test_wifi=0"));assert(!atomic_load(&wifi_on));assert(!strcmp(last_control,"@pixel_test_wifi ok\n"));
        assert(muse_pixel_avatar_test_boot_offline());
        assert(muse_pixel_avatar_console("pixel.test_wifi=1"));assert(atomic_load(&wifi_on) && atomic_load(&wifi_calls)==2);
        assert(!muse_pixel_avatar_test_boot_offline() && atomic_load(&test_flag_writes)==2);
#else
        assert(!muse_pixel_avatar_console("pixel.test_wifi=0"));assert(!muse_pixel_avatar_console("pixel.test_wifi=1"));assert(!atomic_load(&wifi_calls) && !atomic_load(&test_flag_writes));
#endif
        assert(!atomic_load(&config_writes));
    } else {
        seed();
        if(!strcmp(which,"prepare")) {}
        else if(!strcmp(which,"owner_race")) {
            const muse_pixel_avatar_t *held=muse_pixel_avatar_acquire();assert(held);arm(H_OWNER);
            struct job j={{1,0},false,false};pthread_t t;assert(!pthread_create(&t,NULL,worker_job,&j));wait_hit(H_OWNER);
            atomic_store(&paired,false);atomic_store(&setup,false);muse_pixel_avatar_invalidate();assert(!muse_pixel_avatar_acquire());
            unblock();assert(!pthread_join(t,NULL));assert(!s_have_owner && !s_model);assert(held->pack.colors==3);muse_pixel_avatar_release(held);
        } else if(!strcmp(which,"restore_install")) {
            drop_and_select();upload(asset_b);arm(H_READ);struct job w={{1,0},false,false},c={{1,0},false,false};pthread_t wt,ct;
            assert(!pthread_create(&wt,NULL,worker_job,&w));wait_hit(H_READ);int attempts=atomic_load(&lock_attempts);
            assert(!pthread_create(&ct,NULL,commit_job,&c));wait_locks(attempts+1);assert(!atomic_load(&c.done));unblock();
            assert(!pthread_join(wt,NULL) && !pthread_join(ct,NULL) && c.ok);live_is(asset_b);disk_is(asset_b);
        } else if(!strcmp(which,"install_restore")) {
            drop_and_select();upload(asset_b);arm(H_FSYNC);struct job w={{1,0},false,false},c={{1,0},false,false};pthread_t wt,ct;
            assert(!pthread_create(&ct,NULL,commit_job,&c));wait_hit(H_FSYNC);int attempts=atomic_load(&lock_attempts);
            assert(!pthread_create(&wt,NULL,worker_job,&w));wait_locks(attempts+1);assert(!atomic_load(&w.done));unblock();
            assert(!pthread_join(ct,NULL) && !pthread_join(wt,NULL) && c.ok);live_is(asset_b);disk_is(asset_b);
        } else if(!strcmp(which,"preaccept")) {
            const muse_pixel_avatar_t *held=muse_pixel_avatar_acquire();upload(asset_b);arm(H_FSYNC);struct job c={{1,0},false,false};pthread_t t;
            assert(!pthread_create(&t,NULL,commit_job,&c));wait_hit(H_FSYNC);muse_pixel_avatar_identity(key_b);assert(!muse_pixel_avatar_acquire());
            unblock();assert(!pthread_join(t,NULL) && !c.ok);disk_is(asset_a);assert(held->pack.colors==3);muse_pixel_avatar_release(held);
        } else if(!strcmp(which,"postaccept_clear")) {
            upload(asset_b);arm(H_RENAME);struct job c={{1,0},false,false},w={{1,0},false,false};pthread_t ct,wt;
            assert(!pthread_create(&ct,NULL,commit_job,&c));wait_hit(H_RENAME);atomic_store(&paired,false);muse_pixel_avatar_invalidate();muse_pixel_avatar_clear();assert(!muse_pixel_avatar_acquire());
            int attempts=atomic_load(&lock_attempts);assert(!pthread_create(&wt,NULL,worker_job,&w));wait_locks(attempts+1);assert(!atomic_load(&w.done));
            unblock();assert(!pthread_join(ct,NULL) && !pthread_join(wt,NULL) && c.ok);assert(!s_model && access(PIXEL_FILE,F_OK)!=0 && access(PIXEL_TMP,F_OK)!=0);
        } else if(!strcmp(which,"clear_preaccept")) {
            upload(asset_b);arm(H_FSYNC);struct job c={{1,0},false,false};pthread_t t;assert(!pthread_create(&t,NULL,commit_job,&c));wait_hit(H_FSYNC);
            muse_pixel_avatar_clear();muse_pixel_avatar_identity(key_a);assert(s_clear_pending && !muse_pixel_avatar_acquire());unblock();
            assert(!pthread_join(t,NULL) && !c.ok);assert(!begin(asset_size,key_a));disk_is(asset_a);worker_state_t state={1,0};worker_step(&state);assert(!s_clear_pending && access(PIXEL_FILE,F_OK)!=0);
        } else if(!strcmp(which,"double_clear")) {
            muse_pixel_avatar_clear();arm(H_UNLINK);struct job w={{1,0},false,false};pthread_t t;
            assert(!pthread_create(&t,NULL,worker_job,&w));wait_hit(H_UNLINK);
            muse_pixel_avatar_clear();muse_pixel_avatar_identity(key_a);unblock();assert(!pthread_join(t,NULL));
            assert(s_clear_pending && (atomic_load(&s_jobs)&JOB_CLEAR));assert(!begin(asset_size,key_a));
            worker_state_t state={1,0};worker_step(&state);assert(!s_clear_pending && !(atomic_load(&s_jobs)&JOB_CLEAR));
            upload(asset_b);assert(commit());worker_step(&state);disk_is(asset_b);live_is(asset_b);
        } else if(!strcmp(which,"read_fence")) {
            drop_and_select();arm(H_READ);struct job w={{1,0},false,false};pthread_t t;assert(!pthread_create(&t,NULL,worker_job,&w));wait_hit(H_READ);
            muse_pixel_avatar_identity(key_b);unblock();assert(!pthread_join(t,NULL));assert(!s_model && !muse_pixel_avatar_acquire());
        } else if(!strcmp(which,"faults")) {
            uint32_t revision=s_model->view.revision;upload(asset_b);atomic_store(&fail_fsync,1);assert(!commit());disk_is(asset_a);live_is(asset_a);assert(s_model->view.revision==revision);
            upload(asset_b);atomic_store(&fail_rename,1);assert(!commit());disk_is(asset_a);live_is(asset_a);
            uint8_t *bad=malloc(asset_size);assert(bad);memcpy(bad,asset_b,asset_size);bad[60]^=1;upload(bad);assert(!commit());disk_is(asset_a);live_is(asset_a);free(bad);
        } else if(!strcmp(which,"null_identity")) {
            muse_pixel_avatar_identity(key_b);muse_pixel_avatar_identity(NULL);assert(s_have_identity && !memcmp(s_identity,key_b,32));worker_state_t state={1,0};worker_step(&state);assert(!muse_pixel_avatar_acquire());disk_is(asset_a);
        } else if(!strcmp(which,"unknown_identity")) {
            muse_pixel_avatar_identity_unavailable();muse_pixel_avatar_identity(NULL);
            assert(s_identity_required && !s_have_identity && !muse_pixel_avatar_acquire());
            worker_state_t state={1,0};worker_step(&state);assert(!s_model);disk_is(asset_a);
            atomic_store(&personal,false);muse_pixel_avatar_source_changed();
            atomic_store(&personal,true);muse_pixel_avatar_source_changed();worker_step(&state);assert(!s_model);
            muse_pixel_avatar_identity(key_a);worker_step(&state);live_is(asset_a);
            muse_pixel_avatar_invalidate();worker_step(&state);live_is(asset_a);
        } else if(!strcmp(which,"owner_memory")) {
            atomic_store(&fail_owner,1);worker_state_t state={1,0};worker_step(&state);live_is(asset_a);assert(s_have_owner);
        } else if(!strcmp(which,"source_roundtrip")) {
            upload(asset_b);atomic_store(&personal,false);muse_pixel_avatar_source_changed();assert(!muse_pixel_avatar_acquire());atomic_store(&personal,true);muse_pixel_avatar_source_changed();assert(!commit());disk_is(asset_a);
            worker_state_t state={1,0};worker_step(&state);live_is(asset_a);
        } else if(!strcmp(which,"bounds_expiry")) {
            assert(begin(asset_size,key_a));assert(muse_pixel_avatar_console("pixel.chunk=1:YWJj"));assert(!s_upload_offset);
            atomic_store(&clock_us,31000000);expire_upload();assert(!s_upload);disk_is(asset_a);
            assert(begin(asset_size,key_a));muse_pixel_avatar_source_changed();expire_upload();assert(!s_upload);
        } else if(!strcmp(which,"export_key")) {
            atomic_store(&canonical,&image_a);arm(H_KEY);pthread_t t;assert(!pthread_create(&t,NULL,export_job,NULL));wait_hit(H_KEY);
            muse_pixel_avatar_identity(key_b);atomic_store(&canonical,&image_b);unblock();assert(!pthread_join(t,NULL));assert(!atomic_load(&export_headers) && !atomic_load(&export_ends));
        } else if(!strcmp(which,"export_revoke")) {
            atomic_store(&canonical,&image_a);arm(H_CHUNK);pthread_t t;assert(!pthread_create(&t,NULL,export_job,NULL));wait_hit(H_CHUNK);
            atomic_store(&paired,false);muse_pixel_avatar_invalidate();unblock();assert(!pthread_join(t,NULL));assert(atomic_load(&export_headers)==1 && atomic_load(&export_chunks)==1 && !atomic_load(&export_ends));
        } else if(!strcmp(which,"export_good")) {
            atomic_store(&canonical,&image_a);export_image(NULL);assert(atomic_load(&export_headers)==1 && atomic_load(&export_ends)==1 && atomic_load(&export_chunks)>1);assert(strstr(last_header,"source_key"));
        } else if(!strcmp(which,"mutex_retry")) {
            drop_and_select();SemaphoreHandle_t m=atomic_exchange(&s_cache_mutex,NULL);vSemaphoreDelete(m);atomic_store(&fail_mutex,1);worker_state_t state={1,0};worker_step(&state);assert(!s_model);worker_step(&state);live_is(asset_a);
        } else assert(!"unknown case");
    }
    cleanup();free(asset_a);free(asset_b);puts("PASS production pixel lifecycle/transaction case");return 0;
}
'''


class PixelBackend(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix='pixel-backend-races-')
        cls.addClassCleanup(cls.tmp.cleanup)
        cls.stage = Path(cls.tmp.name)
        (cls.stage / 'fakes.h').write_text(FAKES)
        for name in ('muse_state.h', 'muse_settings.h', 'muse_voice.h', 'config_store.h',
                     'esp_heap_caps.h', 'esp_timer.h', 'mbedtls/md.h', 'mbedtls/base64.h',
                     'freertos/FreeRTOS.h', 'freertos/task.h', 'freertos/semphr.h',
                     'freertos/idf_additions.h'):
            path = cls.stage / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('#include "fakes.h"\n')
        for name in ('muse_avatar.h', 'muse_avatar_render.h', 'muse_pixel_avatar.h', 'muse_pixel_art.h'):
            shutil.copyfile(MUSE / name, cls.stage / name)
        source = (ROOT / 'main/pixel_avatar.c').read_text()
        source = source.replace('"/files/.muse-avatar-v1/pixel.dat"', '"' + str(cls.stage / 'pixel.dat') + '"')
        source = source.replace('"/files/.muse-avatar-v1/pixel.tmp"', '"' + str(cls.stage / 'pixel.tmp') + '"')
        source = source.replace('mkdir("/files/.muse-avatar-v1",0700);', '')
        (cls.stage / 'backend.c').write_text(source)
        (cls.stage / 'test.c').write_text(HARNESS)
        original = pack_bytes()
        candidate = bytearray(original)
        candidate[66] ^= 0x1F  # Same generation/owner, visibly different palette.
        struct.pack_into('<I', candidate, 56, zlib.crc32(candidate[64:]))
        struct.pack_into('<I', candidate, 60, zlib.crc32(candidate[:60]))
        (cls.stage / 'a.msp').write_bytes(original)
        (cls.stage / 'b.msp').write_bytes(candidate)
        cls.executables = {}
        for snapshot in (0, 1):
            (cls.stage / 'sdkconfig.h').write_text(
                '#define CONFIG_MUSE_PIXEL_AVATAR 1\n#define CONFIG_MUSE_AVATAR_ANIMATIONS 1\n'
                f'#define CONFIG_LV_USE_SNAPSHOT {snapshot}\n')
            exe = cls.stage / f'backend-test-{snapshot}'
            built = subprocess.run([
                *shlex.split(os.environ.get('CC', 'cc')), '-std=c11', '-D_POSIX_C_SOURCE=200809L',
                '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
                '-ffunction-sections', '-fdata-sections', *LINK_GC, '-fsanitize=address,undefined',
                '-fno-omit-frame-pointer', '-pthread', '-I', str(cls.stage),
                str(cls.stage / 'test.c'), str(MUSE / 'muse_pixel_art.c'), '-o', str(exe),
            ], capture_output=True, text=True)
            if built.returncode:
                raise AssertionError(built.stdout + built.stderr)
            cls.executables[snapshot] = exe

    def setUp(self):
        for name in ('pixel.dat', 'pixel.tmp'):
            (self.stage / name).unlink(missing_ok=True)

    def run_case(self, case, *, snapshot=1, prepare=False):
        if prepare:
            self.run_case('prepare', snapshot=snapshot)
        result = subprocess.run(
            [str(self.executables[snapshot]), case, str(self.stage / 'a.msp'), str(self.stage / 'b.msp')],
            capture_output=True, text=True, timeout=30,
            env={**os.environ, 'ASAN_OPTIONS': ASAN_OPTIONS,
                 'UBSAN_OPTIONS': 'halt_on_error=1'},
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('PASS production pixel lifecycle/transaction case', result.stdout)

    def test_offline_cold_boot_retries_mount_and_allocations(self):
        self.run_case('offline', prepare=True)

    def test_initial_task_creation_failure_retries_without_cloud_callback(self):
        self.run_case('start_retry', prepare=True)

    def test_pre_invalidation_owner_probe_cannot_revalidate_erased_pairing(self):
        self.run_case('owner_race')

    def test_restore_started_before_install_cannot_overwrite_new_model(self):
        self.run_case('restore_install')

    def test_install_started_before_restore_supersedes_old_probe(self):
        self.run_case('install_restore')

    def test_invalidation_during_staging_preserves_previous_disk_asset(self):
        self.run_case('preaccept')

    def test_post_acceptance_revocation_hides_model_and_clear_is_serialized(self):
        self.run_case('postaccept_clear')

    def test_clear_revokes_staging_before_file_transaction_finishes(self):
        self.run_case('clear_preaccept')

    def test_new_clear_during_old_clear_remains_pending_until_its_own_transaction(self):
        self.run_case('double_clear')

    def test_generation_change_during_cache_read_prevents_publication(self):
        self.run_case('read_fence')

    def test_fsync_rename_and_validation_failures_preserve_previous_asset(self):
        self.run_case('faults')

    def test_null_teardown_does_not_erase_known_online_mismatch(self):
        self.run_case('null_identity')

    def test_authoritative_digest_failure_blocks_owner_only_restore_until_valid_identity(self):
        self.run_case('unknown_identity')

    def test_owner_probe_allocation_failure_keeps_valid_model(self):
        self.run_case('owner_memory')

    def test_source_roundtrip_cancels_upload_and_preserves_cache(self):
        self.run_case('source_roundtrip')

    def test_chunk_bounds_and_stale_or_expired_upload_cleanup(self):
        self.run_case('bounds_expiry')

    def test_export_pins_image_key_instead_of_labeling_old_pixels_as_new(self):
        self.run_case('export_key')

    def test_export_stops_after_owner_revocation(self):
        self.run_case('export_revoke')

    def test_export_matching_immutable_image_completes(self):
        self.run_case('export_good')

    def test_cache_mutex_allocation_failure_is_retried(self):
        self.run_case('mutex_retry')

    def test_snapshot_only_wifi_diagnostic_does_not_erase_configuration(self):
        for snapshot in (0, 1):
            with self.subTest(snapshot=snapshot):
                self.run_case('diagnostic', snapshot=snapshot)


if __name__ == '__main__':
    unittest.main()
