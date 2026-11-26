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

/* Bench UI timing on mg.perf: see muse_perf.h. */
#include "muse_perf.h"

#include <stdlib.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "muse_board.h"
#include "muse_ui.h"
#if CONFIG_LV_USE_PROFILER
#include "muse_lv_profiler.h"
#endif

static const char *TAG = "mg.perf";

#define TOUCH_GIVE_UP_US 1000000   /* a touch that redraws nothing */
#define TALK_GIVE_UP_US 3000000
#define CPU_TASKS_MAX 48
#define CPU_TOP 10

#define SWIPE_MS 250             /* a brisk flick */
#define GESTURE_TAIL_US 300000   /* the gesture ends this long after its last scroll or animation frame */
#define GESTURE_CAP_US 3000000   /* ... or this long after the release, whatever keeps animating */
#define GESTURE_WATCH_MS 20
#define GESTURE_FRAMES_MAX 200
#define SLOW_FRAME_US 33000
#define VERY_SLOW_FRAME_US 50000

static inline uint32_t now_us(void)
{
    return (uint32_t)esp_timer_get_time();   /* wraps after 71 min: only differences are used */
}

static inline int ms(uint32_t from, uint32_t to)
{
    return (int)((to - from + 500) / 1000);
}

/* ---- CPU share by task ---- */

#define CPU_STATS (CONFIG_FREERTOS_USE_TRACE_FACILITY && CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS)

#if CPU_STATS
/* Every task's run time at one moment. */
typedef struct {
    int n;
    TaskHandle_t task[CPU_TASKS_MAX];
    configRUN_TIME_COUNTER_TYPE runtime[CPU_TASKS_MAX];
    configRUN_TIME_COUNTER_TYPE total;
} cpu_snap_t;

typedef struct {
    const char *name;
    uint32_t permille;
} cpu_share_t;

static int by_share(const void *a, const void *b)
{
    return (int)((const cpu_share_t *)b)->permille - (int)((const cpu_share_t *)a)->permille;
}

/*
 * Writes each task's share of the core since *snap into line (busiest first,
 * at most CPU_TOP of them, at least 0.5%) and moves *snap to now. With no
 * earlier snapshot, line is left empty. False if out of memory.
 */
static bool cpu_since(cpu_snap_t *snap, char *line, size_t cap)
{
    line[0] = '\0';
    UBaseType_t n = uxTaskGetNumberOfTasks() + 4;
    if (n > CPU_TASKS_MAX) {
        n = CPU_TASKS_MAX;
    }
    TaskStatus_t *st = malloc(n * sizeof(*st));
    cpu_share_t *share = malloc(n * sizeof(*share));
    if (!st || !share) {
        free(st);
        free(share);
        return false;
    }
    configRUN_TIME_COUNTER_TYPE total;
    n = uxTaskGetSystemState(st, n, &total);
    configRUN_TIME_COUNTER_TYPE elapsed = total - snap->total;

    int nshare = 0;
    for (UBaseType_t i = 0; i < n && snap->total; i++) {
        configRUN_TIME_COUNTER_TYPE before = 0;
        for (int j = 0; j < snap->n; j++) {
            if (snap->task[j] == st[i].xHandle) {
                before = snap->runtime[j];
                break;
            }
        }
        uint64_t d = st[i].ulRunTimeCounter - before;
        share[nshare].name = st[i].pcTaskName;
        share[nshare].permille = elapsed ? (uint32_t)(d * 1000 / elapsed) : 0;
        nshare++;
    }
    qsort(share, nshare, sizeof(*share), by_share);
    size_t len = 0;
    for (int i = 0; i < nshare && i < CPU_TOP && share[i].permille >= 5 && len < cap; i++) {
        len += snprintf(line + len, cap - len, " %s %u.%u%%", share[i].name, (unsigned)(share[i].permille / 10),
                        (unsigned)(share[i].permille % 10));
    }

    for (UBaseType_t i = 0; i < n; i++) {
        snap->task[i] = st[i].xHandle;
        snap->runtime[i] = st[i].ulRunTimeCounter;
    }
    snap->n = (int)n;
    snap->total = total;
    free(st);
    free(share);
    return true;
}

/* Each task's share of one core since the last call. */
static void log_cpu(void)
{
    static cpu_snap_t snap;
    char line[256];
    if (cpu_since(&snap, line, sizeof(line)) && line[0]) {
        ESP_LOGI(TAG, "cpu (%% of a core):%s", line);
    }
}
#else
static void log_cpu(void)
{
}
#endif

/* ---- one period's numbers, written on the LVGL task, read by the reporter ---- */

typedef struct {
    uint32_t refreshes;     /* LVGL's refresh timer ran */
    uint32_t frames;        /* ... and drew something */
    uint64_t frame_us, wait_us, flush_us;
    uint32_t frame_max_us;
    uint64_t px;
    uint32_t ticks;
    uint64_t tick_us;
    uint32_t tick_max_us;
} window_t;

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static window_t s_win;
static uint32_t s_screen_px;

/* The refresh under way (LVGL task only). */
static struct {
    uint32_t start, wait_from, flush_from;
    uint32_t wait_us, flush_us, px, flushes;
} s_frame;

/* ---- touch: interrupt -> read -> first flush -> frame done ---- */

static volatile uint32_t s_touch_irq;
static lv_indev_read_cb_t s_touch_read;
static bool s_touch_down;
static struct {
    uint32_t irq, read, flush;
    bool on;
} s_touch;

void IRAM_ATTR muse_perf_touch_irq(void)
{
    s_touch_irq = now_us() | 1;   /* never 0 */
}

/* ---- injected gestures and their stats (LVGL task, or under the display lock) ---- */

static lv_indev_t *s_indev;
static int32_t s_w, s_h;

static struct {
    bool on, started, at_end;
    int32_t x0, y0, x1, y1;
    uint32_t dur_us, start;
} s_inj;

typedef struct {
    uint32_t frame_us, wait_us, flush_us, px;
} frame_rec_t;

static struct {
    bool on;
    char label[40];
    char from[32];
    uint32_t start, released, last_active, last_flush, first_flush, max_gap;
    int nframes, dropped;
    frame_rec_t frames[GESTURE_FRAMES_MAX];
} s_gs;

static lv_timer_t *s_gs_watch;
#if CPU_STATS
static cpu_snap_t s_gs_cpu;
#endif

static void gesture_begin(uint32_t t)
{
#if CONFIG_LV_USE_PROFILER
    if (muse_lv_prof_enabled()) {
        muse_lv_prof_enable(true);   /* a fresh tally for this gesture */
    }
#endif
    s_gs.on = true;
    s_gs.start = s_gs.last_active = s_gs.last_flush = t;
    s_gs.released = s_gs.first_flush = s_gs.max_gap = 0;
    s_gs.nframes = s_gs.dropped = 0;
    muse_ui_page_name(s_gs.from, sizeof(s_gs.from));
#if CPU_STATS
    char none[4];
    s_gs_cpu.total = 0;
    cpu_since(&s_gs_cpu, none, sizeof(none));
#endif
    lv_timer_resume(s_gs_watch);
}

static int by_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

#define MS1(us) (unsigned)((us) / 1000), (unsigned)((us) / 100 % 10)   /* "%u.%u" */

static void gesture_end(uint32_t t, bool capped)
{
    s_gs.on = false;
    lv_timer_pause(s_gs_watch);
    char to[32];
    muse_ui_page_name(to, sizeof(to));

    int n = s_gs.nframes;
    uint64_t sum = 0, wait = 0, flush = 0, px = 0;
    uint32_t max = 0, sorted[GESTURE_FRAMES_MAX];
    int slow = 0, very_slow = 0;
    for (int i = 0; i < n; i++) {
        const frame_rec_t *f = &s_gs.frames[i];
        sum += f->frame_us;
        wait += f->wait_us;
        flush += f->flush_us;
        px += f->px;
        max = f->frame_us > max ? f->frame_us : max;
        slow += f->frame_us > SLOW_FRAME_US;
        very_slow += f->frame_us > VERY_SLOW_FRAME_US;
        sorted[i] = f->frame_us;
    }
    qsort(sorted, n, sizeof(sorted[0]), by_u32);
    uint32_t div = n ? (uint32_t)n : 1;
    uint32_t avg = (uint32_t)(sum / div), avg_wait = (uint32_t)(wait / div), avg_flush = (uint32_t)(flush / div);
    uint32_t render = avg > avg_wait + avg_flush ? avg - avg_wait - avg_flush : 0;
    uint32_t p95 = n ? sorted[(n * 95 + 99) / 100 - 1] : 0;
    uint32_t avg_px = (uint32_t)(px / div);
    uint32_t span = t - s_gs.start;
    unsigned fps10 = span ? (unsigned)((uint64_t)n * 10000000 / span) : 0;

    ESP_LOGI(TAG, "gesture %s %s->%s: %u ms, %d frames (%u.%u/s); frame avg %u.%u p95 %u.%u max %u.%u ms = "
             "render %u.%u + panel wait %u.%u + flush %u.%u; %u px/frame (%u%%); first flush %u ms, longest "
             "without a flush %u ms; >33 ms: %d, >50 ms: %d%s%s",
             s_gs.label, s_gs.from, to, (unsigned)(span / 1000), n, fps10 / 10, fps10 % 10, MS1(avg), MS1(p95),
             MS1(max), MS1(render), MS1(avg_wait), MS1(avg_flush), (unsigned)avg_px,
             (unsigned)(s_screen_px ? (uint64_t)avg_px * 100 / s_screen_px : 0),
             (unsigned)(s_gs.first_flush ? (s_gs.first_flush - s_gs.start) / 1000 : 0),
             (unsigned)(s_gs.max_gap / 1000), slow, very_slow, capped ? " (still animating: capped)" : "",
             s_gs.dropped ? " (frame list full)" : "");
#if CPU_STATS
    char line[256];
    if (cpu_since(&s_gs_cpu, line, sizeof(line)) && line[0]) {
        ESP_LOGI(TAG, "gesture %s cpu:%s", s_gs.label, line);
    }
#endif
#if CONFIG_LV_USE_PROFILER
    if (muse_lv_prof_enabled()) {
        char prof[640];
        muse_lv_prof_report(prof, sizeof(prof), 16, n);
        ESP_LOGI(TAG, "gesture %s lvgl:%s", s_gs.label, prof);
    }
#endif
}

/* Something still moving: the injected finger, an animation (the tileview's
 * snap is one) or a scroll coasting on. */
static bool ui_moving(void)
{
    return s_inj.on || lv_anim_count_running() > 0 || (s_indev && lv_indev_get_scroll_obj(s_indev));
}

static void gesture_watch(lv_timer_t *timer)
{
    (void)timer;
    if (!s_gs.on || s_inj.on) {
        return;
    }
    uint32_t t = now_us();
    if (ui_moving()) {
        s_gs.last_active = t;
    }
    uint32_t idle_from = s_gs.last_active > s_gs.released ? s_gs.last_active : s_gs.released;
    if (t - idle_from >= GESTURE_TAIL_US) {
        gesture_end(t, false);
    } else if (t - s_gs.released >= GESTURE_CAP_US) {
        gesture_end(t, true);
    }
}

/* The injected finger, wherever the timeline has it now. */
static void inject_read(lv_indev_data_t *data)
{
    uint32_t t = now_us();
    if (!s_inj.started) {
        s_inj.started = true;
        s_inj.start = t;
        gesture_begin(t);
    }
    uint32_t el = t - s_inj.start;
    data->state = LV_INDEV_STATE_PRESSED;
    if (el < s_inj.dur_us) {
        data->point.x = s_inj.x0 + (int32_t)((int64_t)(s_inj.x1 - s_inj.x0) * el / s_inj.dur_us);
        data->point.y = s_inj.y0 + (int32_t)((int64_t)(s_inj.y1 - s_inj.y0) * el / s_inj.dur_us);
        return;
    }
    data->point.x = s_inj.x1;
    data->point.y = s_inj.y1;
    if (!s_inj.at_end) {
        s_inj.at_end = true;   /* one read at the end point, then lift */
        return;
    }
    data->state = LV_INDEV_STATE_RELEASED;
    s_inj.on = false;
    s_gs.released = s_gs.last_active = t;
}

/* LVGL reads a pressed pointer every LV_DEF_REFR_PERIOD by itself; this is
 * the first read, as the touch interrupt would ask for. */
static void inject_kick(void *arg)
{
    (void)arg;
    lv_indev_read(s_indev);
}

static int32_t clamp(int32_t v, int32_t hi)
{
    return v < 0 ? 0 : v > hi ? hi : v;
}

static void inject(const char *label, int32_t x0, int32_t y0, int32_t x1, int32_t y1, int ms)
{
    if (!s_indev || !s_touch_read) {
        ESP_LOGW(TAG, "%s: no touch input to inject into", label);
        return;
    }
    if (!muse_board->display_lock(-1)) {
        return;
    }
    if (s_inj.on || s_gs.on) {
        muse_board->display_unlock();
        ESP_LOGW(TAG, "%s: busy with the last gesture", label);
        return;
    }
    s_inj.x0 = clamp(x0, s_w - 1);
    s_inj.y0 = clamp(y0, s_h - 1);
    s_inj.x1 = clamp(x1, s_w - 1);
    s_inj.y1 = clamp(y1, s_h - 1);
    s_inj.dur_us = (uint32_t)(ms < 1 ? 1 : ms) * 1000;
    s_inj.started = s_inj.at_end = false;
    s_inj.on = true;
    strlcpy(s_gs.label, label, sizeof(s_gs.label));
    lv_async_call(inject_kick, NULL);
    muse_board->display_unlock();
}

/* Draw buffers belong to the display adapter. Replacing/freeing them via
 * LVGL leaves the adapter's DMA bookkeeping pointing at freed storage.
 * Compare heights with separately built bench profiles instead. */
static void set_bufs(int lines)
{
    ESP_LOGW(TAG, "bufs=%d: buffers are adapter-owned; rebuild with a different buffer height", lines);
}

bool muse_perf_console(const char *line)
{
    char dir[8];
    int a, b, c, d, ms = SWIPE_MS;
    if (!strncmp(line, "mark=", 5)) {
        ESP_LOGI(TAG, "mark: %s", line + 5);
        return true;
    }
    if (!strncmp(line, "bufs=", 5)) {
        int lines = atoi(line + 5);
        if (lines >= 4 && lines <= s_h) {
            set_bufs(lines);
        } else {
            ESP_LOGW(TAG, "bufs=lines (4..%d)", (int)s_h);
        }
        return true;
    }
    if (!strncmp(line, "cf=", 3)) {
        bool swapped = !strcmp(line + 3, "swapped");
        if (!swapped && strcmp(line + 3, "native")) {
            ESP_LOGW(TAG, "cf=native|swapped");
            return true;
        }
        if (muse_board->display_lock(-1)) {
            lv_display_set_color_format(NULL, swapped ? LV_COLOR_FORMAT_RGB565_SWAPPED : LV_COLOR_FORMAT_RGB565);
            lv_obj_invalidate(lv_screen_active());
            muse_board->display_unlock();
        }
        ESP_LOGI(TAG, "drawing %s RGB565", swapped ? "byte-swapped" : "native");
        return true;
    }
    if (!strncmp(line, "prof=", 5)) {
#if CONFIG_LV_USE_PROFILER
        muse_lv_prof_enable(!strcmp(line + 5, "on"));
        ESP_LOGI(TAG, "LVGL profile %s", muse_lv_prof_enabled() ? "on" : "off");
#else
        ESP_LOGW(TAG, "no LVGL profiler in this build (CONFIG_LV_USE_PROFILER)");
#endif
        return true;
    }
    if (sscanf(line, "swipe=%7[a-z],%d", dir, &ms) >= 1) {
        /* Three quarters of the way across, centred: a page swipe clearly past halfway. */
        int32_t lx = s_w / 8, rx = s_w * 7 / 8, ty = s_h / 8, by = s_h * 7 / 8, cx = s_w / 2, cy = s_h / 2;
        if (!strcmp(dir, "left")) {
            inject(line, rx, cy, lx, cy, ms);
        } else if (!strcmp(dir, "right")) {
            inject(line, lx, cy, rx, cy, ms);
        } else if (!strcmp(dir, "up")) {
            inject(line, cx, by, cx, ty, ms);
        } else if (!strcmp(dir, "down")) {
            inject(line, cx, ty, cx, by, ms);
        } else {
            ESP_LOGW(TAG, "swipe=left|right|up|down[,ms]");
        }
        return true;
    }
    if (!strncmp(line, "drag=", 5)) {
        if (sscanf(line, "drag=%d,%d,%d,%d,%d", &a, &b, &c, &d, &ms) == 5) {
            inject(line, a, b, c, d, ms);
        } else {
            ESP_LOGW(TAG, "drag=x0,y0,x1,y1,ms");
        }
        return true;
    }
    return false;
}

static void touch_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    if (s_inj.on) {
        inject_read(data);   /* the real controller is ignored meanwhile */
        return;
    }
    s_touch_read(indev, data);
    bool down = data->state == LV_INDEV_STATE_PRESSED;
    if (down && !s_touch_down) {
        uint32_t t = now_us();
        uint32_t irq = s_touch_irq;
        s_touch_irq = 0;
        s_touch.irq = irq && t - irq < 200000 ? irq : 0;
        s_touch.read = t;
        s_touch.flush = 0;
        s_touch.on = true;
    }
    s_touch_down = down;
}

/* ---- talk press: press -> mode -> frame tick -> first flush -> frame done ---- */

typedef struct {
    uint32_t press, mode, face, flush;
    int new_mode;
    bool on;
} talk_t;

static talk_t s_talk;

void muse_perf_talk_press(void)
{
    taskENTER_CRITICAL(&s_lock);
    s_talk.press = now_us();
    s_talk.mode = s_talk.face = s_talk.flush = 0;
    s_talk.new_mode = -1;
    s_talk.on = true;
    taskEXIT_CRITICAL(&s_lock);
}

void muse_perf_mode(int mode)
{
    taskENTER_CRITICAL(&s_lock);
    if (s_talk.on && !s_talk.mode) {
        s_talk.mode = now_us();
        s_talk.new_mode = mode;
    }
    taskEXIT_CRITICAL(&s_lock);
}

void muse_perf_face(void)
{
    taskENTER_CRITICAL(&s_lock);
    if (s_talk.on && s_talk.mode && !s_talk.face) {
        s_talk.face = now_us();
    }
    taskEXIT_CRITICAL(&s_lock);
}

void muse_perf_ui_tick(int64_t us)
{
    taskENTER_CRITICAL(&s_lock);
    s_win.ticks++;
    s_win.tick_us += (uint64_t)us;
    if ((uint32_t)us > s_win.tick_max_us) {
        s_win.tick_max_us = (uint32_t)us;
    }
    taskEXIT_CRITICAL(&s_lock);
}

/* After a frame that drew: finish whichever press is waiting on it. */
static void report_presses(uint32_t t)
{
    if (s_touch.on && s_touch.flush) {
        if (s_touch.irq) {
            ESP_LOGI(TAG, "touch: irq -> read %d ms -> first flush %d ms -> frame done %d ms (total %d ms)",
                     ms(s_touch.irq, s_touch.read), ms(s_touch.read, s_touch.flush), ms(s_touch.flush, t),
                     ms(s_touch.irq, t));
        } else {
            ESP_LOGI(TAG, "touch: read -> first flush %d ms -> frame done %d ms (no interrupt seen)",
                     ms(s_touch.read, s_touch.flush), ms(s_touch.flush, t));
        }
        s_touch.on = false;
    } else if (s_touch.on && t - s_touch.read > TOUCH_GIVE_UP_US) {
        ESP_LOGI(TAG, "touch: nothing redrawn within %d ms", TOUCH_GIVE_UP_US / 1000);
        s_touch.on = false;
    }

    taskENTER_CRITICAL(&s_lock);
    bool done = s_talk.on && s_talk.flush;
    bool stale = s_talk.on && !done && t - s_talk.press > TALK_GIVE_UP_US;
    talk_t k = s_talk;
    if (done || stale) {
        s_talk.on = false;
    }
    taskEXIT_CRITICAL(&s_lock);
    if (done) {
        ESP_LOGI(TAG, "talk: press -> mode %d %d ms -> ui tick %d ms -> first flush %d ms -> frame done %d ms "
                 "(total %d ms)", k.new_mode, ms(k.press, k.mode), ms(k.mode, k.face), ms(k.face, k.flush),
                 ms(k.flush, t), ms(k.press, t));
    } else if (stale) {
        ESP_LOGI(TAG, "talk: no face change drawn within %d ms (mode %s)", TALK_GIVE_UP_US / 1000,
                 k.mode ? "changed" : "unchanged");
    }
}

static void disp_event(lv_event_t *e)
{
    uint32_t t = now_us();
    switch (lv_event_get_code(e)) {
    case LV_EVENT_REFR_START:
        memset(&s_frame, 0, sizeof(s_frame));
        s_frame.start = t;
        break;
    case LV_EVENT_FLUSH_START: {
        const lv_area_t *a = lv_event_get_param(e);
        s_frame.flush_from = t;
        if (s_gs.on) {
            uint32_t gap = t - s_gs.last_flush;
            s_gs.max_gap = gap > s_gs.max_gap ? gap : s_gs.max_gap;
            s_gs.last_flush = t;
            s_gs.first_flush = s_gs.first_flush ? s_gs.first_flush : t;
        }
        s_frame.px += a ? (uint32_t)lv_area_get_size(a) : 0;
        s_frame.flushes++;
        if (s_touch.on && !s_touch.flush) {
            s_touch.flush = t;
        }
        taskENTER_CRITICAL(&s_lock);
        if (s_talk.on && s_talk.face && !s_talk.flush) {
            s_talk.flush = t;
        }
        taskEXIT_CRITICAL(&s_lock);
        break;
    }
    case LV_EVENT_FLUSH_FINISH:
        s_frame.flush_us += t - s_frame.flush_from;
        break;
    case LV_EVENT_FLUSH_WAIT_START:
        s_frame.wait_from = t;
        break;
    case LV_EVENT_FLUSH_WAIT_FINISH:
        s_frame.wait_us += t - s_frame.wait_from;
        break;
    case LV_EVENT_REFR_READY: {
        uint32_t frame = t - s_frame.start;
        taskENTER_CRITICAL(&s_lock);
        s_win.refreshes++;
        if (s_frame.flushes) {
            s_win.frames++;
            s_win.frame_us += frame;
            s_win.wait_us += s_frame.wait_us;
            s_win.flush_us += s_frame.flush_us;
            s_win.px += s_frame.px;
            if (frame > s_win.frame_max_us) {
                s_win.frame_max_us = frame;
            }
        }
        taskEXIT_CRITICAL(&s_lock);
        if (s_gs.on && s_frame.flushes) {
            if (s_gs.nframes < GESTURE_FRAMES_MAX) {
                s_gs.frames[s_gs.nframes++] = (frame_rec_t){ frame, s_frame.wait_us, s_frame.flush_us, s_frame.px };
            } else {
                s_gs.dropped++;
            }
            if (ui_moving()) {
                s_gs.last_active = t;
            }
        }
        report_presses(t);
        break;
    }
    default:
        break;
    }
}

/* ---- the reporter ---- */

static void reporter(void *arg)
{
    (void)arg;
    TickType_t wake = xTaskGetTickCount();
    log_cpu();   /* the baseline */
    for (;;) {
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(CONFIG_MUSE_PERF_PERIOD_MS));
        taskENTER_CRITICAL(&s_lock);
        window_t w = s_win;
        memset(&s_win, 0, sizeof(s_win));
        taskEXIT_CRITICAL(&s_lock);

        uint32_t f = w.frames ? w.frames : 1;
        uint32_t frame = (uint32_t)(w.frame_us / f), wait = (uint32_t)(w.wait_us / f);
        uint32_t flush = (uint32_t)(w.flush_us / f);
        uint32_t render = frame > wait + flush ? frame - wait - flush : 0;
        uint32_t px = (uint32_t)(w.px / f);
        ESP_LOGI(TAG, "%d s: %u frames (%u.%u/s) in %u refreshes; frame avg %u.%u ms max %u ms = render %u.%u + "
                 "panel wait %u.%u + flush %u.%u; %u px/frame (%u%%); ui tick avg %u.%u max %u ms; "
                 "heap %u K free, min %u K, largest %u K",
                 CONFIG_MUSE_PERF_PERIOD_MS / 1000, (unsigned)w.frames,
                 (unsigned)(w.frames * 10000 / CONFIG_MUSE_PERF_PERIOD_MS / 10),
                 (unsigned)(w.frames * 10000 / CONFIG_MUSE_PERF_PERIOD_MS % 10), (unsigned)w.refreshes,
                 (unsigned)(frame / 1000), (unsigned)(frame / 100 % 10), (unsigned)(w.frame_max_us / 1000),
                 (unsigned)(render / 1000), (unsigned)(render / 100 % 10), (unsigned)(wait / 1000),
                 (unsigned)(wait / 100 % 10), (unsigned)(flush / 1000), (unsigned)(flush / 100 % 10),
                 (unsigned)px, (unsigned)(s_screen_px ? (uint64_t)px * 100 / s_screen_px : 0),
                 (unsigned)(w.ticks ? w.tick_us / w.ticks / 1000 : 0),
                 (unsigned)(w.ticks ? w.tick_us / w.ticks / 100 % 10 : 0), (unsigned)(w.tick_max_us / 1000),
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                 (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024),
                 (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024));
        log_cpu();
    }
}

void muse_perf_start(lv_display_t *disp, lv_indev_t *touch)
{
    s_w = lv_display_get_horizontal_resolution(disp);
    s_h = lv_display_get_vertical_resolution(disp);
    s_screen_px = (uint32_t)(s_w * s_h);
    s_indev = touch;
    s_gs_watch = lv_timer_create(gesture_watch, GESTURE_WATCH_MS, NULL);
    lv_timer_pause(s_gs_watch);
    lv_display_add_event_cb(disp, disp_event, LV_EVENT_ALL, NULL);
    if (touch) {
        s_touch_read = lv_indev_get_read_cb(touch);
        if (s_touch_read) {
            lv_indev_set_read_cb(touch, touch_read);
        }
    }
    if (xTaskCreate(reporter, "muse_perf", 4096, NULL, 1, NULL) != pdPASS) {
        ESP_LOGW(TAG, "no reporter task");
        return;
    }
    ESP_LOGI(TAG, "UI timing on, every %d ms", CONFIG_MUSE_PERF_PERIOD_MS);
}
