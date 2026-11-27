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
 * OPT: motion compensation in packed 4-lane operations (TINYH264_PACKED4_MC):
 * every interpolation function of h264bsd_reconstruct.c, bit-exact with the
 * C ones (renamed *_c there; this file defines the public names). Four
 * output pixels per step, in the operations of h264bsd_packed4_ops.h (or a
 * port's header, TINYH264_PACKED4_OPS_H):
 *
 *   mul4 / mac4    4 unsigned bytes x 4 signed byte coefficients into four
 *                  16-bit lanes (+= for mac4)
 *   rnd5_pack      per lane (x + 16) >> 5, saturated to 0..255, packed
 *   align8/16/24   a funnel shift: bytes 1/2/3.. of hi:lo
 *
 * so the 6-tap filter (1, -5, 20, 20, -5, 1) is one multiply and five
 * multiply-accumulates for four pixels, then a rounding shift and a
 * saturating pack. Rows are read as aligned words and funnel-shifted, so
 * nothing depends on unaligned loads: the reference picture's width is a
 * multiple of 16, and the edge-fill buffer here has a stride rounded up to 4,
 * so a block's byte alignment is the same on every row and each alignment
 * gets its own copy of the loops. An aligned word may reach up to 3 bytes
 * past a block's last tap (the pictures carry 32 bytes of slack).
 *
 * Covered: the half samples b and h, the quarter samples a, c, d, n (an
 * integer sample averaged in) and e, g, p, r (two half samples averaged);
 * j, f, i, k, q (the 2-D filter: the horizontal 6-tap unrounded into 16-bit
 * lanes, the vertical one into 32-bit lanes with HMUL / HMAC); chroma 4 or 8
 * wide (2-wide chroma stays in C).
 *
 * h264bsdPacked4Check() runs every function against the C on random blocks,
 * for a port to check its operations on the target.
 *
 * Little-endian targets only: rows are read as native words whose byte k is
 * lane k.
 */

#include "basetype.h"

#if defined(TINYH264_PACKED4_MC) && TINYH264_PACKED4_MC

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "TINYH264_PACKED4_MC needs a little-endian target"
#endif

#include "h264bsd_reconstruct.h"
#ifdef TINYH264_PACKED4_OPS_H
#include TINYH264_PACKED4_OPS_H
#else
#include "h264bsd_packed4_ops.h"
#endif

void h264bsdInterpolateVerHalf_c(u8 *ref, u8 *mb, i32 x0, i32 y0, u32 width, u32 height, u32 partWidth,
                                 u32 partHeight);
void h264bsdInterpolateVerQuarter_c(u8 *ref, u8 *mb, i32 x0, i32 y0, u32 width, u32 height, u32 partWidth,
                                    u32 partHeight, u32 verOffset);
void h264bsdInterpolateHorHalf_c(u8 *ref, u8 *mb, i32 x0, i32 y0, u32 width, u32 height, u32 partWidth,
                                 u32 partHeight);
void h264bsdInterpolateHorQuarter_c(u8 *ref, u8 *mb, i32 x0, i32 y0, u32 width, u32 height, u32 partWidth,
                                    u32 partHeight, u32 horOffset);
void h264bsdInterpolateHorVerQuarter_c(u8 *ref, u8 *mb, i32 x0, i32 y0, u32 width, u32 height, u32 partWidth,
                                       u32 partHeight, u32 horVerOffset);
void h264bsdInterpolateMidHalf_c(u8 *ref, u8 *mb, i32 x0, i32 y0, u32 width, u32 height, u32 partWidth,
                                 u32 partHeight);
void h264bsdInterpolateMidVerQuarter_c(u8 *ref, u8 *mb, i32 x0, i32 y0, u32 width, u32 height, u32 partWidth,
                                       u32 partHeight, u32 verOffset);
void h264bsdInterpolateMidHorQuarter_c(u8 *ref, u8 *mb, i32 x0, i32 y0, u32 width, u32 height, u32 partWidth,
                                       u32 partHeight, u32 horOffset);
void h264bsdInterpolateChromaHor_c(u8 *pRef, u8 *predPartChroma, i32 x0, i32 y0, u32 width, u32 height,
                                   u32 xFrac, u32 chromaPartWidth, u32 chromaPartHeight);
void h264bsdInterpolateChromaVer_c(u8 *pRef, u8 *predPartChroma, i32 x0, i32 y0, u32 width, u32 height,
                                   u32 yFrac, u32 chromaPartWidth, u32 chromaPartHeight);
void h264bsdInterpolateChromaHorVer_c(u8 *ref, u8 *predPartChroma, i32 x0, i32 y0, u32 width, u32 height,
                                      u32 xFrac, u32 yFrac, u32 chromaPartWidth, u32 chromaPartHeight);

#define AI static inline __attribute__((always_inline))

/* bytes n..n+3 of the little-endian words w[] (n a constant after inlining) */
AI u32 fun(const u32 *w, const int n)
{
    const int k = n >> 2;
    switch (n & 3) {
    case 1:
        return align8(w[k + 1], w[k]);
    case 2:
        return align16(w[k + 1], w[k]);
    case 3:
        return align24(w[k + 1], w[k]);
    default:
        return w[k];
    }
}

/* per byte (x + y + 1) >> 1 */
AI u32 avg4(u32 x, u32 y)
{
    return (x | y) - (((x ^ y) >> 1) & 0x7f7f7f7fu);
}

#define C1  0x01010101u
#define C5  0xfbfbfbfbu   /* -5 */
#define C20 0x14141414u

AI u32 tap6(u32 s0, u32 s1, u32 s2, u32 s3, u32 s4, u32 s5)
{
    p4_t a = mul4(s0, C1);
    a = mac4(a, s1, C5);
    a = mac4(a, s2, C20);
    a = mac4(a, s3, C20);
    a = mac4(a, s4, C5);
    a = mac4(a, s5, C1);
    return rnd5_pack(a);
}

/* Horizontal: dst[r][c] = 6-tap over src[r][c..c+5]; q = 2/3: averaged with
 * src[r][c+q] (a, c); acc: averaged with what dst holds. src's alignment o. */
AI void hor_core(const u8 *src, u32 stride, u8 *dst, u32 pw, u32 ph, const int o, const int q, const int acc)
{
    const u8 *row = src - o;
    for (u32 r = ph; r; r--) {
        const u32 *w = (const u32 *)row;
        u32 *d = (u32 *)dst;
        u32 W[3];
        W[0] = w[0];
        W[1] = w[1];
        for (u32 c = pw >> 2; c; c--) {
            W[2] = w[2];
            u32 v = tap6(fun(W, o), fun(W, o + 1), fun(W, o + 2), fun(W, o + 3), fun(W, o + 4), fun(W, o + 5));
            if (q)
                v = avg4(v, fun(W, o + q));
            if (acc)
                v = avg4(v, *d);
            *d++ = v;
            W[0] = W[1];
            W[1] = W[2];
            w++;
        }
        row += stride;
        dst += 16;
    }
}

/* Vertical: dst[r][c] = 6-tap over src[r..r+5][c]; q = 2/3: averaged with
 * src[r+q][c] (d, n); acc: averaged with what dst holds. */
AI u32 ldw(const u8 *p, const int o)
{
    const u32 *w = (const u32 *)(p - o);
    return o ? fun(w, o) : w[0];
}

AI void ver_core(const u8 *src, u32 stride, u8 *dst, u32 pw, u32 ph, const int o, const int q, const int acc)
{
    for (u32 c = 0; c < pw; c += 4) {
        const u8 *p = src + c;
        u32 s0 = ldw(p, o), s1 = ldw(p + stride, o), s2 = ldw(p + 2 * stride, o), s3 = ldw(p + 3 * stride, o),
            s4 = ldw(p + 4 * stride, o);
        p += 5 * stride;
        u32 *d = (u32 *)(dst + c);
        for (u32 r = ph; r; r--) {
            u32 s5 = ldw(p, o);
            u32 v = tap6(s0, s1, s2, s3, s4, s5);
            if (q == 2)
                v = avg4(v, s2);
            else if (q == 3)
                v = avg4(v, s3);
            if (acc)
                v = avg4(v, *d);
            *d = v;
            d += 4;
            p += stride;
            s0 = s1;
            s1 = s2;
            s2 = s3;
            s3 = s4;
            s4 = s5;
        }
    }
}

/* the alignment last, then the mode constants, so each case is a constant copy */
#define HOR(src, stride, dst, pw, ph, q, acc) do { \
    const u8 *s_ = (src);                          \
    switch ((u32)(size_t)s_ & 3) {                 \
    case 0: hor_core(s_, stride, dst, pw, ph, 0, q, acc); break; \
    case 1: hor_core(s_, stride, dst, pw, ph, 1, q, acc); break; \
    case 2: hor_core(s_, stride, dst, pw, ph, 2, q, acc); break; \
    default: hor_core(s_, stride, dst, pw, ph, 3, q, acc); break; \
    } } while (0)

#define VER(src, stride, dst, pw, ph, q, acc) do { \
    const u8 *s_ = (src);                          \
    switch ((u32)(size_t)s_ & 3) {                 \
    case 0: ver_core(s_, stride, dst, pw, ph, 0, q, acc); break; \
    case 1: ver_core(s_, stride, dst, pw, ph, 1, q, acc); break; \
    case 2: ver_core(s_, stride, dst, pw, ph, 2, q, acc); break; \
    default: ver_core(s_, stride, dst, pw, ph, 3, q, acc); break; \
    } } while (0)

/* The block's top-left in the picture, or in an edge-filled copy (stride a
 * multiple of 4) when it reaches past an edge. */
static const u8 *block(u8 *ref, i32 x0, i32 y0, u32 width, u32 height, u32 bw, u32 bh, u32 *fill, u32 *stride)
{
    if (x0 < 0 || (u32)x0 + bw > width || y0 < 0 || (u32)y0 + bh > height) {
        u32 s = (bw + 3) & ~3u;
        h264bsdFillBlock(ref, (u8 *)fill, x0, y0, width, height, bw, bh, s);
        *stride = s;
        return (const u8 *)fill;
    }
    *stride = width;
    return ref + (u32)y0 * width + (u32)x0;
}

/* an edge-filled luma block: 21 rows of 24, and a word of slack per row read */
#define FILL_WORDS (21 * 24 / 4 + 4)

void h264bsdInterpolateVerHalf(u8 *ref, u8 *mb, i32 x0, i32 y0, u32 width, u32 height, u32 partWidth,
                               u32 partHeight)
{
    u32 fill[FILL_WORDS], st;
    const u8 *s = block(ref, x0, y0, width, height, partWidth, partHeight + 5, fill, &st);
    VER(s, st, mb, partWidth, partHeight, 0, 0);
}

void h264bsdInterpolateVerQuarter(u8 *ref, u8 *mb, i32 x0, i32 y0, u32 width, u32 height, u32 partWidth,
                                  u32 partHeight, u32 verOffset)
{
    u32 fill[FILL_WORDS], st;
    const u8 *s = block(ref, x0, y0, width, height, partWidth, partHeight + 5, fill, &st);
    if (verOffset)
        VER(s, st, mb, partWidth, partHeight, 3, 0);
    else
        VER(s, st, mb, partWidth, partHeight, 2, 0);
}

void h264bsdInterpolateHorHalf(u8 *ref, u8 *mb, i32 x0, i32 y0, u32 width, u32 height, u32 partWidth,
                               u32 partHeight)
{
    u32 fill[FILL_WORDS], st;
    const u8 *s = block(ref, x0, y0, width, height, partWidth + 5, partHeight, fill, &st);
    HOR(s, st, mb, partWidth, partHeight, 0, 0);
}

void h264bsdInterpolateHorQuarter(u8 *ref, u8 *mb, i32 x0, i32 y0, u32 width, u32 height, u32 partWidth,
                                  u32 partHeight, u32 horOffset)
{
    u32 fill[FILL_WORDS], st;
    const u8 *s = block(ref, x0, y0, width, height, partWidth + 5, partHeight, fill, &st);
    if (horOffset)
        HOR(s, st, mb, partWidth, partHeight, 3, 0);
    else
        HOR(s, st, mb, partWidth, partHeight, 2, 0);
}

void h264bsdInterpolateHorVerQuarter(u8 *ref, u8 *mb, i32 x0, i32 y0, u32 width, u32 height, u32 partWidth,
                                     u32 partHeight, u32 horVerOffset)
{
    u32 fill[FILL_WORDS], st;
    const u8 *s = block(ref, x0, y0, width, height, partWidth + 5, partHeight + 5, fill, &st);
    /* b (row y) or s (row y+1), then averaged with h (column x) or m (x+1) */
    HOR(s + (2 + (horVerOffset >> 1)) * st, st, mb, partWidth, partHeight, 0, 0);
    VER(s + 2 + (horVerOffset & 1), st, mb, partWidth, partHeight, 0, 1);
}

/* ---- 2-D: j (and f, q, i, k averaged with b, s, h, m) -------------------
 * Horizontal 6-tap without rounding into 16-bit lanes (-2550..10710), then
 * the vertical 6-tap on those in 32-bit lanes, two lanes a step (HMUL, HMAC),
 * then (x + 512) >> 10 and the same saturating pack. */
AI u32 vtap6(u32 t0, u32 t1, u32 t2, u32 t3, u32 t4, u32 t5)
{
    p4_t a = HMUL(t0, 1);
    HMAC(a, t1, -5);
    HMAC(a, t2, 20);
    HMAC(a, t3, 20);
    HMAC(a, t4, -5);
    HMAC(a, t5, 1);
    i32 lo = ((i32)(u32)a + 512) >> 10, hi = ((i32)(u32)(a >> 32) + 512) >> 10;
    return ((u32)lo & 0xffff) | ((u32)hi << 16);
}

AI u32 pack2(u32 w01, u32 w23)
{
    return PACKB(((p4_t)w23 << 32) | w01);
}

AI void mid_core(const u8 *src, u32 stride, u8 *dst, u32 pw, u32 ph, const int o, const int acc)
{
    p4_t t[21 * 4];
    const u8 *row = src - o;
    u32 groups = pw >> 2;
    for (u32 r = 0; r < ph + 5; r++) {
        const u32 *w = (const u32 *)row;
        u32 W[3];
        W[0] = w[0];
        W[1] = w[1];
        for (u32 g = 0; g < groups; g++) {
            W[2] = w[2];
            p4_t a = mul4(fun(W, o), C1);
            a = mac4(a, fun(W, o + 1), C5);
            a = mac4(a, fun(W, o + 2), C20);
            a = mac4(a, fun(W, o + 3), C20);
            a = mac4(a, fun(W, o + 4), C5);
            a = mac4(a, fun(W, o + 5), C1);
            t[r * 4 + g] = a;
            W[0] = W[1];
            W[1] = W[2];
            w++;
        }
        row += stride;
    }
    for (u32 g = 0; g < groups; g++) {
        u32 *d = (u32 *)(dst + 4 * g);
        const p4_t *c = t + g;
        for (u32 r = ph; r; r--) {
            u32 lo = vtap6((u32)c[0], (u32)c[4], (u32)c[8], (u32)c[12], (u32)c[16], (u32)c[20]);
            u32 hi = vtap6((u32)(c[0] >> 32), (u32)(c[4] >> 32), (u32)(c[8] >> 32), (u32)(c[12] >> 32),
                           (u32)(c[16] >> 32), (u32)(c[20] >> 32));
            u32 v = pack2(lo, hi);
            if (acc)
                v = avg4(v, *d);
            *d = v;
            d += 4;
            c += 4;
        }
    }
}

#define MID(src, stride, dst, pw, ph, acc) do { \
    const u8 *s_ = (src);                      \
    switch ((u32)(size_t)s_ & 3) {             \
    case 0: mid_core(s_, stride, dst, pw, ph, 0, acc); break; \
    case 1: mid_core(s_, stride, dst, pw, ph, 1, acc); break; \
    case 2: mid_core(s_, stride, dst, pw, ph, 2, acc); break; \
    default: mid_core(s_, stride, dst, pw, ph, 3, acc); break; \
    } } while (0)

void h264bsdInterpolateMidHalf(u8 *ref, u8 *mb, i32 x0, i32 y0, u32 width, u32 height, u32 partWidth,
                               u32 partHeight)
{
    u32 fill[FILL_WORDS], st;
    const u8 *s = block(ref, x0, y0, width, height, partWidth + 5, partHeight + 5, fill, &st);
    MID(s, st, mb, partWidth, partHeight, 0);
}

void h264bsdInterpolateMidVerQuarter(u8 *ref, u8 *mb, i32 x0, i32 y0, u32 width, u32 height, u32 partWidth,
                                     u32 partHeight, u32 verOffset)
{
    u32 fill[FILL_WORDS], st;
    const u8 *s = block(ref, x0, y0, width, height, partWidth + 5, partHeight + 5, fill, &st);
    HOR(s + (2 + verOffset) * st, st, mb, partWidth, partHeight, 0, 0);   /* b or s */
    MID(s, st, mb, partWidth, partHeight, 1);
}

void h264bsdInterpolateMidHorQuarter(u8 *ref, u8 *mb, i32 x0, i32 y0, u32 width, u32 height, u32 partWidth,
                                     u32 partHeight, u32 horOffset)
{
    u32 fill[FILL_WORDS], st;
    const u8 *s = block(ref, x0, y0, width, height, partWidth + 5, partHeight + 5, fill, &st);
    VER(s + 2 + horOffset, st, mb, partWidth, partHeight, 0, 0);   /* h or m */
    MID(s, st, mb, partWidth, partHeight, 1);
}

/* ---- chroma: eighth-sample bilinear, 4 pixels a step ----------------------
 * (A(8-x)(8-y) + Bx(8-y) + C(8-x)y + Dxy + 32) >> 6 with the same packed
 * multiply-accumulate (coefficients <= 64, sums <= 16320). Each component is
 * its own plane (Cb, then Cr width * height on); the output is two 8x8
 * blocks. */
AI u32 rnd6_pack(p4_t a)
{
    return PACKB(RND6(a));
}

AI void chroma_core(const u8 *src, u32 stride, u8 *dst, u32 cw, u32 ch, u32 cA, u32 cB, u32 cC, u32 cD,
                    const int o)
{
    for (u32 c = 0; c < cw; c += 4) {
        const u8 *p = src + c;
        u32 a0 = ldw(p, o), b0 = ldw(p + 1, (o + 1) & 3);
        u32 *d = (u32 *)(dst + c);
        for (u32 r = ch; r; r--) {
            p += stride;
            u32 a1 = ldw(p, o), b1 = ldw(p + 1, (o + 1) & 3);
            /* unsigned samples x signed coefficients (all <= 64 here) */
            p4_t acc = mul4(a0, cA);
            acc = mac4(acc, b0, cB);
            acc = mac4(acc, a1, cC);
            acc = mac4(acc, b1, cD);
            *d = rnd6_pack(acc);
            d += 2;   /* 8-byte rows */
            a0 = a1;
            b0 = b1;
        }
    }
}

static void chroma_any(u8 *ref, u8 *pred, i32 x0, i32 y0, u32 width, u32 height, u32 xFrac, u32 yFrac,
                       u32 cw, u32 ch)
{
    u32 cA = (8 - xFrac) * (8 - yFrac) * 0x01010101u, cB = xFrac * (8 - yFrac) * 0x01010101u;
    u32 cC = (8 - xFrac) * yFrac * 0x01010101u, cD = xFrac * yFrac * 0x01010101u;
    u32 fill[9 * 12 / 4 + 4];
    for (u32 comp = 0; comp < 2; comp++) {
        u8 *plane = ref + (comp ? width * height : 0);   /* Cr: a plane past Cb */
        const u8 *s;
        u32 st;
        if (x0 < 0 || (u32)x0 + cw + 1 > width || y0 < 0 || (u32)y0 + ch + 1 > height) {
            st = (cw + 1 + 3) & ~3u;
            h264bsdFillBlock(plane, (u8 *)fill, x0, y0, width, height, cw + 1, ch + 1, st);
            s = (const u8 *)fill;
        } else {
            st = width;
            s = plane + (u32)y0 * width + (u32)x0;
        }
        u8 *dst = pred + comp * 64;
        switch ((u32)(size_t)s & 3) {
        case 0: chroma_core(s, st, dst, cw, ch, cA, cB, cC, cD, 0); break;
        case 1: chroma_core(s, st, dst, cw, ch, cA, cB, cC, cD, 1); break;
        case 2: chroma_core(s, st, dst, cw, ch, cA, cB, cC, cD, 2); break;
        default: chroma_core(s, st, dst, cw, ch, cA, cB, cC, cD, 3); break;
        }
    }
}

void h264bsdInterpolateChromaHor(u8 *pRef, u8 *predPartChroma, i32 x0, i32 y0, u32 width, u32 height,
                                 u32 xFrac, u32 chromaPartWidth, u32 chromaPartHeight)
{
    if (chromaPartWidth < 4) {
        h264bsdInterpolateChromaHor_c(pRef, predPartChroma, x0, y0, width, height, xFrac, chromaPartWidth,
                                      chromaPartHeight);
        return;
    }
    chroma_any(pRef, predPartChroma, x0, y0, width, height, xFrac, 0, chromaPartWidth, chromaPartHeight);
}

void h264bsdInterpolateChromaVer(u8 *pRef, u8 *predPartChroma, i32 x0, i32 y0, u32 width, u32 height,
                                 u32 yFrac, u32 chromaPartWidth, u32 chromaPartHeight)
{
    if (chromaPartWidth < 4) {
        h264bsdInterpolateChromaVer_c(pRef, predPartChroma, x0, y0, width, height, yFrac, chromaPartWidth,
                                      chromaPartHeight);
        return;
    }
    chroma_any(pRef, predPartChroma, x0, y0, width, height, 0, yFrac, chromaPartWidth, chromaPartHeight);
}

void h264bsdInterpolateChromaHorVer(u8 *ref, u8 *predPartChroma, i32 x0, i32 y0, u32 width, u32 height,
                                    u32 xFrac, u32 yFrac, u32 chromaPartWidth, u32 chromaPartHeight)
{
    if (chromaPartWidth < 4) {
        h264bsdInterpolateChromaHorVer_c(ref, predPartChroma, x0, y0, width, height, xFrac, yFrac,
                                         chromaPartWidth, chromaPartHeight);
        return;
    }
    chroma_any(ref, predPartChroma, x0, y0, width, height, xFrac, yFrac, chromaPartWidth, chromaPartHeight);
}

/* ---- the check against the C ------------------------------------------- */

static u32 ck_seed = 0x12345678;

static u32 ck_rand(void)
{
    ck_seed = ck_seed * 1664525u + 1013904223u;
    return ck_seed >> 8;
}

/* Every function, offset, partition size and picture-edge case against the
 * C on a random 64x48 picture (chroma: two 32x24 planes in it). Returns the
 * number of mismatching blocks (0: bit-exact); *blocks: blocks run. One
 * caller at a time. */
u32 h264bsdPacked4Check(u32 iters, u32 *blocks)
{
    enum { W = 64, H = 48 };
    static u32 picw[W * H / 4 + 16];   /* + slack: aligned words past the end */
    static const u8 sizes[7][2] = { { 16, 16 }, { 16, 8 }, { 8, 16 }, { 8, 8 }, { 8, 4 }, { 4, 8 }, { 4, 4 } };
    u8 *pic = (u8 *)picw;
    u32 a[16 * 16 / 4 + 4], b[16 * 16 / 4 + 4];
    u32 bad = 0;
    for (u32 i = 0; i < sizeof picw; i++)
        pic[i] = (u8)ck_rand();
    *blocks = 0;
    for (u32 it = 0; it < iters; it++) {
        u32 pw = sizes[it % 7][0], ph = sizes[it % 7][1];
        /* blocks past every edge too */
        i32 x0 = (i32)(ck_rand() % (W + 24)) - 12, y0 = (i32)(ck_rand() % (H + 24)) - 12;
        u32 fn = ck_rand() % 14, off = ck_rand() & 3, xf = ck_rand() & 7, yf = ck_rand() & 7;
        for (u32 k = 0; k < sizeof(a) / 4; k++)
            a[k] = b[k] = ck_rand();   /* the averaging cases read dst too */
        for (int pass = 0; pass < 2; pass++) {
            u8 *mb = (u8 *)(pass ? b : a);
            switch (fn) {
            case 0: (pass ? h264bsdInterpolateVerHalf : h264bsdInterpolateVerHalf_c)(pic, mb, x0, y0, W, H, pw, ph); break;
            case 1: (pass ? h264bsdInterpolateVerQuarter : h264bsdInterpolateVerQuarter_c)(pic, mb, x0, y0, W, H, pw, ph, off & 1); break;
            case 2: (pass ? h264bsdInterpolateHorHalf : h264bsdInterpolateHorHalf_c)(pic, mb, x0, y0, W, H, pw, ph); break;
            case 3: (pass ? h264bsdInterpolateHorQuarter : h264bsdInterpolateHorQuarter_c)(pic, mb, x0, y0, W, H, pw, ph, off & 1); break;
            case 4: case 5: (pass ? h264bsdInterpolateHorVerQuarter : h264bsdInterpolateHorVerQuarter_c)(pic, mb, x0, y0, W, H, pw, ph, off); break;
            case 6: (pass ? h264bsdInterpolateMidHalf : h264bsdInterpolateMidHalf_c)(pic, mb, x0, y0, W, H, pw, ph); break;
            case 7: (pass ? h264bsdInterpolateMidVerQuarter : h264bsdInterpolateMidVerQuarter_c)(pic, mb, x0, y0, W, H, pw, ph, off & 1); break;
            case 8: (pass ? h264bsdInterpolateMidHorQuarter : h264bsdInterpolateMidHorQuarter_c)(pic, mb, x0, y0, W, H, pw, ph, off & 1); break;
            default: {
                /* chroma: blocks pw/2 x ph/2 */
                u32 cw = pw / 2, chh = ph / 2;
                i32 cx = x0 / 2, cy = y0 / 2;
                if (fn == 9 || fn == 12)
                    (pass ? h264bsdInterpolateChromaHor : h264bsdInterpolateChromaHor_c)(pic, mb, cx, cy, W / 2, H / 2, xf | 1, cw, chh);
                else if (fn == 10)
                    (pass ? h264bsdInterpolateChromaVer : h264bsdInterpolateChromaVer_c)(pic, mb, cx, cy, W / 2, H / 2, yf | 1, cw, chh);
                else
                    (pass ? h264bsdInterpolateChromaHorVer : h264bsdInterpolateChromaHorVer_c)(pic, mb, cx, cy, W / 2, H / 2, xf | 1, yf | 1, cw, chh);
                break;
            }
            }
        }
        (*blocks)++;
        u32 diff = 0;
        if (fn >= 9) {
            for (u32 i = 0; i < 128; i++)
                diff |= ((u8 *)a)[i] != ((u8 *)b)[i];
        } else {
            for (u32 y = 0; y < ph; y++)
                for (u32 x = 0; x < pw; x++)
                    diff |= ((u8 *)a)[y * 16 + x] != ((u8 *)b)[y * 16 + x];
        }
        bad += diff;
    }
    return bad;
}

#endif /* TINYH264_PACKED4_MC */
