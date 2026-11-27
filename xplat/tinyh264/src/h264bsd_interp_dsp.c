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

/*
 * OPT: the luma 6-tap and chroma bilinear interpolation of
 * h264bsd_reconstruct.c on the Armv8-M DSP extension (TINYH264_DSP_MC; also
 * Armv7E-M), behind the same hooks as the ESP32-S31's SIMD:
 *   h264bsdDspHor    positions a, b, c (off 0, -1, 1) for partitions 4, 8 or
 *                    16 pixels wide: src at the block's row 0, column -2;
 *   h264bsdDspVer    positions d, h, n (off 0, -1, 1): src at row -2;
 *   h264bsdDspChroma both chroma bilinear cases, 4 or 8 pixels wide.
 * Output rows 16 bytes apart (chroma: 8), as the C writes them.
 *
 * Two pixels to a register in 16-bit lanes (even columns in one, odd in the
 * other: UXTB16), the 6-tap sum on both lanes at once: biased by 81 * 32 so
 * neither lane goes negative (no borrow between lanes), then >> 5, the bias
 * off with unsigned saturation (the clip at 0, UQSUB16) and USAT16 (the clip
 * at 255): bit for bit the C's clp[(sum + 16) >> 5]. A quarter position
 * averages with the integer pixel rounding up, four bytes at once.
 *
 * Without the extension (a host build) the four instructions are plain C with
 * the same results, so the host's bit-exact stream tests check the arithmetic
 * and a Cortex-M33 run checks the instructions. Word loads stay within the
 * taps (and the chroma block's rows and columns): nothing past them is read.
 */

#include "basetype.h"

#if defined(TINYH264_DSP_MC) && TINYH264_DSP_MC

#include <string.h>

#if defined(__ARM_FEATURE_DSP) && __ARM_FEATURE_DSP
static inline u32 uxtb16(u32 x)
{
    u32 r;
    __asm__("uxtb16 %0, %1" : "=r"(r) : "r"(x));
    return r;
}
static inline u32 uxtb16_r8(u32 x)
{
    u32 r;
    __asm__("uxtb16 %0, %1, ror #8" : "=r"(r) : "r"(x));
    return r;
}
static inline u32 uqsub16(u32 a, u32 b)
{
    u32 r;
    __asm__("uqsub16 %0, %1, %2" : "=r"(r) : "r"(a), "r"(b));
    return r;
}
static inline u32 usat16_8(u32 x)
{
    u32 r;
    __asm__("usat16 %0, #8, %1" : "=r"(r) : "r"(x));
    return r;
}
#else
static inline u32 uxtb16(u32 x)
{
    return x & 0x00ff00ffu;
}
static inline u32 uxtb16_r8(u32 x)
{
    return (x >> 8) & 0x00ff00ffu;
}
static inline u32 uqsub16(u32 a, u32 b)
{
    u32 lo = (a & 0xffff) > (b & 0xffff) ? (a & 0xffff) - (b & 0xffff) : 0;
    u32 hi = (a >> 16) > (b >> 16) ? (a >> 16) - (b >> 16) : 0;
    return lo | hi << 16;
}
static inline u32 usat16_8(u32 x)   /* lanes are never negative here */
{
    u32 lo = x & 0xffff, hi = x >> 16;
    return (lo > 255 ? 255 : lo) | (hi > 255 ? 255 : hi) << 16;
}
#endif

#if defined(__GNUC__)
/* A constant the compiler keeps in a register (not rebuilt in two adds). */
#define KREG(v) ({ u32 k_ = (v); __asm__("" : "+r"(k_)); k_; })
#else
#define KREG(v) (v)
#endif

static inline u32 ld32(const u8 *p)
{
    u32 w;
    memcpy(&w, p, 4);   /* an unaligned word load where the core allows it */
    return w;
}

static inline void st32(u8 *p, u32 w)
{
    memcpy(p, &w, 4);
}

/* Bytes averaged rounding up, four at once: (a + b + 1) >> 1. */
static inline u32 avg_up(u32 a, u32 b)
{
    return (a | b) - (((a ^ b) >> 1) & 0x7f7f7f7fu);
}

/* Both lanes: clip((t0 + t5) - 5 (t1 + t4) + 20 (t2 + t3) + 16) >> 5).
 * kb = 16 + 81 * 32 a lane, k81 = 81 a lane (registers). */
static inline u32 six(u32 t0, u32 t1, u32 t2, u32 t3, u32 t4, u32 t5, u32 kb, u32 k81)
{
    u32 t = t0 + t5 + kb;
    t += (t2 + t3) * 20;              /* lanes stay under 2^16 */
    t -= (t1 + t4) * 5;               /* and above 0 */
    t = (t >> 5) & 0x07ff07ffu;
    return usat16_8(uqsub16(t, k81));
}

/* One row of the horizontal 6-tap, 8 pixels a step (4 for w == 4), OFF
 * -1 half, 0/1 quarter: a constant in each instance. */
#define HOR_ROW(OFF) \
    do { \
        u32 x = 0; \
        for (; x + 8 <= w; x += 8) { \
            const u8 *s = src + x; \
            u32 w0 = ld32(s), w2 = ld32(s + 2), w4 = ld32(s + 4), w6 = ld32(s + 6); \
            u32 w8 = ld32(s + 8); \
            u32 e0 = uxtb16(w0), o1 = uxtb16_r8(w0), e2 = uxtb16(w2), o3 = uxtb16_r8(w2); \
            u32 e4 = uxtb16(w4), o5 = uxtb16_r8(w4), e6 = uxtb16(w6), o7 = uxtb16_r8(w6); \
            u32 e8 = uxtb16(w8), o9 = uxtb16_r8(w8), e10 = uxtb16_r8(ld32(s + 9)); /* 10, 12 */ \
            u32 r0 = six(e0, o1, e2, o3, e4, o5, kb, k81) | six(o1, e2, o3, e4, o5, e6, kb, k81) << 8; \
            u32 r1 = six(e4, o5, e6, o7, e8, o9, kb, k81) | six(o5, e6, o7, e8, o9, e10, kb, k81) << 8; \
            if ((OFF) == 0) { \
                r0 = avg_up(r0, w2); \
                r1 = avg_up(r1, w6); \
            } else if ((OFF) == 1) { \
                r0 = avg_up(r0, ld32(s + 3)); \
                r1 = avg_up(r1, ld32(s + 7)); \
            } \
            st32(mb + x, r0); \
            st32(mb + x + 4, r1); \
        } \
        if (x < w) { \
            const u8 *s = src + x; \
            u32 w0 = ld32(s), w2 = ld32(s + 2), w4 = ld32(s + 4); \
            u32 e0 = uxtb16(w0), o1 = uxtb16_r8(w0), e2 = uxtb16(w2), o3 = uxtb16_r8(w2); \
            u32 e4 = uxtb16(w4), o5 = uxtb16_r8(w4), e6 = uxtb16_r8(ld32(s + 5)); /* 6, 8 */ \
            u32 r0 = six(e0, o1, e2, o3, e4, o5, kb, k81) | six(o1, e2, o3, e4, o5, e6, kb, k81) << 8; \
            if ((OFF) == 0) \
                r0 = avg_up(r0, w2); \
            else if ((OFF) == 1) \
                r0 = avg_up(r0, ld32(s + 3)); \
            st32(mb + x, r0); \
        } \
    } while (0)

void h264bsdDspHor(const u8 *src, u32 stride, u8 *mb, u32 w, u32 h, i32 off)
{
    u32 kb = KREG(0x0a300a30u), k81 = KREG(0x00510051u);
    if (off < 0) {
        for (u32 y = 0; y < h; y++, src += stride, mb += 16)
            HOR_ROW(-1);
    } else if (off == 0) {
        for (u32 y = 0; y < h; y++, src += stride, mb += 16)
            HOR_ROW(0);
    } else {
        for (u32 y = 0; y < h; y++, src += stride, mb += 16)
            HOR_ROW(1);
    }
}

/* Vertical: a column of 4 pixels at a time down the block, the six rows'
 * even and odd lanes kept across the rows. */
#define VER_COL(OFF) \
    do { \
        const u8 *s = src + x; \
        u32 r0 = ld32(s), r1 = ld32(s + stride), r2 = ld32(s + 2 * stride), r3 = ld32(s + 3 * stride); \
        u32 r4 = ld32(s + 4 * stride); \
        u32 e0 = uxtb16(r0), e1 = uxtb16(r1), e2 = uxtb16(r2), e3 = uxtb16(r3), e4 = uxtb16(r4); \
        u32 o0 = uxtb16_r8(r0), o1 = uxtb16_r8(r1), o2 = uxtb16_r8(r2), o3 = uxtb16_r8(r3); \
        u32 o4 = uxtb16_r8(r4); \
        u8 *o = mb + x; \
        s += 5 * stride; \
        for (u32 y = 0; y < h; y++, s += stride, o += 16) { \
            u32 r5 = ld32(s); \
            u32 e5 = uxtb16(r5), o5 = uxtb16_r8(r5); \
            u32 r = six(e0, e1, e2, e3, e4, e5, kb, k81) | six(o0, o1, o2, o3, o4, o5, kb, k81) << 8; \
            if ((OFF) == 0) \
                r = avg_up(r, (e2 | o2 << 8)); \
            else if ((OFF) == 1) \
                r = avg_up(r, (e3 | o3 << 8)); \
            st32(o, r); \
            e0 = e1; e1 = e2; e2 = e3; e3 = e4; e4 = e5; \
            o0 = o1; o1 = o2; o2 = o3; o3 = o4; o4 = o5; \
        } \
    } while (0)

void h264bsdDspVer(const u8 *src, u32 stride, u8 *mb, u32 w, u32 h, i32 off)
{
    u32 kb = KREG(0x0a300a30u), k81 = KREG(0x00510051u);
    for (u32 x = 0; x < w; x += 4) {
        if (off < 0)
            VER_COL(-1);
        else if (off == 0)
            VER_COL(0);
        else
            VER_COL(1);
    }
}

/* One chroma component, w 4 or 8 wide (dst rows 8 bytes apart): (wa A + wb
 * B + wc C + wd D + 32) >> 6, A the sample, B its right, C below, D below
 * right. rows == h: wc and wd are 0 and the row below isn't read (it may be
 * past the block); wb and wd 0 (vertical only): B and D are A and C, so the
 * column right of the block isn't read either. */
void h264bsdDspChroma(const u8 *src, u32 stride, u8 *dst, u32 w, u32 h, u32 rows,
                      u32 wa, u32 wb, u32 wc, u32 wd)
{
    u32 k32 = KREG(0x00200020u);
    if (rows > h) {
        const u32 r = (wb | wd) != 0;
        for (u32 y = 0; y < h; y++, src += stride, dst += 8) {
            for (u32 x = 0; x < w; x += 4) {
                u32 a = ld32(src + x), b = ld32(src + x + r);
                u32 c = ld32(src + stride + x), d = ld32(src + stride + x + r);
                u32 te = uxtb16(a) * wa + uxtb16(b) * wb + uxtb16(c) * wc + uxtb16(d) * wd + k32;
                u32 to = uxtb16_r8(a) * wa + uxtb16_r8(b) * wb + uxtb16_r8(c) * wc + uxtb16_r8(d) * wd + k32;
                st32(dst + x, ((te >> 6) & 0x00ff00ffu) | ((to >> 6) & 0x00ff00ffu) << 8);
            }
        }
    } else {
        for (u32 y = 0; y < h; y++, src += stride, dst += 8) {
            for (u32 x = 0; x < w; x += 4) {
                u32 a = ld32(src + x), b = ld32(src + x + 1);
                u32 te = uxtb16(a) * wa + uxtb16(b) * wb + k32;
                u32 to = uxtb16_r8(a) * wa + uxtb16_r8(b) * wb + k32;
                st32(dst + x, ((te >> 6) & 0x00ff00ffu) | ((to >> 6) & 0x00ff00ffu) << 8);
            }
        }
    }
}

#endif /* TINYH264_DSP_MC */
