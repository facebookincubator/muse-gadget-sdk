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

#include "muse_lcd_bands.h"

#include <string.h>

#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "muse_mem.h"

typedef struct {
    const uint8_t *data;   /* NULL: run s_call instead */
    int x1, y1, x2, y2;
} band_t;

static const char *TAG = "lcd_bands";

static lv_display_t *s_disp;
static esp_lcd_panel_handle_t s_panel;
static QueueHandle_t s_bands;            /* room for LVGL's band and a call */
static SemaphoreHandle_t s_call_lock;
static SemaphoreHandle_t s_call_done;
static void (*s_call)(void *);
static void *s_call_arg;
static SemaphoreHandle_t s_chunk_free;   /* internal buffers not on the wire */
static uint8_t *s_chunk[2];
static size_t s_chunk_bytes;
static int s_chunks_out;                 /* of the band being sent */
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

/* Tearing effect (muse_lcd_bands_set_te): the last scan's start, the period, the blanking. */
static bool s_te_on;
static int s_te_gpio = -1;
static volatile int64_t s_te_rise, s_te_fall, s_te_period, s_te_blank;

/* Writes timed against the scan, for the stats (te_hold, te_done). */
typedef struct {
    const char *what;
    uint32_t n, held, torn;
    int64_t sum_us;
} te_stats_t;
static te_stats_t s_te_ui = { .what = "UI band" };
static int s_rows;                       /* the panel's */
/* A band's write, learned from the ones before: about s_fixed_us + bytes *
 * s_ns_per_kb / 1024000 (a smoothed least-squares line over the size). */
static float s_mean_kb, s_mean_us, s_var_kb, s_cov;
static int64_t s_fixed_us, s_ns_per_kb;
static volatile int64_t s_band_end;      /* when the last band's last piece went */
static int64_t s_band_start, s_band_fall, s_band_p, s_band_scan;
static int s_band_y1, s_band_y2;
static size_t s_band_bytes;

/* A piece has gone, or failed to; true if it was the band's last. */
static bool IRAM_ATTR chunk_done(void)
{
    portENTER_CRITICAL_SAFE(&s_lock);
    bool last = s_chunks_out > 0 && --s_chunks_out == 0;
    portEXIT_CRITICAL_SAFE(&s_lock);
    return last;
}

static bool IRAM_ATTR on_chunk_sent(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *edata, void *ctx)
{
    (void)io;
    (void)edata;
    (void)ctx;
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_chunk_free, &woken);
    bool last = chunk_done();
    if (last) {
        s_band_end = esp_timer_get_time();
    }
    bool yield = last && esp_lv_adapter_display_notify_color_trans_done_from_isr(s_disp);
    return yield || woken == pdTRUE;
}

static esp_err_t queue_band(lv_display_t *disp, esp_lcd_panel_handle_t panel, int x1, int y1, int x2, int y2,
                            const void *data, void *ctx)
{
    (void)disp;
    (void)panel;
    (void)ctx;
    band_t b = { data, x1, y1, x2, y2 };
    return xQueueSend(s_bands, &b, 0) == pdTRUE ? ESP_OK : ESP_FAIL;
}

/*
 * The panel reads its memory top to bottom once a refresh, starting as TE
 * falls and taking the period less the blanking (16.2 of 16.8 ms on the
 * CO5300), while QSPI writes it at its own pace (26 ms for a whole 466 px
 * screen, a few ms for a small area). Where the scan and the writer pass each
 * other inside the area, that refresh shows the new picture on one side of
 * the meeting row and the old one on the other: a tear line. They pass each
 * other in rows [y1, y2) if one reaches y1 first and the other reaches y2
 * first. True if a write of those rows starting at t0 and taking w meets any
 * scan around it, the last scan having started at `fall`.
 */
static bool te_meets(int64_t t0, int64_t w, int y1, int y2, int64_t fall, int64_t p, int64_t scan)
{
    for (int k = -1; k <= 3; k++) {
        int64_t f = fall + k * p;
        int64_t ta = f + scan * y1 / s_rows, tb = f + scan * y2 / s_rows;
        if ((t0 < ta) != (t0 + w < tb)) {
            return true;
        }
    }
    return false;
}

/* The scan timing, read whole (the interrupt writes each value as two words). */
static bool te_now(int64_t *fall, int64_t *p, int64_t *scan)
{
    int64_t f, q;
    do {
        f = s_te_fall;
        q = s_te_period;
    } while (f != s_te_fall || q != s_te_period);
    *fall = f;
    *p = q;
    *scan = q - s_te_blank;
    return s_te_on && s_rows && q && esp_timer_get_time() - f < 4 * q;   /* else no TE: the panel is asleep */
}

/*
 * Before a write of rows [y1, y2) expected to take w, give or take
 * spread percent: waits for the first moment it would stay clear of every
 * scan (now, usually; looked for in half-millisecond steps over two
 * refreshes). A write that meets a scan wherever it starts isn't held.
 */
static void te_hold(te_stats_t *st, int y1, int y2, int64_t w, int spread)
{
    for (int tries = 0; tries < 3; tries++) {
        int64_t fall, p, scan, now = esp_timer_get_time();
        if (!te_now(&fall, &p, &scan)) {
            return;
        }
        /* The write starts a little after this (the first piece is copied
         * first) and may take more or less than w: clear for all of it. */
        int64_t lo = w * (100 - spread) / 100 + 400, hi = w * (100 + spread) / 100 + 400, d = 0;
        while (d < 2 * p && (te_meets(now + d - 300, lo, y1, y2, fall, p, scan) ||
                             te_meets(now + d - 300, (lo + hi) / 2, y1, y2, fall, p, scan) ||
                             te_meets(now + d - 300, hi, y1, y2, fall, p, scan))) {
            d += 500;
        }
        if (d == 0 || d >= 2 * p) {
            return;   /* clear now, or never */
        }
        vTaskDelay(pdMS_TO_TICKS((d + 999) / 1000));   /* then check again */
        st->held += tries == 0;
    }
}

/* After the write of rows [y1, y2) that started at t0 and took w, the scans
 * then as te_now gave them: did one meet it? */
static void te_done(te_stats_t *st, int y1, int y2, int64_t t0, int64_t w, int64_t fall, int64_t p, int64_t scan)
{
    st->sum_us += w;
    st->torn += te_meets(t0, w, y1, y2, fall, p, scan);
    if (++st->n % 1000 == 1) {
        ESP_LOGI(TAG, "tearing effect: refresh %d us (blank %d us); %s %d us avg (%d us + %d us/KB), %u of %u held "
                 "for the scan, %u met it", (int)p, (int)(p - scan), st->what, (int)(st->sum_us / st->n),
                 (int)s_fixed_us, (int)(s_ns_per_kb / 1000), (unsigned)st->held, (unsigned)st->n, (unsigned)st->torn);
    }
}

/*
 * Each band LVGL sends has been drawn whole, so its write takes its size at
 * the wire's pace, learned from the bands before (the interrupt notes when
 * each band's last piece went): hold it until it stays clear of the scan.
 * Bands of one refresh go out one at a time as LVGL draws them, so a band
 * boundary can still show for a refresh: each band is whole, though.
 */
static void te_band(int y1, int y2, size_t bytes)
{
    if (s_band_bytes) {
        int64_t w = s_band_end - s_band_start;
        if (w > 0) {
            if (s_band_p) {
                te_done(&s_te_ui, s_band_y1, s_band_y2, s_band_start, w, s_band_fall, s_band_p, s_band_scan);
            }
            float kb = s_band_bytes / 1024.0f, us = (float)w, a = s_mean_kb ? 1 / 16.0f : 1;
            s_mean_kb += a * (kb - s_mean_kb);
            s_mean_us += a * (us - s_mean_us);
            s_var_kb += a * ((kb - s_mean_kb) * (kb - s_mean_kb) - s_var_kb);
            s_cov += a * ((kb - s_mean_kb) * (us - s_mean_us) - s_cov);
            float slope = s_var_kb > 0.01f ? s_cov / s_var_kb : s_mean_us / s_mean_kb;
            slope = slope > 1 ? slope : 1;
            float fixed = s_mean_us - slope * s_mean_kb;
            s_fixed_us = fixed > 0 ? (int64_t)fixed : 0;
            s_ns_per_kb = (int64_t)(slope * 1000);
        }
        s_band_bytes = 0;
    }
    if (!s_te_on) {
        return;
    }
    if (s_ns_per_kb) {
        te_hold(&s_te_ui, y1, y2, s_fixed_us + (int64_t)bytes * s_ns_per_kb / 1024 / 1000, 50);
    }
    s_band_p = te_now(&s_band_fall, &s_band_p, &s_band_scan) ? s_band_p : 0;
    s_band_start = esp_timer_get_time();
    s_band_y1 = y1;
    s_band_y2 = y2;
    s_band_bytes = bytes;
}

/*
 * The panel's draw waits for the piece before to finish, so each piece is
 * copied while the one before is on the wire. Even row counts keep every
 * piece's window starting on an even row, which the CO5300 needs.
 */
static void send_bands(void *arg)
{
    (void)arg;
    int k = 0;
    band_t b;
    for (;;) {
        xQueueReceive(s_bands, &b, portMAX_DELAY);
        if (!b.data) {
            s_call(s_call_arg);
            xSemaphoreGive(s_call_done);
            continue;
        }
        size_t row = (size_t)(b.x2 - b.x1) * 2;
        te_band(b.y1, b.y2, row * (size_t)(b.y2 - b.y1));
        int rows = s_chunk_bytes / row & ~1;
        /* LVGL sends no more until this band is done, so nothing is still
         * counting down. */
        s_chunks_out = (b.y2 - b.y1 + rows - 1) / rows;
        for (int y = b.y1; y < b.y2; y += rows) {
            int h = b.y2 - y < rows ? b.y2 - y : rows;
            xSemaphoreTake(s_chunk_free, portMAX_DELAY);
            memcpy(s_chunk[k], b.data + (size_t)(y - b.y1) * row, h * row);
            esp_err_t err = esp_lcd_panel_draw_bitmap(s_panel, b.x1, y, b.x2, y + h, s_chunk[k]);
            if (err != ESP_OK) {
                xSemaphoreGive(s_chunk_free);
                if (chunk_done()) {
                    lv_display_flush_ready(s_disp);
                }
            }
            k ^= 1;
        }
    }
}

lv_display_t *muse_lcd_bands_register(esp_lv_adapter_display_config_t cfg, int lines, size_t chunk_bytes)
{
    s_panel = cfg.panel;
    s_chunk_bytes = chunk_bytes;
    if (xTaskGetCoreID(xTaskGetCurrentTaskHandle()) != MUSE_UI_CORE) {
        ESP_LOGE(TAG, "not pinned to core %d: the panel's SPI interrupt can strand the send task", MUSE_UI_CORE);
    }
    s_bands = xQueueCreate(2, sizeof(band_t));
    s_call_lock = xSemaphoreCreateMutex();
    s_call_done = xSemaphoreCreateBinary();
    s_chunk_free = xSemaphoreCreateCounting(2, 2);
    s_chunk[0] = heap_caps_malloc(chunk_bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    s_chunk[1] = heap_caps_malloc(chunk_bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!s_bands || !s_call_lock || !s_call_done || !s_chunk_free || !s_chunk[0] || !s_chunk[1] ||
        xTaskCreatePinnedToCore(send_bands, "lcd_send", 2560, NULL, MUSE_UI_PRIORITY + 1, NULL, MUSE_UI_CORE) != pdPASS) {
        return NULL;
    }
    const esp_lcd_panel_io_callbacks_t io_cbs = { .on_color_trans_done = on_chunk_sent };
    esp_lcd_panel_io_register_event_callbacks(cfg.panel_io, &io_cbs, NULL);
    esp_lv_adapter_set_default_display_idf_callback_registration_enabled(false);

    cfg.profile.buffer_height = lines;
    cfg.profile.use_psram = true;
    cfg.profile.require_double_buffer = true;
    s_disp = esp_lv_adapter_register_display(&cfg);
    if (!s_disp) {
        return NULL;
    }
    s_rows = cfg.profile.ver_res;
    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565_SWAPPED);
    const esp_lv_adapter_draw_bitmap_callbacks_t draw_cbs = { .custom_draw_bitmap = queue_band };
    esp_lv_adapter_set_draw_bitmap_callbacks(s_disp, &draw_cbs, NULL);
    return s_disp;
}

void muse_lcd_bands_run(void (*fn)(void *arg), void *arg)
{
    if (!s_bands) {
        fn(arg);
        return;
    }
    xSemaphoreTake(s_call_lock, portMAX_DELAY);
    s_call = fn;
    s_call_arg = arg;
    const band_t call = { 0 };
    xQueueSend(s_bands, &call, portMAX_DELAY);
    xSemaphoreTake(s_call_done, portMAX_DELAY);
    xSemaphoreGive(s_call_lock);
}

static void IRAM_ATTR on_te(void *arg)
{
    (void)arg;
    int64_t t = esp_timer_get_time();
    if (gpio_get_level(s_te_gpio)) {
        if (s_te_rise) {
            s_te_period = t - s_te_rise;
        }
        s_te_rise = t;
    } else {
        s_te_blank = t - s_te_rise;
        s_te_fall = t;
    }
}

void muse_lcd_bands_set_te(int gpio)
{
    s_te_gpio = gpio;
    const gpio_config_t io = {
        .pin_bit_mask = 1ULL << gpio,
        .mode = GPIO_MODE_INPUT,
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    esp_err_t err = gpio_config(&io);
    if (err == ESP_OK) {
        err = gpio_install_isr_service(0);
        err = err == ESP_ERR_INVALID_STATE ? ESP_OK : err;   /* someone else installed it */
    }
    if (err == ESP_OK) {
        err = gpio_isr_handler_add(gpio, on_te, NULL);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "tearing effect on GPIO%d: %s", gpio, esp_err_to_name(err));
        return;
    }
    s_te_on = true;
}
