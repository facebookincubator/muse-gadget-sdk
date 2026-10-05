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

// On-device Lua scripts the agent installs (script.*), with the board's
// hardware as functions (led.set{...}, camera.detect{...}) and its events as
// callbacks (on("tap", fn), on("detection", fn)). The sandbox is
// muse_script_sandbox.c; this is its home on the device:
//
//  - One scripting task owns every lua_State; everyone else posts to its queue.
//    Its stack is in PSRAM, so it never touches flash: files are read and
//    written on the caller's task (the control session's, the console's).
//  - Priority 1: the UI, voice, Wi-Fi and lwIP all preempt it, and the
//    sandbox's budgets stop a script that runs too long between waits.
//  - Scripts live on a LittleFS partition, /scripts/<name>.lua, with
//    <name>.auto beside those that start at boot. Two crashes in a row and the
//    next boot skips them.

#include <dirent.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cJSON.h"
#include "esp_attr.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "muse_apps.h"
#include "muse_hw.h"
#include "muse_hw_commands.h"
#include "muse_hw_commands_priv.h"
#include "muse_script_sandbox.h"
#include "noise_control.h"

static const char *TAG = "link.script";

#define SCRIPT_STACK (32 * 1024)    // PSRAM; LUAI_MAXCCALLS=48 needs ~20 KB at worst
#define SCRIPT_PRIO 1
#define SCRIPT_QUEUE_LEN 24
#define REPLY_WAIT_MS 1000
#define AUTOSTART_DELAY_MS 5000     // after boot, once Muse's UI is up
#define AUTOSTART_RETRY_MS 1000     // no memory for its task yet: Wi-Fi and TLS are still coming up
#define AUTOSTART_TRIES 10
#define HEALTHY_US (60 * 1000000LL) // running this long clears the crash count

#define BASE "/scripts"
#define LABEL "scripts"
#define FALLBACK_OFFSET 0x830000    // must match partitions_muse.csv
#define FALLBACK_SIZE 0x100000
#define SCRIPT_MAX (16 * 1024)

// What scripts may call. Not what writes flash on the calling task (the
// scripting task can't: display.set_brightness, audio.set_volume), restarts
// or powers off the board, manages scripts, pairing or the VM.
static const char *const COMMANDS[] = {
    "device.status", "device.time",
    "display.show_text", "display.show_ui", "display.draw_url", "display.show_animation", "display.power",
    "led.set",
    "audio.beep", "audio.play_url", "audio.listen", "audio.record",
    "wifi.scan",
    "camera.capture", "camera.detect", "camera.models",
    "camera.preview", "camera.watch", "camera.stop",
    "grove.power", "i2c.scan", "i2c.read", "i2c.write", "uart.write", "uart.read",
    "storage.info", "storage.list", "storage.read", "storage.write", "storage.delete",
    "app.define", "app.update", "app.remove", "app.show", "app.list",
    "pet.status", "pet.care", "pet.name",
    NULL,
};

typedef enum { MSG_EVENT, MSG_START, MSG_CHECK, MSG_STOP, MSG_LIST, MSG_LOGS, MSG_RESULT } msg_type_t;

typedef struct {
    msg_type_t type;
    char name[32];
    muse_hw_event_t ev;    // MSG_EVENT
    char *text;            // script source (PSRAM); the task frees it
    size_t len;
    cJSON *json;           // MSG_RESULT: the result; the task frees it
    int64_t token;
    SemaphoreHandle_t done;   // synchronous requests; the sender owns the message
    bool ok;
    char *err;
    size_t errlen;
    cJSON *out;
} script_msg_t;

static QueueHandle_t s_q;
static sb_runtime_t *s_rt;
static bool s_mounted;
static volatile int64_t s_next_token;
static esp_timer_handle_t s_boot;

// ---- The crash-loop guard ---------------------------------------------------

#define MARK_RUNNING 0x5C4197A1u
RTC_NOINIT_ATTR static uint32_t s_mark;
RTC_NOINIT_ATTR static uint32_t s_crashes;

static bool autostart_allowed(void) {
    esp_reset_reason_t r = esp_reset_reason();
    bool crashed = r == ESP_RST_PANIC || r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT || r == ESP_RST_WDT;
    if (r == ESP_RST_POWERON || s_mark != MARK_RUNNING) {
        s_crashes = 0;
    } else if (crashed) {
        s_crashes++;
    }
    s_mark = 0;
    if (s_crashes >= 2) {
        ESP_LOGW(TAG, "scripts crashed the board %" PRIu32 " times in a row: autostart skipped", s_crashes);
        return false;
    }
    return true;
}

static void healthy(void *arg) {
    (void)arg;
    s_crashes = 0;
}

// ---- The sandbox's platform ---------------------------------------------------

static int64_t now_us(void) {
    return esp_timer_get_time();
}

static void *mem_realloc(void *p, size_t n) {
    // Always PSRAM: CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL would put Lua's many
    // small blocks in what little internal RAM is left.
    return heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static void log_line(const char *script, const char *line) {
    ESP_LOGI(TAG, "%s: %s", script, line);
}

// A command for a script: Link's own dispatcher (as the console uses) or the
// hardware commands. One that answers later gets a token; its result comes
// back through muse_script_result() and resumes the script.
static cJSON *device_call(const char *script, const char *command, const cJSON *params) {
    (void)script;
    int64_t token = __atomic_add_fetch(&s_next_token, 1, __ATOMIC_RELAXED);
    char request_id[64];   // with the script's name: app.define makes its apps its own
    snprintf(request_id, sizeof(request_id), "script:%" PRId64 ":%s", token, script);
    cJSON *p = params ? (cJSON *)params : NULL;
    noise_ctrl_command_cb dispatch = hw_dispatch();
    cJSON *r = dispatch ? dispatch(command, p, request_id, HW_SCRIPT_SESSION)
                        : muse_hw_command(command, p, request_id, HW_SCRIPT_SESSION);
    if (!r) return hw_error("unsupported", "no such command");
    if (cJSON_IsTrue(cJSON_GetObjectItem(r, "_async"))) cJSON_AddNumberToObject(r, "_token", (double)token);
    return r;
}

// notify(): a message in the agent's chat, the one way the device can reach it.
static void notify_done(void *ctx, int status, const uint8_t *data, size_t len, bool end) {
    (void)ctx;
    (void)data;
    (void)len;
    if (status > 0 && status != 200) ESP_LOGW(TAG, "notify: the chat answered %d", status);
    else if (status < 0) ESP_LOGW(TAG, "notify: the chat request was dropped");
    (void)end;
}

static void notify(const char *script, const char *text) {
    cJSON *body = cJSON_CreateObject();
    char msg[400];
    snprintf(msg, sizeof(msg), "[%s on %s] %s", script, "the device", text);
    cJSON_AddStringToObject(body, "message", msg);
    cJSON_AddStringToObject(body, "output_modality", "text");
    char *json = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!json) return;
    char req_id[40];
    snprintf(req_id, sizeof(req_id), "muse-%08" PRIx32 "-%08" PRIx32, esp_random(), esp_random());
    const char *headers[] = { "x-request-id", req_id, "x-app-id", "hatch-web", "Content-Type", "application/json", NULL };
    int64_t id = noise_ctrl_req_open("POST", "/chat/stream", headers, false, notify_done, NULL);
    if (!id || !noise_ctrl_req_send(id, json, strlen(json), true, 200)) {
        ESP_LOGW(TAG, "notify from %s not sent (not connected?): %s", script, text);
        if (id) noise_ctrl_req_cancel(id);
    } else {
        ESP_LOGI(TAG, "notify from %s: %s", script, text);
    }
    cJSON_free(json);
}

static void cpu_yield(void) {
    vTaskDelay(1);
}

// ---- The scripting task ---------------------------------------------------------

static void handle(script_msg_t *m) {
    switch (m->type) {
        case MSG_EVENT: {
            // Converted here, on this task's roomy stack, not the one that raised it.
            cJSON *data = hw_event_json(&m->ev, m->ev.us);
            cJSON_DeleteItemFromObject(data, "ms_ago");
            sb_event(s_rt, m->name, data);
            cJSON_Delete(data);
            break;
        }
        case MSG_START:
            s_mark = MARK_RUNNING;
            m->ok = sb_start(s_rt, m->name, m->text, m->len, m->err, m->errlen);
            if (m->ok) muse_apps_reshow();   // its page's "show" may have come before its handlers
            break;
        case MSG_CHECK: m->ok = sb_check_syntax(s_rt, m->text, m->len, m->err, m->errlen); break;
        case MSG_STOP: m->ok = sb_stop(s_rt, m->name); break;
        case MSG_LIST: m->out = sb_list(s_rt); break;
        case MSG_LOGS: m->out = sb_logs(s_rt, m->name); break;
        case MSG_RESULT:
            sb_async_result(s_rt, m->token, m->json);
            cJSON_Delete(m->json);
            m->json = NULL;
            break;
    }
    heap_caps_free(m->text);
    m->text = NULL;
}

static void script_task(void *arg) {
    (void)arg;
    for (;;) {
        TickType_t ticks = portMAX_DELAY;
        int64_t wake = sb_next_wake_us(s_rt);
        if (wake != INT64_MAX) {
            int64_t d = wake - esp_timer_get_time();
            ticks = d <= 0 ? 0 : pdMS_TO_TICKS((d + 999) / 1000);
        }
        script_msg_t *m;
        if (xQueueReceive(s_q, &m, ticks) == pdTRUE) {
            handle(m);
            if (m->done) {
                xSemaphoreGive(m->done);
            } else {
                heap_caps_free(m);
            }
        }
        sb_run_due(s_rt);
    }
}

static char *psram_copy(const char *s, size_t len) {
    char *p = heap_caps_malloc(len + 1, MALLOC_CAP_SPIRAM);
    if (p) {
        memcpy(p, s, len);
        p[len] = '\0';
    }
    return p;
}

// Never blocks; drops the message if the queue is full.
static bool post(script_msg_t *m) {
    if (s_q && xQueueSend(s_q, &m, 0) == pdTRUE) return true;
    heap_caps_free(m->text);
    cJSON_Delete(m->json);
    heap_caps_free(m);
    return false;
}

// A round trip to the scripting task: at most one of its slices, plus the queue.
static bool request(script_msg_t *m) {
    if (!s_q) return false;
    m->done = xSemaphoreCreateBinary();
    if (!m->done) return false;
    if (xQueueSend(s_q, &m, pdMS_TO_TICKS(REPLY_WAIT_MS)) != pdTRUE) {
        vSemaphoreDelete(m->done);
        heap_caps_free(m->text);
        return false;
    }
    xSemaphoreTake(m->done, portMAX_DELAY);
    vSemaphoreDelete(m->done);
    return true;
}

static bool task_start(const char *name, const char *src, size_t len, char *err, size_t errlen) {
    script_msg_t m = { .type = MSG_START, .err = err, .errlen = errlen, .len = len };
    strlcpy(m.name, name, sizeof(m.name));
    if (!(m.text = psram_copy(src, len))) return false;
    return request(&m) && m.ok;
}

static bool task_check(const char *src, size_t len, char *err, size_t errlen) {
    script_msg_t m = { .type = MSG_CHECK, .err = err, .errlen = errlen, .len = len };
    if (!(m.text = psram_copy(src, len))) return false;
    return request(&m) && m.ok;
}

static bool task_stop(const char *name) {
    script_msg_t m = { .type = MSG_STOP };
    strlcpy(m.name, name, sizeof(m.name));
    return request(&m) && m.ok;
}

static cJSON *task_query(msg_type_t type, const char *name) {
    script_msg_t m = { .type = type };
    if (name) strlcpy(m.name, name, sizeof(m.name));
    return request(&m) ? m.out : NULL;
}

void muse_script_result(const char *request_id, cJSON *result) {
    script_msg_t *m = heap_caps_calloc(1, sizeof(*m), MALLOC_CAP_SPIRAM);
    if (!m || strncmp(request_id, "script:", 7) != 0) {
        cJSON_Delete(result);
        heap_caps_free(m);
        return;
    }
    m->type = MSG_RESULT;
    m->token = strtoll(request_id + 7, NULL, 10);
    m->json = result;
    if (!post(m)) ESP_LOGW(TAG, "script queue full: a result was dropped");
}

// Every hardware event, to the scripts' on(type, fn) handlers. Runs on the
// task that raised it (the touch task's stack is small): just a copy.
static void on_hw_event(const muse_hw_event_t *ev) {
    if (!s_q) return;
    script_msg_t *m = heap_caps_calloc(1, sizeof(*m), MALLOC_CAP_SPIRAM);
    if (!m) return;
    m->type = MSG_EVENT;
    m->ev = *ev;
    strlcpy(m->name, muse_hw_event_name(ev->type), sizeof(m->name));
    post(m);
}

// ---- Storage ----------------------------------------------------------------------

// A USB flash writes the new partition table; a board updated over the air
// keeps its old one. There, claim the same range if the chip has it.
static const esp_partition_t *find_partition(void) {
    const esp_partition_t *p =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_LITTLEFS, LABEL);
    if (p) return p;
    uint32_t size = 0;
    if (esp_flash_get_size(NULL, &size) != ESP_OK || size < FALLBACK_OFFSET + FALLBACK_SIZE) return NULL;
    if (esp_partition_register_external(NULL, FALLBACK_OFFSET, FALLBACK_SIZE, LABEL, ESP_PARTITION_TYPE_DATA,
                                        ESP_PARTITION_SUBTYPE_DATA_LITTLEFS, &p) != ESP_OK) {
        return NULL;
    }
    ESP_LOGI(TAG, "no scripts partition in the table: using 0x%x-0x%x", FALLBACK_OFFSET,
             FALLBACK_OFFSET + FALLBACK_SIZE);
    return p;
}

static bool store_mount(void) {
    const esp_partition_t *p = find_partition();
    if (!p) {
        ESP_LOGW(TAG, "no room for scripts on this flash: they run but won't be kept");
        return false;
    }
    esp_vfs_littlefs_conf_t conf = { .base_path = BASE, .partition = p, .format_if_mount_failed = true };
    s_mounted = esp_vfs_littlefs_register(&conf) == ESP_OK;
    return s_mounted;
}

static void path(char *out, size_t n, const char *name, const char *ext) {
    snprintf(out, n, BASE "/%s.%s", name, ext);
}

static bool store_save(const char *name, const char *src, size_t len, bool autostart) {
    if (!s_mounted || len > SCRIPT_MAX) return false;
    char tmp[64], dst[64], aut[64];
    path(tmp, sizeof(tmp), name, "tmp");
    path(dst, sizeof(dst), name, "lua");
    path(aut, sizeof(aut), name, "auto");
    FILE *f = fopen(tmp, "wb");
    if (!f) return false;
    bool ok = fwrite(src, 1, len, f) == len;
    ok = fclose(f) == 0 && ok;
    if (ok) {
        ok = rename(tmp, dst) == 0;   // replaces dst atomically on LittleFS: never no script at all
    }
    if (!ok) {
        unlink(tmp);
        return false;
    }
    if (autostart) {
        FILE *a = fopen(aut, "wb");
        if (a) fclose(a);
    } else {
        unlink(aut);
    }
    return true;
}

// PSRAM, NUL-terminated; heap_caps_free it.
static char *store_load(const char *name, size_t *len) {
    char p[64];
    path(p, sizeof(p), name, "lua");
    FILE *f = fopen(p, "rb");
    if (!f) return NULL;
    char *buf = heap_caps_malloc(SCRIPT_MAX + 1, MALLOC_CAP_SPIRAM);
    size_t n = buf ? fread(buf, 1, SCRIPT_MAX, f) : 0;
    fclose(f);
    if (!buf) return NULL;
    buf[n] = '\0';
    *len = n;
    return buf;
}

static bool store_delete(const char *name) {
    char p[64];
    path(p, sizeof(p), name, "auto");
    unlink(p);
    path(p, sizeof(p), name, "lua");
    return unlink(p) == 0;
}

// [{name, bytes, autostart}]
static cJSON *store_list(void) {
    cJSON *arr = cJSON_CreateArray();
    DIR *d = s_mounted ? opendir(BASE) : NULL;
    for (struct dirent *e; d && (e = readdir(d));) {
        size_t n = strlen(e->d_name);
        if (n < 5 || n > 36 || strcmp(e->d_name + n - 4, ".lua")) continue;
        char name[33], p[64];
        snprintf(name, sizeof(name), "%.*s", (int)(n - 4), e->d_name);
        struct stat st;
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name", name);
        path(p, sizeof(p), name, "lua");
        cJSON_AddNumberToObject(o, "bytes", stat(p, &st) == 0 ? st.st_size : 0);
        path(p, sizeof(p), name, "auto");
        cJSON_AddBoolToObject(o, "autostart", stat(p, &st) == 0);
        cJSON_AddItemToArray(arr, o);
    }
    if (d) closedir(d);
    return arr;
}

bool muse_script_put(const char *name, const char *src, bool run, char *err, size_t errlen) {
    size_t len = strlen(src);
    if (!store_save(name, src, len, true)) {
        snprintf(err, errlen, "couldn't save it");
        return false;
    }
    return !run || task_start(name, src, len, err, errlen);
}

// Once, 5 s after boot (retried each second a few times while there's no
// memory for it), on a task of its own with an internal stack for the
// file reads. Not on the esp_timer task: building the pages waits for the
// display lock, which would stall every timer meanwhile, and at that task's
// priority (22) could starve the display's own send task.
static void autostart(void *arg) {
    (void)arg;
    // The crash-loop guard covers the pages too: rebuilding them (and the
    // library's first-boot writes) can crash as well as a script can.
    bool allowed = autostart_allowed();
    if (allowed) {
        s_mark = MARK_RUNNING;
        muse_hw_apps_load();   // their pages first, so scripts find them
    } else {
        ESP_LOGW(TAG, "apps not restored either");
    }
    if (s_mounted && allowed) {
        cJSON *list = store_list();
        cJSON *o;
        cJSON_ArrayForEach(o, list) {
            if (!cJSON_IsTrue(cJSON_GetObjectItem(o, "autostart"))) continue;
            const char *name = cJSON_GetObjectItem(o, "name")->valuestring;
            size_t len;
            char *src = store_load(name, &len);
            char err[160] = "";
            if (!src || !task_start(name, src, len, err, sizeof(err))) ESP_LOGW(TAG, "%s: %s", name, err);
            else ESP_LOGI(TAG, "%s started", name);
            heap_caps_free(src);
        }
        cJSON_Delete(list);
    }
}

static void autostart_task(void *arg) {
    autostart(arg);
    vTaskDelete(NULL);
}

static void autostart_later(void *arg) {
    static int tries;
    if (xTaskCreate(autostart_task, "script_boot", 5120, arg, 2, NULL) == pdPASS) return;
    if (++tries < AUTOSTART_TRIES && esp_timer_start_once(s_boot, AUTOSTART_RETRY_MS * 1000LL) == ESP_OK) return;
    ESP_LOGE(TAG, "no memory to start the apps and scripts");
    autostart_allowed();   // the crash-loop guard still counts this boot

}

// On the scripting task: a camera it turned on for itself goes with it.
static void script_ended(const char *script) {
    muse_hw_camera_script_ended(script);
}

void muse_script_init(void) {
    sb_platform_t pf = {
        .now_us = now_us, .mem_realloc = mem_realloc, .mem_free = heap_caps_free, .log = log_line,
        .device_call = device_call, .notify = notify, .cpu_yield = cpu_yield, .ended = script_ended,
    };
    sb_limits_t lim;
    sb_default_limits(&lim);
    lim.slice_instructions = 500000;   // ~0.1 s of Lua on the S3 between waits
    lim.slice_ms = 250;
    lim.commands = COMMANDS;
    s_rt = sb_new(&pf, &lim);
    s_q = xQueueCreateWithCaps(SCRIPT_QUEUE_LEN, sizeof(script_msg_t *), MALLOC_CAP_SPIRAM);
    if (!s_rt || !s_q
        || xTaskCreatePinnedToCoreWithCaps(script_task, "muse_script", SCRIPT_STACK, NULL, SCRIPT_PRIO, NULL,
                                           tskNO_AFFINITY, MALLOC_CAP_SPIRAM) != pdPASS) {
        ESP_LOGE(TAG, "no memory for scripts");
        s_q = NULL;
        return;
    }
    muse_hw_set_listener(on_hw_event);
    store_mount();
    static esp_timer_handle_t timer;
    const esp_timer_create_args_t targs = { .callback = healthy, .name = "script_ok" };
    if (esp_timer_create(&targs, &timer) == ESP_OK) esp_timer_start_once(timer, HEALTHY_US);
    const esp_timer_create_args_t bargs = { .callback = autostart_later, .name = "script_boot" };
    if (esp_timer_create(&bargs, &s_boot) == ESP_OK) esp_timer_start_once(s_boot, AUTOSTART_DELAY_MS * 1000LL);
    ESP_LOGI(TAG, "scripts ready%s", s_mounted ? "" : " (not kept: no storage)");
}

// ---- Commands -----------------------------------------------------------------------

bool muse_script_storage(void) {
    return s_mounted;
}

static bool valid_name(const char *name) {
    size_t n = name ? strlen(name) : 0;
    if (n < 1 || n > 31) return false;
    for (size_t i = 0; i < n; i++) {
        char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) {
            return false;
        }
    }
    return true;
}

void muse_script_register(cJSON *commands) {
    cJSON *inst = cJSON_CreateObject();
    cJSON_AddItemToObject(inst, "name", hw_param("string", "1-31 letters, digits, _ or -."));
    cJSON_AddItemToObject(inst, "source", hw_param("string", "The Lua 5.4 source, up to 16 KB."));
    cJSON *inst_opt = cJSON_CreateObject();
    cJSON_AddItemToObject(inst_opt, "autostart", hw_param("boolean", "Start it at every boot; default false."));
    cJSON_AddItemToObject(inst_opt, "run", hw_param("boolean", "Start it now; default true."));
    hw_add(commands, "script.install",
           "Save a Lua script on the device, and start it. Scripts react to the device without "
           "the cloud: on(\"tap\"|\"long_press\"|\"swipe\"|\"wheel_click\"|\"wheel_turn\"|\"button\"|"
           "\"detection\", fn(e)), every(ms, fn), after(ms, fn), wait(ms), cancel(id), now(), print(), "
           "stop(), notify(text) (a chat message to you; at most every 10 s). Device commands are "
           "functions, e.g. led.set{color=\"red\"}, camera.detect{model=\"person\"}, "
           "audio.beep{tones=\"880:200\"}, display.show_ui{...}; each returns its payload, or nil, "
           "message, code. Also device.call(name, params). Budgets: 256 KB of memory each, and a "
           "callback that runs ~0.1 s without waiting is stopped.",
           inst, inst_opt, 10000);
    cJSON *run_opt = cJSON_CreateObject();
    cJSON_AddItemToObject(run_opt, "name", hw_param("string", "A saved script."));
    cJSON_AddItemToObject(run_opt, "source", hw_param("string", "Or Lua to run once without saving."));
    hw_add(commands, "script.run", "Start (or restart) a saved script, or run Lua without saving it.",
           NULL, run_opt, 10000);
    cJSON *name = hw_params("name", hw_param("string", "The script."));
    hw_add(commands, "script.stop", "Stop a running script.", name, NULL, 5000);
    hw_add(commands, "script.delete", "Stop a script and delete it from the device.",
           cJSON_Duplicate(name, true), NULL, 5000);
    hw_add(commands, "script.logs", "A script's last 32 lines of print() output and errors.",
           cJSON_Duplicate(name, true), NULL, 5000);
    hw_add(commands, "script.read",
           "A saved script's source and whether it starts at boot: to see how one works, or to "
           "change it and install it again. app.library's examples are scripts too.",
           cJSON_Duplicate(name, true), NULL, 5000);
    hw_add(commands, "script.list",
           "The saved scripts (name, bytes, autostart) and the loaded ones (state, error, memory, "
           "timers, handlers, CPU time).", NULL, NULL, 5000);
}

static cJSON *install(cJSON *params) {
    const char *name = hw_str(params, "name");
    const char *src = hw_str(params, "source");
    if (!valid_name(name)) return hw_error("invalid_params", "name is 1-31 letters, digits, _ or -");
    if (!src) return hw_error("missing_param", "source is required");
    size_t len = strlen(src);
    if (len > SCRIPT_MAX) return hw_error("invalid_params", "the source is over 16 KB");
    char err[200] = "";
    if (!task_check(src, len, err, sizeof(err))) return hw_error("syntax_error", err[0] ? err : "doesn't compile");
    bool autostart = cJSON_IsTrue(cJSON_GetObjectItem(params, "autostart"));
    cJSON *run = cJSON_GetObjectItem(params, "run");
    bool saved = store_save(name, src, len, autostart);
    cJSON *pl = cJSON_CreateObject();
    cJSON_AddStringToObject(pl, "name", name);
    cJSON_AddBoolToObject(pl, "saved", saved);
    cJSON_AddBoolToObject(pl, "autostart", saved && autostart);
    if (!cJSON_IsBool(run) || cJSON_IsTrue(run)) {
        bool ok = task_start(name, src, len, err, sizeof(err));
        cJSON_AddBoolToObject(pl, "running", ok);
        if (!ok) cJSON_AddStringToObject(pl, "error", err);
    }
    return hw_ok(pl);
}

static cJSON *run(cJSON *params) {
    const char *name = hw_str(params, "name");
    const char *src = hw_str(params, "source");
    char err[200] = "";
    if (src) {
        if (!name) name = "adhoc";
        if (!valid_name(name)) return hw_error("invalid_params", "name is 1-31 letters, digits, _ or -");
        if (strlen(src) > SCRIPT_MAX) return hw_error("invalid_params", "the source is over 16 KB");
        if (!task_start(name, src, strlen(src), err, sizeof(err))) return hw_error("script_error", err);
    } else {
        if (!valid_name(name)) return hw_error("missing_param", "name or source is required");
        size_t len;
        char *saved = store_load(name, &len);
        if (!saved) return hw_error("not_found", "no script saved by that name");
        bool ok = task_start(name, saved, len, err, sizeof(err));
        heap_caps_free(saved);
        if (!ok) return hw_error("script_error", err);
    }
    cJSON *pl = cJSON_CreateObject();
    cJSON_AddStringToObject(pl, "name", name);
    cJSON_AddBoolToObject(pl, "running", true);
    return hw_ok(pl);
}

cJSON *muse_script_command(const char *command, cJSON *params) {
    if (!s_q) return hw_error("unavailable", "scripts didn't start (no memory)");
    if (!strcmp(command, "script.install")) return install(params);
    if (!strcmp(command, "script.run")) return run(params);
    const char *name = hw_str(params, "name");
    if (!strcmp(command, "script.list")) {
        cJSON *pl = cJSON_CreateObject();
        cJSON_AddItemToObject(pl, "saved", store_list());
        cJSON *loaded = task_query(MSG_LIST, NULL);
        cJSON_AddItemToObject(pl, "loaded", loaded ? loaded : cJSON_CreateArray());
        cJSON_AddBoolToObject(pl, "storage", s_mounted);
        return hw_ok(pl);
    }
    if (!valid_name(name)) return hw_error("missing_param", "name is required");
    if (!strcmp(command, "script.stop")) {
        if (!task_stop(name)) return hw_error("not_found", "no running script by that name");
        return hw_ok(NULL);
    }
    if (!strcmp(command, "script.delete")) {
        task_stop(name);
        muse_hw_apps_remove_owned(name);
        if (!store_delete(name)) return hw_error("not_found", "no script saved by that name");
        return hw_ok(NULL);
    }
    if (!strcmp(command, "script.read")) {
        size_t len;
        char *src = store_load(name, &len);
        if (!src) return hw_error("not_found", "no script saved by that name");
        char p[64];
        path(p, sizeof(p), name, "auto");
        struct stat st;
        cJSON *pl = cJSON_CreateObject();
        cJSON_AddStringToObject(pl, "name", name);
        cJSON_AddStringToObject(pl, "source", src);
        cJSON_AddBoolToObject(pl, "autostart", stat(p, &st) == 0);
        heap_caps_free(src);
        return hw_ok(pl);
    }
    if (!strcmp(command, "script.logs")) {
        cJSON *logs = task_query(MSG_LOGS, name);
        if (!logs) return hw_error("not_found", "no script loaded by that name");
        cJSON *pl = cJSON_CreateObject();
        cJSON_AddItemToObject(pl, "lines", logs);
        return hw_ok(pl);
    }
    return NULL;
}
