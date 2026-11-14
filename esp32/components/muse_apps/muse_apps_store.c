/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * The installed apps' task: app.install downloads a module from the VM (a
 * .wasm file, or an app's folder holding app.wasm) and installs it, taking
 * the id, name, icon and colour it isn't given from the app's manifest, app.remove and the launcher remove one, app.run and the
 * launcher read one to run. All of it, and every flash write, happens here,
 * one request at a time, so a running app keeps its frames while an install
 * downloads. Internal RAM is scarce (audio's DMA and TLS need what's left), so
 * the lists, the queue and, where flash writes allow, the stack are in PSRAM.
 * The catalog (muse_apps_catalog.c) lives in the "apps" LittleFS partition; a
 * table without it (an over-the-air update keeps the table a board had) keeps
 * apps in memory until restart. Other tasks read a copy of the list.
 *
 * The gadget's own files (files.put, files.list, files.remove) are this
 * task's too, in the "files" partition (muse_apps_flash.c): files.put streams
 * a file from the VM to flash a frame at a time. Apps read them through a
 * second, small task, so a read doesn't wait out an install or a download.
 */

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/md.h"
#include "muse_apps_flash.h"
#include "nvs.h"
#include "muse_apps_priv.h"

static const char *TAG = "muse_apps_store";

#define LABEL "apps"
#define ROOT "/apps"
#define APP_MAX_BYTES (256 * 1024)
#define FETCH_TIMEOUT_MS 30000
#define TASK_STACK_BYTES 8192
// A flash write turns the caches off unless code and read-only data run from
// PSRAM (XIP), and with them off a stack in PSRAM can't be reached.
#if CONFIG_SPIRAM_FETCH_INSTRUCTIONS && CONFIG_SPIRAM_RODATA
#define TASK_STACK_CAPABILITIES (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#else
#define TASK_STACK_CAPABILITIES (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
#endif
#define TASK_PRIORITY 2
#define QUEUE_LEN 4

#define FILES_LABEL "files"
#define FILES_ROOT "/files"
#define FILE_MAX_BYTES (3 * 1024 * 1024)
#define FILE_TIMEOUT_MS 240000        // a 3 MB file takes about a minute over Link
#define FILE_SLACK 8192               // LittleFS's own blocks for a file
#define READER_STACK_BYTES 4096
#define READER_QUEUE_LEN 4

typedef enum { ST_INSTALL, ST_REMOVE, ST_OPEN, ST_FORGET, ST_FILE_PUT, ST_FILE_REMOVE, ST_FILE_LIST } st_kind_t;

typedef struct {
    st_kind_t kind;
    muse_apps_origin_t o;
    char id[MUSE_APPS_ID_MAX];
    char name[MUSE_APPS_NAME_MAX];
    char icon[MUSE_APPS_LOOK_MAX], color[MUSE_APPS_LOOK_MAX];
    char path[192];
    char sha256[MUSE_APPS_SHA_HEX + 1];
    char file[MUSE_APPS_FLASH_NAME_MAX];  // ST_FILE_*: the file's name in the partition
    uint8_t *bytes;  // ST_INSTALL, ST_FILE_PUT from the console's upload: no download; owned by the request
    size_t len;
    uint32_t epoch;  // s_epoch when it was asked
} st_req_t;

static QueueHandle_t s_q;
static SemaphoreHandle_t s_lock;  // the copy below
static EXT_RAM_BSS_ATTR muse_apps_entry_t s_list[MUSE_APPS_MAX];
static int s_list_n;
static bool s_persistent;
static bool s_files;  // the "files" partition is mounted
static atomic_bool s_forget;  // unpaired: remove everything before anything else
static atomic_uint s_epoch;   // moves at each unpairing: what was asked before it isn't done
static atomic_bool s_ready;   // the last run's unpairing, if any, is wiped: files may be read
static atomic_bool s_unwiped; // an unpairing's files aren't all gone yet: none are read

// Copies the catalog for other tasks, after each change.
static void publish(void) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_list_n = muse_apps_catalog_count();
    for (int i = 0; i < s_list_n; i++) s_list[i] = *muse_apps_catalog_get(i);
    xSemaphoreGive(s_lock);
}

int muse_apps_store_list(muse_apps_entry_t *out, int max) {
    if (!s_lock) return 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int n = s_list_n < max ? s_list_n : max;
    memcpy(out, s_list, n * sizeof(*out));
    xSemaphoreGive(s_lock);
    return n;
}

cJSON *muse_apps_store_list_result(void) {
    static EXT_RAM_BSS_ATTR muse_apps_entry_t apps[MUSE_APPS_MAX];  // Link's task only
    int n = muse_apps_store_list(apps, MUSE_APPS_MAX);
    cJSON *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "ok", true);
    cJSON *payload = cJSON_AddObjectToObject(result, "payload");
    cJSON *list = cJSON_AddArrayToObject(payload, "apps");
    for (int i = 0; i < n; i++) {
        cJSON *app = cJSON_CreateObject();
        cJSON_AddStringToObject(app, "id", apps[i].id);
        cJSON_AddStringToObject(app, "name", apps[i].name);
        cJSON_AddStringToObject(app, "sha256", apps[i].sha256);
        cJSON_AddNumberToObject(app, "bytes", apps[i].bytes);
        if (apps[i].icon[0]) cJSON_AddStringToObject(app, "icon", apps[i].icon);
        if (apps[i].color[0]) cJSON_AddStringToObject(app, "color", apps[i].color);
        cJSON_AddItemToArray(list, app);
    }
    char running[MUSE_APPS_ID_MAX];
    muse_apps_running_id(running, sizeof(running));
    if (running[0]) {
        cJSON_AddStringToObject(payload, "running", running);
    } else {
        cJSON_AddNullToObject(payload, "running");
    }
    cJSON_AddBoolToObject(payload, "kept_after_restart", s_persistent);
    size_t total = 0, used = 0;
    if (s_persistent && esp_littlefs_info(LABEL, &total, &used) == ESP_OK) {
        cJSON_AddNumberToObject(payload, "free_bytes", (double)(total > used ? total - used : 0));
    }
    return result;
}

// An unpairing's wipe, kept in NVS until it's done: the gadget may restart
// (as unpairing does, shortly after) before the store gets to it.
#define NVS_SPACE "muse_apps"
#define NVS_FORGET "forget"

static void forget_mark(bool on) {
    nvs_handle_t h;
    if (nvs_open(NVS_SPACE, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "couldn't %s the unpairing's wipe in NVS", on ? "record" : "clear");
        return;
    }
    if (on) {
        nvs_set_u8(h, NVS_FORGET, 1);
    } else {
        nvs_erase_key(h, NVS_FORGET);
    }
    nvs_commit(h);
    nvs_close(h);
}

static bool forget_marked(void) {
    nvs_handle_t h;
    uint8_t v = 0;
    esp_err_t err;
    // The store starts before the firmware initializes NVS: wait for it rather than miss a mark.
    while ((err = nvs_open(NVS_SPACE, NVS_READONLY, &h)) == ESP_ERR_NVS_NOT_INITIALIZED) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (err != ESP_OK) return false;   // never marked
    bool marked = nvs_get_u8(h, NVS_FORGET, &v) == ESP_OK && v;
    nvs_close(h);
    return marked;
}

// A partition that's there but didn't mount: whatever it holds is still there.
static bool unmounted(const char *label, bool mounted) {
    return !mounted && esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_LITTLEFS, label);
}

// Unpairing removes every app, before the next change: an install that was
// downloading meanwhile doesn't land for the next owner.
static bool forget_if_asked(void) {
    if (!atomic_exchange(&s_forget, false)) return false;
    muse_apps_stop_installed(NULL);
    bool apps_gone = muse_apps_catalog_wipe();
    publish();
    atomic_store(&s_unwiped, true);
    if (!apps_gone || (s_files && !muse_apps_flash_wipe(FILES_ROOT)) || unmounted(LABEL, s_persistent) ||
        unmounted(FILES_LABEL, s_files)) {
        // Something was still open, or a partition didn't mount: the mark stays, and the next start wipes again.
        ESP_LOGW(TAG, "unpaired: not everything could be removed yet; trying again at the next start");
        return true;
    }
    forget_mark(false);
    atomic_store(&s_unwiped, false);
    ESP_LOGI(TAG, "unpaired: every app and file removed");
    return true;
}

static size_t free_bytes(void) {
    size_t total = 0, used = 0;
    if (!s_persistent) return SIZE_MAX;
    if (esp_littlefs_info(LABEL, &total, &used) != ESP_OK) return 0;
    return total > used ? total - used : 0;
}

// A path to a .wasm file is that file; anything else is an app's folder.
static void module_path(const char *path, char *out, size_t cap) {
    size_t n = strlen(path);
    if (n >= 5 && !strcmp(path + n - 5, ".wasm")) {
        snprintf(out, cap, "%s", path);
    } else {
        snprintf(out, cap, "%.*s%sapp.wasm", (int)(cap > 12 ? cap - 12 : 0), path,
                 n && path[n - 1] == '/' ? "" : "/");
    }
}

static void install(const st_req_t *r) {
    char err[160] = "";
    uint8_t *data = r->bytes;
    size_t len = r->len;
    char file[sizeof(r->path) + 12];
    module_path(r->path, file, sizeof(file));
    if (!data && !muse_apps_vm_file_read(file, APP_MAX_BYTES, FETCH_TIMEOUT_MS, &data, &len, err, sizeof(err))) {
        muse_apps_reply(&r->o, muse_apps_error("fetch_failed", err));
        return;
    }
    // What the request leaves out comes from the app's manifest.
    char id[MUSE_APPS_ID_MAX], name[MUSE_APPS_NAME_MAX], icon[MUSE_APPS_LOOK_MAX], color[MUSE_APPS_LOOK_MAX];
    char m_name[MUSE_APPS_ID_MAX] = "", m_title[MUSE_APPS_NAME_MAX] = "", m_icon[MUSE_APPS_LOOK_MAX] = "",
         m_color[MUSE_APPS_LOOK_MAX] = "";
    muse_apps_manifest_info(data, len, m_name, sizeof(m_name), m_title, sizeof(m_title), m_icon, sizeof(m_icon),
                            m_color, sizeof(m_color));
    snprintf(id, sizeof(id), "%s", r->id[0] ? r->id : m_name);
    snprintf(name, sizeof(name), "%s", r->name[0] ? r->name : m_title);
    snprintf(icon, sizeof(icon), "%s", r->icon[0] ? r->icon : m_icon);
    snprintf(color, sizeof(color), "%s", r->color[0] ? r->color : m_color);
    if (!id[0]) {
        free(data);
        muse_apps_reply(&r->o, muse_apps_error("missing_param", "give an id, or build the app with a manifest naming it"));
        return;
    }
    if (forget_if_asked()) {
        free(data);
        muse_apps_reply(&r->o, muse_apps_error("unpaired", "the gadget was unpaired"));
        return;
    }
    bool changed = false;
    const char *code = muse_apps_catalog_install(id, name, icon, color, data, len, r->sha256, free_bytes(),
                                                 &changed, err, sizeof(err));
    free(data);
    if (code) {
        muse_apps_reply(&r->o, muse_apps_error(code, err));
        return;
    }
    publish();
    const muse_apps_entry_t *e = NULL;
    for (int i = 0; i < muse_apps_catalog_count() && !e; i++) {
        if (!strcmp(muse_apps_catalog_get(i)->id, id)) e = muse_apps_catalog_get(i);
    }
    cJSON *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "ok", true);
    cJSON *payload = cJSON_AddObjectToObject(result, "payload");
    cJSON_AddStringToObject(payload, "id", e->id);
    cJSON_AddStringToObject(payload, "name", e->name);
    cJSON_AddStringToObject(payload, "sha256", e->sha256);
    cJSON_AddNumberToObject(payload, "bytes", e->bytes);
    if (e->icon[0]) cJSON_AddStringToObject(payload, "icon", e->icon);
    if (e->color[0]) cJSON_AddStringToObject(payload, "color", e->color);
    cJSON_AddBoolToObject(payload, "changed", changed);
    muse_apps_reply(&r->o, result);
}

static void remove_app(const st_req_t *r) {
    muse_apps_stop_installed(r->id);
    char err[160] = "";
    bool removed = false;
    const char *code = muse_apps_catalog_remove(r->id, &removed, err, sizeof(err));
    if (code) {
        muse_apps_reply(&r->o, muse_apps_error(code, err));
        return;
    }
    publish();
    cJSON *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "ok", true);
    cJSON_AddBoolToObject(cJSON_AddObjectToObject(result, "payload"), "removed", removed);
    muse_apps_reply(&r->o, result);
}

static void open_app(const st_req_t *r) {
    char err[160] = "";
    uint8_t *data = NULL;
    size_t len = 0;
    const char *code = muse_apps_catalog_read(r->id, &data, &len, err, sizeof(err));
    if (code) {
        publish();  // a damaged app is gone
        muse_apps_open_failed(&r->o, code, err);
        return;
    }
    char name[MUSE_APPS_NAME_MAX] = "";
    for (int i = 0; i < muse_apps_catalog_count(); i++) {
        if (!strcmp(muse_apps_catalog_get(i)->id, r->id)) strcpy(name, muse_apps_catalog_get(i)->name);
    }
    muse_apps_run_installed(&r->o, r->id, name, data, len);
}

// ---- The gadget's own files ----

static size_t files_free(void) {
    size_t total = 0, used = 0;
    if (!s_files || esp_littlefs_info(FILES_LABEL, &total, &used) != ESP_OK) return 0;
    return total > used ? total - used : 0;
}

static bool no_files(const st_req_t *r) {
    if (s_files) return false;
    muse_apps_reply(&r->o, muse_apps_error("no_storage",
                                           "this gadget has no files partition: flash it over USB to get one"));
    return true;
}

typedef struct {
    FILE *f;
    mbedtls_md_context_t md;
} sink_t;

static bool write_piece(void *ctx, const uint8_t *data, size_t len) {
    sink_t *s = ctx;
    mbedtls_md_update(&s->md, data, len);
    return fwrite(data, 1, len, s->f) == len;
}

static void hex(const uint8_t sum[32], char out[MUSE_APPS_SHA_HEX + 1]) {
    for (int i = 0; i < 32; i++) {
        out[i * 2] = "0123456789abcdef"[sum[i] >> 4];
        out[i * 2 + 1] = "0123456789abcdef"[sum[i] & 15];
    }
    out[MUSE_APPS_SHA_HEX] = '\0';
}

static void file_put(const st_req_t *r) {
    char err[160] = "", sha[MUSE_APPS_SHA_HEX + 1] = "";
    bool replaced = false;
    size_t len = r->len;
    const char *code = NULL;
    size_t room = files_free();
    if (r->bytes) {
        muse_apps_sha256_hex(r->bytes, r->len, sha);
        if (r->sha256[0] && strcasecmp(r->sha256, sha)) {
            code = "hash_mismatch";
            snprintf(err, sizeof(err), "its SHA-256 is %s", sha);
        } else {
            code = muse_apps_flash_put(FILES_ROOT, r->file, r->bytes, r->len, room, &replaced, err, sizeof(err));
        }
    } else {
        size_t max = room > FILE_SLACK ? room - FILE_SLACK : 0;
        if (max > FILE_MAX_BYTES) max = FILE_MAX_BYTES;
        sink_t sink = {0};
        char tmp[MUSE_APPS_FLASH_NAME_MAX + 32];
        mbedtls_md_init(&sink.md);
        bool md_ok = mbedtls_md_setup(&sink.md, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 0) == 0 &&
                     mbedtls_md_starts(&sink.md) == 0;
        code = md_ok ? muse_apps_flash_create(FILES_ROOT, r->file, &sink.f, tmp, sizeof(tmp), err, sizeof(err))
                     : "storage_failed";
        if (!code) {
            bool ok = muse_apps_vm_file_stream(r->path, max, FILE_TIMEOUT_MS, write_piece, &sink, &len, err,
                                               sizeof(err));
            ok = (fclose(sink.f) == 0) && ok;
            uint8_t sum[32];
            if (ok && mbedtls_md_finish(&sink.md, sum) == 0) hex(sum, sha);
            if (!ok) {
                muse_apps_flash_abort(tmp);
                code = strstr(err, "larger than") ? (max < FILE_MAX_BYTES ? "no_space" : "too_large")
                       : strstr(err, "store") ? "no_space" : "fetch_failed";
            } else if (r->sha256[0] && strcasecmp(r->sha256, sha)) {
                muse_apps_flash_abort(tmp);
                code = "hash_mismatch";
                snprintf(err, sizeof(err), "its SHA-256 is %s", sha);
            } else if (forget_if_asked()) {
                mbedtls_md_free(&sink.md);
                muse_apps_flash_abort(tmp);
                muse_apps_reply(&r->o, muse_apps_error("unpaired", "the gadget was unpaired"));
                return;
            } else {
                code = muse_apps_flash_commit(FILES_ROOT, r->file, tmp, &replaced, err, sizeof(err));
            }
        }
        mbedtls_md_free(&sink.md);
    }
    if (code) {
        muse_apps_reply(&r->o, muse_apps_error(code, err));
        return;
    }
    cJSON *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "ok", true);
    cJSON *payload = cJSON_AddObjectToObject(result, "payload");
    cJSON_AddStringToObject(payload, "name", r->file);
    cJSON_AddNumberToObject(payload, "bytes", (double)len);
    cJSON_AddStringToObject(payload, "sha256", sha);
    cJSON_AddBoolToObject(payload, "replaced", replaced);
    cJSON_AddNumberToObject(payload, "free_bytes", (double)files_free());
    muse_apps_reply(&r->o, result);
}

static void file_remove(const st_req_t *r) {
    char err[160] = "";
    bool removed = false;
    const char *code = muse_apps_flash_remove(FILES_ROOT, r->file, &removed, err, sizeof(err));
    if (code) {
        muse_apps_reply(&r->o, muse_apps_error(code, err));
        return;
    }
    cJSON *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "ok", true);
    cJSON *payload = cJSON_AddObjectToObject(result, "payload");
    cJSON_AddBoolToObject(payload, "removed", removed);
    cJSON_AddNumberToObject(payload, "free_bytes", (double)files_free());
    muse_apps_reply(&r->o, result);
}

static void file_list(const st_req_t *r) {
    size_t total = 0, used = 0, files_bytes = 0;
    bool more = false;
    cJSON *list = muse_apps_flash_list(FILES_ROOT, &files_bytes, &more);
    esp_littlefs_info(FILES_LABEL, &total, &used);
    cJSON *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "ok", true);
    cJSON *payload = cJSON_AddObjectToObject(result, "payload");
    cJSON_AddItemToObject(payload, "files", list);
    if (more) cJSON_AddBoolToObject(payload, "more", true);
    cJSON_AddNumberToObject(payload, "files_bytes", (double)files_bytes);
    cJSON_AddNumberToObject(payload, "free_bytes", (double)(total > used ? total - used : 0));
    cJSON_AddNumberToObject(payload, "total_bytes", (double)total);
    muse_apps_reply(&r->o, result);
}

// ---- Apps' reads, on the reader task ----

typedef struct {
    SemaphoreHandle_t done;
    uint8_t *data;
    size_t len;
    int32_t err;
} load_wait_t;

typedef struct {
    char name[MUSE_APPS_FLASH_NAME_MAX];
    size_t offset, max;
    int32_t op;        // file_read's operation, or
    load_wait_t *wait; // a blocked caller's
} rd_req_t;

static QueueHandle_t s_rq;

static int32_t gadget_error(const char *code) {
    if (!strcmp(code, "not_found")) return GADGET_ERR_NOT_FOUND;
    if (!strcmp(code, "too_large")) return GADGET_ERR_TOO_LARGE;
    if (!strcmp(code, "bad_name")) return GADGET_ERR_ARG;
    return GADGET_ERR_FAILED;
}

static void reader_task(void *arg) {
    (void)arg;
    for (rd_req_t r;;) {
        if (!xQueueReceive(s_rq, &r, portMAX_DELAY)) continue;
        uint8_t *data = NULL;
        size_t len = 0;
        int32_t result = GADGET_ERR_UNSUPPORTED;
        if (s_files && (!atomic_load(&s_ready) || atomic_load(&s_unwiped))) {
            result = GADGET_ERR_FAILED;   // possibly the last owner's: not until they're wiped
        } else if (s_files) {
            char err[96];
            const char *code = muse_apps_flash_read(FILES_ROOT, r.name, r.offset, r.max, &data, &len, err, sizeof(err));
            result = code ? gadget_error(code) : (int32_t)len;
        }
        if (r.wait) {
            r.wait->data = data;
            r.wait->len = len;
            r.wait->err = result < 0 ? result : 0;
            xSemaphoreGive(r.wait->done);
        } else {
            muse_apps_op_finish(r.op, result, result > 0 ? data : NULL, result > 0 ? len : 0);
            if (result <= 0) free(data);
        }
    }
}

bool muse_apps_store_file_read(const char *name, size_t offset, size_t max, int32_t op) {
    rd_req_t r = {.offset = offset, .max = max, .op = op};
    snprintf(r.name, sizeof(r.name), "%s", name);
    return s_rq && xQueueSend(s_rq, &r, 0) == pdTRUE;
}

int32_t muse_apps_store_file_load(const char *name, size_t max, uint8_t **out, size_t *len) {
    *out = NULL;
    *len = 0;
    load_wait_t w = {.done = xSemaphoreCreateBinary()};
    rd_req_t r = {.max = max, .wait = &w};
    snprintf(r.name, sizeof(r.name), "%s", name);
    if (!w.done) return GADGET_ERR_NO_MEMORY;
    if (!s_rq || xQueueSend(s_rq, &r, pdMS_TO_TICKS(1000)) != pdTRUE) {
        vSemaphoreDelete(w.done);
        return GADGET_ERR_BUSY;
    }
    xSemaphoreTake(w.done, portMAX_DELAY);  // a flash read: milliseconds
    vSemaphoreDelete(w.done);
    if (w.err) return w.err;
    *out = w.data;
    *len = w.len;
    return 0;
}

static void store_task(void *arg) {
    (void)arg;
    if (s_persistent) {
        muse_apps_catalog_open(ROOT);
    } else {
        muse_apps_catalog_open(NULL);
    }
    if (forget_marked()) {   // an unpairing the last run didn't finish wiping for
        atomic_store(&s_forget, true);
        forget_if_asked();
    }
    atomic_store(&s_ready, true);
    publish();
    for (st_req_t r;;) {
        if (!xQueueReceive(s_q, &r, portMAX_DELAY)) continue;
        forget_if_asked();
        if (r.kind != ST_FORGET && r.epoch != atomic_load(&s_epoch)) {
            // Asked before the gadget was unpaired: not for its next owner.
            if (r.kind == ST_OPEN) {
                muse_apps_open_failed(&r.o, "unpaired", "the gadget was unpaired");
            } else {
                muse_apps_reply(&r.o, muse_apps_error("unpaired", "the gadget was unpaired"));
            }
            free(r.bytes);
            continue;
        }
        switch (r.kind) {
        case ST_INSTALL: install(&r); break;
        case ST_REMOVE: remove_app(&r); break;
        case ST_OPEN: open_app(&r); break;
        case ST_FORGET: break;
        case ST_FILE_PUT:
            if (!no_files(&r)) file_put(&r);
            free(r.bytes);
            break;
        case ST_FILE_REMOVE:
            if (!no_files(&r)) file_remove(&r);
            break;
        case ST_FILE_LIST:
            if (!no_files(&r)) file_list(&r);
            break;
        }
    }
}

static bool mount(const char *label, const char *root, const char *without) {
    if (!esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_LITTLEFS, label)) {
        ESP_LOGW(TAG, "no \"%s\" partition: %s", label, without);
        return false;
    }
    // A new partition is blank and has to be formatted, once.
    const esp_vfs_littlefs_conf_t conf = {
        .base_path = root, .partition_label = label, .format_if_mount_failed = true};
    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "can't mount the \"%s\" partition (%s): %s", label, esp_err_to_name(err), without);
        return false;
    }
    size_t total = 0, used = 0;
    esp_littlefs_info(label, &total, &used);
    ESP_LOGI(TAG, "%s partition: %u of %u KB used", label, (unsigned)(used / 1024), (unsigned)(total / 1024));
    return true;
}

void muse_apps_store_start(void) {
    s_lock = xSemaphoreCreateMutex();
    s_q = xQueueCreateWithCaps(QUEUE_LEN, sizeof(st_req_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_lock || !s_q) {
        ESP_LOGE(TAG, "no memory for the store");
        if (s_q) vQueueDeleteWithCaps(s_q);   // without it, requests are refused rather than left unanswered
        s_q = NULL;
        return;
    }
    s_persistent = mount(LABEL, ROOT, "installed apps last until restart");
    s_files = mount(FILES_LABEL, FILES_ROOT, "no files on the gadget");
    if (s_files) muse_apps_flash_tidy(FILES_ROOT);
    // Without its task a queue would take requests nobody answers: drop it, and they're refused.
    if (xTaskCreateWithCaps(store_task, "muse_apps_store", TASK_STACK_BYTES, NULL, TASK_PRIORITY, NULL,
                            TASK_STACK_CAPABILITIES) != pdPASS) {
        ESP_LOGE(TAG, "couldn't start the store task");
        vQueueDeleteWithCaps(s_q);
        s_q = NULL;
    }
    s_rq = xQueueCreateWithCaps(READER_QUEUE_LEN, sizeof(rd_req_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_rq || xTaskCreateWithCaps(reader_task, "muse_apps_files", READER_STACK_BYTES, NULL, TASK_PRIORITY, NULL,
                                     TASK_STACK_CAPABILITIES) != pdPASS) {
        ESP_LOGE(TAG, "couldn't start the files' reader task");
        if (s_rq) vQueueDeleteWithCaps(s_rq);
        s_rq = NULL;
    }
}

static bool post(st_req_t *r) {
    r->epoch = atomic_load(&s_epoch);
    return s_q && xQueueSend(s_q, r, 0) == pdTRUE;
}

static void set_app(st_req_t *r, const char *id, const char *name, const char *icon, const char *color) {
    snprintf(r->id, sizeof(r->id), "%s", id ? id : "");
    snprintf(r->name, sizeof(r->name), "%s", name ? name : "");
    snprintf(r->icon, sizeof(r->icon), "%s", icon ? icon : "");
    snprintf(r->color, sizeof(r->color), "%s", color ? color : "");
}

bool muse_apps_store_install(const muse_apps_origin_t *o, const char *id, const char *name, const char *icon,
                             const char *color, const char *path, const char *sha256) {
    st_req_t r = {.kind = ST_INSTALL, .o = *o};
    if (id && strlen(id) >= sizeof(r.id)) return false;   // cut short, it could replace another app
    set_app(&r, id, name, icon, color);
    snprintf(r.path, sizeof(r.path), "%s", path);
    snprintf(r.sha256, sizeof(r.sha256), "%s", sha256 ? sha256 : "");
    return post(&r);
}

bool muse_apps_store_install_bytes(const muse_apps_origin_t *o, const char *id, const char *name, const char *icon,
                                   const char *color, uint8_t *bytes, size_t len) {
    st_req_t r = {.kind = ST_INSTALL, .o = *o, .bytes = bytes, .len = len};
    if (id && strlen(id) >= sizeof(r.id)) return false;
    set_app(&r, id, name, icon, color);
    return post(&r);
}

bool muse_apps_store_remove(const muse_apps_origin_t *o, const char *id) {
    st_req_t r = {.kind = ST_REMOVE, .o = *o};
    if (strlen(id) >= sizeof(r.id)) return false;   // cut short, it could name another app
    snprintf(r.id, sizeof(r.id), "%s", id);
    return post(&r);
}

bool muse_apps_store_open(const muse_apps_origin_t *o, const char *id) {
    st_req_t r = {.kind = ST_OPEN, .o = *o};
    if (strlen(id) >= sizeof(r.id)) return false;
    snprintf(r.id, sizeof(r.id), "%s", id);
    return post(&r);
}

bool muse_apps_store_file_put(const muse_apps_origin_t *o, const char *path, const char *name, const char *sha256) {
    st_req_t r = {.kind = ST_FILE_PUT, .o = *o};
    snprintf(r.path, sizeof(r.path), "%s", path);
    snprintf(r.file, sizeof(r.file), "%s", name);
    snprintf(r.sha256, sizeof(r.sha256), "%s", sha256 ? sha256 : "");
    return post(&r);
}

bool muse_apps_store_file_put_bytes(const muse_apps_origin_t *o, const char *name, uint8_t *bytes, size_t len) {
    st_req_t r = {.kind = ST_FILE_PUT, .o = *o, .bytes = bytes, .len = len};
    snprintf(r.file, sizeof(r.file), "%s", name);
    return post(&r);
}

bool muse_apps_store_file_remove(const muse_apps_origin_t *o, const char *name) {
    st_req_t r = {.kind = ST_FILE_REMOVE, .o = *o};
    snprintf(r.file, sizeof(r.file), "%s", name);
    return post(&r);
}

bool muse_apps_store_file_list(const muse_apps_origin_t *o) {
    st_req_t r = {.kind = ST_FILE_LIST, .o = *o};
    return post(&r);
}

void muse_apps_store_forget_all(void) {
    atomic_fetch_add(&s_epoch, 1);
    atomic_store(&s_unwiped, true);
    forget_mark(true);   // before anything: kept across a restart until the wipe is done
    atomic_store(&s_forget, true);
    st_req_t r = {.kind = ST_FORGET};
    if (s_q) xQueueSend(s_q, &r, 0);  // full: the flag is seen after the request in hand
}
