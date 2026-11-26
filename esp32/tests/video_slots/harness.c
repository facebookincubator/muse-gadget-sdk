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
 * muse_video.c's decode and show tasks on the host, on pthreads, with a fake
 * panel, stream ring and UI, for tests/video_seams_harness.c
 * (tests/test_video_untrusted.py), which adds the real muse_vdec.c and
 * muse_yuv.c and drives them.
 *
 * muse_video.c is included, so its statics (s_slots, s_frames, ...) are in
 * reach.
 */
#include <pthread.h>
#include <sched.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#if defined(__GLIBC__) && (__GLIBC__ < 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ < 38)) /* no strlcpy before glibc 2.38 */
static size_t strlcpy(char *d, const char *s, size_t n)
{
    size_t l = strlen(s);
    if (n) {
        size_t k = l < n - 1 ? l : n - 1;
        memcpy(d, s, k);
        d[k] = 0;
    }
    return l;
}
#endif

#include "muse_video.c"
#include "muse_video_tick.c"
#include "muse_video_fit.c"   /* the panel fit and the round cover (components/muse) */
#include "../../components/muse/muse_vdirty.c"   /* changed-only redraw */
#include "muse_lvgl_hold.c"
#include "../../components/muse/muse_mic_ring.c"   /* the real ring state (its listener) */
#include "../../components/muse/muse_turn_dot.c"    /* the real turn dot: its repaints run in every ordering */

/* ---- time, log ------------------------------------------------------------ */

static int64_t t0_us;
int64_t esp_timer_get_time(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    int64_t us = (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
    if (!t0_us) t0_us = us - 1000000;
    return us - t0_us;
}

static atomic_int g_lost_logs, g_stuck_logs, g_restarts, g_verbose;
void fake_log(char level, const char *tag, const char *fmt, ...)
{
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (strstr(line, "pictures lost")) atomic_fetch_add(&g_lost_logs, 1);
    if (strstr(line, "still waiting for a picture") || strstr(line, "give back its pictures")) atomic_fetch_add(&g_stuck_logs, 1);
    if (g_verbose || level == 'E' || strstr(line, "still waiting") || strstr(line, "give back")) {
        printf("%c (%lld) %s: %s\n", level, (long long)(esp_timer_get_time() / 1000), tag, line);
    }
}

void esp_restart(void)
{
    atomic_fetch_add(&g_restarts, 1);
    printf("E esp_restart() called\n");
}

/* ---- FreeRTOS on pthreads -------------------------------------------------- */

struct fake_queue {
    pthread_mutex_t m;
    pthread_cond_t c;
    unsigned len, item, count, head;
    uint8_t *buf;
};

static struct timespec deadline(TickType_t ms)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += ms / 1000;
    ts.tv_nsec += (long)(ms % 1000) * 1000000;
    if (ts.tv_nsec >= 1000000000) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000;
    }
    return ts;
}

static QueueHandle_t q_new(unsigned len, unsigned item, unsigned initial)
{
    struct fake_queue *q = calloc(1, sizeof *q);
    pthread_mutex_init(&q->m, NULL);
    pthread_cond_init(&q->c, NULL);
    q->len = len;
    q->item = item;
    q->count = initial;
    q->buf = calloc(len ? len : 1, item ? item : 1);
    return q;
}

/* wait for cond(q) under q->m; false on timeout */
static bool q_wait(struct fake_queue *q, bool (*cond)(struct fake_queue *), TickType_t wait)
{
    struct timespec ts = deadline(wait);
    while (!cond(q)) {
        if (wait == 0) return false;
        if (wait == portMAX_DELAY) {
            pthread_cond_wait(&q->c, &q->m);
        } else if (pthread_cond_timedwait(&q->c, &q->m, &ts) != 0 && !cond(q)) {
            return false;
        }
    }
    return true;
}
static bool has_room(struct fake_queue *q) { return q->count < q->len; }
static bool has_item(struct fake_queue *q) { return q->count > 0; }

QueueHandle_t xQueueCreate(UBaseType_t len, UBaseType_t item) { return q_new(len, item, 0); }

BaseType_t xQueueSend(QueueHandle_t q, const void *item, TickType_t wait)
{
    pthread_mutex_lock(&q->m);
    if (!q_wait(q, has_room, wait)) {
        pthread_mutex_unlock(&q->m);
        return pdFALSE;
    }
    memcpy(q->buf + ((q->head + q->count) % q->len) * q->item, item, q->item);
    q->count++;
    pthread_cond_broadcast(&q->c);
    pthread_mutex_unlock(&q->m);
    return pdTRUE;
}

BaseType_t xQueueSendToFront(QueueHandle_t q, const void *item, TickType_t wait)
{
    pthread_mutex_lock(&q->m);
    if (!q_wait(q, has_room, wait)) {
        pthread_mutex_unlock(&q->m);
        return pdFALSE;
    }
    q->head = (q->head + q->len - 1) % q->len;
    memcpy(q->buf + q->head * q->item, item, q->item);
    q->count++;
    pthread_cond_broadcast(&q->c);
    pthread_mutex_unlock(&q->m);
    return pdTRUE;
}

static BaseType_t q_take(QueueHandle_t q, void *item, TickType_t wait, bool remove)
{
    pthread_mutex_lock(&q->m);
    if (!q_wait(q, has_item, wait)) {
        pthread_mutex_unlock(&q->m);
        return pdFALSE;
    }
    if (q->item) memcpy(item, q->buf + q->head * q->item, q->item);
    if (remove) {
        if (q->item) q->head = (q->head + 1) % q->len;
        q->count--;
        pthread_cond_broadcast(&q->c);
    }
    pthread_mutex_unlock(&q->m);
    return pdTRUE;
}
BaseType_t xQueueReceive(QueueHandle_t q, void *item, TickType_t wait) { return q_take(q, item, wait, true); }
BaseType_t xQueuePeek(QueueHandle_t q, void *item, TickType_t wait) { return q_take(q, item, wait, false); }

UBaseType_t uxQueueMessagesWaiting(QueueHandle_t q)
{
    pthread_mutex_lock(&q->m);
    unsigned n = q->count;
    pthread_mutex_unlock(&q->m);
    return n;
}

SemaphoreHandle_t xSemaphoreCreateCounting(UBaseType_t max, UBaseType_t initial) { return q_new(max, 0, initial); }
SemaphoreHandle_t xSemaphoreCreateBinary(void) { return q_new(1, 0, 0); }
SemaphoreHandle_t xSemaphoreCreateMutex(void) { return q_new(1, 0, 1); }
SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *buf)
{
    (void)buf;
    return xSemaphoreCreateMutex();
}
BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t wait) { return q_take(s, NULL, wait, true); }
BaseType_t xSemaphoreGive(SemaphoreHandle_t s)
{
    pthread_mutex_lock(&s->m);
    if (s->count >= s->len) {
        pthread_mutex_unlock(&s->m);
        return pdFALSE;
    }
    s->count++;
    pthread_cond_broadcast(&s->c);
    pthread_mutex_unlock(&s->m);
    return pdTRUE;
}
UBaseType_t uxSemaphoreGetCount(SemaphoreHandle_t s) { return uxQueueMessagesWaiting(s); }

struct fake_task {
    pthread_t th;
    TaskFunction_t fn;
    void *arg;
    int core;
    pthread_mutex_t m;
    pthread_cond_t c;
    uint32_t notify;
};
static __thread struct fake_task *t_self;

static void *task_main(void *p)
{
    t_self = p;
    t_self->fn(t_self->arg);
    return NULL;
}

BaseType_t xTaskCreatePinnedToCore(TaskFunction_t fn, const char *name, uint32_t stack, void *arg, UBaseType_t prio,
                                   TaskHandle_t *h, BaseType_t core)
{
    (void)name;
    (void)stack;
    (void)prio;
    struct fake_task *t = calloc(1, sizeof *t);
    t->fn = fn;
    t->arg = arg;
    t->core = core;
    pthread_mutex_init(&t->m, NULL);
    pthread_cond_init(&t->c, NULL);
    if (h) *h = t;
    pthread_create(&t->th, NULL, task_main, t);
    pthread_detach(t->th);
    return pdPASS;
}
BaseType_t xTaskCreatePinnedToCoreWithCaps(TaskFunction_t fn, const char *name, uint32_t stack, void *arg,
                                           UBaseType_t prio, TaskHandle_t *h, BaseType_t core, unsigned caps)
{
    (void)caps;
    return xTaskCreatePinnedToCore(fn, name, stack, arg, prio, h, core);
}
BaseType_t xTaskGetCoreID(TaskHandle_t t) { return t ? t->core : (t_self ? t_self->core : 0); }
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t t) { (void)t; return 1024; }
TaskHandle_t xTaskGetIdleTaskHandleForCore(BaseType_t core)
{
    (void)core;
    return NULL;
}
void xTaskNotifyGive(TaskHandle_t t)
{
    if (!t) return;
    pthread_mutex_lock(&t->m);
    t->notify++;
    pthread_cond_broadcast(&t->c);
    pthread_mutex_unlock(&t->m);
}
uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t wait)
{
    struct fake_task *t = t_self;
    pthread_mutex_lock(&t->m);
    struct timespec ts = deadline(wait);
    while (!t->notify) {
        if (wait == 0) break;
        if (wait == portMAX_DELAY) {
            pthread_cond_wait(&t->c, &t->m);
        } else if (pthread_cond_timedwait(&t->c, &t->m, &ts) != 0) {
            break;
        }
    }
    uint32_t n = t->notify;
    if (n) t->notify = clear ? 0 : n - 1;
    pthread_mutex_unlock(&t->m);
    return n;
}
void vTaskDelay(TickType_t ticks) { usleep(ticks * 1000); }
/* muse_video's tasks retire between streams: a task ends itself,
 * or the show task ends the parked decode tasks (blocked in a wait, a
 * cancellation point). */
void vTaskDelete(TaskHandle_t t)
{
    if (!t || t == t_self) {
        pthread_exit(NULL);
    }
    pthread_cancel(t->th);
}
void vTaskDeleteWithCaps(TaskHandle_t t) { vTaskDelete(t); }
void taskYIELD(void) { sched_yield(); }

struct fake_timer {
    esp_timer_create_args_t a;
    uint64_t period;
};
static void *timer_main(void *p)
{
    struct fake_timer *t = p;
    for (;;) {
        usleep(t->period);
        t->a.callback(t->a.arg);
    }
    return NULL;
}
esp_err_t esp_timer_create(const esp_timer_create_args_t *a, esp_timer_handle_t *h)
{
    struct fake_timer *t = calloc(1, sizeof *t);
    t->a = *a;
    *h = t;
    return ESP_OK;
}
esp_err_t esp_timer_start_periodic(esp_timer_handle_t h, uint64_t period_us)
{
    h->period = period_us;
    pthread_t th;
    pthread_create(&th, NULL, timer_main, h);
    pthread_detach(th);
    return ESP_OK;
}

/* ---- the UI: esp_lv_adapter's worker, pause and resume as the real adapter has them ----
 *
 * pause() marks the adapter paused and waits (up to its timeout, or forever
 * with -1) for the worker to acknowledge at the top of its loop; resume()
 * clears the mark and wakes the worker. As in esp_lv_adapter.c, neither takes
 * a lock: a resume between a pause's mark and the worker's acknowledgement
 * leaves the pause waiting (muse_lvgl_hold serializes them). A UI frame
 * running while video draws to the panel is counted (two writers on one
 * panel).
 *
 * Both versions of the pause handshake:
 *   - upstream 0.6.4's (g_ad_fixed 0, ADAPTER=racy): mark paused, THEN clear
 *     the pending acknowledgement, so one the worker posts in between is
 *     cleared away and the pause waits for one that never comes; a timeout
 *     un-marks the adapter without waking the worker, which stays asleep in
 *     its paused wait;
 *   - the fixed one cmake/esp_lvgl_adapter_fix.cmake builds (the default):
 *     clear, then mark; a timeout wakes the worker.
 * Fault injection: g_force_race makes a pause wait, right after marking the
 * adapter paused, until the worker has acknowledged (the window the racy
 * order loses it in), every g_force_race-th pause (1: all); g_drop_acks makes
 * the worker acknowledge without posting the semaphore that many times (an
 * acknowledgement lost some other way). */
static pthread_mutex_t g_lv = PTHREAD_MUTEX_INITIALIZER;   /* held while a UI frame runs */
static atomic_int g_asleep;
static atomic_int g_ad_paused, g_ad_ack, g_pauses, g_resumes, g_lv_running, g_drawing, g_overlaps;
static pthread_mutex_t g_ad_m = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_ad_c = PTHREAD_COND_INITIALIZER;   /* the worker's notification, and the pause's semaphore */
static int g_ad_notify, g_ad_sem;
static atomic_int g_ad_blocked;                            /* a pause waiting for its acknowledgement */
static atomic_int g_ad_fixed = 1, g_force_race, g_forced, g_drop_acks, g_ad_timeouts, g_ui_loops;
static atomic_uint g_pause_no;

int esp_lv_adapter_pause(int timeout_ms)
{
    if (atomic_load(&g_ad_paused)) return 0;
    bool fixed = atomic_load(&g_ad_fixed);
    atomic_store(&g_ad_ack, 0);
    if (fixed) {
        pthread_mutex_lock(&g_ad_m);
        g_ad_sem = 0;   /* the fix: clear a stale acknowledgement before the mark */
        pthread_mutex_unlock(&g_ad_m);
    }
    atomic_store(&g_ad_paused, 1);
    unsigned no = atomic_fetch_add(&g_pause_no, 1);
    int every = atomic_load(&g_force_race);
    if (every > 0 && no % (unsigned)every == 0) {
        /* the worker acknowledges right here, between the mark and (racy order) the clear */
        int64_t until = esp_timer_get_time() + 1000000;   /* a worker that sleeps for good never does */
        while (!atomic_load(&g_ad_ack) && esp_timer_get_time() < until) usleep(1000);
        atomic_fetch_add(&g_forced, 1);
    }
    pthread_mutex_lock(&g_ad_m);
    if (!fixed) g_ad_sem = 0;   /* upstream 0.6.4: clear pending, after the mark */
    atomic_fetch_add(&g_ad_blocked, 1);
    struct timespec ts = deadline(timeout_ms < 0 ? 0 : (TickType_t)timeout_ms);
    while (!g_ad_sem) {
        if (timeout_ms < 0) {
            pthread_cond_wait(&g_ad_c, &g_ad_m);
        } else if (pthread_cond_timedwait(&g_ad_c, &g_ad_m, &ts) != 0) {
            break;
        }
    }
    bool got = g_ad_sem != 0;
    g_ad_sem = 0;
    atomic_fetch_sub(&g_ad_blocked, 1);
    if (!got) {
        atomic_store(&g_ad_paused, 0);
        if (fixed) {   /* the fix: wake the worker a timed-out pause un-marks */
            g_ad_notify++;
            pthread_cond_broadcast(&g_ad_c);
        }
        pthread_mutex_unlock(&g_ad_m);
        atomic_fetch_add(&g_ad_timeouts, 1);
        return ESP_ERR_TIMEOUT;
    }
    pthread_mutex_unlock(&g_ad_m);
    atomic_fetch_add(&g_pauses, 1);
    return 0;
}
int esp_lv_adapter_resume(void)
{
    if (!atomic_load(&g_ad_paused)) return 0;
    atomic_store(&g_ad_paused, 0);
    atomic_store(&g_ad_ack, 0);
    pthread_mutex_lock(&g_ad_m);
    g_ad_notify++;
    pthread_cond_broadcast(&g_ad_c);
    pthread_mutex_unlock(&g_ad_m);
    atomic_fetch_add(&g_resumes, 1);
    return 0;
}
#define g_lv_paused g_ad_paused

static void *ui_main(void *p)
{
    (void)p;
    for (;;) {
        if (atomic_load(&g_ad_paused)) {
            pthread_mutex_lock(&g_ad_m);
            if (!atomic_load(&g_ad_ack)) {
                atomic_store(&g_ad_ack, 1);
                if (atomic_load(&g_drop_acks) > 0) {
                    atomic_fetch_sub(&g_drop_acks, 1);   /* acknowledged, but the pause never hears it */
                } else {
                    g_ad_sem = 1;
                    pthread_cond_broadcast(&g_ad_c);
                }
            }
            while (!g_ad_notify) pthread_cond_wait(&g_ad_c, &g_ad_m);   /* ulTaskNotifyTake(portMAX_DELAY) */
            g_ad_notify = 0;
            pthread_mutex_unlock(&g_ad_m);
            continue;
        }
        atomic_fetch_add(&g_ui_loops, 1);
        if (!atomic_load(&g_asleep)) {   /* the UI ticks the view only with the screen on */
            pthread_mutex_lock(&g_lv);
            atomic_store(&g_lv_running, 1);
            if (atomic_load(&g_drawing)) atomic_fetch_add(&g_overlaps, 1);
            muse_video_tick();
            usleep(3000);   /* rendering and flushing the frame */
            if (atomic_load(&g_drawing)) atomic_fetch_add(&g_overlaps, 1);
            atomic_store(&g_lv_running, 0);
            pthread_mutex_unlock(&g_lv);
        }
        pthread_mutex_lock(&g_ad_m);
        struct timespec ts = deadline(16);
        while (!g_ad_notify && pthread_cond_timedwait(&g_ad_c, &g_ad_m, &ts) == 0) {
        }
        g_ad_notify = 0;
        pthread_mutex_unlock(&g_ad_m);
    }
    return NULL;
}

struct fake_lv_obj {
    int flags;
    char text[64];
};
const lv_font_t lv_font_montserrat_20;
static struct fake_lv_obj g_screen;
lv_obj_t *lv_screen_active(void) { return &g_screen; }
lv_obj_t *lv_obj_create(lv_obj_t *parent)
{
    (void)parent;
    return calloc(1, sizeof(struct fake_lv_obj));
}
lv_obj_t *lv_label_create(lv_obj_t *parent) { return lv_obj_create(parent); }
const char *lv_label_get_text(const lv_obj_t *o) { return o->text; }
void lv_label_set_text(lv_obj_t *o, const char *t) { snprintf(o->text, sizeof o->text, "%s", t); }
void lv_label_set_long_mode(lv_obj_t *o, int m) { (void)o, (void)m; }
void lv_obj_add_flag(lv_obj_t *o, int f) { o->flags |= f; }
void lv_obj_remove_flag(lv_obj_t *o, int f) { o->flags &= ~f; }
void lv_obj_set_flag(lv_obj_t *o, int f, bool on) { on ? lv_obj_add_flag(o, f) : lv_obj_remove_flag(o, f); }
bool lv_obj_has_flag(const lv_obj_t *o, int f) { return (o->flags & f) != 0; }
void lv_obj_align(lv_obj_t *o, int a, int x, int y) { (void)o, (void)a, (void)x, (void)y; }
void lv_obj_invalidate(lv_obj_t *o) { (void)o; }
void lv_obj_move_foreground(lv_obj_t *o) { (void)o; }
void lv_obj_remove_style_all(lv_obj_t *o) { (void)o; }
void lv_obj_set_size(lv_obj_t *o, int w, int h) { (void)o, (void)w, (void)h; }
void lv_obj_set_width(lv_obj_t *o, int w) { (void)o, (void)w; }
void lv_obj_set_style_bg_color(lv_obj_t *o, lv_color_t c, int s) { (void)o, (void)c, (void)s; }
void lv_obj_set_style_bg_opa(lv_obj_t *o, int opa, int s) { (void)o, (void)opa, (void)s; }
void lv_obj_set_style_text_align(lv_obj_t *o, int a, int s) { (void)o, (void)a, (void)s; }
void lv_obj_set_style_text_color(lv_obj_t *o, lv_color_t c, int s) { (void)o, (void)c, (void)s; }
void lv_obj_set_style_text_font(lv_obj_t *o, const lv_font_t *f, int s) { (void)o, (void)f, (void)s; }
lv_color_t lv_color_black(void) { return (lv_color_t){ 0, 0, 0 }; }
lv_color_t lv_color_hex(uint32_t c) { return (lv_color_t){ (uint8_t)(c >> 16), (uint8_t)(c >> 8), (uint8_t)c }; }

/* ---- the board, the screen's state ---------------------------------------- */

static atomic_int g_draw_us = 15000;
/* VD_FILL=1: a 368 x 448 panel the picture fills, with draw_windows (changed-only redraw, as the C6). */
static atomic_uint g_pw;                  /* the panel's write count */
static atomic_int g_win_draws, g_win_pieces, g_win_bad;
static uint32_t fake_writes(void) { return atomic_load(&g_pw); }
static void fake_draw(int x1, int y1, int x2, int y2, int rows,
                      void (*fill)(uint8_t *dst, int stride, int row, int rows, void *arg), void *arg)
{
    static uint8_t line[480 * 2 * 8 + 64];
    int w = x2 - x1, h = y2 - y1;
    if (w <= 0 || w > 480) return;
    int pieces = (h + rows - 1) / rows;
    atomic_fetch_add(&g_pw, 1);
    atomic_store(&g_drawing, 1);
    if (atomic_load(&g_lv_running)) atomic_fetch_add(&g_overlaps, 1);
    for (int r = 0; r < h; r += rows) {
        fill(line, w * 2, r, rows < h - r ? rows : h - r, arg);
        usleep(atomic_load(&g_draw_us) / (pieces ? pieces : 1));
        if (atomic_load(&g_lv_running)) atomic_fetch_add(&g_overlaps, 1);
    }
    atomic_store(&g_drawing, 0);
}
static void fake_windows(int x0, int y0, const muse_board_win_t *win, int n, int rows,
                         void (*fill)(uint8_t *dst, int stride, int x, int y, int w, int rows, void *arg), void *arg)
{
    static uint8_t line[480 * 2 * 8 + 64];
    atomic_fetch_add(&g_pw, 1);
    atomic_fetch_add(&g_win_draws, 1);
    atomic_store(&g_drawing, 1);
    for (int i = 0; i < n; i++) {
        const muse_board_win_t *w = &win[i];
        if (w->w <= 0 || w->h <= 0 || w->x < 0 || w->y < 0 || x0 + w->x + w->w > 368 || y0 + w->y + w->h > 448 ||
            w->x % 2 || w->w % 2 || w->y % 2) {
            atomic_fetch_add(&g_win_bad, 1);
            continue;
        }
        for (int r = 0; r < w->h; r += rows) {
            fill(line, w->w * 2, w->x, w->y + r, w->w, rows < w->h - r ? rows : w->h - r, arg);
            atomic_fetch_add(&g_win_pieces, 1);
        }
        usleep(atomic_load(&g_draw_us) / (2 * n));
    }
    atomic_store(&g_drawing, 0);
}
static muse_board_t g_board = { "fake round panel", 466, 466, true, fake_draw };
const muse_board_t *muse_board = &g_board;
const muse_board_t *muse_board_get(void) { return &g_board; }

bool muse_state_asleep(void) { return atomic_load(&g_asleep); }
/* The turn the user is in: the closer moves it at random, so the turn dot
 * shows, breathes and goes mid-stream (its box repainted by the show task). */
static atomic_int g_mode = MUSE_MODE_IDLE;
muse_mode_t muse_state_mode(float *secs) { if (secs) *secs = 0; return (muse_mode_t)atomic_load(&g_mode); }
void muse_state_set_asleep(bool asleep) { atomic_store(&g_asleep, asleep); }
void muse_state_poke(void) {}

/* UI streams' frame buffer (muse_ui_fb.c): never active here, so UI pictures
 * take the plain conversion and still patches are refused. */
bool muse_ui_fb_begin(int w, int h) { (void)w, (void)h; return false; }
void muse_ui_fb_end(void) {}
bool muse_ui_fb_active(void) { return false; }
int muse_ui_fb_picture(const muse_pic_t *p0, const muse_pic_t *p1, int h0, bool all, muse_ui_rect_t *rects)
{
    (void)p0, (void)p1, (void)h0, (void)all, (void)rects;
    return 0;
}
bool muse_ui_fb_still(const uint8_t *d, uint32_t n, muse_ui_rect_t *r) { (void)d, (void)n, (void)r; return false; }
bool muse_ui_fb_can_inflate(void) { return false; }
void muse_ui_fb_fill(uint8_t *dst, int stride, int row, int rows, void *arg) { (void)dst, (void)stride, (void)row, (void)rows, (void)arg; }
const uint16_t *muse_ui_fb_pixels(int *w, int *h)
{
    *w = *h = 0;
    return NULL;
}
void muse_ui_fb_last_stats(int *mbs, int *total, int *skipped, uint32_t *convert_us) { *mbs = *total = *skipped = 0, *convert_us = 0; }

/* ---- the ring: frames the driver pushes ------------------------------------ */

#define FIFO 64
static struct {
    pthread_mutex_t m;
    uint8_t au[FIFO][8];
    muse_vframe_t fr[FIFO];
    unsigned head, count;
    uint32_t received;
} g_ring = { PTHREAD_MUTEX_INITIALIZER };

int32_t muse_vstream_pop(muse_vstream_t *s, uint8_t *dst, uint32_t cap, muse_vframe_t *f)
{
    (void)s;
    (void)cap;
    pthread_mutex_lock(&g_ring.m);
    if (!g_ring.count) {
        pthread_mutex_unlock(&g_ring.m);
        return 0;
    }
    unsigned i = g_ring.head;
    memcpy(dst, g_ring.au[i], 5);
    *f = g_ring.fr[i];
    g_ring.head = (g_ring.head + 1) % FIFO;
    g_ring.count--;
    pthread_mutex_unlock(&g_ring.m);
    return 5;
}
uint32_t muse_vstream_available(const muse_vstream_t *s)
{
    (void)s;
    return 64 * 1024;
}
uint32_t muse_vstream_received(const muse_vstream_t *s)
{
    (void)s;
    return g_ring.received;
}

static uint16_t g_number;
static void push(char type, int w, int h, bool gap)
{
    pthread_mutex_lock(&g_ring.m);
    if (g_ring.count < FIFO) {
        unsigned i = (g_ring.head + g_ring.count) % FIFO;
        uint8_t *a = g_ring.au[i];
        a[0] = (uint8_t)type;
        a[1] = (uint8_t)w;
        a[2] = (uint8_t)(w >> 8);
        a[3] = (uint8_t)h;
        a[4] = (uint8_t)(h >> 8);
        g_ring.fr[i] = (muse_vframe_t){ g_number++, type == 'K' ? MUSE_VIDEO_FRAME_KEY : 0, 5, gap };
        g_ring.count++;
        g_ring.received += 5;
    }
    pthread_mutex_unlock(&g_ring.m);
    if (s_dec_task) xTaskNotifyGive(s_dec_task);
}
static void ring_clear(void)
{
    pthread_mutex_lock(&g_ring.m);
    g_ring.count = 0;
    pthread_mutex_unlock(&g_ring.m);
}
