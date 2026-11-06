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
// VP8 bilinear prediction, one 2-tap pass, on the ESP32-S3's PIE (128-bit SIMD).
// Muse gadget port (vp8/common/filter.c calls it; bit-exact with the C):
//   dst[r][c] = (src[r][c] * w0 + src[r][c + step] * w1 + 64) >> 7
// with step 1 (horizontal) or the source stride (vertical), in 16-bit lanes:
// every product and sum fits (255 * 128 + 64 < 32768).
//
// void vp8_mg_bil16_pie(const u8 *src, int src_stride, u8 *dst, int dst_stride,
//                       int rows, const s16 *tab, int step)
// void vp8_mg_bil8_pie (same; 8 pixels a row)
// src: any alignment (the rows are read 16 bytes at a time from their aligned
// blocks and shifted into place). dst: 16-byte aligned (8-byte for bil8).
// tab: 16-byte aligned, four vectors of 8 x s16: w0, w1, 64, 1.
    .section .iram1, "ax"

    .macro LOADROW qa, qb, ptr, tmp
    mov     \tmp, \ptr
    ee.ld.128.usar.ip \qa, \tmp, 16      // the aligned block holding ptr; SAR_BYTE = ptr & 15
    ee.vld.128.ip \qb, \tmp, 0           // the next one
    ee.src.q \qa, \qa, \qb               // 16 bytes from ptr
    .endm

    .global vp8_mg_bil16_pie
    .type vp8_mg_bil16_pie, @function
    .align 4
vp8_mg_bil16_pie:
    entry   a1, 32
    l32i    a10, a1, 32                  // step
    mov     a11, a7
    ee.vld.128.ip q6, a11, 16            // w0
    ee.vld.128.ip q7, a11, 16            // w1 (a11 -> 64, 1)
    movi    a12, 0
    movi    a13, 7
    loopnez a6, .L16end
    LOADROW q0, q1, a2, a8               // a = src
    add     a9, a2, a10
    LOADROW q2, q3, a9, a8               // b = src + step
    ee.zero.q q1
    ee.vzip.8 q0, q1                     // a: q0 = px 0-7, q1 = px 8-15 (u16)
    ee.zero.q q3
    ee.vzip.8 q2, q3                     // b
    wsr.sar a12
    ee.vmul.s16 q0, q0, q6
    ee.vmul.s16 q1, q1, q6
    ee.vmul.s16 q2, q2, q7
    ee.vmul.s16 q3, q3, q7
    ee.vadds.s16 q0, q0, q2
    ee.vadds.s16 q1, q1, q3
    mov     a8, a11
    ee.vld.128.ip q4, a8, 16             // 64
    ee.vadds.s16 q0, q0, q4
    ee.vadds.s16 q1, q1, q4
    ee.vld.128.ip q4, a8, 0              // 1
    wsr.sar a13
    ee.vmul.s16 q0, q0, q4               // >> 7
    ee.vmul.s16 q1, q1, q4
    ee.vunzip.8 q0, q1                   // q0 = the 16 bytes
    mov     a8, a4
    ee.vst.128.ip q0, a8, 0
    add     a2, a2, a3
    add     a4, a4, a5
.L16end:
    retw.n

    .global vp8_mg_bil8_pie
    .type vp8_mg_bil8_pie, @function
    .align 4
vp8_mg_bil8_pie:
    entry   a1, 32
    l32i    a10, a1, 32                  // step
    mov     a11, a7
    ee.vld.128.ip q6, a11, 16            // w0
    ee.vld.128.ip q7, a11, 16            // w1 (a11 -> 64, 1)
    movi    a12, 0
    movi    a13, 7
    loopnez a6, .L8end
    LOADROW q0, q1, a2, a8
    add     a9, a2, a10
    LOADROW q2, q3, a9, a8
    ee.zero.q q1
    ee.vzip.8 q0, q1                     // a: q0 = px 0-7 (u16)
    ee.zero.q q3
    ee.vzip.8 q2, q3                     // b: q2 = px 0-7
    wsr.sar a12
    ee.vmul.s16 q0, q0, q6
    ee.vmul.s16 q2, q2, q7
    ee.vadds.s16 q0, q0, q2
    mov     a8, a11
    ee.vld.128.ip q4, a8, 16             // 64
    ee.vadds.s16 q0, q0, q4
    ee.vld.128.ip q4, a8, 0              // 1
    wsr.sar a13
    ee.vmul.s16 q0, q0, q4               // >> 7
    ee.zero.q q1
    ee.vunzip.8 q0, q1                   // q0 low half = the 8 bytes
    mov     a8, a4
    ee.vst.l.64.ip q0, a8, 0
    add     a2, a2, a3
    add     a4, a4, a5
.L8end:
    retw.n
