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
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "muse_vstream.h"
#include "muse_board_win.h"

/*
 * Full-screen video from a phone (CONFIG_MUSE_VIDEO): its camera, say, as
 * H.264, carried by a transport (Wi-Fi: muse_video_wifi.c) into the
 * transport's ring (muse_vstream.h). The menu's Phone camera item opens the
 * view and asks the transports' clients for a stream; any button closes it.
 * Frames are decoded on the first core (two bands: one per core) and drawn
 * on the UI core straight to the panel (LVGL paused), scaled to fit.
 * ../../../protocols/VIDEO.md has the design.
 */

/* What muse_video tells a transport after each frame, for its client. */
typedef struct {
    uint16_t shown, dropped;   /* since the stream started */
    uint8_t decode_ms;         /* average over the last stats period */
    uint16_t last_number;      /* the frame number of the last picture the panel finished drawing */
    uint16_t last_age_ms;      /* how long ago; 0xffff: none yet */
} muse_video_status_t;

/* A way for streams to arrive. Every callback may be called from the decoder
 * or the UI's tasks; none may block for long. */
typedef struct {
    const char *name;
    muse_vstream_t *ring;
    /* The decoder took a frame out of the ring: room for more (credits). */
    void (*freed)(const muse_video_status_t *st);
    /* The decoder can't go on until a keyframe. */
    void (*keyframe)(void);
    /* The gadget ends the stream (out of memory, or the view closed). */
    void (*stop)(void);
    /* The view opened (true) or closed: ask a client to start or stop. */
    void (*request)(bool want);
    /* A client that can stream is connected. */
    bool (*has_client)(void);
} muse_video_transport_t;

/* The board can show video (a panel driver with draw_frame). */
bool muse_video_supported(void);
/* The view is up (the menu picked it, or a client started a stream). */
bool muse_video_is_open(void);
/* The user opened the view (the menu, a key, the console's video=1), not only a client's start. */
bool muse_video_view_open(void);
/* A stream is on, or the decoder still holds its memory (it closes a moment after the stream ends). */
bool muse_video_decoding(void);
/* From the menu: open the view and ask the clients for a stream. */
void muse_video_open(void);
/* Close the view and end the stream (any button while it's open). */
void muse_video_close(void);
/* The passcode lock: while held,
 * a UI stream or phone camera view is kept off the screen like a drawn file
 * (its pictures dropped, the screen never woken for them, the view hidden)
 * and shows once unlocked if still live.
 * Any task (muse_passcode.c at every lock change). */
void muse_video_set_hold(bool hold);
/* The same while a passcode keypad (Set, Change or Turn off passcode) covers
 * the screen of an unlocked gadget, so its touches and picture stay on the
 * gadget. LVGL's task (muse_passcode_ui.c). */
void muse_video_set_cover(bool cover);
/* The last stream's numbers (size, pictures shown, decode times, CPU peak, internal RAM at its start, once
 * the decoder opened and at its lowest), three lines. */
int muse_video_last_summary(char *out, size_t cap);
/* From the UI's frame tick, LVGL locked: keeps the view's captions current.
 * True while the view is up (the face isn't drawn). */
bool muse_video_tick(void);
/* A client of some transport is connected (the menu's Ready / No phone). */
bool muse_video_has_client(void);
/* LVGL is paused: frames go straight to the panel and LVGL reads no touch. */
bool muse_video_drawing(void);
/* The live video picture on the panel now, by its frame number extended to 32
 * bits (the A/V anchor); false while no picture of
 * the running stream has been drawn. Any task. */
bool muse_video_shown32(uint32_t *frame);
/*
 * Something drawn over the video (a board's own overlay): called on the
 * show task with each piece of a frame once it's filled, RGB565 in the
 * panel's byte order, with where the piece's first pixel is on the panel.
 * It must be quick and must not block. NULL: nothing.
 */
typedef void (*muse_video_overlay_t)(uint8_t *dst, int stride, int x, int y, int w, int rows);
void muse_video_set_overlay(muse_video_overlay_t overlay);
/*
 * Where the overlay draws now (changed-only redraw, below): up to max
 * rectangles in panel coordinates, each covering every pixel it may touch,
 * and *gen, a count that changes whenever anything it draws shows, hides or
 * changes. Any task; quick. When gen changes, the next picture redraws
 * those rectangles (where it draws now and where it drew); unchanged, a
 * window over one is drawn with it and the rest of the panel keeps it. NULL
 * (the default with an overlay set): every picture whole.
 */
typedef int (*muse_video_overlay_rects_t)(muse_board_win_t *out, int max, uint32_t *gen);
void muse_video_set_overlay_rects(muse_video_overlay_rects_t rects);

/*
 * Changed-only redraw of streamed video (muse_vdirty.h;
 * boards with draw_windows): a picture is drawn as windows over only what
 * changed since the picture the panel holds (each macroblock's digest
 * against the last picture's, through the scaler to the panel's cells), the
 * overlays' rectangles with it; the panel's memory keeps the rest. On by
 * default; console
 * >vdirty=0|1 (A/B, RAM only).
 * Shadow compare (>vdirty.shadow=N, the check that it's right with nobody
 * watching the panel): after each of the next N windowed pictures, the
 * picture converted whole with its overlays, the mic ring and the turn dot,
 * against a per-cell digest of everything the panel was sent; differing
 * cells and their pixels are logged ("vdirty shadow"), the pictures after an
 * overlay, ring, size or place change tagged with it.
 */
void muse_video_set_dirty(bool on);
void muse_video_dirty_shadow(int frames);
/* win_cost: a window's fixed cost in cells; full_pct: windows over this share of the picture draw it whole;
 * max_windows: more than this draws it whole. Values <= 0 keep the current one. */
void muse_video_dirty_tune(int win_cost, int full_pct, int max_windows);
/* One line: on/off, the tuning, the shadow compare's result so far. */
void muse_video_dirty_status(char *buf, size_t n);
/*
 * The reply's audio clock (lip sync; mg_ble_av_clock): true while a
 * reply plays after a resolved A/V anchor, with the anchor's picture and the
 * time heard since its sample. The boards here show every picture, so it
 * decides nothing yet. Set once at start.
 */
void muse_video_set_audio_clock(bool (*clock)(uint32_t *frame, int32_t *pos_us));
/* Console (automation words, RAM only): >lipsync=0|1 (0: measured only, the old rules decide: the A/B),
 * >lipsync.tune=LATE,RESYNC,GAP,MAXSKIP (ms, ms, ms, count; out of range keeps the current one),
 * >lipsync? : on/off, the tuning, and this stream's replies so far. */
void muse_video_avsync(bool on);
void muse_video_avsync_tune(int late_ms, int resync_ms, int gap_ms, int max_skips);
int muse_video_avsync_status(char *out, size_t cap);
/*
 * Boards (draw_frame and draw_windows): each piece as it goes to the panel,
 * after the mic ring and the turn dot, with where it is on the panel. Only
 * the shadow compare reads it (it returns at once otherwise).
 */
void muse_video_piece_sent(const uint8_t *px, int stride, int x, int y, int w, int rows);
/* Boards that hold a draw for the panel's scan (boards/muse_lcd_bands.c): how long it waited and how long it wrote,
 * once a draw_frame or draw_windows call, for the vdirty: line's TE wait. */
void muse_video_panel_timing(uint32_t te_hold_us, uint32_t write_us);
/* Bench (>shot): the next frame drawn, as the panel gets it (overlays and
 * the mic ring included), RGB565 in the panel's byte order, w x h. False:
 * no frame within timeout_ms (no video). The buffer stays the module's. */
bool muse_video_shot(const uint8_t **px, int *w, int *h, int timeout_ms);
/* Bench: capture the next frame drawn and keep it for the next muse_video_shot
 * (which then returns it at once). Does nothing while a capture is pending. */
void muse_video_shot_arm(void);
/* Bench: the frame captured last (or kept by muse_video_shot_arm), without a new capture. */
bool muse_video_shot_last(const uint8_t **px, int *w, int *h);
/* A UI stream (MUSE_VIDEO_CONTENT_UI) is on: the panel's touches go to the
 * client, not to Muse's own screens. */
bool muse_video_ui_active(void);
/* Bench: log the MD5 of each decoded UI picture (the crop, I420), to compare
 * against the encoder's. */
void muse_video_set_md5(bool on);
/* Bench: dump the last UI picture (decoded planes and RGB565) on the console. */
void muse_video_dump(void);
/* The same dump into a PSRAM buffer (console ui.dump=buf): for a board whose console isn't on its USB. */
void muse_video_dump_to_buffer(void);
/* Copies up to cap bytes of that buffer from off into out, once the dump is complete (any task; the buffer is never
 * handed out, since the show task and ui.dump=buf free it). *total: its length, 0 until it's complete. Returns the
 * bytes copied. */
size_t muse_video_dump_read(size_t off, void *out, size_t cap, size_t *total);
/* Frees that buffer (it stays until then, or until the next ui.dump=buf); a dump still being written is dropped. */
void muse_video_dump_release(void);
/* UI pictures' conversion: true (default) the fine path (muse_yuv_1x_hq), false the fast one. */
void muse_video_set_hq(bool on);

/* ---- for transports ---- */
/* Up to two; call once each, before any begin. */
void muse_video_add_transport(const muse_video_transport_t *t);
/* What this board offers; NULL if it can't show video. */
const muse_video_caps_t *muse_video_caps(void);
/* Bench (video.fps=N, boards without PSRAM): the frame rate the board suggests, RAM only. */
void muse_video_set_suggested_fps(int fps);
/* A client starts a stream into t's ring (which the transport has started).
 * False: busy (another transport streams, or the panel isn't up yet: the
 * client asks again). */
bool muse_video_begin(const muse_video_transport_t *t, const muse_video_params_t *vp);
/* The client (or its connection) ended t's stream. */
void muse_video_end(const muse_video_transport_t *t);
/* Bytes arrived in the ring: wakes the decoder (else it looks every 2 ms). */
void muse_video_wake(void);
/* The receive ring's size every transport should use (CONFIG_MUSE_VIDEO_BUFFER). */
uint32_t muse_video_buffer_size(void);
