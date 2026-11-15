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
 * Muse's VM, over Link's Noise session (muse_link_req_*, which Link backs with
 * noise_ctrl_req_* in main/noise_control.h), for apps: the whole files app.run
 * and the console's loader read and write, and the worker that runs the app's
 * own requests (chat, files, state; muse_apps_vm.h). Routes and bounds are the
 * gadget's own (AGENTS.md, Requests to Muse's VM from your code).
 *
 * Every request carries the headers Muse's own chat requests on this session
 * do (components/muse/muse_chat_link.c): an x-request-id and the x-app-id.
 * A refusal's status and the first bytes of its body go to the log.
 */

#include <ctype.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include "muse_apps_priv.h"
#include "muse_apps_vm.h"
#include "muse_link.h"

static const char *TAG = "muse_apps_vm";


/* ---- The gadget's headers ---- */

/* The app identity Muse's own chat requests on Link's session carry; the VM
 * depends on the name (AGENTS.md, Say Muse, never Hatch). */
#define APP_ID "hatch-web"
#define ERR_BYTES 128   /* of a refusal's body, for the log */

typedef struct {
    char id[40];
    const char *list[11];
} gadget_headers_t;

/* x-request-id and x-app-id, then Accept, Content-Type and Range as asked:
 * name, value pairs ending with NULL, as muse_link_req_open() takes them. */
static const char *const *gadget_headers(gadget_headers_t *h, const char *accept, bool json_body, const char *range) {
    snprintf(h->id, sizeof(h->id), "muse-app-%08" PRIx32 "%08" PRIx32, esp_random(), esp_random());
    int n = 0;
    h->list[n++] = "x-request-id";
    h->list[n++] = h->id;
    h->list[n++] = "x-app-id";
    h->list[n++] = APP_ID;
    if (accept) {
        h->list[n++] = "Accept";
        h->list[n++] = accept;
    }
    if (json_body) {
        h->list[n++] = "Content-Type";
        h->list[n++] = "application/json";
    }
    if (range) {
        h->list[n++] = "Range";
        h->list[n++] = range;
    }
    h->list[n] = NULL;
    return h->list;
}

/* The first bytes of a refusal's body, printable, for a log line. */
static void printable(char *out, size_t cap, const char *in, size_t n) {
    size_t k = 0;
    for (size_t i = 0; i < n && k + 1 < cap; i++) {
        unsigned char c = (unsigned char)in[i];
        out[k++] = c >= 0x20 && c < 0x7F ? (char)c : c == '\n' ? ' ' : '.';
    }
    if (cap) out[k] = '\0';
}

/* ---- Whole files: app.run's download and the console's upload ---- */

#define POLL_MS 20
#define PATH_PREFIX "/fs/raw/"
#define WRITE_PIECE 5700    // bytes a write: 7600 in base64, and the JSON around it under 8 KB
#define WRITE_REPLY 1024
#define SEND_WAIT_MS 5000

// The reply to the one request in flight, written on the Noise session task.
// Frames of an earlier request (cancelled, or timed out) carry an older
// generation and are dropped, so they never touch the buffer.
static struct {
    SemaphoreHandle_t lock;
    uint32_t gen;
    uint8_t *buf;  // where this range goes
    size_t cap, len;
    int status;
    bool done, complete, too_large;
    char err[ERR_BYTES];  // the start of a refusal's body
    size_t err_len;
} s_r;

static SemaphoreHandle_t s_one;  // one read at a time

static void on_frame(void *ctx, int status, const uint8_t *data, size_t len, bool end) {
    xSemaphoreTake(s_r.lock, portMAX_DELAY);
    if ((uint32_t)(uintptr_t)ctx == s_r.gen && !s_r.done) {
        if (status) s_r.status = status;
        bool ok = true;
        if (len && (s_r.status == 200 || s_r.status == 206)) {
            if (len > s_r.cap - s_r.len) {
                s_r.too_large = true;
                ok = false;
            } else {
                memcpy(s_r.buf + s_r.len, data, len);
                s_r.len += len;
            }
        } else if (len && s_r.status > 0) {
            size_t n = sizeof(s_r.err) - 1 - s_r.err_len;
            n = len < n ? len : n;
            memcpy(s_r.err + s_r.err_len, data, n);
            s_r.err_len += n;
        }
        s_r.complete = end && status >= 0 && ok;
        s_r.done = end || status < 0 || !ok;
    }
    xSemaphoreGive(s_r.lock);
}

static bool reply_done(void) {
    xSemaphoreTake(s_r.lock, portMAX_DELAY);
    bool done = s_r.done;
    xSemaphoreGive(s_r.lock);
    return done;
}

static void reply_start(uint8_t *buf, size_t cap) {
    s_r.buf = buf;
    s_r.cap = cap;
    s_r.len = 0;
    s_r.status = 0;
    s_r.done = s_r.complete = s_r.too_large = false;
    s_r.err_len = 0;
}

char *muse_apps_vm_raw_path(const char *path) {
    while (*path == '/') path++;
    size_t prefix = strlen(PATH_PREFIX);
    char *out = malloc(prefix + strlen(path) * 3 + 1);
    if (!out) return NULL;
    memcpy(out, PATH_PREFIX, prefix);
    char *p = out + prefix;
    for (const unsigned char *c = (const unsigned char *)path; *c; c++) {
        if (isalnum(*c) || strchr("-._~/", *c)) {
            *p++ = (char)*c;
        } else {
            *p++ = '%';
            *p++ = "0123456789ABCDEF"[*c >> 4];
            *p++ = "0123456789ABCDEF"[*c & 15];
        }
    }
    *p = '\0';
    return out;
}

// GETs [offset, offset + want) into dst; waits for the reply or the deadline.
// Returns the HTTP status (or a negative noise_ctrl_req_cb status, 0 if it
// couldn't be sent, -3 on timeout) and the byte count in *got.
static int get_range(const char *url, size_t offset, size_t want, uint8_t *dst,
                     int64_t deadline_us, size_t *got, bool *too_large) {
    char range[48];
    snprintf(range, sizeof(range), "bytes=%u-%u", (unsigned)offset, (unsigned)(offset + want - 1));
    gadget_headers_t h;
    const char *const *headers = gadget_headers(&h, NULL, false, range);
    xSemaphoreTake(s_r.lock, portMAX_DELAY);
    uint32_t token = ++s_r.gen;
    reply_start(dst, want);
    xSemaphoreGive(s_r.lock);

    int64_t id = muse_link_req_open("GET", url, headers, true, on_frame, (void *)(uintptr_t)token);
    bool timed_out = false;
    while (id && !reply_done()) {
        if (esp_timer_get_time() >= deadline_us) {
            timed_out = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }

    xSemaphoreTake(s_r.lock, portMAX_DELAY);
    s_r.gen++;  // drop anything still to come
    s_r.buf = NULL;
    int status = s_r.status;
    bool complete = s_r.complete;
    *got = s_r.len;
    *too_large = s_r.too_large;
    char err[ERR_BYTES];
    printable(err, sizeof(err), s_r.err, s_r.err_len);
    xSemaphoreGive(s_r.lock);
    if (id && !complete) muse_link_req_cancel(id);
    if (status > 0 && status != 200 && status != 206 && status != 416) {
        ESP_LOGW(TAG, "GET %s: HTTP %d%s%s", url, status, err[0] ? ": " : "", err);
    }
    if (!id) return 0;
    if (timed_out) return -3;
    if (!complete && status >= 0) return -1;
    return status;
}

// The locks, made by the first caller; later ones wait on them. Callers can
// race here (the app task, the store, two image loaders), so the pair is
// published once, under a lock, and a loser's are deleted.
static portMUX_TYPE s_setup_mux = portMUX_INITIALIZER_UNLOCKED;

static bool setup(char *err, size_t err_len) {
    taskENTER_CRITICAL(&s_setup_mux);
    bool ready = s_one && s_r.lock;
    taskEXIT_CRITICAL(&s_setup_mux);
    if (!ready) {
        SemaphoreHandle_t one = xSemaphoreCreateMutex(), lock = xSemaphoreCreateMutex();
        taskENTER_CRITICAL(&s_setup_mux);
        if (!s_one && one && lock) {
            s_r.lock = lock;
            s_one = one;
            one = lock = NULL;
        }
        taskEXIT_CRITICAL(&s_setup_mux);
        if (one) vSemaphoreDelete(one);
        if (lock) vSemaphoreDelete(lock);
    }
    taskENTER_CRITICAL(&s_setup_mux);
    ready = s_one && s_r.lock;
    taskEXIT_CRITICAL(&s_setup_mux);
    if (!ready) {
        snprintf(err, err_len, "out of memory");
        return false;
    }
    return true;
}

bool muse_apps_vm_file_read(const char *path, size_t max_bytes, int timeout_ms,
                  uint8_t **out, size_t *len, char *err, size_t err_len) {
    *out = NULL;
    *len = 0;
    if (!setup(err, err_len)) return false;
    char *url = muse_apps_vm_raw_path(path);
    if (!url) {
        snprintf(err, err_len, "out of memory");
        return false;
    }
    // The wait for another transfer (a long files.put) counts against the timeout too.
    int64_t start = esp_timer_get_time();
    int64_t deadline = start + (int64_t)timeout_ms * 1000;
    if (xSemaphoreTake(s_one, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        free(url);
        snprintf(err, err_len, "busy with another transfer");
        return false;
    }
    size_t chunk = muse_link_req_max_body();
    uint8_t *buf = NULL;
    size_t cap = 0, have = 0;
    bool ok = false;
    for (;;) {
        // One byte past the limit, to tell a file that's too large.
        size_t want = max_bytes + 1 - have;
        if (want > chunk) want = chunk;
        if (have + want > cap) {
            uint8_t *bigger = heap_caps_realloc(buf, have + want, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!bigger) {
                snprintf(err, err_len, "out of memory for %u bytes", (unsigned)(have + want));
                break;
            }
            buf = bigger;
            cap = have + want;
        }
        size_t got = 0;
        bool too_large = false;
        int status = get_range(url, have, want, buf + have, deadline, &got, &too_large);
        if (status == 416) {  // starts at the end: the file ended on the last range
            ok = true;
            break;
        }
        if (status == 206 || (status == 200 && have == 0 && !too_large)) {
            have += got;
            if (have > max_bytes) {
                snprintf(err, err_len, "larger than %u bytes", (unsigned)max_bytes);
                break;
            }
            if (status == 200 || got < want) {  // the whole file, or its last range
                ok = true;
                break;
            }
            continue;
        }
        if (status == 0) {
            snprintf(err, err_len, "not connected to the VM");
        } else if (status == -3) {
            snprintf(err, err_len, "timed out after %u bytes", (unsigned)have);
        } else if (status == MUSE_LINK_REQ_TOO_LARGE || too_large) {
            snprintf(err, err_len, "the VM sent a frame too large for this board");
        } else if (status < 0) {
            snprintf(err, err_len, "the VM connection dropped");
        } else if (status == 404) {
            snprintf(err, err_len, "no such file on the VM");
        } else {
            snprintf(err, err_len, "the VM answered HTTP %d", status);
        }
        break;
    }
    xSemaphoreGive(s_one);
    ESP_LOGI(TAG, "%s: %s, %u bytes in %d ms", url, ok ? "read" : err, (unsigned)have,
             (int)((esp_timer_get_time() - start) / 1000));
    free(url);
    if (!ok) {
        free(buf);
        return false;
    }
    *out = buf;
    *len = have;
    return true;
}

bool muse_apps_vm_file_stream(const char *path, size_t max_bytes, int timeout_ms,
                              bool (*sink)(void *ctx, const uint8_t *data, size_t len), void *ctx, size_t *len,
                              char *err, size_t err_len) {
    *len = 0;
    if (!setup(err, err_len)) return false;
    char *url = muse_apps_vm_raw_path(path);
    size_t chunk = muse_link_req_max_body();
    uint8_t *piece = heap_caps_malloc(chunk, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!url || !piece) {
        free(url);
        free(piece);
        snprintf(err, err_len, "out of memory");
        return false;
    }
    xSemaphoreTake(s_one, portMAX_DELAY);
    int64_t start = esp_timer_get_time();
    int64_t deadline = start + (int64_t)timeout_ms * 1000;
    size_t have = 0;
    bool ok = false;
    for (;;) {
        size_t want = max_bytes + 1 - have;  // one byte past the limit tells a file too large
        if (want > chunk) want = chunk;
        size_t got = 0;
        bool too_large = false;
        int status = get_range(url, have, want, piece, deadline, &got, &too_large);
        if (status == 416) {  // starts at the end: the file ended on the last range
            ok = true;
            break;
        }
        if (status == 206 || (status == 200 && have == 0 && !too_large)) {
            if (have + got > max_bytes) {
                snprintf(err, err_len, "larger than %u bytes", (unsigned)max_bytes);
                break;
            }
            if (got && !sink(ctx, piece, got)) {
                snprintf(err, err_len, "couldn't store it after %u bytes", (unsigned)have);
                break;
            }
            have += got;
            if (status == 200 || got < want) {  // the whole file, or its last range
                ok = true;
                break;
            }
            continue;
        }
        if (status == 0) {
            snprintf(err, err_len, "not connected to the VM");
        } else if (status == -3) {
            snprintf(err, err_len, "timed out after %u bytes", (unsigned)have);
        } else if (status == MUSE_LINK_REQ_TOO_LARGE || too_large) {
            snprintf(err, err_len, "the VM sent a frame too large for this board");
        } else if (status < 0) {
            snprintf(err, err_len, "the VM connection dropped");
        } else if (status == 404) {
            snprintf(err, err_len, "no such file on the VM");
        } else {
            snprintf(err, err_len, "the VM answered HTTP %d", status);
        }
        break;
    }
    xSemaphoreGive(s_one);
    ESP_LOGI(TAG, "%s: %s, %u bytes in %d ms", url, ok ? "streamed" : err, (unsigned)have,
             (int)((esp_timer_get_time() - start) / 1000));
    free(url);
    free(piece);
    *len = have;
    return ok;
}

// POSTs a JSON body (one piece, under 8 KB) and waits for the reply, which
// goes in reply (NUL-terminated, cut short if longer): a refusal's body too.
// Returns the status as get_range() does.
static int post_json(const char *route, const char *body, char *reply, size_t reply_cap,
                     int64_t deadline_us) {
    gadget_headers_t h;
    const char *const *headers = gadget_headers(&h, "application/json", true, NULL);
    xSemaphoreTake(s_r.lock, portMAX_DELAY);
    uint32_t token = ++s_r.gen;
    reply_start((uint8_t *)reply, reply_cap - 1);
    xSemaphoreGive(s_r.lock);

    int64_t id = muse_link_req_open("POST", route, headers, false, on_frame, (void *)(uintptr_t)token);
    bool sent = id && muse_link_req_send(id, body, strlen(body), true, SEND_WAIT_MS);
    bool timed_out = false;
    while (sent && !reply_done()) {
        if (esp_timer_get_time() >= deadline_us) {
            timed_out = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }

    xSemaphoreTake(s_r.lock, portMAX_DELAY);
    s_r.gen++;
    s_r.buf = NULL;
    int status = s_r.status;
    bool complete = s_r.complete || s_r.too_large;  // a long reply is still an answer
    if (s_r.err_len) {
        printable(reply, reply_cap, s_r.err, s_r.err_len);
    } else {
        reply[s_r.len] = '\0';
    }
    xSemaphoreGive(s_r.lock);
    if (id && !complete) muse_link_req_cancel(id);
    if (status > 0 && status != 200) {
        ESP_LOGW(TAG, "POST %s: HTTP %d%s%s", route, status, reply[0] ? ": " : "", reply);
    }
    if (!sent) return 0;
    if (timed_out) return -3;
    if (!complete && status >= 0) return -1;
    return status;
}

bool muse_apps_vm_file_write(const char *path, const uint8_t *data, size_t len, int timeout_ms,
                   char *err, size_t err_len) {
    if (!setup(err, err_len)) return false;
    char *reply = malloc(WRITE_REPLY);
    char *b64 = malloc(WRITE_PIECE / 3 * 4 + 8);
    if (!reply || !b64) {
        free(reply);
        free(b64);
        snprintf(err, err_len, "out of memory");
        return false;
    }
    xSemaphoreTake(s_one, portMAX_DELAY);
    int64_t start = esp_timer_get_time();
    int64_t deadline = start + (int64_t)timeout_ms * 1000;
    bool ok = true;
    size_t off = 0;
    do {
        size_t piece = len - off < WRITE_PIECE ? len - off : WRITE_PIECE;
        size_t b64_len = 0;
        mbedtls_base64_encode((unsigned char *)b64, WRITE_PIECE / 3 * 4 + 8, &b64_len, data + off, piece);
        b64[b64_len] = '\0';
        cJSON *body = cJSON_CreateObject();
        cJSON_AddStringToObject(body, "path", path);
        cJSON_AddStringToObject(body, "data_base64", b64);
        cJSON_AddBoolToObject(body, "overwrite", off == 0);
        cJSON_AddBoolToObject(body, "append", off > 0);
        cJSON_AddBoolToObject(body, "create_parent", true);
        char *text = cJSON_PrintUnformatted(body);
        cJSON_Delete(body);
        int status = text ? post_json("/fs/write", text, reply, WRITE_REPLY, deadline) : 0;
        free(text);
        if (status != 200) {
            if (status == 0) {
                snprintf(err, err_len, "not connected to the VM");
            } else if (status == -3) {
                snprintf(err, err_len, "timed out after %u bytes", (unsigned)off);
            } else if (status < 0) {
                snprintf(err, err_len, "the VM connection dropped");
            } else {
                snprintf(err, err_len, "the VM answered HTTP %d at byte %u: %.120s", status, (unsigned)off, reply);
            }
            ok = false;
            break;
        }
        off += piece;
    } while (off < len);
    xSemaphoreGive(s_one);
    ESP_LOGI(TAG, "write %s: %s, %u bytes in %d ms", path, ok ? "done" : err, (unsigned)off,
             (int)((esp_timer_get_time() - start) / 1000));
    free(reply);
    free(b64);
    return ok;
}

/* ---- The app's own requests: the worker (muse_apps_vm.h) ----
 * Natives queue jobs; the worker, a task with its stack in PSRAM made at the
 * first job, runs them one at a time and may wait on the VM. Stopping an app
 * moves the epoch: its queued jobs are dropped and its request in flight is
 * cancelled. The worker stays for the next app. */

#define JOBS 8                  /* limits.ops, the chat's connects, and room */
#define WORKER_STACK 6144
#define WORKER_PRIORITY 3       /* above the app task, below the UI and the network */
#define BODY_PIECE 6144         /* of a body, under the session's 8 KB */
#define WAKE_MS 100

typedef struct {
    muse_apps_job_fn run, drop;
    void *arg;
    uint32_t epoch;
} job_t;

static QueueHandle_t s_jobs;
static TaskHandle_t s_worker;
static bool s_worker_starting;
static portMUX_TYPE s_worker_mux = portMUX_INITIALIZER_UNLOCKED;
static atomic_uint s_epoch;     /* moved by muse_apps_vm_close_all() */
static uint32_t s_job_epoch;    /* the running job's: the worker only */

/* The worker's request in flight, written on the session task. */
static struct {
    SemaphoreHandle_t lock;
    uint32_t gen;
    TaskHandle_t waiter;
    uint8_t *buf;
    size_t cap, len;
    int status;
    bool done, complete, cut;
    char err[ERR_BYTES];
    size_t err_len;
} s_w;

static bool cancelled(void) {
    return s_job_epoch != atomic_load(&s_epoch);
}

static void worker(void *arg) {
    (void)arg;
    for (;;) {
        job_t j;
        if (xQueueReceive(s_jobs, &j, portMAX_DELAY) != pdTRUE) continue;
        if (j.epoch != atomic_load(&s_epoch)) {
            if (j.drop) j.drop(j.arg);
            continue;
        }
        s_job_epoch = j.epoch;
        j.run(j.arg);
    }
}

/* The worker and its queue, made by the first job. */
static bool worker_ready(void) {
    if (s_worker) return true;
    taskENTER_CRITICAL(&s_worker_mux);
    bool other = s_worker_starting;
    s_worker_starting = true;
    taskEXIT_CRITICAL(&s_worker_mux);
    if (other) return false;  // another task is making it right now
    if (!s_jobs) s_jobs = xQueueCreateWithCaps(JOBS, sizeof(job_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_w.lock) s_w.lock = xSemaphoreCreateMutex();
    TaskHandle_t t = NULL;
    if (s_jobs && s_w.lock &&
        xTaskCreatePinnedToCoreWithCaps(worker, "muse_apps_vm", WORKER_STACK, NULL, WORKER_PRIORITY, &t,
                                        tskNO_AFFINITY, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        t = NULL;
    }
    if (!t) ESP_LOGE(TAG, "couldn't start the apps' VM worker");
    taskENTER_CRITICAL(&s_worker_mux);
    s_worker = t;
    s_worker_starting = false;
    taskEXIT_CRITICAL(&s_worker_mux);
    return t != NULL;
}

bool muse_apps_vm_job(muse_apps_job_fn run, muse_apps_job_fn drop, void *arg) {
    if (!run || !worker_ready()) return false;
    job_t j = {run, drop, arg, atomic_load(&s_epoch)};
    return xQueueSend(s_jobs, &j, 0) == pdTRUE;
}

static void w_frame(void *ctx, int status, const uint8_t *data, size_t len, bool end) {
    TaskHandle_t wake = NULL;
    xSemaphoreTake(s_w.lock, portMAX_DELAY);
    if ((uint32_t)(uintptr_t)ctx == s_w.gen && !s_w.done) {
        if (status) s_w.status = status;
        if (len && s_w.status >= 200 && s_w.status < 300) {
            size_t n = s_w.cap - s_w.len;
            if (len > n) {
                s_w.cut = true;  // more than asked for: keep what fits, and stop
            } else {
                n = len;
            }
            if (n) {
                memcpy(s_w.buf + s_w.len, data, n);
                s_w.len += n;
            }
        } else if (len && s_w.status > 0) {
            size_t n = sizeof(s_w.err) - 1 - s_w.err_len;
            n = len < n ? len : n;
            memcpy(s_w.err + s_w.err_len, data, n);
            s_w.err_len += n;
        }
        s_w.complete = end && status >= 0 && !s_w.cut;
        s_w.done = end || status < 0 || s_w.cut;
        wake = s_w.waiter;
    }
    xSemaphoreGive(s_w.lock);
    if (wake) xTaskNotifyGive(wake);
}

static bool w_done(void) {
    xSemaphoreTake(s_w.lock, portMAX_DELAY);
    bool done = s_w.done;
    xSemaphoreGive(s_w.lock);
    return done;
}

static const char *describe(int status, char *buf, size_t cap) {
    switch (status) {
    case MUSE_APPS_VM_NOT_SENT: return "not sent: the session is down or busy";
    case MUSE_APPS_VM_DROPPED: return "dropped: the session or the stream ended";
    case MUSE_APPS_VM_TOO_LARGE: return "a reply frame too large for this board";
    case MUSE_APPS_VM_TIMEOUT: return "timed out";
    case MUSE_APPS_VM_CANCELLED: return "cancelled: the app stopped";
    default: snprintf(buf, cap, "HTTP %d", status); return buf;
    }
}

int muse_apps_vm_fetch(muse_apps_fetch_t *f) {
    f->reply_len = 0;
    f->reply_cut = false;
    f->error[0] = '\0';
    if (cancelled()) return MUSE_APPS_VM_CANCELLED;
    int64_t start = esp_timer_get_time();
    int64_t deadline = start + (int64_t)f->timeout_ms * 1000;
    ulTaskNotifyTake(pdTRUE, 0);  // a stale wake from an earlier request
    xSemaphoreTake(s_w.lock, portMAX_DELAY);
    uint32_t token = ++s_w.gen;
    s_w.waiter = xTaskGetCurrentTaskHandle();
    s_w.buf = f->reply;
    s_w.cap = f->reply ? f->reply_cap : 0;
    s_w.len = 0;
    s_w.status = 0;
    s_w.done = s_w.complete = s_w.cut = false;
    s_w.err_len = 0;
    xSemaphoreGive(s_w.lock);

    gadget_headers_t h;
    const char *accept = f->ndjson ? "application/x-ndjson" : f->json ? "application/json" : NULL;
    int64_t id = muse_link_req_open(f->verb, f->path, gadget_headers(&h, accept, f->body != NULL, f->range),
                                    f->body == NULL, w_frame, (void *)(uintptr_t)token);
    bool sent = id != 0;
    if (sent && f->body) {
        const uint8_t *body = f->body;
        size_t off = 0;
        do {  // in pieces the session takes; an answer that comes first (a refusal) ends it
            size_t n = f->body_len - off < BODY_PIECE ? f->body_len - off : BODY_PIECE;
            if (cancelled() || w_done()) break;
            sent = muse_link_req_send(id, body + off, n, off + n == f->body_len, SEND_WAIT_MS);
            off += n;
        } while (sent && off < f->body_len);
    }
    bool timed_out = false, aborted = false;
    while (sent && !w_done()) {
        if (cancelled()) {
            aborted = true;
            break;
        }
        int64_t left_ms = (deadline - esp_timer_get_time()) / 1000;
        if (left_ms <= 0) {
            timed_out = true;
            break;
        }
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(left_ms < WAKE_MS ? left_ms + 1 : WAKE_MS));
    }

    xSemaphoreTake(s_w.lock, portMAX_DELAY);
    s_w.gen++;  // drop anything still to come
    s_w.buf = NULL;
    s_w.waiter = NULL;
    int status = s_w.status;
    bool done = s_w.done, complete = s_w.complete;
    f->reply_len = s_w.len;
    f->reply_cut = s_w.cut;
    printable(f->error, sizeof(f->error), s_w.err, s_w.err_len);
    xSemaphoreGive(s_w.lock);
    if (id && !complete) muse_link_req_cancel(id);
    if (!id) {
        status = MUSE_APPS_VM_NOT_SENT;
    } else if (!(done && status > 0)) {  // no answer
        status = aborted ? MUSE_APPS_VM_CANCELLED : timed_out ? MUSE_APPS_VM_TIMEOUT
               : status < 0 ? status : MUSE_APPS_VM_DROPPED;
    }
    int ms = (int)((esp_timer_get_time() - start) / 1000);
    if (status >= 200 && status < 300) {
        ESP_LOGD(TAG, "%s %s: HTTP %d, %u bytes in %d ms", f->verb, f->path, status, (unsigned)f->reply_len, ms);
    } else if (status != MUSE_APPS_VM_CANCELLED) {
        char what[24];
        ESP_LOGW(TAG, "%s %s: %s in %d ms%s%s", f->verb, f->path, describe(status, what, sizeof(what)), ms,
                 f->error[0] ? ": " : "", f->error);
    }
    return status;
}

int64_t muse_apps_vm_open(const char *verb, const char *path, const char *accept, const char *body,
                          muse_link_req_cb cb, void *ctx) {
    gadget_headers_t h;
    int64_t id = muse_link_req_open(verb, path, gadget_headers(&h, accept, body != NULL, NULL), body == NULL, cb, ctx);
    if (id && body && !muse_link_req_send(id, body, strlen(body), true, SEND_WAIT_MS)) {
        muse_link_req_cancel(id);
        id = 0;
    }
    return id;
}

int32_t muse_apps_vm_error(int status) {
    switch (status) {
    case MUSE_APPS_VM_NOT_SENT:
    case MUSE_APPS_VM_DROPPED: return GADGET_ERR_OFFLINE;
    case MUSE_APPS_VM_TOO_LARGE:
    case 413: return GADGET_ERR_TOO_LARGE;
    case MUSE_APPS_VM_TIMEOUT:
    case 408:
    case 504: return GADGET_ERR_TIMEOUT;
    case 401:
    case 403: return GADGET_ERR_DENIED;
    case 404:
    case 410: return GADGET_ERR_NOT_FOUND;
    default: return GADGET_ERR_FAILED;
    }
}

size_t muse_apps_vm_json_escape(char *out, const char *text, size_t n) {
    char *o = out;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)text[i];
        const char *esc = c == '"' ? "\\\"" : c == '\\' ? "\\\\" : c == '\n' ? "\\n" : c == '\r' ? "\\r"
                        : c == '\t' ? "\\t" : c == '\b' ? "\\b" : c == '\f' ? "\\f" : NULL;
        if (esc) {
            *o++ = esc[0];
            *o++ = esc[1];
        } else if (c < 0x20) {
            o += sprintf(o, "\\u%04x", c);
        } else {
            *o++ = (char)c;
        }
    }
    return (size_t)(o - out);
}

/* The app stopped (after the chat and files modules let go of theirs): its
 * queued jobs are dropped and the worker's request in flight is cancelled,
 * at once. Nothing here waits. */
void muse_apps_vm_close_all(void) {
    atomic_fetch_add(&s_epoch, 1);
    if (s_worker) xTaskNotifyGive(s_worker);
}
