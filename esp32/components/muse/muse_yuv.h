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

#include <stdint.h>

/*
 * I420 to RGB565 in the panel's byte order (big-endian, as LVGL's
 * RGB565_SWAPPED), BT.601 limited range, for video on the QSPI AMOLEDs.
 * The S3's SIMD does 16 pixels a step (muse_yuv_pie.S); the last pixels of a
 * row that isn't a multiple of 16 take the C path, with the same arithmetic,
 * so both give identical pixels.
 */

/*
 * Two times upscaled: src_rows source rows (even) of src_w pixels become
 * 2 * src_rows rows of 2 * src_w pixels at out (out_stride bytes apart).
 * y: 16-byte aligned, ystride a multiple of 16; u, v: 8-byte aligned,
 * cstride a multiple of 8; out and out_stride: multiples of 8.
 */
void muse_yuv_up2(const uint8_t *y, const uint8_t *u, const uint8_t *v, int ystride, int cstride, int src_w,
                  int src_rows, uint8_t *out, int out_stride);

/*
 * Any other scale: output rows [row, row + rows) of an out_w x out_h picture
 * made from the src_w x src_h one, luma bilinear and chroma nearest. The
 * caller has the source rows up to muse_yuv_src_rows(...) decoded.
 */
void muse_yuv_scaled(const uint8_t *y, const uint8_t *u, const uint8_t *v, int ystride, int cstride, int src_w,
                     int src_h, int out_w, int out_h, int row, int rows, uint8_t *out, int out_stride);
int muse_yuv_src_rows(int src_h, int out_h, int out_rows);

/*
 * One and a half times upscaled (320 px to 480): output rows [row, row + rows)
 * (row and rows even) of a 3/2 src_w square. Each source pixel pair becomes
 * three (the middle one their average), rows likewise, in YUV, then
 * muse_yuv_1x() converts. Needs source rows up to muse_yuv_up32_src_rows().
 */
void muse_yuv_up32(const uint8_t *y, const uint8_t *u, const uint8_t *v, int ystride, int cstride, int src_w,
                   int src_h, int row, int rows, uint8_t *out, int out_stride);
int muse_yuv_up32_src_rows(int src_h, int out_rows);

/* Not scaled: src_rows (even) rows of src_w (even) pixels, one to one. */
void muse_yuv_1x(const uint8_t *y, const uint8_t *u, const uint8_t *v, int ystride, int cstride, int src_w,
                 int src_rows, uint8_t *out, int out_stride);
