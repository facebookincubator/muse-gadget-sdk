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

#include "tns.h"
#include "tables.h"


/* ----------------------------------------------------------------------------
 *  Filter Coefficients
 * -------------------------------------------------------------------------- */

/**
 * Resolve LPC Weighting indication according bitrate
 * dt, nbytes      Duration and size of the frame
 * return          True when LPC Weighting enabled
 */
static bool resolve_lpc_weighting(enum lc3_dt dt, int nbytes)
{
    return nbytes * 8 < 120 * (int)(1 + dt);
}

/**
 * LPC Coefficients
 * dt, bw          Duration and bandwidth of the frame
 * maxorder        Maximum order of filter
 * x               Spectral coefficients (magnitude below 2^23)
 * r0, err         Return the prediction gains as `r0 / err` (Q29)
 * a, rc           Output LPC coefficients (Q24) and reflection ones (Q31)
 */
#ifndef LC3FX_TNS_RECIP
#define LC3FX_TNS_RECIP 0   /* 1: reciprocals in place of the 64 bits divisions (encoder only, not bit-exact) */
#endif

#if LC3FX_TNS_RECIP
/**
 * Reciprocal 2^61 / x of x in [2^30, 2^31[: one 32 bits division (16 bits
 * estimate) and one Newton step (about 30 bits), no 64 bits division
 */
static inline uint32_t recip_q61(uint32_t x)
{
    uint32_t y = (0xffffffffu / (x >> 15)) << 14;
    int64_t d = (int64_t)(((uint64_t)1 << 61) - (uint64_t)x * y);
    return y + (int32_t)(((int64_t)y * (int32_t)(d >> 15)) >> 46);
}

/**
 * v * 2^sh / e, for 0 < e < 2^32, rounded toward zero like the division
 * it replaces up to the last bits, saturated to +/- (2^31 - 1)
 */
static inline int32_t div_recip(int64_t v, int sh, uint32_t e)
{
    int z = lc3fx_clz(e) - 1;                   /* e << z in [2^30, 2^31[ */
    uint32_t en = z >= 0 ? e << z : e >> 1;
    uint32_t inv = recip_q61(en);

    /* v * 2^sh / e = v * inv * 2^(sh + z - 61), with v on 31 bits */

    int vs = 64 - lc3fx_clz64(v < 0 ? -(uint64_t)v : (uint64_t)v) - 31;
    vs = vs > 0 ? vs : 0;
    int64_t p = (int64_t)(int32_t)(v >> vs) * inv;
    int r = 61 - sh - z - vs;

    if (r <= 0)
        return p > 0 ? INT32_MAX : p < 0 ? -INT32_MAX : 0;
    if (r < 64) p >>= r; else p = p < 0 ? -1 : 0;
    return p > INT32_MAX ? INT32_MAX : p < -INT32_MAX ? -INT32_MAX : (int32_t)p;
}
#endif

LC3_HOT static void compute_lpc_coeffs(
    enum lc3_dt dt, enum lc3_bandwidth bw, int maxorder,
    const int32_t *x, int32_t *r0, int32_t *err,
    int32_t (*a)[9], int32_t (*rc)[8])
{
    static const int sub_7m5_nb[]   = {  9,  26,  43,  60 };
    static const int sub_7m5_wb[]   = {  9,  46,  83, 120 };
    static const int sub_7m5_sswb[] = {  9,  66, 123, 180 };
    static const int sub_7m5_swb[]  = {  9,  46,  82, 120, 159, 200, 240 };
    static const int sub_7m5_fb[]   = {  9,  56, 103, 150, 200, 250, 300 };

    static const int sub_10m_nb[]   = { 12,  34,  57,  80 };
    static const int sub_10m_wb[]   = { 12,  61, 110, 160 };
    static const int sub_10m_sswb[] = { 12,  88, 164, 240 };
    static const int sub_10m_swb[]  = { 12,  61, 110, 160, 213, 266, 320 };
    static const int sub_10m_fb[]   = { 12,  74, 137, 200, 266, 333, 400 };

    /* Lag window, Q31 */

    static const int32_t lag_window[] = {
        2147483647, 2143248866, 2130594561, 2109669863, 2080720080,
        2044081896, 2000176863, 1949503329, 1892627025 };

    static const int * const sub_tab[LC3_NUM_DT][LC3_NUM_BANDWIDTH] = {
        [LC3_DT_7M5] = {
            sub_7m5_nb, sub_7m5_wb, sub_7m5_sswb, sub_7m5_swb,
            sub_7m5_fb                                        },
        [LC3_DT_10M] = {
            sub_10m_nb, sub_10m_wb, sub_10m_sswb, sub_10m_swb,
            sub_10m_fb                                        },
    };

    const int *sub = sub_tab[dt][bw];

    /* --- Normalized autocorrelation (Q29) --- */

    int nfilters = 1 + (dt >= LC3_DT_5M && bw >= LC3_BANDWIDTH_SWB);
    int nsubdivisions = 2 + (dt >= LC3_DT_7M5);

    const int32_t *xs, *xe = x + *sub;
    int32_t r[2][9];

    for (int f = 0; f < nfilters; f++) {
        int64_t c[9][3];
        bool zero = false;

        for (int s = 0; s < nsubdivisions; s++) {
            xs = xe, xe = x + *(++sub);

            /* The subdivision is normalized on 13 bits. A flat spectrum (an
             * isolated impulse) of 76 coefficients can bring the energy
             * (lag 0, summed on 64 bits) up to 2^32: from 2^31, it is
             * normalized again on 12 bits. The energy then bounds every lag
             * and partial sum within 32 bits (the unsigned sums of the
             * dropped pass may wrap) */

            int16_t t[80 + 15];
            int n = xe - xs;

            uint32_t m = 0;
            for (int i = 0; i < n; i++)
                m |= (uint32_t)(xs[i] ^ (xs[i] >> 31));

            int sh = lc3fx_nbits(m) - 13;
        normalize:
            if (sh > 0)
                for (int i = 0; i < n; i++) t[i] = xs[i] >> sh;
            else
                for (int i = 0; i < n; i++) t[i] = LC3FX_SHL(xs[i], -sh);

#if !defined(LC3FX_TNS_AC_BLOCK) || LC3FX_TNS_AC_BLOCK
            /* Lags by blocks of 5 over a zero padded copy: each `t[i]` is
             * loaded once for 5 products, and every lag runs over the same
             * `n` terms (the padding adds zeros: same sums, bit-exact) */

            /* Padded to a multiple of 5 (and 4) terms, and 10 more for
             * the lags: the products of the padding are zeros */

            int n5 = n + (5 - n % 5) % 5, n4 = (n + 3) & ~3;
            for (int i = n; i < n5 + 10; i++)
                t[i] = 0;

            /* Lags 0 to 4 */
            {
                int64_t a0 = 0;
                uint32_t a1 = 0, a2 = 0, a3 = 0, a4 = 0;
                int32_t w0 = t[0], w1 = t[1], w2 = t[2], w3 = t[3], w4, x;
                int i = 0;

                /* Unrolled by the window length: the window registers
                 * rotate by renaming, no moves */
#define AC5(A, B, C, D, E) \
                x = t[i]; E = t[i+4]; \
                a0 += x * A; a1 += x * B; a2 += x * C; \
                a3 += x * D; a4 += x * E; i++;

                while (i < n5) {
                    AC5(w0, w1, w2, w3, w4)
                    AC5(w1, w2, w3, w4, w0)
                    AC5(w2, w3, w4, w0, w1)
                    AC5(w3, w4, w0, w1, w2)
                    AC5(w4, w0, w1, w2, w3)
                }
#undef AC5

                int64_t a[5] = { a0, (int32_t)a1, (int32_t)a2,
                                     (int32_t)a3, (int32_t)a4 };
                for (int j = 0; j < 5 && j <= maxorder; j++)
                    c[j][s] = a[j];
            }

            /* Lags 5 to 8, by a window of 4 */

            if (maxorder > 4) {
                const int16_t *q = t + 5;
                uint32_t a0 = 0, a1 = 0, a2 = 0, a3 = 0;
                int32_t w0 = q[0], w1 = q[1], w2 = q[2], w3, x;
                int i = 0;

#define AC4(A, B, C, D) \
                x = t[i]; D = q[i+3]; \
                a0 += x * A; a1 += x * B; a2 += x * C; a3 += x * D; i++;

                while (i < n4) {
                    AC4(w0, w1, w2, w3)
                    AC4(w1, w2, w3, w0)
                    AC4(w2, w3, w0, w1)
                    AC4(w3, w0, w1, w2)
                }
#undef AC4

                c[5][s] = (int32_t)a0, c[6][s] = (int32_t)a1;
                c[7][s] = (int32_t)a2, c[8][s] = (int32_t)a3;
            }
#else
                        /* Lags by pairs, sharing the loads */

            for (int k = 0; k <= maxorder; k += 2) {
                int64_t v0 = 0;
                uint32_t v1 = 0;
                int i, nk = n - k - 1;
                int32_t tk = t[k];

                _Pragma("GCC unroll 4")
                for (i = 0; i < nk; i++) {
                    int32_t ti = t[i], tk1 = t[i+k+1];
                    v0 += ti * tk;
                    v1 += ti * tk1;
                    tk = tk1;
                }
                v0 += t[i] * tk;

                c[k][s] = v0;
                if (k < maxorder)
                    c[k+1][s] = (int32_t)v1;
            }
#endif

            if (c[0][s] > INT32_MAX) {
                sh++;
                goto normalize;
            }

            zero |= c[0][s] == 0;
        }

        r[f][0] = nsubdivisions << 29;
        for (int k = 1; k <= maxorder; k++)
            r[f][k] = 0;

        if (zero)
            continue;

        for (int s = 0; s < nsubdivisions; s++) {

            /* Normalize the energy in [2^30, 2^31[, and divide by
             * multiplication with the inverse (Q61) */

            int sh = 64 - lc3fx_clz64(c[0][s]) - 31;
            int64_t e = sh >= 0 ? c[0][s] >> sh : LC3FX_SHL64(c[0][s], -sh);
#if LC3FX_TNS_RECIP
            int64_t inv = recip_q61((uint32_t)e);
#else
            int64_t inv = ((int64_t)1 << 61) / e;
#endif

            for (int k = 1; k <= maxorder; k++) {
                int64_t ck = sh >= 0 ? c[k][s] >> sh : LC3FX_SHL64(c[k][s], -sh);
                r[f][k] += (int32_t)((ck * inv) >> 32);
            }
        }

        for (int k = 1; k <= maxorder; k++)
            r[f][k] = lc3fx_mul31(r[f][k], lag_window[k]);
    }

    /* --- Levinson-Durbin recursion --- */

    for (int f = 0; f < nfilters; f++) {
        int32_t *a0 = a[f], a1[9];

        /* The prediction error stays in [1, r0], r0 <= 3 2^29: each step
         * removes `e k^2` with k^2 < 1 */

        uint32_t e = r[f][0];

        r0[f] = r[f][0];
        a0[0] = 1 << 24;

        for (int k = 1; k <= maxorder; k++) {
            int64_t num = LC3FX_SHL64(r[f][k], 24);
            for (int i = 1; i < k; i++)
                num += (int64_t)a0[i] * r[f][k-i];

#if LC3FX_TNS_RECIP
            int32_t k31 = e > 0 ? -div_recip(num, 7, e) : 0;
#else
            int64_t q = e > 0 ? -(LC3FX_SHL64(num, 7) / (int64_t)e) : 0;
            int32_t k31 = q > INT32_MAX ? INT32_MAX :
                          q < -INT32_MAX ? -INT32_MAX : (int32_t)q;
#endif

            e -= (uint32_t)(((uint64_t)(uint32_t)lc3fx_mul31(k31, k31) * e) >> 31);

            for (int i = 1; i < k; i++)
                a1[i] = a0[i] + lc3fx_mul31(k31, a0[k-i]);
            for (int i = 1; i < k; i++)
                a0[i] = a1[i];

            a0[k] = k31 >> 7;
            rc[f][k-1] = k31;
        }

        err[f] = (int32_t)e;
    }
}

/**
 * LPC Weighting
 * r0, err         Prediction gain as `r0 / err`
 * a               LPC coefficients (Q24), weighted as output
 */
LC3_HOT static void lpc_weighting(int32_t r0, int32_t err, int32_t *a)
{
    /* gamma = 1 - (1 - 0.85) * (2 - pred_gain) / (2 - 1.5)
     *       = 0.4 + 0.3 * pred_gain */

    int32_t gamma = 858993459 + (int32_t)(((int64_t)644245094 * r0) / err);
    int32_t g = gamma;

    for (int i = 1; i < 9; i++) {
        a[i] = lc3fx_mul31(a[i], g);
        g = lc3fx_mul31(g, gamma);
    }
}

/**
 * Quotient (x 2^31) / e truncated toward zero, saturated to +/- (2^31 - 1),
 * for 1 <= e <= 2^31, given `inv` = floor(2^62 / e): the same result as the
 * 64 bits division, with multiplications (one correction step at most)
 */
static inline int32_t div31_inv(int32_t x, uint32_t e, uint64_t inv)
{
    uint32_t m = x < 0 ? -(uint32_t)x : (uint32_t)x;

    if (m >= e)
        return x < 0 ? -INT32_MAX : INT32_MAX;

    /* m < e: m inv < 2^62, and the estimate is floor(m 2^31 / e) or
     * one less */

    uint64_t p = (uint64_t)m * (uint32_t)inv +
                (((uint64_t)m * (uint32_t)(inv >> 32)) << 32);
    uint32_t q = (uint32_t)(p >> 31);
    uint64_t r = ((uint64_t)m << 31) - (uint64_t)q * e;
    q += (r >= e);

    return x < 0 ? -(int32_t)q : (int32_t)q;
}

/**
 * LPC reflection
 * a, maxorder     LPC coefficients (Q24), and maximum order (4 or 8)
 * rc              Output reflection coefficients (Q31)
 */
LC3_HOT static void lpc_reflection(
    const int32_t *a, int maxorder, int32_t *rc)
{
    int32_t b[2][7], *b0, *b1;
    int64_t e, v;

#define CLIP31(v) ( (v) > INT32_MAX ? INT32_MAX : \
                    (v) < -INT32_MAX ? -INT32_MAX : (int32_t)(v) )

    v = LC3FX_SHL64(a[maxorder], 7);
    rc[maxorder-1] = CLIP31(v);
    e = ((int64_t)1 << 31) - lc3fx_mul31(rc[maxorder-1], rc[maxorder-1]);
    e = e > 0 ? e : 1;
#if !LC3FX_TNS_RECIP
    uint64_t inv = ((uint64_t)1 << 62) / (uint64_t)e;  /* one division by `e` */
#endif

    b1 = b[1];
    for (int i = 0; i < maxorder-1; i++) {
#if LC3FX_TNS_RECIP
        b1[i] = div_recip((int32_t)(a[1+i] -
            lc3fx_mul31(rc[maxorder-1], a[(maxorder-1)-i])), 31, (uint32_t)e);
#else
        b1[i] = div31_inv(a[1+i] -
            lc3fx_mul31(rc[maxorder-1], a[(maxorder-1)-i]), (uint32_t)e, inv);
#endif
    }

    for (int k = maxorder-2; k > 0; k--) {
        b0 = b1, b1 = b[k & 1];

        v = LC3FX_SHL64(b0[k], 7);
        rc[k] = CLIP31(v);
        e = ((int64_t)1 << 31) - lc3fx_mul31(rc[k], rc[k]);
        e = e > 0 ? e : 1;
#if !LC3FX_TNS_RECIP
        inv = ((uint64_t)1 << 62) / (uint64_t)e;
#endif

        for (int i = 0; i < k; i++) {
#if LC3FX_TNS_RECIP
            b1[i] = div_recip((int32_t)(b0[i] - lc3fx_mul31(rc[k], b0[k-1-i])), 31, (uint32_t)e);
#else
            b1[i] = div31_inv(b0[i] - lc3fx_mul31(rc[k], b0[k-1-i]),
                (uint32_t)e, inv);
#endif
        }
    }

    v = LC3FX_SHL64(b1[0], 7);
    rc[0] = CLIP31(v);

#undef CLIP31
}

/**
 * Quantization of RC coefficients
 * rc, maxorder    Refelection coefficients (Q31), and maximum order (4 or 8)
 * order           Return order of coefficients
 * rc_i            Return quantized coefficients
 */
static void quantize_rc(const int32_t *rc, int maxorder, int *order, int *rc_q)
{
    /* Quantization table, sin(delta * (i + 0.5)), delta = Pi / 17,
     * rounded to fixed point Q15 value (LC3-Plus HR requirements). */

    static const int32_t q_thr[] = {
        0x0bcf, 0x2307, 0x390e, 0x4d23, 0x5e98, 0x6cd4, 0x775b, 0x7dd2,
    };

    *order = maxorder;

    for (int i = 0; i < maxorder; i++) {
        int32_t rc_m = (rc[i] < 0 ? -rc[i] : rc[i]) >> 16;

        rc_q[i] = 4 * (rc_m >= q_thr[4]);
        for (int j = 0; j < 4 && rc_m >= q_thr[rc_q[i]]; j++, rc_q[i]++);

        if (rc[i] < 0)
            rc_q[i] = -rc_q[i];

        *order = rc_q[i] != 0 ? maxorder : *order - 1;
    }
}

/**
 * Unquantization of RC coefficients
 * rc_q, order     Quantized coefficients, and order
 * rc              Return refelection coefficients (Q31)
 */
static void unquantize_rc(const int *rc_q, int order, int32_t rc[8])
{
    /* Quantization table, sin(delta * i), delta = Pi / 17,
     * rounded to fixed point Q15 value (LC3-Plus HR requirements). */

    static const int32_t q_inv[] = {
        0x0000, 0x1785, 0x2e3d, 0x4362,
        0x563c, 0x6625, 0x7295, 0x7b1d, 0x7f74,
    };

    int i;

    for (i = 0; i < order; i++) {
        int32_t rc_m = q_inv[LC3_ABS(rc_q[i])] << 16;
        rc[i] = rc_q[i] < 0 ? -rc_m : rc_m;
    }
}


/* ----------------------------------------------------------------------------
 *  Filtering
 * -------------------------------------------------------------------------- */

/**
 * Forward filtering
 * dt, bw          Duration and bandwidth of the frame
 * rc_order, rc    Order of coefficients, and coefficients (Q31)
 * x               Spectral coefficients, filtered as output
 */
LC3_HOT static void forward_filtering(
    enum lc3_dt dt, enum lc3_bandwidth bw,
    const int rc_order[2], int32_t (* const rc)[8], int32_t *x)
{
    int nfilters = 1 + (dt >= LC3_DT_5M && bw >= LC3_BANDWIDTH_SWB);
    int nf = lc3_ne(dt, (enum lc3_srate)LC3_MIN(bw, LC3_BANDWIDTH_FB))
                >> (nfilters - 1);
    int i0, ie = 3*(1 + dt);

    int32_t s[8] = { 0 };

    for (int f = 0; f < nfilters; f++) {

        i0 = ie;
        ie = nf * (1 + f);

        if (!rc_order[f])
            continue;

        for (int i = i0; i < ie; i++) {
            int32_t xi = x[i];
            int32_t s0, s1 = xi;

            _Pragma("GCC unroll 8")
            for (int k = 0; k < rc_order[f]; k++) {
                s0 = s[k];
                s[k] = s1;

                s1  = LC3FX_SHL(lc3fx_mulh(rc[f][k], xi), 1) + s0;
                xi += LC3FX_SHL(lc3fx_mulh(rc[f][k], s0), 1);
            }

            x[i] = xi;
        }
    }
}

/**
 * Inverse filtering with saturation
 * order, rc       Order of coefficients, and coefficients (Q31)
 * s               Filter states, updated
 * x, n            Spectral coefficients, filtered as output, and count
 */
__attribute__((noinline))
static void inverse_filtering_sat(
    int order, const int32_t *rc, int32_t *s, int32_t *x, int n)
{
    for (int i = 0; i < n; i++) {
        int32_t xi = x[i];

        for (int k = order-1; k >= 0; k--) {
            int32_t t = LC3FX_SHL(lc3fx_mulh(s[k], rc[k]), 1);
            if (__builtin_sub_overflow(xi, t, &xi))
                xi = t < 0 ? INT32_MAX : INT32_MIN;

            t = LC3FX_SHL(lc3fx_mulh(rc[k], xi), 1);
            if (__builtin_add_overflow(s[k], t, &s[k+1]))
                s[k+1] = t < 0 ? INT32_MIN : INT32_MAX;
        }
        s[0] = xi;
        x[i] = xi;
    }
}

/**
 * Inverse filtering
 * dt, bw          Duration and bandwidth of the frame
 * rc_order, rc    Order of coefficients, and unquantized coefficients (Q31)
 * gb              Bound of log2(G), G the product of (1 + |k|) over the stages
 * x               Spectral coefficients (magnitude below 2^22), filtered as output
 *
 * A stage grows a value by at most (1 + |k|): every state and partial sum
 * stays below 2^22 + 2 G (M + 2), M the largest of the last 8 outputs.
 * While the outputs stay within 2^(29 - gb), the filter cannot overflow;
 * past it, the rest of the spectrum is filtered with saturation.
 */
LC3_HOT static void inverse_filtering(
    enum lc3_dt dt, enum lc3_bandwidth bw,
    const int rc_order[2], int32_t (* const rc)[8], int gb, int32_t *x)
{
    int nfilters = 1 + (dt >= LC3_DT_5M && bw >= LC3_BANDWIDTH_SWB);
    int nf = lc3_ne(dt, (enum lc3_srate)LC3_MIN(bw, LC3_BANDWIDTH_FB))
                >> (nfilters - 1);
    int i0, ie = 3*(1 + dt);

    int32_t s[9] = { 0 };
    int32_t lim = (1 << (29 - gb)) - 2;
    bool sat = false;

    for (int f = 0; f < nfilters; f++) {

        i0 = ie;
        ie = nf * (1 + f);

        if (!rc_order[f])
            continue;

        /* The coefficients of order above `rc_order` are zero:
         * their states do not feed back, and are not computed */

        int order = rc_order[f];
        const int32_t *rcf = rc[f];
        int i = i0;

        for ( ; i < ie && !sat; i++) {
            int32_t xi = x[i];

            _Pragma("GCC unroll 2")
            for (int k = order-1; k >= 0; k--) {
                xi -= LC3FX_SHL(lc3fx_mulh(s[k], rcf[k]), 1);
                s[k+1] = s[k] + LC3FX_SHL(lc3fx_mulh(rcf[k], xi), 1);
            }
            s[0] = xi;
            x[i] = xi;

            sat = (uint32_t)xi + (uint32_t)lim > 2 * (uint32_t)lim;
        }

        if (i < ie)
            inverse_filtering_sat(order, rcf, s, x + i, ie - i);

        for (int k = order; k <= 8; k++)
            s[k] = 0;
    }
}


/* ----------------------------------------------------------------------------
 *  Interface
 * -------------------------------------------------------------------------- */

/**
 * TNS analysis
 */
void lc3_tns_analyze(enum lc3_dt dt, enum lc3_bandwidth bw,
    bool nn_flag, int nbytes, struct lc3_tns_data *data, int32_t *x)
{
    /* Processing steps :
     * - Determine the LPC (Linear Predictive Coding) Coefficients
     * - Check is the filtering is disabled
     * - The coefficients are weighted on low bitrates and predicition gain
     * - Convert to reflection coefficients and quantize
     * - Finally filter the spectral coefficients */

    int32_t r0[2], err[2], a[2][9];
    int32_t rc[2][8];

    data->lpc_weighting = resolve_lpc_weighting(dt, nbytes);
    data->nfilters = 1 + (dt >= LC3_DT_5M && bw >= LC3_BANDWIDTH_SWB);
    int maxorder = dt <= LC3_DT_5M ? 4 : 8;

    compute_lpc_coeffs(dt, bw, maxorder, x, r0, err, a, rc);

    for (int f = 0; f < data->nfilters; f++) {

        data->rc_order[f] = 0;

        /* Prediction gain `r0 / err` above 1.5 */

        if (nn_flag || 2 * (int64_t)r0[f] <= 3 * (int64_t)err[f])
            continue;

        /* Weighting, when the prediction gain is below 2.
         * Otherwise the reflection coefficients of the recursion are kept */

        if (data->lpc_weighting && r0[f] < 2 * (int64_t)err[f]) {
            lpc_weighting(r0[f], err[f], a[f]);
            lpc_reflection(a[f], maxorder, rc[f]);
        }

        quantize_rc(rc[f], maxorder, &data->rc_order[f], data->rc[f]);
        unquantize_rc(data->rc[f], data->rc_order[f], rc[f]);
    }

    LC3FX_MARK(6);
    forward_filtering(dt, bw, data->rc_order, rc, x);
}

/**
 * TNS analysis bypass: signal "no filter" (valid bitstream, decoder unchanged)
 */
void lc3_tns_bypass(enum lc3_dt dt, enum lc3_bandwidth bw, int nbytes, struct lc3_tns_data *data)
{
    data->lpc_weighting = resolve_lpc_weighting(dt, nbytes);
    data->nfilters = 1 + (dt >= LC3_DT_5M && bw >= LC3_BANDWIDTH_SWB);
    for (int f = 0; f < data->nfilters; f++)
        data->rc_order[f] = 0;
}

/**
 * TNS synthesis
 */
void lc3_tns_synthesize(enum lc3_dt dt, enum lc3_bandwidth bw,
    const struct lc3_tns_data *data, int32_t *x)
{
    /* log2(1 + |k|) in Q4, rounded up, by quantization index; the states
     * carry from a filter to the next: the gains of both are counted */

    static const uint8_t lg2[9] = { 0, 4, 8, 10, 12, 14, 15, 16, 16 };

    int32_t rc[2][8] = { 0 };
    int gb = 0;

    for (int f = 0; f < data->nfilters; f++)
        if (data->rc_order[f]) {
            unquantize_rc(data->rc[f], data->rc_order[f], rc[f]);
            for (int k = 0; k < data->rc_order[f]; k++)
                gb += lg2[LC3_ABS(data->rc[f][k])];
        }

    inverse_filtering(dt, bw, data->rc_order, rc, (gb + 15) >> 4, x);
}

/**
 * Bit consumption of bitstream data
 */
int lc3_tns_get_nbits(const struct lc3_tns_data *data)
{
    int nbits = 0;

    for (int f = 0; f < data->nfilters; f++) {

        int nbits_2048 = 2048;
        int rc_order = data->rc_order[f];

        nbits_2048 += rc_order > 0 ? lc3_tns_order_bits
            [data->lpc_weighting][rc_order-1] : 0;

        for (int i = 0; i < rc_order; i++)
            nbits_2048 += lc3_tns_coeffs_bits[i][8 + data->rc[f][i]];

        nbits += (nbits_2048 + (1 << 11) - 1) >> 11;
    }

    return nbits;
}

/**
 * Put bitstream data
 */
void lc3_tns_put_data(lc3_bits_t *bits, const struct lc3_tns_data *data)
{
    for (int f = 0; f < data->nfilters; f++) {
        int rc_order = data->rc_order[f];

        lc3_put_bits(bits, rc_order > 0, 1);
        if (rc_order <= 0)
            continue;

        lc3_put_symbol_ool(bits,
            lc3_tns_order_models + data->lpc_weighting, rc_order-1);

        for (int i = 0; i < rc_order; i++)
            lc3_put_symbol_ool(bits,
                lc3_tns_coeffs_models + i, 8 + data->rc[f][i]);
    }
}

/**
 * Get bitstream data
 */
int lc3_tns_get_data(lc3_bits_t *bits,
    enum lc3_dt dt, enum lc3_bandwidth bw, int nbytes, lc3_tns_data_t *data)
{
    data->nfilters = 1 + (dt >= LC3_DT_5M && bw >= LC3_BANDWIDTH_SWB);
    data->lpc_weighting = resolve_lpc_weighting(dt, nbytes);

    for (int f = 0; f < data->nfilters; f++) {

        data->rc_order[f] = lc3_get_bit(bits);
        if (!data->rc_order[f])
            continue;

        data->rc_order[f] += lc3_get_symbol_ool(bits,
            lc3_tns_order_models + data->lpc_weighting);
        if (dt <= LC3_DT_5M && data->rc_order[f] > 4)
            return -1;

        for (int i = 0; i < data->rc_order[f]; i++)
            data->rc[f][i] = (int)lc3_get_symbol_ool(bits,
                lc3_tns_coeffs_models + i) - 8;
    }

    return 0;
}
