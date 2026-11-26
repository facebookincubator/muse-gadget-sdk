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
#if defined(__has_include)
#if __has_include("sdkconfig.h")
#include "sdkconfig.h"
#endif
#endif

/*
 * I420 to RGB565 in the panel's byte order (big-endian, as LVGL's
 * RGB565_SWAPPED), BT.601 limited range, for video on the QSPI AMOLEDs.
 * The S3's SIMD does 16 pixels a step (muse_yuv_pie.S); the last pixels of a
 * row that isn't a multiple of 16 take the C path, with the same arithmetic,
 * so both give identical pixels.
 */

/*
 * Every converter writes exactly the destination slice it is given (its
 * rows, its width): never a row past it, whatever the row count, so a band
 * seam or a strip anywhere can't spill into the memory after a DMA buffer.
 */

/*
 * Two times upscaled: out_rows rows of 2 * src_w pixels at out (out_stride
 * bytes apart), output row k from source row k / 2 (y is a row pair's first
 * row; u and v its chroma row). Any out_rows: a pair whose four rows don't
 * all fit writes only those that do. Reads (out_rows + 1) / 2 source rows.
 * y: 16-byte aligned, ystride a multiple of 16; u, v: 8-byte aligned,
 * cstride a multiple of 8; out and out_stride: multiples of 8.
 */
void muse_yuv_up2(const uint8_t *y, const uint8_t *u, const uint8_t *v, int ystride, int cstride, int src_w,
                  int out_rows, uint8_t *out, int out_stride);

/*
 * Any other scale: output rows [row, row + rows) of an out_w x out_h picture
 * made from the src_w x src_h one, luma bilinear and chroma nearest. The
 * caller has the source rows up to muse_yuv_src_rows(...) decoded.
 */
void muse_yuv_scaled(const uint8_t *y, const uint8_t *u, const uint8_t *v, int ystride, int cstride, int src_w,
                     int src_h, int out_w, int out_h, int row, int rows, uint8_t *out, int out_stride);
int muse_yuv_src_rows(int src_h, int out_h, int out_rows);

/*
 * Fill (muse_video_fit's MUSE_VIDEO_FIT_FILL): output rows [row, row + rows) of
 * the src_w x src_h picture scaled by num / den, its window's top left crop_x,
 * crop_y in the scaled picture, nearest pixel: each source row converted once
 * and spread across out_w (at most 512) columns, a repeated row copied. src_w
 * at most MUSE_YUV_FILL_MAX_SRC, even. The caller has source rows up to
 * muse_yuv_fill_src_rows(...) decoded (muse_video_fill_src: which source row
 * an output row takes).
 */
/* The widest source muse_yuv_fill takes: any streamed picture up to the S31's 480, cover-scaled (no black bars). */
#define MUSE_YUV_FILL_MAX_SRC 512
void muse_yuv_fill(const uint8_t *y, const uint8_t *u, const uint8_t *v, int ystride, int cstride, int src_w,
                   int src_h, int num, int den, int crop_x, int crop_y, int out_w, int row, int rows, uint8_t *out,
                   int out_stride);
/*
 * A window of the same fill (changed-only redraw, muse_vdirty.h): output
 * columns [out_x, out_x + w) of rows [row, row + rows), the window's first
 * pieces with first set (the row kept for a repeated source row is another
 * window's otherwise). Pixel for pixel what muse_yuv_fill() writes there.
 */
void muse_yuv_fill_win(const uint8_t *y, const uint8_t *u, const uint8_t *v, int ystride, int cstride, int src_w,
                       int src_h, int num, int den, int crop_x, int crop_y, int out_x, int w, int row, int rows,
                       bool first, uint8_t *out, int out_stride);
/* The same spread for a row already in RGB565. */
void muse_yuv_fill_row565(const uint16_t *src, int src_w, int num, int den, int crop_x, int out_w, uint16_t *out);
int muse_yuv_fill_src_rows(int src_h, int num, int den, int crop_y, int out_rows);
/* CPU cycles muse_yuv_fill() took since the last call (the stats). */
uint32_t muse_yuv_fill_cycles(void);
void muse_yuv_fill_breakdown(uint32_t *conv, uint32_t *exp, uint32_t *copy);

/*
 * One and a half times upscaled (320 px to 480): output rows [row, row + rows)
 * (row even; an odd rows ends on a pair's first row) of a 3/2 src_w square. Each source pixel pair becomes
 * three (the middle one their average), rows likewise, in YUV, then
 * muse_yuv_1x() converts. Needs source rows up to muse_yuv_up32_src_rows();
 * never reads a source row at or past src_h.
 */
void muse_yuv_up32(const uint8_t *y, const uint8_t *u, const uint8_t *v, int ystride, int cstride, int src_w,
                   int src_h, int row, int rows, uint8_t *out, int out_stride);
int muse_yuv_up32_src_rows(int src_h, int out_rows);

/* Not scaled: src_rows rows (an odd count ends on a pair's first row) of src_w (even) pixels, one to one. */
void muse_yuv_1x(const uint8_t *y, const uint8_t *u, const uint8_t *v, int ystride, int cstride, int src_w,
                 int src_rows, uint8_t *out, int out_stride);

/*
 * muse_yuv_1x() for still pictures (UI streams, protocols/UI.md), slower and
 * finer: chroma upsampled bilinearly (centred between each 2x2's luma, as the
 * phone averages it) instead of one colour per 2x2, BT.601 limited range
 * with rounding, and a 4x4 ordered dither into RGB565 so flat areas and
 * gradients don't band. src_h: the picture's rows (for the chroma below the
 * last row); row: the first source row (even) of the rows converted.
 */
void muse_yuv_1x_hq(const uint8_t *y, const uint8_t *u, const uint8_t *v, int ystride, int cstride, int src_w,
                    int src_h, int row, int rows, uint8_t *out, int out_stride);
/* The same for columns x0..x1-1 only (even), written at out + 2 * x of each output row. */
void muse_yuv_1x_hq_cols(const uint8_t *y, const uint8_t *u, const uint8_t *v, int ystride, int cstride, int src_w,
                         int src_h, int row, int rows, int x0, int x1, uint8_t *out, int out_stride);
