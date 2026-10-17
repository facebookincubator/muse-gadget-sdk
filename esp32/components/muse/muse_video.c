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
#include "muse_video.h"

#include <string.h>

#include "esp_debug_helpers.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "h264bsd_decoder.h"
#if CONFIG_MUSE_VIDEO_SIMD_MC
#include "h264bsd_reconstruct.h"   /* h264bsdPieState() */
#endif
#include "muse_board.h"
#include "muse_mem.h"
#include "muse_state.h"
#include "muse_yuv.h"

static const char *TAG = "muse_video";

#if CONFIG_IDF_TARGET_ESP32S3
#define MAX_SRC 240                    /* 2x upscaled, up to 480 px */
#else
/* The S31 (ESP-Mosaico) decodes up to the panel's own 480 px, shown one to
 * one; anything up to half the panel is shown two times upscaled. */
#define MAX_SRC 480
#endif
/* The decoder's deepest path (an inter block through the interpolation, with
 * the S31's SIMD scratch, or tinyh264's first-use check) takes about 4 KB. */
#if CONFIG_IDF_TARGET_ESP32S31
#define VIDEO_DEC_STACK 8192
#else
#define VIDEO_DEC_STACK 6144
#endif
/* Bands decoded in parallel (muse_video_params_t.tiles): one per core,
 * where the picture is big enough to need both. */
#define MAX_BANDS (MAX_SRC > 240 ? 2 : 1)
#define TILE_SPLIT_PCT 60
#define AU_MAX (64 * 1024)
#define SHOW_LEAD_US 8000            /* paced pictures go to the panel this early (show_task) */
#define DECODE_CORE 0                  /* the UI core converts and sends */
#define KEYFRAME_ASK_MS 300
#define PIECE_ROWS 8                   /* output rows per DMA piece: two source row pairs */
#define MAX_PLAYOUT_MS 500             /* the ring holds well over this at the suggested bitrate */
#define MAX_TRANSPORTS 2

/*
 * A picture on its way to the panel. The decoder hands it over before it
 * decodes it and reports rows as they become final (h264bsdSetProgress),
 * so the panel draws the top of the picture while the bottom is still being
 * decoded on the other core. Two slots: picture N shows while N + 1 decodes,
 * and N + 2 waits for N's slot (the decoder's buffer for it).
 */
typedef struct {
    const uint8_t *volatile pic;
    volatile int w, h;                 /* the decoder's picture, pixels (macroblock aligned); the whole picture with bands */
    volatile int rows;                 /* luma rows that are final */
    volatile bool done, failed;
    int64_t t_whole;                   /* when its last byte was in (popped from the ring) */
    /* With a playout delay: when it shows. The show task waits for the
     * whole picture and this time, so the decoder can work ahead (pace). */
    volatile int64_t due;
    /* With two bands (muse_video_params_t.tiles), pic and rows are the
     * top band's and these the bottom's, decoded on the other core. */
    const uint8_t *volatile pic1;
    volatile int rows1;
    int bands;
    volatile int h0;                   /* two bands: the top band's rows (each band's SPS gives its own) */
    uint16_t number;                   /* its frame number (the header's), echoed to the client once shown */
} slot_t;

/* The bottom band's decoder, on the UI core (it waits on the panel most of
 * the time): one job per frame from the decode task. */
typedef struct {
    const uint8_t *au;
    uint32_t len;
    slot_t *sl;
    uint32_t gen;
    volatile bool ok, got;
    volatile uint32_t us;              /* its decode time */
    volatile int h;                    /* its picture's rows */
} band_job_t;
static band_job_t s_band;
/* Playout pacing (pace_due()): waits and late frames, for the stats. */
static uint32_t s_pace_waits_us, s_pace_late, s_late_in, s_slot_wait_us;
/* Two bands: each band's decode time, for the stats (which core is the long pole). */
static uint32_t s_top_us, s_bot_us, s_band_n;
static TaskHandle_t s_band_task;
/* The band task's "done": a semaphore of its own, since the decode task's
 * notification also means "frames arrived" (the transport wakes it). */
static SemaphoreHandle_t s_band_done;

typedef struct {
    slot_t *s;
    int vis_w, vis_h;
    int scale;                         /* 2: two times; 1: one to one; 3: 3/2 (320 -> 480); 0: out_w x out_h */
    int out_w, out_h;
} fill_arg_t;

static muse_video_caps_t s_caps;
static const muse_video_transport_t *s_transports[MAX_TRANSPORTS];
static int s_ntransports;
/* The transport whose stream is on (or was last); set by begin. */
static const muse_video_transport_t *volatile s_tp;
/* The last picture drawn: its frame number and when (for the latency echo). */
static volatile uint16_t s_last_number;
static volatile int64_t s_last_shown_t;
static volatile bool s_open, s_streaming, s_redraw, s_paused;
static volatile bool s_ui_up;          /* the UI's frame tick has run: the panel and its sender exist */
static volatile bool s_wake_req;       /* begin() found the screen dark: the show task wakes it */
static volatile int s_refusals;        /* begin() refused in a row: a stuck UI shows up here */
static volatile int s_show_stage;      /* where the show task is, for a stuck-pipeline log */
static volatile int64_t s_stage_t;     /* when it got there */
static volatile int64_t s_slot_wait_t; /* when the decoder started waiting for a slot (0: not waiting) */
static volatile int64_t s_awake_tick;  /* the UI's last frame tick with the screen on and the view up */
static volatile uint32_t s_gen;        /* bumped by every begin */
static muse_video_params_t s_params;
static TaskHandle_t s_dec_task;
static QueueHandle_t s_frames;         /* slot indexes, decoder to show task */
static SemaphoreHandle_t s_slots;      /* free slots */
static slot_t s_slot[2];
static uint8_t *s_au;
static volatile uint16_t s_shown, s_dropped;
/* Pictures decoded but not drawn because a newer one was ready (the panel's
 * pace is a little under the stream's): see show_task. */
static volatile uint32_t s_skipped;
/* Popped to on the panel, and the draw alone, summed over s_lat_n frames (us). */
static volatile uint32_t s_lat_sum, s_draw_sum, s_lat_n;
/* Time between pictures reaching the panel: the largest, and how many took over 1.5 frame periods. */
static volatile uint32_t s_gap_max, s_gap_late;
static lv_obj_t *s_root, *s_title, *s_caption;

bool muse_video_supported(void)
{
    return muse_board && muse_board->draw_frame && s_ntransports > 0;
}

uint32_t muse_video_buffer_size(void)
{
    return CONFIG_MUSE_VIDEO_BUFFER;
}

void muse_video_add_transport(const muse_video_transport_t *t)
{
    if (t && s_ntransports < MAX_TRANSPORTS) {
        s_transports[s_ntransports++] = t;
    }
}

bool muse_video_has_client(void)
{
    for (int i = 0; i < s_ntransports; i++) {
        if (s_transports[i]->has_client && s_transports[i]->has_client()) {
            return true;
        }
    }
    return false;
}

/* What the client hears after each frame. */
static void tell_freed(uint8_t avg_ms)
{
    const muse_video_transport_t *t = s_tp;
    if (!t || !t->freed) {
        return;
    }
    muse_video_status_t st = { .shown = s_shown, .dropped = s_dropped, .decode_ms = avg_ms, .last_age_ms = 0xffff };
    int64_t shown_t = s_last_shown_t;
    if (shown_t) {
        int64_t age = (esp_timer_get_time() - shown_t) / 1000;
        st.last_number = s_last_number;
        st.last_age_ms = (uint16_t)(age < 0xfffe ? age : 0xfffe);
    }
    t->freed(&st);
}

static void ask_keyframe(void)
{
    const muse_video_transport_t *t = s_tp;
    if (t && t->keyframe) {
        t->keyframe();
    }
}

bool muse_video_is_open(void)
{
    return s_open;
}

const muse_video_caps_t *muse_video_caps(void)
{
    /* The board's static description: a transport may ask before Muse's
     * boot task (on the UI core) has set muse_board. */
    const muse_board_t *b = muse_board ? muse_board : muse_board_get();
    if (!b || !b->draw_frame) {
        return NULL;
    }
    int w = b->width, h = b->height;
    /* The S31 suggests the panel's own size (480 px, decoded in two bands);
     * the S3 half of it, two times upscaled. */
    int src = (w < h ? w : h) / (MAX_SRC > 240 ? 1 : 2) & ~7;
    if (src > MAX_SRC) {
        src = MAX_SRC;
    }
    s_caps = (muse_video_caps_t){
        .display_w = (uint16_t)w,
        .display_h = (uint16_t)h,
        .round = b->round,
        .max_w = MAX_SRC,
        .max_h = MAX_SRC,
        .max_fps = 30,
        .sug_w = (uint16_t)src,
        .sug_h = (uint16_t)src,
        .sug_fps = 30,
        /* For a link of well under 1 Mb/s (480 px at 600 kb/s still beats
         * 320 or 400 px at the same rate); a transport that carries more
         * suggests its own (muse_video_wifi.c). */
        .sug_kbps = MAX_SRC > 240 ? 600 : 450,
        .max_playout_ms = MAX_PLAYOUT_MS,
        .max_tiles = MAX_BANDS,
        /* The top band decodes on the first core, which has little else to
         * do; the bottom shares the second with the panel's sends and the
         * colour conversion. With equal bands the bottom took 28 ms a frame
         * at 480 px and the top 20: 60% on top evens them out. */
        .tile_split_pct = MAX_BANDS > 1 ? TILE_SPLIT_PCT : 0,
    };
    return &s_caps;
}

/* ---- drawing (UI core) -------------------------------------------------- */

static void fill(uint8_t *dst, int stride, int row, int rows, void *arg)
{
    fill_arg_t *a = arg;
    slot_t *sl = a->s;
    /* With two bands, the bottom half of the output comes from the bottom
     * band, as if it were a picture of its own (pieces never straddle). */
    int vis_h = a->vis_h, out_h = a->out_h;
    const uint8_t *pic = sl->pic;
    volatile int *have = &sl->rows;
    int w = sl->w, h = sl->h;
    if (sl->bands == 2) {
        /* The bands may differ in height (capability_tile_split): the top's
         * share of the output is its share of the picture. */
        int h0 = sl->h0, vis0 = vis_h * h0 / h, out0 = out_h * h0 / h;
        if (row < out0 && row + rows > out0) {
            /* A piece across the seam: each band its part. */
            int top = out0 - row;
            fill(dst, stride, row, top, arg);
            fill(dst + (size_t)top * stride, stride, out0, rows - top, arg);
            return;
        }
        if (row >= out0) {
            row -= out0;
            vis_h -= vis0;
            out_h -= out0;
            h -= h0;
            while (!sl->pic1 && !sl->done) {
                vTaskDelay(1);
            }
            pic = sl->pic1;
            have = &sl->rows1;
        } else {
            vis_h = vis0;
            out_h = out0;
            h = h0;
        }
    }
    int sr = a->scale == 1 || a->scale == 2 ? row / a->scale : 0;
    int need = a->scale == 1 || a->scale == 2 ? (row + rows) / a->scale
             : a->scale == 3 ? muse_yuv_up32_src_rows(vis_h, row + rows)
             : muse_yuv_src_rows(vis_h, out_h, row + rows);
    while (*have < need && !sl->done) {
        vTaskDelay(1);   /* the decoder is a macroblock row or so behind */
    }
    if (!pic) {
        return;
    }
    const uint8_t *y = pic + (size_t)sr * w;
    const uint8_t *u = pic + (size_t)w * h + (size_t)(sr / 2) * (w / 2);
    const uint8_t *v = u + (size_t)(w / 2) * (h / 2);
    if (a->scale == 3) {
        muse_yuv_up32(pic, pic + (size_t)w * h, pic + (size_t)w * h + (size_t)(w / 2) * (h / 2), w, w / 2,
                      a->vis_w, vis_h, row, rows, dst, stride);
    } else if (a->scale == 0) {
        muse_yuv_scaled(pic, pic + (size_t)w * h, pic + (size_t)w * h + (size_t)(w / 2) * (h / 2), w, w / 2,
                        a->vis_w, vis_h, a->out_w, out_h, row, rows, dst, stride);
    } else if (a->scale == 2) {
        muse_yuv_up2(y, u, v, w, w / 2, a->vis_w, rows / 2, dst, stride);
    } else {
        muse_yuv_1x(y, u, v, w, w / 2, a->vis_w, rows, dst, stride);
    }
}

static void pause_lvgl(bool pause)
{
    if (pause == s_paused) {
        return;
    }
    s_paused = pause;
    if (muse_board->display_pause) {
        muse_board->display_pause(pause);
    }
    if (!pause) {
        s_redraw = true;   /* the panel shows the last frame: LVGL draws it all again */
    }
}

static void show_task(void *arg)
{
    (void)arg;
    int k;
    int64_t poked = 0;
    for (;;) {
        s_show_stage = 0;
        if (s_wake_req) {
            s_wake_req = false;
            if (muse_state_asleep()) {
                ESP_LOGI(TAG, "waking the screen for video");
                muse_state_set_asleep(false);
            }
        }
        if (xQueueReceive(s_frames, &k, pdMS_TO_TICKS(100)) != pdTRUE) {
            if (!s_streaming && s_paused) {
                pause_lvgl(false);   /* the stream ended: show the captions again */
            }
            continue;
        }
        slot_t *sl = &s_slot[k];
        while (!sl->pic && !sl->done) {
            vTaskDelay(1);
        }
        /* Live video drawn in step with the panel's refresh goes out at most
         * every other refresh (29.9 fps at 60 Hz): a 30 fps stream gains a
         * frame on it every few seconds, and the frames queue up behind.
         * When a newer picture is already on its way and this one is late,
         * skip this one (decoded all the same: the next predicts from it). */
        int next;
        if (!sl->due && s_params.fps && xQueuePeek(s_frames, &next, 0) == pdTRUE) {
            while (!sl->done) {
                vTaskDelay(1);
            }
            /* The next one already whole means this one is a frame behind:
             * show the newer one now rather than both a frame late. */
            bool newer = s_slot[next].done && !s_slot[next].failed;
            if (newer || esp_timer_get_time() - sl->t_whole > 1200000 / s_params.fps) {
                s_skipped++;
                xSemaphoreGive(s_slots);
                continue;
            }
        }
        if (sl->due) {
            /* Paced: the whole picture, at its time. */
            while (!sl->done) {
                vTaskDelay(1);
            }
            /* A little early: a panel drawn in step with its refresh
             * (muse_lcd_bands_set_te) then starts at the scan nearest the
             * time instead of the next one after it, up to 17 ms late, and
             * hands the buffer back to the decoder sooner. */
            int64_t due = sl->due - SHOW_LEAD_US, t = esp_timer_get_time();
            if (due > t) {
                s_pace_waits_us += (uint32_t)(due - t);
                int64_t ms = (due - t) / 1000;
                if (ms > 1) {
                    vTaskDelay(pdMS_TO_TICKS(ms - 1));
                }
                while (esp_timer_get_time() < due) {
                    taskYIELD();
                }
            }
        }
        bool drawable = sl->pic && !sl->failed && s_open;
        /* The screen may have gone dark (or never woken) before the stream:
         * wake it, and draw only once the UI has been through a frame awake
         * (it turns the panel on there; pausing LVGL before that hangs). */
        if (drawable && muse_state_asleep()) {
            ESP_LOGI(TAG, "waking the screen for video");
            muse_state_set_asleep(false);
        }
        if (drawable && !s_paused && esp_timer_get_time() - s_awake_tick > 300000) {
            drawable = false;
            s_dropped++;
        }
        if (!drawable) {
            while (!sl->done) {
                vTaskDelay(1);   /* the decoder still writes into it */
            }
            xSemaphoreGive(s_slots);
            continue;
        }
        s_show_stage = 1;
        s_stage_t = esp_timer_get_time();
        pause_lvgl(true);
        s_show_stage = 2;
        s_stage_t = esp_timer_get_time();
        int pw = sl->w, ph = sl->h;
        int vis_w = (s_params.width && s_params.width <= pw ? s_params.width : pw) & ~1;
        int vis_h = (s_params.height && s_params.height <= ph ? s_params.height : ph) & ~3;
        /* Two times upscaled when that fits the panel; one to one when the
         * picture is the panel's size or more; between the two (on the S31),
         * scaled up to fill the panel's width. */
        int scale = 2 * vis_w <= muse_board->width && 2 * vis_h <= muse_board->height ? 2 : 1;
        int ow, oh;
        if (scale == 1 && MAX_SRC > 240 && vis_w * 3 / 2 == muse_board->width && vis_h * 3 / 2 <= muse_board->height &&
            vis_w % 4 == 0 && vis_h % 4 == 0) {
            scale = 3;   /* 320 on the 480 panel: the cheap 3/2 path */
            ow = vis_w * 3 / 2;
            oh = vis_h * 3 / 2 & ~3;
        } else if (scale == 1 && MAX_SRC > 240 && vis_w < muse_board->width && vis_h < muse_board->height) {
            scale = 0;
            ow = muse_board->width & ~3;
            oh = (vis_h * ow / vis_w) & ~3;
            if (oh > muse_board->height) {
                oh = muse_board->height & ~3;
                ow = (vis_w * oh / vis_h) & ~3;
            }
        } else {
            if (scale * vis_w > muse_board->width) {
                vis_w = muse_board->width / scale & ~1;
            }
            if (scale * vis_h > muse_board->height) {
                vis_h = muse_board->height / scale & ~3;
            }
            ow = scale * vis_w;
            oh = scale * vis_h;
        }
        int x1 = (muse_board->width - ow) / 2 & ~1, y1 = (muse_board->height - oh) / 2 & ~1;
        fill_arg_t a = { sl, vis_w, vis_h, scale, ow, oh };
        int64_t d0 = esp_timer_get_time();
        muse_board->draw_frame(x1, y1, x1 + ow, y1 + oh, PIECE_ROWS, fill, &a);
        s_show_stage = 3;
        s_stage_t = esp_timer_get_time();
        while (!sl->done) {
            vTaskDelay(1);
        }
        xSemaphoreGive(s_slots);
        s_shown++;
        int64_t now = esp_timer_get_time();
        s_last_number = sl->number;
        s_last_shown_t = now;
        static int64_t last_shown;
        if (last_shown && now - last_shown < 1000000) {
            uint32_t gap = (uint32_t)(now - last_shown);
            if (gap > s_gap_max) {
                s_gap_max = gap;
            }
            if (s_params.fps && gap > 1500000u / s_params.fps) {
                s_gap_late++;
            }
        }
        last_shown = now;
        s_draw_sum += (uint32_t)(now - d0);
        s_lat_sum += (uint32_t)(now - sl->t_whole);
        s_lat_n++;
        if (now - poked > 1000000) {
            poked = now;
            muse_state_poke();   /* no auto-sleep while video plays */
        }
    }
}

/* ---- CPU accounting ------------------------------------------------------ */

#if CONFIG_FREERTOS_USE_TRACE_FACILITY && CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
#define CPU_MAX_TASKS 48
/*
 * Logs each core's busy share and the busiest tasks since the last call:
 * what video costs and what's left (for two-way audio, say).
 */
static void cpu_report(void)
{
    static TaskHandle_t prev_task[CPU_MAX_TASKS];
    static configRUN_TIME_COUNTER_TYPE prev_rt[CPU_MAX_TASKS], prev_total;
    static int prev_n;
    TaskStatus_t *st = heap_caps_malloc(CPU_MAX_TASKS * sizeof(*st), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!st) {
        return;
    }
    configRUN_TIME_COUNTER_TYPE total;
    UBaseType_t n = uxTaskGetSystemState(st, CPU_MAX_TASKS, &total);
    configRUN_TIME_COUNTER_TYPE elapsed = total - prev_total;
    if (prev_total && elapsed) {
        uint32_t idle[2] = { 0, 0 };
        struct { const char *name; uint32_t pm; } top[4] = { 0 };
        for (UBaseType_t i = 0; i < n; i++) {
            configRUN_TIME_COUNTER_TYPE before = 0;
            for (int j = 0; j < prev_n; j++) {
                if (prev_task[j] == st[i].xHandle) {
                    before = prev_rt[j];
                    break;
                }
            }
            uint32_t pm = (uint32_t)((uint64_t)(st[i].ulRunTimeCounter - before) * 1000 / elapsed);
            bool is_idle = false;
            for (int c = 0; c < 2; c++) {
                if (st[i].xHandle == xTaskGetIdleTaskHandleForCore(c)) {
                    idle[c] = pm;
                    is_idle = true;
                }
            }
            if (is_idle) {
                continue;
            }
            for (int k = 0; k < 4; k++) {
                if (pm > top[k].pm) {
                    memmove(&top[k + 1], &top[k], (3 - k) * sizeof(top[0]));
                    top[k].name = st[i].pcTaskName;
                    top[k].pm = pm;
                    break;
                }
            }
        }
        ESP_LOGI(TAG, "cpu: core0 busy %u%% core1 busy %u%%; %s %u%%, %s %u%%, %s %u%%, %s %u%%",
                 (unsigned)(100 - idle[0] / 10), (unsigned)(100 - idle[1] / 10), top[0].name ? top[0].name : "-",
                 (unsigned)(top[0].pm / 10), top[1].name ? top[1].name : "-", (unsigned)(top[1].pm / 10),
                 top[2].name ? top[2].name : "-", (unsigned)(top[2].pm / 10), top[3].name ? top[3].name : "-",
                 (unsigned)(top[3].pm / 10));
    }
    prev_n = (int)n;
    for (UBaseType_t i = 0; i < n; i++) {
        prev_task[i] = st[i].xHandle;
        prev_rt[i] = st[i].ulRunTimeCounter;
    }
    prev_total = total;
    free(st);
}
#else
static void cpu_report(void) {}
#endif

/* ---- decoding (first core) ---------------------------------------------- */

static void on_progress(void *ctx, const u8 *pic, u32 width_mbs, u32 height_mbs, u32 rows)
{
    slot_t *sl = ctx;
    sl->w = (int)width_mbs * 16;
    if (sl->bands == 2) {
        /* The whole picture: the stream's height (the bottom band's own
         * size comes with its picture). */
        sl->h0 = (int)height_mbs * 16;
        sl->h = (s_params.height + 15) & ~15;
    } else {
        sl->h = (int)height_mbs * 16;
    }
    sl->pic = pic;
    sl->rows = (int)rows;
}

static void on_progress1(void *ctx, const u8 *pic, u32 width_mbs, u32 height_mbs, u32 rows)
{
    slot_t *sl = ctx;
    (void)width_mbs;
    (void)height_mbs;
    sl->pic1 = pic;
    sl->rows1 = (int)rows;
}

/*
 * The bottom band: its own decoder, so the two halves of a frame decode at
 * once (each core about half the work). Its picture buffers live until the
 * next stream (a new gen) or the decode task closes the stream.
 */
static void band_task(void *arg)
{
    (void)arg;
    storage_t *dec = NULL;
    uint32_t gen = 0;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        band_job_t *j = &s_band;
        if (!j->sl) {   /* close */
            if (dec) {
                h264bsdShutdown(dec);
                h264bsdFree(dec);
                dec = NULL;
            }
            xSemaphoreGive(s_band_done);
            continue;
        }
        if (!dec || gen != j->gen) {
            if (dec) {
                h264bsdShutdown(dec);
                h264bsdFree(dec);
            }
            gen = j->gen;
            dec = h264bsdAlloc();
            if (dec && h264bsdInit(dec, 1) != 0) {
                h264bsdFree(dec);
                dec = NULL;
            }
        }
        j->ok = j->got = false;
        if (dec) {
            h264bsdSetProgress(dec, on_progress1, j->sl);
            u8 *pic = NULL;
            u32 w = 0, h = 0;
            int64_t t0 = esp_timer_get_time();
            u32 rc = h264bsdDecode(dec, (u8 *)j->au, j->len, &pic, &w, &h);
            j->us = (uint32_t)(esp_timer_get_time() - t0);
            j->got = rc == H264BSD_PIC_RDY && pic;
            j->ok = rc == H264BSD_PIC_RDY || rc == H264BSD_RDY;
            j->h = (int)h;
            if (j->got) {
                j->sl->pic1 = pic;
                j->sl->rows1 = (int)h;
            }
        }
        xSemaphoreGive(s_band_done);
    }
}

/* Closes the bottom band's decoder and waits for it. */
static void band_close(void)
{
    if (!s_band_task) {
        return;
    }
    s_band.sl = NULL;
    xTaskNotifyGive(s_band_task);
    xSemaphoreTake(s_band_done, portMAX_DELAY);
}

/*
 * Playout pacing (playout_ms): frame N shows at base + (N - first) frame
 * periods, base being the first frame's arrival plus playout_ms. The decoder
 * doesn't wait for that time: it decodes as soon as a slot is free, so it
 * works up to a frame ahead and a slow frame (high motion) borrows the time
 * a quick one left; the show task waits for the time (slot_t.due). A frame
 * that arrives or finishes decoding later than its time shows at once and
 * moves the schedule later, so the ones after it don't rush. A keyframe
 * after a gap starts over.
 */
static int64_t s_pace_base;
static uint16_t s_pace_first;

/* When frame fr shows (0: no playout delay, it shows as it decodes). */
static int64_t pace_due(const muse_vframe_t *fr, bool key, int64_t now)
{
    if (!s_params.playout_ms || !s_params.fps) {
        return 0;
    }
    int64_t period = 1000000 / s_params.fps;
    if (!s_pace_base || (key && fr->after_gap)) {
        s_pace_base = now + (int64_t)s_params.playout_ms * 1000;
        s_pace_first = fr->number;
    }
    int64_t due = s_pace_base + (int64_t)(uint16_t)(fr->number - s_pace_first) * period;
    if (now - due > period / 2) {
        s_pace_base += now - due;   /* late: show it now, and the rest after it */
        s_pace_late++;
        s_late_in++;
        return now;
    }
    if (due - now > (int64_t)s_params.playout_ms * 1000 + period) {
        s_pace_base = now + (int64_t)s_params.playout_ms * 1000;   /* numbering jumped: start over */
        s_pace_first = fr->number;
        return s_pace_base;
    }
    return due;
}

/* After the decode: a frame done later than its time moves the schedule. */
static int64_t pace_decoded(int64_t due)
{
    int64_t t = esp_timer_get_time(), period = 1000000 / (s_params.fps ? s_params.fps : 30);
    if (due && t - due > period / 2) {
        s_pace_base += t - due;
        s_pace_late++;
        return t;
    }
    return due;
}

static void decode_task(void *arg)
{
    (void)arg;
    storage_t *dec = NULL;
    uint32_t gen = 0;
    bool need_key = true;
    int next_slot = 0;
    int64_t asked = 0, stat_t = 0;
    uint32_t dec_us_sum = 0, dec_us_max = 0, dec_n = 0, bytes = 0;
    uint32_t avg_ms = 0, rx_last = 0;
    for (;;) {
        if (!s_streaming) {
            if (dec) {
                /* Both pictures drawn (or dropped) before the buffers go. */
                xSemaphoreTake(s_slots, portMAX_DELAY);
                xSemaphoreTake(s_slots, portMAX_DELAY);
                xSemaphoreGive(s_slots);
                xSemaphoreGive(s_slots);
                h264bsdShutdown(dec);
                h264bsdFree(dec);
                dec = NULL;
                band_close();
                ESP_LOGI(TAG, "decoder closed: %u shown, %u dropped", s_shown, s_dropped);
            }
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }
        if (!dec || gen != s_gen) {
            if (dec) {
                h264bsdShutdown(dec);
                h264bsdFree(dec);
            }
            gen = s_gen;
            dec = h264bsdAlloc();
            if (!dec || h264bsdInit(dec, 1) != 0) {
                ESP_LOGE(TAG, "no memory for the decoder");
                if (dec) {
                    h264bsdFree(dec);
                    dec = NULL;
                }
                if (s_tp && s_tp->stop) {
                    s_tp->stop();
                }
                s_streaming = false;
                continue;
            }
            need_key = true;
            s_pace_base = 0;
            dec_us_sum = dec_us_max = dec_n = bytes = 0;
            rx_last = 0;
            stat_t = esp_timer_get_time();
            ESP_LOGI(TAG, "decoder open: %ux%u %u fps %u kb/s%s (decoding on core %d)", s_params.width, s_params.height,
                     s_params.fps, s_params.kbps, s_params.tiles == 2 ? ", two bands (one per core)" : "",
                     (int)xTaskGetCoreID(NULL));
        }
        muse_vstream_t *vs = s_tp->ring;
        muse_vframe_t fr;
        int32_t n = muse_vstream_pop(vs, s_au, AU_MAX, &fr);
        if (n == 0) {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2));
            continue;
        }
        int64_t now = esp_timer_get_time();
        int64_t t_whole = now;
        bool key = n > 0 && (fr.flags & MUSE_VIDEO_FRAME_KEY);
        if (n < 0 || (fr.after_gap && !key)) {
            need_key = true;
        }
        if (n < 0 || (need_key && !key)) {
            s_dropped++;
            if (now - asked > KEYFRAME_ASK_MS * 1000) {
                asked = now;
                ask_keyframe();
            }
            tell_freed((uint8_t)avg_ms);
            continue;
        }
        need_key = false;
        bytes += (uint32_t)n;
        /* The ring has room again: credits go before the decode. */
        tell_freed((uint8_t)avg_ms);
        int64_t due = pace_due(&fr, key, now);

        /* A slot: the picture two back has been drawn, so the decoder may
         * reuse its buffer. */
        int64_t t_slot = esp_timer_get_time();
        s_slot_wait_t = t_slot;
        while (xSemaphoreTake(s_slots, pdMS_TO_TICKS(2000)) != pdTRUE) {
            ESP_LOGW(TAG, "decoder: still waiting for a picture to be drawn (stage %d)", s_show_stage);
        }
        s_slot_wait_t = 0;
        s_slot_wait_us += (uint32_t)(esp_timer_get_time() - t_slot);
        /* Two bands: the top band's access unit after its length, the
         * bottom band's after that. */
        int bands = s_params.tiles == 2 && s_band_task ? 2 : 1;
        uint32_t len0 = (uint32_t)n;
        if (bands == 2) {
            len0 = n >= 4 ? (uint32_t)(s_au[0] | s_au[1] << 8 | s_au[2] << 16 | (uint32_t)s_au[3] << 24) : UINT32_MAX;
            if (len0 > (uint32_t)n - 4) {
                ESP_LOGW(TAG, "bad band length %u in a %d byte frame", (unsigned)len0, (int)n);
                need_key = true;
                s_dropped++;
                xSemaphoreGive(s_slots);
                continue;
            }
        }
        slot_t *sl = &s_slot[next_slot];
        sl->pic = NULL;
        sl->rows = 0;
        sl->pic1 = NULL;
        sl->rows1 = 0;
        sl->bands = bands;
        sl->h0 = (int)(s_params.height / 2);
        sl->failed = false;
        sl->done = false;
        sl->t_whole = t_whole;
        sl->due = due;
        sl->number = fr.number;
        h264bsdSetProgress(dec, on_progress, sl);
        xQueueSend(s_frames, &next_slot, portMAX_DELAY);
        next_slot ^= 1;

        /* The whole access unit at once: parameter sets and the picture's
         * slices; h264bsdDecode works through its NAL units in turn. */
        u8 *pic = NULL;
        u32 w = 0, h = 0;
        int64_t t0 = esp_timer_get_time();
        if (bands == 2) {
            s_band.au = s_au + 4 + len0;
            s_band.len = (uint32_t)n - 4 - len0;
            s_band.sl = sl;
            s_band.gen = gen;
            xTaskNotifyGive(s_band_task);
        }
        u32 rc = h264bsdDecode(dec, bands == 2 ? s_au + 4 : s_au, len0, &pic, &w, &h);
        if (bands == 2) {
            s_top_us += (uint32_t)(esp_timer_get_time() - t0);
        }
        bool got = rc == H264BSD_PIC_RDY && pic;
        bool ok = rc == H264BSD_PIC_RDY || rc == H264BSD_RDY;
        if (bands == 2) {
            xSemaphoreTake(s_band_done, portMAX_DELAY);   /* the bottom band */
            s_bot_us += s_band.us;
            s_band_n++;
            got = got && s_band.got;
            ok = ok && s_band.ok;
        }
        uint32_t us = (uint32_t)(esp_timer_get_time() - t0);
        if (got && bands == 2 && (int)h + s_band.h != (int)((s_params.height + 15) & ~15)) {
            static int64_t told_h;
            if (now - told_h > 5000000) {
                told_h = now;
                ESP_LOGW(TAG, "bands of %u and %d rows don't make the stream's %u", (unsigned)h, s_band.h, s_params.height);
            }
            got = false;
        }
        if (got) {
            sl->w = (int)w;
            sl->h0 = (int)h;
            sl->h = bands == 2 ? (int)h + s_band.h : (int)h;
            sl->pic = pic;   /* the same buffer progress reported, with no output reordering */
            sl->rows = (int)h;
        } else {
            sl->failed = true;
        }
        sl->due = pace_decoded(due);
        sl->done = true;
        if (!ok) {
            static int64_t told;
            if (now - told > 2000000) {
                told = now;
                ESP_LOGW(TAG, "frame %u not decoded (%d bytes, %s%s): waiting for a keyframe", fr.number, (int)n,
                         key ? "keyframe" : "not a keyframe", bands == 2 ? ", two bands" : "");
            }
            need_key = true;
            s_dropped++;
            if (now - asked > KEYFRAME_ASK_MS * 1000) {
                asked = now;
                ask_keyframe();
            }
            continue;
        }
        if (!got) {
            continue;
        }
        dec_us_sum += us;
        dec_n++;
        if (us > dec_us_max) {
            dec_us_max = us;
        }
        if (now - stat_t >= 2000000) {
            uint32_t secs_ms = (uint32_t)((now - stat_t) / 1000);
            avg_ms = dec_n ? dec_us_sum / dec_n / 1000 : 0;
            uint32_t ln = s_lat_n ? s_lat_n : 1;
            uint32_t rx = muse_vstream_received(vs);
            ESP_LOGI(TAG, "%u fps decoded, decode avg %u ms max %u ms, in->shown %u ms (draw %u), %u kb/s (rx %u), "
                     "shown %u dropped %u skipped %u, ring free %u; show gap max %u ms, %u over 1.5 periods%s",
                     (unsigned)(dec_n * 1000 / secs_ms), (unsigned)avg_ms, (unsigned)(dec_us_max / 1000),
                     (unsigned)(s_lat_sum / ln / 1000), (unsigned)(s_draw_sum / ln / 1000),
                     (unsigned)(bytes * 8 / secs_ms), (unsigned)((rx - rx_last) * 8 / secs_ms), s_shown, s_dropped,
                     (unsigned)s_skipped, (unsigned)muse_vstream_available(vs), (unsigned)(s_gap_max / 1000), (unsigned)s_gap_late,
                     s_params.playout_ms ? "" : "");
            if (s_params.playout_ms) {
                ESP_LOGI(TAG, "playout %u ms: waited %u ms per frame, %u late (%u arrived late), decoder waited %u ms per frame for a buffer",
                         s_params.playout_ms, (unsigned)(dec_n ? s_pace_waits_us / dec_n / 1000 : 0), (unsigned)s_pace_late,
                         (unsigned)s_late_in, (unsigned)(dec_n ? s_slot_wait_us / dec_n / 1000 : 0));
            }
            s_pace_waits_us = s_pace_late = s_late_in = s_slot_wait_us = 0;
            s_gap_max = s_gap_late = 0;
            rx_last = rx;
            cpu_report();
            if (s_band_n) {
                ESP_LOGI(TAG, "bands: top %u ms (core %d), bottom %u ms (core %d)", (unsigned)(s_top_us / s_band_n / 1000),
                         DECODE_CORE, (unsigned)(s_bot_us / s_band_n / 1000), MUSE_UI_CORE);
                s_top_us = s_bot_us = s_band_n = 0;
            }
#if CONFIG_MUSE_VIDEO_SIMD_MC
            /* tinyh264 logs its own check; this puts it beside the stats */
            static bool pie_said;
            if (!pie_said && h264bsdPieState() >= 0) {
                pie_said = true;
                ESP_LOGI(TAG, "motion compensation: %s", h264bsdPieState() ? "S31 SIMD (passed its check)" : "C (the SIMD failed its check)");
            }
#endif
            s_lat_sum = s_draw_sum = s_lat_n = 0;
            dec_us_sum = dec_us_max = dec_n = bytes = 0;
            stat_t = now;
        }
    }
}

/* Bench aid: when the pipeline or the UI looks stuck, every task's backtrace, once. */
static void stuck_check(void *arg)
{
    (void)arg;
    static int64_t stage_since;
    static int last_stage;
    static bool dumped;
    int64_t now = esp_timer_get_time();
    if (s_show_stage != last_stage) {
        last_stage = s_show_stage;
        stage_since = now;
    }
    (void)stage_since;
    int stage = s_show_stage;
    int64_t slot_wait = s_slot_wait_t;
    bool stuck = (stage != 0 && now - s_stage_t > 3000000) || s_refusals > 6 ||
                 (slot_wait && now - slot_wait > 3000000);
    if (!stuck) {
        dumped = false;
    }
    if (stuck && !dumped) {
        dumped = true;
        ESP_LOGW(TAG, "stuck (show stage %d, %d refusals, asleep %d, ui %d): task backtraces follow", stage,
                 s_refusals, muse_state_asleep(), s_ui_up);
        esp_backtrace_print_all_tasks(12);
    }
    /* A panel that hasn't taken a frame in 10 s won't: start over rather
     * than leave the gadget dark and deaf (the phone reconnects). */
    if (stage != 0 && now - s_stage_t > 10000000) {
        ESP_LOGE(TAG, "the panel has been stuck for 10 s (stage %d): restarting", stage);
        esp_restart();
    }
}

static bool start_tasks(void)
{
    if (s_dec_task) {
        return true;
    }
    static esp_timer_handle_t watch;
    const esp_timer_create_args_t wa = { .callback = stuck_check, .name = "video_watch" };
    if (esp_timer_create(&wa, &watch) == ESP_OK) {
        esp_timer_start_periodic(watch, 1000000);
    }
    s_frames = xQueueCreate(2, sizeof(int));
    s_slots = xSemaphoreCreateCounting(2, 2);
    s_au = heap_caps_malloc(AU_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_frames || !s_slots || !s_au) {
        ESP_LOGE(TAG, "no memory");
        return false;
    }
    if (xTaskCreatePinnedToCore(show_task, "video_show", 3072, NULL, MUSE_UI_PRIORITY, NULL, MUSE_UI_CORE) != pdPASS ||
        xTaskCreatePinnedToCore(decode_task, "video_dec", VIDEO_DEC_STACK, NULL, 4, &s_dec_task, DECODE_CORE) != pdPASS) {
        ESP_LOGE(TAG, "no memory for the tasks");
        return false;
    }
    /* Two bands at once on chips fast enough for 480 px (the S31). */
    s_band_done = MAX_BANDS > 1 ? xSemaphoreCreateBinary() : NULL;
    if (MAX_BANDS > 1 && s_band_done &&
        xTaskCreatePinnedToCore(band_task, "video_band", VIDEO_DEC_STACK, NULL, 4, &s_band_task, MUSE_UI_CORE) != pdPASS) {
        ESP_LOGE(TAG, "no memory for the tasks");
        return false;
    }
    return true;
}

/* ---- the transports' side ---------------------------------------------- */

bool muse_video_begin(const muse_video_transport_t *t, const muse_video_params_t *vp)
{
    /* A client that reconnects at once can ask before the panel is up (2 s
     * into boot); drawing then would wait forever on a sender that doesn't
     * exist yet. It hears busy and asks again. */
    if (!muse_video_supported() || !start_tasks()) {
        return false;
    }
    if (muse_state_asleep() || !s_ui_up) {
        s_refusals++;
        /* Wake the screen, from the show task: here the protocol is locked,
         * and waking takes the display lock, which the UI holds while it asks
         * the protocol for the link state. The client's retry finds it on. */
        s_wake_req = true;
        return false;
    }
    if (s_streaming && s_tp && s_tp != t) {
        ESP_LOGW(TAG, "%s asks for a stream while %s has one: busy", t->name, s_tp->name);
        return false;
    }
    s_refusals = 0;
    s_tp = t;
    s_params = *vp;
    s_gen++;
    s_shown = s_dropped = 0;
    s_last_shown_t = 0;
    s_open = true;   /* a client may start one without the menu: show it */
    s_streaming = true;
    xTaskNotifyGive(s_dec_task);
    return true;
}

void muse_video_wake(void)
{
    TaskHandle_t t = s_dec_task;
    if (t && s_streaming) {
        xTaskNotifyGive(t);
    }
}

void muse_video_end(const muse_video_transport_t *t)
{
    if (t != s_tp) {
        return;   /* not the stream that's on */
    }
    s_streaming = false;
    if (s_dec_task) {
        xTaskNotifyGive(s_dec_task);
    }
}

/* ---- the view ----------------------------------------------------------- */

void muse_video_open(void)
{
    if (!muse_video_supported()) {
        return;
    }
    ESP_LOGI(TAG, "view open");
    s_open = true;
    for (int i = 0; i < s_ntransports; i++) {
        if (s_transports[i]->request) {
            s_transports[i]->request(true);
        }
    }
}

void muse_video_close(void)
{
    if (!s_open) {
        return;
    }
    ESP_LOGI(TAG, "view closed");
    s_open = false;
    for (int i = 0; i < s_ntransports; i++) {
        if (s_transports[i]->request) {
            s_transports[i]->request(false);   /* the clients stop */
        }
    }
    if (s_streaming && s_tp && s_tp->stop) {
        s_tp->stop();   /* ends the stream on our side at once */
    }
    s_streaming = false;
    if (s_dec_task) {
        xTaskNotifyGive(s_dec_task);
    }
    pause_lvgl(false);
}

static void build(void)
{
    lv_obj_t *scr = lv_screen_active();
    s_root = lv_obj_create(scr);
    lv_obj_remove_style_all(s_root);
    lv_obj_set_size(s_root, muse_board->width, muse_board->height);
    lv_obj_set_style_bg_color(s_root, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_root, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    s_title = lv_label_create(s_root);
    lv_obj_set_style_text_font(s_title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_title, lv_color_hex(0xa77dff), 0);
    lv_label_set_text(s_title, "PHONE CAMERA");
    lv_obj_align(s_title, LV_ALIGN_CENTER, 0, -24);
    s_caption = lv_label_create(s_root);
    lv_obj_set_style_text_font(s_caption, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_caption, lv_color_hex(0xf2efff), 0);
    lv_obj_set_style_text_align(s_caption, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(s_caption, muse_board->width * 3 / 4);
    lv_label_set_long_mode(s_caption, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_align(s_caption, LV_ALIGN_CENTER, 0, 24);
    lv_obj_add_flag(s_root, LV_OBJ_FLAG_HIDDEN);
}

bool muse_video_tick(void)
{
    if (!muse_video_supported()) {
        return false;
    }
    s_ui_up = true;
    if (!s_root) {
        build();
    }
    bool open = s_open;
    if (lv_obj_has_flag(s_root, LV_OBJ_FLAG_HIDDEN) == open) {
        lv_obj_set_flag(s_root, LV_OBJ_FLAG_HIDDEN, !open);
        if (open) {
            lv_obj_move_foreground(s_root);
        }
    }
    if (open) {
        s_awake_tick = esp_timer_get_time();   /* this tick runs only while the screen is on */
    }
    if (!open) {
        if (s_redraw) {
            s_redraw = false;
            lv_obj_invalidate(lv_screen_active());
        }
        return false;
    }
    const char *text = s_streaming          ? "Starting..."
                       : muse_video_has_client() ? "Waiting for the phone..."
                                               : "Open Muse Gadgets Camera on your phone";
    if (strcmp(lv_label_get_text(s_caption), text) != 0) {
        lv_label_set_text(s_caption, text);
    }
    if (s_redraw) {
        s_redraw = false;
        lv_obj_invalidate(lv_screen_active());
    }
    return true;
}
