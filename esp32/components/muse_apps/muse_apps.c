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
 * WebAssembly apps: the runtime. The app task and its lifecycle, events,
 * operations, timers and frames, the memory budget, the manifest, and Muse's
 * app.run, app.stop, app.status, app.logs and app.api, plus the console's
 * loader. The API apps call lives in the modules muse_apps_priv.h lists.
 */

#include "muse_apps.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_pthread.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "muse_app_view.h"
#include "muse_apps_flash.h"
#include "muse_apps_priv.h"
#include "muse_apps_wamr_mem.h"
#include "muse_board.h"
#include "capabilities_registry.h"
#include "muse_link.h"
#include "muse_settings.h"
#include "muse_state.h"
#include "muse_ui.h"
#include "muse_voice.h"
#include "sdkconfig.h"
#if CONFIG_MUSE_VIDEO
#include "muse_video.h"
#endif
#include "wasm_export.h"

static const char *TAG = "muse_apps";

#define APP_MAX_BYTES (256 * 1024)          /* limits.module_bytes */
#define BUDGET_MAX_BYTES (2 * 1024 * 1024)  /* limits.budget_bytes: the most an app gets */
#define BUDGET_MIN_BYTES (1024 * 1024)      /* less than this beside the reserve: no app runs */
#define RESERVE_BYTES (1024 * 1024)         /* PSRAM left for the rest of the gadget */
#define FETCH_TIMEOUT_MS 30000
#define STOP_GRACE_MS 2000                  /* on_stop's saves may finish this long */
#define WASM_STACK_BYTES (16 * 1024)        /* the app's operand and call stack */
#define TASK_STACK_BYTES (12 * 1024)        /* the interpreter's own, in PSRAM */
#define TASK_PRIORITY 2                     /* below the UI, input and the network */
#define STEP_LIMIT 4000000                  /* limits.instructions: well under a second */
#define START_STEP_LIMIT 16000000           /* limits.instructions_start */
#define POLL_MS 50                          /* requests and state changes, while running */
#define PATH_MAX_LEN 192
#define REQUEST_ID_LEN 64
#define MAX_OPS 4                           /* limits.ops */
#define MAX_TIMERS 8                        /* limits.timers */
#define MAX_NAMES 16                        /* entries in a manifest's requires or uses */
#define NAME_LEN 32
#define LOG_BYTES 4096
#define LOG_LINE 200                        /* limits.log_line_bytes */

_Static_assert(MUSE_APP_EV_BUTTON == GADGET_EV_BUTTON && MUSE_APP_EV_TOUCH_DOWN == GADGET_EV_TOUCH_DOWN &&
               MUSE_APP_EV_TOUCH_MOVE == GADGET_EV_TOUCH_MOVE && MUSE_APP_EV_TOUCH_UP == GADGET_EV_TOUCH_UP &&
               MUSE_APP_EV_GESTURE == GADGET_EV_GESTURE, "the view's events are the API's");
_Static_assert(MUSE_APP_BUTTON_PRIMARY == GADGET_BUTTON_PRIMARY && MUSE_APP_BUTTON_BACK == GADGET_BUTTON_BACK &&
               MUSE_APP_BTN_LONG == GADGET_BTN_LONG && MUSE_APP_GESTURE_DOWN == GADGET_GESTURE_DOWN,
               "the view's buttons and gestures are the API's");
_Static_assert(MUSE_APP_STOP_USER == GADGET_STOP_USER && MUSE_APP_STOP_CALL == GADGET_STOP_CALL &&
               MUSE_APP_STOP_VIDEO == GADGET_STOP_VIDEO, "the view's stop reasons are the API's");


typedef enum { REQ_RUN_VM, REQ_RUN_BYTES, REQ_OPEN_FAILED, REQ_PUT, REQ_STOP, REQ_LAUNCHER } req_kind_t;

typedef struct {
    req_kind_t kind;
    muse_apps_origin_t o;
    int reason;                     /* REQ_STOP from the view: a GADGET_STOP_; 0 for app.stop */
    bool removed;                   /* REQ_STOP: installed app id ("" any) was removed: close it if it runs */
    char id[MUSE_APPS_ID_MAX];      /* REQ_RUN_BYTES: the installed app ("" from the console); REQ_STOP: removed */
    char name[MUSE_APPS_NAME_MAX];  /* REQ_RUN_BYTES: its name in the launcher */
    char path[PATH_MAX_LEN];        /* REQ_RUN_VM, REQ_PUT */
    char code[24];                  /* REQ_OPEN_FAILED */
    char message[128];              /* REQ_OPEN_FAILED */
    uint8_t *bytes;                 /* REQ_RUN_BYTES, REQ_PUT: owned by the request */
    size_t len;
} req_t;

typedef struct {
    int type, a, b;
} input_t;

#define EV_WAKE 0   /* only wakes the task: a request, or an operation finished */
#define STOP_TRAP 100

typedef struct {
    int32_t id;          /* 0: free */
    bool done;
    uint32_t app_off, cap;
    void *data;
    size_t len;
    int32_t result;
} op_t;

typedef struct {
    bool on, repeat;
    int64_t period_us, due_us;
} app_timer_t;

static QueueHandle_t s_reqs, s_inputs;
#define INPUT_QUEUE_LEN 32
static muse_apps_result_fn s_send_result;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;   /* ops, memory, the log */

/* The running app: the app task, unless noted. In PSRAM (muse_apps_start). */
typedef struct {
    wasm_module_t module;
    wasm_module_inst_t inst;
    wasm_exec_env_t env;
    wasm_function_inst_t on_frame, on_event, on_stop;
    char name[NAME_LEN + 16];
    char source[PATH_MAX_LEN];
    char id[MUSE_APPS_ID_MAX];      /* an installed app's, "" otherwise */
    char title[MUSE_APPS_NAME_MAX]; /* its name in the launcher */
    bool from_launcher;             /* closing it goes back to the launcher */
    bool running;
    bool ui_locked;
    bool has_manifest;
    char uses[MAX_NAMES][NAME_LEN];
    int n_uses;
    char requires[MAX_NAMES][NAME_LEN];
    int n_requires;
    int64_t started_us;
    uint32_t frames, events;
    int frame_rate;
    int64_t next_frame_us;
    int mode;                  /* the last Muse mode the app was told */
    int dark, online;          /* the last screen and link states it was told */
    bool keep_awake;
    int64_t last_poke_us;
    bool exit_asked;
    app_timer_t timers[MAX_TIMERS + 1];
} app_state_t;

/* The launcher has the screen (no app runs): the app task only. */
static bool s_launcher;
static int64_t s_launcher_next_us;

/* The installed app running, for app.list on Link's task. */
static portMUX_TYPE s_running_mux = portMUX_INITIALIZER_UNLOCKED;
static char s_running_id[MUSE_APPS_ID_MAX];

/* Its last exit, for app.status. */
typedef struct {
    char app[NAME_LEN + 16];
    char reason[16];
    char message[160];
    int64_t at_us;
    uint32_t frames;
    size_t peak;
} last_exit_t;

/* Both in PSRAM: internal RAM is what Wi-Fi, Noise and the panel's DMA run
 * short of. NULL until muse_apps_start. */
static struct {
    app_state_t app;
    last_exit_t last;
} *s_state;
#define s_app (s_state->app)
#define s_last (s_state->last)

static op_t s_ops[MAX_OPS];
static int32_t s_next_op = 1;

/* The budget: what WAMR holds for the app, what the modules allocated, and
 * what they charged for LVGL. Under s_mux. */
static struct {
    bool counting;
    size_t wamr, fw, charged, peak;
    bool over_logged;
} s_mem;
/* The running app's budget, set when it starts (budget_now()). Under s_mux. */
static size_t s_budget = BUDGET_MAX_BYTES;

/* The app's log: a ring of lines, under s_mux. */
static char *s_log;
static size_t s_log_head, s_log_len;

/* The console's upload in progress: the serial task only. */
static uint8_t *s_stage;
static size_t s_stage_len;

/* ---- Small things ------------------------------------------------------------------- */

static size_t used_locked(void)
{
    return s_mem.wamr + s_mem.fw + s_mem.charged;
}

static const char *stop_name(int reason)
{
    switch (reason) {
    case GADGET_STOP_USER: return "user";
    case GADGET_STOP_REPLACED: return "replaced";
    case GADGET_STOP_CALL: return "call";
    case GADGET_STOP_VIDEO: return "video";
    case GADGET_STOP_EXIT: return "exit";
    case GADGET_STOP_UPDATE: return "update";
    case STOP_TRAP: return "trap";
    default: return "stopped";
    }
}

const char *muse_apps_name(void)
{
    return s_state ? s_app.name : "";
}

bool muse_apps_post(int type, int a, int b)
{
    input_t in = {type, a, b};
    return s_inputs && xQueueSend(s_inputs, &in, 0) == pdTRUE;
}

static void wake(void)
{
    muse_apps_post(EV_WAKE, 0, 0);
}

static bool listed(char names[][NAME_LEN], int n, const char *name)
{
    for (int i = 0; name && i < n; i++) {
        if (!strcmp(names[i], name)) {
            return true;
        }
    }
    return false;
}

bool muse_apps_permitted(const char *perm)
{
    return s_state && listed(s_app.uses, s_app.n_uses, perm);
}

bool muse_apps_device_allowed(const char *name)
{
    return s_state && (listed(s_app.uses, s_app.n_uses, name) || listed(s_app.requires, s_app.n_requires, name));
}

void muse_apps_keep_awake(bool on)
{
    s_app.keep_awake = on;
}

void muse_apps_exit(void)
{
    s_app.exit_asked = true;
}

static char s_device_dir[80];
static pthread_once_t s_device_dir_once = PTHREAD_ONCE_INIT;

static void init_device_dir(void)
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_device_dir, sizeof(s_device_dir), "workspace/gadgets/devices/%s%s-%02X%02X%02X",
             CONFIG_HOMEHUB_BLE_NAME_PREFIX, CONFIG_HOMEHUB_BLE_NAME_SUFFIX, mac[3], mac[4], mac[5]);
}

const char *muse_apps_device_dir(void)
{
    pthread_once(&s_device_dir_once, init_device_dir);
    return s_device_dir;
}

int32_t muse_apps_vm_path(const char *path, char *out, size_t cap)
{
    if (!path || !*path || path[0] == '/' || strlen(path) >= cap) {
        return GADGET_ERR_ARG;
    }
    for (const char *p = path; *p; p++) {   /* no ".." segment */
        if (p[0] == '.' && p[1] == '.' && (p == path || p[-1] == '/') && (p[2] == '/' || p[2] == '\0')) {
            return GADGET_ERR_ARG;
        }
    }
    if (path[0] == '~' && path[1] == '/') {
        if (!muse_apps_permitted("files.home")) {
            return GADGET_ERR_DENIED;
        }
        snprintf(out, cap, "%s", path + 2);
        return 0;
    }
    const char *slash = strrchr(s_app.source, '/');
    if (!slash) {
        return GADGET_ERR_UNSUPPORTED;   /* a console app has no folder */
    }
    int n = snprintf(out, cap, "%.*s/%s", (int)(slash - s_app.source), s_app.source, path);
    return n > 0 && (size_t)n < cap ? 0 : GADGET_ERR_ARG;
}

int32_t muse_apps_gadget_path(const char *path, char *name, size_t cap)
{
    if (!path || path[0] != '/' || !muse_apps_flash_name_ok(path + 1) || strlen(path + 1) >= cap) {
        return GADGET_ERR_ARG;
    }
    if (!muse_apps_permitted("files.gadget")) {
        return GADGET_ERR_DENIED;
    }
    snprintf(name, cap, "%s", path + 1);
    return 0;
}

/* ---- The log -------------------------------------------------------------------------- */

static void log_append(const char *line)
{
    if (!s_log) {
        return;
    }
    size_t n = strlen(line);
    taskENTER_CRITICAL(&s_mux);
    for (size_t i = 0; i <= n; i++) {   /* with its NUL, which separates lines */
        s_log[(s_log_head + s_log_len) % LOG_BYTES] = i < n ? line[i] : '\0';
        if (s_log_len < LOG_BYTES) {
            s_log_len++;
        } else {
            s_log_head = (s_log_head + 1) % LOG_BYTES;
        }
    }
    taskEXIT_CRITICAL(&s_mux);
}

void muse_apps_log(const char *fmt, ...)
{
    if (!s_state) {
        return;
    }
    char line[LOG_LINE + 16];
    int64_t ms = s_app.started_us ? (esp_timer_get_time() - s_app.started_us) / 1000 : 0;
    int k = snprintf(line, sizeof(line), "%lld.%03lld ", (long long)(ms / 1000), (long long)(ms % 1000));
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line + k, sizeof(line) - k, fmt, ap);
    va_end(ap);
    ESP_LOGI(TAG, "%s: %s", s_app.name, line + k);
    log_append(line);
}

/* ---- WAMR's memory, all in PSRAM, counted while an app is loaded ------------------------ */
/* Not WAMR's own ESP-IDF os_realloc: it returns the data shifted by 4 bytes
 * when realloc() moves a block to the other 8-byte alignment, which corrupts
 * the loader's work stacks on a large module. Blocks are 8-byte aligned with
 * their size and whether they're counted in front. */

typedef struct {
    uint32_t size, counted;
} block_t;

/* Whether bytes more fit the budget beside what's used, without wrapping
 * around for a size near SIZE_MAX. Under s_mux. */
static bool fits_locked(size_t bytes)
{
    size_t used = used_locked();
    return used <= s_budget && bytes <= s_budget - used;
}

static void *mem_alloc(unsigned size)
{
    if (size > UINT32_MAX - sizeof(block_t)) {
        return NULL;
    }
    bool counted = false;
    taskENTER_CRITICAL(&s_mux);
    if (s_mem.counting) {
        if (!fits_locked(size)) {
            taskEXIT_CRITICAL(&s_mux);
            if (!s_mem.over_logged) {
                s_mem.over_logged = true;
                ESP_LOGW(TAG, "app over its memory budget (%u bytes asked)", size);
            }
            return NULL;
        }
        s_mem.wamr += size;
        counted = true;
        size_t u = used_locked();
        if (u > s_mem.peak) {
            s_mem.peak = u;
        }
    }
    taskEXIT_CRITICAL(&s_mux);
    block_t *b = heap_caps_aligned_alloc(8, size + sizeof(block_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!b) {
        if (counted) {
            taskENTER_CRITICAL(&s_mux);
            s_mem.wamr -= size;
            taskEXIT_CRITICAL(&s_mux);
        }
        return NULL;
    }
    b->size = size;
    b->counted = counted;
    return b + 1;
}

static void mem_free(void *p)
{
    if (!p) {
        return;
    }
    block_t *b = (block_t *)p - 1;
    if (b->counted) {
        taskENTER_CRITICAL(&s_mux);
        s_mem.wamr -= b->size;
        taskEXIT_CRITICAL(&s_mux);
    }
    heap_caps_free(b);
}

static void *mem_realloc(void *p, unsigned size)
{
    if (!p) {
        return mem_alloc(size);
    }
    block_t *b = (block_t *)p - 1;
    if (size <= b->size) {
        return p;
    }
    void *n = mem_alloc(size);
    if (!n) {
        return NULL;
    }
    memcpy(n, p, b->size);
    mem_free(p);
    return n;
}

/* ---- WAMR's linear memory (muse_apps_wamr_mem.c) ------------------------------------------ */
/* WAMR maps it through its platform layer, not the allocator above: these
 * keep it in PSRAM and in the budget like the rest of WAMR's memory, and zero
 * it, as WAMR expects of a new page. */

void *muse_apps_linear_map(size_t size)
{
    void *p = size <= UINT32_MAX ? mem_alloc((unsigned)size) : NULL;
    if (p) {
        memset(p, 0, size);
    }
    return p;
}

void *muse_apps_linear_remap(void *old, size_t old_size, size_t new_size)
{
    if (!old) {
        return muse_apps_linear_map(new_size);
    }
    block_t *b = (block_t *)old - 1;
    if (new_size <= b->size) {
        return old;   /* linear memory only grows */
    }
    if (new_size > UINT32_MAX - sizeof(block_t)) {
        return NULL;
    }
    /* Only the growth is new to the budget: the old block goes once copied. */
    size_t more = new_size - b->size;
    if (b->counted) {
        taskENTER_CRITICAL(&s_mux);
        bool ok = fits_locked(more);
        if (ok) {
            s_mem.wamr += more;
            size_t u = used_locked();
            if (u > s_mem.peak) {
                s_mem.peak = u;
            }
        }
        taskEXIT_CRITICAL(&s_mux);
        if (!ok) {
            return NULL;
        }
    }
    block_t *n = heap_caps_aligned_alloc(8, new_size + sizeof(block_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!n) {
        if (b->counted) {
            taskENTER_CRITICAL(&s_mux);
            s_mem.wamr -= more;
            taskEXIT_CRITICAL(&s_mux);
        }
        return NULL;
    }
    n->size = (uint32_t)new_size;
    n->counted = b->counted;
    size_t keep = old_size < b->size ? old_size : b->size;
    memcpy(n + 1, old, keep);
    memset((uint8_t *)(n + 1) + keep, 0, new_size - keep);
    heap_caps_free(b);
    return n + 1;
}

void muse_apps_linear_unmap(void *p)
{
    mem_free(p);
}

/* ---- The modules' memory ----------------------------------------------------------------- */

bool muse_apps_charge(size_t bytes)
{
    bool ok;
    taskENTER_CRITICAL(&s_mux);
    ok = !s_mem.counting || fits_locked(bytes);
    if (ok && s_mem.counting) {
        s_mem.charged += bytes;
        size_t u = used_locked();
        if (u > s_mem.peak) {
            s_mem.peak = u;
        }
    }
    taskEXIT_CRITICAL(&s_mux);
    return ok;
}

void muse_apps_uncharge(size_t bytes)
{
    taskENTER_CRITICAL(&s_mux);
    s_mem.charged = bytes > s_mem.charged ? 0 : s_mem.charged - bytes;
    taskEXIT_CRITICAL(&s_mux);
}

void *muse_apps_alloc(size_t bytes)
{
    if (bytes > SIZE_MAX - sizeof(block_t)) {
        return NULL;
    }
    taskENTER_CRITICAL(&s_mux);
    bool ok = fits_locked(bytes);
    if (ok) {
        s_mem.fw += bytes;
        size_t u = used_locked();
        if (u > s_mem.peak) {
            s_mem.peak = u;
        }
    }
    taskEXIT_CRITICAL(&s_mux);
    if (!ok) {
        return NULL;
    }
    block_t *b = heap_caps_aligned_alloc(8, bytes + sizeof(block_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!b) {
        taskENTER_CRITICAL(&s_mux);
        s_mem.fw -= bytes;
        taskEXIT_CRITICAL(&s_mux);
        return NULL;
    }
    b->size = bytes;
    b->counted = 1;
    return b + 1;
}

void *muse_apps_calloc(size_t n, size_t size)
{
    if (size && n > SIZE_MAX / size) {
        return NULL;
    }
    void *p = muse_apps_alloc(n * size);
    if (p) {
        memset(p, 0, n * size);
    }
    return p;
}

void muse_apps_free(void *p)
{
    if (!p) {
        return;
    }
    block_t *b = (block_t *)p - 1;
    taskENTER_CRITICAL(&s_mux);
    s_mem.fw = b->size > s_mem.fw ? 0 : s_mem.fw - b->size;
    taskEXIT_CRITICAL(&s_mux);
    heap_caps_free(b);
}

size_t muse_apps_mem_used(void)
{
    taskENTER_CRITICAL(&s_mux);
    size_t u = used_locked();
    taskEXIT_CRITICAL(&s_mux);
    return u;
}

/* What an app started now would get: the PSRAM free (and about to be freed)
 * beyond the reserve, at most limits.budget_bytes, in whole wasm pages. A
 * board with less PSRAM gives less; under BUDGET_MIN_BYTES no app runs. */
static size_t budget_now(size_t also_free)
{
    size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM) + also_free;
    size_t b = free_psram > RESERVE_BYTES ? free_psram - RESERVE_BYTES : 0;
    b = b > BUDGET_MAX_BYTES ? BUDGET_MAX_BYTES : b;
    return b & ~(size_t)(65536 - 1);
}

/* The loaded app's budget (from its start, on_start included); with none,
 * what one started now would get. */
size_t muse_apps_mem_budget(void)
{
    taskENTER_CRITICAL(&s_mux);
    bool loaded = s_mem.counting;
    size_t b = s_budget;
    taskEXIT_CRITICAL(&s_mux);
    return loaded ? b : budget_now(0);
}

/* ---- LVGL's lock ------------------------------------------------------------------------- */

void muse_apps_ui_lock(void)
{
    if (!s_app.ui_locked) {
        muse_app_view_lock();
        s_app.ui_locked = true;
    }
}

static void ui_release(void)
{
    if (s_app.ui_locked) {
        s_app.ui_locked = false;
        muse_app_view_unlock();
    }
}

/* ---- Operations ---------------------------------------------------------------------------- */

int32_t muse_apps_op_begin(wasm_exec_env_t env, void *buf, uint32_t cap)
{
    uint32_t off = 0;
    if (buf) {
        off = (uint32_t)wasm_runtime_addr_native_to_app(wasm_runtime_get_module_inst(env), buf);
    }
    int32_t id = GADGET_ERR_LIMIT;
    taskENTER_CRITICAL(&s_mux);
    for (int i = 0; i < MAX_OPS; i++) {
        if (!s_ops[i].id) {
            id = s_next_op;
            s_next_op = s_next_op >= 0x7FFFFFF0 ? 1 : s_next_op + 1;
            s_ops[i] = (op_t){.id = id, .app_off = off, .cap = buf ? cap : 0};
            break;
        }
    }
    taskEXIT_CRITICAL(&s_mux);
    return id;
}

void muse_apps_op_finish(int32_t op, int32_t result, void *data, size_t len)
{
    bool found = false;
    taskENTER_CRITICAL(&s_mux);
    for (int i = 0; i < MAX_OPS; i++) {
        if (s_ops[i].id == op && !s_ops[i].done) {
            s_ops[i].done = true;
            s_ops[i].result = result;
            s_ops[i].data = data;
            s_ops[i].len = len;
            found = true;
            break;
        }
    }
    taskEXIT_CRITICAL(&s_mux);
    if (!found) {
        free(data);   /* the app stopped, or it was finished twice */
        return;
    }
    wake();
}

static int ops_pending(void)
{
    int n = 0;
    taskENTER_CRITICAL(&s_mux);
    for (int i = 0; i < MAX_OPS; i++) {
        n += s_ops[i].id && !s_ops[i].done;
    }
    taskEXIT_CRITICAL(&s_mux);
    return n;
}

/* Takes one finished operation off the table, or returns false. */
static bool take_done(op_t *out)
{
    bool got = false;
    taskENTER_CRITICAL(&s_mux);
    for (int i = 0; i < MAX_OPS && !got; i++) {
        if (s_ops[i].id && s_ops[i].done) {
            *out = s_ops[i];
            s_ops[i] = (op_t){0};
            got = true;
        }
    }
    taskEXIT_CRITICAL(&s_mux);
    return got;
}

static void ops_clear(void)
{
    op_t gone[MAX_OPS];
    taskENTER_CRITICAL(&s_mux);
    memcpy(gone, s_ops, sizeof(gone));
    memset(s_ops, 0, sizeof(s_ops));
    taskEXIT_CRITICAL(&s_mux);
    for (int i = 0; i < MAX_OPS; i++) {
        free(gone[i].data);
    }
}

/* ---- Timers and frames ------------------------------------------------------------------------ */

int32_t muse_apps_timer_set(int32_t id, int32_t ms, bool repeat)
{
    if (id < 1 || id > MAX_TIMERS || ms < 10) {
        return GADGET_ERR_ARG;
    }
    app_timer_t *t = &s_app.timers[id];
    t->on = true;
    t->repeat = repeat;
    t->period_us = (int64_t)ms * 1000;
    t->due_us = esp_timer_get_time() + t->period_us;
    return 0;
}

int32_t muse_apps_timer_stop(int32_t id)
{
    if (id < 1 || id > MAX_TIMERS) {
        return GADGET_ERR_ARG;
    }
    s_app.timers[id].on = false;
    return 0;
}

int32_t muse_apps_frame_rate(int32_t fps)
{
    if (fps < 0 || fps > 30) {
        return GADGET_ERR_ARG;
    }
    s_app.frame_rate = fps;
    s_app.next_frame_us = esp_timer_get_time();
    return 0;
}

/* ---- Replies ----------------------------------------------------------------------------------- */

cJSON *muse_apps_error(const char *code, const char *message)
{
    cJSON *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "ok", false);
    cJSON *error = cJSON_AddObjectToObject(result, "error");
    cJSON_AddStringToObject(error, "code", code);
    cJSON_AddStringToObject(error, "message", message);
    return result;
}

static cJSON *ok_result(cJSON **payload)
{
    cJSON *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "ok", true);
    *payload = cJSON_AddObjectToObject(result, "payload");
    return result;
}

void muse_apps_reply(const muse_apps_origin_t *o, cJSON *result)
{
    if (o->from == MUSE_APPS_FROM_MUSE) {
        if (s_send_result) {
            s_send_result(o->session_generation, o->request_id, result);
        } else {
            cJSON_Delete(result);
        }
        return;
    }
    if (o->from == MUSE_APPS_FROM_CONSOLE) {
        char *text = cJSON_PrintUnformatted(result);
        printf("@app %s\n", text ? text : "{\"ok\":false}");
        fflush(stdout);
        free(text);
    }
    cJSON_Delete(result);
}

/* ---- Module hooks: empty unless a module defines its own (the component links whole) ---- */

__attribute__((weak)) void muse_apps_ui_start(void) {}
__attribute__((weak)) void muse_apps_chat_start(void) {}
__attribute__((weak)) void muse_apps_dev_start(void) {}
__attribute__((weak)) void muse_apps_ui_stop(void) {}
__attribute__((weak)) void muse_apps_canvas_stop(void) {}
__attribute__((weak)) void muse_apps_chat_stop(void) {}
__attribute__((weak)) void muse_apps_files_stop(void) {}
__attribute__((weak)) void muse_apps_dev_stop(void) {}
__attribute__((weak)) void muse_apps_sys_stop(void) {}

/* ---- Running an app (the app task) ---------------------------------------------------------------- */

static void on_view_input(int type, int a, int b)
{
    muse_apps_post(type, a, b);   /* full: the app is behind, drop it */
}

static void on_view_exit(int reason);

void muse_apps_launcher_close(void)
{
    on_view_exit(GADGET_STOP_USER);   /* as the power key */
}

/* A view exit (a call or video taking the screen, power, the exit hold) the
 * request queue had no room for, as its reason + 1: it mustn't be lost. */
static atomic_int s_view_stop;

static void on_view_exit(int reason)
{
    req_t r = {.kind = REQ_STOP, .o = {.from = MUSE_APPS_FROM_VIEW}, .reason = reason};
    if (!s_reqs || xQueueSend(s_reqs, &r, 0) != pdTRUE) {
        atomic_store(&s_view_stop, reason + 1);
    }
    wake();
}

static void unload(void)
{
    if (s_app.env) {
        wasm_runtime_destroy_exec_env(s_app.env);
    }
    if (s_app.inst) {
        wasm_runtime_deinstantiate(s_app.inst);
    }
    if (s_app.module) {
        wasm_runtime_unload(s_app.module);
    }
    s_app.env = NULL;
    s_app.inst = NULL;
    s_app.module = NULL;
    s_app.on_frame = s_app.on_event = s_app.on_stop = NULL;
    taskENTER_CRITICAL(&s_mux);
    size_t wamr = s_mem.wamr, fw = s_mem.fw, charged = s_mem.charged;
    s_mem = (typeof(s_mem)){0};
    taskEXIT_CRITICAL(&s_mux);
    if (wamr || fw || charged) {
        ESP_LOGW(TAG, "after the app: %u bytes WAMR, %u modules, %u charged still counted", (unsigned)wamr,
                 (unsigned)fw, (unsigned)charged);
    }
}

/* Calls into the app with an instruction budget; false if it trapped. */
static bool call_app(wasm_function_inst_t fn, uint32_t argc, uint32_t *argv, int limit)
{
    wasm_runtime_set_instruction_count_limit(s_app.env, limit);
    bool ok = wasm_runtime_call_wasm(s_app.env, fn, argc, argv);
    ui_release();
    return ok;
}

static void set_running_id(const char *id)
{
    taskENTER_CRITICAL(&s_running_mux);
    snprintf(s_running_id, sizeof(s_running_id), "%s", id);
    taskEXIT_CRITICAL(&s_running_mux);
}

void muse_apps_running_id(char *out, size_t cap)
{
    taskENTER_CRITICAL(&s_running_mux);
    snprintf(out, cap, "%s", s_running_id);
    taskEXIT_CRITICAL(&s_running_mux);
}

/* Closes the running app. With keep_view the screen stays covered, its
 * objects gone, for the launcher or the next app. */
static void stop(int reason, const char *message, bool keep_view)
{
    if (!s_app.running && !s_app.inst) {
        return;
    }
    bool was_running = s_app.running;
    s_app.running = false;
    if (was_running && reason != STOP_TRAP && s_app.on_stop) {
        uint32_t argv[1] = {(uint32_t)reason};
        if (!call_app(s_app.on_stop, 1, argv, STEP_LIMIT)) {
            const char *what = wasm_runtime_get_exception(s_app.inst);
            muse_apps_log("on_stop trapped: %s", what ? what : "?");
        }
        /* Its saves copied their data: they may finish while the app is
         * already gone from the screen. */
        int64_t until = esp_timer_get_time() + STOP_GRACE_MS * 1000LL;
        while (ops_pending() && esp_timer_get_time() < until) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
    ops_clear();
    muse_apps_ui_stop();
    muse_apps_canvas_stop();
    muse_apps_chat_stop();
    muse_apps_files_stop();
    muse_apps_dev_stop();
    muse_apps_sys_stop();
    muse_apps_vm_close_all();
    if (!keep_view) {
        muse_app_view_close();
    }
    muse_ui_app_brightness(0);
    set_running_id("");
    xQueueReset(s_inputs);

    taskENTER_CRITICAL(&s_mux);
    size_t peak = s_mem.peak;
    taskEXIT_CRITICAL(&s_mux);
    snprintf(s_last.app, sizeof(s_last.app), "%s", s_app.name);
    snprintf(s_last.reason, sizeof(s_last.reason), "%s", stop_name(reason));
    snprintf(s_last.message, sizeof(s_last.message), "%s", message ? message : "");
    s_last.at_us = esp_timer_get_time();
    s_last.frames = s_app.frames;
    s_last.peak = peak;
    muse_apps_log("stopped (%s%s%s) after %u frames, %u events, peak %u KB", s_last.reason, message ? ": " : "",
                  message ? message : "", (unsigned)s_app.frames, (unsigned)s_app.events, (unsigned)(peak / 1024));
    unload();
    s_app.started_us = 0;
}

/* The launcher takes the screen (muse_apps_launcher.c), optionally with a
 * note at the bottom. */
static void show_launcher(const char *note)
{
    const muse_app_view_cbs_t cbs = {.input = on_view_input, .exit = on_view_exit};
    if (muse_voice_in_call()) {   /* the call keeps its buttons */
        muse_app_view_close();
        return;
    }
#if CONFIG_MUSE_VIDEO
    if (muse_video_is_open()) {
        muse_app_view_close();
        return;
    }
#endif
    if (!muse_app_view_open(&cbs)) {
        return;
    }
    xQueueReset(s_inputs);
    if (!muse_apps_launcher_show()) {
        muse_app_view_close();
        return;
    }
    s_launcher = true;
    s_launcher_next_us = esp_timer_get_time();
    muse_state_set_asleep(false);   /* asked for: Settings, the menu, the console, or back from an app */
    muse_state_poke();
    if (note) {
        muse_apps_launcher_note(note);
    }
}

/* The launcher gives the screen back: to the face, or (keep_view) to an app. */
static void hide_launcher(bool keep_view)
{
    if (!s_launcher) {
        return;
    }
    s_launcher = false;
    muse_apps_launcher_hide();
    if (!keep_view) {
        muse_app_view_close();
    }
}

/* A trap in a callback: the app is done. Tell the log and the console; then
 * back to the launcher if the app came from it, else to Muse's face. */
static void crashed(const char *where)
{
    const char *what = wasm_runtime_get_exception(s_app.inst);
    char msg[160];
    snprintf(msg, sizeof(msg), "trapped in %s: %s", where, what ? what : "?");
    printf("@app.exit {\"reason\":\"trap\",\"in\":\"%s\",\"message\":\"%s\"}\n", where, what ? what : "?");
    fflush(stdout);
    bool back = s_app.from_launcher;
    char note[80];
    snprintf(note, sizeof(note), "%s stopped", s_app.title[0] ? s_app.title : s_app.name);
    stop(STOP_TRAP, msg, back);
    if (back) {
        show_launcher(note);
    } else {
        muse_state_set_caption("APP STOPPED");
    }
}

/* Imports must all be the host API's: an app that imports anything else,
 * or anything that isn't a function, isn't run. */
static bool imports_allowed(char *err, size_t err_len)
{
    int32_t count = wasm_runtime_get_import_count(s_app.module);
    for (int32_t i = 0; i < count; i++) {
        wasm_import_t imp;
        wasm_runtime_get_import_type(s_app.module, i, &imp);
        if (imp.kind != WASM_IMPORT_EXPORT_KIND_FUNC || !imp.module_name || strcmp(imp.module_name, "gadget") ||
            !imp.linked) {
            snprintf(err, err_len, "imports %s.%s, which this gadget doesn't offer",
                     imp.module_name ? imp.module_name : "?", imp.name ? imp.name : "?");
            return false;
        }
    }
    return true;
}

/* ---- The manifest ---------------------------------------------------------------------------------- */

static bool leb(const uint8_t *p, size_t len, size_t *at, uint32_t *out)
{
    uint32_t v = 0;
    for (int shift = 0; shift < 35; shift += 7) {
        if (*at >= len) {
            return false;
        }
        uint8_t c = p[(*at)++];
        v |= (uint32_t)(c & 0x7F) << shift;
        if (!(c & 0x80)) {
            *out = v;
            return true;
        }
    }
    return false;
}

/* What WAMR would run itself while instantiating, before the instruction
 * limit applies, so that a loop there would hold the app task: a start
 * function, __post_instantiate or __wasm_call_ctors. NULL for none, as in the
 * reactors build.py makes (their _initialize runs here, metered). */
static const char *runs_itself(const uint8_t *p, size_t len)
{
    for (size_t at = 8; at < len;) {
        uint8_t id = p[at++];
        uint32_t size;
        if (!leb(p, len, &at, &size) || size > len - at) {
            break;
        }
        if (id == 8) {
            return "a start function";
        }
        at += size;
    }
    for (int32_t i = 0; i < wasm_runtime_get_export_count(s_app.module); i++) {
        wasm_export_t e;
        wasm_runtime_get_export_type(s_app.module, i, &e);
        if (e.name && (!strcmp(e.name, "__post_instantiate") || !strcmp(e.name, "__wasm_call_ctors"))) {
            return e.name;
        }
    }
    return NULL;
}

/* A callback export with the API's signature: n i32 parameters, no results.
 * WAMR writes results over the arguments it's given, so one more would
 * land past them. */
static bool callback_is(wasm_function_inst_t fn, uint32_t n)
{
    wasm_valkind_t types[3];
    if (!fn) {
        return true;
    }
    if (wasm_func_get_result_count(fn, s_app.inst) || wasm_func_get_param_count(fn, s_app.inst) != n || n > 3) {
        return false;
    }
    wasm_func_get_param_types(fn, s_app.inst, types);
    for (uint32_t i = 0; i < n; i++) {
        if (types[i] != WASM_I32) {
            return false;
        }
    }
    return true;
}

/* The gadget.manifest custom section, or NULL. */
static const uint8_t *find_manifest(const uint8_t *p, size_t len, size_t *out_len)
{
    static const char want[] = "gadget.manifest";
    size_t at = 8;
    if (len < 8 || memcmp(p, "\0asm", 4)) {
        return NULL;
    }
    while (at < len) {
        uint8_t id = p[at++];
        uint32_t size;
        if (!leb(p, len, &at, &size) || size > len - at) {
            return NULL;
        }
        size_t end = at + size;
        if (id == 0) {
            uint32_t n;
            size_t k = at;
            if (leb(p, end, &k, &n) && n == sizeof(want) - 1 && n <= end - k && !memcmp(p + k, want, n)) {
                *out_len = end - (k + n);
                return p + k + n;
            }
        }
        at = end;
    }
    return NULL;
}

static bool valid_app_name(const char *s)
{
    if (!s || !*s || strlen(s) >= NAME_LEN) {
        return false;
    }
    for (; *s; s++) {
        if (!((*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') || *s == '-')) {
            return false;
        }
    }
    return true;
}

static bool feature_present(const char *f)
{
    int w, h;
    bool round;
    muse_app_view_size(&w, &h, &round);
    unsigned b = muse_app_view_buttons_present();
    if (!strcmp(f, "touch")) return muse_board && muse_board->touch;
    if (!strcmp(f, "round")) return round;
    if (!strcmp(f, "rect")) return !round;
    if (!strcmp(f, "buttons2")) return b & (1u << MUSE_APP_BUTTON_SECONDARY);
    if (!strcmp(f, "nav")) return b & (1u << MUSE_APP_BUTTON_ENTER);
    return capabilities_registry_find(f) != NULL;   /* a device's name */
}

static bool read_names(cJSON *arr, char names[][NAME_LEN], int *n, const char *what, char *err, size_t err_len)
{
    *n = 0;
    if (!arr) {
        return true;
    }
    if (!cJSON_IsArray(arr)) {
        snprintf(err, err_len, "manifest: %s isn't a list", what);
        return false;
    }
    cJSON *it;
    cJSON_ArrayForEach(it, arr) {
        if (!cJSON_IsString(it) || strlen(it->valuestring) >= NAME_LEN || *n >= MAX_NAMES) {
            snprintf(err, err_len, "manifest: %s holds something too long, too many, or not a name", what);
            return false;
        }
        snprintf(names[(*n)++], NAME_LEN, "%s", it->valuestring);
    }
    return true;
}

/* False if it didn't fit whole. */
static bool copy_string(cJSON *j, const char *key, char *out, size_t cap)
{
    const char *v = cJSON_GetStringValue(cJSON_GetObjectItem(j, key));
    return cap && snprintf(out, cap, "%s", v ? v : "") < (int)cap;
}

bool muse_apps_manifest_info(const uint8_t *bytes, size_t len, char *name, size_t name_cap, char *title,
                             size_t title_cap, char *icon, size_t icon_cap, char *color, size_t color_cap)
{
    size_t mlen = 0;
    const uint8_t *m = bytes ? find_manifest(bytes, len, &mlen) : NULL;
    while (m && mlen && m[mlen - 1] == '\0') {
        mlen--;
    }
    cJSON *j = m ? cJSON_ParseWithLength((const char *)m, mlen) : NULL;
    if (!copy_string(j, "name", name, name_cap) && name_cap) {
        name[0] = '\0';   /* cut short, it could be another app's: no name, then */
    }
    copy_string(j, "title", title, title_cap);
    copy_string(j, "icon", icon, icon_cap);
    copy_string(j, "color", color, color_cap);
    bool ok = cJSON_IsObject(j);
    cJSON_Delete(j);
    return ok;
}

/* Reads and checks the manifest; false with a reason if the app can't run. */
static bool check_manifest(const uint8_t *bytes, size_t len, char *err, size_t err_len)
{
    s_app.has_manifest = false;
    s_app.n_uses = s_app.n_requires = 0;
    size_t mlen = 0;
    const uint8_t *m = find_manifest(bytes, len, &mlen);
    if (!m) {
        return true;   /* runs, with no permissions and no devices */
    }
    while (mlen && m[mlen - 1] == '\0') {
        mlen--;
    }
    cJSON *j = cJSON_ParseWithLength((const char *)m, mlen);
    if (!cJSON_IsObject(j)) {
        cJSON_Delete(j);
        snprintf(err, err_len, "the manifest isn't a JSON object");
        return false;
    }
    bool ok = false;
    cJSON *api = cJSON_GetObjectItem(j, "api");
    cJSON *name = cJSON_GetObjectItem(j, "name");
    int major = 0, minor = 0;
    if (!cJSON_IsString(api) || sscanf(api->valuestring, "%d.%d", &major, &minor) != 2) {
        snprintf(err, err_len, "manifest: api must be a version such as \"1.0\"");
    } else if (major != MUSE_APPS_API_MAJOR || minor > MUSE_APPS_API_MINOR) {
        snprintf(err, err_len, "manifest: written for API %d.%d; this gadget serves %d.%d", major, minor,
                 MUSE_APPS_API_MAJOR, MUSE_APPS_API_MINOR);
    } else if (!cJSON_IsString(name) || !valid_app_name(name->valuestring)) {
        snprintf(err, err_len, "manifest: name must be a-z, 0-9 and '-', under %d characters", NAME_LEN);
    } else if (read_names(cJSON_GetObjectItem(j, "requires"), s_app.requires, &s_app.n_requires, "requires", err,
                          err_len) &&
               read_names(cJSON_GetObjectItem(j, "uses"), s_app.uses, &s_app.n_uses, "uses", err, err_len)) {
        ok = true;
        for (int i = 0; ok && i < s_app.n_requires; i++) {
            if (!feature_present(s_app.requires[i])) {
                snprintf(err, err_len, "needs %s, which this gadget doesn't have", s_app.requires[i]);
                ok = false;
            }
        }
        if (ok) {
            snprintf(s_app.name, sizeof(s_app.name), "%s", name->valuestring);
            s_app.has_manifest = true;
        }
    }
    cJSON_Delete(j);
    return ok;
}

/* ---- app.run ----------------------------------------------------------------------------------------- */

static void name_from_path(const char *path)
{
    char tmp[PATH_MAX_LEN];
    snprintf(tmp, sizeof(tmp), "%s", path);
    size_t n = strlen(tmp);
    while (n && tmp[n - 1] == '/') {
        tmp[--n] = '\0';
    }
    char *base = strrchr(tmp, '/');
    base = base ? base + 1 : tmp;
    char *dot = strstr(base, ".wasm");
    if (dot && !strcmp(dot, ".wasm")) {
        *dot = '\0';
    }
    if (!strcmp(base, "app") && base != tmp) {   /* <name>/app.wasm: the folder's name */
        base[-1] = '\0';
        char *up = strrchr(tmp, '/');
        base = up ? up + 1 : tmp;
    }
    snprintf(s_app.name, sizeof(s_app.name), "%.*s", NAME_LEN - 1, base);
}

static bool ends_with(const char *s, const char *tail)
{
    size_t n = strlen(s), k = strlen(tail);
    return n >= k && !strcmp(s + n - k, tail);
}

/* A run that didn't happen: say why where it was asked, and put back the
 * launcher if it asked, else the face if the view had the screen. */
static void run_failed(const req_t *r, bool covered, const char *code, const char *message)
{
    muse_apps_reply(&r->o, muse_apps_error(code, message));
    if (r->o.from == MUSE_APPS_FROM_LAUNCHER) {
        char note[80];
        snprintf(note, sizeof(note), "%s won't open", r->name[0] ? r->name : "That app");
        show_launcher(note);
    } else if (covered) {
        muse_app_view_close();
    }
}

static void run(req_t *r)
{
    bool covered = s_app.running || s_launcher;   /* the view has the screen already */
    stop(GADGET_STOP_REPLACED, NULL, true);
    hide_launcher(true);
    s_app.id[0] = s_app.title[0] = '\0';
    uint8_t *bytes = r->bytes;
    size_t len = r->len;
    r->bytes = NULL;
    char err[192] = "";
    int64_t t0 = esp_timer_get_time();
    if (r->kind == REQ_RUN_VM) {
        /* The face, not a covered screen, while it downloads: power and the
         * face's own controls work as usual meanwhile. */
        if (covered) {
            muse_app_view_close();
            covered = false;
        }
        /* A folder holds app.wasm; a path to a .wasm file runs that file. */
        if (ends_with(r->path, ".wasm")) {
            snprintf(s_app.source, sizeof(s_app.source), "%s", r->path);
        } else {
            snprintf(s_app.source, sizeof(s_app.source), "%.*s%sapp.wasm", PATH_MAX_LEN - 12, r->path,
                     ends_with(r->path, "/") ? "" : "/");
        }
        name_from_path(s_app.source);
        if (!muse_apps_vm_file_read(s_app.source, APP_MAX_BYTES, FETCH_TIMEOUT_MS, &bytes, &len, err, sizeof(err))) {
            run_failed(r, covered, "fetch_failed", err);
            return;
        }
    } else if (r->id[0]) {
        /* An installed app: by convention its folder on the VM is its id's,
         * where its relative file_read and ui_image_load paths point. */
        snprintf(s_app.name, sizeof(s_app.name), "%.*s", NAME_LEN - 1, r->id);
        snprintf(s_app.source, sizeof(s_app.source), "workspace/gadgets/apps/%s/app.wasm", r->id);
        snprintf(s_app.title, sizeof(s_app.title), "%s", r->name[0] ? r->name : r->id);
    } else {
        snprintf(s_app.name, sizeof(s_app.name), "console-app");
        snprintf(s_app.source, sizeof(s_app.source), "console");
    }
    int64_t t1 = esp_timer_get_time();

    size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    /* The download (or the console's 256 KB upload buffer) is freed once the
     * module is loaded, before the app can grow into its budget. */
    size_t freed_soon = bytes ? heap_caps_get_allocated_size(bytes) : 0;
    size_t budget = budget_now(freed_soon);
    if (budget < BUDGET_MIN_BYTES) {
        free(bytes);
        snprintf(err, sizeof(err), "not enough memory: %u KB free, an app needs at least %u KB plus %u KB to spare",
                 (unsigned)((free_psram + freed_soon) / 1024), BUDGET_MIN_BYTES / 1024, RESERVE_BYTES / 1024);
        run_failed(r, covered, "out_of_memory", err);
        return;
    }
    if (!check_manifest(bytes, len, err, sizeof(err))) {
        free(bytes);
        run_failed(r, covered, "refused", err);
        return;
    }

    memset(s_app.timers, 0, sizeof(s_app.timers));
    taskENTER_CRITICAL(&s_mux);
    s_mem = (typeof(s_mem)){.counting = true};
    s_budget = budget;
    taskEXIT_CRITICAL(&s_mux);
    LoadArgs args = {0};
    args.name = s_app.name;
    args.wasm_binary_freeable = true;  /* the loader keeps copies: the download can go */
    s_app.module = wasm_runtime_load_ex(bytes, (uint32_t)len, &args, err, sizeof(err));
    const char *itself = s_app.module ? runs_itself(bytes, len) : NULL;
    free(bytes);
    if (!s_app.module) {
        unload();
        run_failed(r, covered, "invalid_app", err);
        return;
    }
    if (itself) {
        snprintf(err, sizeof(err), "the app has %s, which would run before its limits: build it as a reactor", itself);
        unload();   /* after: the name is the module's */
        run_failed(r, covered, "invalid_app", err);
        return;
    }
    int32_t imports = wasm_runtime_get_import_count(s_app.module);
    if (!imports_allowed(err, sizeof(err))) {
        unload();
        run_failed(r, covered, "invalid_app", err);
        return;
    }
    InstantiationArgs inst_args = {
        .default_stack_size = WASM_STACK_BYTES,
        .host_managed_heap_size = 0,
        .max_memory_pages = budget / 65536,
    };
    s_app.inst = wasm_runtime_instantiate_ex(s_app.module, &inst_args, err, sizeof(err));
    if (s_app.inst) {
        s_app.env = wasm_runtime_create_exec_env(s_app.inst, WASM_STACK_BYTES);
    }
    if (!s_app.inst || !s_app.env) {
        unload();
        run_failed(r, covered, "out_of_memory", s_app.inst ? "no memory for the app's stack" : err);
        return;
    }
    wasm_function_inst_t on_start = wasm_runtime_lookup_function(s_app.inst, "on_start");
    s_app.on_frame = wasm_runtime_lookup_function(s_app.inst, "on_frame");
    s_app.on_event = wasm_runtime_lookup_function(s_app.inst, "on_event");
    s_app.on_stop = wasm_runtime_lookup_function(s_app.inst, "on_stop");
    wasm_function_inst_t init = wasm_runtime_lookup_function(s_app.inst, "_initialize");
    if (!on_start) {
        unload();
        run_failed(r, covered, "invalid_app", "the app exports no on_start()");
        return;
    }
    if (!callback_is(on_start, 0) || !callback_is(init, 0) || !callback_is(s_app.on_frame, 1) ||
        !callback_is(s_app.on_event, 3) || !callback_is(s_app.on_stop, 1)) {
        unload();
        run_failed(r, covered, "invalid_app",
                   "a callback isn't as gadget.h declares it: void on_start(void), on_frame(int32_t), "
                   "on_event(int32_t, int32_t, int32_t), on_stop(int32_t)");
        return;
    }
    if (muse_voice_in_call()) {   /* its buttons hang up: an app mustn't take them */
        unload();
        run_failed(r, covered, "busy", "a call is on");
        return;
    }
#if CONFIG_MUSE_VIDEO
    if (muse_video_is_open()) {   /* the video view has the screen */
        unload();
        run_failed(r, covered, "busy", "the video view is open");
        return;
    }
#endif
    const muse_app_view_cbs_t cbs = {.input = on_view_input, .exit = on_view_exit};
    xQueueReset(s_inputs);
    if (!muse_app_view_open(&cbs)) {
        unload();
        run_failed(r, covered, "no_display", "the screen isn't available");
        return;
    }
    s_app.running = true;
    if (r->kind == REQ_RUN_BYTES) {
        snprintf(s_app.id, sizeof(s_app.id), "%s", r->id);
    }
    set_running_id(s_app.id);
    s_app.from_launcher = r->o.from == MUSE_APPS_FROM_LAUNCHER;
    s_app.started_us = esp_timer_get_time();
    s_app.frames = s_app.events = 0;
    s_app.frame_rate = s_app.on_frame ? 30 : 0;
    s_app.next_frame_us = s_app.started_us;
    s_app.mode = s_app.dark = s_app.online = -1;   /* told on its first turn */
    s_app.keep_awake = s_app.exit_asked = false;
    if (!s_app.has_manifest) {
        muse_apps_log("no manifest: no permissions, no devices");
    }
    muse_apps_ui_start();
    muse_apps_chat_start();
    muse_apps_dev_start();
    /* A WASI reactor's _initialize (static constructors) first, then on_start:
     * one budget between them. */
    wasm_runtime_set_instruction_count_limit(s_app.env, START_STEP_LIMIT);
    bool inited = !init || wasm_runtime_call_wasm(s_app.env, init, 0, NULL);
    bool started = inited && wasm_runtime_call_wasm(s_app.env, on_start, 0, NULL);
    ui_release();
    if (!started) {
        const char *what = wasm_runtime_get_exception(s_app.inst);
        snprintf(err, sizeof(err), "%s trapped: %s", inited ? "on_start" : "_initialize", what ? what : "?");
        bool back = r->o.from == MUSE_APPS_FROM_LAUNCHER;
        stop(STOP_TRAP, err, back);
        run_failed(r, false, "app_trapped", err);
        return;
    }
    int64_t t2 = esp_timer_get_time();
    muse_apps_log("running: %u bytes from %s, fetched in %d ms, loaded and started in %d ms, %d imports, %u KB"
                  " of a %u KB budget",
                  (unsigned)len, s_app.source, (int)((t1 - t0) / 1000), (int)((t2 - t1) / 1000), (int)imports,
                  (unsigned)(muse_apps_mem_used() / 1024), (unsigned)(budget / 1024));
    cJSON *payload;
    cJSON *result = ok_result(&payload);
    if (s_app.id[0]) {
        cJSON_AddStringToObject(payload, "id", s_app.id);
    }
    cJSON_AddStringToObject(payload, "app", s_app.name);
    cJSON_AddNumberToObject(payload, "bytes", (double)len);
    if (r->kind == REQ_RUN_VM) {
        cJSON_AddNumberToObject(payload, "fetch_ms", (double)((t1 - t0) / 1000));
    }
    cJSON_AddNumberToObject(payload, "load_ms", (double)((t2 - t1) / 1000));
    cJSON_AddNumberToObject(payload, "memory_kb", (double)(muse_apps_mem_used() / 1024));
    muse_apps_reply(&r->o, result);
}

/* Writes the console's upload to the VM, where app.run can read it. */
static void put(req_t *r)
{
    char err[160];
    int64_t t0 = esp_timer_get_time();
    if (!muse_apps_vm_file_write(r->path, r->bytes, r->len, FETCH_TIMEOUT_MS, err, sizeof(err))) {
        muse_apps_reply(&r->o, muse_apps_error("write_failed", err));
        return;
    }
    cJSON *payload;
    cJSON *result = ok_result(&payload);
    cJSON_AddStringToObject(payload, "path", r->path);
    cJSON_AddNumberToObject(payload, "bytes", (double)r->len);
    cJSON_AddNumberToObject(payload, "write_ms", (double)((esp_timer_get_time() - t0) / 1000));
    muse_apps_reply(&r->o, result);
}

static void handle_stop(const req_t *r)
{
    if (r->removed) {   /* app.remove, or unpairing: only if it's that installed app */
        if (!s_app.running || !s_app.id[0] || (r->id[0] && strcmp(r->id, s_app.id))) {
            return;
        }
        bool back = s_app.from_launcher;
        stop(GADGET_STOP_USER, "removed", back);
        if (back) {
            show_launcher(NULL);
        }
        return;
    }
    if (r->o.from == MUSE_APPS_FROM_VIEW) {
        if (r->reason == GADGET_STOP_USER) {   /* the power key: an app goes back where it came from */
            if (s_app.running) {
                bool back = s_app.from_launcher;
                stop(GADGET_STOP_USER, NULL, back);
                if (back) {
                    show_launcher(NULL);
                }
            } else {
                hide_launcher(false);
            }
        } else {   /* a call or the video view: everything goes */
            stop(r->reason, NULL, false);
            hide_launcher(false);
        }
        return;
    }
    bool was = s_app.running;   /* app.stop: back to the face */
    stop(GADGET_STOP_USER, NULL, false);
    hide_launcher(false);
    cJSON *payload;
    cJSON *result = ok_result(&payload);
    cJSON_AddBoolToObject(payload, "stopped", was);
    muse_apps_reply(&r->o, result);
}

static void handle(req_t *r)
{
    switch (r->kind) {
    case REQ_PUT:
        put(r);
        break;
    case REQ_STOP:
        handle_stop(r);
        break;
    case REQ_OPEN_FAILED:
        muse_apps_reply(&r->o, muse_apps_error(r->code, r->message));
        if (r->o.from == MUSE_APPS_FROM_LAUNCHER && s_launcher) {
            muse_apps_launcher_note(r->message);
        }
        break;
    case REQ_LAUNCHER:
        if (!s_app.running && !s_launcher) {
            show_launcher(NULL);
        }
        break;
    case REQ_RUN_VM:
    case REQ_RUN_BYTES:
        run(r);
        break;
    }
    free(r->bytes);
    r->bytes = NULL;
}

/* ---- The app task's loop --------------------------------------------------------------------------- */

static bool deliver(int type, int a, int b)
{
    if (!s_app.on_event || !s_app.running) {
        return true;
    }
    uint32_t argv[3] = {(uint32_t)type, (uint32_t)a, (uint32_t)b};
    s_app.events++;
    if (!call_app(s_app.on_event, 3, argv, STEP_LIMIT)) {
        crashed("on_event");
        return false;
    }
    return true;
}

/* Copies a finished operation's data into the app's buffer, then tells it. */
static bool deliver_op(op_t *op)
{
    if (op->data && op->cap && op->len && s_app.inst) {
        uint32_t n = op->len < op->cap ? (uint32_t)op->len : op->cap;
        if (wasm_runtime_validate_app_addr(s_app.inst, op->app_off, n)) {
            memcpy(wasm_runtime_addr_app_to_native(s_app.inst, op->app_off), op->data, n);
        }
    }
    free(op->data);
    op->data = NULL;
    return deliver(GADGET_EV_DONE, op->id, op->result);
}

/* State the app is told about when it changes: Muse's mode, the screen, the link. */
static bool deliver_changes(void)
{
    int mode = muse_apps_muse_mode();
    if (mode != s_app.mode) {
        s_app.mode = mode;
        if (!deliver(GADGET_EV_MUSE, mode, 0)) {
            return false;
        }
    }
    int dark = muse_ui_dark();
    if (dark != s_app.dark) {
        bool first = s_app.dark < 0;
        s_app.dark = dark;
        if (!first && !deliver(GADGET_EV_SCREEN, !dark, 0)) {
            return false;
        }
    }
    int online = muse_link_state() == MUSE_LINK_ONLINE;
    if (online != s_app.online) {
        bool first = s_app.online < 0;
        s_app.online = online;
        if (!first && !deliver(GADGET_EV_CONNECTION, online, 0)) {
            return false;
        }
    }
    return true;
}

static int64_t next_deadline(int64_t now)
{
    int64_t next = now + POLL_MS * 1000LL;
    for (int i = 1; i <= MAX_TIMERS; i++) {
        if (s_app.timers[i].on && s_app.timers[i].due_us < next) {
            next = s_app.timers[i].due_us;
        }
    }
    if (s_app.on_frame && s_app.frame_rate > 0 && !s_app.dark && s_app.next_frame_us < next) {
        next = s_app.next_frame_us;
    }
    return next;
}

static void turn(void)
{
    /* Finished operations, then queued input, then what changed. */
    op_t op;
    /* At most as many as can be outstanding: an app that starts another
     * operation from each EV_DONE mustn't hold the turn. */
    for (int k = 0; k < MAX_OPS && s_app.running && take_done(&op); k++) {
        if (!deliver_op(&op)) {
            return;
        }
    }
    input_t in;
    /* At most a queue's worth a turn: input that keeps coming (a held touch)
     * mustn't keep stops, timers and frames waiting. */
    for (int k = 0; k < INPUT_QUEUE_LEN && s_app.running && xQueueReceive(s_inputs, &in, 0); k++) {
        if (in.type != EV_WAKE && !deliver(in.type, in.a, in.b)) {
            return;
        }
    }
    if (!s_app.running || !deliver_changes()) {
        return;
    }
    int64_t now = esp_timer_get_time();
    for (int i = 1; i <= MAX_TIMERS && s_app.running; i++) {
        app_timer_t *t = &s_app.timers[i];
        if (t->on && t->due_us <= now) {
            if (t->repeat) {
                t->due_us += t->period_us;
                if (t->due_us <= now) {
                    t->due_us = now + t->period_us;   /* fell behind: skip, don't burst */
                }
            } else {
                t->on = false;
            }
            if (!deliver(GADGET_EV_TIMER, i, 0)) {
                return;
            }
        }
    }
    if (s_app.running && s_app.on_frame && s_app.frame_rate > 0 && !s_app.dark && s_app.next_frame_us <= now) {
        uint32_t argv[1] = {(uint32_t)(now / 1000)};
        if (!call_app(s_app.on_frame, 1, argv, STEP_LIMIT)) {
            crashed("on_frame");
            return;
        }
        s_app.frames++;
        int rate = s_app.frame_rate;   /* the callback may have changed it, to 0 too */
        if (rate > 0) {
            s_app.next_frame_us += 1000000 / rate;
            if (s_app.next_frame_us < now) {
                s_app.next_frame_us = now + 1000000 / rate;
            }
        }
    }
    if (s_app.running && s_app.keep_awake && now - s_app.last_poke_us > 1000000) {
        s_app.last_poke_us = now;
        muse_state_poke();   /* as a touch would */
    }
    if (s_app.running && s_app.exit_asked) {
        bool back = s_app.from_launcher;
        stop(GADGET_STOP_EXIT, NULL, back);
        if (back) {
            show_launcher(NULL);
        }
    }
}

/* The launcher's turn: its input, then a look at the list four times a second
 * while the screen is on. Returns how long until the next. */
#define LAUNCHER_FRAME_US 250000
static int64_t launcher_turn(int64_t now)
{
    input_t in;
    while (s_launcher && xQueueReceive(s_inputs, &in, 0)) {
        if (in.type != EV_WAKE) {
            muse_apps_launcher_event(in.type, in.a, in.b);
        }
    }
    if (!s_launcher) {
        return 0;
    }
    if (muse_ui_dark()) {
        s_launcher_next_us = now;
        return POLL_MS * 1000LL;
    }
    if (s_launcher_next_us <= now) {
        muse_apps_launcher_frame((uint32_t)(now / 1000));
        s_launcher_next_us += LAUNCHER_FRAME_US;
        if (s_launcher_next_us < now) {
            s_launcher_next_us = now + LAUNCHER_FRAME_US;
        }
    }
    return s_launcher_next_us - esp_timer_get_time();
}

static void *app_task(void *arg)
{
    (void)arg;
    RuntimeInitArgs init;
    memset(&init, 0, sizeof(init));
    init.mem_alloc_type = Alloc_With_Allocator;
    init.mem_alloc_option.allocator.malloc_func = (void *)mem_alloc;
    init.mem_alloc_option.allocator.realloc_func = (void *)mem_realloc;
    init.mem_alloc_option.allocator.free_func = (void *)mem_free;
    /* WAMR sorts the import table it's given and keeps it, so it can't stay in
     * flash: a copy in PSRAM rather than writable internal RAM. */
#define GADGET_FN(name, sig, perm) {#name, (void *)gadget_##name, sig, NULL},
    static const NativeSymbol natives[] = {MUSE_APPS_FUNCTIONS(GADGET_FN)};
#undef GADGET_FN
    size_t count = sizeof(natives) / sizeof(natives[0]);
    NativeSymbol *copy = heap_caps_malloc(sizeof(natives), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (copy) {
        memcpy(copy, natives, sizeof(natives));
    }
    if (!copy || !wasm_runtime_full_init(&init) || !wasm_runtime_register_natives("gadget", copy, (uint32_t)count)) {
        ESP_LOGE(TAG, "WAMR didn't start: no apps");
        for (req_t r;;) {
            if (xQueueReceive(s_reqs, &r, portMAX_DELAY)) {
                free(r.bytes);
                muse_apps_reply(&r.o, muse_apps_error("unavailable", "the WebAssembly runtime didn't start"));
            }
        }
    }
    ESP_LOGI(TAG, "ready for apps: API %d.%d, %u imports", MUSE_APPS_API_MAJOR, MUSE_APPS_API_MINOR, (unsigned)count);

    for (;;) {
        req_t r;
        int pending = atomic_exchange(&s_view_stop, 0);
        if (pending) {
            req_t stop = {.kind = REQ_STOP, .o = {.from = MUSE_APPS_FROM_VIEW}, .reason = pending - 1};
            handle(&stop);
        }
        if (!s_app.running && !s_launcher) {
            if (xQueueReceive(s_reqs, &r, portMAX_DELAY)) {
                handle(&r);
            }
            continue;
        }
        while ((s_app.running || s_launcher) && xQueueReceive(s_reqs, &r, 0)) {
            handle(&r);
        }
        int64_t now = esp_timer_get_time();
        int64_t wait_us;
        if (s_app.running) {
            turn();
            if (!s_app.running) {
                continue;
            }
            now = esp_timer_get_time();
            wait_us = next_deadline(now) - now;
        } else if (s_launcher) {
            wait_us = launcher_turn(now);
        } else {
            continue;
        }
        input_t in;
        TickType_t ticks = wait_us <= 0 ? 0 : pdMS_TO_TICKS((wait_us + 999) / 1000);
        if (xQueuePeek(s_inputs, &in, ticks ? ticks : 1) == pdTRUE && in.type == EV_WAKE) {
            xQueueReceive(s_inputs, &in, 0);
        }
    }
    return NULL;
}

static bool post_req(const req_t *r);

/* Apps in Settings (muse_settings_ui.c, through muse_app_view.h). */
static void open_launcher(void)
{
    req_t r = {.kind = REQ_LAUNCHER, .o = {.from = MUSE_APPS_FROM_LAUNCHER}};
    post_req(&r);
}

void muse_apps_start(muse_apps_result_fn send_result)
{
    s_send_result = send_result;
    /* In PSRAM: internal RAM is what Wi-Fi, Noise and the panel's DMA run short
     * of. No interrupt handler uses these queues, so they needn't be internal. */
    s_reqs = xQueueCreateWithCaps(4, sizeof(req_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_inputs = xQueueCreateWithCaps(INPUT_QUEUE_LEN, sizeof(input_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_log = heap_caps_malloc(LOG_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_state = heap_caps_calloc(1, sizeof(*s_state), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_reqs || !s_inputs || !s_log || !s_state) {
        ESP_LOGE(TAG, "no memory for the app queues");
        /* None of it, then: commands answer that apps didn't start. */
        if (s_reqs) vQueueDeleteWithCaps(s_reqs);
        if (s_inputs) vQueueDeleteWithCaps(s_inputs);
        heap_caps_free(s_log);
        heap_caps_free(s_state);
        s_reqs = s_inputs = NULL;
        s_log = NULL;
        s_state = NULL;
        return;
    }
    muse_apps_store_start();
    /* A pthread: WAMR's ESP-IDF port asks pthread_self() for the thread that runs wasm. */
    esp_pthread_cfg_t cfg = esp_pthread_get_default_config();
    cfg.stack_size = TASK_STACK_BYTES;
    cfg.prio = TASK_PRIORITY;
    cfg.thread_name = "muse_apps";
    cfg.stack_alloc_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    esp_pthread_set_cfg(&cfg);
    pthread_t t;
    if (pthread_create(&t, NULL, app_task, NULL) == 0) {
        pthread_detach(t);
        muse_app_view_set_home(open_launcher);
    } else {
        ESP_LOGE(TAG, "couldn't start the app task");
        vQueueDeleteWithCaps(s_reqs);   /* nobody would answer: requests are refused instead */
        s_reqs = NULL;
    }
    cfg = esp_pthread_get_default_config();
    esp_pthread_set_cfg(&cfg);
}

/* ---- Muse's commands --------------------------------------------------------------------------------- */

static cJSON *status_payload(void)
{
    cJSON *payload;
    cJSON *result = ok_result(&payload);
    bool running = s_app.running;
    cJSON_AddStringToObject(payload, "state", running ? "running" : s_launcher ? "launcher" : "idle");
    if (running) {
        if (s_app.id[0]) {
            cJSON_AddStringToObject(payload, "id", s_app.id);
        }
        cJSON_AddStringToObject(payload, "app", s_app.name);
        cJSON_AddStringToObject(payload, "source", s_app.source);
        cJSON_AddNumberToObject(payload, "running_s", (double)((esp_timer_get_time() - s_app.started_us) / 1000000));
        cJSON_AddNumberToObject(payload, "frames", s_app.frames);
        cJSON_AddNumberToObject(payload, "events", s_app.events);
        taskENTER_CRITICAL(&s_mux);
        size_t used = used_locked(), peak = s_mem.peak;
        taskEXIT_CRITICAL(&s_mux);
        cJSON_AddNumberToObject(payload, "memory_kb", (double)(used / 1024));
        cJSON_AddNumberToObject(payload, "peak_kb", (double)(peak / 1024));
    }
    cJSON_AddNumberToObject(payload, "budget_kb", (double)(muse_apps_mem_budget() / 1024));
    if (s_last.at_us) {
        cJSON *last = cJSON_AddObjectToObject(payload, "last_exit");
        cJSON_AddStringToObject(last, "app", s_last.app);
        cJSON_AddStringToObject(last, "reason", s_last.reason);
        if (s_last.message[0]) {
            cJSON_AddStringToObject(last, "message", s_last.message);
        }
        cJSON_AddNumberToObject(last, "ago_s", (double)((esp_timer_get_time() - s_last.at_us) / 1000000));
        cJSON_AddNumberToObject(last, "frames", s_last.frames);
        cJSON_AddNumberToObject(last, "peak_kb", (double)(s_last.peak / 1024));
    }
    return result;
}

/* The newest lines of the log that fit in about 3 KB, oldest first. */
static cJSON *logs_payload(void)
{
    char *copy = malloc(LOG_BYTES + 1);
    if (!copy) {
        return muse_apps_error("out_of_memory", "no memory to copy the log");
    }
    taskENTER_CRITICAL(&s_mux);
    size_t n = s_log_len;
    for (size_t i = 0; i < n; i++) {
        copy[i] = s_log[(s_log_head + i) % LOG_BYTES];
    }
    taskEXIT_CRITICAL(&s_mux);
    copy[n] = '\0';
    cJSON *payload;
    cJSON *result = ok_result(&payload);
    cJSON *lines = cJSON_AddArrayToObject(payload, "lines");
    /* Lines are NUL-separated; the first may be cut. Walk back from the end. */
    size_t budget = 3000, end = n;
    int count = 0;
    const char *starts[64];
    while (end > 0 && count < 64) {
        size_t start = end - 1;
        while (start > 0 && copy[start - 1] != '\0') {
            start--;
        }
        size_t len = strlen(copy + start);
        if (len + 8 > budget || (start == 0 && s_log_len == LOG_BYTES)) {
            break;
        }
        budget -= len + 8;
        starts[count++] = copy + start;
        end = start ? start - 1 : 0;
        if (start == 0) {
            break;
        }
    }
    for (int i = count - 1; i >= 0; i--) {
        if (*starts[i]) {
            cJSON_AddItemToArray(lines, cJSON_CreateString(starts[i]));
        }
    }
    free(copy);
    return result;
}

int muse_apps_safe_inset(void)
{
    int w, h;
    bool round;
    muse_app_view_size(&w, &h, &round);
    int side = w < h ? w : h;
    return round ? (int)(side * 0.1465f + 0.5f) : 0;   /* the largest centred square: (1 - 1/sqrt 2) / 2 */
}

static const char *const s_commands[] = {"app.install", "app.list",   "app.remove", "app.run",    "app.stop",
                                         "app.status",  "app.logs",   "app.api",   "files.put",  "files.list",
                                         "files.remove"};

bool muse_apps_handles(const char *command)
{
    for (size_t i = 0; i < sizeof(s_commands) / sizeof(s_commands[0]); i++) {
        if (!strcmp(command, s_commands[i])) {
            return true;
        }
    }
    return false;
}

static bool post_req(const req_t *r)
{
    if (!s_reqs || xQueueSend(s_reqs, r, 0) != pdTRUE) {
        return false;
    }
    wake();
    return true;
}

static const char *string_param(cJSON *params, const char *name)
{
    const char *s = cJSON_GetStringValue(cJSON_GetObjectItem(params, name));
    return s && *s ? s : NULL;
}

static cJSON *async_result(bool posted)
{
    if (!posted) {
        return muse_apps_error("busy", "the gadget is busy with apps: try again");
    }
    cJSON *async = cJSON_CreateObject();
    cJSON_AddBoolToObject(async, "_async", true);
    return async;
}

/* files.put {path, name?}, files.list, files.remove {name}: the gadget's own
 * files, on the store's task. */
static cJSON *files_command(const char *what, cJSON *params, const muse_apps_origin_t *o, const char *path)
{
    if (!strcmp(what, "list")) {
        return async_result(muse_apps_store_file_list(o));
    }
    const char *name = string_param(params, "name");
    if (!strcmp(what, "put")) {
        if (!path) {
            return muse_apps_error("missing_param", "path is required");
        }
        if (!name) {   /* the file's own name */
            const char *slash = strrchr(path, '/');
            name = slash ? slash + 1 : path;
        }
    } else if (!name) {
        return muse_apps_error("missing_param", "name is required");
    }
    if (name[0] == '/') {
        name++;   /* as apps write it */
    }
    if (!muse_apps_flash_name_ok(name)) {
        return muse_apps_error("bad_name", "a name is folders and a file of A-Z, a-z, 0-9, '.', '-' and '_', "
                                           "joined by '/', under 96 characters");
    }
    if (!strcmp(what, "put")) {
        return async_result(muse_apps_store_file_put(o, path, name, string_param(params, "sha256")));
    }
    return async_result(muse_apps_store_file_remove(o, name));
}

/* A path to run or install from: on the VM, a .wasm file or an app's folder. */
static bool is_path(const char *s)
{
    return strchr(s, '/') || ends_with(s, ".wasm");
}

cJSON *muse_apps_command(const char *command, cJSON *params, const char *request_id, uint64_t session_generation)
{
    if (!muse_settings_apps_on()) {
        return muse_apps_error("apps_off", "apps are off on this gadget: the person can turn on Run apps in Settings");
    }
    if (!s_state) {
        return muse_apps_error("unavailable", "apps didn't start");
    }
    /* Answered at once, from what the app task and the store keep. */
    if (!strcmp(command, "app.status")) {
        return status_payload();
    }
    if (!strcmp(command, "app.logs")) {
        return logs_payload();
    }
    if (!strcmp(command, "app.api")) {
        return muse_apps_api_command(params);
    }
    if (!strcmp(command, "app.list")) {
        return muse_apps_store_list_result();
    }
    muse_apps_origin_t o = {.from = MUSE_APPS_FROM_MUSE, .session_generation = session_generation};
    snprintf(o.request_id, sizeof(o.request_id), "%s", request_id);
    const char *id = string_param(params, "id"), *path = string_param(params, "path");
    if (id && strlen(id) >= MUSE_APPS_ID_MAX) {
        return muse_apps_error("bad_id", "an id is 1 to 31 of a-z, 0-9, - and _");
    }
    if (path && strlen(path) >= PATH_MAX_LEN - 12) {
        return muse_apps_error("invalid_param", "path is too long");
    }
    if (!strcmp(command, "app.stop")) {
        req_t r = {.kind = REQ_STOP, .o = o};
        return async_result(post_req(&r));
    }
    if (!strncmp(command, "files.", 6)) {
        return files_command(command + 6, params, &o, path);
    }
    if (!strcmp(command, "app.install")) {
        if (!path) {
            return muse_apps_error("missing_param", "path is required");
        }
        return async_result(muse_apps_store_install(&o, id, string_param(params, "name"), string_param(params, "icon"),
                                                    string_param(params, "color"), path,
                                                    string_param(params, "sha256")));
    }
    if (!strcmp(command, "app.remove")) {
        if (!id) {
            return muse_apps_error("missing_param", "id is required");
        }
        return async_result(muse_apps_store_remove(&o, id));
    }
    /* app.run: an installed app by id, or an app straight from the VM by path. */
    if (id) {
        return async_result(muse_apps_store_open(&o, id));
    }
    if (!path) {
        return muse_apps_error("missing_param", "id (an installed app) or path (on the VM) is required");
    }
    req_t r = {.kind = REQ_RUN_VM, .o = o};
    snprintf(r.path, sizeof(r.path), "%s", path);
    return async_result(post_req(&r));
}

/* ---- From the store's task (muse_apps_store.c) ---------------------------------------------------- */

void muse_apps_forget_all(void)
{
    muse_apps_store_forget_all();
}

void muse_apps_vm_changed(void)
{
    if (s_state) {
        muse_apps_vm_close_all();   /* its queued requests are dropped */
        on_view_exit(GADGET_STOP_USER);
    }
}

void muse_apps_run_installed(const muse_apps_origin_t *o, const char *id, const char *name, uint8_t *bytes,
                             size_t len)
{
    req_t r = {.kind = REQ_RUN_BYTES, .o = *o, .bytes = bytes, .len = len};
    snprintf(r.id, sizeof(r.id), "%s", id);
    snprintf(r.name, sizeof(r.name), "%s", name);
    if (!post_req(&r)) {
        free(bytes);
        muse_apps_reply(o, muse_apps_error("busy", "the app task is busy"));
    }
}

void muse_apps_open_failed(const muse_apps_origin_t *o, const char *code, const char *message)
{
    req_t r = {.kind = REQ_OPEN_FAILED, .o = *o};
    snprintf(r.code, sizeof(r.code), "%s", code);
    snprintf(r.message, sizeof(r.message), "%s", message);
    if (!post_req(&r)) {
        muse_apps_reply(o, muse_apps_error(code, message));
    }
}

void muse_apps_stop_installed(const char *id)
{
    req_t r = {.kind = REQ_STOP, .o = {.from = MUSE_APPS_FROM_VIEW}, .removed = true};
    snprintf(r.id, sizeof(r.id), "%s", id ? id : "");
    if (s_reqs && xQueueSend(s_reqs, &r, pdMS_TO_TICKS(1000)) == pdTRUE) {
        wake();
    }
}

/* ---- The console (the serial task) ------------------------------------------------------------------ */

static int b64_value(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

/* Appends base64 text to the upload; false if it isn't base64 or won't fit. */
static bool stage_b64(const char *text)
{
    uint32_t acc = 0;
    int bits = 0;
    for (const char *p = text; *p && *p != '='; p++) {
        int v = b64_value(*p);
        if (v < 0) {
            return false;
        }
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (s_stage_len >= APP_MAX_BYTES) {
                return false;
            }
            s_stage[s_stage_len++] = (uint8_t)(acc >> bits);
        }
    }
    return true;
}

/* "timer:clock:orange" is app timer with the clock glyph on orange; either may be empty. */
static void split_look(char *id, const char **icon, const char **color)
{
    char *c = strchr(id, ':');
    *icon = *color = NULL;
    if (!c) {
        return;
    }
    *c = '\0';
    *icon = c + 1;
    if ((c = strchr(c + 1, ':'))) {
        *c = '\0';
        *color = c + 1;
    }
}

static void console_say(const char *json)
{
    printf("@app %s\n", json);
    fflush(stdout);
}

static void console_reply(cJSON *result)
{
    char *text = cJSON_PrintUnformatted(result);
    console_say(text ? text : "{\"ok\":false}");
    free(text);
    cJSON_Delete(result);
}

bool muse_apps_console(const char *line)
{
    if (!s_state) {
        return false;
    }
    if (!muse_settings_apps_on() && (!strncmp(line, "app", 3) || !strncmp(line, "files.", 6))) {
        console_say("{\"ok\":false,\"error\":{\"code\":\"apps_off\",\"message\":\"turn on Run apps in Settings\"}}");
        return true;
    }
    if (!strcmp(line, "app.begin")) {
        if (!s_stage) {
            s_stage = heap_caps_malloc(APP_MAX_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        }
        s_stage_len = 0;
        console_say(s_stage ? "{\"ok\":true,\"bytes\":0}" : "{\"ok\":false,\"error\":{\"code\":\"out_of_memory\"}}");
        return true;
    }
    bool more = !strncmp(line, "app+=", 5);
    if (more || !strncmp(line, "app=", 4)) {
        if (!s_stage) {
            console_say("{\"ok\":false,\"error\":{\"code\":\"no_upload\",\"message\":\"send app.begin first\"}}");
            return true;
        }
        if (!stage_b64(line + (more ? 5 : 4))) {
            free(s_stage);   /* start again with app.begin */
            s_stage = NULL;
            s_stage_len = 0;
            console_say("{\"ok\":false,\"error\":{\"code\":\"bad_piece\",\"message\":\"not base64, or too large\"}}");
            return true;
        }
        if (more) {
            printf("@app {\"ok\":true,\"bytes\":%u}\n", (unsigned)s_stage_len);
            fflush(stdout);
            return true;
        }
        req_t r = {.kind = REQ_RUN_BYTES, .o = {.from = MUSE_APPS_FROM_CONSOLE}, .bytes = s_stage, .len = s_stage_len};
        s_stage = NULL;
        s_stage_len = 0;
        if (!post_req(&r)) {
            free(r.bytes);
            console_say("{\"ok\":false,\"error\":{\"code\":\"busy\"}}");
        }
        return true;
    }
    if (!strncmp(line, "app.put=", 8)) {
        if (!s_stage || !s_stage_len) {
            console_say("{\"ok\":false,\"error\":{\"code\":\"no_upload\",\"message\":\"send app.begin and app+= first\"}}");
            return true;
        }
        req_t r = {.kind = REQ_PUT, .o = {.from = MUSE_APPS_FROM_CONSOLE}, .bytes = s_stage, .len = s_stage_len};
        if (strlen(line + 8) >= sizeof(r.path)) {   /* cut short, it would write another file */
            console_say("{\"ok\":false,\"error\":{\"code\":\"bad_path\",\"message\":\"the path is too long\"}}");
            return true;
        }
        snprintf(r.path, sizeof(r.path), "%s", line + 8);
        s_stage = NULL;
        s_stage_len = 0;
        if (!post_req(&r)) {
            free(r.bytes);
            console_say("{\"ok\":false,\"error\":{\"code\":\"busy\"}}");
        }
        return true;
    }
    const muse_apps_origin_t console = {.from = MUSE_APPS_FROM_CONSOLE};
    if (!strncmp(line, "app.install.upload=", 19)) {   /* "app.install.upload=[ID[:ICON:COLOR] [NAME]]": the upload, installed */
        char id[64] = "";
        sscanf(line + 19, "%63s", id);
        const char *name = strchr(line + 19, ' '), *icon, *color;
        split_look(id, &icon, &color);
        if (!strcmp(id, "-")) {   /* the manifest's name */
            id[0] = '\0';
        }
        if (!s_stage || !s_stage_len) {
            console_say("{\"ok\":false,\"error\":{\"code\":\"no_upload\",\"message\":\"app.begin, app+= pieces, then app.install.upload=[ID] [NAME]\"}}");
            return true;
        }
        if (muse_apps_store_install_bytes(&console, id, name ? name + 1 : NULL, icon, color, s_stage, s_stage_len)) {
            s_stage = NULL;
            s_stage_len = 0;
        } else {
            console_say("{\"ok\":false,\"error\":{\"code\":\"busy\"}}");
        }
        return true;
    }
    if (!strncmp(line, "app.install=", 12)) {   /* "app.install=PATH" or "app.install=ID[:ICON:COLOR] PATH [NAME]", ID "-" for the manifest's */
        char a[64] = "", path[PATH_MAX_LEN] = "";
        const char *name = NULL, *icon = NULL, *color = NULL;
        /* Words split at blanks, a tab too; one too long for its buffer is a bad line, not two words. */
        const char *w1 = line + 12 + strspn(line + 12, " \t");
        const char *w2 = w1 + strcspn(w1, " \t");
        size_t n1 = (size_t)(w2 - w1);
        w2 += strspn(w2, " \t");
        size_t n2 = strcspn(w2, " \t");
        int n = !n1 || n1 >= sizeof(a) || n2 > 179 ? 0 : sscanf(line + 12, "%63s %179s", a, path);
        if (n == 1 && is_path(a)) {   /* just a path: the manifest says the rest */
            snprintf(path, sizeof(path), "%s", a);
            a[0] = '\0';
        } else if (n == 2) {
            name = w2 + n2;
            name += strspn(name, " \t");
            if (!*name) {
                name = NULL;
            }
            split_look(a, &icon, &color);
            if (!strcmp(a, "-")) {   /* the manifest's name */
                a[0] = '\0';
            }
        }
        if (!path[0] || !muse_apps_store_install(&console, a, name, icon, color, path, NULL)) {
            console_say("{\"ok\":false,\"error\":{\"code\":\"bad_line\",\"message\":\"app.install=PATH, or app.install=ID PATH [NAME]\"}}");
        }
        return true;
    }
    if (!strncmp(line, "app.run=", 8)) {   /* an installed app's id, or a path on the VM */
        bool posted;
        if (is_path(line + 8)) {
            req_t r = {.kind = REQ_RUN_VM, .o = console};
            if (strlen(line + 8) >= sizeof(r.path) - 12) {   /* cut short, it would run another app */
                console_say("{\"ok\":false,\"error\":{\"code\":\"bad_path\",\"message\":\"the path is too long\"}}");
                return true;
            }
            snprintf(r.path, sizeof(r.path) - 12, "%s", line + 8);
            posted = post_req(&r);
        } else {
            posted = muse_apps_store_open(&console, line + 8);
        }
        if (!posted) {
            console_say("{\"ok\":false,\"error\":{\"code\":\"busy\"}}");
        }
        return true;
    }
    if (!strncmp(line, "app.remove=", 11)) {
        if (!muse_apps_store_remove(&console, line + 11)) {
            console_say("{\"ok\":false,\"error\":{\"code\":\"busy\"}}");
        }
        return true;
    }
    if (!strcmp(line, "app.list")) {
        console_reply(muse_apps_store_list_result());
        return true;
    }
    if (!strncmp(line, "files.upload=", 13)) {   /* "files.upload=NAME": the upload, as a file on the gadget */
        const char *name = line + 13 + (line[13] == '/');
        if (!s_stage || !muse_apps_flash_name_ok(name)) {
            console_say("{\"ok\":false,\"error\":{\"code\":\"bad_line\",\"message\":\"app.begin, app+= pieces, then files.upload=NAME\"}}");
            return true;
        }
        if (muse_apps_store_file_put_bytes(&console, name, s_stage, s_stage_len)) {
            s_stage = NULL;
            s_stage_len = 0;
        } else {
            console_say("{\"ok\":false,\"error\":{\"code\":\"busy\"}}");
        }
        return true;
    }
    if (!strncmp(line, "files.put=", 10) || !strncmp(line, "files.remove=", 13) || !strcmp(line, "files.list")) {
        /* "files.put=PATH [NAME]", "files.remove=NAME", "files.list": as the commands */
        cJSON *params = cJSON_CreateObject();
        char path[PATH_MAX_LEN] = "", name[MUSE_APPS_FLASH_NAME_MAX + 1] = "";
        const char *what = line + 6;
        if (!strncmp(line, "files.put=", 10)) {
            /* A word too long for its buffer is refused, not cut into another path or name. */
            const char *w = line + 10 + strspn(line + 10, " ");
            size_t n1 = strcspn(w, " "), n2 = strcspn(w + n1 + strspn(w + n1, " "), " ");
            if (n1 > 179 || n2 > 96) {
                cJSON_Delete(params);
                console_say("{\"ok\":false,\"error\":{\"code\":\"bad_line\",\"message\":\"the path or name is too long\"}}");
                return true;
            }
            sscanf(line + 10, "%179s %96s", path, name);
            what = "put";
        } else if (!strncmp(line, "files.remove=", 13)) {
            if (strlen(line + 13) >= sizeof(name)) {   /* refused, not cut into another file's name */
                cJSON_Delete(params);
                console_say("{\"ok\":false,\"error\":{\"code\":\"bad_line\",\"message\":\"the name is too long\"}}");
                return true;
            }
            snprintf(name, sizeof(name), "%s", line + 13);
            what = "remove";
        }
        if (name[0]) {
            cJSON_AddStringToObject(params, "name", name);
        }
        cJSON *answer = files_command(what, params, &console, path[0] ? path : NULL);
        cJSON_Delete(params);
        if (cJSON_IsTrue(cJSON_GetObjectItem(answer, "_async"))) {
            cJSON_Delete(answer);   /* the store answers */
        } else {
            console_reply(answer);
        }
        return true;
    }
    if (!strcmp(line, "app.launcher")) {   /* bench: as Apps in Settings */
        open_launcher();
        console_say("{\"ok\":true}");
        return true;
    }
    if (!strncmp(line, "app.press=", 10)) {   /* bench: "app.press=talk|second,down|up", as the board's button */
        char role[8] = "", edge[8] = "";
        bool second = false;
        unsigned bits = 0;
        if (sscanf(line + 10, "%7[a-z],%7s", role, edge) == 2 &&
            (!strcmp(role, "talk") || (second = !strcmp(role, "second")))) {
            bool down = !strcmp(edge, "down");
            if (!second) {
                bits = down ? MUSE_BTN_TALK_PRESS : MUSE_BTN_TALK_RELEASE;
            } else if (muse_board && muse_board->speaker_button) {
                bits = down ? MUSE_BTN_SPEAKER_PRESS : MUSE_BTN_SPEAKER_RELEASE;
            } else {
                bits = down ? MUSE_BTN_AUX_PRESS : MUSE_BTN_AUX_RELEASE;
            }
        }
        if (bits) {
            muse_app_view_buttons(bits);   /* what reaches no app is dropped */
        }
        console_say(bits ? "{\"ok\":true}" : "{\"ok\":false,\"error\":{\"code\":\"bad_press\"}}");
        return true;
    }
    if (!strcmp(line, "app.key")) {   /* bench: the power key, short */
        on_view_exit(GADGET_STOP_USER);
        console_say("{\"ok\":true}");
        return true;
    }
    if (!strncmp(line, "app.event=", 10)) {   /* bench: "app.event=TYPE,A,B" as if it happened */
        int type = 0, a = 0, b = 0;
        if (sscanf(line + 10, "%d,%d,%d", &type, &a, &b) < 1 || !muse_apps_post(type, a, b)) {
            console_say("{\"ok\":false,\"error\":{\"code\":\"bad_event\"}}");
        } else {
            console_say("{\"ok\":true}");
        }
        return true;
    }
    if (!strcmp(line, "app.stop")) {
        req_t r = {.kind = REQ_STOP, .o = {.from = MUSE_APPS_FROM_CONSOLE}};
        if (!post_req(&r)) {
            console_say("{\"ok\":false,\"error\":{\"code\":\"busy\"}}");
        }
        return true;
    }
    if (!strcmp(line, "app.status")) {
        console_reply(status_payload());
        return true;
    }
    if (!strcmp(line, "app.logs")) {
        console_reply(logs_payload());
        return true;
    }
    static const char prefix[] = "app.api";
    size_t prefix_len = sizeof(prefix) - 1;
    if (!strcmp(line, prefix) || (!strncmp(line, prefix, prefix_len) && line[prefix_len] == '=')) {
        cJSON *params = cJSON_CreateObject();
        if (!params) {
            console_reply(muse_apps_error("out_of_memory", "could not build the API request"));
            return true;
        }
        if (line[prefix_len] == '=') {
            const char *value = line + prefix_len + 1;
            char *end = NULL;
            long part = strtol(value, &end, 10);
            if (end == value || *end) {
                cJSON_Delete(params);
                console_reply(muse_apps_error("invalid_param", "part must be an integer"));
                return true;
            }
            if (!cJSON_AddNumberToObject(params, "part", (double)part)) {
                cJSON_Delete(params);
                console_reply(muse_apps_error("out_of_memory", "could not build the API request"));
                return true;
            }
        }
        console_reply(muse_apps_api_command(params));
        cJSON_Delete(params);
        return true;
    }
    return false;
}

/* ---- link.register: kept short, it shares 8 KB with every command ---------------------------------- */

static void add_command(cJSON *commands, const char *name, const char *description, cJSON *required,
                        cJSON *optional, int timeout_ms)
{
    cJSON *command = cJSON_CreateObject();
    cJSON_AddStringToObject(command, "description", description);
    cJSON_AddItemToObject(command, "required", required ? required : cJSON_CreateObject());
    cJSON_AddItemToObject(command, "optional", optional ? optional : cJSON_CreateObject());
    if (timeout_ms) {
        cJSON_AddNumberToObject(command, "timeout_ms", timeout_ms);
    }
    cJSON_AddItemToObject(commands, name, command);
}

static cJSON *one_param(const char *name, const char *type, const char *description)
{
    cJSON *params = cJSON_CreateObject();
    cJSON *p = cJSON_AddObjectToObject(params, name);
    cJSON_AddStringToObject(p, "type", type);
    cJSON_AddStringToObject(p, "description", description);
    return params;
}

static void add_param(cJSON *params, const char *name, const char *type, const char *description)
{
    cJSON *p = cJSON_AddObjectToObject(params, name);
    cJSON_AddStringToObject(p, "type", type);
    cJSON_AddStringToObject(p, "description", description);
}

void muse_apps_advertise(cJSON *commands)
{
    if (!muse_settings_apps_on()) {
        return;   /* off in Settings: Muse isn't offered them */
    }
    cJSON *install = cJSON_CreateObject();
    add_param(install, "path", "string",
              "On this VM, relative to home: an app's folder holding app.wasm (workspace/gadgets/apps/clock), or "
              "a .wasm file.");
    cJSON *look = cJSON_CreateObject();
    add_param(look, "id", "string", "Its id here (a-z, 0-9, - and _); else its manifest's name.");
    add_param(look, "name", "string", "Its name in the launcher; else its manifest's title, or the id.");
    add_param(look, "sha256", "string", "The file's SHA-256 (build.py prints it): the gadget checks it.");
    char names[400], description[480];
    muse_apps_glyph_names(names, sizeof(names));
    snprintf(description, sizeof(description), "Its launcher glyph, else its manifest's: one of %s.", names);
    add_param(look, "icon", "string", description);
    muse_apps_color_names(names, sizeof(names));
    snprintf(description, sizeof(description), "Its colour, else its manifest's: one of %s.", names);
    add_param(look, "color", "string", description);
    add_command(commands, "app.install",
                "Install a WebAssembly app on this gadget from this VM, or update it (same id, new file). It stays in the launcher (Settings > Apps) until app.remove or the user "
                "removes it there. Doesn't run it.",
                install, look, 60000);
    add_command(commands, "app.list",
                "The installed apps (the user can remove them on the gadget too), which one runs, and the room left.",
                NULL, NULL, 0);
    add_command(commands, "app.remove", "Remove an installed app.",
                one_param("id", "string", "The installed app's id."), NULL, 10000);
    cJSON *run = cJSON_CreateObject();
    add_param(run, "id", "string", "An installed app's id.");
    add_param(run, "path", "string", "Or an app on this VM, as app.install takes it, run without installing.");
    add_command(commands, "app.run",
                "Run an app on the screen until app.stop, another app.run, the power key, a call or the video view. "
                "Replies once it runs, or why not; then use app.status and app.logs.",
                NULL, run, 60000);
    add_command(commands, "app.stop", "Close the running app, after its on_stop. Replies whether one ran.", NULL,
                NULL, 0);
    cJSON *put = cJSON_CreateObject();
    add_param(put, "path", "string", "The file on this VM, relative to home, up to 3 MB.");
    cJSON *put_opt = one_param("name", "string", "Its name there, folders allowed (photos/cat.jpg); else the file's own.");
    add_param(put_opt, "sha256", "string", "The file's SHA-256: the gadget checks it, and answers with its own.");
    add_command(commands, "files.put",
                "Copy a file from this VM to the gadget's own flash (4 MB in all), for apps to read offline: "
                "file_read and ui_image_load take it as \"/NAME\" (files.gadget permission). Replaces a file of "
                "the same name.",
                put, put_opt, 240000);
    add_command(commands, "files.list", "The gadget's own files (name, bytes) and the room left.", NULL, NULL, 10000);
    add_command(commands, "files.remove", "Remove one of the gadget's own files.",
                one_param("name", "string", "As files.list names it."), NULL, 10000);
    add_command(commands, "app.status",
                "The running app (name, memory, frames) and how the last one ended (reason, trap message).", NULL,
                NULL, 0);
    add_command(commands, "app.logs", "The app's recent log lines: its sys_log output and the runtime's notes.",
                NULL, NULL, 0);
    add_command(commands, "app.api",
                "A part of the machine-readable app API: part, parts and text. Join text in order. "
                "Use device.guide for this gadget's runtime capabilities.",
                one_param("part", "number", "Required integer 0 to parts - 1; request 0 first to learn parts."), NULL, 0);
}
