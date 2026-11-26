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

/* boards/muse_lcd_bands.c with a fake wire that can lose a piece's done
 * (tests/test_lcd_bands_recovery.py). */
#include <stdarg.h>

#include "../components/muse/boards/muse_lcd_bands.c"

int64_t fake_now_us = 1000000;
char fake_log[16384];

void fake_logf(char level, const char *tag, const char *fmt, ...)
{
    size_t n = strlen(fake_log);
    if (n > sizeof fake_log - 512) {
        return;
    }
    n += snprintf(fake_log + n, sizeof fake_log - n, "%c %s: ", level, tag);
    va_list ap;
    va_start(ap, fmt);
    n += vsnprintf(fake_log + n, sizeof fake_log - n, fmt, ap);
    va_end(ap);
    snprintf(fake_log + n, sizeof fake_log - n, "\n");
}

/* The wire: pieces in flight, done in order; one may be lost. */
static esp_lcd_panel_io_color_trans_done_cb_t s_done_cb;
static int s_in_flight, s_pieces_sent, s_lose_piece = -1, s_lost;
static int s_flush_ready, s_async, s_invalidated;
static lv_display_flush_wait_cb_t s_wait_cb;
static esp_lv_adapter_draw_bitmap_cb_t s_draw_cb;
static struct fake_disp { int unused; } s_fake_disp;
static struct fake_obj { int unused; } s_screen, s_top, s_sys;

static bool wire_done_one(void)
{
    if (s_in_flight - s_lost <= 0) {
        return false;
    }
    s_in_flight--;
    esp_lcd_panel_io_event_data_t e = { 0 };
    s_done_cb(NULL, &e, NULL);
    return true;
}

static void wire_drain(void)
{
    while (wire_done_one()) {
    }
}

static void wire_send(void)
{
    if (s_pieces_sent++ == s_lose_piece) {
        s_lost++;   /* on the wire for good: its done never comes */
    }
    s_in_flight++;
}

SemaphoreHandle_t xSemaphoreCreateCounting(int max, int initial)
{
    struct fake_sem *s = calloc(1, sizeof *s);
    s->max = max;
    s->count = initial;
    return s;
}
SemaphoreHandle_t xSemaphoreCreateBinary(void) { return xSemaphoreCreateCounting(1, 0); }
SemaphoreHandle_t xSemaphoreCreateMutex(void) { return xSemaphoreCreateCounting(1, 1); }

BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t ticks)
{
    while (s->count == 0 && ticks && wire_done_one()) {   /* blocked: the wire goes on */
    }
    if (s->count > 0) {
        s->count--;
        return pdTRUE;
    }
    if (ticks == portMAX_DELAY) {
        printf("HANG: an unbounded wait that nothing will end\n%s", fake_log);
        fflush(stdout);
        abort();
    }
    fake_now_us += (int64_t)ticks * 1000;
    return pdFALSE;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t s)
{
    if (s->count >= s->max) {
        return pdFALSE;
    }
    s->count++;
    return pdTRUE;
}

BaseType_t xSemaphoreGiveFromISR(SemaphoreHandle_t s, BaseType_t *woken)
{
    (void)woken;
    return xSemaphoreGive(s);
}

UBaseType_t uxSemaphoreGetCount(SemaphoreHandle_t s) { return (UBaseType_t)s->count; }

QueueHandle_t xQueueCreate(int cap, size_t item)
{
    struct fake_queue *q = calloc(1, sizeof *q);
    q->cap = cap;
    q->item = item;
    return q;
}

BaseType_t xQueueSend(QueueHandle_t q, const void *item, TickType_t ticks)
{
    if (q->n == q->cap) {
        if (ticks == portMAX_DELAY) {
            printf("HANG: a full queue nothing drains\n");
            abort();
        }
        return pdFALSE;
    }
    memcpy(q->buf[q->n++], item, q->item);
    return pdTRUE;
}

BaseType_t xQueueReceive(QueueHandle_t q, void *item, TickType_t ticks)
{
    (void)ticks;
    if (!q->n) {
        return pdFALSE;
    }
    memcpy(item, q->buf[0], q->item);
    memmove(q->buf[0], q->buf[1], sizeof q->buf[0] * (size_t)--q->n);
    return pdTRUE;
}

BaseType_t xTaskCreatePinnedToCore(void (*fn)(void *), const char *name, uint32_t stack, void *arg, UBaseType_t prio,
                                   TaskHandle_t *out, BaseType_t core)
{
    (void)fn; (void)name; (void)stack; (void)arg; (void)prio; (void)out; (void)core;
    return pdPASS;
}

/* "late": the lost piece's done turns up inside piece_lost's settle window. */
static int s_late_step, s_late_ready, s_late_out;
void vTaskDelay(TickType_t ticks)
{
    fake_now_us += (int64_t)ticks * 1000;
    if (s_late_step == 1 && s_settling) {
        int ready = s_flush_ready;
        s_late_step = 2;
        s_lost = 0;
        wire_drain();
        s_late_ready = s_flush_ready - ready;   /* what the late done ended: nothing */
        s_late_out = s_chunks_out;
    }
}

esp_err_t esp_lcd_panel_io_register_event_callbacks(esp_lcd_panel_io_handle_t io, const esp_lcd_panel_io_callbacks_t *cbs,
                                                    void *ctx)
{
    (void)io;
    (void)ctx;
    s_done_cb = cbs->on_color_trans_done;
    return ESP_OK;
}

esp_err_t esp_lcd_panel_draw_bitmap(esp_lcd_panel_handle_t p, int x1, int y1, int x2, int y2, const void *data)
{
    (void)p; (void)x1; (void)y1; (void)x2; (void)y2; (void)data;
    wire_send();
    return ESP_OK;
}

esp_err_t esp_lcd_panel_io_tx_param(esp_lcd_panel_io_handle_t io, int cmd, const void *param, size_t n)
{
    (void)io; (void)cmd; (void)param; (void)n;
    return ESP_OK;
}

esp_err_t esp_lcd_panel_io_tx_color(esp_lcd_panel_io_handle_t io, int cmd, const void *data, size_t n)
{
    (void)io; (void)cmd; (void)data; (void)n;
    wire_send();
    return ESP_OK;
}

void lv_display_set_color_format(lv_display_t *d, int f) { (void)d; (void)f; }
void lv_display_flush_ready(lv_display_t *d) { (void)d; s_flush_ready++; }
void lv_display_set_flush_wait_cb(lv_display_t *d, lv_display_flush_wait_cb_t cb) { (void)d; s_wait_cb = cb; }
int lv_async_call(lv_async_cb_t cb, void *arg)
{
    s_async++;
    cb(arg);
    return 0;
}
lv_obj_t *lv_screen_active(void) { return &s_screen; }
lv_obj_t *lv_layer_top(void) { return &s_top; }
lv_obj_t *lv_layer_sys(void) { return &s_sys; }
void lv_obj_invalidate(lv_obj_t *o) { (void)o; s_invalidated++; }
void esp_lv_adapter_set_default_display_idf_callback_registration_enabled(bool on) { (void)on; }
lv_display_t *esp_lv_adapter_register_display(const esp_lv_adapter_display_config_t *cfg)
{
    (void)cfg;
    return &s_fake_disp;
}
esp_err_t esp_lv_adapter_set_draw_bitmap_callbacks(lv_display_t *d, const esp_lv_adapter_draw_bitmap_callbacks_t *cbs,
                                                   void *ctx)
{
    (void)d;
    (void)ctx;
    s_draw_cb = cbs->custom_draw_bitmap;
    return ESP_OK;
}
bool esp_lv_adapter_display_notify_color_trans_done_from_isr(lv_display_t *d)
{
    lv_display_flush_ready(d);   /* the adapter's tear mode none: flush_ready from the interrupt */
    return false;
}

/* --- what LVGL and the send task do, one step at a time --- */

#define W 466
#define ROWS 116   /* a band: 4 pieces of 32 rows (and one of 20) */
static uint8_t s_band[W * 2 * ROWS];

/* LVGL sends a band (its flush_cb through the adapter); 0 if queued. */
static int lvgl_flush(void)
{
    esp_err_t err = s_draw_cb(&s_fake_disp, NULL, 0, 0, W, ROWS, s_band, NULL);
    if (err != ESP_OK) {
        lv_display_flush_ready(&s_fake_disp);   /* the adapter, on a failed draw */
    }
    return err;
}

/* LVGL's wait_for_flushing with the callback set: how long it took, in ms. */
static long lvgl_wait(void)
{
    if (!s_wait_cb) {
        printf("HANG: LVGL would busy-wait (no flush_wait_cb)\n");
        abort();
    }
    int64_t t0 = fake_now_us;
    s_wait_cb(&s_fake_disp);
    return (long)((fake_now_us - t0) / 1000);
}

/* The send task takes what's queued. */
static int k_buf;
static bool send_step(void)
{
    band_t b;
    if (!xQueueReceive(s_bands, &b, 0)) {
        return false;
    }
    if (!b.data) {
        if (wire_idle(0)) {
            s_call(s_call_arg);
        }
        xSemaphoreGive(s_call_done);
        return true;
    }
    send_band(&b, &k_buf);
    return true;
}

static void fill(uint8_t *dst, int stride, int row, int rows, void *arg)
{
    (void)dst; (void)stride; (void)row; (void)rows; (void)arg;
}

static void report(const char *what, long wait_ms)
{
    printf("%s wait=%ld flush_ready=%d async=%d invalidated=%d free=%u out=%d lost_pieces=%u lost_flushes=%u "
           "in_flight=%d\n", what, wait_ms, s_flush_ready, s_async, s_invalidated, (unsigned)uxSemaphoreGetCount(s_chunk_free),
           s_chunks_out, (unsigned)s_lost_pieces, (unsigned)s_lost_flushes, s_in_flight - s_lost);
}

static void setup(void)
{
    esp_lv_adapter_display_config_t cfg = { 0 };
    cfg.profile.ver_res = 466;
    if (!muse_lcd_bands_register(cfg, ROWS, (size_t)W * 2 * 32)) {
        printf("register failed\n");
        exit(1);
    }
}

int main(int argc, char **argv)
{
    const char *t = argc > 1 ? argv[1] : "";
    setup();
    if (!strcmp(t, "normal")) {
        for (int i = 0; i < 3; i++) {
            lvgl_flush();
            send_step();
            wire_drain();
            report("band", lvgl_wait());
        }
    } else if (!strcmp(t, "lost")) {
        s_lose_piece = 2;   /* the band's third piece never comes back */
        lvgl_flush();
        send_step();
        report("lost", lvgl_wait());
        s_lose_piece = -1;
        lvgl_flush();
        send_step();
        wire_drain();
        report("next", lvgl_wait());
    } else if (!strcmp(t, "late")) {
        s_lose_piece = 1;
        s_late_step = 1;     /* the lost done turns up after all, while piece_lost settles */
        lvgl_flush();
        send_step();
        lvgl_wait();
        printf("settle delivered=%d ended=%d out=%d\n", s_late_step == 2, s_late_ready, s_late_out);
        report("late", 0);
        s_lose_piece = -1;
        lvgl_flush();
        send_step();
        wire_drain();
        report("next", lvgl_wait());
    } else if (!strcmp(t, "stuck")) {
        /* The send task never takes the band (stuck in the driver). */
        lvgl_flush();
        report("stuck", lvgl_wait());
        int a = lvgl_flush();
        long w2 = lvgl_wait();
        int b = lvgl_flush();   /* the queue is full now */
        long w3 = lvgl_wait();
        printf("second=%d wait2=%ld third=%d wait3=%ld\n", a, w2, b, w3);
        report("after", 0);
    } else if (!strcmp(t, "frame")) {
        s_lose_piece = 1;
        frame_job_t j = { 0, 0, W, ROWS, 32, fill, NULL };
        send_frame(&j);
        report("frame", 0);
        s_lose_piece = -1;
        send_frame(&j);
        wire_drain();
        report("frame2", 0);
    }
    printf("--log--\n%s", fake_log);
    return 0;
}
