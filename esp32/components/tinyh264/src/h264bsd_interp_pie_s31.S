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
// Luma 6-tap interpolation (h264bsdInterpolate{Ver,Hor}{Half,Quarter}) on
// the ESP32-S31's SIMD (PIE), 8 or 16 pixels a row. The pixels go in 16-bit
// lanes split by parity (even pixels in one vector, odd in the other), so
// every step is lane-wise and the result packs back as even | odd << 8:
//   r = clip((A + F - 5(B + E) + 20(C + D) + 16) >> 5), then for quarter
//   positions (r + I + 1) >> 1 with the integer sample I.
// x20 and x5 are 32-bit shifts of lanes that can't carry into each other
// (all values there are non-negative and under 2^14). The same arithmetic as
// the C, so the same pixels; QEMU runs none of this, so h264bsd_reconstruct.c
// checks it against the C on first use and keeps the C if they differ
// (tests/tinyh264_pie_harness.c is the lane-op model of this file, checked
// against the C on the host). Chroma (h264bsdPieChroma, below) likewise.
//
// void h264bsdPieVer(const u8 *src, u32 stride, u8 *mb, u32 w, u32 h,
//                    i32 off, u8 *scratch, const s16 *k)
// void h264bsdPieHor(... same ...)
//   src: tap row (column) 0 of output pixel 0; mb: 16 bytes a row, 8-byte
//   aligned; off: -1 half, 0/1 quarter (the integer sample's offset);
//   scratch: 16-byte aligned, (h + 5) * 32 bytes (Ver) or 128 (Hor);
//   k: 16-byte aligned { 0x00ff x8, 16 x8, 1 x8, 255 x8 }.
// PIE addresses must be x8-x15: a0 src, a2 mb, a3 the integer row, s0 the
// tap walker, s1 the constants or the scratch walker. t0 w, t1 h, t2 off,
// t3 scratch, t4 shift, t5 k, t6 loop.
    .section .iram1.h264bsd_pie, "ax"

    .macro SAR n
    li      t4, \n
    esp.movx.w.sar t4
    .endm

    // q0 = clip(six taps) of the six vectors at s0, s0+step, ...;
    // q6 = 1 afterwards. Uses q0-q6 and s1.
    .macro SIX step
    esp.vld.128.ip q0, s0, \step      // A
    esp.vld.128.ip q1, s0, \step      // B
    esp.vld.128.ip q2, s0, \step      // C
    esp.vld.128.ip q3, s0, \step      // D
    esp.vld.128.ip q4, s0, \step      // E
    esp.vld.128.ip q5, s0, \step      // F
    esp.vadd.s16 q2, q2, q3           // C + D
    esp.vadd.s16 q0, q0, q5           // A + F
    esp.vadd.s16 q1, q1, q4           // B + E
    SAR 4
    esp.vsl.32 q3, q2                 // 16(C + D)
    SAR 2
    esp.vsl.32 q2, q2                 // 4(C + D)
    esp.vsl.32 q4, q1                 // 4(B + E)
    esp.vadd.s16 q0, q0, q3
    esp.vadd.s16 q0, q0, q2
    esp.vadd.s16 q1, q1, q4           // 5(B + E)
    esp.vsub.s16 q0, q0, q1
    addi    s1, t5, 16
    esp.vld.128.ip q5, s1, 16         // 16
    esp.vld.128.ip q6, s1, 16         // 1
    esp.vadd.s16 q0, q0, q5
    SAR 5
    esp.vmul.s16 q0, q0, q6           // >> 5
    esp.zero.q q5
    esp.vmax.s16 q0, q0, q5
    esp.vld.128.ip q5, s1, 0          // 255
    esp.vmin.s16 q0, q0, q5
    .endm

    // Quarter positions: q0 = (q0 + I + 1) >> 1 with I at a3 (q6 = 1).
    .macro AVG
    esp.vld.128.ip q5, a3, 0
    esp.vadd.s16 q0, q0, q5
    esp.vadd.s16 q0, q0, q6
    SAR 1
    esp.vmul.s16 q0, q0, q6
    .endm

    // q0 = 16 bytes at a0 (any alignment); a0 unchanged. Uses q1.
    .macro LDU
    esp.ld.128.usar.ip q0, a0, 16
    esp.ld.128.usar.ip q1, a0, -16
    esp.src.q q0, q0, q1
    .endm

    // Chroma: q0 = (wA*A + wB*B + wC*C + wD*D + 32) >> 6 of the vectors at
    // s0, s0+32, s0+64, s0+96, weights at a5. Uses q0-q5 and s1.
    .macro BILIN
    esp.vld.128.ip q0, s0, 32         // A
    esp.vld.128.ip q1, s0, 32         // B
    esp.vld.128.ip q2, s0, 32         // C (row r+1)
    esp.vld.128.ip q3, s0, 32         // D
    mv      s1, a5
    SAR 0
    esp.vld.128.ip q4, s1, 16
    esp.vmul.s16 q0, q0, q4
    esp.vld.128.ip q4, s1, 16
    esp.vmul.s16 q1, q1, q4
    esp.vld.128.ip q4, s1, 16
    esp.vmul.s16 q2, q2, q4
    esp.vld.128.ip q4, s1, 16
    esp.vmul.s16 q3, q3, q4
    esp.vadd.s16 q0, q0, q1
    esp.vadd.s16 q2, q2, q3
    esp.vadd.s16 q0, q0, q2
    esp.vld.128.ip q4, s1, 16         // 32
    esp.vadd.s16 q0, q0, q4
    esp.vld.128.ip q5, s1, 16         // 1
    SAR 6
    esp.vmul.s16 q0, q0, q5
    .endm

    // q0 even, q7 odd -> (even | odd << 8) stored at a2 (8 or 16 bytes,
    // a2 += 16).
    .macro STORE
    SAR 8
    esp.vsl.32 q7, q7
    esp.orq q0, q0, q7
    esp.vst.l.64.ip q0, a2, 8
    li      t4, 16
    bne     t0, t4, 1f
    esp.vst.h.64.ip q0, a2, 8
    j       2f
1:  addi    a2, a2, 8
2:
    .endm

    .macro PROLOGUE
    addi    sp, sp, -16
    sw      s0, 0(sp)
    sw      s1, 4(sp)
    mv      t0, a3
    mv      t1, a4
    mv      t2, a5
    mv      t3, a6
    mv      t5, a7
    .endm

    .macro EPILOGUE
    lw      s0, 0(sp)
    lw      s1, 4(sp)
    addi    sp, sp, 16
    ret
    .endm

    .global h264bsdPieVer
    .type h264bsdPieVer, @function
    .align 4
h264bsdPieVer:
    PROLOGUE
    // Rows 0 .. h+4 into the scratch, each as even and odd lanes.
    mv      s1, t5
    esp.vld.128.ip q7, s1, 0          // 0x00ff
    mv      s1, t3
    addi    t6, t1, 5
    SAR 8
.Lver_split:
    LDU
    esp.andq q2, q0, q7
    SAR 8                             // (the unaligned loads set only SAR_BYTE; be sure)
    esp.vsr.u32 q0, q0
    esp.andq q0, q0, q7
    esp.vst.128.ip q2, s1, 16
    esp.vst.128.ip q0, s1, 16
    add     a0, a0, a1
    addi    t6, t6, -1
    bnez    t6, .Lver_split
    // Output rows: taps at scratch + (r + k) * 32 (+ 16 for odd), the
    // integer sample at row r + 2 + off.
    mv      t6, t1
.Lver_row:
    mv      s0, t3
    SIX 32
    bltz    t2, 3f
    slli    a3, t2, 5
    add     a3, a3, t3
    addi    a3, a3, 64
    AVG
3:  esp.orq q7, q0, q0                // even result
    addi    s0, t3, 16
    SIX 32
    bltz    t2, 4f
    slli    a3, t2, 5
    add     a3, a3, t3
    addi    a3, a3, 80
    AVG
4:  esp.orq q1, q0, q0                // odd result
    esp.orq q0, q7, q7
    esp.orq q7, q1, q1
    STORE
    addi    t3, t3, 32
    addi    t6, t6, -1
    bnez    t6, .Lver_row
    EPILOGUE
    .size h264bsdPieVer, . - h264bsdPieVer

    .global h264bsdPieHor
    .type h264bsdPieHor, @function
    .align 4
h264bsdPieHor:
    PROLOGUE
    mv      t6, t1
.Lhor_row:
    // Columns 0, 2, 4 and 6 onwards, each as even and odd lanes:
    // E0 O0 E2 O2 E4 O4 E6 O6 at scratch + 0, 16, ... 112.
    mv      s1, t5
    esp.vld.128.ip q7, s1, 0          // 0x00ff
    mv      s1, t3
    SAR 8
    li      a3, 4
.Lhor_split:
    LDU
    esp.andq q2, q0, q7
    SAR 8                             // (the unaligned loads set only SAR_BYTE; be sure)
    esp.vsr.u32 q0, q0
    esp.andq q0, q0, q7
    esp.vst.128.ip q2, s1, 16
    esp.vst.128.ip q0, s1, 16
    addi    a0, a0, 2
    addi    a3, a3, -1
    bnez    a3, .Lhor_split
    addi    a0, a0, -8
    add     a0, a0, a1
    // Even pixels: E0 O0 E2 O2 E4 O4; the integer sample is E2 (off 0) or O2.
    mv      s0, t3
    SIX 16
    bltz    t2, 3f
    slli    a3, t2, 4
    add     a3, a3, t3
    addi    a3, a3, 32
    AVG
3:  esp.orq q7, q0, q0
    // Odd pixels: O0 E2 O2 E4 O4 E6; the integer sample is O2 (off 0) or E4.
    addi    s0, t3, 16
    SIX 16
    bltz    t2, 4f
    slli    a3, t2, 4
    add     a3, a3, t3
    addi    a3, a3, 48
    AVG
4:  esp.orq q1, q0, q0
    esp.orq q0, q7, q7
    esp.orq q7, q1, q1
    STORE
    addi    t6, t6, -1
    bnez    t6, .Lhor_row
    EPILOGUE
    .size h264bsdPieHor, . - h264bsdPieHor

// void h264bsdPieChroma(const u8 *src, u32 stride, u8 *dst, u32 h, u32 rows,
//                       const s16 *wt, u8 *scratch, const s16 *k)
// One chroma component, 8 pixels a row (dst 8 bytes a row, 8-byte aligned):
//   r = (wA*A + wB*B + wC*C + wD*D + 32) >> 6, A/B at columns x/x+1 of row
//   r, C/D of row r+1 (the C's Hor, Ver and HorVer bilinear weights).
// wt: 16-byte aligned { wA x8, wB x8, wC x8, wD x8, 32 x8, 1 x8 }; rows: the
// rows read (h, or h + 1 when wC/wD are used); scratch: 16-byte aligned,
// (h + 1) * 64 bytes.
    .global h264bsdPieChroma
    .type h264bsdPieChroma, @function
    .align 4
h264bsdPieChroma:
    addi    sp, sp, -16
    sw      s0, 0(sp)
    sw      s1, 4(sp)
    mv      t1, a3
    mv      t3, a6
    mv      s1, a7
    esp.vld.128.ip q7, s1, 0          // 0x00ff
    // Rows into the scratch: columns x and x+1, each as even and odd lanes.
    mv      s1, t3
    mv      t6, a4
.Lchr_split:
    LDU
    esp.andq q2, q0, q7
    SAR 8
    esp.vsr.u32 q0, q0
    esp.andq q0, q0, q7
    esp.vst.128.ip q2, s1, 16
    esp.vst.128.ip q0, s1, 16
    addi    a0, a0, 1
    LDU
    esp.andq q2, q0, q7
    SAR 8
    esp.vsr.u32 q0, q0
    esp.andq q0, q0, q7
    esp.vst.128.ip q2, s1, 16
    esp.vst.128.ip q0, s1, 16
    addi    a0, a0, -1
    add     a0, a0, a1
    addi    t6, t6, -1
    bnez    t6, .Lchr_split
    mv      t6, t1
.Lchr_row:
    mv      s0, t3
    BILIN
    esp.orq q7, q0, q0                // even
    addi    s0, t3, 16
    BILIN
    SAR 8
    esp.vsl.32 q0, q0
    esp.orq q0, q0, q7
    esp.vst.l.64.ip q0, a2, 8
    addi    t3, t3, 64
    addi    t6, t6, -1
    bnez    t6, .Lchr_row
    lw      s0, 0(sp)
    lw      s1, 4(sp)
    addi    sp, sp, 16
    ret
    .size h264bsdPieChroma, . - h264bsdPieChroma
