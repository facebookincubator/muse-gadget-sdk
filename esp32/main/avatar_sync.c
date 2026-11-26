/* Copyright (c) Meta Platforms, Inc. and affiliates.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * http://www.apache.org/licenses/LICENSE-2.0
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "sdkconfig.h"
#if CONFIG_MUSE_AVATAR_SYNC
#include "avatar_sync.h"
#include "muse_avatar.h"
#if CONFIG_MUSE_PIXEL_AVATAR
#include "muse_pixel_avatar.h"
#endif
#if CONFIG_MUSE_AVATAR_ANIMATIONS
#include "muse_avatar_motion.h"
#include "avatar_motion_console.h"
#endif
#include "muse_settings.h"
#include "muse_state.h"
#include "muse_voice.h"
#if CONFIG_MUSE_VIDEO
#include "muse_video.h"
#endif
#if CONFIG_MUSE_APP_VIEW
#include "muse_app_view.h"
#endif
#include "avatar_identity.h"
#include "avatar_request.h"
#include "avatar_webp.h"
#include "avatar_cache.h"
#include "vm_file_path.h"
#include "config_store.h"
#include "image_fetch.h"
#include "noise_tunnel_internal.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_log.h"
#include "mbedtls/md.h"
#include <sys/stat.h>
#include "noise_control.h"
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IDENTITY_MAX 8192
static atomic_bool s_probe_busy;
static atomic_uint s_source;
static atomic_uint s_generation = 1, s_jobs;
static atomic_uint s_content_epoch = 1;
static atomic_uint s_metadata_epoch;
static atomic_bool s_worker_busy, s_ready, s_binding_blocked;
static _Atomic(const char *) s_status = "Built-in";
static atomic_size_t s_last_peak;
static atomic_int s_last_ms;
static atomic_uint s_identity_reads, s_file_reads, s_bytes_received, s_cache_writes, s_cache_loads;
static atomic_uint s_path_id, s_revision_id, s_cached_path_id, s_cached_revision_id;
static atomic_uint s_completions;
static portMUX_TYPE s_image_guard = portMUX_INITIALIZER_UNLOCKED;
#if CONFIG_MUSE_PIXEL_AVATAR
typedef struct { muse_avatar_image_t view; unsigned refs; uint32_t crc, content_epoch; uint8_t pixel_key[32]; bool pixel_key_valid; } image_t;
#else
typedef struct { muse_avatar_image_t view; unsigned refs; uint32_t crc, content_epoch; } image_t;
#endif
static image_t *s_image;
static avatar_identity_t s_identity;
static uint8_t s_binding[32];
#if CONFIG_MUSE_PIXEL_AVATAR
static bool pixel_identity_key(const avatar_identity_t *id,uint8_t key[32]) {
    if(!id)return false;
    mbedtls_md_context_t md;mbedtls_md_init(&md);
    bool ok=!mbedtls_md_setup(&md,mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),0) && !mbedtls_md_starts(&md) &&
        !mbedtls_md_update(&md,(const uint8_t *)id->path,strlen(id->path)+1) &&
        !mbedtls_md_update(&md,(const uint8_t *)id->revision,strlen(id->revision)) && !mbedtls_md_finish(&md,key);
    mbedtls_md_free(&md);return ok;
}
static void pixel_identity(const avatar_identity_t *id) {
    uint8_t key[32]={0};bool have=pixel_identity_key(id,key);
    muse_pixel_avatar_identity(have?key:NULL);
}
static bool set_image_pixel_key(image_t *image,const avatar_identity_t *id) {
    image->pixel_key_valid=pixel_identity_key(id,image->pixel_key);return image->pixel_key_valid;
}
bool muse_avatar_copy_image_key(const muse_avatar_image_t *image,uint8_t out[32]) {
    if(!image || !out)return false;
#if CONFIG_MUSE_AVATAR_ANIMATIONS
    if(image->release)return false; /* only canonical stills own this metadata */
#endif
    const image_t *owner=(const image_t *)image;
    if(!owner->pixel_key_valid)return false;
    memcpy(out,owner->pixel_key,32);return true;
}
#endif
#if CONFIG_MUSE_AVATAR_ANIMATIONS
static uint8_t s_motion_source[32], s_motion_binding[32];
static bool s_motion_bound;
bool muse_avatar_personal_allowed(void) {
    return !atomic_load(&s_binding_blocked) && atomic_load(&s_source)==MUSE_AVATAR_PERSONAL;
}
bool avatar_sync_motion_keys(uint8_t source[32], uint8_t binding[32]) {
    portENTER_CRITICAL(&s_image_guard);
    bool ok=s_motion_bound && !atomic_load(&s_binding_blocked) && atomic_load(&s_source)==MUSE_AVATAR_PERSONAL;
    if (ok) { memcpy(source,s_motion_source,32); memcpy(binding,s_motion_binding,32); }
    portEXIT_CRITICAL(&s_image_guard); return ok;
}
static void motion_binding(const avatar_identity_t *id) {
    uint8_t key[32]={0};
    if (id) {
        mbedtls_md_context_t ctx; mbedtls_md_init(&ctx);
        if (mbedtls_md_setup(&ctx,mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),0) ||
            mbedtls_md_starts(&ctx) || mbedtls_md_update(&ctx,(const uint8_t *)id->path,strlen(id->path)+1) ||
            mbedtls_md_update(&ctx,(const uint8_t *)id->revision,strlen(id->revision)) || mbedtls_md_finish(&ctx,key)) id=NULL;
        mbedtls_md_free(&ctx);
    }
    portENTER_CRITICAL(&s_image_guard);
    s_motion_bound=id!=NULL;
    if (id) { memcpy(s_motion_source,key,32); memcpy(s_motion_binding,s_binding,32); }
    portEXIT_CRITICAL(&s_image_guard);
    muse_avatar_motion_bind(id?key:NULL,id?s_binding:NULL);
#if CONFIG_MUSE_PIXEL_AVATAR
    pixel_identity(id);
#endif
}
#else
#if CONFIG_MUSE_PIXEL_AVATAR
#define motion_binding(id) pixel_identity(id)
#else
#define motion_binding(id) ((void)0)
#endif
#endif

static bool s_have_binding;
static uint32_t s_revision, s_cache_sequence;
static int s_cache_slot = -1;
static bool s_image_saved;
#define JOB_REFRESH 1u
#define JOB_CLEAR 2u
#define JOB_BINDING 4u
#define AVATAR_PEAK (1536 * 1024)
#define AVATAR_RESERVE (1024 * 1024)
#if CONFIG_SPIRAM_FETCH_INSTRUCTIONS && CONFIG_SPIRAM_RODATA && CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM
#define AVATAR_TASK_CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#else
#define AVATAR_TASK_CAPS (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
#endif
#define CACHE_A "/files/.muse-avatar-v1/a.dat"
#define CACHE_B "/files/.muse-avatar-v1/b.dat"
#define CACHE_META "/files/.muse-avatar-v1/metadata.dat"
#define CACHE_META_TMP "/files/.muse-avatar-v1/metadata.tmp"
static void worker_kick(void);
static void *request_alloc(size_t n);
static void request_yield(void);
static bool probe_stop(uintptr_t unused) { (void)unused; return false; }

/* Only keys and types cross USB. Never print identity string values. */
static cJSON *schema(const cJSON *v, int depth) {
    if (depth > 8) return cJSON_CreateString("depth_limit");
    if (cJSON_IsObject(v) || cJSON_IsArray(v)) {
        cJSON *out = cJSON_IsObject(v) ? cJSON_CreateObject() : cJSON_CreateArray();
        if (!out) return NULL;
        int count = 0;
        for (const cJSON *c = v->child; c && count++ < 64; c = c->next) {
            cJSON *item = schema(c, depth + 1);
            bool added = item && (cJSON_IsObject(v) ? cJSON_AddItemToObject(out, c->string, item) : cJSON_AddItemToArray(out, item));
            if (!added) { cJSON_Delete(item); cJSON_Delete(out); return NULL; }
        }
        return out;
    }
    return cJSON_CreateString(cJSON_IsString(v) ? "string" : cJSON_IsBool(v) ? "boolean" :
                              cJSON_IsNumber(v) ? "number" : "null");
}

static void probe_task(void *arg) {
    (void)arg;
    /* Bounded by the request's 10 s deadline; a timeout frees the buffer. */
    const avatar_request_ops_t ops = {noise_ctrl_req_open, noise_ctrl_req_cancel, request_alloc,
                                      heap_caps_free, esp_timer_get_time, request_yield, probe_stop};
    avatar_response_t *r = avatar_request_get("/identity", NULL, IDENTITY_MAX, 0, &ops);
    cJSON *root = r && r->status == 200 ? cJSON_ParseWithLength((const char *)r->body, r->len) : NULL;
    cJSON *shape = root ? schema(root, 0) : NULL;
    char *json = shape ? cJSON_PrintUnformatted(shape) : NULL;
    printf("@avatar_schema %s\n", json ? json : "{\"error\":\"identity_unavailable\"}");
    cJSON_free(json);
    cJSON_Delete(shape);
    cJSON_Delete(root);
    avatar_request_release(r);
    atomic_store(&s_probe_busy, false);
    vTaskDeleteWithCaps(NULL);
}

bool avatar_sync_console(const char *line) {
#if CONFIG_MUSE_PIXEL_AVATAR
    if(muse_pixel_avatar_console(line))return true;
#endif
#if CONFIG_MUSE_AVATAR_ANIMATIONS
    if (avatar_motion_console(line)) return true;
#endif
    if (!strcmp(line, "avatar")) {
        const muse_avatar_image_t *image = muse_avatar_acquire();
        printf("@avatar {\"source\":\"%s\",\"status\":\"%s\",\"width\":%d,\"height\":%d,\"revision\":%u,\"peak_bytes\":%u,\"refresh_ms\":%d,\"identity_reads\":%u,\"file_reads\":%u,\"downloaded_bytes\":%u,\"cache_writes\":%u,\"cache_loads\":%u,\"path_id\":%u,\"asset_revision_id\":%u,\"cached_path_id\":%u,\"cached_asset_revision_id\":%u,\"resident_bytes\":%u,\"completed_refreshes\":%u}\n",
               muse_avatar_source() == MUSE_AVATAR_PERSONAL ? "personal" : "builtin", muse_avatar_status(),
               image ? image->data.width : 0, image ? image->data.height : 0,
               image ? (unsigned)image->revision : 0, (unsigned)atomic_load(&s_last_peak), atomic_load(&s_last_ms),
               atomic_load(&s_identity_reads), atomic_load(&s_file_reads), atomic_load(&s_bytes_received),
               atomic_load(&s_cache_writes), atomic_load(&s_cache_loads),
               atomic_load(&s_path_id), atomic_load(&s_revision_id), atomic_load(&s_cached_path_id), atomic_load(&s_cached_revision_id),
               (unsigned)avatar_webp_live_bytes(), atomic_load(&s_completions));
        muse_avatar_release(image); return true;
    }
    if (!strcmp(line, "avatar.source=builtin") || !strcmp(line, "avatar.source=personal")) {
        muse_state_set_asleep(false); muse_state_poke();
        muse_avatar_select(!strcmp(line, "avatar.source=personal") ? MUSE_AVATAR_PERSONAL : MUSE_AVATAR_BUILTIN);
        return true;
    }
    if (!strcmp(line, "avatar.refresh")) { muse_avatar_refresh(); return true; }
    if (!strcmp(line, "avatar.clear")) { muse_avatar_clear(); return true; }
    if (strcmp(line, "avatar.identity")) return false;
    bool expected = false;
    if (!atomic_compare_exchange_strong(&s_probe_busy, &expected, true)) return true;
    if (xTaskCreateWithCaps(probe_task, "avatar_probe", 4096, NULL, 2, NULL,
                           AVATAR_TASK_CAPS) != pdPASS) {
        atomic_store(&s_probe_busy, false);
        printf("@avatar_schema {\"error\":\"no_memory\"}\n");
    }
    return true;
}

const muse_avatar_image_t *muse_avatar_acquire(void) {
    image_t *image = NULL;
    portENTER_CRITICAL(&s_image_guard);
    if (atomic_load(&s_source) == MUSE_AVATAR_PERSONAL && !atomic_load(&s_binding_blocked) && s_image) {
        image = s_image; image->refs++;
    }
    portEXIT_CRITICAL(&s_image_guard);
    return image ? &image->view : NULL;
}
void muse_avatar_release(const muse_avatar_image_t *view) {
    if (!view) return;
#if CONFIG_MUSE_AVATAR_ANIMATIONS
    if (view->release) { view->release(view); return; }
#endif
    image_t *image = (image_t *)view;
    bool last;
    portENTER_CRITICAL(&s_image_guard);
    last = --image->refs == 0;
    portEXIT_CRITICAL(&s_image_guard);
    if (last) { muse_webp_free((void *)view->data.pixels); heap_caps_free(image); }
}
static void publish(image_t *image) {
    if (!image) motion_binding(NULL);
    portENTER_CRITICAL(&s_image_guard);
    image_t *old = s_image; s_image = image;
    if (image) atomic_store(&s_metadata_epoch, image->content_epoch);
    portEXIT_CRITICAL(&s_image_guard);
    if (old) muse_avatar_release(&old->view);
    if (!image) s_image_saved = false;
}
static image_t *new_image(uint16_t *pixels, int w, int h, uint32_t content_epoch) {
    image_t *image = heap_caps_calloc(1, sizeof(*image), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!image) { muse_webp_free(pixels); return NULL; }
    image->view = (muse_avatar_image_t){.data={pixels, w, h}, .revision=++s_revision};
    image->refs = 1;
    image->content_epoch = content_epoch;
    image->crc = avatar_cache_crc(pixels, (size_t)w * h * 2);
    return image;
}
static bool busy(void) {
    muse_mode_t mode = muse_state_mode(NULL);
#if CONFIG_MUSE_VIDEO
    if (muse_video_is_open()) return true;
#endif
#if CONFIG_MUSE_APP_VIEW
    if (muse_app_view_is_open()) return true;
#endif
    return muse_voice_in_call() || mode == MUSE_MODE_LISTENING || mode == MUSE_MODE_SPEAKING ||
           mode == MUSE_MODE_THINKING || image_fetch_busy() ||
           heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) < 48 * 1024 ||
           !noise_tx_has_dma_headroom(NULL) || !noise_tx_has_contiguous_dma_headroom();
}
static bool cancelled(unsigned generation) {
    return generation != atomic_load(&s_generation) || atomic_load(&s_source) != MUSE_AVATAR_PERSONAL;
}

static void *request_alloc(size_t n) { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }
static bool request_stop(uintptr_t generation) { return cancelled((unsigned)generation) || muse_state_asleep() || busy(); }
static void request_yield(void) { vTaskDelay(pdMS_TO_TICKS(20)); }
static avatar_response_t *get(const char *path, const char *const *headers, size_t cap, unsigned generation) {
    const avatar_request_ops_t ops = {noise_ctrl_req_open, noise_ctrl_req_cancel, request_alloc,
                                      heap_caps_free, esp_timer_get_time, request_yield, request_stop};
    return avatar_request_get(path, headers, cap, generation, &ops);
}
static uint8_t *fetch_webp(const char *path, unsigned generation, size_t *len) {
    char *url = vm_file_request_path(path);
    if (!url) return NULL;
    size_t chunk = noise_ctrl_req_max_body(); if (chunk > 4096) chunk = 4096;
    uint8_t *encoded = NULL;
    size_t total = 0, offset = 0;
    int64_t deadline = esp_timer_get_time() + 60000000;
    while (!cancelled(generation) && !muse_state_asleep() && !busy() && esp_timer_get_time() < deadline) {
        size_t want = total && total - offset < chunk ? total - offset : chunk;
        char range[64]; snprintf(range, sizeof(range), "bytes=%u-%u", (unsigned)offset, (unsigned)(offset + want - 1));
        const char *headers[] = {"Range", range, NULL};
        atomic_fetch_add(&s_file_reads, 1);
        avatar_response_t *r = get(url, headers, want, generation);
        if (!r) break;
        bool ok = r->len > 0 && (r->status == 206 || (r->status == 200 && offset == 0));
        if (ok && !encoded) {
            ok = r->len >= 12 && !memcmp(r->body, "RIFF", 4) && !memcmp(r->body + 8, "WEBP", 4);
            if (ok) {
                uint32_t n = (uint32_t)r->body[4] | (uint32_t)r->body[5] << 8 |
                             (uint32_t)r->body[6] << 16 | (uint32_t)r->body[7] << 24;
                ok = n >= 4 && n <= 512 * 1024 - 8;
                total = (size_t)n + 8;
                size_t live = avatar_webp_live_bytes();
                if (ok && live < AVATAR_PEAK && heap_caps_get_free_size(MALLOC_CAP_SPIRAM) > AVATAR_PEAK - live + AVATAR_RESERVE)
                    encoded = heap_caps_malloc(total, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                if (!encoded) ok = false;
            }
        }
        if (ok) ok = r->len <= total - offset && (r->status != 206 || r->len == want || r->len == total - offset);
        if (ok) { memcpy(encoded + offset, r->body, r->len); offset += r->len; atomic_fetch_add(&s_bytes_received, r->len); }
        avatar_request_release(r);
        if (!ok) break;
        if (offset == total) { free(url); *len = total; return encoded; }
    }
    free(url); heap_caps_free(encoded); return NULL;
}

static bool binding(uint8_t out[32]) {
    if (!config_is_provisioned()) return false;
    char *value = heap_caps_calloc(1, 2048, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!value) return false;
    mbedtls_md_context_t md; mbedtls_md_init(&md);
    bool ok = mbedtls_md_setup(&md, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 0) == 0 && mbedtls_md_starts(&md) == 0;
    const char *keys[] = {"vm_url", "noise_host", "refresh_token", "node_id"};
    for (size_t i = 0; ok && i < sizeof(keys) / sizeof(keys[0]); i++) {
        value[0] = 0;
        bool found = config_get_str(keys[i], value, 2048);
        if (!found && i == 2) { ok = false; break; }
        ok = mbedtls_md_update(&md, (const uint8_t *)value, strlen(value) + 1) == 0;
    }
    if (ok) {
        char vm[128] = {0}, saved[128] = {0};
        config_get_str("avatar_vm", saved, sizeof(saved));
        if (noise_ctrl_copy_vm_id(vm, sizeof(vm))) {
            if (strcmp(vm, saved)) config_set_str("avatar_vm", vm);
        } else memcpy(vm, saved, sizeof(vm));
        ok = vm[0] && mbedtls_md_update(&md, (const uint8_t *)vm, strlen(vm) + 1) == 0;
        muse_settings_hatch_host(value);
        ok = ok && mbedtls_md_update(&md, (const uint8_t *)value, strlen(value) + 1) == 0;
        muse_settings_hatch_vm(value);
        ok = ok && mbedtls_md_update(&md, (const uint8_t *)value, strlen(value) + 1) == 0 && mbedtls_md_finish(&md, out) == 0;
    }
    memset(value, 0, 2048); heap_caps_free(value); mbedtls_md_free(&md);
    return ok;
}
static void cache_clear(void) {
#if CONFIG_MUSE_AVATAR_ANIMATIONS
    muse_avatar_motion_clear();
#endif
    muse_avatar_files_mount();
    remove(CACHE_A); remove(CACHE_B);
    remove(CACHE_META); remove(CACHE_META_TMP);
    s_cache_sequence = 0; s_cache_slot = -1;
}
static bool cache_load(void) {
    uint32_t content_epoch = atomic_load(&s_content_epoch);
    if (!muse_avatar_files_mount()) return false;
    avatar_cache_header_t a, b, h;
    bool av = avatar_cache_read(CACHE_A, s_binding, &a, NULL, 0);
    bool bv = avatar_cache_read(CACHE_B, s_binding, &b, NULL, 0);
    for (int attempt = 0; attempt < 2 && (av || bv); attempt++) {
        int slot = av && (!bv || (int32_t)(a.sequence - b.sequence) > 0) ? 0 : 1;
        h = slot ? b : a;
        uint16_t *pixels = muse_webp_malloc(h.bytes);
        if (!pixels) return false;
        bool ok = avatar_cache_read(slot ? CACHE_B : CACHE_A, s_binding, &h, pixels, h.bytes);
        if (ok) {
            avatar_cache_identity_read(CACHE_META, &h);
            image_t *image = new_image(pixels, h.width, h.height, content_epoch);
            if (!image) return false;
#if CONFIG_MUSE_PIXEL_AVATAR
            if(!set_image_pixel_key(image,&h.identity)) {muse_avatar_release(&image->view);return false;}
#endif
            publish(image); s_identity = h.identity; motion_binding(&s_identity); s_cache_slot = slot; s_cache_sequence = h.sequence;
            s_image_saved = true;
            atomic_store(&s_cached_path_id, avatar_cache_crc(h.identity.path, strlen(h.identity.path)));
            atomic_store(&s_cached_revision_id, avatar_cache_crc(h.identity.revision, strlen(h.identity.revision)));
            atomic_fetch_add(&s_cache_loads, 1);
            atomic_store(&s_status, "Using saved avatar"); return true;
        }
        muse_webp_free(pixels);
        if (slot) bv = false; else av = false;
    }
    return false;
}
static bool cache_proceed(void *ctx) { return !cancelled(*(unsigned *)ctx) && !busy(); }
static void cached_identity(const avatar_identity_t *identity) {
    atomic_store(&s_cached_path_id, avatar_cache_crc(identity->path, strlen(identity->path)));
    atomic_store(&s_cached_revision_id, avatar_cache_crc(identity->revision, strlen(identity->revision)));
}
static bool cache_save(image_t *image, const avatar_identity_t *identity, unsigned generation) {
    if (cancelled(generation) || busy() || !muse_avatar_files_mount()) return false;
    mkdir("/files/.muse-avatar-v1", 0700);
    avatar_cache_header_t h;
    avatar_cache_header_init(&h, s_cache_sequence + 1, image->view.data.width,
                             image->view.data.height, s_binding, identity, image->view.data.pixels);
    int slot = s_cache_slot == 0 ? 1 : 0;
    bool ok = avatar_cache_write_while(slot ? CACHE_B : CACHE_A, &h, image->view.data.pixels, cache_proceed, &generation);
    if (ok && !cancelled(generation)) {
        s_cache_sequence = h.sequence; s_cache_slot = slot;
        cached_identity(identity); atomic_fetch_add(&s_cache_writes, 1); return true;
    }
    return false;
}
static bool sync_once(unsigned generation) {
    uint32_t content_epoch = atomic_load(&s_content_epoch);
    int64_t started = esp_timer_get_time();
    atomic_fetch_add(&s_identity_reads, 1);
    avatar_response_t *r = get("/identity", NULL, 8192, generation);
    if (!r) return false;
    avatar_identity_t identity;
    int available = avatar_identity_parse((const char *)r->body, r->len, &identity);
    avatar_request_release(r);
    if (cancelled(generation)) return false;
    /* image_exists can be false while an asset is being generated. It does
     * not prove that the person chose the built-in character. */
    if (available == 0) return false;
    if (available != 1) return false;
#if CONFIG_MUSE_PIXEL_AVATAR
    /* A known new generation invalidates a pixel pack even if downloading
     * its still later fails. Keep the last good still as a separate fallback. */
    pixel_identity(&identity);
#endif
    atomic_store(&s_path_id, avatar_cache_crc(identity.path, strlen(identity.path)));
    atomic_store(&s_revision_id, avatar_cache_crc(identity.revision, strlen(identity.revision)));
    if (s_image && identity.revision[0] && !strcmp(identity.path, s_identity.path) && !strcmp(identity.revision, s_identity.revision)) {
        atomic_store(&s_status, "Up to date"); return true;
    }
    size_t len = 0;
    uint8_t *bytes = fetch_webp(identity.path, generation, &len);
    if (!bytes) return false;
    avatar_webp_result_t decoded;
    bool ok = avatar_webp_decode(bytes, len, 466, AVATAR_PEAK - len - 16 * 1024, &decoded);
    heap_caps_free(bytes);
    atomic_store(&s_last_peak, decoded.peak_bytes + len + 16 * 1024);
    if (!ok) {
        ESP_LOGW("link.avatar", "decode %s; peak=%u", decoded.error, (unsigned)decoded.peak_bytes);
        return false;
    }
    if (cancelled(generation)) { muse_webp_free(decoded.pixels); return false; }
    image_t *image = new_image(decoded.pixels, decoded.width, decoded.height, content_epoch);
    if (!image) return false;
#if CONFIG_MUSE_PIXEL_AVATAR
    if(!set_image_pixel_key(image,&identity)) {muse_avatar_release(&image->view);return false;}
#endif
    bool same = s_image && image->crc == s_image->crc && decoded.width == s_image->view.data.width && decoded.height == s_image->view.data.height &&
                !memcmp(image->view.data.pixels, s_image->view.data.pixels, (size_t)decoded.width * decoded.height * 2);
    bool saved = same ? s_image_saved : cache_save(image, &identity, generation);
    if (same && saved && !busy() && !cancelled(generation) &&
        (strcmp(identity.path, s_identity.path) || strcmp(identity.revision, s_identity.revision))) {
        avatar_cache_header_t h;
        avatar_cache_header_init(&h, s_cache_sequence, decoded.width, decoded.height, s_binding, &identity, decoded.pixels);
        if (avatar_cache_identity_write(CACHE_META, CACHE_META_TMP, &h)) {
            cached_identity(&identity); atomic_fetch_add(&s_cache_writes, 1);
        }
    }
    if (cancelled(generation)) { muse_avatar_release(&image->view); return false; }
#if CONFIG_MUSE_PIXEL_AVATAR
    bool same_key=s_image && s_image->pixel_key_valid && !memcmp(s_image->pixel_key,image->pixel_key,32);
    if (same && same_key) muse_avatar_release(&image->view); else publish(image);
#else
    if (same) muse_avatar_release(&image->view); else publish(image);
#endif
    s_image_saved = saved;
    s_identity = identity; motion_binding(&s_identity);
    atomic_store(&s_last_ms, (int)((esp_timer_get_time() - started) / 1000));
    atomic_store(&s_status, saved ? "Up to date" : "Up to date (not saved)");
    ESP_LOGI("link.avatar", "ready %dx%d; ms=%d peak=%u saved=%d", decoded.width, decoded.height,
             atomic_load(&s_last_ms), (unsigned)atomic_load(&s_last_peak), saved);
    return true;
}

static void worker_task(void *arg) {
    (void)arg;
    int64_t next = 0;
    unsigned retry_s = 60;
    bool loaded = false, connected = false;
    avatar_webp_set_budget(AVATAR_PEAK);
    for (;;) {
        unsigned jobs = atomic_exchange(&s_jobs, 0);
        unsigned generation = atomic_load(&s_generation);
        muse_avatar_source_t source = muse_avatar_source();
        if (source != muse_settings_avatar_source() && muse_settings_set_avatar_source(source) != ESP_OK) {
            unsigned expected = source;
            if (atomic_compare_exchange_strong(&s_source, &expected, muse_settings_avatar_source())) {
                atomic_fetch_add(&s_generation, 1); atomic_store(&s_status, "Could not save setting");
#if CONFIG_MUSE_PIXEL_AVATAR
                muse_pixel_avatar_source_changed();
#endif
            }
            continue;
        }
        if ((jobs & JOB_BINDING) || !s_have_binding) {
            uint8_t current[32];
            bool have = binding(current);
            if (!have || !s_have_binding || memcmp(current, s_binding, 32)) {
                atomic_fetch_add(&s_content_epoch, 1);
                publish(NULL); memset(&s_identity, 0, sizeof(s_identity)); loaded = false;
                s_cache_slot = -1; s_cache_sequence = 0; next = 0;
            }
            s_have_binding = have;
            if (have) memcpy(s_binding, current, 32);
            atomic_store(&s_binding_blocked, !have);
        }
        if (jobs & JOB_CLEAR) { publish(NULL); cache_clear(); memset(&s_identity, 0, sizeof(s_identity)); loaded = true; next = 0; }
        if (source == MUSE_AVATAR_BUILTIN) { publish(NULL); memset(&s_identity, 0, sizeof(s_identity)); atomic_store(&s_status, "Built-in"); break; }
        if (s_have_binding && !loaded && !busy()) { cache_load(); loaded = true; }
        bool online = noise_ctrl_is_connected();
        if (jobs & JOB_REFRESH) next = 0;
        if (online && !connected) { next = 0; atomic_fetch_or(&s_jobs, JOB_BINDING); }
        connected = online;
        if (!s_have_binding) atomic_store(&s_status, "Waiting for Muse");
        if (s_have_binding && online && !muse_state_asleep() && !busy() &&
            (esp_timer_get_time() >= next || (jobs & JOB_REFRESH))) {
            atomic_store(&s_status, "Updating");
            bool ok = sync_once(generation);
            if (ok) { atomic_fetch_add(&s_completions, 1); retry_s = 60; next = esp_timer_get_time() + 900000000; }
            else {
                atomic_store(&s_status, s_image ? "Using saved avatar" : "Unavailable");
                if (cancelled(generation) || muse_state_asleep() || busy()) next = 0;
                else {
                    next = esp_timer_get_time() + (int64_t)(retry_s + esp_random() % 15) * 1000000;
                    retry_s = retry_s < 1800 ? retry_s * 2 : 3600;
                }
            }
        }
        if (muse_state_asleep() && !atomic_load(&s_jobs)) muse_state_wait_awake(UINT32_MAX);
        else vTaskDelay(pdMS_TO_TICKS(1000));
    }
    atomic_store(&s_worker_busy, false);
    if (atomic_load(&s_jobs) || muse_avatar_source() == MUSE_AVATAR_PERSONAL) worker_kick();
    vTaskDeleteWithCaps(NULL);
}
static void worker_kick(void) {
    if (!atomic_load(&s_ready)) return;
    muse_state_nudge();
    bool expected = false;
    if (!atomic_compare_exchange_strong(&s_worker_busy, &expected, true)) return;
    if (xTaskCreateWithCaps(worker_task, "muse_avatar", 8192, NULL, 2, NULL, AVATAR_TASK_CAPS) != pdPASS) {
        atomic_store(&s_worker_busy, false); atomic_store(&s_status, "No memory");
    }
}
void muse_avatar_start(void) {
    atomic_store(&s_source, muse_settings_avatar_source()); atomic_store(&s_ready, true);
#if CONFIG_MUSE_PIXEL_AVATAR
    muse_pixel_avatar_start();
#endif
    if (muse_avatar_source() == MUSE_AVATAR_PERSONAL || (atomic_load(&s_jobs) & JOB_CLEAR)) worker_kick();
}
muse_avatar_source_t muse_avatar_source(void) { return atomic_load(&s_source); }
void muse_avatar_select(muse_avatar_source_t source) {
    if (source != MUSE_AVATAR_BUILTIN && source != MUSE_AVATAR_PERSONAL) return;
    bool changed=atomic_load(&s_source)!=(unsigned)source;
    atomic_store(&s_source, source); atomic_fetch_add(&s_generation, 1);
#if CONFIG_MUSE_PIXEL_AVATAR
    if(changed)muse_pixel_avatar_source_changed();
#else
    (void)changed;
#endif
    atomic_store(&s_status, source == MUSE_AVATAR_PERSONAL ? "Updating" : "Built-in");
    atomic_fetch_or(&s_jobs, JOB_REFRESH); worker_kick();
}
void muse_avatar_refresh(void) { if (muse_avatar_source() == MUSE_AVATAR_PERSONAL) { atomic_fetch_or(&s_jobs, JOB_REFRESH); worker_kick(); } }
uint32_t muse_avatar_content_epoch(void) {
    uint32_t epoch = atomic_load(&s_content_epoch);
    return !atomic_load(&s_binding_blocked) && atomic_load(&s_metadata_epoch) == epoch ? epoch : 0;
}
void muse_avatar_clear(void) {
#if CONFIG_MUSE_PIXEL_AVATAR
    muse_pixel_avatar_clear();
#endif
    motion_binding(NULL); atomic_fetch_add(&s_content_epoch, 1); atomic_fetch_add(&s_generation, 1); atomic_fetch_or(&s_jobs, JOB_CLEAR); worker_kick();
}
void muse_avatar_pairing_changed(void) {
#if CONFIG_MUSE_PIXEL_AVATAR
    muse_pixel_avatar_invalidate();
#endif
    motion_binding(NULL);
    atomic_store(&s_binding_blocked, true); atomic_fetch_add(&s_generation, 1);
    atomic_fetch_add(&s_content_epoch, 1);
    atomic_fetch_or(&s_jobs, JOB_BINDING);
    if (muse_avatar_source() == MUSE_AVATAR_PERSONAL) worker_kick();
}
const char *muse_avatar_status(void) { return atomic_load(&s_status); }
#endif
