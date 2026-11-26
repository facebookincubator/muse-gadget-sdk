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

/*
 * Changed-only redraw of streamed video (muse_video.c): a
 * panel with its own memory (GRAM) keeps the picture it was sent, so a
 * picture whose macroblocks mostly equal the one before needs only the
 * changed ones converted and written, as a few windows. This is the pure
 * part (host-tested: tests/test_muse_vdirty.py), for scaled pictures:
 *
 * - Which macroblocks changed: a 64-bit digest of each macroblock's Y, U and
 *   V (muse_vdirty_digest) against the previous picture's, or the decoder's
 *   own map where it has one (H.264's muse_vdec_same).
 *   The digest sees the decoded pixels, so deblocking, skips with a motion
 *   vector and intra blocks that repeat are all exact.
 * - Where they land on the panel: the output is cut into cells
 *   (muse_vdirty_axis), each axis at the output pixels where the scaler's
 *   source macroblock changes (a nearest-pixel scaler: one to one, two
 *   times, the fill, a round panel's cover), each cut moved up to the
 *   panel's alignment. A cell reads one or two macroblocks a side; it is
 *   dirty if any of them changed (muse_vdirty_cells). Windows are whole
 *   cells, so they never cut through a cell that has to be redrawn.
 * - The windows (muse_vdirty_plan): one per cell row's first to last dirty
 *   cell, rows merged where one window costs less than two, or the whole
 *   picture when the windows would cover most of it or be too many.
 * - The shadow compare's model of the panel (muse_vdirty_model_rows): a
 *   digest per cell of the pixels last sent there, the same digest of a
 *   picture converted whole to compare with. No copy of the frame: it fits
 *   a board without PSRAM.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "muse_board_win.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- the digest ----------------------------------------------------------
 * Two 32-bit lanes, each word of input mixed into both (multiply, rotate,
 * multiply: a bijection of the lane for each word, so two inputs that differ
 * in one word never collide). Two different inputs of the same length
 * collide with a probability around 2^-64 per comparison. */
typedef struct {
    uint32_t a, b;
} muse_vdirty_hash_t;

void muse_vdirty_hash_init(muse_vdirty_hash_t *h);
void muse_vdirty_hash_bytes(muse_vdirty_hash_t *h, const uint8_t *p, int n);
static inline bool muse_vdirty_hash_eq(const muse_vdirty_hash_t *x, const muse_vdirty_hash_t *y)
{
    return x->a == y->a && x->b == y->b;
}

/*
 * Each macroblock's digest of a w x h 4:2:0 picture (w even), the grid from
 * its top left ((w + 15) / 16 x (h + 15) / 16; the right and bottom ones may
 * be partial), into changed (1: it differs from dg's, or prev_ok is false)
 * and dg (now this picture's). same (may be NULL): the decoder's map of
 * macroblocks equal to the picture dg holds, same_cols a row; those keep
 * their digest without being read. Returns the macroblocks changed.
 */
int muse_vdirty_digest(const uint8_t *y, const uint8_t *u, const uint8_t *v, int ystride, int cstride, int w, int h,
                       muse_vdirty_hash_t *dg, bool prev_ok, const uint8_t *same, int same_cols, uint8_t *changed);

/* acc |= map, n bytes (a union of changed maps). */
void muse_vdirty_or(uint8_t *acc, const uint8_t *map, int n);

/* ---- the output's cells ------------------------------------------------- */
#define MUSE_VDIRTY_MAX_CELLS 32   /* a side: the Mosaico's 480 one to one is 30 */

typedef struct {
    int n;                                      /* cells */
    int16_t b[MUSE_VDIRTY_MAX_CELLS + 1];       /* cell i: output pixels [b[i], b[i + 1]) */
    uint8_t m0[MUSE_VDIRTY_MAX_CELLS];          /* the source macroblocks it reads: m0 .. m1 */
    uint8_t m1[MUSE_VDIRTY_MAX_CELLS];
} muse_vdirty_axis_t;

/*
 * The source pixel output pixel o shows (a nearest-pixel scaler, as
 * muse_video_fill_src): floor(((o + crop) * 2 + 1) * den / (2 * num)),
 * clamped to [0, src_n). One to one is num = den = 1 (crop: the first source
 * row shown), two times num 2, den 1 (a cover's crop, negative where it
 * extends the edge), the fill out / vis.
 */
int muse_vdirty_src(int o, int crop, int num, int den, int src_n);

/*
 * One axis's cells for out_n output pixels of src_n source pixels, every
 * cut but the end on a multiple of align (a power of two, or 1). false
 * (ax->n 0): more than MUSE_VDIRTY_MAX_CELLS cells, or bad arguments.
 */
bool muse_vdirty_axis(muse_vdirty_axis_t *ax, int out_n, int src_n, int num, int den, int crop, int align);

/*
 * The cells (ax->n a row, ay->n rows) dirty for a changed-macroblock map
 * (mb_cols a row, mb_rows rows); a cell reading a macroblock outside the
 * map is dirty.
 */
void muse_vdirty_cells(const uint8_t *mb, int mb_cols, int mb_rows, const muse_vdirty_axis_t *ax,
                       const muse_vdirty_axis_t *ay, uint8_t *cells);

/* Marks every cell a rectangle of the output (x, y, w, h: from the picture's top left) touches. */
void muse_vdirty_mark(uint8_t *cells, const muse_vdirty_axis_t *ax, const muse_vdirty_axis_t *ay, int x, int y, int w,
                      int h);

/*
 * A round panel: true if cell (cx, cy) lies wholly outside the circle of
 * radius r around (ox, oy) (output coordinates, with a pixel to spare), so
 * nothing of it shows. muse_vdirty_clip_round clears those cells in a cell
 * map and returns how many it cleared: a picture's corners outside the
 * circle are never pushed again (static areas).
 */
bool muse_vdirty_cell_hidden(const muse_vdirty_axis_t *ax, const muse_vdirty_axis_t *ay, int cx, int cy, int ox, int oy,
                             int r);
int muse_vdirty_clip_round(uint8_t *cells, const muse_vdirty_axis_t *ax, const muse_vdirty_axis_t *ay, int ox, int oy,
                           int r);

/* ---- the windows -------------------------------------------------------- */
/* A window: output pixels, from the picture's top left (the boards' muse_board_win_t, so draw_windows takes
 * them as they are). */
typedef muse_board_win_t muse_vdirty_win_t;

typedef struct {
    int win_cost;      /* a window's fixed cost (two panel commands, a piece's start), in cells' worth */
    int full_pct;      /* windows over this share of the picture's pixels: the whole picture instead */
    int max_windows;   /* more windows than this: the whole picture instead */
} muse_vdirty_cfg_t;

typedef enum {
    MUSE_VDIRTY_WINDOWS,     /* draw the windows */
    MUSE_VDIRTY_NONE,        /* nothing changed: nothing to draw */
    MUSE_VDIRTY_FULL_AREA,   /* the windows would cover more than full_pct: draw it all */
    MUSE_VDIRTY_FULL_MANY,   /* more than max_windows: draw it all */
} muse_vdirty_res_t;

typedef struct {
    muse_vdirty_res_t res;
    int dirty;      /* dirty cells */
    int cells;      /* cells in all */
    int area;       /* pixels the windows cover */
    int total;      /* pixels in the picture */
} muse_vdirty_stats_t;

/* The windows for the dirty cells, top to bottom: up to max_out into out; returns how many (0 but for
 * MUSE_VDIRTY_WINDOWS). */
int muse_vdirty_plan(const uint8_t *cells, const muse_vdirty_axis_t *ax, const muse_vdirty_axis_t *ay,
                     const muse_vdirty_cfg_t *cfg, muse_vdirty_win_t *out, int max_out, muse_vdirty_stats_t *st);

/* ---- the panel's model (the shadow compare) ------------------------------
 * cell_h (ax->n x ay->n): each cell's digest of its pixels, row by row.
 * Rows [y, y + rows) of output columns [x, x + w) (from the picture's top
 * left; px the first, stride bytes a row, RGB565), as written to the panel:
 * every cell inside the columns takes them, starting over at its first row.
 * A write has to cover whole cells, top to bottom; false if its columns cut
 * one. */
bool muse_vdirty_model_rows(muse_vdirty_hash_t *cell_h, const muse_vdirty_axis_t *ax, const muse_vdirty_axis_t *ay,
                            const uint8_t *px, int stride, int x, int y, int w, int rows);

#ifdef __cplusplus
}
#endif
