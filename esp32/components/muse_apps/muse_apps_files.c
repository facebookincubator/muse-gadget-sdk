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
 * The API's file_read and state_* groups: files on Muse's VM, through the VM
 * worker (muse_apps_vm.h). Reads are GET /fs/raw/ Range requests of at most a
 * frame each; a state file is written whole in one POST /fs/write, its body
 * sent in pieces, so a save replaces the last one or fails without leaving
 * half a file.
 */

#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "mbedtls/base64.h"
#include "muse_apps_flash.h"
#include "muse_apps_priv.h"
#include "muse_apps_vm.h"
#include "muse_link.h"

static const char *TAG = "muse_apps_files";

#define READ_MAX (64 * 1024)        /* limits.read_bytes */
#define STATE_MAX (64 * 1024)       /* limits.state_bytes */
#define NAME_CHARS 32               /* limits.name_chars */
#define STATE_NAMES 32              /* state files one app can have */
#define PATH_CAP 256
#define PIECE_TIMEOUT_MS 15000
#define WRITE_TIMEOUT_MS 30000

/* ---- The app's state files this run, for state_bytes ----
 * There's no list or stat route, so the total counts the files this run
 * saved or loaded. Allocated with the first state call, freed with the app. */

typedef struct {
    char name[NAME_CHARS + 1];
    uint32_t size;
} state_size_t;

static state_size_t *s_sizes;
static portMUX_TYPE s_sizes_mux = portMUX_INITIALIZER_UNLOCKED;

/* Under s_sizes_mux: name's entry, or a free one if make; NULL if none. */
static state_size_t *size_entry(const char *name, bool make)
{
    state_size_t *free_one = NULL;
    for (int i = 0; s_sizes && i < STATE_NAMES; i++) {
        if (!strcmp(s_sizes[i].name, name)) {
            return &s_sizes[i];
        }
        if (!free_one && !s_sizes[i].name[0]) {
            free_one = &s_sizes[i];
        }
    }
    if (make && free_one) {
        snprintf(free_one->name, sizeof(free_one->name), "%s", name);
        free_one->size = 0;
        return free_one;
    }
    return NULL;
}

#define NO_SIZE UINT32_MAX   /* no entry */

/* Room for name at len bytes: 0, or GADGET_ERR_TOO_LARGE, ERR_LIMIT or
 * ERR_NO_MEMORY. With prev, it's counted, and *prev is what it replaced. */
static int32_t sizes_take(const char *name, size_t len, uint32_t *prev)
{
    if (!s_sizes) {
        state_size_t *t = muse_apps_calloc(STATE_NAMES, sizeof(state_size_t));
        if (!t) {
            return GADGET_ERR_NO_MEMORY;
        }
        taskENTER_CRITICAL(&s_sizes_mux);
        if (!s_sizes) {
            s_sizes = t;
            t = NULL;
        }
        taskEXIT_CRITICAL(&s_sizes_mux);
        muse_apps_free(t);
    }
    int32_t r = 0;
    taskENTER_CRITICAL(&s_sizes_mux);
    size_t total = 0;
    bool room = false;
    for (int i = 0; s_sizes && i < STATE_NAMES; i++) {
        bool other = s_sizes[i].name[0] && strcmp(s_sizes[i].name, name);
        total += other ? s_sizes[i].size : 0;
        room |= !other;   /* its own entry, or a free one */
    }
    if (total + len > STATE_MAX) {
        r = GADGET_ERR_TOO_LARGE;
    } else if (!room) {
        r = GADGET_ERR_LIMIT;
    } else if (prev) {
        state_size_t *e = size_entry(name, false);
        *prev = e ? e->size : NO_SIZE;
        size_entry(name, true)->size = (uint32_t)len;
    }
    taskEXIT_CRITICAL(&s_sizes_mux);
    return r;
}

/* A failed save: back to what it was before. */
static void sizes_undo(const char *name, uint32_t prev)
{
    taskENTER_CRITICAL(&s_sizes_mux);
    state_size_t *e = size_entry(name, false);
    if (e && prev == NO_SIZE) {
        e->name[0] = '\0';
    } else if (e) {
        e->size = prev;
    }
    taskEXIT_CRITICAL(&s_sizes_mux);
}

/* A loaded file's size: any task. */
static void sizes_note(const char *name, size_t len)
{
    taskENTER_CRITICAL(&s_sizes_mux);
    state_size_t *e = size_entry(name, true);
    if (e) {
        e->size = (uint32_t)len;
    }
    taskEXIT_CRITICAL(&s_sizes_mux);
}

void muse_apps_files_stop(void)
{
    taskENTER_CRITICAL(&s_sizes_mux);
    state_size_t *t = s_sizes;
    s_sizes = NULL;
    taskEXIT_CRITICAL(&s_sizes_mux);
    muse_apps_free(t);
}

/* ---- Names and paths ---- */

static bool valid_name(const char *name)
{
    size_t n = name ? strlen(name) : 0;
    if (!n || n > NAME_CHARS || !strcmp(name, ".") || !strcmp(name, "..")) {
        return false;
    }
    for (const char *p = name; *p; p++) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '-' || *p == '_' || *p == '.')) {
            return false;
        }
    }
    return true;
}

/* <gadget's folder>/state/<app>/<name>, with the app's name made safe for a
 * path (one from a file name may hold anything). */
static bool state_path(const char *name, char *out, size_t cap)
{
    char app[48];
    size_t k = 0;
    for (const char *p = muse_apps_name(); *p && k < sizeof(app) - 1; p++) {
        char c = *p;
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
                  c == '_' || c == '.';
        app[k++] = ok ? c : '_';
    }
    app[k] = '\0';
    if (!k || !strcmp(app, ".") || !strcmp(app, "..")) {
        snprintf(app, sizeof(app), "app");
    }
    int n = snprintf(out, cap, "%s/state/%s/%s", muse_apps_device_dir(), app, name);
    return n > 0 && (size_t)n < cap;
}

/* ---- Reads: file_read and state_load ---- */

typedef struct {
    int32_t op;
    bool state;                 /* state_load: a file larger than the buffer is ERR_TOO_LARGE */
    char name[NAME_CHARS + 1];
    size_t offset, want;
    char path[PATH_CAP];
} read_job_t;

static void read_run(void *arg)
{
    read_job_t *j = arg;
    size_t ask = j->want + (j->state ? 1 : 0);   /* one byte more tells a state file too large */
    uint8_t *buf = heap_caps_malloc(ask, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    char *url = muse_apps_vm_raw_path(j->path);
    size_t piece_max = muse_link_req_max_body();
    size_t have = 0;
    int status = 0;
    int32_t result = buf && url ? 0 : GADGET_ERR_NO_MEMORY;
    while (!result && have < ask) {
        size_t piece = ask - have < piece_max ? ask - have : piece_max;
        char range[48];
        snprintf(range, sizeof(range), "bytes=%u-%u", (unsigned)(j->offset + have),
                 (unsigned)(j->offset + have + piece - 1));
        muse_apps_fetch_t f = {.verb = "GET", .path = url, .range = range, .reply = buf + have,
                               .reply_cap = piece, .timeout_ms = PIECE_TIMEOUT_MS};
        status = muse_apps_vm_fetch(&f);
        if (status == 206) {
            have += f.reply_len;
            if (f.reply_len < piece) {
                break;   /* the end of the file */
            }
        } else if (status == 200 && j->offset == 0 && have == 0 && !f.reply_cut) {
            have = f.reply_len;   /* the VM sent the whole file, which fits */
            break;
        } else if (status == 416) {
            break;   /* at or past the end */
        } else if (status == 200) {
            ESP_LOGW(TAG, "%s: the VM ignored Range", url);
            result = f.reply_cut ? GADGET_ERR_TOO_LARGE : GADGET_ERR_FAILED;
        } else {
            result = muse_apps_vm_error(status);
        }
    }
    if (!result && j->state && have > j->want) {
        result = GADGET_ERR_TOO_LARGE;
    }
    if (!result) {
        result = (int32_t)have;
        if (j->state) {
            sizes_note(j->name, have);
        }
    } else if (status != MUSE_APPS_VM_CANCELLED && !(j->state && result == GADGET_ERR_NOT_FOUND)) {
        muse_apps_log("%s %s: %s %d", j->state ? "state_load" : "file_read", j->state ? j->name : j->path,
                      status > 0 ? "HTTP" : "status", status);
    }
    if (result > 0) {
        muse_apps_op_finish(j->op, result, buf, have);   /* the runtime frees buf */
    } else {
        heap_caps_free(buf);
        muse_apps_op_finish(j->op, result, NULL, 0);
    }
    free(url);
    heap_caps_free(j);
}

static void read_drop(void *arg)
{
    read_job_t *j = arg;
    muse_apps_op_finish(j->op, GADGET_ERR_FAILED, NULL, 0);
    heap_caps_free(j);
}

static int32_t start_read(wasm_exec_env_t env, read_job_t *j, void *buf)
{
    int32_t op = muse_apps_op_begin(env, buf, (uint32_t)j->want);
    if (op < 0) {
        heap_caps_free(j);
        return op;
    }
    j->op = op;
    if (!muse_apps_vm_job(read_run, read_drop, j)) {
        heap_caps_free(j);
        muse_apps_op_finish(op, GADGET_ERR_NO_MEMORY, NULL, 0);
    }
    return op;
}

/* A path from "/": the gadget's own file, read on the store's reader task. */
static int32_t gadget_read(wasm_exec_env_t env, const char *path, int32_t offset, void *buf, uint32_t buf_size)
{
    char name[MUSE_APPS_FLASH_NAME_MAX];
    int32_t r = muse_apps_gadget_path(path, name, sizeof(name));
    if (r < 0) {
        return r;
    }
    uint32_t want = buf_size < READ_MAX ? buf_size : READ_MAX;
    int32_t op = muse_apps_op_begin(env, buf, want);
    if (op >= 0 && !muse_apps_store_file_read(name, (size_t)offset, want, op)) {
        muse_apps_op_finish(op, GADGET_ERR_BUSY, NULL, 0);
    }
    return op;
}

int32_t gadget_file_read(wasm_exec_env_t env, const char *path, int32_t offset, void *buf, uint32_t buf_size)
{
    if (offset < 0 || !buf || !buf_size) {
        return GADGET_ERR_ARG;
    }
    if (path && path[0] == '/') {
        return gadget_read(env, path, offset, buf, buf_size);
    }
    read_job_t *j = heap_caps_calloc(1, sizeof(*j), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!j) {
        return GADGET_ERR_NO_MEMORY;
    }
    int32_t r = muse_apps_vm_path(path, j->path, sizeof(j->path));
    if (r == 0 && !muse_link_req_ready()) {
        r = GADGET_ERR_OFFLINE;
    }
    if (r < 0) {
        heap_caps_free(j);
        return r;
    }
    j->offset = (size_t)offset;
    j->want = buf_size < READ_MAX ? buf_size : READ_MAX;
    return start_read(env, j, buf);
}

int32_t gadget_state_load(wasm_exec_env_t env, const char *name, void *buf, uint32_t buf_size)
{
    if (!valid_name(name) || !buf || !buf_size) {
        return GADGET_ERR_ARG;
    }
    read_job_t *j = heap_caps_calloc(1, sizeof(*j), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!j) {
        return GADGET_ERR_NO_MEMORY;
    }
    if (!state_path(name, j->path, sizeof(j->path))) {
        heap_caps_free(j);
        return GADGET_ERR_ARG;
    }
    if (!muse_link_req_ready()) {
        heap_caps_free(j);
        return GADGET_ERR_OFFLINE;
    }
    j->state = true;
    snprintf(j->name, sizeof(j->name), "%s", name);
    j->want = buf_size < STATE_MAX ? buf_size : STATE_MAX;
    return start_read(env, j, buf);
}

/* ---- state_save: one POST /fs/write ---- */

typedef struct {
    int32_t op;
    char name[NAME_CHARS + 1];
    uint32_t prev;              /* the size counted before, for a failed save */
    size_t data_len, len;
    char body[];                /* the request's JSON: the data is copied in, base64 */
} save_job_t;

static void save_run(void *arg)
{
    save_job_t *j = arg;
    muse_apps_fetch_t f = {.verb = "POST", .path = "/fs/write", .json = true, .body = j->body,
                           .body_len = j->len, .timeout_ms = WRITE_TIMEOUT_MS};
    int status = muse_apps_vm_fetch(&f);
    bool ok = status >= 200 && status < 300;
    /* Every write's status, so a refusal's cause shows (the error body is in the log too). */
    ESP_LOGI(TAG, "state_save %s: %s %d, %u bytes", j->name, status > 0 ? "HTTP" : "status", status,
             (unsigned)j->data_len);
    if (!ok && status != MUSE_APPS_VM_CANCELLED) {
        muse_apps_log("state_save %s: %s %d%s%s", j->name, status > 0 ? "HTTP" : "status", status,
                      f.error[0] ? ": " : "", f.error);
    }
    if (!ok) {
        sizes_undo(j->name, j->prev);
    }
    muse_apps_op_finish(j->op, ok ? 0 : muse_apps_vm_error(status), NULL, 0);
    heap_caps_free(j);
}

static void save_drop(void *arg)
{
    save_job_t *j = arg;
    sizes_undo(j->name, j->prev);
    muse_apps_op_finish(j->op, GADGET_ERR_FAILED, NULL, 0);
    heap_caps_free(j);
}

int32_t gadget_state_save(wasm_exec_env_t env, const char *name, const void *data, uint32_t data_len)
{
    if (!valid_name(name) || (!data && data_len)) {
        return GADGET_ERR_ARG;
    }
    if (data_len > STATE_MAX) {
        return GADGET_ERR_TOO_LARGE;
    }
    char path[PATH_CAP];
    if (!state_path(name, path, sizeof(path))) {
        return GADGET_ERR_ARG;
    }
    if (!muse_link_req_ready()) {
        return GADGET_ERR_OFFLINE;
    }
    size_t b64 = 4 * ((data_len + 2) / 3) + 1;
    size_t cap = 6 * strlen(path) + b64 + 128;
    save_job_t *j = heap_caps_malloc(sizeof(*j) + cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!j) {
        return GADGET_ERR_NO_MEMORY;
    }
    /* All three booleans, every time: the VM requires them. */
    char *o = j->body;
    o += sprintf(o, "{\"path\":\"");
    o += muse_apps_vm_json_escape(o, path, strlen(path));
    o += sprintf(o, "\",\"data_base64\":\"");
    size_t olen = 0;
    if (data_len) {
        mbedtls_base64_encode((unsigned char *)o, b64, &olen, data, data_len);
    }
    o += olen;
    o += sprintf(o, "\",\"overwrite\":true,\"append\":false,\"create_parent\":true}");
    j->len = (size_t)(o - j->body);
    j->data_len = data_len;
    snprintf(j->name, sizeof(j->name), "%s", name);
    int32_t r = sizes_take(name, data_len, NULL);
    int32_t op = r ? r : muse_apps_op_begin(env, NULL, 0);
    if (op < 0) {
        heap_caps_free(j);
        return op;
    }
    sizes_take(name, data_len, &j->prev);
    j->op = op;
    if (!muse_apps_vm_job(save_run, save_drop, j)) {
        sizes_undo(name, j->prev);
        heap_caps_free(j);
        muse_apps_op_finish(op, GADGET_ERR_NO_MEMORY, NULL, 0);
    }
    return op;
}
