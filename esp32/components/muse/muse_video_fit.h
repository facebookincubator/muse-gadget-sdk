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

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Where a video picture goes on the panel, and how it's scaled (muse_video.c's
 * show task), with no IDF in it so tests/test_muse_video_fit.py runs it for
 * every board on the host.
 *
 * Square panels fit: the picture two times upscaled when that fits, else one
 * to one (and on the S31, 3/2 or scaled to the panel's width), centered, black
 * around it. Round panels (muse_board_t.round) cover (they
 * fill the circle): a camera picture is two times upscaled and the
 * window is the whole panel, so the square's corners, which the round mask
 * hides anyway, take the crop, and no black column or row is left inside the
 * circle. A picture whose two times doesn't reach the panel's edge has its
 * edge pixels extended into the sliver (cheaper than scaling more, which
 * would cost frames). UI streams stay one to one everywhere.
 */

typedef struct {
    int panel_w, panel_h;
    bool round;              /* muse_board_t.round */
    int x_align;             /* muse_board_t.frame_x_align, at least 2 */
    int max_src;             /* the decoder's largest side: 240 on the S3, 480 on the S31 */
    bool ui;                 /* a UI stream: one to one */
    int bands;               /* the picture's bands (cover needs one) */
    int pic_w, pic_h;        /* the picture (crop) to show */
    bool fill;               /* a rectangular panel the picture fills (muse_board_t.video_fill): scaled to cover it,
                                the overflow cropped evenly, nearest pixel (muse_yuv_fill) */
    bool rgb;                /* the picture comes as RGB565 rows: no two times cover (its block is YUV) */
    int fill_max_src;        /* the widest picture the fill converter takes (MUSE_YUV_FILL_MAX_SRC; 0: 256) */
} muse_video_fit_in_t;

typedef struct {
    int scale;               /* 2: two times; 1: one to one; 3: 3/2; 0: out_w x out_h; 4: fill (fill_num/fill_den) */
    int vis_w, vis_h;        /* the source picture used */
    int x, y, out_w, out_h;  /* the window on the panel */
    bool cover;              /* round cover: 2x, the window offset into it by crop_x, crop_y */
    int crop_x, crop_y;      /* the window's top left in the 2x picture (< 0: edge extended); fill: in the scaled one */
    int fill_num, fill_den;  /* fill: the scale, num / den (out_w / vis_w or out_h / vis_h, whichever covers) */
    /* Extend (two bands, or wider than the fill takes, where the plain placement falls short of the panel): the
     * window is the whole panel; the picture is placed as the plain path places it (scale), iw x ih at ix, iy in the
     * window, and its edge pixels are extended to the window's edges (no black bars). */
    bool extend;
    int ix, iy, iw, ih;
} muse_video_fit_t;

#define MUSE_VIDEO_FIT_FILL 4

/* Fill: the source row (or column) output row o takes: the one whose centre
 * the output pixel's centre falls in, crop being the window's offset in the
 * scaled picture, clamped to the src_n there are. */
int muse_video_fill_src(int o, int crop, int num, int den, int src_n);

/* false: nothing to show (no width or height). */
bool muse_video_fit(const muse_video_fit_in_t *in, muse_video_fit_t *out);

/*
 * The size a board suggests (capability suggested): on the S31 the
 * panel's own size; on the S3 half of it, two times upscaled, a multiple of 8;
 * on a round S3 panel the smallest multiple of 16 at least half the panel, so
 * the 2x picture covers the circle (412: 208, cropped 2 px a side; 466: 240,
 * 7 px a side). At most max_src.
 */
int muse_video_suggest(int panel_w, int panel_h, bool round, int max_src);

/*
 * Two bands (muse_video.c's fill()): the output row where the top band's
 * part ends, for a fit of scale with vis_h source rows shown as out_h output
 * rows. The bands stack, the picture's rows [0, h0) the top band's (h0: its
 * decoded rows, whatever the stream made them), so the top band shows
 * *vis0 = min(h0, vis_h) of its rows, rounded down to the converter's own
 * granularity: a row pair at 1x and 2x (four output rows), two pairs at 3/2
 * (six output rows, the 3/2 pattern's period), a pair when scaled. Every
 * piece's part on either side of the seam is then whole units of its
 * converter. At most out_h.
 */
int muse_video_band_seam(int scale, int vis_h, int out_h, int h0, int *vis0);

/*
 * Two bands' decoded heights h0 (top) and h1 (bottom) make a stream of height
 * rows (protocols/VIDEO.md: each band whole macroblocks, coded on its own):
 * the top band uncropped (a multiple of 16) and shorter than the stream, and
 * the two exactly the stream's height, or both uncropped and together the
 * stream's height in whole macroblocks.
 */
bool muse_video_bands_ok(int h0, int h1, int height);

/*
 * The cover path's conversion: window rows [row, row + rows) of a cover fit,
 * converted two source rows at a time (four output rows, by up2, the SIMD
 * kernel muse_yuv_up2) into a block of four 2x rows, then copied into dst
 * cropped (or edge extended). The block is kept between calls, so the next
 * piece reuses it: reset blk to -1 for each new picture. buf: 4 rows of
 * 2 * vis_w pixels (MUSE_VIDEO_COVER_BUF(vis_w) bytes), 16-byte aligned.
 */
typedef void (*muse_video_up2_fn)(const uint8_t *y, const uint8_t *u, const uint8_t *v, int ystride, int cstride,
                                  int src_w, int out_rows, uint8_t *out, int out_stride);

#define MUSE_VIDEO_COVER_BUF(vis_w) ((unsigned)(vis_w) * 2 * 2 * 4)

typedef struct {
    uint8_t *buf;
    int blk;                 /* the source row pair in buf (-1: none) */
} muse_video_cover_t;

/* The source rows the window's rows [row, row + rows) need decoded. */
int muse_video_cover_src_rows(const muse_video_fit_t *f, int row, int rows);

void muse_video_cover_fill(muse_video_cover_t *c, const muse_video_fit_t *f, muse_video_up2_fn up2,
                           const uint8_t *y, const uint8_t *u, const uint8_t *v, int ystride, int cstride, int row,
                           int rows, uint8_t *dst, int stride);
/*
 * A window of the same cover (changed-only redraw, muse_vdirty.h): output
 * columns [x, x + w) of rows [row, row + rows), pixel for pixel what
 * muse_video_cover_fill writes there, converting only the source columns
 * the window shows (two times, a pair at a time) into c->buf. first: the
 * window's first piece (c->buf holds another window's columns).
 */
void muse_video_cover_fill_win(muse_video_cover_t *c, const muse_video_fit_t *f, muse_video_up2_fn up2,
                               const uint8_t *y, const uint8_t *u, const uint8_t *v, int ystride, int cstride, int x,
                               int w, int row, int rows, bool first, uint8_t *dst, int stride);

#ifdef __cplusplus
}
#endif
