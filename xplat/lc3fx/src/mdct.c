/******************************************************************************
 *
 *  Copyright 2022 Google LLC
 *  Copyright 2026 Meta Platforms, Inc. and affiliates (fixed-point port)
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at:
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 ******************************************************************************/

/*
 * Fixed-point MDCT, port of liblc3 mdct.c
 *
 * Block floating point, with an L1-norm headroom rule: every value of every
 * FFT stage is a partial DFT, a sum of input values times unit twiddles, so
 * its magnitude is bounded by the sum of the input magnitudes. The input of
 * the FFT is normalized so that this sum stays below 2^29: the FFT stages
 * then run without any scaling nor saturation (`mulh(2x, w)` = x * w for
 * Q31 twiddles), keeping full precision for tonal and noisy signals alike.
 * The pre- and post-rotations halve the values (`mulh(x, w)` = x * w / 2),
 * accounted in the exponents.
 */

#include "mdct.h"
#include "tables.h"

#define mulh lc3fx_mulh

typedef struct lc3fx_complex cplx_t;


/* ----------------------------------------------------------------------------
 *  FFT processing
 * -------------------------------------------------------------------------- */

/**
 * FFT 5 Points
 * x, y            Input and output coefficients, of size 5xn
 * n               Number of interleaved transform to perform
 */
LC3_HOT static void fft_5(const cplx_t *x, cplx_t *y, int n)
{
    const int32_t cos1 =   663608942;   /* cos(-2Pi 1/5) */
    const int32_t cos2 = -1737350766;   /* cos(-2Pi 2/5) */
    const int32_t sin1 = -2042378317;   /* sin(-2Pi 1/5) */
    const int32_t sin2 = -1262259218;   /* sin(-2Pi 2/5) */

    for (int i = 0; i < n; i++, x++, y += 5) {

        int32_t s14r = x[1*n].re + x[4*n].re, s14i = x[1*n].im + x[4*n].im;
        int32_t d14r = x[1*n].re - x[4*n].re, d14i = x[1*n].im - x[4*n].im;
        int32_t s23r = x[2*n].re + x[3*n].re, s23i = x[2*n].im + x[3*n].im;
        int32_t d23r = x[2*n].re - x[3*n].re, d23i = x[2*n].im - x[3*n].im;

        int32_t x0r = x[0].re, x0i = x[0].im;

        y[0].re = x0r + s14r + s23r;
        y[0].im = x0i + s14i + s23i;

        s14r = LC3FX_SHL(s14r, 1), s14i = LC3FX_SHL(s14i, 1);
        d14r = LC3FX_SHL(d14r, 1), d14i = LC3FX_SHL(d14i, 1);
        s23r = LC3FX_SHL(s23r, 1), s23i = LC3FX_SHL(s23i, 1);
        d23r = LC3FX_SHL(d23r, 1), d23i = LC3FX_SHL(d23i, 1);

        int32_t a1 = mulh(s14r, cos1), a2 = mulh(s14r, cos2);
        int32_t b1 = mulh(s23r, cos1), b2 = mulh(s23r, cos2);
        int32_t c1 = mulh(s14i, cos1), c2 = mulh(s14i, cos2);
        int32_t e1 = mulh(s23i, cos1), e2 = mulh(s23i, cos2);
        int32_t f1 = mulh(d14i, sin1), f2 = mulh(d14i, sin2);
        int32_t g1 = mulh(d23i, sin1), g2 = mulh(d23i, sin2);
        int32_t h1 = mulh(d14r, sin1), h2 = mulh(d14r, sin2);
        int32_t k1 = mulh(d23r, sin1), k2 = mulh(d23r, sin2);

        y[1].re = x0r + a1 - f1 + b2 - g2;
        y[1].im = x0i + c1 + h1 + e2 + k2;

        y[2].re = x0r + a2 - f2 + b1 + g1;
        y[2].im = x0i + c2 + h2 + e1 - k1;

        y[3].re = x0r + a2 + f2 + b1 - g1;
        y[3].im = x0i + c2 - h2 + e1 + k1;

        y[4].re = x0r + a1 + f1 + b2 + g2;
        y[4].im = x0i + c1 - h1 + e2 - k2;
    }
}

/**
 * FFT Butterfly 3 Points
 * twiddles        Twiddles factors, determine size of transform
 * x, y            Input and output coefficients
 * n               Number of interleaved transforms
 */
LC3_HOT static void fft_bf3(
    const struct lc3_fft_bf3_twiddles *twiddles,
    const cplx_t *x, cplx_t *y, int n)
{
    int n3 = twiddles->n3;
    const cplx_t (*w0)[2] = twiddles->t;
    const cplx_t (*w1)[2] = w0 + n3, (*w2)[2] = w1 + n3;

    const cplx_t *x0 = x, *x1 = x0 + n*n3, *x2 = x1 + n*n3;
    cplx_t *y0 = y, *y1 = y0 + n3, *y2 = y1 + n3;

    for (int i = 0; i < n; i++, y0 += 3*n3, y1 += 3*n3, y2 += 3*n3)
        for (int j = 0; j < n3; j++, x0++, x1++, x2++) {

            int32_t x0r = x0->re, x0i = x0->im;
            int32_t x1r = LC3FX_SHL(x1->re, 1), x1i = LC3FX_SHL(x1->im, 1);
            int32_t x2r = LC3FX_SHL(x2->re, 1), x2i = LC3FX_SHL(x2->im, 1);

            y0[j].re = x0r + mulh(x1r, w0[j][0].re) - mulh(x1i, w0[j][0].im)
                           + mulh(x2r, w0[j][1].re) - mulh(x2i, w0[j][1].im);
            y0[j].im = x0i + mulh(x1i, w0[j][0].re) + mulh(x1r, w0[j][0].im)
                           + mulh(x2i, w0[j][1].re) + mulh(x2r, w0[j][1].im);

            y1[j].re = x0r + mulh(x1r, w1[j][0].re) - mulh(x1i, w1[j][0].im)
                           + mulh(x2r, w1[j][1].re) - mulh(x2i, w1[j][1].im);
            y1[j].im = x0i + mulh(x1i, w1[j][0].re) + mulh(x1r, w1[j][0].im)
                           + mulh(x2i, w1[j][1].re) + mulh(x2r, w1[j][1].im);

            y2[j].re = x0r + mulh(x1r, w2[j][0].re) - mulh(x1i, w2[j][0].im)
                           + mulh(x2r, w2[j][1].re) - mulh(x2i, w2[j][1].im);
            y2[j].im = x0i + mulh(x1i, w2[j][0].re) + mulh(x1r, w2[j][0].im)
                           + mulh(x2i, w2[j][1].re) + mulh(x2r, w2[j][1].im);
        }
}

/**
 * FFT Butterfly 2 Points
 * twiddles        Twiddles factors, determine size of transform
 * x, y            Input and output coefficients
 * n               Number of interleaved transforms
 */
LC3_HOT static void fft_bf2(
    const struct lc3_fft_bf2_twiddles *twiddles,
    const cplx_t *x, cplx_t *y, int n)
{
    int n2 = twiddles->n2;
    const cplx_t *w = twiddles->t;

    const cplx_t *x0 = x, *x1 = x0 + n*n2;
    cplx_t *y0 = y, *y1 = y0 + n2;

    for (int i = 0; i < n; i++, y0 += 2*n2, y1 += 2*n2) {

        for (int j = 0; j < n2; j++, x0++, x1++) {

            int32_t x0r = x0->re, x0i = x0->im;
            int32_t x1r = LC3FX_SHL(x1->re, 1), x1i = LC3FX_SHL(x1->im, 1);
            int32_t tr = mulh(x1r, w[j].re) - mulh(x1i, w[j].im);
            int32_t ti = mulh(x1i, w[j].re) + mulh(x1r, w[j].im);

            y0[j].re = x0r + tr;
            y0[j].im = x0i + ti;

            y1[j].re = x0r - tr;
            y1[j].im = x0i - ti;
        }
    }
}

/**
 * FFT 2 successive Butterflies 2 Points (radix 2^2), sharing loads/stores
 * twa, twb        Twiddles factors of the 2 stages
 * x, y            Input and output coefficients
 * n               Number of interleaved transforms of the first stage
 */
LC3_HOT static void fft_bf2x2(
    const struct lc3_fft_bf2_twiddles *twa,
    const struct lc3_fft_bf2_twiddles *twb,
    const cplx_t *x, cplx_t *y, int n)
{
    int n2a = twa->n2, n2b = twb->n2;
    const cplx_t *wa = twa->t, *wb = twb->t;
    int nb = n >> 1;

#define CMUL2(o_r, o_i, xr, xi, w) \
    do { int32_t _r = LC3FX_SHL(xr, 1), _i = LC3FX_SHL(xi, 1); \
         o_r = mulh(_r, (w).re) - mulh(_i, (w).im); \
         o_i = mulh(_i, (w).re) + mulh(_r, (w).im); } while (0)

    for (int i = 0; i < nb; i++) {
        const cplx_t *a0 = x + i*n2a, *a1 = a0 + n*n2a;
        const cplx_t *b0 = a0 + nb*n2a, *b1 = b0 + n*n2a;
        cplx_t *z0 = y + i * 2*n2b, *z1 = z0 + n2b;

        _Pragma("GCC unroll 2")
        for (int j = 0; j < n2a; j++) {
            int32_t tr, ti;

            /* --- First stage, transforms `i` and `i + n/2` --- */

            CMUL2(tr, ti, a1[j].re, a1[j].im, wa[j]);
            int32_t p0r = a0[j].re + tr, p0i = a0[j].im + ti;
            int32_t p1r = a0[j].re - tr, p1i = a0[j].im - ti;

            CMUL2(tr, ti, b1[j].re, b1[j].im, wa[j]);
            int32_t q0r = b0[j].re + tr, q0i = b0[j].im + ti;
            int32_t q1r = b0[j].re - tr, q1i = b0[j].im - ti;

            /* --- Second stage, coefficients `j` and `j + n2a` --- */

            CMUL2(tr, ti, q0r, q0i, wb[j]);
            z0[j].re = p0r + tr; z0[j].im = p0i + ti;
            z1[j].re = p0r - tr; z1[j].im = p0i - ti;

            CMUL2(tr, ti, q1r, q1i, wb[j + n2a]);
            z0[j + n2a].re = p1r + tr; z0[j + n2a].im = p1i + ti;
            z1[j + n2a].re = p1r - tr; z1[j + n2a].im = p1i - ti;
        }
    }

#undef CMUL2
}

/**
 * Perform FFT
 * x, y0, y1       Input, and 2 scratch buffers of size `n`
 * n               Number of points 30, 40, 60, 80, 90, 120, 160, 180, 240
 * return          The buffer `y0` or `y1` that hold the result
 *
 * Input `x` can be the same as the `y0` second scratch buffer
 */
/* Radix 3 and single radix 2 stages, used by some configurations only:
 * radix 3 for 7.5 ms (and LC3plus durations), 24 and 48 kHz; the single
 * radix 2 stage when the power of 2 is odd (8, 24 and 32 kHz, LC3plus) */

#ifndef LC3_DT_MASK
#define LC3_DT_MASK 0xf
#endif
#define LC3_FFT_RADIX3  ((LC3_DT_MASK & 0x7) || (LC3_SR_MASK & 0x14))
#define LC3_FFT_RADIX2  ((LC3_DT_MASK & 0x3) || (LC3_SR_MASK & 0x0d))

static cplx_t *fft(const cplx_t *x, int n,
    cplx_t *y0, cplx_t *y1)
{
    cplx_t *y[2] = { y1, y0 };
    int i2, i3 = 0, is = 0;

    fft_5(x, y[is], n /= 5);

    if (LC3_FFT_RADIX3)
        for ( ; n & (n-1); i3++, is ^= 1)
            fft_bf3(lc3_fft_twiddles_bf3[i3], y[is], y[is ^ 1], n /= 3);

    for (i2 = 0; n > 2; i2 += 2, is ^= 1, n >>= 2)
        fft_bf2x2(lc3_fft_twiddles_bf2[i2][i3], lc3_fft_twiddles_bf2[i2+1][i3],
            y[is], y[is ^ 1], n >> 1);

    if (LC3_FFT_RADIX2 && n > 1) {
        fft_bf2(lc3_fft_twiddles_bf2[i2][i3], y[is], y[is ^ 1], n >> 1);
        is ^= 1;
    }

    return y[is];
}


/* ----------------------------------------------------------------------------
 *  MDCT processing
 * -------------------------------------------------------------------------- */

/**
 * Windowing of samples before MDCT
 * dt, sr          Duration and samplerate
 * x, d            Input current and delayed samples
 * y, d            Output windowed samples (Q14), and delayed ones
 * return          Sum of the magnitudes of the outputs, divided by 2^10
 */
LC3_HOT static uint32_t mdct_window(
    enum lc3_dt dt, enum lc3_srate sr,
    const int16_t *x, int16_t *d, int32_t *y)
{
    const int32_t *win = lc3_mdct_win[dt][sr];
    int ns = lc3_ns(dt, sr), nd = lc3_nd(dt, sr);

    const int32_t *w0 = win, *w1 = w0 + ns;
    const int32_t *w2 = w1, *w3 = w2 + nd;

    const int16_t *x0 = x + ns-nd, *x1 = x0;
    int32_t *y0 = y + ns/2, *y1 = y0;
    int16_t *d0 = d, *d1 = d + nd;

    uint32_t m = 0;
    int32_t v;

#define W(a, w) mulh(LC3FX_SHL((int32_t)(a), 16), (w))
#define ACC(v)  (m += (uint32_t)((v) ^ ((v) >> 31)) >> 10)

    while (x1 > x) {
        v = W(*d0, *(w0++)) - W(*(--x1), *(--w1));  *(--y0) = v; ACC(v);
        v = W(*(d0++) = *(x0++), *(w2++));          *(y1++) = v; ACC(v);
    }

    for (x1 += ns; x0 < x1; ) {
        v = W(*d0, *(w0++)) - W(*(--d1), *(--w1));  *(--y0) = v; ACC(v);
        v = W(*(d0++) = *(x0++), *(w2++)) + W(*d1 = *(--x1), *(--w3));
        *(y1++) = v; ACC(v);
    }

#undef W
#undef ACC

    return m;
}

/**
 * Pre-rotate MDCT coefficients of N/2 points, before FFT N/4 points FFT
 * def             Size and twiddles factors
 * x, y            Input and output coefficients
 * sh              Normalization shift of the input (left if positive)
 *
 * `x` and y` can be the same buffer
 */
LC3_HOT static void mdct_pre_fft(const struct lc3_mdct_rot_def *def,
    const int32_t *x, cplx_t *y, int sh)
{
    int n4 = def->n4;

    const int32_t *x0 = x, *x1 = x0 + 2*n4;
    const cplx_t *w0 = def->w, *w1 = w0 + n4;
    cplx_t *y0 = y, *y1 = y0 + n4;

    int lsh = sh > 0 ? sh : 0, rsh = sh < 0 ? -sh : 0;

    while (x0 < x1) {
        cplx_t u, uw = *(w0++);
        int32_t a = LC3FX_SHL(*(--x1), lsh) >> rsh, b = LC3FX_SHL(*x0, lsh) >> rsh;
        u.re = - mulh(a, uw.re) + mulh(b, uw.im);
        u.im =   mulh(b, uw.re) + mulh(a, uw.im);
        x0++;

        cplx_t v, vw = *(--w1);
        a = LC3FX_SHL(*(--x1), lsh) >> rsh, b = LC3FX_SHL(*x0, lsh) >> rsh;
        v.re = - mulh(a, vw.im) + mulh(b, vw.re);
        v.im = - mulh(b, vw.im) - mulh(a, vw.re);
        x0++;

        *(y0++) = u;
        *(--y1) = v;
    }
}

/**
 * Post-rotate FFT N/4 points coefficients, resulting MDCT N points
 * def             Size and twiddles factors
 * x, y            Input and output coefficients
 *
 * `x` and y` can be the same buffer
 */
LC3_HOT static void mdct_post_fft(const struct lc3_mdct_rot_def *def,
    const cplx_t *x, int32_t *y)
{
    int n4 = def->n4, n8 = n4 >> 1;

    const cplx_t *w0 = def->w + n8, *w1 = w0 - 1;
    const cplx_t *x0 = x + n8, *x1 = x0 - 1;

    int32_t *y0 = y + n4, *y1 = y0;

    for ( ; y1 > y; x0++, x1--, w0++, w1--) {

        int32_t u0 = mulh(x0->im, w0->im) + mulh(x0->re, w0->re);
        int32_t u1 = mulh(x1->re, w1->im) - mulh(x1->im, w1->re);

        int32_t v0 = mulh(x0->re, w0->im) - mulh(x0->im, w0->re);
        int32_t v1 = mulh(x1->im, w1->im) + mulh(x1->re, w1->re);

        *(y0++) = u0;  *(y0++) = u1;
        *(--y1) = v0;  *(--y1) = v1;
    }
}

/**
 * Pre-rotate IMDCT coefficients of N points, before FFT N/4 points FFT
 * def             Size and twiddles factors
 * x, y            Input and output coefficients
 *
 * The real and imaginary parts of `y` are swapped,
 * to operate on FFT instead of IFFT
 */
LC3_HOT static void imdct_pre_fft(const struct lc3_mdct_rot_def *def,
    const int32_t *x, cplx_t *y, int sh)
{
    int n4 = def->n4;
    int lsh = sh > 0 ? sh : 0, rsh = sh < 0 ? -sh : 0;

    const int32_t *x0 = x, *x1 = x0 + 2*n4;

    const cplx_t *w0 = def->w, *w1 = w0 + n4;
    cplx_t *y0 = y, *y1 = y0 + n4;

    while (x0 < x1) {
        int32_t u0 = LC3FX_SHL(*(x0++), lsh) >> rsh, u1 = LC3FX_SHL(*(--x1), lsh) >> rsh;
        int32_t v0 = LC3FX_SHL(*(x0++), lsh) >> rsh, v1 = LC3FX_SHL(*(--x1), lsh) >> rsh;
        cplx_t uw = *(w0++), vw = *(--w1);

        (y0  )->re = - mulh(u0, uw.re) - mulh(u1, uw.im);
        (y0++)->im = - mulh(u1, uw.re) + mulh(u0, uw.im);

        (--y1)->re = - mulh(v1, vw.re) - mulh(v0, vw.im);
        (  y1)->im = - mulh(v0, vw.re) + mulh(v1, vw.im);
    }
}

/**
 * Post-rotate FFT N/4 points coefficients, resulting IMDCT N points
 * def             Size and twiddles factors
 * x, y            Input and output coefficients
 * sh              Shift (left if positive) applied to the outputs, saturated
 *
 * `x` and y` can be the same buffer
 * The real and imaginary parts of `x` are swapped,
 * to operate on FFT instead of IFFT
 */
LC3_HOT static void imdct_post_fft(const struct lc3_mdct_rot_def *def,
    const cplx_t *x, int32_t *y, int sh)
{
    int n4 = def->n4;

    const cplx_t *w0 = def->w, *w1 = w0 + n4;
    const cplx_t *x0 = x, *x1 = x0 + n4;
    int32_t *y0 = y, *y1 = y0 + 2*n4;

        if (sh <= 0) {
        int rsh = sh > -31 ? -sh : 31;

        while (x0 < x1) {
            cplx_t uz = *(x0++), vz = *(--x1);
            cplx_t uw = *(w0++), vw = *(--w1);

            *(y0++) = (mulh(uz.re, uw.im) - mulh(uz.im, uw.re)) >> rsh;
            *(--y1) = (mulh(uz.re, uw.re) + mulh(uz.im, uw.im)) >> rsh;
            *(--y1) = (mulh(vz.re, vw.im) - mulh(vz.im, vw.re)) >> rsh;
            *(y0++) = (mulh(vz.re, vw.re) + mulh(vz.im, vw.im)) >> rsh;
        }

        return;
    }

    int lsh = sh < 31 ? sh : 31;
    int32_t lim = INT32_MAX >> lsh;

#define SAT_SHL(v) ( (v) > lim ? INT32_MAX : (v) < -lim ? -INT32_MAX : LC3FX_SHL(v, lsh) )

    while (x0 < x1) {
        cplx_t uz = *(x0++), vz = *(--x1);
        cplx_t uw = *(w0++), vw = *(--w1);

        int32_t a = mulh(uz.re, uw.im) - mulh(uz.im, uw.re);
        int32_t b = mulh(uz.re, uw.re) + mulh(uz.im, uw.im);
        int32_t c = mulh(vz.re, vw.im) - mulh(vz.im, vw.re);
        int32_t e = mulh(vz.re, vw.re) + mulh(vz.im, vw.im);

        *(y0++) = SAT_SHL(a);
        *(--y1) = SAT_SHL(b);
        *(--y1) = SAT_SHL(c);
        *(y0++) = SAT_SHL(e);
    }

#undef SAT_SHL
}

/**
 * Apply windowing of samples
 * dt, sr          Duration and samplerate
 * x, d            Middle half of IMDCT coefficients (Q14) and delayed samples
 * y, d            Output samples and delayed ones (Q12)
 */
LC3_HOT static void imdct_window(
    enum lc3_dt dt, enum lc3_srate sr,
    const int32_t *x, int32_t *d, int32_t *y)
{
    /* The full MDCT coefficients is given by symmetry :
     *   T[   0 ..  n/4-1] = -half[n/4-1 .. 0    ]
     *   T[ n/4 ..  n/2-1] =  half[0     .. n/4-1]
     *   T[ n/2 .. 3n/4-1] =  half[n/4   .. n/2-1]
     *   T[3n/4 ..    n-1] =  half[n/2-1 .. n/4  ]  */

    const int32_t *win = lc3_mdct_win[dt][sr];
    int n4 = lc3_ns(dt, sr) >> 1, nd = lc3_nd(dt, sr);
    const int32_t *w2 = win, *w0 = w2 + 3*n4, *w1 = w0;

    const int32_t *x0 = d + nd-n4, *x1 = x0;
    int32_t *y0 = y + nd-n4, *y1 = y0, *y2 = d + nd, *y3 = d;

    _Pragma("GCC unroll 2")
    while (y0 > y) {
        *(--y0) = *(--x0) - mulh(*(x  ), *(w1++));
        *(y1++) = *(x1++) + mulh(*(x++), *(--w0));
    }

    while (y1 < y + nd) {
        *(y1++) = *(x1++) + mulh(*(x++), *(--w0));
    }

    while (y1 < y + 2*n4) {
        *(y1++) = mulh(*(x  ), *(--w0));
        *(--y2) = mulh(*(x++), *(w2++));
    }

    while (y2 > y3) {
        *(y3++) = mulh(*(x  ), *(--w0));
        *(--y2) = mulh(*(x++), *(w2++));
    }
}


/* ----------------------------------------------------------------------------
 *  Interface
 * -------------------------------------------------------------------------- */

/**
 * Forward MDCT transformation
 */
int lc3_mdct_forward(enum lc3_dt dt, enum lc3_srate sr,
    const int16_t *x, int16_t *d, int32_t *y, int32_t *buf)
{
    const struct lc3_mdct_rot_def *rot = lc3_mdct_rot[dt][sr];
    int ns = lc3_ns(dt, sr);

    /* --- Windowing, L1-norm normalized to 2^30 ---
     * The pre-rotation (twiddles below 0.57, halving) brings
     * the L1-norm of the FFT input below 2^29 */

    uint32_t s = mdct_window(dt, sr, x, d, buf);
    int sh = 30 - (lc3fx_nbits(s + ns) + 10);

    /* --- Pre-rotation, FFT and post-rotation --- */

    cplx_t *z = (cplx_t *)buf;

    mdct_pre_fft(rot, buf, z, sh);
    z = fft(z, ns/2, z, (cplx_t *)y);
    mdct_post_fft(rot, z, y);

    /* The windowed samples are in Q(14+sh),
     * the pre- and post-rotations halve the values */

    return -(14 + sh) + 2;
}

/**
 * Inverse MDCT transformation
 */
int32_t *lc3_mdct_inverse(enum lc3_dt dt, enum lc3_srate sr,
    const int32_t *x, int e, int32_t *d, int32_t *buf0, int32_t *buf1)
{
    const struct lc3_mdct_rot_def *rot = lc3_mdct_rot[dt][sr];
    int ns = lc3_ns(dt, sr);

    /* --- L1-norm normalization to 2^30 --- */

    uint32_t s = ns;
    _Pragma("GCC unroll 4")
    for (int i = 0; i < ns; i++)
        s += (uint32_t)(x[i] ^ (x[i] >> 31)) >> 10;

    int sh = 30 - (lc3fx_nbits(s) + 10);

    /* --- Pre-rotation, FFT and post-rotation --- */

    cplx_t *z = (cplx_t *)buf0;

    imdct_pre_fft(rot, x, z, sh);
    z = fft(z, ns/2, z, (cplx_t *)buf1);

    /* Samples = z * 2^(e - sh + 2), converted to Q14 */

    imdct_post_fft(rot, z, (int32_t *)z, e - sh + 2 + 14);

    int32_t *y = (int32_t *)z == buf0 ? buf1 : buf0;
    imdct_window(dt, sr, (int32_t *)z, d, y);

    return y;
}
