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
#include <stdint.h>

#include "muse_vdec.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * A UI stream's picture as the panel shows it (protocols/UI.md): an RGB565
 * frame buffer the decoded pictures and the phone's lossless still patches
 * both write into. Each decoded picture is compared with the last one,
 * macroblock by macroblock, and only the macroblocks that changed are
 * converted (muse_yuv_1x_hq) and redrawn, so a patch stays on the panel
 * wherever the picture hasn't changed since, and a small change (a clock's
 * second) redraws a small area.
 */

typedef struct {
    uint16_t x, y, w, h;
} muse_ui_rect_t;

#define MUSE_UI_MAX_RECTS 8

/* For a stream of w x h pictures (one to one on the panel). False: no memory. */
bool muse_ui_fb_begin(int w, int h);
void muse_ui_fb_end(void);
bool muse_ui_fb_active(void);

/*
 * A decoded picture: the top band's planes (p0, rows 0..h0-1) and, with two
 * bands, the bottom band's (p1); all: redraw everything (a keyframe, a
 * screen that woke). Converts what changed into the frame buffer and returns
 * the rectangles to send to the panel (at most MUSE_UI_MAX_RECTS, even
 * coordinates); 0: nothing changed.
 */
int muse_ui_fb_picture(const muse_pic_t *p0, const muse_pic_t *p1, int h0, bool all, muse_ui_rect_t *rects);
/* (-1, nothing read: a band that doesn't cover the copy's width and its rows, muse_pic_covers.) */

/*
 * A still patch (a frame with MUSE_VIDEO_FRAME_STILL): u16 x, y, w, h, u8
 * format (1: RGB565 big-endian, zlib), then the data. Writes it into the
 * frame buffer and sets *r to its rectangle. False: malformed (nothing
 * written).
 */
bool muse_ui_fb_still(const uint8_t *d, uint32_t n, muse_ui_rect_t *r);

/* Can this chip inflate still patches? The ROM's tinfl, checked once against a
 * known stream (the S3 and S31 ROMs have it; elsewhere false). */
bool muse_ui_fb_can_inflate(void);

/* muse_lcd_fill_fn for a rectangle of the frame buffer (arg: the muse_ui_rect_t). */
void muse_ui_fb_fill(uint8_t *dst, int stride, int row, int rows, void *arg);

/* The frame buffer itself (row-major RGB565, panel byte order), for a dump. */
const uint16_t *muse_ui_fb_pixels(int *w, int *h);

/* Bench numbers of the last picture: macroblocks changed, of total, how many
 * the decoder said were unchanged (not compared), conversion time (us). */
void muse_ui_fb_last_stats(int *mbs, int *total, int *skipped, uint32_t *convert_us);

#ifdef __cplusplus
}
#endif
