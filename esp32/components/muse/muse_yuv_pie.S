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
// I420 -> big-endian RGB565, two rows x 16 pixels per iteration, ESP32-S3 PIE (128-bit SIMD).
// Math (identical to the C path in yuv2rgb.c):
//   y' = ((Y-16)*149) >> 7, u = U-128, v = V-128
//   R = clamp(y' + (v*204>>7)), G = clamp(y' - ((u*50>>7) + (v*104>>7))), B = clamp(y' + (u*258>>7))
//   out16 (little-endian lane) = (R&0xF8) | ((G&0xE0)>>5) | ((G&0x1C)<<11) | ((B&0xF8)<<5)
//   -> memory bytes: [RRRRRGGG][GGGBBBBB]  (big-endian RGB565 for SPI/QSPI panels)
//
// void muse_yuv_pie_2rows(const u8 *y0, const u8 *y1, const u8 *u, const u8 *v,
//                        u8 *o0, u8 *o1, u32 nchunks, const s16 *k, s16 *scratch)
// y0/y1: 16B aligned; u/v: 8B aligned; o0/o1: 8B aligned; k/scratch: 16B aligned.
// k layout (8 x s16 lanes each): [128,204,50,104,258] [16,149,255,0xF8,0xF8,0xE0,0x1C]
    .section .iram1, "ax"
    .global muse_yuv_pie_2rows
    .type muse_yuv_pie_2rows, @function
    .align 4

    .macro HALF oreg
    mov     a11, a13                 // half constant sequence
    ee.vld.128.ip q7, a11, 16        // 16
    ee.vsubs.s16 q0, q0, q7
    ee.vld.128.ip q7, a11, 16        // 149
    movi    a14, 7
    wsr.sar a14
    ee.vmul.s16 q0, q0, q7           // y'
    ee.zero.q q5
    ee.vld.128.ip q6, a11, 16        // 255
    // R
    ee.vld.128.ip q7, a12, 64        // rv  (a12 -> bu)
    ee.vadds.s16 q2, q0, q7
    ee.vmax.s16 q2, q2, q5
    ee.vmin.s16 q2, q2, q6
    ee.vld.128.ip q7, a11, 16        // 0xF8
    ee.andq q2, q2, q7
    // B
    ee.vld.128.ip q7, a12, -32       // bu  (a12 -> g)
    ee.vadds.s16 q4, q0, q7
    ee.vmax.s16 q4, q4, q5
    ee.vmin.s16 q4, q4, q6
    ee.vld.128.ip q7, a11, 16        // 0xF8
    ee.andq q4, q4, q7
    movi    a14, 5
    wsr.sar a14
    ee.vsl.32 q4, q4
    // G
    ee.vld.128.ip q7, a12, -16       // g   (a12 -> next half rv)
    ee.vsubs.s16 q3, q0, q7
    ee.vmax.s16 q3, q3, q5
    ee.vmin.s16 q3, q3, q6
    ee.vld.128.ip q7, a11, 16        // 0xE0
    ee.andq q1, q3, q7
    ee.vsr.32 q1, q1
    ee.vld.128.ip q7, a11, 16        // 0x1C
    ee.andq q3, q3, q7
    movi    a14, 11
    wsr.sar a14
    ee.vsl.32 q3, q3
    ee.orq  q2, q2, q4
    ee.orq  q2, q2, q1
    ee.orq  q2, q2, q3
    ee.vst.l.64.ip q2, \oreg, 8
    ee.vst.h.64.ip q2, \oreg, 8
    .endm

    .macro ROW yreg, oreg
    ee.vld.128.ip q0, \yreg, 16
    ee.zero.q q1
    ee.vzip.8 q0, q1                 // q0 = Y px0-7, q1 = Y px8-15 (u16 lanes)
    addi    a15, a10, 96
    ee.vst.128.ip q1, a15, 0         // scratch[6] = Y px8-15
    mov     a12, a10                 // scratch walker: rv0
    HALF \oreg
    addi    a15, a10, 96
    ee.vld.128.ip q0, a15, 0
    HALF \oreg
    .endm

    .macro HALF2 oreg
    mov     a11, a13                 // half constant sequence
    ee.vld.128.ip q7, a11, 16        // 16
    ee.vsubs.s16 q0, q0, q7
    ee.vld.128.ip q7, a11, 16        // 149
    movi    a14, 7
    wsr.sar a14
    ee.vmul.s16 q0, q0, q7           // y'
    ee.zero.q q5
    ee.vld.128.ip q6, a11, 16        // 255
    // R
    ee.vld.128.ip q7, a12, 64        // rv  (a12 -> bu)
    ee.vadds.s16 q2, q0, q7
    ee.vmax.s16 q2, q2, q5
    ee.vmin.s16 q2, q2, q6
    ee.vld.128.ip q7, a11, 16        // 0xF8
    ee.andq q2, q2, q7
    // B
    ee.vld.128.ip q7, a12, -32       // bu  (a12 -> g)
    ee.vadds.s16 q4, q0, q7
    ee.vmax.s16 q4, q4, q5
    ee.vmin.s16 q4, q4, q6
    ee.vld.128.ip q7, a11, 16        // 0xF8
    ee.andq q4, q4, q7
    movi    a14, 5
    wsr.sar a14
    ee.vsl.32 q4, q4
    // G
    ee.vld.128.ip q7, a12, -16       // g   (a12 -> next half rv)
    ee.vsubs.s16 q3, q0, q7
    ee.vmax.s16 q3, q3, q5
    ee.vmin.s16 q3, q3, q6
    ee.vld.128.ip q7, a11, 16        // 0xE0
    ee.andq q1, q3, q7
    ee.vsr.32 q1, q1
    ee.vld.128.ip q7, a11, 16        // 0x1C
    ee.andq q3, q3, q7
    movi    a14, 11
    wsr.sar a14
    ee.vsl.32 q3, q3
    ee.orq  q2, q2, q4
    ee.orq  q2, q2, q1
    ee.orq  q2, q2, q3
    ee.orq  q4, q2, q2
    ee.vzip.16 q2, q4                // each pixel twice: q2 = px0-3 x2, q4 = px4-7 x2
    add     a15, \oreg, a9           // next output row (a9 = output stride in bytes)
    ee.vst.l.64.ip q2, \oreg, 8
    ee.vst.h.64.ip q2, \oreg, 8
    ee.vst.l.64.ip q4, \oreg, 8
    ee.vst.h.64.ip q4, \oreg, 8
    ee.vst.l.64.ip q2, a15, 8
    ee.vst.h.64.ip q2, a15, 8
    ee.vst.l.64.ip q4, a15, 8
    ee.vst.h.64.ip q4, a15, 8
    .endm

    .macro ROW2 yreg, oreg
    ee.vld.128.ip q0, \yreg, 16
    ee.zero.q q1
    ee.vzip.8 q0, q1                 // q0 = Y px0-7, q1 = Y px8-15 (u16 lanes)
    addi    a15, a10, 96
    ee.vst.128.ip q1, a15, 0         // scratch[6] = Y px8-15
    mov     a12, a10                 // scratch walker: rv0
    HALF2 \oreg
    addi    a15, a10, 96
    ee.vld.128.ip q0, a15, 0
    HALF2 \oreg
    .endm

muse_yuv_pie_2rows:
    entry   a1, 32
    l32i    a8, a1, 32               // nchunks
    l32i    a9, a1, 36               // k
    l32i    a10, a1, 40              // scratch
    addi    a13, a9, 80              // half-constant sequence
    loopnez a8, .Lend
    // ---- chroma for 16 pixels (8 samples) ----
    ee.vld.l.64.ip q0, a4, 8         // U
    ee.vld.l.64.ip q1, a5, 8         // V
    ee.zero.q q7
    ee.vzip.8 q0, q7
    ee.zero.q q7
    ee.vzip.8 q1, q7
    mov     a11, a9
    ee.vld.128.ip q7, a11, 16        // 128
    ee.vsubs.s16 q0, q0, q7
    ee.vsubs.s16 q1, q1, q7
    movi    a14, 7
    wsr.sar a14
    ee.vld.128.ip q7, a11, 16        // 204
    ee.vmul.s16 q2, q1, q7           // rv
    ee.vld.128.ip q7, a11, 16        // 50
    ee.vmul.s16 q3, q0, q7           // gu
    ee.vld.128.ip q7, a11, 16        // 104
    ee.vmul.s16 q4, q1, q7           // gv
    ee.vadds.s16 q3, q3, q4          // g
    ee.vld.128.ip q7, a11, 16        // 258
    ee.vmul.s16 q4, q0, q7           // bu
    ee.orq  q5, q2, q2
    ee.vzip.16 q2, q5                // rv px0-7, px8-15
    ee.orq  q6, q3, q3
    ee.vzip.16 q3, q6
    ee.orq  q7, q4, q4
    ee.vzip.16 q4, q7
    mov     a15, a10
    ee.vst.128.ip q2, a15, 16        // s0 rv0
    ee.vst.128.ip q5, a15, 16        // s1 rv1
    ee.vst.128.ip q3, a15, 16        // s2 g0
    ee.vst.128.ip q6, a15, 16        // s3 g1
    ee.vst.128.ip q4, a15, 16        // s4 bu0
    ee.vst.128.ip q7, a15, 16        // s5 bu1
    // ---- two luma rows ----
    ROW a2, a6
    ROW a3, a7
.Lend:
    retw.n

    .global muse_yuv_pie_2rows_up2
    .type muse_yuv_pie_2rows_up2, @function
    .align 4
muse_yuv_pie_2rows_up2:
    entry   a1, 32
    l32i    a8, a1, 32               // nchunks
    l32i    a9, a1, 36               // k
    l32i    a10, a1, 40              // scratch
    addi    a13, a9, 80              // half-constant sequence
    l32i    a9, a1, 44               // output row stride (bytes)
    addi    a8, a8, 0
    loopnez a8, .Lend2
    // ---- chroma for 16 pixels (8 samples) ----
    ee.vld.l.64.ip q0, a4, 8         // U
    ee.vld.l.64.ip q1, a5, 8         // V
    ee.zero.q q7
    ee.vzip.8 q0, q7
    ee.zero.q q7
    ee.vzip.8 q1, q7
    addi    a11, a13, -80
    ee.vld.128.ip q7, a11, 16        // 128
    ee.vsubs.s16 q0, q0, q7
    ee.vsubs.s16 q1, q1, q7
    movi    a14, 7
    wsr.sar a14
    ee.vld.128.ip q7, a11, 16        // 204
    ee.vmul.s16 q2, q1, q7           // rv
    ee.vld.128.ip q7, a11, 16        // 50
    ee.vmul.s16 q3, q0, q7           // gu
    ee.vld.128.ip q7, a11, 16        // 104
    ee.vmul.s16 q4, q1, q7           // gv
    ee.vadds.s16 q3, q3, q4          // g
    ee.vld.128.ip q7, a11, 16        // 258
    ee.vmul.s16 q4, q0, q7           // bu
    ee.orq  q5, q2, q2
    ee.vzip.16 q2, q5                // rv px0-7, px8-15
    ee.orq  q6, q3, q3
    ee.vzip.16 q3, q6
    ee.orq  q7, q4, q4
    ee.vzip.16 q4, q7
    mov     a15, a10
    ee.vst.128.ip q2, a15, 16        // s0 rv0
    ee.vst.128.ip q5, a15, 16        // s1 rv1
    ee.vst.128.ip q3, a15, 16        // s2 g0
    ee.vst.128.ip q6, a15, 16        // s3 g1
    ee.vst.128.ip q4, a15, 16        // s4 bu0
    ee.vst.128.ip q7, a15, 16        // s5 bu1
    // ---- two luma rows ----
    ROW2 a2, a6
    ROW2 a3, a7
.Lend2:
    retw.n
