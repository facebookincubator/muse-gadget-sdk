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
#include "sdkconfig.h"
#include "muse_video.h"
#if CONFIG_MUSE_APP_VIEW
#include "muse_app_view.h"
#endif

#include <stdarg.h>
#include <string.h>

#if defined(__has_include) && __has_include("esp_attr.h")
#include "esp_attr.h"
#endif
#ifndef EXT_RAM_BSS_ATTR
#define EXT_RAM_BSS_ATTR   /* a host harness: no PSRAM */
#endif
#include "esp_debug_helpers.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_rom_md5.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "muse_vdec.h"
#include "muse_vgate.h"
#if CONFIG_MUSE_VIDEO_SIMD_MC
#include "h264bsd_reconstruct.h"   /* h264bsdPieState() */
#endif
#include "muse_board.h"
#include "muse_lvgl_hold.h"
#include "muse_mem.h"
#include "muse_state.h"
#include "muse_yuv.h"
#include "muse_ui_fb.h"
#include "muse_video_tick.h"
#include "muse_video_fit.h"
#include "muse_vdirty.h"
#include "muse_mic_ring.h"
#include "muse_turn_dot.h"

static const char *TAG = "muse_video";

#if   CONFIG_IDF_TARGET_ESP32S3
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
/* The decode tasks' stacks: the decoder works in them all the time (its fill
 * and prediction buffers), so they must be in main SRAM. Plain task creation
 * takes internal memory that can be LP RAM, part of the heap on the S31
 * (CONFIG_ESP_SYSTEM_ALLOW_RTC_FAST_MEM_AS_HEAP) and many times slower: once
 * SRAM ran low, a band decoding there took 250-450 ms instead of 15.
 * MALLOC_CAP_DMA is what main SRAM has and LP RAM doesn't. The tasks live
 * for good, so nothing deletes them. */
#define VIDEO_DEC_STACK_CAPS (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT | MALLOC_CAP_DMA)
/* Bands decoded in parallel (muse_video_params_t.tiles): one per core,
 * where the picture is big enough to need both. */
#define MAX_BANDS (MAX_SRC > 240 ? 2 : 1)
#define TILE_SPLIT_PCT 60
/*
 * UI streams (MUSE_VIDEO_CONTENT_UI, protocols/UI.md): a mostly still
 * picture at the panel's own size (466 on the StopWatch, coded as 480),
 * shown one to one, in two bands on the two cores on every chip: frames come
 * only on change, so the S3 has time for them. A keyframe of a busy screen
 * at fine quantizers runs to ~100 KB.
 */
#define UI_MAX 480
#define UI_BANDS 2
#define UI_FPS 15
#ifdef CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ
#define VIDEO_CPU_MHZ CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ   /* the fill's cycle counts in us (the stats line) */
#else
#define VIDEO_CPU_MHZ 160
#endif
#define AU_MAX (128 * 1024)
#define BIG_CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define SHOW_STACK 3072
#define UI_POKE_US 1000000             /* keeps the screen on while a UI stream is on */
#define SHOW_LEAD_US 8000            /* paced pictures go to the panel this early (show_task) */
#define STALL_US 2500000             /* no picture for this long: the UI gets the screen back (show_task) */
#define DECODE_CORE 0                  /* the UI core converts and sends */
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
    /* Each band's planes and strides (pic and pic1 are their luma). */
    muse_pic_t p0, p1;
    int bands;
    volatile int h0;                   /* two bands: the top band's rows (each band's SPS gives its own) */
    uint16_t number;                   /* its frame number (the header's), echoed to the client once shown */
    uint32_t number32;                 /* ...extended to 32 bits, counted from the stream's start */
    /* For the UI stream's per-frame line: its size, kind and each band's decode time. */
    uint32_t bytes, dec_us, top_us, bot_us;
    bool key;
    uint32_t dec_index;                /* which decode this picture was (s_dec_index): ring_check */
    bool waited;                       /* the decoder waited for its frame's last bytes: keeping up (the lip-sync floor) */
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
static void log_memory(const char *when);
/* The band task's "done": a semaphore of its own, since the decode task's
 * notification also means "frames arrived" (the transport wakes it). */
static SemaphoreHandle_t s_band_done;

typedef struct {
    slot_t *s;
    int vis_w, vis_h;
    int scale;                         /* 2: two times; 1: one to one; 3: 3/2 (320 -> 480); 0: out_w x out_h */
    int out_w, out_h;
    int x, y;                          /* where the output's top left is on the panel */
    muse_video_fit_t fit;              /* fit.cover: a round panel's cover (muse_video_fit.h) */
} fill_arg_t;

/* The cover path's block of four 2x rows (muse_video_fit.h): allocated at the
 * first cover picture, freed when the tasks retire. blk is reset for every
 * picture drawn. */
static muse_video_cover_t s_cov = { NULL, -1 };

static volatile muse_video_overlay_t s_overlay;
/* The received frame numbers extended to 32 bits (decode task); reset at each stream's start. */
static uint32_t s_ext;
/* The picture on the panel now, its number extended to 32 bits
 * (number32): the A/V anchor's reader (muse_video_shown32). */
static volatile uint32_t s_shown32;
static volatile bool s_shown32_ok;
static volatile bool s_ext_reset = true;
/* The last picture's place on the panel (end exclusive; x2 0: none yet), for
 * repainting the mic ring around and over it. */
static int s_win_x1, s_win_y1, s_win_x2, s_win_y2;

static muse_video_caps_t s_caps;
static const muse_video_transport_t *s_transports[MAX_TRANSPORTS];
static int s_ntransports;
/* The transport whose stream is on (or was last); set by begin. */
static const muse_video_transport_t *volatile s_tp;
/* The last picture drawn: its frame number and when (for the latency echo). */
static volatile uint16_t s_last_number;
static volatile int64_t s_last_shown_t;
static volatile bool s_open, s_streaming, s_redraw, s_paused;
static volatile bool s_view;   /* the user opened the view (muse_video_open); s_open is also set by a client's start */
static volatile bool s_hold;   /* the passcode lock (muse_video_set_hold()) */
static volatile bool s_cover;  /* a passcode keypad is up (muse_video_set_cover()) */
/* Held off the screen: locked, or a passcode keypad is up. */
static bool held(void)
{
    return s_hold || s_cover;
}
static volatile bool s_ui_up;          /* the UI's frame tick has run: the panel and its sender exist */
static volatile bool s_wake_req;       /* begin() found the screen dark: the show task wakes it */
static volatile int s_refusals;        /* begin() refused in a row: a stuck UI shows up here */
/* Each picture slot's last event and when, for the stuck-pipeline logs:
 * q queued by the decoder, D decoded, r received by the show task, then given
 * back: d drawn, s skipped, n not drawable, z no size. */
static volatile char s_slot_ev[2] = { '-', '-' };
static volatile int64_t s_slot_ev_t[2];
#define SLOT_EV(k, e) (s_slot_ev[(k) & 1] = (e), s_slot_ev_t[(k) & 1] = esp_timer_get_time())
static volatile int s_show_stage;      /* where the show task is, for a stuck-pipeline log */
#define SHOW_STAGE_DUMP 9              /* ui.dump: it holds both pictures, however long the dump takes */
static volatile int64_t s_stage_t;     /* when it got there */
static volatile int64_t s_slot_wait_t; /* when the decoder started waiting for a slot (0: not waiting) */
static volatile int64_t s_awake_tick;  /* the UI's last frame tick with the screen on and the view up */
static volatile bool s_ui;             /* the stream that's on is a UI stream (s_params.content) */
static volatile bool s_md5;
/* UI pictures through muse_yuv_1x_hq (console ui.hq=0 for the plain path). */
static volatile bool s_hq = true;
static volatile bool s_dump_req;      /* console ui.dump: the show task dumps the last UI picture */
/* ui.dump=buf: the same lines into a PSRAM buffer instead of the console, for
 * a board whose console isn't on its USB (the ESP-Mosaico reads it over ESP-Iris). */
static bool s_dump_to_buf;
static char *s_dbuf;
static size_t s_dlen, s_dcap;
static volatile bool s_dbuf_ready;
/* The buffer is written by the show task while it dumps, and freed and read by other tasks (the console's
 * ui.dump=buf, ESP-Iris's reader): all of it under s_dlock, and a reader copies, never keeps a pointer. s_dgen counts
 * frees: lines of a dump the show task began before one are dropped, and that dump is never marked ready. */
static SemaphoreHandle_t s_dlock;
static StaticSemaphore_t s_dlock_buf;
static portMUX_TYPE s_dlock_mux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_dgen, s_dump_gen;
static int s_last_k = -1;             /* the last slot drawn, and how (for ui.dump) */
static uint32_t s_last_dec_index;     /* its decode (s_dec_index): ui.dump only while nothing came after */
/* A still patch handed from the decode task to the show task (slot STILL_K in s_frames). */
#define STILL_K 2
/* The mic ring went on or off (muse_mic_ring.h): repaint the edges it covers. */
#define RING_K 3
static uint8_t *volatile s_still;
static volatile uint32_t s_still_len;
static volatile uint16_t s_still_number;
static volatile int64_t s_still_t;
static portMUX_TYPE s_still_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_fb_all;                  /* the next UI picture redraws everything (the panel showed something else) */
static fill_arg_t s_last_arg;            /* bench: log each UI picture's MD5 (muse_video_set_md5) */
static volatile uint32_t s_gen;        /* bumped by every begin */
static muse_video_params_t s_params;
static TaskHandle_t s_dec_task;
/*
 * The tasks retire when no stream has been on for VIDEO_RETIRE_US: their
 * stacks (6 KB each of main SRAM for the decode tasks, 3 KB for the show
 * task) are what the S3 runs short of between streams. The show task deletes
 * the parked decode tasks (s_dec_parked, s_band_parked: blocked with their
 * decoders closed) and then itself, under s_task_lock, which start_tasks
 * also takes; the next stream starts them again.
 */
#define VIDEO_RETIRE_US (15LL * 1000 * 1000)
static TaskHandle_t s_show_task;
static volatile bool s_dec_parked, s_band_parked;
/* The last stream's numbers, for a bench that reads them once the stream is
 * over: muse_video_last_summary(). */
static struct {
    uint16_t w, h, cw, ch;
    int16_t reach;                        /* H.264 low-memory decoder's reach in rows (0: whole picture; -1: n/a) */
    uint32_t start_free, start_largest;   /* at the start, before the decoder */
    uint32_t dec_free, dec_largest;       /* once the decoder took its memory */
    uint32_t low;                         /* internal free at its lowest sample (every 2 s and at the start) */
    uint32_t lowest_ever0, lowest_ever1;  /* the heap's lowest ever at the start and the end */
    uint32_t dec_avg_sum, dec_windows, dec_max;
    uint32_t shown, dropped;
    uint8_t cpu_peak;
    const char *refused;
    int64_t t0, t1;
} s_vl;
static volatile bool s_dec_holds;   /* the decode task holds a decoder (begin_locked opens one only when it doesn't) */
static SemaphoreHandle_t s_task_lock;
static StaticSemaphore_t s_task_lock_buf;
static portMUX_TYPE s_task_lock_mux = portMUX_INITIALIZER_UNLOCKED;
static QueueHandle_t s_frames;         /* slot indexes, decoder to show task */
static SemaphoreHandle_t s_slots;      /* free slots */
static slot_t s_slot[2];
/* Decodes started (the decode task), the last drawn picture's, and the mic
 * ring's generation (muse_mic_ring_generation) when it was drawn: ring_check. */
static volatile uint32_t s_dec_index;
static uint32_t s_shown_dec_index, s_ring_gen_shown;
static uint8_t *s_au;
static volatile uint16_t s_shown, s_dropped;
/* Pictures decoded but not drawn because a newer one was ready (the panel's
 * pace is a little under the stream's): see show_task. */
static volatile uint32_t s_skipped;
/* The reply's audio clock (muse_video_set_audio_clock; mg_ble_av_clock), or NULL. */
static bool (*s_audio_clock)(uint32_t *frame, int32_t *pos_us);
/* Popped to on the panel, and the draw alone, summed over s_lat_n frames (us). */
static volatile uint32_t s_lat_sum, s_draw_sum, s_lat_n;
/* The show task's own work per frame (fill_piece: the conversion, then the overlays), for the stats. */
static uint32_t s_fill_us, s_over_us;
/* Time between pictures reaching the panel: the largest, and how many took over 1.5 frame periods. */
static volatile uint32_t s_gap_max, s_gap_late;
/* The once-a-second line (muse_video_tick.h): frames taken from the ring
 * and frames the decoder refused, counted by the decode task alone. */
static muse_vtick_t s_tick;
static uint32_t s_rx_frames, s_dec_errors;
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

static volatile uint32_t s_kf_asks;   /* keyframes asked for */

static void ask_keyframe(void)
{
    const muse_video_transport_t *t = s_tp;
    s_kf_asks++;
    if (t && t->keyframe) {
        t->keyframe();
    }
}

bool muse_video_decoding(void)
{
    return s_streaming || s_dec_holds;
}

bool muse_video_is_open(void)
{
    return s_open;
}

bool muse_video_view_open(void)
{
    return s_view;
}

bool muse_video_shown32(uint32_t *frame)
{
    uint32_t f = s_shown32;
    bool ok = s_shown32_ok && s_streaming;
    *frame = f;
    return ok;
}

bool muse_video_drawing(void)
{
    return s_paused;
}

void muse_video_set_audio_clock(bool (*clock)(uint32_t *frame, int32_t *pos_us))
{
    s_audio_clock = clock;
}


void muse_video_avsync(bool on)
{
    (void)on;
}

void muse_video_avsync_tune(int late_ms, int resync_ms, int gap_ms, int max_skips)
{
    (void)late_ms, (void)resync_ms, (void)gap_ms, (void)max_skips;
}

int muse_video_avsync_status(char *out, size_t cap)
{
    return snprintf(out, cap, "not used on this board (it shows every picture)");
}

void muse_video_set_overlay(muse_video_overlay_t overlay)
{
    s_overlay = overlay;
}

/* A piece of the frame, then whatever is drawn over it. */
static void fill(uint8_t *dst, int stride, int row, int rows, void *arg);

/* Bench (muse_video_shot): the next frame as the panel gets it, captured. */
static uint8_t *s_shot;
static volatile int s_shot_state;   /* 0 idle, 1 asked, 2 capturing this frame, 3 done */
static volatile bool s_shot_whole;  /* s_shot holds a whole captured frame (muse_video_shot_last) */

static void fill_piece(uint8_t *dst, int stride, int row, int rows, void *arg)
{
    int64_t t0 = esp_timer_get_time();
    fill(dst, stride, row, rows, arg);
    int64_t t1 = esp_timer_get_time();
    s_fill_us += (uint32_t)(t1 - t0);
    const fill_arg_t *a = arg;
    muse_video_overlay_t overlay = s_overlay;
    if (overlay) {
        overlay(dst, stride, a->x, a->y + row, a->out_w, rows);
    }
    s_over_us += (uint32_t)(esp_timer_get_time() - t1);
    if (s_shot_state == 2) {
        int W = muse_board->width, H = muse_board->height;
        for (int r = 0; r < rows; r++) {
            int py = a->y + row + r;
            if (py < 0 || py >= H) {
                continue;
            }
            int w = a->x + a->out_w > W ? W - a->x : a->out_w;
            memcpy(s_shot + ((size_t)py * W + a->x) * 2, dst + (size_t)r * stride, (size_t)w * 2);
        }
    }
}

static bool shot_buffer(void)
{
    if (!s_shot) {
        s_shot = heap_caps_malloc((size_t)muse_board->width * muse_board->height * 2, BIG_CAPS);
    }
    return s_shot != NULL;
}

bool muse_video_shot_last(const uint8_t **px, int *w, int *h)
{
    if (!s_shot || !muse_board || !s_shot_whole || s_shot_state == 1 || s_shot_state == 2) {
        return false;   /* (a shot that timed out leaves no picture: the buffer never held a whole one) */
    }
    *px = s_shot;
    *w = muse_board->width;
    *h = muse_board->height;
    return true;
}

void muse_video_shot_arm(void)
{
    if (muse_board && s_shot_state == 0 && shot_buffer()) {
        s_shot_state = 1;   /* the next frame drawn, kept until muse_video_shot takes it */
    }
}

bool muse_video_shot(const uint8_t **px, int *w, int *h, int timeout_ms)
{
    int W = muse_board->width, H = muse_board->height;
    if (!shot_buffer()) {
        return false;
    }
    if (s_shot_state != 3) {
        s_shot_state = 1;
    }
    for (int t = 0; t < timeout_ms && s_shot_state != 3; t += 10) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (s_shot_state != 3) {
        s_shot_state = 0;
        return false;
    }
    s_shot_state = 0;
    *px = s_shot;
    *w = W;
    *h = H;
    return true;
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
     * the S3 half of it, two times upscaled; a round S3 panel enough for the
     * 2x picture to cover the circle (208 on the Watcher's 412, 240 on 466). */
    int src = muse_video_suggest(w, h, b->round, MAX_SRC);
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
        .tile_split_pct = TILE_SPLIT_PCT,   /* video's bands on the S31, UI's everywhere */
        .vp8 = muse_vdec_has(MUSE_VIDEO_CODEC_VP8),
        .no_h264 = !muse_vdec_has(MUSE_VIDEO_CODEC_H264),
        /* UI streams: the panel's own size, one to one, in two bands. */
        .ui_w = (uint16_t)(w < UI_MAX ? w : UI_MAX),
        .ui_h = (uint16_t)(h < UI_MAX ? h : UI_MAX),
        .ui_fps = UI_FPS,
        .ui_max_frame = AU_MAX,
        .ui_tiles = UI_BANDS,
        /* Lossless still patches over the frame buffer (muse_ui_fb.c), where the ROM's inflate works. */
        .ui_still = muse_ui_fb_can_inflate() ? 1u << 1 : 0,   /* mg_video_still_rgb565_zlib */
    };
    return &s_caps;
}


/* ---- drawing (UI core) -------------------------------------------------- */

/* A round panel's cover (muse_video_fit.h): the whole panel from the 2x
 * picture, cropped or edge extended; one band. */
static void fill_cover(uint8_t *dst, int stride, int row, int rows, const fill_arg_t *a)
{
    slot_t *sl = a->s;
    const muse_pic_t *pp = &sl->p0;
    int need = muse_video_cover_src_rows(&a->fit, row, rows);
    while (sl->rows < need && !sl->done) {
        vTaskDelay(1);   /* the decoder is a macroblock row or so behind */
    }
    if (!sl->pic) {
        return;
    }
    if (need > pp->h || a->vis_w > pp->w || sl->failed || !s_cov.buf) {
        memset(dst, 0, (size_t)rows * stride);
        return;
    }
    muse_video_cover_fill(&s_cov, &a->fit, muse_yuv_up2, pp->y, pp->u, pp->v, pp->y_stride, pp->c_stride, row, rows,
                          dst, stride);
}

/*
 * "No black bars" where the picture can't be cover-scaled (two bands,
 * or wider than the fill takes): the window is the whole panel, the picture
 * goes where the plain placement puts it (fit.ix, iy, iw x ih), and each
 * row outside it repeats its first or last row, each column its first or
 * last column. A row at a time through fill() (two bands included).
 */
static void fill_extend(uint8_t *dst, int stride, int row, int rows, const fill_arg_t *a)
{
    const muse_video_fit_t *f = &a->fit;
    fill_arg_t in = *a;
    in.fit.extend = false;
    in.out_w = f->iw;
    in.out_h = f->ih;
    in.x = a->x + f->ix;
    in.y = a->y + f->iy;
    for (int r = 0; r < rows; r++) {
        int ir = row + r - f->iy;
        ir = ir < 0 ? 0 : ir >= f->ih ? f->ih - 1 : ir;
        uint16_t *d = (uint16_t *)(dst + (size_t)r * stride);
        fill((uint8_t *)(d + f->ix), stride, ir, 1, &in);
        for (int x = 0; x < f->ix; x++) {
            d[x] = d[f->ix];
        }
        for (int x = f->ix + f->iw; x < a->out_w; x++) {
            d[x] = d[f->ix + f->iw - 1];
        }
    }
}

/* Black rows, each only as wide as the output: fill_extend hands fill() a
 * pointer inset into a buffer row, so a whole stride would run past it. */
static void fill_rows_black(uint8_t *dst, int stride, int rows, const fill_arg_t *a)
{
    size_t w = (size_t)a->out_w * 2;
    w = w < (size_t)stride ? w : (size_t)stride;
    for (int r = 0; r < rows; r++) {
        memset(dst + (size_t)r * stride, 0, w);
    }
}

static void fill(uint8_t *dst, int stride, int row, int rows, void *arg)
{
    fill_arg_t *a = arg;
    slot_t *sl = a->s;
    if (a->fit.extend) {
        fill_extend(dst, stride, row, rows, a);
        return;
    }
    if (a->fit.cover) {
        fill_cover(dst, stride, row, rows, a);
        return;
    }
    /* With two bands, the bottom part of the output comes from the bottom
     * band, as if it were a picture of its own (pieces never straddle). */
    int vis_h = a->vis_h, out_h = a->out_h;
    const muse_pic_t *pp = &sl->p0;
    const uint8_t *const volatile *picp = &sl->pic;
    volatile int *have = &sl->rows;
    if (sl->bands == 2) {
        /* The bands stack: the picture's rows [0, h0) are the top band's
         * (h0: its decoded rows; each band's SPS gives its own), the rest the
         * bottom band's. The seam falls on the converter's own granularity
         * (muse_video_band_seam), so each side of it is whole row pairs of
         * the converter, whatever h0 the stream's bands decode to. */
        int vis0, out0 = muse_video_band_seam(a->scale, vis_h, out_h, sl->h0, &vis0);
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
            while (!sl->pic1 && !sl->done) {
                vTaskDelay(1);
            }
            pp = &sl->p1;
            picp = &sl->pic1;
            have = &sl->rows1;
        } else {
            vis_h = vis0;
            out_h = out0;
        }
    }
    if (vis_h <= 0 || out_h <= 0 || rows <= 0 || row < 0 || row + rows > out_h) {
        fill_rows_black(dst, stride, rows, a);   /* a band with nothing of the picture: black */
        return;
    }
    const int sc = a->scale;
    int sr = sc == 1 || sc == 2 ? row / sc : 0;
    int need = sc == 1 || sc == 2 ? (row + rows + sc - 1) / sc
             : sc == 3 ? muse_yuv_up32_src_rows(vis_h, row + rows)
             : sc == MUSE_VIDEO_FIT_FILL
                 ? muse_yuv_fill_src_rows(vis_h, a->fit.fill_num, a->fit.fill_den, a->fit.crop_y, row + rows)
                 : muse_yuv_src_rows(vis_h, out_h, row + rows);
    while (*have < need && !sl->done) {
        vTaskDelay(1);   /* the decoder is a macroblock row or so behind */
    }
    const uint8_t *pic = *picp;
    /* Only rows the decoder has finished, and only what the decoded picture
     * holds: a stream whose pictures (or bands) are smaller than it said, or
     * that stopped decoding, is drawn black, not read past. */
    if (!pic || *have < need || need > pp->h || a->vis_w > pp->w || sl->failed) {
        fill_rows_black(dst, stride, rows, a);
        return;
    }
    const int ys = pp->y_stride, cs = pp->c_stride;
    const uint8_t *y = pp->y + (size_t)sr * ys;
    const uint8_t *u = pp->u + (size_t)(sr / 2) * cs;
    const uint8_t *v = pp->v + (size_t)(sr / 2) * cs;
    /* Each converter writes exactly this piece's rows (muse_yuv.h). */
    if (sc == MUSE_VIDEO_FIT_FILL) {
        muse_yuv_fill(pp->y, pp->u, pp->v, ys, cs, a->vis_w, vis_h, a->fit.fill_num, a->fit.fill_den, a->fit.crop_x,
                      a->fit.crop_y, a->out_w, row, rows, dst, stride);
    } else if (sc == 3) {
        muse_yuv_up32(pp->y, pp->u, pp->v, ys, cs, a->vis_w, vis_h, row, rows, dst, stride);
    } else if (sc == 0) {
        muse_yuv_scaled(pp->y, pp->u, pp->v, ys, cs, a->vis_w, vis_h, a->out_w, out_h, row, rows, dst, stride);
    } else if (sc == 2) {
        muse_yuv_up2(y, u, v, ys, cs, a->vis_w, rows, dst, stride);
    } else if (s_ui && s_hq) {
        /* A UI picture: bilinear chroma, rounding and dither (muse_yuv.h);
         * its chroma below the last row only from rows the picture has. */
        muse_yuv_1x_hq(pp->y, pp->u, pp->v, ys, cs, a->vis_w, vis_h < pp->h ? vis_h : pp->h, sr, rows, dst, stride);
    } else {
        muse_yuv_1x(y, u, v, ys, cs, a->vis_w, rows, dst, stride);
    }
}

/* ---- Changed-only redraw of streamed video (muse_vdirty.h) -----------
 *
 * The panel keeps its own memory (GRAM): while it holds picture N - 1, a
 * stream's picture N needs only the cells of the output that changed sent, as
 * a few windows (muse_board_t.draw_windows). The decode task takes each
 * picture's macroblock digests against the last picture's into its slot
 * (vd_slot_map); the show task folds in the pictures it skips (vd_fold),
 * maps the union since the picture on the panel through the scaler to the
 * output's cells, adds the overlays' rectangles when an overlay changed
 * (where it draws now and where it drew with the last picture), and draws
 * the windows. A picture that changed nothing (a repeat, a held picture)
 * writes nothing. A picture is drawn whole after
 * anything else wrote the panel (LVGL, a still, the ring's repaint, a piece
 * given up: muse_board_t.panel_writes), a mic ring change, a new place or
 * size, a keyframe, a broken chain of pictures, or windows over full_pct of
 * it (80 %) or more than max_windows (24) (>vdirty.tune=). Scales: the
 * fill. Two bands, UI streams (muse_ui_fb has its own changed rectangles),
 * and the other scales draw whole. >vdirty=0: every
 * picture whole (A/B).
 */
#define VD_MAX_WIN 24
#define VD_OVL_MAX 4   /* rectangles from the overlay (plus one more source's) */
#define VD_ORECT_MAX (2 * (VD_OVL_MAX + 1))   /* before a draw and after it */
static int frame_x_align(void);
/* On by default; the block lives in PSRAM (the muse_lcd_bands boards: the W175 and the s3 camera build, the
 * StopWatch, the Watcher, the ESP-Mosaico). >vdirty=0|1 either way (bench rows, RAM only). */
static volatile bool s_vd_on = true;
static muse_vdirty_cfg_t s_vd_cfg = { .win_cost = 2, .full_pct = 80, .max_windows = VD_MAX_WIN };
static volatile muse_video_overlay_rects_t s_ovl_rects;

/* The place a picture is drawn at, as the cells and the panel's picture depend on it. */
typedef struct {
    int x, y, out_w, out_h, vis_w, vis_h, num, den, cx, cy, scale, cover;
} vd_place_t;
/* Why a picture was drawn whole, for the 2 s line. */
enum { VD_KEY, VD_CHAIN, VD_PANEL, VD_RING, VD_PLACE, VD_AREA, VD_MANY, VD_PATH, VD_OFF, VD_SHADOW, VD_MEM, VD_IDLE,
       VD_WHY_N };
#define VD_TAG_LEN 48
/* One block a stream (internal RAM), taken by the decode task at the first picture it digests, after the decoder
 * and its pictures, and freed with the decoder. The show task uses it only while it holds a slot. */
/* A slot's picture's changed map (smap[slot]), made by the decode task before done: against picture base;
 * dvalid false: unknown (all of it). seq: the picture's own number (muse_pic_t.seq). */
typedef struct {
    int cols, rows;
    bool dvalid;
    uint32_t seq, base;
} vd_sd_t;

typedef struct {
    int cap;                        /* macroblocks each map holds */
    /* The shadow compare's buffers, inside the block when reserved with it at the stream's start (never taken
     * mid-stream): sh_cells cells of model and check, and a panel row. NULL: none reserved. */
    muse_vdirty_hash_t *sh_model;
    uint8_t *sh_strip;
    int sh_cells;
    /* The decode task's. */
    int cols, rows;                 /* dg's macroblock grid */
    uint32_t seq;                   /* the picture dg holds (muse_pic_t.seq; 0: none) */
    muse_vdirty_hash_t *dg;
    uint8_t *smap[2];               /* each slot's changed map */
    vd_sd_t sd[2];                  /* ...and what it is */
    /* The show task's. */
    uint8_t *u;                     /* the union of changed maps since the picture on the panel */
    uint8_t *cells;
    muse_vdirty_axis_t ax, ay;
    muse_board_win_t win[VD_MAX_WIN];   /* (muse_vdirty_win_t is the same) */
    /* The show task's, about the picture on the panel (the block's, not static: no RAM for good; a new block
     * starts with the panel's picture unknown). */
    bool shown_init;                /* the show task has seen this block */
    bool known;                     /* the panel holds picture top (else: unknown, draw whole) */
    bool have;                      /* u holds a union (else nothing folded yet) */
    uint32_t top;                   /* the last picture drawn or folded in (muse_pic_t.seq) */
    int cap_x, cap_y;               /* the picture's top left on the panel, for the shadow's capture */
    uint32_t writes;                /* the panel's write count after the video's last picture */
    uint32_t ring;                  /* the mic ring's generation then */
    bool ring_on;
    bool axes, axes_cut;            /* ax, ay are good for akey's place / they were cut */
    vd_place_t akey;                /* the place ax, ay were cut for */
    vd_place_t dplace;              /* the place of the picture on the panel (vd_drawn) */
    muse_board_win_t orect[VD_ORECT_MAX];   /* the overlays' rectangles drawn with it */
    uint8_t okind[VD_ORECT_MAX];    /* each one's source: 1 the second source, 0 the overlay */
    int norect;
    uint32_t sgen, ogen;            /* the second source's and the overlay's gen then */
    bool ovl_force;                 /* an overlay changed during that draw: its rectangles are redrawn */
    int fx, fw, fy;                 /* fill_win: the window being drawn (its next row) */
    /* The last overlay / ring / size change, to tag the shadow's compares after it ("after controls off"). */
    char tag[VD_TAG_LEN];
    int tag_left;                   /* compares still tagged with it */
    uint32_t tag_state;             /* what showed (bits: second source, overlay kinds, ring) */
    int tag_w, tag_h;
    /* The 2 s line's counts (show and decode tasks; the decode task reads and clears them). */
    volatile uint32_t dg_us, dg_n, mb_chg, mb_all;   /* digests: time, pictures, MBs */
    volatile uint32_t why[VD_WHY_N];
    volatile uint32_t n_win, n_none, n_full, cell_pm, area_pm, wins, us_win, us_full, fill_win_us, fill_full_us,
        ovl_cells;
    /* The board's own timing of each draw (muse_video_panel_timing: lcd_bands' TE hold), windowed and whole. */
    bool drawing;
    uint32_t last_te, te_win, te_full, te_n_win, te_n_full;
    uint32_t hidden_cells;          /* dirty cells a round panel hides, not pushed */
} vd_mem_t;
static vd_mem_t *volatile s_vd;

/* The show task's: the picture the panel holds and what changed since. */
static const char *const s_vd_why_name[VD_WHY_N] = { "key", "chain", "panel", "ring", "place", "area",
                                                     "many", "path", "off", "shadow", "memory", "idle" };
/*
 * Adaptive (a phone camera's video can change
 * most of its macroblocks, so nearly every picture goes whole and the digests
 * only cost time): after VD_IDLE_AFTER_US of pictures drawn whole
 * because most of them changed, the decode task stops digesting and every
 * picture is drawn whole at no cost, until a keyframe or s_vd_retry_t
 * (backing off from VD_RETRY_MS to VD_RETRY_MAX_MS while it keeps
 * failing). A windowed or unchanged picture resets it all.
 */
#define VD_IDLE_AFTER_US 2000000
#define VD_RETRY_MS 4000
#define VD_RETRY_MAX_MS 16000
static volatile bool s_vd_idle;     /* the show task's; the decode task reads it */
static int64_t s_vd_run_t, s_vd_retry_t;   /* the show task's: the run of whole pictures's start; idle until */
static uint16_t s_vd_backoff_ms = VD_RETRY_MS;
static int8_t s_vd_last_why = -1;   /* vd_plan's reason for the last picture drawn whole (-1: windows) */

/* Shadow compare (>vdirty.shadow=N): a digest per cell of what the panel was sent (muse_video_piece_sent, after
 * the overlays, the ring and the dot), against the picture converted whole with them after each windowed one. */
static volatile int s_vd_shadow;    /* windowed pictures left to compare */
static muse_vdirty_hash_t *s_vd_model, *s_vd_check;
static uint8_t *s_vd_strip;
static int s_vd_mcells;             /* cells the model holds (0: none) */
static bool s_vd_model_ok;          /* a whole picture has gone into it since its cells were laid out */
static volatile bool s_vd_capture;  /* the video's own draw is on the panel (muse_video_piece_sent takes it) */
static volatile bool s_vd_cap_cut;  /* ...and a piece cut cells (the model can't follow it) */
static bool s_vd_follow;            /* vd_plan: the cells are this picture's place (the model may follow its draw) */
static uint32_t s_vd_cmp_n, s_vd_cmp_bad, s_vd_cmp_cells, s_vd_cmp_px, s_vd_cmp_skip, s_vd_cmp_tagged,
    s_vd_cmp_tagged_bad;
static int s_vd_bad_x = -1, s_vd_bad_y = -1;
static uint16_t s_vd_bad_number;
/* The last overlay / ring / size change, to tag the compares after it ("after controls off"). */
#define VD_TAG_PICS 5

void muse_video_set_overlay_rects(muse_video_overlay_rects_t rects)
{
    s_ovl_rects = rects;
}

void muse_video_set_dirty(bool on)
{
    s_vd_on = on;
}

void muse_video_dirty_tune(int win_cost, int full_pct, int max_windows)
{
    if (win_cost > 0) {
        s_vd_cfg.win_cost = win_cost;
    }
    if (full_pct > 0) {
        s_vd_cfg.full_pct = full_pct > 100 ? 100 : full_pct;
    }
    if (max_windows > 0) {
        s_vd_cfg.max_windows = max_windows > VD_MAX_WIN ? VD_MAX_WIN : max_windows;
    }
}

void muse_video_dirty_shadow(int frames)
{
    s_vd_cmp_n = s_vd_cmp_bad = s_vd_cmp_cells = s_vd_cmp_px = s_vd_cmp_skip = 0;
    s_vd_cmp_tagged = s_vd_cmp_tagged_bad = 0;
    s_vd_bad_x = s_vd_bad_y = -1;
    s_vd_shadow = frames > 0 ? frames : 0;   /* the show task makes (and frees) the buffers */
}

static bool vd_shadow_waiting(void);

void muse_video_dirty_status(char *buf, size_t n)
{
    snprintf(buf, n,
             "%s%s, window cost %d cells, whole over %d%% or %d windows; shadow: %u compared, %u differ (%u cells, "
             "%u px; first frame %u at %d,%d), %u after a change (%u differ), %u skipped (overlay changed during "
             "it), %d left%s",
             s_vd_on ? "on" : "off", s_vd_on && s_vd_idle ? " (idle: most of every picture changes)" : "",
             s_vd_cfg.win_cost, s_vd_cfg.full_pct, s_vd_cfg.max_windows, (unsigned)s_vd_cmp_n,
             (unsigned)s_vd_cmp_bad, (unsigned)s_vd_cmp_cells, (unsigned)s_vd_cmp_px, (unsigned)s_vd_bad_number,
             s_vd_bad_x, s_vd_bad_y, (unsigned)s_vd_cmp_tagged, (unsigned)s_vd_cmp_tagged_bad,
             (unsigned)s_vd_cmp_skip, s_vd_shadow,
             vd_shadow_waiting() ? " (from the next stream: this one's block has no room for the compare)" : "");
}

/* The decode task: the stream's block goes (both slots its own, or the stream ended). */
static void vd_free(void)
{
    vd_mem_t *m = s_vd;
    s_vd = NULL;
    heap_caps_free(m);
    s_vd_idle = false;   /* a new stream starts digesting (the show task resets the rest with the block) */
    if (m) {
        ESP_LOGI(TAG, "vdirty: stream over (internal %u free, largest %u)",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    }
}

/* One block for pictures of up to n macroblocks, with the shadow's buffers for sh_cells cells if sh_cells > 0
 * (NULL: no room). PSRAM where there is some, else internal RAM. */
static vd_mem_t *vd_alloc(int n, int sh_cells)
{
    size_t a = (sizeof(vd_mem_t) + 15) & ~(size_t)15;
    size_t mn = ((size_t)n + 3) & ~(size_t)3;
    size_t sz = a + (size_t)n * sizeof(muse_vdirty_hash_t) + 4 * mn;
    size_t sh = sh_cells > 0 ? 2 * (size_t)sh_cells * sizeof(muse_vdirty_hash_t) : 0;
    size_t strip = sh_cells > 0 ? ((size_t)muse_board->width * 2 + 15) & ~(size_t)15 : 0;
    sz = ((sz + 15) & ~(size_t)15) + sh + strip;
    vd_mem_t *m = heap_caps_aligned_alloc(16, sz, BIG_CAPS);
    if (!m) {
        return NULL;
    }
    memset(m, 0, sizeof(*m));
    m->cap = n;
    m->dg = (muse_vdirty_hash_t *)((uint8_t *)m + a);
    uint8_t *p = (uint8_t *)(m->dg + n);
    m->smap[0] = p;
    m->smap[1] = p + mn;
    m->u = p + 2 * mn;
    m->cells = p + 3 * mn;
    if (sh_cells > 0) {
        uint8_t *q = (uint8_t *)m + (((a + (size_t)n * sizeof(muse_vdirty_hash_t) + 4 * mn) + 15) & ~(size_t)15);
        m->sh_model = (muse_vdirty_hash_t *)q;
        m->sh_strip = q + sh;   /* (16-byte aligned: sh is a multiple of 16) */
        m->sh_cells = sh_cells;
    }
    ESP_LOGI(TAG, "vdirty: %u bytes for %d macroblocks%s (internal %u free, largest %u)", (unsigned)sz, n,
             sh_cells > 0 ? " and the shadow compare" : "", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    return m;
}


/* A compare asked that can't run in this stream (the shadow's buffers taken only with the stream's block, never
 * mid-stream): the console says it starts with the next stream. Otherwise they are taken at the next windowed
 * picture. */
static bool vd_shadow_waiting(void)
{
    return false;
}

/* The decode task: the stream's block for pictures of cols x rows macroblocks (NULL: no room). */
static vd_mem_t *vd_mem(int cols, int rows)
{
    vd_mem_t *m = s_vd;
    int n = cols * rows;
    if (m) {
        return n <= m->cap ? m : NULL;
    }
    s_vd = vd_alloc(n, 0);
    return s_vd;
}

/* The decode task: nothing chains to the last picture (a failed decode may have touched it). */
static void vd_break(void)
{
    vd_mem_t *m = s_vd;
    if (m) {
        m->seq = 0;
    }
}

/* The decode task, a picture decoded into slot k (before done): which of its macroblocks changed since the
 * picture decoded before it (dvalid false: unknown, all of it). */
static void vd_slot_map(int k, const muse_pic_t *pic)
{
    slot_t *sl = &s_slot[k];
    if (s_ui || sl->bands != 1 || !muse_board->draw_windows || !muse_board->panel_writes ||
        !pic->y || pic->w <= 0 || pic->h <= 0) {
        vd_break();
        return;
    }
    if (!s_vd_on || s_vd_idle) {
        /* Off (>vdirty=0: no block, no RAM) or idle (adaptive): no digests; the chain starts over after. */
        vd_mem_t *m0 = s_vd;
        if (m0) {
            m0->sd[k].seq = pic->seq;
            m0->sd[k].dvalid = false;
            m0->seq = 0;
        }
        return;
    }
    int cols = (pic->w + 15) / 16, rows = (pic->h + 15) / 16;
    vd_mem_t *m = vd_mem(cols, rows);
    if (!m) {
        return;
    }
    vd_sd_t *sd = &m->sd[k];
    sd->seq = pic->seq;
    sd->dvalid = false;
    int64_t t0 = esp_timer_get_time();
    bool prev_ok = m->seq && m->cols == cols && m->rows == rows;
    /* The decoder's own map (H.264 on PSRAM boards: muse_vdec_same) lines up with the picture's grid when it is
     * against the picture the digests hold. */
    const uint8_t *same = prev_ok && pic->same && pic->same_as == m->seq && pic->same_cols >= cols ? pic->same : NULL;
    uint8_t *map = m->smap[k];
    int chg = muse_vdirty_digest(pic->y, pic->u, pic->v, pic->y_stride, pic->c_stride, pic->w, pic->h, m->dg, prev_ok,
                                 same, pic->same_cols, map);
    m->dg_us += (uint32_t)(esp_timer_get_time() - t0);
    m->dg_n++;
    m->mb_chg += (uint32_t)chg;
    m->mb_all += (uint32_t)(cols * rows);
    sd->cols = cols;
    sd->rows = rows;
    sd->base = m->seq;
    sd->dvalid = prev_ok;
    m->cols = cols;
    m->rows = rows;
    m->seq = pic->seq;
}

/* The show task: the block the slot's map is in, if it's the stream's now (NULL: none). Resets the show task's
 * state on a new block. */
static vd_mem_t *vd_show_mem(void)
{
    vd_mem_t *m = s_vd;
    if (m && !m->shown_init) {
        m->shown_init = true;
        s_vd_model_ok = false;   /* (the block's own state starts zeroed: the panel's picture unknown) */
        s_vd_run_t = 0;
        s_vd_backoff_ms = VD_RETRY_MS;
    }
    return m;
}

/* The decode task: slot k's picture has no changed map (a failed decode, or about to decode into it). */
static void vd_slot_none(int k)
{
    vd_mem_t *m = s_vd;
    if (m) {
        m->sd[k].dvalid = false;
    }
}

/* The show task: a picture decoded but not drawn (skipped): its changes join the next one drawn. */
static void vd_fold(const slot_t *sl)
{
    vd_mem_t *m = vd_show_mem();
    if (!m) {
        return;
    }
    int k = (int)(sl - s_slot);
    const vd_sd_t *sd = &m->sd[k];
    if (m->known) {
        int n = sd->cols * sd->rows;
        if (!sd->dvalid || sd->base != m->top) {
            m->known = false;
        } else if (!m->have) {
            memcpy(m->u, m->smap[k], (size_t)n);
            m->have = true;
        } else {
            muse_vdirty_or(m->u, m->smap[k], n);
        }
    }
    m->top = sd->seq;
}

/* The show task: the panel's picture is no longer known (it showed something else, or a picture was dropped). */
static void vd_invalidate(const slot_t *sl)
{
    vd_mem_t *m = vd_show_mem();
    if (m) {
        m->known = false;
        m->top = m->sd[sl - s_slot].seq;
    }
}

/* The overlays' rectangles now (panel coordinates), each one's source (kind: 1 the second source, 0 the
 * overlay) and their gens; returns how many. */
static int vd_overlays(muse_board_win_t *r, uint8_t *kind, uint32_t *sgen, uint32_t *ogen)
{
    int n = 0;
    *sgen = *ogen = 0;
    muse_video_overlay_rects_t f = s_ovl_rects;
    if (f) {
        int k = f(r + n, VD_OVL_MAX, ogen);
        for (int i = 0; i < k; i++) {
            kind[n++] = 0;
        }
    }
    return n;
}

/* Everything drawn over the picture, as one number: a change between a draw and its compare skips the compare. */
static uint32_t vd_token(uint32_t sgen, uint32_t ogen, bool ring_on)
{
    return sgen * 31u + ogen * 7u + muse_mic_ring_generation() * 3u + (ring_on ? 1u : 0u);
}

/* What showed, for the shadow compare's tags (bit 0 the second source, 1-3 the overlay's, 4 the ring). */
static uint32_t vd_state(uint32_t sgen, uint32_t ogen, bool ring)
{
    return (sgen & 3u ? 1u : 0u) | (ogen & 7u) << 1 | (ring ? 16u : 0u);
}

static void vd_tag_change(uint32_t was, uint32_t now, int w, int h)
{
    vd_mem_t *m = s_vd;
    static const char *const name[5] = { "second source", "overlay", "overlay 2", "bench mark", "mic ring" };
    char t[sizeof(((vd_mem_t *)0)->tag)];
    int o = 0;
    t[0] = 0;
    for (int i = 0; i < 5; i++) {
        if ((was ^ now) >> i & 1u) {
            o += snprintf(t + o, sizeof(t) - (size_t)o, "%s%s %s", o ? ", " : "", name[i], now >> i & 1u ? "on" : "off");
            o = o < (int)sizeof(t) ? o : (int)sizeof(t) - 1;
        }
    }
    if ((w != m->tag_w || h != m->tag_h) && m->tag_w) {
        snprintf(t + o, sizeof(t) - (size_t)o, "%ssize %dx%d", o ? ", " : "", w, h);
    }
    if (t[0]) {
        strlcpy(m->tag, t, sizeof(((vd_mem_t *)0)->tag));
        m->tag_left = VD_TAG_PICS;
    }
}

/* fill and the shadow's reference: the scales changed-only redraw takes. */
static bool vd_scale_ok(const fill_arg_t *a)
{
    if (a->fit.extend) {
        return false;   /* edges extended (two bands): whole */
    }
    return a->fit.cover || a->scale == MUSE_VIDEO_FIT_FILL || a->scale == 1 || a->scale == 2;
}

/* The scaler as muse_vdirty_src takes it, and the cells' alignment, for each scale vd_scale_ok takes. */
static void vd_scaler(const fill_arg_t *a, int *num, int *den, int *cx, int *cy, int *ax_al, int *ay_al)
{
    int al = frame_x_align();
    if (a->fit.cover) {
        *num = 2, *den = 1, *cx = a->fit.crop_x, *cy = a->fit.crop_y;   /* muse_video_cover_fill_win: any column */
        *ax_al = al, *ay_al = 2;
    } else if (a->scale == MUSE_VIDEO_FIT_FILL) {
        *num = a->fit.fill_num, *den = a->fit.fill_den, *cx = a->fit.crop_x, *cy = a->fit.crop_y;
        *ax_al = al, *ay_al = 2;
    } else if (a->scale == 2) {
        /* muse_yuv_up2: a window on whole source pixel pairs (4 columns), whole source row pairs (4 rows). */
        *num = 2, *den = 1, *cx = *cy = 0;
        *ax_al = al > 4 ? al : 4, *ay_al = 4;
    } else {
        *num = *den = 1, *cx = *cy = 0;   /* one to one: muse_yuv_1x on whole pairs */
        *ax_al = al, *ay_al = 2;
    }
}


static vd_place_t vd_place(const fill_arg_t *a)
{
    return (vd_place_t){ a->x, a->y, a->out_w, a->out_h, a->vis_w, a->vis_h, a->fit.fill_num, a->fit.fill_den,
                         a->fit.crop_x, a->fit.crop_y, a->scale, a->fit.cover };
}

/* The panel's picture is at the same place (and scale) as this one. */
static bool vd_same_place(const fill_arg_t *a)
{
    vd_mem_t *m = s_vd;
    vd_place_t p = vd_place(a);
    return !memcmp(&p, &m->dplace, sizeof(p));
}

/* The output's cells for the picture's place, cut again when it moved; false: it has none (too many cells, or
 * more than the block holds). */
static bool vd_axes(vd_mem_t *m, const fill_arg_t *a)
{
    vd_place_t p = vd_place(a);
    if (m->axes_cut && !memcmp(&p, &m->akey, sizeof(p))) {
        return m->axes;
    }
    m->akey = p;
    m->axes_cut = true;
    int num, den, cx, cy, xa, ya;
    vd_scaler(a, &num, &den, &cx, &cy, &xa, &ya);
    m->axes = muse_vdirty_axis(&m->ax, a->out_w, a->vis_w, num, den, cx, xa) &&
              muse_vdirty_axis(&m->ay, a->out_h, a->vis_h, num, den, cy, ya) && m->ax.n * m->ay.n <= m->cap;
    s_vd_model_ok = false;   /* its cells moved */
    return m->axes;
}

/* The shadow's buffers (made when a compare is asked, freed when it's done); true while one runs. */
static bool vd_shadow_ready(vd_mem_t *m, const fill_arg_t *a)
{
    int cells = m && m->axes ? m->ax.n * m->ay.n : 0;
    if (s_vd_shadow <= 0 || !cells) {
        if (s_vd_model) {
            heap_caps_free(s_vd_model);
            heap_caps_free(s_vd_strip);
            s_vd_model = s_vd_check = NULL;
            s_vd_strip = NULL;
            s_vd_mcells = 0;
        }
        return false;
    }
    if (s_vd_model && cells > s_vd_mcells) {
        heap_caps_free(s_vd_model);
        heap_caps_free(s_vd_strip);
        s_vd_model = s_vd_check = NULL;
        s_vd_strip = NULL;
    }
    if (!s_vd_model) {
        s_vd_model = heap_caps_malloc(2 * (size_t)cells * sizeof(muse_vdirty_hash_t), BIG_CAPS);
        s_vd_strip = heap_caps_aligned_alloc(16, (size_t)muse_board->width * 2, BIG_CAPS);
        if (!s_vd_model || !s_vd_strip) {
            heap_caps_free(s_vd_model);
            heap_caps_free(s_vd_strip);
            s_vd_model = s_vd_check = NULL;
            s_vd_strip = NULL;
            ESP_LOGW(TAG, "vdirty shadow: no memory for %d cells: not compared", cells);
            s_vd_shadow = 0;
            return false;
        }
        s_vd_check = s_vd_model + cells;
        s_vd_mcells = cells;
        s_vd_model_ok = false;
    }
    (void)a;
    return true;
}

/* The show task, the stream over: the shadow's buffers go (the next stream's decoder needs the room). */
static void vd_show_idle(void)
{
    if (s_vd_model) {
        heap_caps_free(s_vd_model);
        heap_caps_free(s_vd_strip);
        s_vd_model = s_vd_check = NULL;
        s_vd_strip = NULL;
        s_vd_mcells = 0;
        s_vd_model_ok = false;
    }
}

void muse_video_panel_timing(uint32_t te_hold_us, uint32_t write_us)
{
    vd_mem_t *m = s_vd;
    (void)write_us;   /* (the draw's own time is measured around it) */
    if (m && m->drawing) {
        m->last_te += te_hold_us;
    }
}

void muse_video_piece_sent(const uint8_t *px, int stride, int x, int y, int w, int rows)
{
    vd_mem_t *m = s_vd;
    if (!s_vd_capture || !s_vd_model || !m || !m->axes) {
        return;
    }
    if (!muse_vdirty_model_rows(s_vd_model, &m->ax, &m->ay, px, stride, x - m->cap_x, y - m->cap_y, w, rows)) {
        s_vd_cap_cut = true;   /* a write that cuts cells: the model can't follow it */
    }
}

/* draw_windows' fill: the window's rows of the picture (as fill() makes them there), then the overlays. */
static void fill_win(uint8_t *dst, int stride, int x, int y, int w, int rows, void *arg)
{
    vd_mem_t *m = s_vd;
    const fill_arg_t *a = arg;
    const slot_t *sl = a->s;
    const muse_pic_t *pp = &sl->p0;
    int64_t t0 = esp_timer_get_time();
    bool first = !(x == m->fx && w == m->fw && y == m->fy);
    m->fx = x;
    m->fw = w;
    m->fy = y + rows;
    const int ys = pp->y_stride, cs = pp->c_stride;
    if (!sl->pic || sl->failed || !pp->y || x < 0 || w <= 0 || x + w > a->out_w || y < 0 || y + rows > a->out_h) {
        memset(dst, 0, (size_t)rows * stride);
    } else if (a->fit.cover) {
        muse_video_cover_fill_win(&s_cov, &a->fit, muse_yuv_up2, pp->y, pp->u, pp->v, ys, cs, x, w, y, rows, first,
                                  dst, stride);
    } else if (a->scale == MUSE_VIDEO_FIT_FILL) {
        muse_yuv_fill_win(pp->y, pp->u, pp->v, ys, cs, a->vis_w, a->vis_h, a->fit.fill_num, a->fit.fill_den,
                          a->fit.crop_x, a->fit.crop_y, x, w, y, rows, first, dst, stride);
    } else if (a->scale == 2) {
        /* x, w and y on whole fours (vd_scaler): source pixel and row pairs, as fill() takes them. */
        muse_yuv_up2(pp->y + (size_t)(y / 2) * ys + x / 2, pp->u + (size_t)(y / 4) * cs + x / 4,
                     pp->v + (size_t)(y / 4) * cs + x / 4, ys, cs, w / 2, rows, dst, stride);
    } else {
        muse_yuv_1x(pp->y + (size_t)y * ys + x, pp->u + (size_t)(y / 2) * cs + x / 2, pp->v + (size_t)(y / 2) * cs + x / 2,
                    ys, cs, w, rows, dst, stride);
    }
    int64_t t1 = esp_timer_get_time();
    s_fill_us += (uint32_t)(t1 - t0);
    muse_video_overlay_t overlay = s_overlay;
    if (overlay) {
        overlay(dst, stride, a->x + x, a->y + y, w, rows);
    }
    s_over_us += (uint32_t)(esp_timer_get_time() - t1);
}

/* What a round panel hides (the cover's corners) is never pushed again (static areas). */
static void vd_clip_round(vd_mem_t *m, const fill_arg_t *a)
{
    if (muse_board->round) {
        m->hidden_cells += (uint32_t)muse_vdirty_clip_round(m->cells, &m->ax, &m->ay, muse_board->width / 2 - a->x,
                                                            muse_board->height / 2 - a->y, muse_board->width / 2);
    }
}

/*
 * The show task, before drawing sl at a: the windows to draw (0: nothing
 * changed), or -1: draw it whole (why counted). The picture is whole (done).
 */
static int vd_plan(slot_t *sl, const fill_arg_t *a, const muse_board_win_t *orect, const uint8_t *okind, int norect,
                   uint32_t sgen, uint32_t ogen, bool ring_on)
{
    vd_mem_t *m = vd_show_mem();
    int why = -1;
    s_vd_follow = false;
    uint32_t state = vd_state(sgen, ogen, ring_on);
    if (m && (state != m->tag_state || a->vis_w != m->tag_w || a->vis_h != m->tag_h)) {
        vd_tag_change(m->tag_state, state, a->vis_w, a->vis_h);
        m->tag_state = state;
        m->tag_w = a->vis_w;
        m->tag_h = a->vis_h;
    }
    if (s_vd_idle && (sl->key || esp_timer_get_time() >= s_vd_retry_t)) {
        s_vd_idle = false;   /* try again: the decode task digests from the next picture */
        s_vd_run_t = 0;
    }
    if (!s_vd_on) {
        why = VD_OFF;
    } else if (s_vd_idle) {
        why = VD_IDLE;
    } else if (!muse_board->draw_windows || !muse_board->panel_writes || s_ui || sl->bands != 1 || !vd_scale_ok(a) ||
               s_shot_state != 0 || (s_overlay && !s_ovl_rects)) {
        why = VD_PATH;
    } else if (!m || !m->sd[sl - s_slot].dvalid) {
        why = sl->key ? VD_KEY : m ? VD_CHAIN : VD_MEM;
    } else if (!vd_axes(m, a)) {
        why = VD_MEM;
    } else if (!vd_same_place(a)) {
        why = VD_PLACE;
    } else if (sl->key) {
        why = VD_KEY;
    } else if (!m->known || m->sd[sl - s_slot].base != m->top) {
        why = VD_CHAIN;
    } else if (muse_board->panel_writes() != m->writes) {
        why = VD_PANEL;
    } else if (muse_mic_ring_generation() != m->ring || ring_on != m->ring_on) {
        why = VD_RING;
    } else if (vd_shadow_ready(m, a) && !s_vd_model_ok) {
        why = VD_SHADOW;   /* the shadow starts from a whole picture */
    }
    if (m && why != VD_PATH && why != VD_OFF) {
        s_vd_follow = vd_axes(m, a);   /* the cells for a whole picture too: the shadow's model follows it */
        vd_shadow_ready(m, a);
    }
    s_vd_last_why = (int8_t)why;
    if (why >= 0) {
        if (m) {
            m->why[why]++;
        }
        return -1;
    }
    const vd_sd_t *sd = &m->sd[sl - s_slot];
    const uint8_t *map = m->smap[sl - s_slot];
    int n = sd->cols * sd->rows;
    if (m->have) {
        muse_vdirty_or(m->u, map, n);
        map = m->u;
    }
    muse_vdirty_cells(map, sd->cols, sd->rows, &m->ax, &m->ay, m->cells);
    vd_clip_round(m, a);
    /* The overlays, when anything they draw changed since the picture on the panel (shown, hidden, new text, a
     * flip): where they draw now and where they drew. Unchanged, they need nothing of their own: a cell under one
     * that the video changes is drawn with it (fill_win), the rest of the panel holds them as they are. */
    int ncells = m->ax.n * m->ay.n, before = 0;
    for (int i = 0; i < ncells; i++) {
        before += m->cells[i];
    }
    bool ch[2] = { m->ovl_force || ogen != m->ogen, m->ovl_force || sgen != m->sgen };   /* by kind */
    for (int i = 0; i < norect; i++) {
        if (ch[okind[i]]) {
            muse_vdirty_mark(m->cells, &m->ax, &m->ay, orect[i].x - a->x, orect[i].y - a->y, orect[i].w, orect[i].h);
        }
    }
    for (int i = 0; i < m->norect; i++) {
        if (ch[m->okind[i]]) {
            muse_vdirty_mark(m->cells, &m->ax, &m->ay, m->orect[i].x - a->x, m->orect[i].y - a->y,
                             m->orect[i].w, m->orect[i].h);
        }
    }
    vd_clip_round(m, a);   /* (overlays in a corner too) */
    muse_vdirty_stats_t st;
    int nw = muse_vdirty_plan(m->cells, &m->ax, &m->ay, &s_vd_cfg, m->win, VD_MAX_WIN, &st);
    if (st.res == MUSE_VDIRTY_FULL_AREA || st.res == MUSE_VDIRTY_FULL_MANY) {
        s_vd_last_why = st.res == MUSE_VDIRTY_FULL_AREA ? VD_AREA : VD_MANY;
        m->why[s_vd_last_why]++;
        return -1;
    }
    s_vd_follow = true;
    if (st.cells > 0 && st.total > 0) {
        m->cell_pm += (uint32_t)st.dirty * 1000 / (uint32_t)st.cells;
        m->area_pm += (uint32_t)((uint64_t)st.area * 1000 / (uint32_t)st.total);
        m->ovl_cells += (uint32_t)(st.dirty - before);
    }
    m->wins += (uint32_t)nw;
    return nw;
}

/* The show task, after each picture drawn (nwin: vd_plan's answer): the adaptive pause (VD_IDLE_AFTER_US). */
static void vd_adapt(int nwin)
{
    int64_t now = esp_timer_get_time();
    if (nwin >= 0) {
        s_vd_run_t = 0;   /* windows (or nothing) paid: keep digesting */
        s_vd_backoff_ms = VD_RETRY_MS;
        return;
    }
    if (s_vd_last_why != VD_AREA && s_vd_last_why != VD_MANY) {
        return;   /* whole for another reason (a keyframe, a broken chain, the panel, the ring): no sign either way */
    }
    if (!s_vd_run_t) {
        s_vd_run_t = now;
    } else if (now - s_vd_run_t >= VD_IDLE_AFTER_US) {
        s_vd_idle = true;
        s_vd_retry_t = now + (int64_t)s_vd_backoff_ms * 1000;
        ESP_LOGI(TAG, "vdirty: most of every picture changes: drawn whole, no digests, for %u s (or until a keyframe)",
                 (unsigned)(s_vd_backoff_ms / 1000));
        s_vd_backoff_ms = s_vd_backoff_ms * 2 > VD_RETRY_MAX_MS ? VD_RETRY_MAX_MS : (uint16_t)(s_vd_backoff_ms * 2);
        s_vd_run_t = 0;
    }
}

/*
 * The show task, after drawing sl at a (whole or windows; drew: anything
 * went to the panel): the panel holds it. writes: the panel's write count
 * just before; the draw is one write, so anything more (a piece given up,
 * another writer) means the panel may not hold it after all. orect: the
 * overlays' rectangles before the draw and after it (anything that showed
 * meanwhile), drawn over it; ovl_moved: an overlay changed during the draw
 * (the next picture redraws their rectangles).
 */
static void vd_drawn(const slot_t *sl, const fill_arg_t *a, uint32_t writes, bool drew, uint32_t ring, bool ring_on,
                     const muse_board_win_t *orect, const uint8_t *okind, int norect, uint32_t sgen, uint32_t ogen,
                     bool ovl_moved)
{
    vd_mem_t *m = s_vd;
    if (!m) {
        return;
    }
    uint32_t now = muse_board->panel_writes ? muse_board->panel_writes() : 0;
    m->known = now == writes + (drew ? 1 : 0);
    m->have = false;
    m->top = m->sd[sl - s_slot].seq;
    m->ovl_force = ovl_moved;
    m->writes = now;
    m->ring = ring;
    m->ring_on = ring_on;
    m->dplace = vd_place(a);
    m->norect = norect < VD_ORECT_MAX ? norect : VD_ORECT_MAX;
    memcpy(m->orect, orect, (size_t)m->norect * sizeof(*orect));
    memcpy(m->okind, okind, (size_t)m->norect);
    m->sgen = sgen;
    m->ogen = ogen;
}

/*
 * After a windowed picture (or one with nothing changed), slot held: the
 * picture converted whole, row by row, with the overlays, the ring and the
 * dot as the panel gets them, against the model of everything the panel was
 * sent. tok0: the overlays' state before the draw; a change by now (an
 * overlay that moved on during the compare) skips it rather than call it a
 * difference.
 */
static void vd_shadow_check(const slot_t *sl, const fill_arg_t *a, int nwin, uint32_t tok0)
{
    vd_mem_t *m = s_vd;
    if (!m || !s_vd_model || !s_vd_model_ok) {
        return;
    }
    muse_board_win_t r[VD_OVL_MAX + 1];
    uint8_t kd[VD_OVL_MAX + 1];
    uint32_t sg, og;
    vd_overlays(r, kd, &sg, &og);
    bool ring = muse_mic_ring_on();
    if (vd_token(sg, og, ring) != tok0) {
        s_vd_cmp_skip++;
        return;
    }
    const int w = a->out_w, h = a->out_h, stride = w * 2, cells = m->ax.n * m->ay.n;
    s_cov.blk = -1;   /* the cover's block holds a window's columns */
    for (int row = 0; row < h; row++) {
        fill(s_vd_strip, stride, row, 1, (void *)a);
        muse_video_overlay_t overlay = s_overlay;
        if (overlay) {
            overlay(s_vd_strip, stride, a->x, a->y + row, w, 1);
        }
        muse_mic_ring_apply_as(ring, s_vd_strip, stride, a->x, a->y + row, w, 1);
        muse_turn_dot_apply(s_vd_strip, stride, a->x, a->y + row, w, 1);
        muse_vdirty_model_rows(s_vd_check, &m->ax, &m->ay, s_vd_strip, stride, 0, row, w, 1);
    }
    /* Again after: an overlay that changed while the rows were composed (by another task) is in some of them and
     * not in the picture drawn, so it's no evidence. */
    vd_overlays(r, kd, &sg, &og);
    if (vd_token(sg, og, muse_mic_ring_on()) != tok0) {
        s_vd_cmp_skip++;
        return;
    }
    uint32_t bad = 0, px = 0;
    int bx = -1, by = -1;
    for (int i = 0; i < cells; i++) {
        int cx = i % m->ax.n, cy = i / m->ax.n;
        if (muse_board->round && muse_vdirty_cell_hidden(&m->ax, &m->ay, cx, cy, muse_board->width / 2 - a->x,
                                                         muse_board->height / 2 - a->y, muse_board->width / 2)) {
            continue;   /* nothing of it shows: never pushed again (vd_plan) */
        }
        if (!muse_vdirty_hash_eq(&s_vd_check[i], &s_vd_model[i])) {
            bad++;
            px += (uint32_t)((m->ax.b[cx + 1] - m->ax.b[cx]) * (m->ay.b[cy + 1] - m->ay.b[cy]));
            if (bx < 0) {
                bx = m->ax.b[cx];
                by = m->ay.b[cy];
            }
        }
    }
    bool tagged = m->tag_left > 0;
    s_vd_cmp_n++;
    s_vd_cmp_tagged += tagged;
    if (bad) {
        s_vd_cmp_bad++;
        s_vd_cmp_tagged_bad += tagged;
        s_vd_cmp_cells += bad;
        s_vd_cmp_px += px;
        if (s_vd_bad_x < 0) {
            s_vd_bad_x = bx;
            s_vd_bad_y = by;
            s_vd_bad_number = sl->number;
        }
        if (s_vd_cmp_bad <= 8) {
            ESP_LOGW(TAG, "vdirty shadow: frame %u (%d windows%s%s) DIFFERS from the whole picture: %u cells (%u px), "
                     "first at %d,%d", sl->number, nwin, tagged ? ", after " : "", tagged ? m->tag : "",
                     (unsigned)bad, (unsigned)px, bx, by);
        }
    } else if (tagged) {
        ESP_LOGI(TAG, "vdirty shadow: frame %u (%d windows, after %s, %d of %d): the same as the whole picture",
                 sl->number, nwin, m->tag, VD_TAG_PICS - m->tag_left + 1, VD_TAG_PICS);
    }
    if (tagged) {
        m->tag_left--;
    }
    if (s_vd_cmp_n % 25 == 0 || s_vd_shadow == 1) {
        ESP_LOGI(TAG, "vdirty shadow: %u compared, %u differ (%u cells, %u px), %u after a change (%u differ), %u "
                 "skipped", (unsigned)s_vd_cmp_n, (unsigned)s_vd_cmp_bad, (unsigned)s_vd_cmp_cells, (unsigned)s_vd_cmp_px,
                 (unsigned)s_vd_cmp_tagged, (unsigned)s_vd_cmp_tagged_bad, (unsigned)s_vd_cmp_skip);
    }
    if (--s_vd_shadow <= 0) {
        s_vd_shadow = 0;
        ESP_LOGI(TAG, "vdirty shadow done: %u windowed pictures compared with the whole picture, %s (%u px differ)",
                 (unsigned)s_vd_cmp_n, s_vd_cmp_bad ? "SOME DIFFER" : "all the same", (unsigned)s_vd_cmp_px);
    }
}

/*
 * Bench (console ui.dump): the last UI picture as decoded (Y, U, V: the
 * crop) and as drawn (RGB565 rows, the panel's byte order: big-endian), as
 * base64 lines on the console: "@uidump W H HQ", then "@Y n ...", "@U n ...",
 * "@V n ...", "@P n ..." per row, then "@uidump end". tools/muse/uidump.py
 * makes PNGs of them. Runs on the show task while the picture is still.
 */
static void dlock(void)
{
    portENTER_CRITICAL(&s_dlock_mux);
    if (!s_dlock) {
        s_dlock = xSemaphoreCreateMutexStatic(&s_dlock_buf);
    }
    portEXIT_CRITICAL(&s_dlock_mux);
    xSemaphoreTake(s_dlock, portMAX_DELAY);
}

static void dunlock(void)
{
    xSemaphoreGive(s_dlock);
}

static void dump_emit(const char *s, size_t n)
{
    dlock();
    if (!s_dump_to_buf) {
        dunlock();
        fwrite(s, 1, n, stdout);
        return;
    }
    if (s_dump_gen != s_dgen) {
        dunlock();
        return;   /* freed since this dump began: its lines go nowhere */
    }
    if (s_dlen + n > s_dcap) {
        size_t cap = s_dcap + 256 * 1024 > s_dlen + n ? s_dcap + 256 * 1024 : s_dlen + n;
        char *b = heap_caps_realloc(s_dbuf, cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!b) {
            dunlock();
            return;   /* the reader sees no "@uidump end" */
        }
        s_dbuf = b;
        s_dcap = cap;
    }
    memcpy(s_dbuf + s_dlen, s, n);
    s_dlen += n;
    dunlock();
}

/* The show task, at a ui.dump request: the dump is ready for a reader only if nothing freed the buffer meanwhile. */
static void dump_begin(void)
{
    dlock();
    s_dump_gen = s_dgen;
    dunlock();
}

static void dump_finish(void)
{
    dlock();
    s_dbuf_ready = s_dump_to_buf && s_dump_gen == s_dgen;
    dunlock();
}

static void dump_printf(const char *fmt, ...)
{
    char line[64];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n > 0) {
        dump_emit(line, n < (int)sizeof(line) ? (size_t)n : sizeof(line) - 1);
    }
}

static void b64_line(const char *tag, int n, const uint8_t *d, int len)
{
    static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    EXT_RAM_BSS_ATTR static char line[2600];   /* bench (ui.dump): PSRAM, not internal RAM */
    int o = snprintf(line, 16, "@%s %d ", tag, n);
    for (int i = 0; i < len && o < (int)sizeof(line) - 6; i += 3) {
        uint32_t v = (uint32_t)d[i] << 16 | (i + 1 < len ? d[i + 1] : 0) << 8 | (i + 2 < len ? d[i + 2] : 0);
        line[o++] = t[v >> 18 & 63];
        line[o++] = t[v >> 12 & 63];
        line[o++] = i + 1 < len ? t[v >> 6 & 63] : '=';
        line[o++] = i + 2 < len ? t[v & 63] : '=';
    }
    line[o++] = '\n';
    line[o] = 0;
    dump_emit(line, o);
}

static void dump_picture(const slot_t *sl, const fill_arg_t *arg)
{
    int w = arg->vis_w, h = arg->vis_h, h0 = sl->bands == 2 ? (sl->h0 < h ? sl->h0 : h) : h;
    /* 16-byte aligned, as the panel's pieces are: the vector conversion stores whole 16-byte words. */
    uint8_t *px = heap_caps_aligned_alloc(16, ((size_t)w * 2 * 2 + 15) & ~(size_t)15, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!px) {
        dump_printf("@uidump nomem\n");
        return;
    }
    dump_printf("@uidump %d %d %d\n", w, h, s_hq ? 1 : 0);
    for (int r = 0; r < h; r++) {
        const muse_pic_t *pp = r < h0 ? &sl->p0 : &sl->p1;
        int br = r < h0 ? r : r - h0;
        b64_line("Y", r, pp->y + (size_t)br * pp->y_stride, w);
        if (!(r & 1)) {
            b64_line("U", r / 2, pp->u + (size_t)(br / 2) * pp->c_stride, w / 2);
            b64_line("V", r / 2, pp->v + (size_t)(br / 2) * pp->c_stride, w / 2);
        }
        int fw, fh;
        const uint16_t *fb = muse_ui_fb_pixels(&fw, &fh);
        if (r % 2 == 1 && fb && fw == w && fh == h) {
            b64_line("P", r - 1, (const uint8_t *)(fb + (size_t)(r - 1) * w), w * 2);
            b64_line("P", r, (const uint8_t *)(fb + (size_t)r * w), w * 2);
        } else if (r % 2 == 1) {
            fill_arg_t a = *arg;
            fill(px, w * 2, r - 1, 2, &a);
            b64_line("P", r - 1, px, w * 2);
            b64_line("P", r, px + w * 2, w * 2);
        }
        if ((r & 15) == 15) {
            fflush(stdout);
            vTaskDelay(1);
        }
    }
    dump_printf("@uidump end\n");
    fflush(stdout);
    free(px);
}

void muse_video_dump(void)
{
    dlock();
    s_dump_to_buf = false;
    dunlock();
    s_dump_req = true;
}

/* Frees the buffer (under the lock): a dump still being written drops its lines and is never marked ready. */
static void dbuf_free_locked(void)
{
    s_dgen++;
    s_dbuf_ready = false;
    free(s_dbuf);
    s_dbuf = NULL;
    s_dlen = s_dcap = 0;
}

void muse_video_dump_to_buffer(void)
{
    dlock();
    dbuf_free_locked();
    s_dump_to_buf = true;
    dunlock();
    s_dump_req = true;
}

void muse_video_dump_release(void)
{
    dlock();
    dbuf_free_locked();
    dunlock();
}

size_t muse_video_dump_read(size_t off, void *out, size_t cap, size_t *total)
{
    size_t n = 0;
    dlock();
    *total = s_dbuf_ready ? s_dlen : 0;
    if (s_dbuf_ready && off < s_dlen) {
        n = s_dlen - off < cap ? s_dlen - off : cap;
        memcpy(out, s_dbuf + off, n);
    }
    dunlock();
    return n;
}

void muse_video_set_hq(bool on)
{
    s_hq = on;
}

/*
 * The MD5 of a decoded picture: the visible crop (vis_w x vis_h), its Y
 * plane, then U, then V, each plane's two bands stacked into one.
 */
static void picture_md5(const slot_t *sl, int vis_w, int vis_h, char out[33])
{
    md5_context_t c;
    esp_rom_md5_init(&c);
    int h0 = sl->bands == 2 ? sl->h0 : vis_h;
    for (int plane = 0; plane < 3; plane++) {
        int div = plane ? 2 : 1;
        for (int band = 0; band < sl->bands; band++) {
            const muse_pic_t *pp = band ? &sl->p1 : &sl->p0;
            int rows = band ? vis_h - h0 : (h0 < vis_h ? h0 : vis_h);
            const uint8_t *base = plane == 0 ? pp->y : plane == 1 ? pp->u : pp->v;
            int stride = plane ? pp->c_stride : pp->y_stride;
            for (int r = 0; r < rows / div; r++) {
                esp_rom_md5_update(&c, base + (size_t)r * stride, (uint32_t)(vis_w / div));
            }
        }
    }
    uint8_t d[16];
    esp_rom_md5_final(d, &c);
    for (int i = 0; i < 16; i++) {
        static const char hx[] = "0123456789abcdef";
        out[2 * i] = hx[d[i] >> 4];
        out[2 * i + 1] = hx[d[i] & 15];
    }
    out[32] = 0;
}

void muse_video_set_md5(bool on)
{
    s_md5 = on;
}

/*
 * The show task's alone: it pauses LVGL before drawing and resumes it once
 * the pictures stop, and nothing else in video touches LVGL's worker. (A
 * resume from the view closing on another task would land mid-draw: LVGL then
 * flushes while video draws to the same panel, and a resume landing between
 * the show task's pause and the worker's acknowledgement leaves the pause
 * waiting for good: the screen stuck on the last frame, then a restart.)
 */
static void pause_lvgl(bool pause)
{
    if (pause == s_paused) {
        return;
    }
    s_paused = pause;
    /* LVGL only. Not the board's display_pause: that's the screen-off path,
     * and on the StopWatch it also cuts the OLED's rail, so the video went to
     * an unpowered panel (a dark screen with frames "shown"). The two share
     * the worker through muse_lvgl_hold. */
    muse_lvgl_hold(MUSE_LVGL_HOLD_VIDEO, pause);
    if (!pause) {
        s_redraw = true;   /* the panel shows the last frame: LVGL draws it all again */
    }
}

/* The columns a window on the panel starts and ends on (muse_board_t's
 * frame_x_align): 4 on the SPD2010, which garbles a window that doesn't. */
static int frame_x_align(void)
{
    return muse_board->frame_x_align > 2 ? muse_board->frame_x_align : 2;
}

/* A frame buffer rectangle widened to whole frame_x_align columns, within the
 * picture's fw: the frame buffer holds the whole picture, so the extra
 * columns draw what's already there. */
static void align_rect(muse_ui_rect_t *r, int fw)
{
    int a = frame_x_align();
    int x0 = r->x & ~(a - 1), x1 = (r->x + r->w + a - 1) & ~(a - 1);
    x1 = x1 < fw ? x1 : fw;
    r->x = (uint16_t)x0;
    r->w = (uint16_t)(x1 - x0);
}

/* A still patch (the show task, between pictures): into the frame buffer, then onto the panel. */
static void show_still(void)
{
    portENTER_CRITICAL(&s_still_mux);
    uint8_t *d = s_still;
    uint32_t n = s_still_len;
    uint16_t number = s_still_number;
    int64_t t_in = s_still_t;
    s_still = NULL;
    portEXIT_CRITICAL(&s_still_mux);
    if (!d) {
        return;   /* a newer one replaced it: that one's entry comes next */
    }
    int64_t t0 = esp_timer_get_time();
    muse_ui_rect_t r;
    bool ok = s_ui && s_streaming && s_open && s_paused && !held() && muse_ui_fb_active() &&
              muse_ui_fb_still(d, n, &r);   /* locked: dropped, as pictures are */
    int64_t t1 = esp_timer_get_time();
    if (ok) {
        int fw, fh;
        muse_ui_fb_pixels(&fw, &fh);
        int ox = (muse_board->width - fw) / 2 & ~(frame_x_align() - 1), oy = (muse_board->height - fh) / 2 & ~1;
        s_win_x1 = ox;
        s_win_y1 = oy;
        s_win_x2 = ox + fw;
        s_win_y2 = oy + fh;
        align_rect(&r, fw);
        muse_board->draw_frame(ox + r.x, oy + r.y, ox + r.x + r.w, oy + r.y + r.h, PIECE_ROWS, muse_ui_fb_fill, &r);
    }
    int64_t now = esp_timer_get_time();
    free(d);
    ESP_LOGI(TAG, "ui still %u: %u bytes, %s, inflate %u ms, draw %u ms, in->shown %u ms", number, (unsigned)n,
             ok ? "shown" : "not shown", (unsigned)((t1 - t0) / 1000), (unsigned)((now - t1) / 1000),
             (unsigned)((now - t_in) / 1000));
    s_last_number = number;
    s_last_shown_t = now;
    s_shown++;
    tell_freed(0);
}

static void fill_black(uint8_t *dst, int stride, int row, int rows, void *arg)
{
    (void)row;
    (void)arg;
    memset(dst, 0, (size_t)stride * rows);
}

/* Video's picture holds the screen: the mic ring went on or off. The panel
 * outside the picture is black (the view's background), drawn again with
 * the ring or without (boards/muse_lcd_bands.c paints it on every band);
 * a UI stream's picture is drawn again from its frame buffer, and camera
 * video's next picture comes within a frame anyway. */
static void ring_repaint(void)
{
    if (!s_paused || !s_open) {
        return;
    }
    int W = muse_board->width, H = muse_board->height;
    int x1 = s_win_x1, y1 = s_win_y1, x2 = s_win_x2, y2 = s_win_y2;
    if (x2 <= x1 || y2 <= y1) {
        x1 = x2 = 0;
        y1 = y2 = 0;
    }
    if (y1 > 0) {
        muse_board->draw_frame(0, 0, W, y1, PIECE_ROWS, fill_black, NULL);
    }
    if (y2 < H) {
        muse_board->draw_frame(0, y2, W, H, PIECE_ROWS, fill_black, NULL);
    }
    if (x1 > 0 && y2 > y1) {
        muse_board->draw_frame(0, y1, x1, y2, PIECE_ROWS, fill_black, NULL);
    }
    if (x2 < W && y2 > y1) {
        muse_board->draw_frame(x2, y1, W, y2, PIECE_ROWS, fill_black, NULL);
    }
    int fw, fh;
    if (s_ui && muse_ui_fb_active() && muse_ui_fb_pixels(&fw, &fh) && x2 - x1 == fw && y2 - y1 == fh) {
        muse_ui_rect_t r = { 0, 0, (uint16_t)fw, (uint16_t)fh };
        muse_board->draw_frame(x1, y1, x2, y2, PIECE_ROWS, muse_ui_fb_fill, &r);
    }
}

/*
 * The turn's status dot (muse_turn_dot.h) while video holds the
 * screen (LVGL paused): every band drawn
 * to the panel gets it (boards/muse_lcd_bands.c), so camera video breathes
 * it at its own frame rate. A still picture (a UI stream's, or around a smaller picture) gets
 * its box drawn again from here when it breathes or changes: from the UI
 * frame buffer inside the picture, black outside it; never inside camera
 * video, whose next picture is due within a frame anyway. The show task's.
 */
static void dot_piece(int x0, int y0, int x1, int y1)
{
    if (x1 <= x0 || y1 <= y0) {
        return;
    }
    int wx1 = s_win_x1, wy1 = s_win_y1, wx2 = s_win_x2, wy2 = s_win_y2;
    bool in_x = x0 >= wx1 && x1 <= wx2, in_y = y0 >= wy1 && y1 <= wy2;
    if (wx2 > wx1 && in_x && in_y) {
        /* Inside the picture. */
        int fw, fh;
        if (s_ui && muse_ui_fb_active() && muse_ui_fb_pixels(&fw, &fh) && wx2 - wx1 == fw && wy2 - wy1 == fh) {
            muse_ui_rect_t r = { (uint16_t)(x0 - wx1), (uint16_t)(y0 - wy1), (uint16_t)(x1 - x0), (uint16_t)(y1 - y0) };
            muse_board->draw_frame(x0, y0, x1, y1, PIECE_ROWS, muse_ui_fb_fill, &r);
        }
        return;
    }
    if (wx2 <= wx1 || x1 <= wx1 || x0 >= wx2 || y1 <= wy1 || y0 >= wy2) {
        muse_board->draw_frame(x0, y0, x1, y1, PIECE_ROWS, fill_black, NULL);   /* all outside */
        return;
    }
    /* Across the picture's edge: the parts above, below, left and right of it, then inside. */
    int iy0 = y0 > wy1 ? y0 : wy1, iy1 = y1 < wy2 ? y1 : wy2;
    int ix0 = x0 > wx1 ? x0 : wx1, ix1 = x1 < wx2 ? x1 : wx2;
    dot_piece(x0, y0, x1, iy0);
    dot_piece(x0, iy1, x1, y1);
    dot_piece(x0, iy0, ix0, iy1);
    dot_piece(ix1, iy0, x1, iy1);
    dot_piece(ix0, iy0, ix1, iy1);
}

static void dot_repaint(void)
{
    if (!s_paused || !s_open) {
        return;
    }
    int x0, y0, x1, y1;
    muse_turn_dot_box(&x0, &y0, &x1, &y1);
    dot_piece(x0, y0, x1, y1);
}

/* What the dot shows over video now. */
static muse_turn_dot_kind_t dot_kind(void)
{
    return muse_turn_dot_kind(muse_state_mode(NULL));
}

/* The show task, at each wake while it holds the screen: the dot's look now. */
static void dot_check(void)
{
    if (s_paused && s_open && muse_turn_dot_update(dot_kind(), esp_timer_get_time())) {
        dot_repaint();
    }
}

/*
 * The mic ring went on or off since the picture on the panel was drawn, and
 * no newer one has come to carry it (a stream stalls, or waits for a
 * keyframe after a drop; it could stay half drawn while the mic records):
 * draw that picture again, with the ring as it is now. Only
 * while the decoder is idle (both slots free: nothing writes a picture) and
 * the picture's buffer is still the drawn one: no decode began since it (a
 * decoder may write its next picture into any buffer it owns). A UI
 * stream's picture is drawn again by ring_repaint. The show task's.
 */
static void ring_check(void)
{
    if (!s_paused || !s_open || s_ui || s_last_k < 0 || s_last_k > 1) {
        return;
    }
    uint32_t g = muse_mic_ring_generation();
    if (g == s_ring_gen_shown) {
        return;
    }
    if (s_dec_index != s_shown_dec_index) {
        return;   /* its buffer may hold a later picture: the next one shown carries the ring */
    }
    if (xSemaphoreTake(s_slots, 0) != pdTRUE) {
        return;   /* a picture on its way: it carries the ring */
    }
    if (xSemaphoreTake(s_slots, 0) == pdTRUE) {
        slot_t *sl = &s_slot[s_last_k];
        if (sl->pic && sl->done && !sl->failed && s_last_arg.s == sl && s_dec_index == s_shown_dec_index) {
            s_ring_gen_shown = g;
            fill_arg_t a = s_last_arg;
            muse_board->draw_frame(a.x, a.y, a.x + a.out_w, a.y + a.out_h, PIECE_ROWS, fill_piece, &a);
            ESP_LOGI(TAG, "mic ring %s: picture %u drawn again with it (no newer one yet)", muse_mic_ring_on() ? "on" : "off",
                     (unsigned)sl->number);
        }
        xSemaphoreGive(s_slots);
    }
    xSemaphoreGive(s_slots);
}

/* Called on the task that turned the ring on or off: no drawing there. */
static void on_ring(bool on)
{
    (void)on;
    int k = RING_K;
    if (s_frames && s_paused) {
        xQueueSendToFront(s_frames, &k, 0);
    }
}

static void log_memory(const char *when);

static void task_lock(void)
{
    portENTER_CRITICAL(&s_task_lock_mux);
    if (!s_task_lock) {
        s_task_lock = xSemaphoreCreateMutexStatic(&s_task_lock_buf);
    }
    portEXIT_CRITICAL(&s_task_lock_mux);
    xSemaphoreTake(s_task_lock, portMAX_DELAY);
}

static void task_unlock(void)
{
    xSemaphoreGive(s_task_lock);
}

/* The show task, between streams: true once the decode tasks are gone and
 * this task should end too (no stream for VIDEO_RETIRE_US). */
static bool retire_tasks(void)
{
    static int64_t idle_since;
    int64_t now = esp_timer_get_time();
    if (s_streaming || s_paused) {
        idle_since = 0;
        return false;
    }
    if (!idle_since) {
        idle_since = now;
        return false;
    }
    if (now - idle_since < VIDEO_RETIRE_US) {
        return false;
    }
    bool done = false;
    task_lock();
    if (!s_streaming && s_dec_task && s_dec_parked && (!s_band_task || s_band_parked) &&
        uxQueueMessagesWaiting(s_frames) == 0) {
        vTaskDeleteWithCaps(s_dec_task);
        s_dec_task = NULL;
        s_dec_parked = false;
        if (s_band_task) {
            vTaskDeleteWithCaps(s_band_task);
            s_band_task = NULL;
            s_band_parked = false;
        }
        /* Before the handle clears: a new show task may start the moment it does. */
        heap_caps_free(s_cov.buf);   /* the next cover picture takes it again */
        s_cov.buf = NULL;
        s_show_task = NULL;
        done = true;
    }
    task_unlock();
    idle_since = 0;
    if (done) {
        log_memory("tasks retired");
    }
    return done;
}

static void show_task(void *arg)
{
    (void)arg;
    int k;
    int64_t poked = 0;
    bool was_asleep = false;
    bool was_held = false;   /* the passcode keypad covered the UI picture (muse_video_set_hold) */
    for (;;) {
        s_show_stage = 0;
        if (s_paused && held()) {
            pause_lvgl(false);   /* the lock takes the screen at once, even with pictures still queued */
        }
        dot_check();   /* the turn dot: within a frame of the turn's change, over a still picture too */
        ring_check();  /* the mic ring changed and no new picture came: the last one again, with it */
        if (s_wake_req) {
            s_wake_req = false;
            if (muse_state_asleep()) {
                ESP_LOGI(TAG, "waking the screen for video");
                muse_state_set_asleep(false);
            }
        }
        if (s_dump_req) {
            s_dump_req = false;
            dump_begin();
            s_show_stage = SHOW_STAGE_DUMP;   /* the decoder never takes the pictures back meanwhile */
            s_stage_t = esp_timer_get_time();
            /* Only while the decoder is idle (both slots free) and the last
             * picture's buffer is still the one drawn: a later decode may
             * reuse it, and the stream's end frees it (s_dec_index moves on). */
            bool dumped = false;
            if (s_ui && s_last_k >= 0 && xSemaphoreTake(s_slots, 0) == pdTRUE) {
                if (xSemaphoreTake(s_slots, 0) == pdTRUE) {
                    const slot_t *sl = &s_slot[s_last_k];
                    if (s_dec_index == s_last_dec_index && sl->pic && sl->done && !sl->failed) {
                        dump_picture(sl, &s_last_arg);
                        dumped = true;
                    }
                    xSemaphoreGive(s_slots);
                }
                xSemaphoreGive(s_slots);
            }
            if (!dumped) {
                dump_printf("@uidump none\n");
            }
            dump_finish();
            s_show_stage = 0;
        }
        /* 40 ms while the picture holds the screen: the turn dot's breath
         * (80 ms steps) and a turn's change show within a frame. */
        if (xQueueReceive(s_frames, &k, pdMS_TO_TICKS(s_paused ? 40 : 100)) != pdTRUE) {
            /* The stream ended, the view closed, or no picture for a while (a
             * client that stopped sending without ending the stream, a link
             * gone quiet): give the screen back to the UI, which draws the
             * face over the last picture. The next
             * picture pauses it again. A UI stream is quiet by design (frames
             * only when its picture changes): its last picture stays. */
            int64_t shown = s_last_shown_t;
            bool stalled = !s_ui && (!shown || esp_timer_get_time() - shown > STALL_US);
            if (s_paused && (!s_streaming || !s_open || stalled || held())) {
                s_show_stage = 8;
                s_stage_t = esp_timer_get_time();
                if (s_streaming && s_open) {
                    ESP_LOGW(TAG, "no picture for %u ms: showing the face until the next one",
                             (unsigned)((esp_timer_get_time() - shown) / 1000));
                }
                pause_lvgl(false);   /* the stream ended: show the captions again */
                s_show_stage = 0;
            }
            if (!s_streaming && muse_ui_fb_active()) {
                muse_ui_fb_end();
            }
            if (!s_streaming) {
                vd_show_idle();
            }
            if (retire_tasks()) {
                vTaskDeleteWithCaps(NULL);   /* its stack goes back (made WithCaps: start_tasks) */
            }
            if (s_streaming && s_ui) {
                /* A still UI picture stays on screen for as long as the
                 * stream is on: no auto-sleep. If the screen went dark all
                 * the same, its picture is gone: a keyframe repaints it. */
                int64_t now = esp_timer_get_time();
                if (now - poked > UI_POKE_US) {
                    poked = now;
                    muse_state_poke();
                }
                bool asleep = muse_state_asleep();
                if (was_asleep && !asleep) {
                    ESP_LOGI(TAG, "screen back on: asking for a keyframe to repaint the UI");
                    ask_keyframe();
                }
                was_asleep = asleep;
                bool covered = held();
                if (was_held && !covered && !asleep) {
                    /* The keypad drew over the picture: a quiet stream sends nothing new, so a keyframe repaints. */
                    ESP_LOGI(TAG, "unlocked: asking for a keyframe to repaint the UI");
                    ask_keyframe();
                }
                was_held = covered;
            }
            continue;
        }
        if (k == STILL_K) {
            show_still();
            continue;
        }
        if (k == RING_K) {
            ring_repaint();
            ring_check();
            continue;
        }
        slot_t *sl = &s_slot[k];
        SLOT_EV(k, 'r');
        s_show_stage = 4;   /* waiting for the decoder's first rows (stuck_check names it) */
        s_stage_t = esp_timer_get_time();
        while (!sl->pic && !sl->done) {
            vTaskDelay(1);
        }
        /* Live video drawn in step with the panel's refresh goes out at most
         * every other refresh (29.9 fps at 60 Hz): a 30 fps stream gains a
         * frame on it every few seconds, and the frames queue up behind.
         * When a newer picture is already on its way and this one is late,
         * skip this one (decoded all the same: the next predicts from it). */
        int next;
        if (!sl->due && s_params.fps && xQueuePeek(s_frames, &next, 0) == pdTRUE && next >= 0 && next < 2) {
            while (!sl->done) {
                vTaskDelay(1);
            }
            /* The next one already whole means this one is a frame behind:
             * show the newer one now rather than both a frame late. */
            bool newer = s_slot[next].done && !s_slot[next].failed;
            if (newer || esp_timer_get_time() - sl->t_whole > 1200000 / s_params.fps) {
                s_skipped++;
                SLOT_EV(k, 's');
                vd_fold(sl);   /* its changes join the next picture drawn */
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
        bool drawable = sl->pic && !sl->failed && s_open && !held();   /* locked: dropped, never woken for */
        /* The screen may have gone dark (or never woken) before the stream:
         * wake it, and draw only once the UI has been through a frame awake
         * (it turns the panel on there; pausing LVGL before that hangs). */
        if (drawable && muse_state_asleep()) {
            ESP_LOGI(TAG, "waking the screen for video");
            muse_state_set_asleep(false);
        }
        if (drawable && !s_paused && s_ui) {
            /* A UI frame may be the last for minutes: wait for the screen
             * to wake rather than drop it. */
            for (int i = 0; i < 200 && esp_timer_get_time() - s_awake_tick > 300000 && s_streaming; i++) {
                vTaskDelay(pdMS_TO_TICKS(10));
            }
        }
        if (drawable && !s_paused && esp_timer_get_time() - s_awake_tick > 300000) {
            drawable = false;
            s_dropped++;
        }
        if (!drawable) {
            while (!sl->done) {
                vTaskDelay(1);   /* the decoder still writes into it */
            }
            vd_invalidate(sl);
            SLOT_EV(k, 'n');
            xSemaphoreGive(s_slots);
            continue;
        }
        s_show_stage = 1;
        s_stage_t = esp_timer_get_time();
        bool resumed = !s_paused;
        if (!s_paused) {
            s_fb_all = true;   /* the UI drew over the panel since the last picture */
        }
        pause_lvgl(true);
        if (resumed && muse_mic_ring_on()) {
            /* The UI had the screen, maybe stopped mid-refresh: the edges
             * around the picture get the ring whole (black + ring) rather
             * than whatever part of it LVGL had drawn (a half-drawn ring). */
            ring_repaint();
        }
        if (resumed) {
            /* LVGL drew the dot as it was; this picture's bands paint it as it is. */
            muse_turn_dot_update(dot_kind(), esp_timer_get_time());
            dot_repaint();
        }
        s_show_stage = 2;
        s_stage_t = esp_timer_get_time();
        int pw = sl->w, ph = sl->h;
        muse_video_fit_in_t fin = {
            .panel_w = muse_board->width,
            .panel_h = muse_board->height,
            .round = muse_board->round,
            .x_align = frame_x_align(),
            .max_src = MAX_SRC,
            .ui = s_ui,
            .fill = muse_board->video_fill,
            .fill_max_src = MUSE_YUV_FILL_MAX_SRC,
            .bands = sl->bands,
            .pic_w = s_params.width && s_params.width <= pw ? s_params.width : pw,
            .pic_h = s_params.height && s_params.height <= ph ? s_params.height : ph,
        };
        muse_video_fit_t fit;
        bool fits = muse_video_fit(&fin, &fit);
        if (fits && fit.cover && !s_cov.buf) {
            /* The cover path's block (3.8 KB at 240 px): main SRAM while 32 KB
             * stays free past it, as the decoders' small blocks, else PSRAM
             * (four rows written and read back at once: they stay in the cache). */
            size_t n = MUSE_VIDEO_COVER_BUF(MAX_SRC);
            if (heap_caps_get_free_size(MUSE_SIMD_CAPS) >= n + 32 * 1024) {
                s_cov.buf = heap_caps_aligned_alloc(16, n, MUSE_SIMD_CAPS);   /* main SRAM: SIMD rows (muse_mem.h) */
            }
            if (!s_cov.buf) {
                s_cov.buf = heap_caps_aligned_alloc(16, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            }
            if (!s_cov.buf) {
                ESP_LOGW(TAG, "no memory to cover the round panel: fitting the picture");
                fin.round = false;
                fits = muse_video_fit(&fin, &fit);
            }
        }
        if (!fits) {
            /* Nothing to show, and nothing to divide by below. */
            s_dropped++;
            while (!sl->done) {
                vTaskDelay(1);
            }
            vd_invalidate(sl);
            SLOT_EV(k, 'z');
            xSemaphoreGive(s_slots);
            continue;
        }
        s_cov.blk = -1;   /* a new picture: the cover's block is stale */
        int scale = fit.scale, vis_w = fit.vis_w, vis_h = fit.vis_h, ow = fit.out_w, oh = fit.out_h;
        int x1 = fit.x, y1 = fit.y;
        fill_arg_t a = { sl, vis_w, vis_h, scale, ow, oh, x1, y1, fit };
        s_win_x1 = x1;
        s_win_y1 = y1;
        s_win_x2 = x1 + ow;
        s_win_y2 = y1 + oh;
        int64_t d0 = esp_timer_get_time();
        int fb_rects = -1;
        if (s_ui && s_hq) {
            /* Through the frame buffer: what changed, converted finely, and
             * only that drawn (muse_ui_fb.h). The whole picture first. */
            int fw, fh;
            if (!muse_ui_fb_pixels(&fw, &fh) || fw != vis_w || fh != vis_h) {
                muse_ui_fb_begin(vis_w, vis_h);
                s_fb_all = true;
            }
            if (muse_ui_fb_active()) {
                while (!sl->done) {
                    vTaskDelay(1);
                }
                fb_rects = 0;
                if (!sl->failed) {
                    muse_ui_rect_t rects[MUSE_UI_MAX_RECTS];
                    fb_rects = muse_ui_fb_picture(&sl->p0, sl->bands == 2 ? &sl->p1 : NULL, sl->h0, sl->key || s_fb_all,
                                                  rects);
                    s_fb_all = fb_rects < 0;   /* refused (muse_pic_covers): the next one starts whole */
                    for (int i = 0; i < fb_rects; i++) {
                        muse_ui_rect_t *r = &rects[i];
                        align_rect(r, vis_w);
                        muse_board->draw_frame(x1 + r->x, y1 + r->y, x1 + r->x + r->w, y1 + r->y + r->h, PIECE_ROWS,
                                               muse_ui_fb_fill, r);
                    }
                }
            }
        }
        if (fb_rects < 0) {
            bool shot = s_shot_state == 1 && s_shot;
            if (shot) {
                s_shot_whole = false;   /* overwritten from here: whole again only at state 3 */
                memset(s_shot, 0, (size_t)muse_board->width * muse_board->height * 2);
                s_shot_state = 2;
            }
            s_ring_gen_shown = muse_mic_ring_generation();   /* (read before the panel takes its own look) */
            s_shown_dec_index = sl->dec_index;
            /* Only what changed, if the panel holds the picture before (muse_vdirty.h); else whole. The
             * changed map comes with the whole picture (vd_slot_map, before done). */
            muse_board_win_t orect[VD_ORECT_MAX];
            uint8_t okind[VD_ORECT_MAX];
            uint32_t sgen, ogen, sgen2, ogen2;
            int norect = vd_overlays(orect, okind, &sgen, &ogen);
            bool ring_on = muse_mic_ring_on();
            uint32_t tok0 = vd_token(sgen, ogen, ring_on);
            if (s_vd_on && muse_board->draw_windows && !s_ui && sl->bands == 1 && vd_scale_ok(&a)) {
                while (!sl->done) {
                    vTaskDelay(1);
                }
            }
            int nwin = vd_plan(sl, &a, orect, okind, norect, sgen, ogen, ring_on);
            uint32_t writes = muse_board->panel_writes ? muse_board->panel_writes() : 0;
            if (s_vd) {
                s_vd->cap_x = x1;
                s_vd->cap_y = y1;
            }
            s_vd_cap_cut = false;
            bool capture = s_vd_follow && s_vd_model;
            s_vd_capture = capture;
            uint32_t f0 = s_fill_us;
            if (s_vd) {
                s_vd->last_te = 0;
                s_vd->drawing = true;
            }
            int64_t t_dr = esp_timer_get_time();
            if (nwin < 0) {
                muse_board->draw_frame(x1, y1, x1 + ow, y1 + oh, PIECE_ROWS, fill_piece, &a);
            } else if (nwin > 0) {
                vd_mem_t *vm0 = s_vd;   /* (vd_plan had it: windows come only with the block) */
                vm0->fx = -1;           /* a new picture: no row of the last one kept */
                muse_board->draw_windows(x1, y1, vm0->win, nwin, PIECE_ROWS, fill_win, &a);
            }
            uint32_t dr_us = (uint32_t)(esp_timer_get_time() - t_dr);
            uint32_t f1 = s_fill_us, fill_d = f1 >= f0 ? f1 - f0 : f1;
            s_vd_capture = false;
            /* Anything that showed during the draw is on it too (drawn into the pieces after it showed). */
            int norect2 = norect + vd_overlays(orect + norect, okind + norect, &sgen2, &ogen2);
            vd_drawn(sl, &a, writes, nwin != 0, s_ring_gen_shown, ring_on, orect, okind, norect2, sgen, ogen,
                     sgen2 != sgen || ogen2 != ogen);
            vd_adapt(nwin);
            vd_mem_t *vm = s_vd;   /* (the stream's: the decode task frees it only with both slots) */
            if (vm) {
                vm->drawing = false;
            }
            if (nwin < 0) {
                s_vd_model_ok = capture && !s_vd_cap_cut && vm && vm->known;   /* the model holds this whole picture */
                if (vm) {
                    vm->n_full++;
                    vm->us_full += dr_us;
                    vm->fill_full_us += fill_d;
                    vm->te_full += vm->last_te;
                    vm->te_n_full++;
                }
            } else {
                if (s_vd_cap_cut || !vm->known || (nwin > 0 && !capture)) {
                    s_vd_model_ok = false;
                }
                if (nwin == 0) {
                    vm->n_none++;   /* (windows or none: vd_plan had the block) */
                } else {
                    vm->n_win++;
                    vm->us_win += dr_us;
                    vm->fill_win_us += fill_d;
                    vm->te_win += vm->last_te;
                    vm->te_n_win++;
                }
                if (s_vd_shadow > 0 && s_vd_model_ok) {
                    vd_shadow_check(sl, &a, nwin, tok0);
                }
            }
            if (shot) {
                /* The ring as the bands to the panel get it. */
                muse_mic_ring_apply(s_shot, muse_board->width * 2, 0, 0, muse_board->width, muse_board->height);
                muse_turn_dot_apply(s_shot, muse_board->width * 2, 0, 0, muse_board->width, muse_board->height);
                s_shot_whole = true;
                s_shot_state = 3;
            }
        }
        s_show_stage = 3;
        s_stage_t = esp_timer_get_time();
        while (!sl->done) {
            vTaskDelay(1);
        }
        SLOT_EV(k, 'd');
        s_last_k = k;
        s_last_dec_index = sl->dec_index;
        s_last_arg = a;
        char md5[33] = "";
        if (s_ui && s_md5 && !sl->failed) {
            picture_md5(sl, vis_w, vis_h, md5);
        }
        const slot_t done_sl = *sl;   /* read before the slot goes back: the decode task may refill it at once */
        xSemaphoreGive(s_slots);
        s_shown++;
        int64_t now = esp_timer_get_time();
        s_last_number = done_sl.number;
        s_last_shown_t = now;
        if (!s_ui) {
            s_shown32 = done_sl.number32;   /* drawn: on the panel from now (the A/V anchor waits for it) */
            s_shown32_ok = true;
        }
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
        s_lat_sum += (uint32_t)(now - done_sl.t_whole);
        s_lat_n++;
        if (s_ui) {
            /* Frames are few: one line each, and the client hears at once. */
            char fbs[96] = "";
            if (fb_rects >= 0) {
                int mbs, total, skipped;
                uint32_t cus;
                muse_ui_fb_last_stats(&mbs, &total, &skipped, &cus);
                snprintf(fbs, sizeof(fbs), ", %d/%d macroblocks changed, %d skipped (convert %u ms, %d rects)", mbs,
                         total, skipped, (unsigned)(cus / 1000), fb_rects);
            }
            ESP_LOGI(TAG, "ui frame %u: %s %u bytes, decode %u ms (top %u, bottom %u), draw %u ms, in->shown %u ms%s",
                     done_sl.number, done_sl.key ? "keyframe" : "P", (unsigned)done_sl.bytes, (unsigned)(done_sl.dec_us / 1000),
                     (unsigned)(done_sl.top_us / 1000), (unsigned)(done_sl.bot_us / 1000), (unsigned)((now - d0) / 1000),
                     (unsigned)((now - done_sl.t_whole) / 1000), fbs);
            if (md5[0]) {
                ESP_LOGI(TAG, "ui frame %u md5 %s", done_sl.number, md5);
            }
            if (done_sl.key) {
                ESP_LOGI(TAG, "memory: internal %u free (largest %u), PSRAM %u free",
                         (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                         (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                         (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
            }
            tell_freed((uint8_t)(done_sl.dec_us / 1000 < 255 ? done_sl.dec_us / 1000 : 255));
        }
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
    TaskStatus_t *st = heap_caps_malloc(CPU_MAX_TASKS * sizeof(*st), BIG_CAPS);
    if (!st) {
        return;
    }
    configRUN_TIME_COUNTER_TYPE total;
    UBaseType_t n = uxTaskGetSystemState(st, CPU_MAX_TASKS, &total);
    configRUN_TIME_COUNTER_TYPE elapsed = total - prev_total;
    if (prev_total && elapsed) {
        uint32_t idle[2] = { 0, 1000 };   /* a core that isn't there: idle */
        struct { const char *name; uint32_t pm; } top[4] = { 0 };
        for (UBaseType_t i = 0; i < n; i++) {
            configRUN_TIME_COUNTER_TYPE before = 0;
            for (int j = 0; j < prev_n; j++) {
                if (prev_task[j] == st[i].xHandle) {
                    before = prev_rt[j];
                    break;
                }
            }
            /* A task handle reused by a newer task (a short-lived one, as the
             * console's touch injection makes) has a smaller count than the old
             * one's: that's all this task's. */
            if (st[i].ulRunTimeCounter < before) {
                before = 0;
            }
            uint32_t pm = (uint32_t)((uint64_t)(st[i].ulRunTimeCounter - before) * 1000 / elapsed);
            pm = pm > 1000 ? 1000 : pm;
            bool is_idle = false;
            /* (Each core there is: the C6 has one, and asking for a second's idle task asserts.) */
            for (int c = 0; c < portNUM_PROCESSORS && c < 2; c++) {
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
        unsigned busy0 = 100 - idle[0] / 10;
        if (busy0 > s_vl.cpu_peak) {
            s_vl.cpu_peak = (uint8_t)busy0;
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

static void on_progress(void *ctx, const muse_pic_t *pic, int rows)
{
    slot_t *sl = ctx;
    sl->w = pic->w;
    if (sl->bands == 2) {
        /* The whole picture: the stream's height (the bottom band's own
         * size comes with its picture). */
        sl->h0 = pic->h;
        sl->h = (s_params.height + 15) & ~15;
    } else {
        sl->h = pic->h;
    }
    sl->p0 = *pic;
    sl->pic = pic->y;
    sl->rows = rows;
}

static void on_progress1(void *ctx, const muse_pic_t *pic, int rows)
{
    slot_t *sl = ctx;
    sl->p1 = *pic;
    sl->pic1 = pic->y;
    sl->rows1 = rows;
}

/*
 * The bottom band: its own decoder, so the two halves of a frame decode at
 * once (each core about half the work). Its picture buffers live until the
 * next stream (a new gen) or the decode task closes the stream.
 */
static void band_task(void *arg)
{
    (void)arg;
    muse_vdec_t *dec = NULL;
    uint32_t gen = 0;
    for (;;) {
        s_band_parked = dec == NULL;
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        s_band_parked = false;
        band_job_t *j = &s_band;
        if (!j->sl) {   /* close */
            muse_vdec_close(dec);
            dec = NULL;
            xSemaphoreGive(s_band_done);
            continue;
        }
        if (!dec || gen != j->gen) {
            muse_vdec_close(dec);
            gen = j->gen;
            dec = muse_vdec_open(s_params.codec, s_params.width, s_params.height);
        }
        j->ok = j->got = false;
        if (dec) {
            muse_vdec_set_progress(dec, on_progress1, j->sl);
            muse_pic_t pic;
            int64_t t0 = esp_timer_get_time();
            muse_vdec_result_t rc = muse_vdec_decode(dec, j->au, j->len, &pic);
            j->us = (uint32_t)(esp_timer_get_time() - t0);
            j->got = rc == MUSE_VDEC_PICTURE;
            j->ok = rc != MUSE_VDEC_ERROR;
            j->h = j->got ? pic.h : 0;
            if (j->got) {
                j->sl->p1 = pic;
                j->sl->pic1 = pic.y;
                j->sl->rows1 = pic.h;
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

/* Both pictures back from the show task (it may still be drawing one), saying so while it takes long. */
static void take_both_slots(void)
{
    for (int i = 0; i < 2; i++) {
        int waits = 0;
        while (xSemaphoreTake(s_slots, pdMS_TO_TICKS(2000)) != pdTRUE) {
            ESP_LOGW(TAG, "decoder: waiting for the show task to give back its pictures (show stage %d, %u queued; "
                     "slot 0 last '%c', slot 1 last '%c')", s_show_stage, (unsigned)uxQueueMessagesWaiting(s_frames),
                     s_slot_ev[0], s_slot_ev[1]);
            /* As in the decode loop: the show task idle with none queued holds nothing. */
            if (++waits >= 3 && s_show_stage == 0 && uxQueueMessagesWaiting(s_frames) == 0) {
                ESP_LOGE(TAG, "decoder: %u of 2 pictures lost (show task idle, none queued; slot 0 last '%c', slot 1 "
                         "last '%c'; stream end): giving them back -- please report this log",
                         (unsigned)(2 - i - uxSemaphoreGetCount(s_slots)), s_slot_ev[0], s_slot_ev[1]);
                while (uxSemaphoreGetCount(s_slots) < 2 - i) {
                    xSemaphoreGive(s_slots);
                }
            }
        }
    }
}

static muse_vcounts_t tick_counts(void)
{
    return (muse_vcounts_t){ .rx = s_rx_frames, .shown = s_shown, .dropped = s_dropped,
                                  .skipped = s_skipped, .decode_errors = s_dec_errors };
}

/* Once a second while a video stream runs (not UI streams): the video counters. */
static void video_tick(int64_t now)
{
    if (!muse_vtick_due(&s_tick, now)) {
        return;
    }
    char line[144];
    muse_vcounts_t c = tick_counts();
    if (muse_vtick_line(&s_tick, now, &c, s_params.fps, (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                             (uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL), line, sizeof(line)) > 0) {
        /* The stacks' bytes never used yet (the W175's lcd_send): decode, show and the panel's own
         * send task where there is one. */
        ESP_LOGI(TAG, "%s sdec=%u sshow=%u spanel=%u", line, (unsigned)uxTaskGetStackHighWaterMark(NULL),
                 s_show_task ? (unsigned)uxTaskGetStackHighWaterMark(s_show_task) : 0u,
                 muse_board->panel_stack_free ? (unsigned)muse_board->panel_stack_free() : 0u);
    }
}

static const char *codec_name(uint8_t codec)
{
    return codec == MUSE_VIDEO_CODEC_VP8 ? "VP8" : "H.264";
}

static void decode_task(void *arg)
{
    (void)arg;
    muse_vdec_t *dec = NULL;
    uint32_t gen = 0;
    uint32_t size_told = 0;   /* the stream (gen) whose size mismatch was logged */
    muse_vgate_t gate = { 0 };   /* which frames decode, when to ask for a keyframe */
    bool waited = false;         /* the ring had no whole frame since the last one taken (the lip-sync floor) */
    muse_vgate_start(&gate);
    int next_slot = 0;
    int64_t stat_t = 0;
    uint32_t dec_us_sum = 0, dec_us_max = 0, dec_n = 0, bytes = 0;
    uint32_t avg_ms = 0, rx_last = 0;
    for (;;) {
        if (!s_streaming) {
            if (dec) {
                /* Both pictures drawn (or dropped) before the buffers go. */
                take_both_slots();
                vd_free();   /* (the show task holds no slot: nothing reads it) */
                s_dec_index++;   /* the drawn picture's buffer goes: ring_check never draws it again */
                xSemaphoreGive(s_slots);
                xSemaphoreGive(s_slots);
                muse_vdec_close(dec);
                dec = NULL;
                s_dec_holds = false;
                band_close();
                muse_vtick_stop(&s_tick);
                s_vl.shown = s_shown;
                s_vl.dropped = s_dropped;
                s_vl.t1 = esp_timer_get_time();
                s_vl.lowest_ever1 = (uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
                ESP_LOGI(TAG, "decoder closed: %u shown, %u dropped; stack left: decode %u B, show %u B", s_shown,
                         s_dropped, (unsigned)uxTaskGetStackHighWaterMark(NULL),
                         s_show_task ? (unsigned)uxTaskGetStackHighWaterMark(s_show_task) : 0u);
                log_memory("after the stream");
            }
            s_dec_parked = true;   /* nothing held: the show task may retire this task now */
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            s_dec_parked = false;
            continue;
        }
        if (!dec || gen != s_gen) {
            if (dec) {
                /* A new stream replaced this one: the show task may still
                 * hold its pictures, as when the stream stops. */
                take_both_slots();
                vd_free();
                s_dec_index++;   /* the drawn picture's buffer goes: ring_check never draws it again */
                xSemaphoreGive(s_slots);
                xSemaphoreGive(s_slots);
            }
            muse_vdec_close(dec);
            s_dec_holds = false;
            gen = s_gen;
            dec = NULL;
            if (!dec) {
                dec = muse_vdec_open(s_params.codec, s_params.width, s_params.height);
            }
            s_dec_holds = dec != NULL;
            if (!dec) {
                ESP_LOGE(TAG, "no memory for the decoder");
                if (s_tp && s_tp->stop) {
                    s_tp->stop();
                }
                s_streaming = false;
                continue;
            }
            muse_vgate_start(&gate);
            s_pace_base = 0;
            log_memory("decoding");
            dec_us_sum = dec_us_max = dec_n = bytes = 0;
            rx_last = 0;
            stat_t = esp_timer_get_time();
            if (s_ui) {
                muse_vtick_stop(&s_tick);
            } else {
                s_rx_frames = s_dec_errors = 0;
                muse_vcounts_t c0 = tick_counts();
                muse_vtick_start(&s_tick, stat_t, &c0);
            }
            ESP_LOGI(TAG, "decoder open: %s %ux%u %u fps %u kb/s%s (decoding on core %d)",
                     codec_name(s_params.codec), s_params.width, s_params.height,
                     s_params.fps, s_params.kbps, s_params.tiles == 2 ? ", two bands (one per core)" : "",
                     (int)xTaskGetCoreID(NULL));
        }
        muse_vstream_t *vs = s_tp->ring;
        muse_vframe_t fr;
        int32_t n = muse_vstream_pop(vs, s_au, AU_MAX, &fr);
        video_tick(esp_timer_get_time());
        if (n == 0) {
            waited = true;   /* no whole frame yet: keeping up */
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2));
            continue;
        }
        bool frame_waited = waited;
        waited = false;
        /* Every frame header received, decoded or not, extends the 16-bit
         * number to 32 bits, from 0 at the stream's start (frame
         * domain, mgcommands.h). */
        if (s_ext_reset) {
            s_ext_reset = false;
            s_ext = fr.number;
        } else {
            s_ext += (uint32_t)(int32_t)(int16_t)(uint16_t)(fr.number - (uint16_t)s_ext);
        }
        s_rx_frames++;
        int64_t now = esp_timer_get_time();
        int64_t t_whole = now;
        if (n > 0 && (fr.flags & MUSE_VIDEO_FRAME_STILL)) {
            if (muse_vgate_still(&gate, fr.after_gap, fr.number, now)) {   /* it takes a number too */
                ask_keyframe();   /* a frame missed before it: the picture needs a keyframe */
            }
            /* A UI stream's still patch: not the decoder's. The show task
             * draws it after the pictures queued before it. */
            uint8_t *copy = s_ui ? heap_caps_malloc((size_t)n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : NULL;
            if (copy) {
                memcpy(copy, s_au, (size_t)n);
                portENTER_CRITICAL(&s_still_mux);
                uint8_t *old = s_still;
                s_still = copy;
                s_still_len = (uint32_t)n;
                s_still_number = fr.number;
                s_still_t = now;
                portEXIT_CRITICAL(&s_still_mux);
                free(old);
                int k = STILL_K;
                xQueueSend(s_frames, &k, portMAX_DELAY);
            } else {
                s_dropped++;
            }
            tell_freed((uint8_t)avg_ms);
            continue;
        }
        bool key = n > 0 && (fr.flags & MUSE_VIDEO_FRAME_KEY);
        bool ask;
        if (!muse_vgate_take(&gate, n, key, fr.after_gap, fr.number, now, &ask)) {
            /* A gap (bytes lost, or a frame number skipped), or waiting for
             * a keyframe: dropped. The first trouble after a picture asks at
             * once (muse_vgate.h). Waiting after a catch-up (LAG_RESYNC_MS):
             * skipped, as the late pictures it saves are. */
            s_dropped++;
            if (ask) {
                ask_keyframe();
            }
            tell_freed((uint8_t)avg_ms);
            continue;
        }
        bytes += (uint32_t)n;
        /* The ring has room again: credits go before the decode. */
        tell_freed((uint8_t)avg_ms);
        int64_t due = pace_due(&fr, key, now);

        /* A slot: the picture two back has been drawn, so the decoder may
         * reuse its buffer. */
        int64_t t_slot = esp_timer_get_time();
        s_slot_wait_t = t_slot;
        bool slot = true;
        int slot_waits = 0;
        while (xSemaphoreTake(s_slots, pdMS_TO_TICKS(2000)) != pdTRUE) {
            int64_t tn = esp_timer_get_time();
            ESP_LOGW(TAG, "decoder: still waiting for a picture to be drawn (show stage %d, %u free, %u queued, "
                     "open %d streaming %d paused %d; slot 0 '%c' %u ms ago, slot 1 '%c' %u ms ago, next %d)",
                     s_show_stage, (unsigned)uxSemaphoreGetCount(s_slots), (unsigned)uxQueueMessagesWaiting(s_frames),
                     s_open, s_streaming, s_paused, s_slot_ev[0], (unsigned)((tn - s_slot_ev_t[0]) / 1000), s_slot_ev[1],
                     (unsigned)((tn - s_slot_ev_t[1]) / 1000), next_slot);
            if (!s_streaming || gen != s_gen) {
                slot = false;   /* the stream is gone: drop this one, the stream's end does the rest */
                break;
            }
            /* The show task idle (stage 0: waiting for a picture), none queued,
             * and still no free picture: one was never given back. Nobody
             * holds it, so give it back rather than stop the video for good,
             * and say so loudly: that's a bug to find. */
            if (++slot_waits >= 3 && s_show_stage == 0 && uxQueueMessagesWaiting(s_frames) == 0) {
                ESP_LOGE(TAG, "decoder: %u of 2 pictures lost (show task idle, none queued; slot 0 last '%c', slot 1 "
                         "last '%c', next %d, stream %ux%u %s): giving them back -- please report this log",
                         (unsigned)(2 - uxSemaphoreGetCount(s_slots)), s_slot_ev[0], s_slot_ev[1], next_slot,
                         s_params.width, s_params.height, codec_name(s_params.codec));
                while (uxSemaphoreGetCount(s_slots) < 2) {
                    xSemaphoreGive(s_slots);
                }
            }
        }
        s_slot_wait_t = 0;
        if (!slot) {
            muse_vgate_lost(&gate);
            continue;
        }
        s_slot_wait_us += (uint32_t)(esp_timer_get_time() - t_slot);
        /* Two bands: the top band's access unit after its length, the
         * bottom band's after that. The ring checked the frame in this
         * layout. */
        int bands = fr.bands == 2 && s_band_task ? 2 : 1;
        uint32_t len0 = (uint32_t)n;
        if (bands == 2) {
            len0 = n >= 4 ? (uint32_t)(s_au[0] | s_au[1] << 8 | s_au[2] << 16 | (uint32_t)s_au[3] << 24) : UINT32_MAX;
            if (len0 > (uint32_t)n - 4) {
                ESP_LOGW(TAG, "bad band length %u in a %d byte frame", (unsigned)len0, (int)n);
                muse_vgate_lost(&gate);
                s_dropped++;
                s_dec_errors++;
                xSemaphoreGive(s_slots);
                continue;
            }
        }
        /* An access unit that may make a decoder reallocate its pictures
         * (muse_vdec_may_realloc: a parameter set or IDR slice, a VP8
         * keyframe) frees the one the show task may still be drawing: wait
         * for that one first (both slots ours). */
        bool reset = muse_vdec_may_realloc(dec, bands == 2 ? s_au + 4 : s_au, len0) ||
                       (bands == 2 && muse_vdec_may_realloc(dec, s_au + 4 + len0, (uint32_t)n - 4 - len0));
        if (reset) {
            xSemaphoreTake(s_slots, portMAX_DELAY);
        }
        slot_t *sl = &s_slot[next_slot];
        sl->dec_index = ++s_dec_index;
        sl->pic = NULL;
        sl->rows = 0;
        sl->pic1 = NULL;
        sl->rows1 = 0;
        sl->bands = bands;
        sl->h0 = (int)(s_params.height / 2);
        sl->failed = false;
        vd_slot_none(next_slot);
        sl->done = false;
        sl->t_whole = t_whole;
        sl->waited = frame_waited;
        sl->due = due;
        sl->number = fr.number;
        sl->number32 = s_ext;
        sl->bytes = (uint32_t)n;
        sl->key = key;
        sl->top_us = sl->bot_us = 0;
        muse_vdec_set_progress(dec, on_progress, sl);
        SLOT_EV(next_slot, 'q');
        xQueueSend(s_frames, &next_slot, portMAX_DELAY);
        next_slot ^= 1;

        /* The whole access unit at once: parameter sets and the picture's
         * slices; h264bsdDecode works through its NAL units in turn. */
        muse_pic_t pic;
        int64_t t0 = esp_timer_get_time();
        if (bands == 2) {
            s_band.au = s_au + 4 + len0;
            s_band.len = (uint32_t)n - 4 - len0;
            s_band.sl = sl;
            s_band.gen = gen;
            xTaskNotifyGive(s_band_task);
        }
        muse_vdec_result_t rc = muse_vdec_decode(dec, bands == 2 ? s_au + 4 : s_au, len0, &pic);
        if (bands == 2) {
            sl->top_us = (uint32_t)(esp_timer_get_time() - t0);
            s_top_us += sl->top_us;
        }
        if (reset && bands == 1) {
            xSemaphoreGive(s_slots);
        }
        bool got = rc == MUSE_VDEC_PICTURE;
        bool ok = rc != MUSE_VDEC_ERROR;
        int h = got ? pic.h : 0;
        if (!ok && muse_vdec_no_memory(dec)) {
            /* The stream's own size (its SPS, an iPhone's padded picture) needs more internal RAM than is
             * free: every picture would wait for a keyframe that can't decode either. Ended, and said. */
            ESP_LOGE(TAG, "no memory for the decoder at the stream's size: internal %u free, largest %u",
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
            s_vl.refused = "no memory at the stream's own size";
            if (s_tp && s_tp->stop) {
                s_tp->stop();
            }
            s_streaming = false;
        }
        if (bands == 2) {
            xSemaphoreTake(s_band_done, portMAX_DELAY);   /* the bottom band */
            s_bot_us += s_band.us;
            s_band_n++;
            got = got && s_band.got;
            ok = ok && s_band.ok;
            if (reset) {
                xSemaphoreGive(s_slots);
            }
        }
        uint32_t us = (uint32_t)(esp_timer_get_time() - t0);
        sl->dec_us = us;
        if (bands == 2) {
            sl->bot_us = s_band.us;
        }
        /* Two bands make the stream's height exactly (muse_video_bands_ok):
         * the top band whole macroblocks, the bottom band cropped to the
         * stream's height or coded to whole macroblocks without cropping. */
        if (got && bands == 2 && !muse_video_bands_ok(h, s_band.h, s_params.height)) {
            static int64_t told_h;
            if (now - told_h > 5000000) {
                told_h = now;
                ESP_LOGW(TAG, "bands of %d and %d rows don't make the stream's %u", h, s_band.h, s_params.height);
            }
            got = false;
            ok = false;   /* a decode error (counted below) */
        }
        /* ...and are as wide as each other, within the stream (the drawing reads both at the top's width:
         * a 16-wide bottom under a 464-wide top would read past its planes). */
        if (got && bands == 2 &&
            (sl->p1.w != pic.w || pic.w > (int)((s_params.width + 15) & ~15u) ||
             !muse_pic_covers(&pic, pic.w, h) || !muse_pic_covers(&sl->p1, pic.w, s_band.h))) {
            static int64_t told_w;
            if (now - told_w > 5000000) {
                told_w = now;
                ESP_LOGW(TAG, "bands %d and %d wide don't make the stream's %u", pic.w, sl->p1.w, s_params.width);
            }
            got = false;
            ok = false;
        }
        if (got && size_told != gen && s_params.codec == MUSE_VIDEO_CODEC_H264) {
            /* The picture after frame cropping is what start_streaming's
             * width and height name (mgcommands.h); one that differs is
             * still shown: its top left window of at most the stream's size
             * (an uncropped one padded to whole macroblocks: the padding is
             * left out), or a smaller one whole. Said once a stream. */
            int ph = bands == 2 ? h + s_band.h : h;
            if (pic.w != (int)s_params.width || ph != (int)s_params.height) {
                size_told = gen;
                ESP_LOGW(TAG, "pictures are %dx%d after cropping, the stream said %ux%u: showing %dx%d", pic.w, ph,
                         s_params.width, s_params.height, pic.w < (int)s_params.width ? pic.w : (int)s_params.width,
                         ph < (int)s_params.height ? ph : (int)s_params.height);
            }
        }
        if (got) {
            sl->w = pic.w;
            sl->h0 = h;
            sl->h = bands == 2 ? h + s_band.h : h;
            sl->p0 = pic;
            sl->pic = pic.y;   /* the same buffer progress reported, with no output reordering */
            sl->rows = h;
            vd_slot_map((int)(sl - s_slot), &pic);   /* what changed (changed-only redraw), before done */
        } else {
            sl->failed = true;
            vd_slot_none((int)(sl - s_slot));
            vd_break();   /* a failed decode may have touched the picture the next one predicts from */
        }
        sl->due = pace_decoded(due);
        sl->done = true;
        SLOT_EV(sl - s_slot, 'D');
        /* A decode error, or a keyframe that made no picture (it repaired
         * nothing): drop it, wait for a keyframe, ask (muse_vgate.h). */
        if (muse_vgate_result(&gate, ok, got, key, now, &ask)) {
            static int64_t told;
            if (now - told > 2000000) {
                told = now;
                ESP_LOGW(TAG, "frame %u not decoded (%d bytes, %s%s%s): waiting for a keyframe", fr.number, (int)n,
                         key ? "keyframe" : "not a keyframe", ok ? ", no picture" : "",
                         bands == 2 ? ", two bands" : "");
            }
            s_dropped++;
            s_dec_errors++;
            if (ask) {
                ask_keyframe();
            }
            continue;
        }
        if (!got) {
            continue;
        }
        if (key && !s_ui) {
            /* Keyframes are few in video: each one's decode time, to tell the
             * first's (which sets up the decoder's pictures) from the rest. */
            ESP_LOGI(TAG, "keyframe %u: %d bytes, decode %u.%u ms", fr.number, (int)n, (unsigned)(us / 1000),
                     (unsigned)(us % 1000 / 100));
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
            {
                uint32_t mhz = VIDEO_CPU_MHZ, cv, ex, cp;
                muse_yuv_fill_breakdown(&cv, &ex, &cp);
                ESP_LOGI(TAG, "show work per frame: fill() %u ms incl. waits, fill CPU %u us (convert %u, spread %u, "
                         "copy %u), overlays %u ms", (unsigned)(s_fill_us / ln / 1000),
                         (unsigned)(muse_yuv_fill_cycles() / mhz / ln), (unsigned)(cv / mhz / ln),
                         (unsigned)(ex / mhz / ln), (unsigned)(cp / mhz / ln), (unsigned)(s_over_us / ln / 1000));
            }
            s_fill_us = s_over_us = 0;
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
            vd_mem_t *vm = s_vd;   /* (this task's own) */
            if (vm && (vm->n_win || vm->n_none || vm->n_full)) {
                /* Changed-only redraw over the same 2 s: pictures windowed (the share of macroblocks
                 * changed, of cells and of pixels drawn, windows each, cells the overlays added), unchanged, drawn
                 * whole and why; per picture: decode, digest, convert (incl. overlays' wait-free fill) and panel
                 * write (the rest of the draw: the bus, the panel), windowed and whole; TE: no hold on this path;
                 * the shadow compare so far. */
                uint32_t nw = vm->n_win, nn = vm->n_none, nf = vm->n_full, d = nw ? nw : 1, df = nf ? nf : 1;
                uint32_t dgn = vm->dg_n ? vm->dg_n : 1, mba = vm->mb_all ? vm->mb_all : 1;
                char why[160];
                int o = 0;
                why[0] = 0;
                for (int i = 0; i < VD_WHY_N && o < (int)sizeof(why) - 24; i++) {
                    if (vm->why[i]) {
                        o += snprintf(why + o, sizeof(why) - (size_t)o, "%s%s %u", o ? ", " : "", s_vd_why_name[i],
                                      (unsigned)vm->why[i]);
                    }
                    vm->why[i] = 0;
                }
                uint32_t cw = vm->fill_win_us / d, cf = vm->fill_full_us / df;
                /* The board's TE hold is inside the draw: panel write is the draw less convert and that. */
                uint32_t tw = vm->te_n_win ? vm->te_win / vm->te_n_win : 0, tf = vm->te_n_full ? vm->te_full / vm->te_n_full : 0;
                uint32_t pw = vm->us_win / d > cw + tw ? vm->us_win / d - cw - tw : 0;
                uint32_t pf = vm->us_full / df > cf + tf ? vm->us_full / df - cf - tf : 0;
                ESP_LOGI(TAG, "vdirty: %u windowed (MBs changed %u.%u%%, cells %u.%u%%, drawn %u.%u%% of px, %u.%u "
                              "windows, overlay cells %u.%u, hidden cells skipped %u.%u), %u unchanged, %u whole (%s); "
                              "decode %u ms, digest %u.%u ms, "
                              "convert %u.%u / %u.%u ms, panel write %u.%u / %u.%u ms, TE wait %u.%u / %u.%u ms "
                              "(windowed / whole); shadow %u compared, %u differ",
                         (unsigned)nw, (unsigned)(vm->mb_chg * 1000ull / mba / 10), (unsigned)(vm->mb_chg * 1000ull / mba % 10),
                         (unsigned)(vm->cell_pm / d / 10), (unsigned)(vm->cell_pm / d % 10),
                         (unsigned)(vm->area_pm / d / 10), (unsigned)(vm->area_pm / d % 10),
                         (unsigned)(vm->wins * 10 / d / 10), (unsigned)(vm->wins * 10 / d % 10),
                         (unsigned)(vm->ovl_cells * 10 / d / 10), (unsigned)(vm->ovl_cells * 10 / d % 10),
                         (unsigned)(vm->hidden_cells * 10 / d / 10), (unsigned)(vm->hidden_cells * 10 / d % 10),
                         (unsigned)nn, (unsigned)nf, o ? why : "-", (unsigned)avg_ms,
                         (unsigned)(vm->dg_us / dgn / 1000), (unsigned)(vm->dg_us / dgn / 100 % 10),
                         (unsigned)(cw / 1000), (unsigned)(cw / 100 % 10), (unsigned)(cf / 1000), (unsigned)(cf / 100 % 10),
                         (unsigned)(pw / 1000), (unsigned)(pw / 100 % 10), (unsigned)(pf / 1000), (unsigned)(pf / 100 % 10),
                         (unsigned)(tw / 1000), (unsigned)(tw / 100 % 10), (unsigned)(tf / 1000), (unsigned)(tf / 100 % 10),
                         (unsigned)s_vd_cmp_n, (unsigned)s_vd_cmp_bad);
                vm->n_win = vm->n_none = vm->n_full = 0;
                vm->cell_pm = vm->area_pm = vm->wins = vm->us_win = vm->us_full = 0;
                vm->fill_win_us = vm->fill_full_us = vm->ovl_cells = 0;
                vm->dg_us = vm->dg_n = vm->mb_chg = vm->mb_all = 0;
                vm->te_win = vm->te_full = vm->te_n_win = vm->te_n_full = vm->hidden_cells = 0;
            }
            s_vl.dec_avg_sum += avg_ms;
            s_vl.dec_windows++;
            if (dec_us_max / 1000 > s_vl.dec_max) {
                s_vl.dec_max = (uint32_t)(dec_us_max / 1000);
            }
            uint32_t f = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
            if (f < s_vl.low) {
                s_vl.low = f;
            }
            {
                int cw, ch, reach;
                if (muse_vdec_coded(dec, &cw, &ch, &reach)) {
                    if (cw != s_vl.cw || ch != s_vl.ch || reach != s_vl.reach) {
                        ESP_LOGI(TAG, "H.264 coded %dx%d: motion reach %d rows%s", cw, ch, reach,
                                 reach ? "" : " (the whole picture)");
                    }
                    s_vl.cw = (uint16_t)cw;
                    s_vl.ch = (uint16_t)ch;
                    s_vl.reach = (int16_t)reach;
                }
            }
            s_pace_waits_us = s_pace_late = s_late_in = s_slot_wait_us = 0;
            s_gap_max = s_gap_late = 0;
            rx_last = rx;
            cpu_report();
#if CONFIG_MUSE_VIDEO_VP8_PROF
            if (s_params.codec == MUSE_VIDEO_CODEC_VP8) {
                /* libvpx_vp8 built with MGX_PROF: CPU cycles per stage */
                extern unsigned vp8_mgx_prof[6];
                unsigned f = vp8_mgx_prof[5] ? vp8_mgx_prof[5] : 1, mhz = 240;
                ESP_LOGI(TAG, "vp8 stages (us/frame): tokens %u, prediction %u, residual %u, extend %u, modes %u",
                         vp8_mgx_prof[0] / f / mhz, vp8_mgx_prof[1] / f / mhz, vp8_mgx_prof[2] / f / mhz,
                         vp8_mgx_prof[3] / f / mhz, vp8_mgx_prof[4] / f / mhz);
                memset(vp8_mgx_prof, 0, sizeof(vp8_mgx_prof));
            }
#endif
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
    bool stuck = (stage != 0 && stage != SHOW_STAGE_DUMP && now - s_stage_t > 3000000) || s_refusals > 6 ||
                 (slot_wait && now - slot_wait > 3000000);
    if (!stuck) {
        dumped = false;
    }
    if (stuck && !dumped) {
        dumped = true;
        ESP_LOGW(TAG, "stuck (show stage %d, %d refusals, asleep %d, ui %d): task backtraces follow", stage,
                 s_refusals, muse_state_asleep(), s_ui_up);
#if !CONFIG_IDF_TARGET_ESP32C6   /* (IDF v6.0.1 has it on Xtensa and the S31's master, not the C6) */
        esp_backtrace_print_all_tasks(12);
#endif
    }
    /* A panel that hasn't taken a frame in 10 s won't: start over rather
     * than leave the gadget dark and deaf (the phone reconnects). */
    if (stage >= 1 && stage <= 3 && now - s_stage_t > 10000000) {
        ESP_LOGE(TAG, "the panel has been stuck for 10 s (stage %d): restarting", stage);
        esp_restart();
    }
}

/* A decode task, its stack in main SRAM if there is a block that big, else
 * anywhere internal (slow, but video still works), saying so. */
static bool start_tasks_locked(void);


static bool start_decode_task(TaskFunction_t fn, const char *name, TaskHandle_t *h, int core)
{
    if (xTaskCreatePinnedToCoreWithCaps(fn, name, VIDEO_DEC_STACK, NULL, 4, h, core, VIDEO_DEC_STACK_CAPS) == pdPASS) {
        return true;
    }
    /* PSRAM, never other internal RAM: the heap hands out LP/RTC RAM as
     * MALLOC_CAP_INTERNAL too, and SIMD loads from a stack there fault
     * (muse_mem.h). */
    ESP_LOGW(TAG, "%s: no %d-byte block of main SRAM for its stack (largest %u): in PSRAM, decoding will be slow",
             name, VIDEO_DEC_STACK, (unsigned)heap_caps_get_largest_free_block(VIDEO_DEC_STACK_CAPS));
    return xTaskCreatePinnedToCoreWithCaps(fn, name, VIDEO_DEC_STACK, NULL, 4, h, core,
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) == pdPASS;
}

/* The queues and buffers once; the tasks whenever they aren't running (they
 * retire between streams: retire_tasks). */
static bool start_tasks_locked(void)
{
    if (s_dec_task && s_show_task) {
        return true;
    }
    if (!s_frames) {
        static esp_timer_handle_t watch;
        const esp_timer_create_args_t wa = { .callback = stuck_check, .name = "video_watch" };
        if (esp_timer_create(&wa, &watch) == ESP_OK) {
            esp_timer_start_periodic(watch, 1000000);
        }
        s_frames = xQueueCreate(5, sizeof(int));   /* two pictures, a still patch and a mic ring repaint */
        s_slots = xSemaphoreCreateCounting(2, 2);
        muse_mic_ring_set_listener(on_ring);
        s_au = heap_caps_malloc(AU_MAX, BIG_CAPS);
    }
    if (!s_frames || !s_slots || !s_au) {
        ESP_LOGE(TAG, "no memory");
        return false;
    }
    /* The decode task first: its 6-8 KB stack needs the largest block of
     * internal RAM, and on the StopWatch at a stream's start that block is
     * barely big enough (~7 KB). The show task's 3 KB fits a smaller one;
     * created first, it carved the big block and the decoder never started
     * (every start_video answered busy). */
    if (!s_dec_task && !start_decode_task(decode_task, "video_dec", &s_dec_task, DECODE_CORE)) {
        s_dec_task = NULL;
        ESP_LOGE(TAG, "no memory for the decode task (internal %u free, largest %u)",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        return false;
    }
    /* Its stack in main SRAM (SIMD constants may sit on it:
     * never LP/RTC RAM, muse_mem.h), else PSRAM. */
    if (!s_show_task &&
        xTaskCreatePinnedToCoreWithCaps(show_task, "video_show", SHOW_STACK, NULL, MUSE_UI_PRIORITY, &s_show_task,
                                        MUSE_UI_CORE, MUSE_SIMD_CAPS) != pdPASS &&
        xTaskCreatePinnedToCoreWithCaps(show_task, "video_show", SHOW_STACK, NULL, MUSE_UI_PRIORITY, &s_show_task,
                                        MUSE_UI_CORE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        s_show_task = NULL;
        ESP_LOGE(TAG, "no memory for the show task (internal %u free, largest %u)",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        return false;
    }
    return true;
}


/*
 * The second band's decode task, started by the first stream that asks for
 * two bands (video on the S31, a two-band UI stream anywhere), not before:
 * its stack is 6 KB of main SRAM for good, and on the S3 most streams are one
 * band (the apps send UI in one band by default), where internal RAM is what
 * runs short.
 */
static bool start_band_task(void)
{
    if (s_band_task) {
        return true;
    }
    if (!s_band_done) {
        s_band_done = xSemaphoreCreateBinary();
    }
    if (!s_band_done || !start_decode_task(band_task, "video_band", &s_band_task, MUSE_UI_CORE)) {
        ESP_LOGE(TAG, "no memory for the second band's task (internal %u free, largest %u)",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        return false;
    }
    return true;
}

static void log_memory(const char *when)
{
    ESP_LOGI(TAG, "memory %s: internal %u free (largest %u, lowest ever %u), PSRAM %u free", when,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

/* ---- the transports' side ---------------------------------------------- */

static bool begin_locked(const muse_video_transport_t *t, const muse_video_params_t *vp);

/* Under the task lock: the tasks can't retire between starting and the stream being on. */
bool muse_video_begin(const muse_video_transport_t *t, const muse_video_params_t *vp)
{
    task_lock();
    bool ok = begin_locked(t, vp);
    task_unlock();
    return ok;
}

static bool begin_locked(const muse_video_transport_t *t, const muse_video_params_t *vp)
{
    s_shown32_ok = false;   /* a new stream: no picture of it shown yet */
    /* A client that reconnects at once can ask before the panel is up (2 s
     * into boot); drawing then would wait forever on a sender that doesn't
     * exist yet. It hears busy and asks again. */
    if (!muse_video_supported()) {
        return false;
    }
    if (!start_tasks_locked()) {
        return false;
    }
    if (vp->tiles == 2 && !start_band_task()) {
        return false;   /* busy: no room for the second band's decoder */
    }
    memset(&s_vl, 0, sizeof(s_vl));
    s_vl.w = vp->width;
    s_vl.h = vp->height;
    s_vl.reach = -1;
    s_vl.t0 = esp_timer_get_time();
    s_vl.start_free = s_vl.low = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    s_vl.start_largest = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    s_vl.lowest_ever0 = (uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    if (muse_state_asleep() || !s_ui_up) {
        s_refusals++;
        /* Wake the screen, from the show task: here the protocol is locked,
         * and waking takes the display lock, which the UI holds while it asks
         * the protocol for the link state. The client's retry finds it on.
         * Never while the lock holds video: it waits for the user. */
        if (!held()) {
            s_wake_req = true;
        }
        return false;
    }
    if (s_streaming && s_tp && s_tp != t) {
        ESP_LOGW(TAG, "%s asks for a stream while %s has one: busy", t->name, s_tp->name);
        return false;
    }
    s_refusals = 0;
    s_tp = t;
    s_params = *vp;
    s_ui = vp->content == MUSE_VIDEO_CONTENT_UI;
    if (s_ui) {
        s_params.playout_ms = 0;   /* refused by the transport anyway: each frame as soon as it's decoded */
    }
    t->ring->codec = vp->codec;   /* how the ring tells a frame from noise */
    t->ring->bands = vp->tiles == 2 && s_band_task ? 2 : 1;   /* and in which layout (muse_vframe_t.bands) */
    s_gen++;
    s_shown = s_dropped = 0;
    s_last_shown_t = 0;
    s_open = true;   /* a client may start one without the menu: show it */
    s_ext_reset = true;
    s_streaming = true;
    xTaskNotifyGive(s_dec_task);
    return true;
}


/* Wakes the video's tasks (through the decode task's notification), from any
 * other task. Under the task lock: the show task may be retiring the tasks
 * (deleting them) at that moment. */
static void notify_video(void)
{
    task_lock();
    if (s_dec_task) {
        xTaskNotifyGive(s_dec_task);
    }
    task_unlock();
}

void muse_video_set_hold(bool hold)
{
    if (hold != s_hold) {
        s_hold = hold;
        ESP_LOGI(TAG, "%s", hold ? "held: locked (shown once unlocked, if still live)" : "no longer held");
        notify_video();   /* the show task gives the screen back (or takes it) */
    }
}

void muse_video_set_cover(bool cover)
{
    if (cover != s_cover) {
        s_cover = cover;
        ESP_LOGI(TAG, "%s", cover ? "held: a passcode keypad is up" : "keypad closed");
        notify_video();   /* the show task gives the screen back (or takes it) */
    }
}

bool muse_video_ui_active(void)
{
    return s_streaming && s_ui;
}

void muse_video_wake(void)
{
    if (s_streaming) {
        notify_video();
    }
}

void muse_video_end(const muse_video_transport_t *t)
{
    if (t != s_tp) {
        return;   /* not the stream that's on */
    }
    s_streaming = false;
    notify_video();
}

/* ---- the view ----------------------------------------------------------- */

void muse_video_open(void)
{
    if (!muse_video_supported()) {
        return;
    }
    ESP_LOGI(TAG, "view open");
    s_open = true;
    s_view = true;
#if CONFIG_MUSE_APP_VIEW
    /* After s_open: an app view opening meanwhile sees the video and closes
     * (muse_app_view_open), or is open and stops here. */
    muse_app_view_preempt(MUSE_APP_STOP_VIDEO);
#endif
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
    s_view = false;
    for (int i = 0; i < s_ntransports; i++) {
        if (s_transports[i]->request) {
            s_transports[i]->request(false);   /* the clients stop */
        }
    }
    if (s_streaming && s_tp && s_tp->stop) {
        s_tp->stop();   /* ends the stream on our side at once */
    }
    s_streaming = false;
    task_lock();   /* the tasks may be retiring (long after a stream) */
    if (s_dec_task) {
        xTaskNotifyGive(s_dec_task);
    }
    task_unlock();
    /* The show task gives LVGL back within 100 ms (pause_lvgl is its alone). */
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
    /* held: the locked face has the screen. */
    bool open = s_open && !held();
    if (lv_obj_has_flag(s_root, LV_OBJ_FLAG_HIDDEN) == open) {
        lv_obj_set_flag(s_root, LV_OBJ_FLAG_HIDDEN, !open);
        if (open) {
            lv_obj_move_foreground(s_root);
        }
    }
    if (s_open && !held()) {
        /* This tick runs only while the screen is on, so the panel is on:
         * safe to draw. */
        s_awake_tick = esp_timer_get_time();
    }
    if (!open) {
        if (s_redraw) {
            s_redraw = false;
            lv_obj_invalidate(lv_screen_active());
        }
        return false;
    }
    const char *title = s_streaming && s_ui ? "Phone screen" : "Phone camera";
    if (strcmp(lv_label_get_text(s_title), title) != 0) {
        lv_label_set_text(s_title, title);
    }
    const char *text = s_streaming               ? "Starting..."
                       : muse_video_has_client() ? "Waiting for the phone..."
                                                 : "Open Muse Gadgets Test on your phone";
    if (strcmp(lv_label_get_text(s_caption), text) != 0) {
        lv_label_set_text(s_caption, text);
    }
    if (s_redraw) {
        s_redraw = false;
        lv_obj_invalidate(lv_screen_active());
    }
    return true;
}

int muse_video_last_summary(char *out, size_t cap)
{
    unsigned secs = s_vl.t1 > s_vl.t0 ? (unsigned)((s_vl.t1 - s_vl.t0) / 1000000) : 0;
    unsigned fps10 = secs ? s_vl.shown * 10 / secs : 0;
    return snprintf(out, cap,
                    "video %ux%u coded %ux%u reach %d, %u shown %u dropped in %u s (%u.%u fps)%s%s\n"
                    "decode avg %u ms max %u ms, CPU peak %u%%\n"
                    "internal: start %u/%u, decoder open %u/%u, low %u (lowest ever %u -> %u)",
                    s_vl.w, s_vl.h, s_vl.cw, s_vl.ch, (int)s_vl.reach, (unsigned)s_vl.shown, (unsigned)s_vl.dropped, secs, fps10 / 10,
                    fps10 % 10, s_vl.refused ? ", refused: " : "", s_vl.refused ? s_vl.refused : "",
                    (unsigned)(s_vl.dec_windows ? s_vl.dec_avg_sum / s_vl.dec_windows : 0), (unsigned)s_vl.dec_max,
                    (unsigned)s_vl.cpu_peak, (unsigned)s_vl.start_free, (unsigned)s_vl.start_largest,
                    (unsigned)s_vl.dec_free, (unsigned)s_vl.dec_largest, (unsigned)s_vl.low,
                    (unsigned)s_vl.lowest_ever0, (unsigned)(s_vl.lowest_ever1 ? s_vl.lowest_ever1 : s_vl.lowest_ever0));
}
