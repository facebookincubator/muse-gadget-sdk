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
 * OPT: motion compensation on Arm Helium (MVE; TINYH264_MVE, Cortex-M55/M85
 * and other Armv8.1-M cores with MVE). h264bsdPredictSamples for every
 * partition (16x16 down to 4x4): all sixteen luma positions and the chroma,
 * eight pixels at a time in 16-bit lanes (widening loads, narrowing stores, a
 * lane predicate for 4- and 2-wide blocks), the same integer arithmetic as
 * the C (h264bsd_reconstruct.c, which stays the reference):
 *
 *   six taps     b1 = A + F - 5 (B + E) + 20 (C + D);  b = clip((b1 + 16) >> 5)
 *   centre       j1 = the six taps over six rows of b1 (32-bit lanes);  j = clip((j1 + 512) >> 10)
 *   quarters     (x + y + 1) >> 1 of the two samples the standard names (8.4.2.2.1)
 *   chroma       (wA A + wB B + wC C + wD D + 32) >> 6
 *
 * A block whose taps reach outside the reference picture is first copied with
 * its edges repeated (h264bsdFillBlock, as the C does); that changes no
 * sample, only where they are read. Loads and stores are predicated to the
 * block: nothing past it is read in the picture or written in the macroblock.
 *
 * Inter macroblocks are predicted straight into the picture
 * (h264bsdPredictSamplesPic) and their residual added there
 * (h264bsdAddResidualMb), with no macroblock array copied out.
 *
 * The first call checks the Helium path against the C on random blocks (every
 * partition shape and position, some off the edge) and keeps the C if any
 * sample differs (h264bsdPieState: 1 Helium, 0 C; TINYH264_LOG reports it).
 */
#include "basetype.h"

#if defined(H264BSD_MVE)
#include <arm_mve.h>
#include <string.h>

#include "h264bsd_reconstruct.h"
#include "h264bsd_util.h"

typedef int16x8_t v16;
typedef uint16x8_t u16v;

/* the C reference (h264bsd_reconstruct.c builds it under this name beside this file) */
void h264bsdPredictSamplesC(u8 *data, mv_t *mv, image_t *refPic, u32 xA, u32 yA, u32 partX, u32 partY,
                            u32 partWidth, u32 partHeight);

/* Loads and stores of 8 samples in 16-bit lanes. F (a constant once inlined): the whole 8
 * are in the block, plain loads and stores; else predicated by m (a 2 or 4 wide block),
 * inactive lanes 0. Each predicated one costs a VPST more (GCC keeps it even for an
 * all-true predicate), so the 8- and 16-wide blocks take the F path. */
#define MVE_AI static inline __attribute__((always_inline))

MVE_AI u16v ldu(const u8 *p, mve_pred16_t m, int F)
{
    return F ? vldrbq_u16(p) : vldrbq_z_u16(p, m);
}

MVE_AI v16 ldw(const u8 *p, mve_pred16_t m, int F)
{
    return vreinterpretq_s16_u16(ldu(p, m, F));
}

MVE_AI void stu(u8 *p, u16v v, mve_pred16_t m, int F)
{
    if (F)
        vstrbq_u16(p, v);
    else
        vstrbq_p_u16(p, v, m);
}

/* A + F - 5 (B + E) + 20 (C + D), exact in 16 bits for samples (-2550 .. 10710) */
MVE_AI v16 tap6(v16 a, v16 b, v16 c, v16 d, v16 e, v16 f)
{
    v16 r = vaddq_s16(a, f);

    r = vmlaq_n_s16(r, vaddq_s16(c, d), 20);
    return vmlaq_n_s16(r, vaddq_s16(b, e), -5);
}

/* clip((x + 16) >> 5) as 16-bit lanes 0..255 */
MVE_AI u16v rnd5(v16 x)
{
    return vreinterpretq_u16_u8(vqrshrunbq_n_s16(vdupq_n_u8(0), x, 5));
}

/* the horizontal six taps of a row; s: two samples left of the first output */
MVE_AI v16 hrow(const u8 *s, mve_pred16_t m, int F)
{
    return tap6(ldw(s, m, F), ldw(s + 1, m, F), ldw(s + 2, m, F), ldw(s + 3, m, F), ldw(s + 4, m, F),
                ldw(s + 5, m, F));
}

/* the vertical six taps of a column chunk; s: two rows above the output row */
MVE_AI v16 vcol(const u8 *s, u32 st, mve_pred16_t m, int F)
{
    return tap6(ldw(s, m, F), ldw(s + st, m, F), ldw(s + 2 * st, m, F), ldw(s + 3 * st, m, F),
                ldw(s + 4 * st, m, F), ldw(s + 5 * st, m, F));
}

/* a kernel's 8-column chunk: KERNEL##Chunk(..., m, F), the F path when all 8 are in the block */
#define CHUNKS(w, call)                                                                                    \
    for (u32 x = 0; x < (w); x += 8) {                                                                     \
        mve_pred16_t m = vctp16q((w) - x);                                                                 \
        if ((w) - x >= 8)                                                                                  \
            call(1);                                                                                       \
        else                                                                                               \
            call(0);                                                                                       \
    }

/* ---- luma: g points at the integer sample G of the block's first pixel ------------------ */

/* G: the block itself */
static void McCopy(const u8 *g, u32 st, u8 *mb, u32 os, u32 w, u32 h)
{
    u32 y;

    if (w == 16) {
        for (y = 0; y < h; y++)
            vstrbq_u8(mb + os * y, vldrbq_u8(g + y * st));
    } else if (w == 8) {
        for (y = 0; y < h; y++)
            vstrbq_u16(mb + os * y, vldrbq_u16(g + y * st));
    } else {
        mve_pred16_t m = vctp16q(w);

        for (y = 0; y < h; y++)
            vstrbq_p_u16(mb + os * y, vldrbq_z_u16(g + y * st, m), m);
    }
}

/* b (avg 0), a (avg 1: with G), c (avg 2: with the sample right of G) */
MVE_AI void McHorChunk(const u8 *s, u32 st, u8 *o, u32 os, u32 h, u32 avg, mve_pred16_t m, int F)
{
    for (u32 y = 0; y < h; y++, s += st, o += os) {
        u16v r = rnd5(hrow(s, m, F));

        if (avg)
            r = vrhaddq_u16(r, ldu(s + 1 + avg, m, F));
        stu(o, r, m, F);
    }
}

static void McHor(const u8 *g, u32 st, u8 *mb, u32 os, u32 w, u32 h, u32 avg)
{
#define CALL(F) McHorChunk(g + x - 2, st, mb + x, os, h, avg, m, F)
    CHUNKS(w, CALL)
#undef CALL
}

/* h (avg 0), d (avg 1: with G), n (avg 2: with the sample below G). Each row from its six
 * source rows afresh (reloads are cheap; a window carried in registers spilled) */
MVE_AI void McVerChunk(const u8 *s, u32 st, u8 *o, u32 os, u32 h, u32 avg, mve_pred16_t m, int F)
{
    for (u32 y = 0; y < h; y++, s += st, o += os) {
        u16v r = rnd5(vcol(s, st, m, F));

        if (avg)
            r = vrhaddq_u16(r, ldu(s + (1 + avg) * st, m, F));
        stu(o, r, m, F);
    }
}

static void McVer(const u8 *g, u32 st, u8 *mb, u32 os, u32 w, u32 h, u32 avg)
{
#define CALL(F) McVerChunk(g + x - 2 * st, st, mb + x, os, h, avg, m, F)
    CHUNKS(w, CALL)
#undef CALL
}

/* e, g, p, r: the horizontal half sample of row roff (b: 0, s: 1) averaged with the vertical
 * one of column coff (h: 0, m: 1) */
MVE_AI void McHorVerChunk(const u8 *sh, const u8 *sv, u32 st, u8 *o, u32 os, u32 h, mve_pred16_t m, int F)
{
    for (u32 y = 0; y < h; y++, sv += st, sh += st, o += os)
        stu(o, vrhaddq_u16(rnd5(hrow(sh, m, F)), rnd5(vcol(sv, st, m, F))), m, F);
}

static void McHorVer(const u8 *g, u32 st, u8 *mb, u32 os, u32 w, u32 h, u32 roff, u32 coff)
{
#define CALL(F) McHorVerChunk(g + x - 2 + roff * st, g + x + coff - 2 * st, st, mb + x, os, h, m, F)
    CHUNKS(w, CALL)
#undef CALL
}

/* j (mode 0), f (1: with b), q (2: with s), i (3: with h), k (4: with m). The h + 5 rows'
 * horizontal six-tap sums first (into hb), then each output row from six of them: no
 * window of vectors carried in registers (it spilled). */
MVE_AI void McMidChunk(const u8 *s, const u8 *sv, u32 st, u8 *o, u32 os, u32 h, u32 mode, v16 *hb,
                       mve_pred16_t m, int F)
{
    const v16 k20 = vdupq_n_s16(20), km5 = vdupq_n_s16(-5);
    u32 y;

    for (y = 0; y < h + 5; y++, s += st)
        hb[y] = hrow(s, m, F);
    for (y = 0; y < h; y++, sv += st, o += os) {
        v16 ta = vaddq_s16(hb[y], hb[y + 5]), tb = vaddq_s16(hb[y + 1], hb[y + 4]),
            tc = vaddq_s16(hb[y + 2], hb[y + 3]);
        int32x4_t lo = vaddq_s32(vaddq_s32(vmovlbq_s16(ta), vmullbq_int_s16(tc, k20)), vmullbq_int_s16(tb, km5));
        int32x4_t hi = vaddq_s32(vaddq_s32(vmovltq_s16(ta), vmulltq_int_s16(tc, k20)), vmulltq_int_s16(tb, km5));
        v16 jn = vqrshrntq_n_s32(vqrshrnbq_n_s32(vdupq_n_s16(0), lo, 10), hi, 10);
        u16v j = vreinterpretq_u16_u8(vqmovunbq_s16(vdupq_n_u8(0), jn));

        if (mode == 1)
            j = vrhaddq_u16(j, rnd5(hb[y + 2]));
        else if (mode == 2)
            j = vrhaddq_u16(j, rnd5(hb[y + 3]));
        else if (mode >= 3)
            j = vrhaddq_u16(j, rnd5(vcol(sv, st, m, F)));
        stu(o, j, m, F);
    }
}

static void McMid(const u8 *g, u32 st, u8 *mb, u32 os, u32 w, u32 h, u32 mode)
{
    v16 hb[21];

#define CALL(F) McMidChunk(g + x - 2 - 2 * st, g + x + (mode == 4) - 2 * st, st, mb + x, os, h, mode, hb, m, F)
    CHUNKS(w, CALL)
#undef CALL
}

static void McLuma(const u8 *g, u32 st, u8 *mb, u32 os, u32 w, u32 h, u32 xf, u32 yf)
{
    switch (xf | yf << 2) {
    case 0: McCopy(g, st, mb, os, w, h); break;
    case 1: McHor(g, st, mb, os, w, h, 1); break;          /* a */
    case 2: McHor(g, st, mb, os, w, h, 0); break;          /* b */
    case 3: McHor(g, st, mb, os, w, h, 2); break;          /* c */
    case 4: McVer(g, st, mb, os, w, h, 1); break;          /* d */
    case 8: McVer(g, st, mb, os, w, h, 0); break;          /* h */
    case 12: McVer(g, st, mb, os, w, h, 2); break;         /* n */
    case 5: McHorVer(g, st, mb, os, w, h, 0, 0); break;    /* e */
    case 7: McHorVer(g, st, mb, os, w, h, 0, 1); break;    /* g */
    case 13: McHorVer(g, st, mb, os, w, h, 1, 0); break;   /* p */
    case 15: McHorVer(g, st, mb, os, w, h, 1, 1); break;   /* r */
    case 10: McMid(g, st, mb, os, w, h, 0); break;         /* j */
    case 6: McMid(g, st, mb, os, w, h, 1); break;          /* f */
    case 14: McMid(g, st, mb, os, w, h, 2); break;         /* q */
    case 9: McMid(g, st, mb, os, w, h, 3); break;          /* i */
    default: McMid(g, st, mb, os, w, h, 4); break;         /* 11: k */
    }
}

/* ---- chroma: both components, w 2, 4 or 8 -------------------------------------------- */

/* (wa A + wb B + 32) >> 6 per row, A at s, B at s + d (d: 1 across, st down), or with
 * xf and yf both set the four-sample sum over (A B / C D) */
MVE_AI void McChromaW(const u8 *cb, const u8 *cr, u32 st, u8 *ocb, u8 *ocr, u32 os, u32 h, u32 xf, u32 yf,
                      mve_pred16_t m, int F)
{
    for (u32 c = 0; c < 2; c++) {
        const u8 *s = c ? cr : cb;
        u8 *o = c ? ocr : ocb;
        u32 y;

        if (xf && yf) {
            u16 wa = (u16)((8 - xf) * (8 - yf)), wb = (u16)(xf * (8 - yf)), wc = (u16)((8 - xf) * yf),
                wd = (u16)(xf * yf);
            u16v a = ldu(s, m, F), b = ldu(s + 1, m, F);

            for (y = 0; y < h; y++, o += os) {
                s += st;
                u16v cc = ldu(s, m, F), d = ldu(s + 1, m, F);
                u16v r = vmulq_n_u16(a, wa);

                r = vmlaq_n_u16(r, b, wb);
                r = vmlaq_n_u16(r, cc, wc);
                r = vmlaq_n_u16(r, d, wd);
                stu(o, vrshrq_n_u16(r, 6), m, F);
                a = cc;
                b = d;
            }
        } else if (xf || yf) {
            u32 f = xf | yf, d = xf ? 1 : st;
            u16 wa = (u16)(8 * (8 - f)), wb = (u16)(8 * f);

            for (y = 0; y < h; y++, s += st, o += os) {
                u16v r = vmulq_n_u16(ldu(s, m, F), wa);

                r = vmlaq_n_u16(r, ldu(s + d, m, F), wb);
                stu(o, vrshrq_n_u16(r, 6), m, F);
            }
        } else {
            for (y = 0; y < h; y++, s += st, o += os)
                stu(o, ldu(s, m, F), m, F);
        }
    }
}

static void McChroma(const u8 *cb, const u8 *cr, u32 st, u8 *ocb, u8 *ocr, u32 os, u32 w, u32 h, u32 xf, u32 yf)
{
    mve_pred16_t m = vctp16q(w);

    if (w == 8)
        McChromaW(cb, cr, st, ocb, ocr, os, h, xf, yf, m, 1);
    else
        McChromaW(cb, cr, st, ocb, ocr, os, h, xf, yf, m, 0);
}

/* ---- h264bsdPredictSamples ------------------------------------------------------------ */

/* the prediction's destination: the macroblock array (luma 16 x 16 at stride 16, Cb and Cr
 * 8 x 8 at stride 8 behind it) or the picture's macroblock itself */
typedef struct {
    u8 *l, *cb, *cr;
    u32 ls, cs;
} mcDst_t;

static void PredictMve(const mcDst_t *dst, mv_t *mv, image_t *refPic, u32 xA, u32 yA, u32 partX,
                                   u32 partY, u32 partWidth, u32 partHeight)
{
    /* luma: the window the taps read (2 left/up, 3 right/down where fractional) */
    u8 fill[21 * 24] __attribute__((aligned(16)));
    u32 xf = (u32)mv->hor & 3, yf = (u32)mv->ver & 3;
    i32 width = 16 * (i32)refPic->width, height = 16 * (i32)refPic->height;
    i32 xi = (i32)xA + (i32)partX + (mv->hor >> 2), yi = (i32)yA + (i32)partY + (mv->ver >> 2);
    i32 l = xf ? 2 : 0, r = xf ? 3 : 0, t = yf ? 2 : 0, b = yf ? 3 : 0;
    const u8 *g;
    u32 st;

    if (xi - l < 0 || xi + (i32)partWidth + r > width || yi - t < 0 || yi + (i32)partHeight + b > height) {
        h264bsdFillBlock(refPic->data, fill, xi - 2, yi - 2, (u32)width, (u32)height, partWidth + 5, partHeight + 5,
                         24);
        g = fill + 2 * 24 + 2;
        st = 24;
    } else {
        g = refPic->data + yi * width + xi;
        st = (u32)width;
    }
    McLuma(g, st, dst->l + dst->ls * partY + partX, dst->ls, partWidth, partHeight, xf, yf);

    /* chroma: 1/8 sample; the window one wider / higher where fractional */
    u8 cfill[2 * 9 * 16] __attribute__((aligned(16)));
    u32 cw = partWidth >> 1, ch = partHeight >> 1;
    i32 cwid = width >> 1, chei = height >> 1;
    i32 cx = (((i32)xA + (i32)partX) >> 1) + (mv->hor >> 3), cy = (((i32)yA + (i32)partY) >> 1) + (mv->ver >> 3);
    u32 cxf = (u32)mv->hor & 7, cyf = (u32)mv->ver & 7;
    u8 *cbase = refPic->data + 256 * refPic->width * refPic->height;
    const u8 *cb, *cr;
    u32 cst;

    if (cx < 0 || cx + (i32)cw + (cxf != 0) > cwid || cy < 0 || cy + (i32)ch + (cyf != 0) > chei) {
        h264bsdFillBlock(cbase, cfill, cx, cy, (u32)cwid, (u32)chei, cw + 1, ch + 1, 16);
        h264bsdFillBlock(cbase + cwid * chei, cfill + 9 * 16, cx, cy, (u32)cwid, (u32)chei, cw + 1, ch + 1, 16);
        cb = cfill;
        cr = cfill + 9 * 16;
        cst = 16;
    } else {
        cb = cbase + cy * cwid + cx;
        cr = cb + cwid * chei;
        cst = (u32)cwid;
    }
    u32 co = dst->cs * (partY >> 1) + (partX >> 1);

    McChroma(cb, cr, cst, dst->cb + co, dst->cr + co, dst->cs, cw, ch, cxf, cyf);
}

/* OPT: a port's printf-style log (none by default) */
#ifndef TINYH264_LOG
#define TINYH264_LOG(...) ((void)0)
#endif

/* 1 Helium, 0 C, -1 unchecked, -2 being checked; read and written
 * atomically, as decoders on other threads share it */
static i32 mcState = -1;

static void McSelftest(void);

static inline i32 McOk(void)
{
    if (__atomic_load_n(&mcState, __ATOMIC_ACQUIRE) == -1)
        McSelftest();
    return __atomic_load_n(&mcState, __ATOMIC_ACQUIRE) == 1;
}

static inline mcDst_t MbArray(u8 *data)
{
    mcDst_t d = {data, data + 256, data + 320, 16, 8};
    return d;
}

void h264bsdPredictSamples(u8 *data, mv_t *mv, image_t *refPic, u32 xA, u32 yA, u32 partX, u32 partY,
                                       u32 partWidth, u32 partHeight)
{
    if (McOk()) {
        mcDst_t d = MbArray(data);

        PredictMve(&d, mv, refPic, xA, yA, partX, partY, partWidth, partHeight);
    } else
        h264bsdPredictSamplesC(data, mv, refPic, xA, yA, partX, partY, partWidth, partHeight);
}

/* The same, straight into the current picture's macroblock (image->luma / cb / cr, set by
 * h264bsdSetCurrImageMbPointers): h264bsdInterPrediction then adds the residual in place
 * (h264bsdAddResidualMb) instead of copying the macroblock array out. */
void h264bsdPredictSamplesPic(image_t *image, mv_t *mv, image_t *refPic, u32 xA, u32 yA, u32 partX,
                                          u32 partY, u32 partWidth, u32 partHeight)
{
    u32 w = image->width * 16;
    mcDst_t d = {image->luma, image->cb, image->cr, w, w / 2};

    if (McOk()) {
        PredictMve(&d, mv, refPic, xA, yA, partX, partY, partWidth, partHeight);
        return;
    }
    /* the C's (the check failed): through a macroblock array, the partition copied out */
    u8 data[384] __attribute__((aligned(16)));
    u32 y;

    h264bsdPredictSamplesC(data, mv, refPic, xA, yA, partX, partY, partWidth, partHeight);
    for (y = 0; y < partHeight; y++)
        memcpy(d.l + (partY + y) * d.ls + partX, data + (partY + y) * 16 + partX, partWidth);
    for (y = 0; y < partHeight / 2; y++) {
        u32 o = (partY / 2 + y) * d.cs + partX / 2, i = (partY / 2 + y) * 8 + partX / 2;

        memcpy(d.cb + o, data + 256 + i, partWidth / 2);
        memcpy(d.cr + o, data + 320 + i, partWidth / 2);
    }
}

/* A 4x4 block's residual (-512 .. 511) added to the picture in place, clipped to 0..255 */
static inline void AddBlock4(u8 *p, u32 st, const i32 *r)
{
    const int32x4_t lo = vdupq_n_s32(0), hi = vdupq_n_s32(255);

    for (u32 i = 0; i < 4; i++, p += st, r += 4) {
        int32x4_t v = vaddq_s32(vreinterpretq_s32_u32(vldrbq_u32(p)), vldrwq_s32((const int32_t *)r));

        vstrbq_s32((int8_t *)p, vmaxq_s32(vminq_s32(v, hi), lo));
    }
}

/* An inter macroblock's residual (ProcessResidual's 24 blocks: 16 luma, 4 Cb, 4 Cr; mask: the
 * ones with a residual, ProcessResidualInter's) added to its prediction, already in the
 * picture: what h264bsdWriteOutputBlocks does from the macroblock array. */
void h264bsdAddResidualMb(image_t *image, i32 residual[][16], u32 mask)
{
    extern const u32 h264bsdBlockX[], h264bsdBlockY[];
    u32 w = image->width * 16, m = mask & 0xFFFF;

    while (m) {
        u32 b = (u32)__builtin_ctz(m);

        m &= m - 1;
        AddBlock4(image->luma + h264bsdBlockY[b] * w + h264bsdBlockX[b], w, residual[b]);
    }
    w /= 2;
    m = mask >> 16;
    while (m) {
        u32 c = (u32)__builtin_ctz(m), b = 16 + c;

        m &= m - 1;
        AddBlock4((c < 4 ? image->cb : image->cr) + h264bsdBlockY[c & 3] * w + h264bsdBlockX[c & 3], w, residual[b]);
    }
}

i32 h264bsdPieState(void)
{
    return __atomic_load_n(&mcState, __ATOMIC_ACQUIRE);
}

/* Helium against C: random 48x48 reference pictures (3 x 3 macroblocks: noise, 0/255,
 * ramps), every partition shape, motion vectors to every quarter position, some reaching
 * past the edges. Runs once, on the first inter macroblock. */
static void McSelftest(void)
{
    static u8 pic[48 * 48 * 3 / 2];
    static const u8 shapes[7][2] = {{16, 16}, {16, 8}, {8, 16}, {8, 8}, {8, 4}, {4, 8}, {4, 4}};
    u32 seed = 0x2545f491, n = 0, bad = 0;

    if (!__atomic_compare_exchange_n(&mcState, &(i32){-1}, -2, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return; /* another thread checks; the C meanwhile */
    image_t ref = {0};

    ref.data = pic;
    ref.width = 3;
    ref.height = 3;
    for (u32 it = 0; it < 4 && !bad; it++) {
        for (u32 i = 0; i < sizeof pic; i++) {
            seed = seed * 1664525 + 1013904223;
            pic[i] = it % 3 == 0 ? (u8)(seed >> 24) : it % 3 == 1 ? (u8)((seed >> 31) * 255) : (u8)(i * 7 + it);
        }
        for (u32 k = 0; k < 7 * 16 && !bad; k++) {
            u8 a[384] __attribute__((aligned(16))), c[384] __attribute__((aligned(16)));
            u32 sh = k % 7, w = shapes[sh][0], h = shapes[sh][1];
            mv_t mv;

            seed = seed * 1664525 + 1013904223;
            /* integer part -24 .. +23 around the middle macroblock (past the edges too), the
             * quarter (luma) / eighth (chroma) from k so every position comes up */
            mv.hor = (i16)(((i32)((seed >> 8) % 48) - 24) * 4 + (i32)(k & 3) + (i32)((seed >> 4) & 4));
            mv.ver = (i16)(((i32)((seed >> 16) % 48) - 24) * 4 + (i32)((k >> 2) & 3) + (i32)((seed >> 5) & 4));
            u32 px = (seed >> 26) & 1 ? 16 - w : 0, py = (seed >> 27) & 1 ? 16 - h : 0;

            memset(a, 0x5a, sizeof a);
            memset(c, 0x5a, sizeof c);
            h264bsdPredictSamplesC(c, &mv, &ref, 16, 16, px, py, w, h);
            mcDst_t d = MbArray(a);

            PredictMve(&d, &mv, &ref, 16, 16, px, py, w, h);
            n++;
            if (memcmp(a, c, sizeof a)) {
                bad = 1;
                TINYH264_LOG("Helium motion compensation differs from C (%ux%u at %u,%u, mv %d,%d): using C",
                       (unsigned)w, (unsigned)h, (unsigned)px, (unsigned)py, mv.hor, mv.ver);
            }
        }
    }
    if (!bad)
        TINYH264_LOG("Helium motion compensation matches C on %u blocks: in use", (unsigned)n);
    __atomic_store_n(&mcState, bad ? 0 : 1, __ATOMIC_RELEASE);
}
#endif
