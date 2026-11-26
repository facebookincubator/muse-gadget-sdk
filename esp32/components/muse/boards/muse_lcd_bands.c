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
#include "muse_mic_ring.h"
#include "muse_turn_dot.h"

#include <string.h>

#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_rom_sys.h"   /* esp_rom_delay_us: a window group's last bit of hold */
#include "esp_timer.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#if CONFIG_MUSE_VIDEO
#include "muse_video.h"   /* muse_video_piece_sent, muse_video_panel_timing */
#endif

#include "muse_mem.h"

typedef struct {
    const uint8_t *data;   /* NULL: run s_call instead */
    int x1, y1, x2, y2;
} band_t;

static const char *TAG = "lcd_bands";

static lv_display_t *s_disp;
static esp_lcd_panel_handle_t s_panel;
static esp_lcd_panel_io_handle_t s_io;
static int s_gap_x, s_gap_y;
static QueueHandle_t s_bands;            /* room for LVGL's band and a job (muse_lcd_bands_run) */
static SemaphoreHandle_t s_call_lock;
static SemaphoreHandle_t s_call_done;
static void (*s_call)(void *);
static void *s_call_arg;
static SemaphoreHandle_t s_chunk_free;   /* internal buffers not on the wire */
static uint8_t *s_chunk[2];
static size_t s_chunk_bytes;
static int s_chunks_out;                 /* of the band being sent */
static bool s_settling;                  /* piece_lost: a late done counts for nothing (under s_lock) */
static int s_owed;                       /* pieces piece_lost gave up whose done may still come (under s_lock) */
static bool s_band_out;                  /* the send task is sending LVGL's band (send_band) */
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

/*
 * Never a stuck screen that takes the core with it (seen on the W175 and
 * Watcher: LVGL spun in wait_for_flushing for a band whose last piece never came
 * back, and the voice task and the musegadgets worker on its core starved
 * until the task watchdog fired). LVGL waits for a band while s_band_busy,
 * on s_band_done, a semaphore with a timeout (flush_wait, wait_band,
 * lv_display_set_flush_wait_cb), never in its busy loop; the send task waits
 * for each piece at most PIECE_WAIT_MS, then gives the band up and puts the
 * counts back. Both log what they saw, count, and ask LVGL to draw the whole
 * screen again.
 */
#define PIECE_WAIT_MS 500     /* a piece is a few ms on the wire; a band's write is held at most ~2 refreshes */
#define FLUSH_WAIT_MS 1000    /* a band: its pieces, its TE hold, a piece's recovery */
#define PIECE_SETTLE_MS 200   /* a piece given up: late dones land in this, inside a band's FLUSH_WAIT_MS */
_Static_assert(FLUSH_WAIT_MS == 10 * 100, "wait_band: ten looks, 100 ms apart");
static SemaphoreHandle_t s_band_done;    /* given once the band LVGL queued has gone (or was given up) */
static volatile bool s_band_busy;        /* queued and not yet gone */
static const char *volatile s_send_at = "idle";   /* where the send task is, for a timeout's log */
static volatile int64_t s_send_at_us;
static volatile uint32_t s_lost_pieces, s_lost_flushes;
static volatile bool s_redraw;           /* a band was lost: draw the whole screen again */
#define SEND_AT(where) do { s_send_at = (where); s_send_at_us = esp_timer_get_time(); } while (0)

/* Tearing effect (muse_lcd_bands_set_te): the last scan's start, the period, the blanking. */
static bool s_te_on;
static int s_te_gpio = -1;
static volatile int64_t s_te_rise, s_te_fall, s_te_period, s_te_blank;

/* Writes timed against the scan, for the stats (te_hold, te_done). */
typedef struct {
    const char *what;
    uint32_t n, held, torn;
    uint32_t never, late;   /* window groups: no clear start within two refreshes; still not clear after the tries */
    int64_t sum_us;
} te_stats_t;
static te_stats_t s_te_ui = { .what = "UI band" }, s_te_video = { .what = "video frame" };
#if CONFIG_MUSE_VIDEO   /* windows (muse_lcd_bands_windows) are video's only */
static te_stats_t s_te_win = { .what = "video window group" };
#endif
/* Every write to the panel (muse_lcd_bands_writes): LVGL's bands and everything run on the send task. */
static volatile uint32_t s_writes;
static int s_rows;                       /* the panel's */
/* A band's write, learned from the ones before: about s_fixed_us + bytes *
 * s_ns_per_kb / 1024000 (a smoothed least-squares line over the size). */
static float s_mean_kb, s_mean_us, s_var_kb, s_cov;
static int64_t s_fixed_us, s_ns_per_kb;
static volatile int64_t s_band_end;      /* when the last band's last piece went */
static volatile int64_t s_piece_end;     /* when the last piece of anything went (windows' write times) */
static int64_t s_band_start, s_band_fall, s_band_p, s_band_scan;
static int s_band_y1, s_band_y2;
static size_t s_band_bytes;

/* A piece has gone, or failed to; true if it was the band's last. */
static bool IRAM_ATTR chunk_done(void)
{
    portENTER_CRITICAL_SAFE(&s_lock);
    bool last = !s_settling && s_chunks_out > 0 && --s_chunks_out == 0;
    portEXIT_CRITICAL_SAFE(&s_lock);
    return last;
}

static bool IRAM_ATTR on_chunk_sent(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *edata, void *ctx)
{
    (void)io;
    (void)edata;
    (void)ctx;
    BaseType_t woken = pdFALSE;
    s_piece_end = esp_timer_get_time();
    /* A piece piece_lost gave up: its buffer is free now, but it is no
     * piece of what is being sent. */
    portENTER_CRITICAL_SAFE(&s_lock);
    bool late = s_owed > 0;
    if (late) {
        s_owed--;
    }
    portEXIT_CRITICAL_SAFE(&s_lock);
    xSemaphoreGiveFromISR(s_chunk_free, &woken);
    bool last = !late && chunk_done();
    if (last) {
        s_band_end = esp_timer_get_time();
        s_band_busy = false;
        xSemaphoreGiveFromISR(s_band_done, &woken);   /* wait_band: LVGL's band is done */
    }
    bool yield = last && esp_lv_adapter_display_notify_color_trans_done_from_isr(s_disp);
    /* esp_lcd's SPI IO ignores this callback's return value, so the send
     * task, woken here, would wait for the next tick (up to 1 ms a piece,
     * 58 pieces a frame): ask for the switch at the interrupt's exit. */
    if (yield || woken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
    return yield || woken == pdTRUE;
}

/*
 * Waits until no piece is on the wire, holding `held` of the two buffers.
 *
 * Every call into the panel IO acquires the SPI bus lock. If that happens
 * while a queued piece is still going out, the driver hands the lock over
 * from its interrupt when the piece is done; once in tens of thousands of
 * frames that hand-over is lost (ESP-IDF 6.0.1 spi_bus_lock: the device's
 * lock bit set, no acquiring device, the bus idle, the waiter never woken),
 * and the send task, the video and the UI hang for good. Acquiring only
 * with nothing in flight never takes that path. It costs the gap between
 * pieces (the next one is filled while the last is on the wire either way).
 */
/*
 * LVGL's band is given up (its last piece failed to go out, or a piece was
 * lost), on the send task. LVGL's task can take the core at any point here
 * when it is running above this one (see wait_band), so LVGL's flushing flag
 * and s_band_busy are cleared together: between the two it could queue its
 * next band, which the second would then mark as gone while it is still
 * queued. The give comes after, and only wakes wait_band; one LVGL didn't
 * wait for just makes wait_band look at s_band_busy again.
 */
static void band_failed(void)
{
    portENTER_CRITICAL(&s_lock);
    if (s_disp) {
        lv_display_flush_ready(s_disp);
    }
    s_band_busy = false;
    portEXIT_CRITICAL(&s_lock);
    xSemaphoreGive(s_band_done);
}

/*
 * A piece's done never came back (the interrupt lost, or the bus wedged):
 * whatever was on the wire is given up, nothing is counted in flight, LVGL's
 * band (if one was being sent) is done, and the whole screen is drawn again.
 * For PIECE_SETTLE_MS first nothing new goes out and a done that turns up
 * late counts for nothing. The pieces still out after that are owed: their
 * buffers come back only with their dones, which count for nothing either,
 * so a late one never ends the next band early or frees a buffer refilled
 * for it. take_chunk writes them off when a buffer is still missing a whole
 * PIECE_WAIT_MS later (as does a second loss while some are owed).
 * held: the buffers the caller holds; it gives them up (they count as free).
 */
static void piece_lost(const char *where, int held)
{
    uint32_t n = ++s_lost_pieces;
    portENTER_CRITICAL(&s_lock);
    s_settling = true;
    portEXIT_CRITICAL(&s_lock);
    vTaskDelay(pdMS_TO_TICKS(PIECE_SETTLE_MS));
    portENTER_CRITICAL(&s_lock);
    int out = s_chunks_out;
    s_chunks_out = 0;
    s_settling = false;
    UBaseType_t free = uxSemaphoreGetCount(s_chunk_free);
    int owed = 0;
    if (s_owed == 0) {
        owed = 2 - held - (int)free;
        owed = owed < 0 ? 0 : owed;
    }
    s_owed = owed;
    portEXIT_CRITICAL(&s_lock);
    while (uxSemaphoreGetCount(s_chunk_free) + owed < 2 && xSemaphoreGive(s_chunk_free) == pdTRUE) {
    }
    s_redraw = true;
    s_writes++;   /* the panel holds part of something: video's next picture goes whole */
    if (n <= 3 || n % 50 == 0) {
        ESP_LOGE(TAG, "a piece's done never came back (%s, %d ms): %d in flight, %u of 2 buffers free, %d owed; given "
                 "up and redrawn (%u so far)", where, PIECE_WAIT_MS, out, (unsigned)free, owed, (unsigned)n);
    }
    if (s_band_out) {
        band_failed();
    }
}

/* A free buffer, waiting at most PIECE_WAIT_MS; false: it never came (piece_lost has run). held: the buffers the
 * caller holds already. */
static bool take_chunk(const char *where, int held)
{
    if (xSemaphoreTake(s_chunk_free, pdMS_TO_TICKS(PIECE_WAIT_MS)) == pdTRUE) {
        return true;
    }
    /* Only a piece given up earlier still out: its done isn't coming. Write
     * it off, its buffer free again, and go on. */
    portENTER_CRITICAL(&s_lock);
    int owed = s_owed;
    s_owed = 0;
    portEXIT_CRITICAL(&s_lock);
    for (int i = 0; i < owed; i++) {
        xSemaphoreGive(s_chunk_free);
    }
    if (owed && xSemaphoreTake(s_chunk_free, 0) == pdTRUE) {
        ESP_LOGW(TAG, "%d piece(s) given up earlier never came back: written off", owed);
        return true;
    }
    piece_lost(where, held);
    return false;
}

/* False: a piece never came back (piece_lost has run; the caller gives up). */
static bool wire_idle(int held)
{
    int got = 0;
    for (int i = held; i < 2; i++) {
        if (!take_chunk("waiting for the wire to go idle", held + got)) {
            return false;   /* both buffers are free again */
        }
        got++;
    }
    for (int i = 0; i < got; i++) {
        xSemaphoreGive(s_chunk_free);
    }
    return true;
}

static esp_err_t queue_band(lv_display_t *disp, esp_lcd_panel_handle_t panel, int x1, int y1, int x2, int y2,
                            const void *data, void *ctx)
{
    (void)disp;
    (void)panel;
    (void)ctx;
    band_t b = { data, x1, y1, x2, y2 };
    xSemaphoreTake(s_band_done, 0);   /* left by a band that had gone (or was given up) before LVGL waited */
    s_band_busy = true;
    if (xQueueSend(s_bands, &b, 0) != pdTRUE) {
        /* The send task is behind (or stuck): the adapter ends this flush
         * (flush_ready) on the error; wait_band mustn't wait for it. */
        s_band_busy = false;
        return ESP_FAIL;
    }
    return ESP_OK;
}

static void redraw_all(void *arg)
{
    (void)arg;
    lv_obj_invalidate(lv_screen_active());
    lv_obj_invalidate(lv_layer_top());
    lv_obj_invalidate(lv_layer_sys());
}

/*
 * LVGL's wait for a band to go, in place of its own, which spins. LVGL's task
 * can be running above the send task when it gets here: it holds two mutexes
 * while it draws, and once a higher task has waited for either, FreeRTOS keeps
 * it at that task's priority until it has let go of both. Spinning there, it
 * would keep the send task off the core for good, with the band still queued.
 * Each take's timeout is only a second look at s_band_busy; false after ten
 * of them (FLUSH_WAIT_MS) with the band still queued.
 */
static bool wait_band(lv_display_t *disp)
{
    (void)disp;
    for (int looks = 0; s_band_busy; looks++) {
        if (looks == 10) {
            return false;
        }
        xSemaphoreTake(s_band_done, pdMS_TO_TICKS(100));
    }
    return true;
}

/*
 * LVGL's wait for the band it sent (lv_display_set_flush_wait_cb), on the
 * LVGL task, holding its lock: wait_band, never a busy loop. On a timeout
 * LVGL goes on (it clears its flushing flag) and the screen is drawn again
 * once the send task is back.
 */
static void flush_wait(lv_display_t *disp)
{
    if (!wait_band(disp)) {
        uint32_t n = ++s_lost_flushes;
        s_redraw = true;
        if (n <= 3 || n % 50 == 0) {
            int64_t ms = (esp_timer_get_time() - s_send_at_us) / 1000;
            /* LVGL's own priority now: above its base (MUSE_UI_PRIORITY) means
             * a higher task waits on a lock it holds. */
            ESP_LOGE(TAG, "a band's flush never finished in %d ms: the send task is %s for %lld ms, %d pieces in flight, "
                     "%u of 2 buffers free; LVGL at priority %u (%u so far)", FLUSH_WAIT_MS, (const char *)s_send_at,
                     (long long)ms, s_chunks_out, (unsigned)uxSemaphoreGetCount(s_chunk_free),
                     (unsigned)uxTaskPriorityGet(NULL), (unsigned)n);
        }
    }
    if (s_redraw) {
        s_redraw = false;
        lv_async_call(redraw_all, NULL);   /* after this refresh: the whole screen once more */
    }
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

static void te_log(const te_stats_t *st, int64_t p, int64_t scan);
#if CONFIG_MUSE_VIDEO
static int s_win_ratio = 256;   /* a group of windows' write over its estimate (Q8), learned (send_windows) */
#endif

/* After the write of rows [y1, y2) that started at t0 and took w, the scans
 * then as te_now gave them: did one meet it? */
static void te_done(te_stats_t *st, int y1, int y2, int64_t t0, int64_t w, int64_t fall, int64_t p, int64_t scan)
{
    st->sum_us += w;
    st->torn += te_meets(t0, w, y1, y2, fall, p, scan);
    if (++st->n % 1000 == 1) {
        te_log(st, p, scan);
    }
}

#if CONFIG_MUSE_VIDEO
static bool te_meets_path(const int *r, const int64_t *t, int m, int64_t fall, int64_t p, int64_t scan);

/* te_done for a group of windows (send_windows): its rows r[0..m] and the times its write was at each. */
static void te_done_path(te_stats_t *st, const int *r, const int64_t *t, int m, int64_t fall, int64_t p, int64_t scan)
{
    st->sum_us += t[m] - t[0];
    st->torn += te_meets_path(r, t, m, fall, p, scan);
    if (++st->n % 1000 == 1) {
        te_log(st, p, scan);
    }
}

/* A group's write took us against raw (the bands' line's estimate): the ratio, learned. */
static void win_ratio(int64_t us, int64_t raw)
{
    if (us > 0 && raw > 0) {
        int64_t r = us * 256 / raw;
        r = r < 128 ? 128 : r > 1024 ? 1024 : r;
        s_win_ratio = (int)((s_win_ratio * 7 + r) / 8);
    }
}
#endif

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
/* One band LVGL sent: its pieces, each copied while the one before is on
 * the wire. A piece that never comes back gives the rest of the band up. */
static void send_band_pieces(const band_t *b, int *k);
static void send_band(const band_t *b, int *k)
{
    s_band_out = true;   /* a lost piece fails this band (piece_lost) */
    send_band_pieces(b, k);
    s_band_out = false;
}

static void send_band_pieces(const band_t *b, int *k)
{
    size_t row = (size_t)(b->x2 - b->x1) * 2;
    s_writes++;
    SEND_AT("holding a band for the scan");
    te_band(b->y1, b->y2, row * (size_t)(b->y2 - b->y1));
    int rows = s_chunk_bytes / row & ~1;
    /* LVGL sends no more until this band is done, so nothing is still
     * counting down. */
    s_chunks_out = (b->y2 - b->y1 + rows - 1) / rows;
    for (int y = b->y1; y < b->y2; y += rows) {
        int h = b->y2 - y < rows ? b->y2 - y : rows;
        SEND_AT("waiting for a free buffer");
        if (!take_chunk("a band's piece", 0)) {
            return;
        }
        memcpy(s_chunk[*k], b->data + (size_t)(y - b->y1) * row, h * row);
        if (!wire_idle(1)) {
            return;
        }
        SEND_AT("in esp_lcd_panel_draw_bitmap");
        esp_err_t err = esp_lcd_panel_draw_bitmap(s_panel, b->x1, y, b->x2, y + h, s_chunk[*k]);
        if (err != ESP_OK) {
            xSemaphoreGive(s_chunk_free);
            if (chunk_done()) {
                band_failed();
            }
        }
        *k ^= 1;
    }
}

/*
 * The send task's stack. Everything run on the panel runs here: LVGL's bands, a video frame or windows with their
 * fill (the conversion, the subtitles, the call controls and stats overlays, the ring and the dot, the shadow
 * compare's model) and the tearing line's log. -fstack-usage on the S3: send_bands 144 + send_windows 304
 * + fill_win 112 + the controls overlay 160 + muse_call_rrect 176 = 896 B to the deepest draw; a log line from
 * there (esp_log 96, the Nordic UART mirror's nus_vprintf 224, diagnostic_log_vprintf 608, vsnprintf 480) is
 * about 1.4 KB more, and an interrupt's frame on top. 2560 overflowed (the W175, a call's first controls draw
 * over windowed video logged from inside the fill); 4096 leaves over 25 % past that worst case. No
 * draw logs from the fill now (muse_call's shown_down_log only notes). The 2 s av video line reports what's left
 * (spanel=, muse_lcd_bands_stack_free).
 */
#define LCD_SEND_STACK 4096
static TaskHandle_t s_send_task;

uint32_t muse_lcd_bands_stack_free(void)
{
    return s_send_task ? (uint32_t)uxTaskGetStackHighWaterMark(s_send_task) : 0;
}

static void send_bands(void *arg)
{
    (void)arg;
    int k = 0;
    band_t b;
    for (;;) {
        SEND_AT("idle");
        xQueueReceive(s_bands, &b, portMAX_DELAY);
        if (!b.data) {
            SEND_AT("running a job");
            if (wire_idle(0)) {
                s_call(s_call_arg);
            }
            xSemaphoreGive(s_call_done);
            continue;
        }
        send_band(&b, &k);
    }
}

lv_display_t *muse_lcd_bands_register(esp_lv_adapter_display_config_t cfg, int lines, size_t chunk_bytes)
{
    s_panel = cfg.panel;
    s_io = cfg.panel_io;
    s_chunk_bytes = chunk_bytes;
    if (xTaskGetCoreID(xTaskGetCurrentTaskHandle()) != MUSE_UI_CORE) {
        ESP_LOGE(TAG, "not pinned to core %d: the panel's SPI interrupt can strand the send task", MUSE_UI_CORE);
    }
    s_bands = xQueueCreate(2, sizeof(band_t));
    s_call_lock = xSemaphoreCreateMutex();
    s_call_done = xSemaphoreCreateBinary();
    s_chunk_free = xSemaphoreCreateCounting(2, 2);
    s_band_done = xSemaphoreCreateBinary();
    /* 16-byte aligned: video fills them with the S3's SIMD stores (muse_lcd_bands_frame). */
    s_chunk[0] = heap_caps_aligned_alloc(16, chunk_bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    s_chunk[1] = heap_caps_aligned_alloc(16, chunk_bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!s_bands || !s_call_lock || !s_call_done || !s_chunk_free || !s_band_done || !s_chunk[0] || !s_chunk[1] ||
        xTaskCreatePinnedToCore(send_bands, "lcd_send", LCD_SEND_STACK, NULL, MUSE_UI_PRIORITY + 1, &s_send_task,
                                MUSE_UI_CORE) != pdPASS) {
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
    lv_display_set_flush_wait_cb(s_disp, flush_wait);   /* never LVGL's busy loop */
    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565_SWAPPED);
    const esp_lv_adapter_draw_bitmap_callbacks_t draw_cbs = { .custom_draw_bitmap = queue_band };
    esp_lv_adapter_set_draw_bitmap_callbacks(s_disp, &draw_cbs, NULL);
    return s_disp;
}

void muse_lcd_bands_run(void (*fn)(void *arg), void *arg)
{
    /* Anything run on the panel may change what it shows (a frame, a still,
     * a command that sleeps or wakes it): counted as a write. */
    s_writes++;
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

typedef struct {
    int x1, y1, x2, y2, rows;
    muse_lcd_fill_fn fill;
    void *arg;
} frame_job_t;

/* On the send task: each piece is filled straight into a free DMA buffer
 * while the one before is on the wire. */
void muse_lcd_bands_set_gap(int x, int y)
{
    s_gap_x = x;
    s_gap_y = y;
}

/* QSPI framing of the CO5300 family (and most QSPI AMOLED controllers):
 * opcode 0x02 for a command, 0x32 for pixels on four lines. */
#define QSPI_CMD(c) ((0x02 << 24) | ((c) << 8))
#define QSPI_PIXELS(c) ((0x32 << 24) | ((c) << 8))

/*
 * On the send task: one window for the whole frame, then the pieces as
 * Memory Write and Memory Write Continue (2Ch, 3Ch), each filled straight
 * into a free DMA buffer while the one before is on the wire. Setting the
 * window per piece (as esp_lcd_panel_draw_bitmap does) costs two polled
 * commands that wait for the bus each time: about a third of the frame.
 */
static void send_frame(void *arg)
{
    const frame_job_t *j = arg;
    /* Let LVGL's last band finish first, so its count of pieces in flight
     * (chunk_done) never sees ours. */
    if (!wire_idle(0)) {
        return;
    }
    /* A whole frame is slower to write than the panel scans (26 ms on the
     * 1.75C): it stays clear started within a few ms of a scan's start. */
    static int64_t avg;
    int64_t t_hold = esp_timer_get_time();
    if (s_te_on && avg) {
        te_hold(&s_te_video, j->y1, j->y2, avg, 3);   /* steady: the same size each time */
    }
    int64_t fall = 0, p = 0, scan = 0;
    bool timed = te_now(&fall, &p, &scan);
    int64_t t_start = esp_timer_get_time();
    size_t row = (size_t)(j->x2 - j->x1) * 2;
    int rows = j->rows;
    if ((size_t)rows * row > s_chunk_bytes) {
        rows = (int)(s_chunk_bytes / row) & ~1;
    }
    int x1 = j->x1 + s_gap_x, x2 = j->x2 - 1 + s_gap_x, y1 = j->y1 + s_gap_y, y2 = j->y2 - 1 + s_gap_y;
    /* The mic ring as it is now, for the whole picture: a capture starting or
     * stopping mid-picture left it on only part of the screen (the rows
     * after the change) until the next picture, which on a still stream might
     * not come for a while (muse_video draws the last one again then). */
    bool ring = muse_mic_ring_on();
    bool ok = esp_lcd_panel_io_tx_param(s_io, QSPI_CMD(0x2A), (uint8_t[]){ x1 >> 8, x1, x2 >> 8, x2 }, 4) == ESP_OK &&
              esp_lcd_panel_io_tx_param(s_io, QSPI_CMD(0x2B), (uint8_t[]){ y1 >> 8, y1, y2 >> 8, y2 }, 4) == ESP_OK;
    int k = 0;
    for (int y = j->y1; ok && y < j->y2; y += rows) {
        int h = j->y2 - y < rows ? j->y2 - y : rows;
        if (!take_chunk("a frame's piece", 0)) {
            return;
        }
        j->fill(s_chunk[k], (int)row, y - j->y1, h, j->arg);
        /* The mic ring over every picture drawn straight to the panel. */
        muse_mic_ring_apply_as(ring, s_chunk[k], (int)row, j->x1, y, j->x2 - j->x1, h);
        /* ...and the turn's status dot, the same way. */
        muse_turn_dot_apply(s_chunk[k], (int)row, j->x1, y, j->x2 - j->x1, h);
#if CONFIG_MUSE_VIDEO
        muse_video_piece_sent(s_chunk[k], (int)row, j->x1, y, j->x2 - j->x1, h);   /* the shadow compare's model */
#endif
        if (!wire_idle(1)) {
            return;
        }
        if (esp_lcd_panel_io_tx_color(s_io, QSPI_PIXELS(y == j->y1 ? 0x2C : 0x3C), s_chunk[k], (size_t)h * row) !=
            ESP_OK) {
            xSemaphoreGive(s_chunk_free);
            ok = false;
        }
        k ^= 1;
    }
    /* Both buffers back: the caller's picture is no longer read. */
    if (!wire_idle(0)) {
        return;
    }
    int64_t w = esp_timer_get_time() - t_start;
    avg = avg ? (avg * 7 + (w > avg ? 2 * w - avg : w)) / 8 : w;   /* quick to rise */
#if CONFIG_MUSE_VIDEO
    muse_video_panel_timing((uint32_t)(t_start - t_hold), (uint32_t)w);   /* the vdirty: line's TE wait and write */
#else
    (void)t_hold;
#endif
    if (timed) {
        te_done(&s_te_video, j->y1, j->y2, t_start, w, fall, p, scan);
    }
}

#if CONFIG_MUSE_VIDEO
typedef struct {
    int x0, y0, n;
    const muse_board_win_t *win;
    muse_lcd_fill_win_fn fill;
    void *arg;
} windows_job_t;

/*
 * Windows are timed against the scan a group at a time: windows that touch
 * (each starts on the row the last one ended) are one group, written back to
 * back. Its rows don't go at one pace (a narrow window's rows go faster than
 * a wide one's), so its write is a path: each window's top row at the time
 * its write starts, then the last one's bottom at the end, straight between.
 * The scan crosses the write where the write is ahead of the scan at one of
 * those rows and behind it at another (te_meets is the one-window case).
 * Windows apart have only unchanged rows between them, so a scan between
 * two groups shows no seam.
 */
#define GROUP_MAX 32
static int s_gr[GROUP_MAX + 1];                 /* the group's rows: each window's top, then the last bottom */
static int64_t s_go[GROUP_MAX + 1];             /* the write's estimated time at each, from its start */
static int64_t s_gt[GROUP_MAX + 1];             /* ...and when it was there */
static int64_t s_gpath[GROUP_MAX + 1];          /* te_hold_path's trial times */

static bool te_meets_path(const int *r, const int64_t *t, int m, int64_t fall, int64_t p, int64_t scan)
{
    for (int k = -1; k <= 3; k++) {
        int64_t f = fall + k * p;
        bool ahead0 = t[0] < f + scan * r[0] / s_rows;
        for (int i = 1; i <= m; i++) {
            if ((t[i] < f + scan * r[i] / s_rows) != ahead0) {
                return true;
            }
        }
    }
    return false;
}
#endif

static void te_log(const te_stats_t *st, int64_t p, int64_t scan)
{
    ESP_LOGI(TAG, "tearing effect: refresh %d us (blank %d us); %s %d us avg (%d us + %d us/KB), %u of %u held "
             "for the scan, %u met it", (int)p, (int)(p - scan), st->what, (int)(st->sum_us / st->n),
             (int)s_fixed_us, (int)(s_ns_per_kb / 1000), (unsigned)st->held, (unsigned)st->n, (unsigned)st->torn);
#if CONFIG_MUSE_VIDEO
    if (st == &s_te_win) {
        ESP_LOGI(TAG, "tearing effect: window groups never clear %u, still not clear when written %u, write %d%% of "
                 "the estimate", (unsigned)st->never, (unsigned)st->late, s_win_ratio * 100 / 256);
    }
#endif
}

#if CONFIG_MUSE_VIDEO
/* te_hold for a path of m windows: rows r[0..m], estimated times o[0..m] from its start (o[0] 0).
 * Checked again after every wait, the last one included; under 1.5 ms is waited out exactly (a tick's
 * wait may end up to a millisecond late). */
static void te_hold_path(te_stats_t *st, const int *r, const int64_t *o, int m, int spread)
{
    int64_t whole = o[m] > 0 ? o[m] : 1;
    for (int tries = 0; tries < 6; tries++) {
        int64_t fall, p, scan, now = esp_timer_get_time();
        if (!te_now(&fall, &p, &scan)) {
            return;
        }
        /* The first start clear for the write anywhere in +-spread; failing
         * that (a long group), in a third of it, rather than none. */
        int64_t d = 2 * p;
        for (int sp = spread; d >= 2 * p && sp > 0; sp = sp > 3 ? sp / 3 : 0) {
            for (d = 0; d < 2 * p; d += 500) {
                bool meets = false;
                for (int v = 0; v < 3 && !meets; v++) {
                    int pct = v == 0 ? 100 - sp : v == 1 ? 100 : 100 + sp;
                    for (int i = 0; i <= m; i++) {
                        s_gpath[i] = now + d - 300 + o[i] * pct / 100 + 400 * o[i] / whole;
                    }
                    meets = te_meets_path(r, s_gpath, m, fall, p, scan);
                }
                if (!meets) {
                    break;
                }
            }
        }
        if (d == 0) {
            return;   /* clear now */
        }
        if (d >= 2 * p) {
            st->never++;
            return;
        }
        if (tries == 5) {
            st->late++;
            return;
        }
        if (d < 1500) {
            esp_rom_delay_us((uint32_t)d);
        } else {
            vTaskDelay(pdMS_TO_TICKS((d - 500) / 1000));   /* a little early, then the rest exactly */
        }
        st->held += tries == 0;
    }
}

/*
 * On the send task: each window set as its own panel window, then its pieces
 * (Memory Write, then Write Continue). A window's commands go out only with
 * nothing on the wire (wire_idle: the bus lock's lost hand-over), but its
 * first piece is filled before that, while the last window's last piece is
 * still going: the wire waits only for the commands. Each group is held
 * (te_hold_path) until its write stays clear of every scan, then checked
 * against the scans with the times it took (te_done's count, "met it").
 */
static void send_windows(void *arg)
{
    const windows_job_t *j = arg;
    if (!wire_idle(0)) {
        return;
    }
    int64_t t0 = esp_timer_get_time(), hold_us = 0;
    bool ring = muse_mic_ring_on();   /* one look for the whole picture, as a frame takes */
    int k = 0, first = 0, last = -1, m = 0;   /* the group: windows [first, last], m of them */
    int64_t g_fall = 0, g_p = 0, g_scan = 0, g_raw = 0;
    bool g_timed = false, ok = true;
    for (int i = 0; ok && i < j->n; i++) {
        const muse_board_win_t *w = &j->win[i];
        if (w->w <= 0 || w->h <= 0) {
            continue;
        }
        size_t row = (size_t)w->w * 2;
        /* Whole groups of four rows (the two times converters' source row pairs start on them). */
        int rows = (int)(s_chunk_bytes / row) & ~3;
        if (rows < 4) {
            break;   /* wider than a buffer's four rows: never (a panel row fits many times) */
        }
        rows = rows > w->h ? w->h : rows;
        int px = j->x0 + w->x, py = j->y0 + w->y;
        for (int y = 0; ok && y < w->h; y += rows) {
            int h = w->h - y < rows ? w->h - y : rows;
            if (!take_chunk("a window's piece", 0)) {
                return;
            }
            j->fill(s_chunk[k], (int)row, w->x, w->y + y, w->w, h, j->arg);
            muse_mic_ring_apply_as(ring, s_chunk[k], (int)row, px, py + y, w->w, h);
            muse_turn_dot_apply(s_chunk[k], (int)row, px, py + y, w->w, h);
#if CONFIG_MUSE_VIDEO
            muse_video_piece_sent(s_chunk[k], (int)row, px, py + y, w->w, h);   /* the shadow compare's model */
#endif
            if (!wire_idle(1)) {
                return;
            }
            if (y == 0 && i > last) {
                /* A new group; the last one's write is over (s_piece_end:
                 * its last piece went). */
                if (last >= 0) {
                    s_gt[m] = s_piece_end;
                    if (g_timed) {
                        te_done_path(&s_te_win, s_gr, s_gt, m, g_fall, g_p, g_scan);
                    }
                    win_ratio(s_gt[m] - s_gt[0], g_raw);
                }
                first = last = i;
                while (last + 1 < j->n && last + 1 - first < GROUP_MAX && j->win[last + 1].h > 0 &&
                       j->win[last + 1].y == j->win[last].y + j->win[last].h) {
                    last++;
                }
                m = last - first + 1;
                g_raw = 0;
                for (int q = 0; q < m; q++) {
                    const muse_board_win_t *v = &j->win[first + q];
                    s_gr[q] = j->y0 + v->y;
                    s_go[q] = g_raw * s_win_ratio / 256;
                    g_raw += s_fixed_us + (int64_t)v->w * v->h * 2 * s_ns_per_kb / 1024 / 1000;
                }
                s_gr[m] = j->y0 + j->win[last].y + j->win[last].h;
                s_go[m] = g_raw * s_win_ratio / 256;
                if (s_te_on && s_ns_per_kb) {
                    int64_t h0 = esp_timer_get_time();
                    te_hold_path(&s_te_win, s_gr, s_go, m, 15);
                    hold_us += esp_timer_get_time() - h0;
                }
                g_timed = te_now(&g_fall, &g_p, &g_scan);
            }
            if (y == 0) {
                s_gt[i - first] = esp_timer_get_time();   /* (its top row goes with its first piece) */
                int x1 = px + s_gap_x, x2 = px + w->w - 1 + s_gap_x, y1 = py + s_gap_y, y2 = py + w->h - 1 + s_gap_y;
                ok = esp_lcd_panel_io_tx_param(s_io, QSPI_CMD(0x2A), (uint8_t[]){ x1 >> 8, x1, x2 >> 8, x2 }, 4) ==
                         ESP_OK &&
                     esp_lcd_panel_io_tx_param(s_io, QSPI_CMD(0x2B), (uint8_t[]){ y1 >> 8, y1, y2 >> 8, y2 }, 4) ==
                         ESP_OK;
                if (!ok) {
                    xSemaphoreGive(s_chunk_free);
                    break;
                }
            }
            if (esp_lcd_panel_io_tx_color(s_io, QSPI_PIXELS(y == 0 ? 0x2C : 0x3C), s_chunk[k], (size_t)h * row) !=
                ESP_OK) {
                xSemaphoreGive(s_chunk_free);
                ok = false;
            }
            k ^= 1;
        }
    }
    if (!wire_idle(0)) {
        return;
    }
    if (ok && last >= 0) {
        s_gt[m] = s_piece_end;
        if (g_timed) {
            te_done_path(&s_te_win, s_gr, s_gt, m, g_fall, g_p, g_scan);
        }
        win_ratio(s_gt[m] - s_gt[0], g_raw);
    }
#if CONFIG_MUSE_VIDEO
    muse_video_panel_timing((uint32_t)hold_us, (uint32_t)(esp_timer_get_time() - t0 - hold_us));
#else
    (void)t0, (void)hold_us;
#endif
}

void muse_lcd_bands_windows(int x0, int y0, const muse_board_win_t *win, int n, int rows, muse_lcd_fill_win_fn fill,
                            void *arg)
{
    (void)rows;   /* pieces are as many of a window's rows as a buffer holds, in fours */
    if (n <= 0) {
        return;
    }
    windows_job_t j = { x0, y0, n, win, fill, arg };
    muse_lcd_bands_run(send_windows, &j);
}
#else
/* Without video nothing draws windows (muse_video.c is draw_windows' only caller). */
void muse_lcd_bands_windows(int x0, int y0, const muse_board_win_t *win, int n, int rows, muse_lcd_fill_win_fn fill,
                            void *arg)
{
    (void)x0, (void)y0, (void)win, (void)n, (void)rows, (void)fill, (void)arg;
}
#endif

uint32_t muse_lcd_bands_writes(void)
{
    return s_writes;
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

void muse_lcd_bands_frame(int x1, int y1, int x2, int y2, int rows, muse_lcd_fill_fn fill, void *arg)
{
    frame_job_t j = { x1, y1, x2, y2, rows, fill, arg };
    muse_lcd_bands_run(send_frame, &j);
}
