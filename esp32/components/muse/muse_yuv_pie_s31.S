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
// The ESP32-S3 kernels of muse_yuv_pie.S on the ESP32-S31's SIMD (PIE, the
// esp.* instructions of the P4's PIE): the same instructions one for one, the
// same arithmetic, so the same pixels as the C path. QEMU runs none of these,
// so muse_yuv.c checks them against the C path on first use and falls back
// to C if they differ.
//
// void muse_yuv_pie_2rows(const u8 *y0, const u8 *y1, const u8 *u, const u8 *v,
//                        u8 *o0, u8 *o1, u32 nchunks, const s16 *k, s16 *scratch)
// void muse_yuv_pie_2rows_up2(... same ..., s16 *scratch, u32 ostride)
// RISC-V calling convention: y0..k in a0..a7, scratch (and ostride) on the
// stack. PIE loads and stores take only x8-x15 as the address: a0-a5 keep
// the arguments, s0 walks the constants (and is the spare pointer), s1 the
// scratch. t0 is the scratch base, t3 the half constants, t4 the shift, t6
// the output stride.
    .section .iram1.muse_yuv_pie, "ax"
    .global muse_yuv_pie_2rows
    .type muse_yuv_pie_2rows, @function
    .align 4

    .macro HALF oreg
    mv      s0, t3                 // half constant sequence
    esp.vld.128.ip q7, s0, 16        // 16
    esp.vsub.s16 q0, q0, q7
    esp.vld.128.ip q7, s0, 16        // 149
    li      t4, 7
    esp.movx.w.sar t4
    esp.vmul.s16 q0, q0, q7           // y'
    esp.zero.q q5
    esp.vld.128.ip q6, s0, 16        // 255
    // R
    esp.vld.128.ip q7, s1, 64        // rv  (s1 -> bu)
    esp.vadd.s16 q2, q0, q7
    esp.vmax.s16 q2, q2, q5
    esp.vmin.s16 q2, q2, q6
    esp.vld.128.ip q7, s0, 16        // 0xF8
    esp.andq q2, q2, q7
    // B
    esp.vld.128.ip q7, s1, -32       // bu  (s1 -> g)
    esp.vadd.s16 q4, q0, q7
    esp.vmax.s16 q4, q4, q5
    esp.vmin.s16 q4, q4, q6
    esp.vld.128.ip q7, s0, 16        // 0xF8
    esp.andq q4, q4, q7
    li      t4, 5
    esp.movx.w.sar t4
    esp.vsl.32 q4, q4
    // G
    esp.vld.128.ip q7, s1, -16       // g   (s1 -> next half rv)
    esp.vsub.s16 q3, q0, q7
    esp.vmax.s16 q3, q3, q5
    esp.vmin.s16 q3, q3, q6
    esp.vld.128.ip q7, s0, 16        // 0xE0
    esp.andq q1, q3, q7
    esp.vsr.u32 q1, q1
    esp.vld.128.ip q7, s0, 16        // 0x1C
    esp.andq q3, q3, q7
    li      t4, 11
    esp.movx.w.sar t4
    esp.vsl.32 q3, q3
    esp.orq  q2, q2, q4
    esp.orq  q2, q2, q1
    esp.orq  q2, q2, q3
    esp.vst.l.64.ip q2, \oreg, 8
    esp.vst.h.64.ip q2, \oreg, 8
    .endm

    .macro ROW yreg, oreg
    esp.vld.128.ip q0, \yreg, 16
    esp.zero.q q1
    esp.vzip.8 q0, q1                 // q0 = Y px0-7, q1 = Y px8-15 (u16 lanes)
    addi    s0, t0, 96
    esp.vst.128.ip q1, s0, 0         // scratch[6] = Y px8-15
    mv      s1, t0                 // scratch walker: rv0
    HALF \oreg
    addi    s0, t0, 96
    esp.vld.128.ip q0, s0, 0
    HALF \oreg
    .endm

    .macro HALF2 oreg
    mv      s0, t3                 // half constant sequence
    esp.vld.128.ip q7, s0, 16        // 16
    esp.vsub.s16 q0, q0, q7
    esp.vld.128.ip q7, s0, 16        // 149
    li      t4, 7
    esp.movx.w.sar t4
    esp.vmul.s16 q0, q0, q7           // y'
    esp.zero.q q5
    esp.vld.128.ip q6, s0, 16        // 255
    // R
    esp.vld.128.ip q7, s1, 64        // rv  (s1 -> bu)
    esp.vadd.s16 q2, q0, q7
    esp.vmax.s16 q2, q2, q5
    esp.vmin.s16 q2, q2, q6
    esp.vld.128.ip q7, s0, 16        // 0xF8
    esp.andq q2, q2, q7
    // B
    esp.vld.128.ip q7, s1, -32       // bu  (s1 -> g)
    esp.vadd.s16 q4, q0, q7
    esp.vmax.s16 q4, q4, q5
    esp.vmin.s16 q4, q4, q6
    esp.vld.128.ip q7, s0, 16        // 0xF8
    esp.andq q4, q4, q7
    li      t4, 5
    esp.movx.w.sar t4
    esp.vsl.32 q4, q4
    // G
    esp.vld.128.ip q7, s1, -16       // g   (s1 -> next half rv)
    esp.vsub.s16 q3, q0, q7
    esp.vmax.s16 q3, q3, q5
    esp.vmin.s16 q3, q3, q6
    esp.vld.128.ip q7, s0, 16        // 0xE0
    esp.andq q1, q3, q7
    esp.vsr.u32 q1, q1
    esp.vld.128.ip q7, s0, 16        // 0x1C
    esp.andq q3, q3, q7
    li      t4, 11
    esp.movx.w.sar t4
    esp.vsl.32 q3, q3
    esp.orq  q2, q2, q4
    esp.orq  q2, q2, q1
    esp.orq  q2, q2, q3
    esp.orq  q4, q2, q2
    esp.vzip.16 q2, q4                // each pixel twice: q2 = px0-3 x2, q4 = px4-7 x2
    add     s0, \oreg, t6           // next output row (t6 = output stride in bytes)
    esp.vst.l.64.ip q2, \oreg, 8
    esp.vst.h.64.ip q2, \oreg, 8
    esp.vst.l.64.ip q4, \oreg, 8
    esp.vst.h.64.ip q4, \oreg, 8
    esp.vst.l.64.ip q2, s0, 8
    esp.vst.h.64.ip q2, s0, 8
    esp.vst.l.64.ip q4, s0, 8
    esp.vst.h.64.ip q4, s0, 8
    .endm

    .macro ROW2 yreg, oreg
    esp.vld.128.ip q0, \yreg, 16
    esp.zero.q q1
    esp.vzip.8 q0, q1                 // q0 = Y px0-7, q1 = Y px8-15 (u16 lanes)
    addi    s0, t0, 96
    esp.vst.128.ip q1, s0, 0         // scratch[6] = Y px8-15
    mv      s1, t0                 // scratch walker: rv0
    HALF2 \oreg
    addi    s0, t0, 96
    esp.vld.128.ip q0, s0, 0
    HALF2 \oreg
    .endm

muse_yuv_pie_2rows:
    addi    sp, sp, -16
    sw      s0, 8(sp)
    sw      s1, 12(sp)
    lw      t0, 16(sp)               // scratch
    addi    t3, a7, 80               // half-constant sequence
    beqz    a6, .Lend
.Lloop:
    // ---- chroma for 16 pixels (8 samples) ----
    esp.vld.l.64.ip q0, a2, 8         // U
    esp.vld.l.64.ip q1, a3, 8         // V
    esp.zero.q q7
    esp.vzip.8 q0, q7
    esp.zero.q q7
    esp.vzip.8 q1, q7
    mv      s0, a7
    esp.vld.128.ip q7, s0, 16        // 128
    esp.vsub.s16 q0, q0, q7
    esp.vsub.s16 q1, q1, q7
    li      t4, 7
    esp.movx.w.sar t4
    esp.vld.128.ip q7, s0, 16        // 204
    esp.vmul.s16 q2, q1, q7           // rv
    esp.vld.128.ip q7, s0, 16        // 50
    esp.vmul.s16 q3, q0, q7           // gu
    esp.vld.128.ip q7, s0, 16        // 104
    esp.vmul.s16 q4, q1, q7           // gv
    esp.vadd.s16 q3, q3, q4          // g
    esp.vld.128.ip q7, s0, 16        // 258
    esp.vmul.s16 q4, q0, q7           // bu
    esp.orq  q5, q2, q2
    esp.vzip.16 q2, q5                // rv px0-7, px8-15
    esp.orq  q6, q3, q3
    esp.vzip.16 q3, q6
    esp.orq  q7, q4, q4
    esp.vzip.16 q4, q7
    mv      s0, t0
    esp.vst.128.ip q2, s0, 16        // s0 rv0
    esp.vst.128.ip q5, s0, 16        // s1 rv1
    esp.vst.128.ip q3, s0, 16        // s2 g0
    esp.vst.128.ip q6, s0, 16        // s3 g1
    esp.vst.128.ip q4, s0, 16        // s4 bu0
    esp.vst.128.ip q7, s0, 16        // s5 bu1
    // ---- two luma rows ----
    ROW a0, a4
    ROW a1, a5
    addi    a6, a6, -1
    bnez    a6, .Lloop
.Lend:
    lw      s0, 8(sp)
    lw      s1, 12(sp)
    addi    sp, sp, 16
    ret

    .global muse_yuv_pie_2rows_up2
    .type muse_yuv_pie_2rows_up2, @function
    .align 4
muse_yuv_pie_2rows_up2:
    addi    sp, sp, -16
    sw      s0, 8(sp)
    sw      s1, 12(sp)
    lw      t0, 16(sp)               // scratch
    addi    t3, a7, 80               // half-constant sequence
    lw      t6, 20(sp)               // output row stride (bytes)
    beqz    a6, .Lend2
.Lloop2:
    // ---- chroma for 16 pixels (8 samples) ----
    esp.vld.l.64.ip q0, a2, 8         // U
    esp.vld.l.64.ip q1, a3, 8         // V
    esp.zero.q q7
    esp.vzip.8 q0, q7
    esp.zero.q q7
    esp.vzip.8 q1, q7
    addi    s0, t3, -80
    esp.vld.128.ip q7, s0, 16        // 128
    esp.vsub.s16 q0, q0, q7
    esp.vsub.s16 q1, q1, q7
    li      t4, 7
    esp.movx.w.sar t4
    esp.vld.128.ip q7, s0, 16        // 204
    esp.vmul.s16 q2, q1, q7           // rv
    esp.vld.128.ip q7, s0, 16        // 50
    esp.vmul.s16 q3, q0, q7           // gu
    esp.vld.128.ip q7, s0, 16        // 104
    esp.vmul.s16 q4, q1, q7           // gv
    esp.vadd.s16 q3, q3, q4          // g
    esp.vld.128.ip q7, s0, 16        // 258
    esp.vmul.s16 q4, q0, q7           // bu
    esp.orq  q5, q2, q2
    esp.vzip.16 q2, q5                // rv px0-7, px8-15
    esp.orq  q6, q3, q3
    esp.vzip.16 q3, q6
    esp.orq  q7, q4, q4
    esp.vzip.16 q4, q7
    mv      s0, t0
    esp.vst.128.ip q2, s0, 16        // s0 rv0
    esp.vst.128.ip q5, s0, 16        // s1 rv1
    esp.vst.128.ip q3, s0, 16        // s2 g0
    esp.vst.128.ip q6, s0, 16        // s3 g1
    esp.vst.128.ip q4, s0, 16        // s4 bu0
    esp.vst.128.ip q7, s0, 16        // s5 bu1
    // ---- two luma rows ----
    ROW2 a0, a4
    ROW2 a1, a5
    addi    a6, a6, -1
    bnez    a6, .Lloop2
.Lend2:
    lw      s0, 8(sp)
    lw      s1, 12(sp)
    addi    sp, sp, 16
    ret
