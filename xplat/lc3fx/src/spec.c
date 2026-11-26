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
 * Spectral quantization, fixed-point port of liblc3 spec.c (LC3 only,
 * the LC3-Plus high-resolution paths are removed).
 *
 * Encoder: the global gain estimation works on 4-coefficients energies
 * in dB Q16 (as liblc3 does), computed from 64 bits sums of squares. The
 * quantized coefficients are kept in Q15, so the rounding offsets (6/16)
 * and thresholds (10/16) of the reference are exact.
 *
 * Decoder: the decoded spectrum is returned as integers in Q4 (16 times
 * the quantized values). The residual offsets (5/16, 3/16), the LSB mode
 * and the noise level (8 - nf) / 16 are exact in this format; the global
 * gain is applied later, through the SNS synthesis.
 */

#include "spec.h"
#include "bits.h"
#include "tables.h"


/* ----------------------------------------------------------------------------
 *  Global Gain / Quantization
 * -------------------------------------------------------------------------- */

/**
 * Resolve quantized gain index offset
 * sr, nbytes      Samplerate and size of the frame
 * return          Gain index offset
 */
static int resolve_gain_offset(enum lc3_srate sr, int nbytes)
{
    int sr_ind = sr;

    int g_off = (nbytes * 8) / (10 * (1 + sr_ind));
    return LC3_MIN(255, 105 + 5*(1 + sr_ind) + LC3_MIN(g_off, 115));
}

/**
 * log2(10) / 28 * g, in Q16
 */
static inline int32_t gain_log2_q16(int g)
{
    return (int32_t)(((int64_t)g * 254778081) >> 15);
}

/**
 * Global Gain Estimation
 * dt, sr          Duration and samplerate of the frame
 * x, x_exp        Spectral coefficients, and exponent
 * nbytes          Size of the frame
 * nbits_budget    Number of bits available coding the spectrum
 * nbits_off       Offset on the available bits, temporarily smoothed (Q16)
 * g_off           Gain index offset
 * reset_off       Return True when the nbits_off must be reset
 * g_min           Return lower bound of quantized gain value
 * return          The quantized gain value
 */
LC3_HOT static int estimate_gain(
    enum lc3_dt dt, enum lc3_srate sr, const int32_t *x, int x_exp,
    int nbytes, int nbits_budget, int32_t nbits_off, int g_off,
    bool *reset_off, int *g_min)
{
    (void)nbytes;

    int n4 = lc3_ne(dt, sr) / 4;
    int32_t e[LC3_MAX_NE / 4];

        /* --- Energy (dB) by 4 MDCT blocks ---
     * The coefficients are taken on 16 bits (x >> k): the sums of 4
     * squares fit in 32 bits, and the dropped bits are below the finest
     * quantization step (quantized values are below 2^15).
     * dB = 10 log10(2) * log2(e), with e = sum (x >> k)^2 * 2^(2 (x_exp + k)),
     * floored at 1e-10 (-100 dB) */

    const int32_t k_db = 1616142483;        /* 10 log10(2), Q29 */
    const int32_t db_min = -100 * 65536;
    uint32_t x_max = 0, x_or = 0;

    _Pragma("GCC unroll 4")
    for (int i = 0; i < 4*n4; i++) {
        uint32_t m = (uint32_t)(x[i] ^ (x[i] >> 31));
        int32_t d = (int32_t)(x_max - m);         /* magnitudes below 2^31 */
        x_max -= d & (d >> 31);
        x_or |= m;
    }

    int k = lc3fx_nbits(x_or) - 15;
    k = k > 0 ? k : 0;

    int32_t l2_off = 2 * (x_exp + k) * 65536;

    for (int i = 0; i < n4; i++) {
        int32_t v0 = x[4*i + 0] >> k, v1 = x[4*i + 1] >> k;
        int32_t v2 = x[4*i + 2] >> k, v3 = x[4*i + 3] >> k;
        uint32_t s = (uint32_t)(v0 * v0) + (uint32_t)(v1 * v1) +
                     (uint32_t)(v2 * v2) + (uint32_t)(v3 * v3);

        int32_t db = db_min;
        if (s) {
            int32_t l2 = lc3fx_log2_q16_u32(s) + l2_off;
            int32_t v = lc3fx_mulh(LC3FX_SHL(l2, 3), k_db);
            db = v > db_min ? v : db_min;
        }
        e[i] = db;
    }

    /* --- Determine gain index --- */

    int nbits = nbits_budget + ((nbits_off + (1 << 15)) >> 16);
    int g_int = 255 - g_off;

    const int k_20_28 = 46811;   /* 20/28 in Q16 */
    const int k_2u7 = 176947;    /* 2.7   in Q16 */
    const int k_1u4 = 91750;     /* 1.4   in Q16 */

    /* The terms are positive: the sum stops as soon as it is above the
     * threshold, the decision is the same */

    const int v_thr = nbits * k_1u4;

    for (int i = 128, j, j0 = n4-1, j1 ; i > 0; i >>= 1) {
        int gn = (g_int - i) * k_20_28;
        int v = 0;

        for (j = j0; j >= 0 && e[j] < gn; j--);

        for (j1 = j; j >= 3 && v <= v_thr; ) {
            _Pragma("GCC unroll 4")
            for (int jj = 0; jj < 4; jj++, j--) {
                int e_diff = e[j] - gn;

                v += e_diff < 0 ? k_2u7 :
                     e_diff < 43 << 16 ?   e_diff + ( 7 << 16)
                                       : 2*e_diff - (36 << 16);
            }
        }

        for ( ; j >= 0 && v <= v_thr; j--) {
            int e_diff = e[j] - gn;

            v += e_diff < 0 ? k_2u7 :
                 e_diff < 43 << 16 ?   e_diff + ( 7 << 16)
                                   : 2*e_diff - (36 << 16);
        }

        if (v > v_thr)
            j0 = j1;
        else
            g_int = g_int - i;
    }

    /* --- Limit gain index ---
     * Such that 32767 * 10^(g_min / 28) > x_max, evaluated in log2 */

    const int32_t log2_lim = 983037;        /* log2(32767), Q16 */
    int32_t log2_xmax = x_max ?
        lc3fx_log2_q16_u32(x_max) + x_exp * 65536 : INT32_MIN / 2;

    *g_min = 255 - g_off;
    for (int i = 128 ; i > 0; i >>= 1)
        if (log2_lim + gain_log2_q16(*g_min - i) > log2_xmax)
            *g_min -= i;

    *reset_off = g_int < *g_min || x_max == 0;
    if (*reset_off)
        g_int = *g_min;

    return g_int;
}

/**
 * Global Gain Adjustment
 * dt, sr          Duration and samplerate of the frame
 * g_idx           The estimated quantized gain index
 * nbits           Computed number of bits coding the spectrum
 * nbits_budget    Number of bits available for coding the spectrum
 * g_idx_min       Minimum gain index value
 * return          Gain adjust value (-1 to 2)
 */
LC3_HOT static int adjust_gain(
    enum lc3_dt dt, enum lc3_srate sr,
    int g_idx, int nbits, int nbits_budget, int g_idx_min)
{
    (void)dt;

    /* --- Compute delta threshold --- */

    static const int16_t t_tab[LC3_NUM_SRATE][3] = {
        {  80,  500,  850 }, { 230, 1025, 1700 }, { 380, 1550, 2550 },
        { 530, 2075, 3400 }, { 680, 2600, 4250 },
        { 680, 2600, 4250 }, { 830, 3125, 5100 }
    };
    const int16_t *t = t_tab[sr];

    int delta, den = 48;

    if (nbits < t[0]) {
        delta = 3*(nbits + 48);

    } else if (nbits < t[1]) {
        int n0 = 3*(t[0] + 48), range = t[1] - t[0];
        delta = n0 * range + (nbits - t[0]) * (t[1] - n0);
        den *= range;

    } else {
        delta = LC3_MIN(nbits, t[2]);
    }

    delta = (delta + den/2) / den;

    /* --- Adjust gain --- */

    if (nbits < nbits_budget - (delta + 2))
        return -(g_idx > g_idx_min);

    if (nbits > nbits_budget)
        return (g_idx < 255) + (g_idx < 254 && nbits >= nbits_budget + delta);

    return 0;
}

/**
 * Spectrum quantization
 * dt, sr          Duration and samplerate of the frame
 * g_int           Quantization gain value
 * x, x_exp        Spectral coefficients and exponent, quantized Q15 as output
 * n               Return count of significants
 */
LC3_HOT static void quantize(
    enum lc3_dt dt, enum lc3_srate sr, int g_int,
    int32_t *x, int x_exp, int *n)
{
    int32_t g_mant;
    int g_exp = lc3fx_gain_pow(-g_int, &g_mant);

    /* xq = x * 2^x_exp * g_mant * 2^(g_exp - 30) * 2^15
     *    = (x * g_mant) >> r */

    int r = 15 - x_exp - g_exp;
    int ne = lc3_ne(dt, sr);

    if (r >= 32) {
        int s = r - 32;
        if (s > 31) {
            memset(x, 0, ne * sizeof(*x));
        } else {
            _Pragma("GCC unroll 4")
            for (int i = 0; i < ne; i++)
                x[i] = lc3fx_mulh(x[i], g_mant) >> s;
        }

        } else if (r > 0) {

        /* Left shift of the input within its headroom, and `mulh` */

        uint32_t m = 0;
        _Pragma("GCC unroll 4")
        for (int i = 0; i < ne; i++)
            m |= (uint32_t)(x[i] ^ (x[i] >> 31));

        int s1 = 31 - lc3fx_nbits(m), s2 = s1 - (32 - r);

        if (s2 >= 0) {
            _Pragma("GCC unroll 4")
            for (int i = 0; i < ne; i++)
                x[i] = lc3fx_mulh(LC3FX_SHL(x[i], s1), g_mant) >> s2;
        } else {
            for (int i = 0; i < ne; i++)
                x[i] = lc3fx_sat32(((int64_t)x[i] * g_mant) >> r);
        }

    } else {
        for (int i = 0; i < ne; i++)
            x[i] = lc3fx_sat32(LC3FX_SHL64((int64_t)x[i] * g_mant, LC3_MIN(-r, 31)));
    }

    /* --- Count of significants, magnitude above 10/16 --- */

    const int32_t xq_min = 10 << 11;

    /* The count drops the trailing pairs below the threshold:
     * scanned from the end */

    int nq = ne;
    for ( ; nq > 0; nq -= 2) {
        int32_t a = x[nq-2] < 0 ? -x[nq-2] : x[nq-2];
        int32_t b = x[nq-1] < 0 ? -x[nq-1] : x[nq-1];
        if ((a | b) >= xq_min)
            break;
    }
    *n = nq;
}


/* ----------------------------------------------------------------------------
 *  Spectrum coding
 * -------------------------------------------------------------------------- */

/**
 * Resolve High-bitrate and LSB modes according size of the frame
 * sr, nbytes      Samplerate and size of the frame
 * p_lsb_mode      True when LSB mode allowed, when not NULL
 * return          True when High-Rate mode enabled
 */
static bool resolve_modes(enum lc3_srate sr, int nbytes, bool *p_lsb_mode)
{
    int sr_ind = sr;

    if (p_lsb_mode)
      *p_lsb_mode = (nbytes >= 20 * (3 + sr_ind));

    return (nbytes > 20 * (1 + sr_ind));
}

/**
 * Quantized magnitude, rounding offset 6/16
 */
static inline uint32_t qmag(int32_t x)
{
    return ((uint32_t)(x < 0 ? -x : x) + (6 << 11)) >> 15;
}

/**
 * Bit consumption
 * dt, sr, nbytes  Duration, samplerate and size of the frame
 * x               Spectral quantized coefficients (Q15)
 * n               Count of significant coefficients, updated on truncation
 * nbits_budget    Truncate to stay in budget, when not zero
  * p_lsb_mode      Return True when LSB's are not AC coded, or NULL
 * p_raw           Return the bit consumption without the LSB's (Q11),
 *                 and the count of LSB's, when not NULL
 * return          The number of bits coding the spectrum
 */
LC3_HOT static int compute_nbits(
    enum lc3_dt dt, enum lc3_srate sr, int nbytes,
    const int32_t *x, int *n, int nbits_budget, bool *p_lsb_mode, int *p_raw)
{
    bool lsb_mode, high_rate = resolve_modes(sr, nbytes, &lsb_mode);
    int ne = lc3_ne(dt, sr);

    /* --- Loop on quantized coefficients --- */

    int nbits = 0, nbits_lsb = 0;
    uint8_t state = 0;

    int nbits_end = 0;
    int n_end = 0;

    nbits_budget = nbits_budget ? nbits_budget * 2048 : INT_MAX;

    for (int i = 0, h = 0; h < 2; h++) {
        const uint8_t (*lut_coeff)[4] = lc3_spectrum_lookup[high_rate][h];

        for ( ; i < LC3_MIN(*n, (ne + 2) >> (1 - h))
                && nbits <= nbits_budget; i += 2) {

            uint32_t a = qmag(x[i+0]);
            uint32_t b = qmag(x[i+1]);

            const uint8_t *lut = lut_coeff[state];

            /* --- Sign values --- */

            int s = (a != 0) + (b != 0);
            nbits += s * 2048;

            /* --- LSB values Reduce to 2*2 bits MSB values ---
             * Reduce to 2x2 bits MSB values. The LSB's pair are arithmetic
             * coded with an escape code followed by 1 bit for each values.
             * The LSB mode does not arthmetic code the first LSB,
             * add the sign of the LSB when one of pair was at value 1 */

            uint32_t m = (a | b) >> 2;
            unsigned k = 0;

            if (m) {

                if (lsb_mode) {
                    nbits += lc3_spectrum_bits[lut[k++]][16] - 2*2048;
                    nbits_lsb += 2 + (a == 1) + (b == 1);
                }

                for (m >>= lsb_mode; m; m >>= 1, k++)
                    nbits += lc3_spectrum_bits[lut[LC3_MIN(k, 3)]][16];

                nbits += k * 2*2048;
                a >>= k;
                b >>= k;

                k = LC3_MIN(k, 3);
            }

            /* --- MSB values --- */

            nbits += lc3_spectrum_bits[lut[k]][a + 4*b];

            /* --- Update state --- */

            if (s && nbits <= nbits_budget) {
                n_end = i + 2;
                nbits_end = nbits;
            }

            state = (state << 4) + (__builtin_expect(k > 1, 0) ? 12 + k : 1 + (a + b) * (k + 1));
        }
    }

    /* --- Return --- */

        *n = n_end;

    if (p_raw)
        p_raw[0] = nbits_end, p_raw[1] = nbits_lsb;

    if (p_lsb_mode)
        *p_lsb_mode = lsb_mode &&
            nbits_end + nbits_lsb * 2048 > nbits_budget;

    if (nbits_budget >= INT_MAX)
        nbits_end += nbits_lsb * 2048;

    return (nbits_end + 2047) / 2048;
}

/**
 * Put quantized spectrum
 * bits            Bitstream context
 * dt, sr, nbytes  Duration, samplerate and size of the frame
 * x               Spectral quantized coefficients (Q15)
 * nq, lsb_mode    Count of significants, and LSB discard indication
 */
LC3_HOT static void put_quantized(lc3_bits_t *bits,
    enum lc3_dt dt, enum lc3_srate sr, int nbytes,
    const int32_t *x, int nq, bool lsb_mode)
{
    bool high_rate = resolve_modes(sr, nbytes, NULL);
    int ne = lc3_ne(dt, sr);

    /* --- Loop on quantized coefficients --- */

    uint8_t state = 0;

    for (int i = 0, h = 0; h < 2; h++) {
        const uint8_t (*lut_coeff)[4] = lc3_spectrum_lookup[high_rate][h];

        for ( ; i < LC3_MIN(nq, (ne + 2) >> (1 - h)); i += 2) {

            uint32_t a = qmag(x[i+0]);
            uint32_t b = qmag(x[i+1]);

            const uint8_t *lut = lut_coeff[state];

            /* --- LSB values Reduce to 2*2 bits MSB values ---
             * Reduce to 2x2 bits MSB values. The LSB's pair are arithmetic
             * coded with an escape code and 1 bits for each values.
             * The LSB mode discard the first LSB (at this step) */

            uint32_t m = (a | b) >> 2;
            unsigned k = 0, shr = 0;

            if (__builtin_expect(m != 0, 0)) {

                /* Escapes: the LSB's of the pair, but the first in LSB
                 * mode (one symbol coding site for both) */

                unsigned l = lsb_mode, nesc = l;
                for (m >>= l; m; m >>= 1)
                    nesc++;

                for ( ; k < nesc; k++) {
                    if (k >= l) {
                        lc3_put_bit(bits, (a >> k) & 1);
                        lc3_put_bit(bits, (b >> k) & 1);
                    }
                    lc3_put_symbol(bits,
                        lc3_spectrum_models + lut[LC3_MIN(k, 3)], 16);
                }

                a >>= l;
                b >>= l;

                shr = k - l;
                k = LC3_MIN(k, 3);
            }

            /* --- Sign values --- */

            if (a) lc3_put_bit(bits, x[i+0] < 0);
            if (b) lc3_put_bit(bits, x[i+1] < 0);

            /* --- MSB values --- */

            a >>= shr;
            b >>= shr;

            lc3_put_symbol(bits, lc3_spectrum_models + lut[k], a + 4*b);

            /* --- Update state --- */

            state = (state << 4) + (__builtin_expect(k > 1, 0) ? 12 + k : 1 + (a + b) * (k + 1));
        }
    }
}

/**
 * Get quantized spectrum
 * bits            Bitstream context
 * dt, sr, nbytes  Duration, samplerate and size of the frame
 * nq, lsb_mode    Count of significants, and LSB discard indication
 * x               Return `nq` spectral quantized coefficients (Q4)
 * nf_seed         Return the noise factor seed associated
 * return          0: Ok  -1: Invalid bitstream data
 */
LC3_HOT static int get_quantized(lc3_bits_t *bits,
    enum lc3_dt dt, enum lc3_srate sr, int nbytes,
    int nq, bool lsb_mode, int32_t *x, uint16_t *nf_seed)
{
    bool high_rate = resolve_modes(sr, nbytes, NULL);
    int ne = lc3_ne(dt, sr);

    *nf_seed = 0;

    /* --- Loop on quantized coefficients --- */

    uint8_t state = 0;

    for (int i = 0, h = 0; h < 2; h++) {
        const uint8_t (*lut_coeff)[4] = lc3_spectrum_lookup[high_rate][h];

        for ( ; i < LC3_MIN(nq, (ne + 2) >> (1 - h)); i += 2) {

            const uint8_t *lut = lut_coeff[state];
            const int max_shl = 14;

            /* --- LSB values ---
             * Until the symbol read indicates the escape value 16,
             * read an LSB bit for each values.
             * The LSB mode discard the first LSB (at this step) */

            int u = 0, v = 0;
            int k = 0, shl = 0;
            unsigned s;

            /* Until the symbol is not the escape: the LSB's of the pair,
             * but the first in LSB mode. One symbol decoding site */

            for (;;) {
                s = lc3_get_symbol(bits, lc3_spectrum_models + lut[k]);
                if (__builtin_expect(s < 16, 1))
                    break;

                if (shl >= max_shl)
                    return -1;

                if (!lsb_mode || shl > 0) {
                    u |= lc3_get_bit(bits) << shl;
                    v |= lc3_get_bit(bits) << shl;
                }

                k += (k < 3);
                shl++;
            }

            /* --- MSB & sign values --- */

            int a = s % 4;
            int b = s / 4;

            u |= a << shl;
            v |= b << shl;

            x[i+0] = (u && lc3_get_bit(bits) ? -u : u) * 16;
            x[i+1] = (v && lc3_get_bit(bits) ? -v : v) * 16;

            *nf_seed = (*nf_seed + (u & 0x7fff) * (i  )
                                 + (v & 0x7fff) * (i+1)) & 0xffff;

            /* --- Update state --- */

            state = (state << 4) + (__builtin_expect(k > 1, 0) ? 12 + k : 1 + (a + b) * (k + 1));
        }
    }

    return 0;
}

/**
 * Put residual bits of quantization
 * bits            Bitstream context
 * nbits           Maximum number of bits to output
 * x, n            Spectral quantized (Q15), and count of significants
 */
LC3_HOT static void put_residual(lc3_bits_t *bits,
    int nbits, const int32_t *x, int n)
{
    const int32_t xq_lim = 10 << 11;

    for (int i = 0; i < n && nbits > 0; i++) {
        int32_t xr = x[i] < 0 ? -x[i] : x[i];

        if (xr < xq_lim)
            continue;

        bool b = ((xr & 0x7fff) < xq_lim) ^ (x[i] < 0);
        lc3_put_bit(bits, b);
        nbits--;
    }
}

/**
 * Get residual bits of quantization
 * bits            Bitstream context
 * nbits           Maximum number of bits to output
 * x, nq           Spectral quantized (Q4), and count of significants
 */
LC3_HOT static void get_residual(lc3_bits_t *bits,
    int nbits, int32_t *x, int n)
{
    const int xq_off_1 = 5;   /* 5/16 */
    const int xq_off_2 = 3;   /* 3/16 */

    for (int i = 0; i < n && nbits > 0; i++) {

        if (x[i] == 0)
          continue;

        if (lc3_get_bit(bits) == 0)
            x[i] -= x[i] < 0 ? xq_off_1 : xq_off_2;
        else
            x[i] += x[i] > 0 ? xq_off_1 : xq_off_2;

        nbits--;
    }
}

/**
 * Put LSB values of quantized spectrum values
 * bits            Bitstream context
 * nbits           Maximum number of bits to output
 * x, n            Spectral quantized (Q15), and count of significants
 */
LC3_HOT static void put_lsb(lc3_bits_t *bits,
    int nbits, const int32_t *x, int n)
{
    for (int i = 0; i < n && nbits > 0; i += 2) {
        uint32_t a = qmag(x[i+0]);
        uint32_t b = qmag(x[i+1]);

        if ((a | b) >> 2 == 0)
            continue;

        if (nbits-- > 0)
            lc3_put_bit(bits, a & 1);
        if (a == 1 && nbits-- > 0)
            lc3_put_bit(bits, x[i+0] < 0);

        if (nbits-- > 0)
            lc3_put_bit(bits, b & 1);
        if (b == 1 && nbits-- > 0)
            lc3_put_bit(bits, x[i+1] < 0);
    }
}

/**
 * Get LSB values of quantized spectrum values
 * bits            Bitstream context
 * nbits           Maximum number of bits to output
 * x, nq           Spectral quantized (Q4), and count of significants
 * nf_seed         Update the noise factor seed according
 */
LC3_HOT static void get_lsb(lc3_bits_t *bits,
    int nbits, int32_t *x, int nq, uint16_t *nf_seed)
{
    for (int i = 0; i < nq && nbits > 0; i += 2) {

        int32_t a = LC3_ABS(x[i]), b = LC3_ABS(x[i+1]);

        if (LC3_MAX(a, b) < 4 * 16)
            continue;

        if (nbits-- > 0 && lc3_get_bit(bits)) {
            if (a) {
                x[i] += x[i] < 0 ? -16 : 16;
                *nf_seed = (*nf_seed + i) & 0xffff;
            } else if (nbits-- > 0) {
                x[i] = lc3_get_bit(bits) ? -16 : 16;
                *nf_seed = (*nf_seed + i) & 0xffff;
            }
        }

        if (nbits-- > 0 && lc3_get_bit(bits)) {
            if (b) {
                x[i+1] += x[i+1] < 0 ? -16 : 16;
                *nf_seed = (*nf_seed + i+1) & 0xffff;
            } else if (nbits-- > 0) {
                x[i+1] = lc3_get_bit(bits) ? -16 : 16;
                *nf_seed = (*nf_seed + i+1) & 0xffff;
            }
        }
    }
}


/* ----------------------------------------------------------------------------
 *  Noise coding
 * -------------------------------------------------------------------------- */

/**
 * Estimate noise level
 * dt, bw          Duration and bandwidth of the frame
 * x, n            Spectral quantized (Q15), and count of significants
 * return          Noise factor (0 to 7)
 */
LC3_HOT static int estimate_noise(
    enum lc3_dt dt, enum lc3_bandwidth bw, const int32_t *x, int n)
{
    int bw_stop = lc3_ne(dt, (enum lc3_srate)LC3_MIN(bw, LC3_BANDWIDTH_FB));
    int w = 1 + (dt >= LC3_DT_7M5) + (dt>= LC3_DT_10M);

    const int32_t xq_lim = 10 << 11;

    int64_t sum = 0;
    int i, ns = 0, z = 0;

    for (i = 6 * (1 + dt) - w; i < LC3_MIN(n, bw_stop); i++) {
        z = (z + 1) & -(LC3_ABS(x[i]) < xq_lim);
        if (z > 2*w)
            sum += LC3_ABS(x[i - w]), ns++;
    }

    for ( ; i < bw_stop + w; i++)
        if (++z > 2*w)
            sum += LC3_ABS(x[i - w]), ns++;

    /* nf = 8 - (int)(16 * sum / ns + 0.5), with sum in Q15 */

    int nf = ns ? 8 - (int)((16 * sum + ((int64_t)ns << 14)) / ((int64_t)ns << 15)) : 8;

    return LC3_CLIP(nf, 0, 7);
}

/**
 * Noise filling
 * dt, bw          Duration and bandwidth of the frame
 * nf, nf_seed     The noise factor and pseudo-random seed
 * x, nq           Spectral quantized (Q4), and count of significants
 */
LC3_HOT static void fill_noise(enum lc3_dt dt, enum lc3_bandwidth bw,
    int nf, uint16_t nf_seed, int32_t *x, int nq)
{
    int bw_stop = lc3_ne(dt, (enum lc3_srate)LC3_MIN(bw, LC3_BANDWIDTH_FB));
    int w = 1 + (dt >= LC3_DT_7M5) + (dt>= LC3_DT_10M);

    int32_t s = 8 - nf;     /* (8 - nf) / 16 in Q4 */
    int i, z = 0;

    for (i = 6 * (1 + dt) - w; i < LC3_MIN(nq, bw_stop); i++) {
        z = x[i] ? 0 : z + 1;
        if (z > 2*w) {
            nf_seed = (13849 + nf_seed*31821) & 0xffff;
            x[i - w] = nf_seed & 0x8000 ? -s : s;
        }
    }

    for ( ; i < bw_stop + w; i++)
        if (++z > 2*w) {
            nf_seed = (13849 + nf_seed*31821) & 0xffff;
            x[i - w] = nf_seed & 0x8000 ? -s : s;
        }
}

/**
 * Put noise factor
 * bits            Bitstream context
 * nf              Noise factor (0 to 7)
 */
static void put_noise_factor(lc3_bits_t *bits, int nf)
{
    lc3_put_bits(bits, nf, 3);
}

/**
 * Get noise factor
 * bits            Bitstream context
 * return          Noise factor (0 to 7)
 */
static int get_noise_factor(lc3_bits_t *bits)
{
    return lc3_get_bits(bits, 3);
}


/* ----------------------------------------------------------------------------
 *  Encoding
 * -------------------------------------------------------------------------- */

/**
 * Bit consumption of the number of coded coefficients
 * dt, sr, nbytes  Duration, samplerate and size of the frame
 * return          Bit consumpution of the number of coded coefficients
 */
static int get_nbits_nq(enum lc3_dt dt, enum lc3_srate sr)
{
    int ne = lc3_ne(dt, sr);
    return 4 + (ne > 32) + (ne > 64) + (ne > 128) + (ne > 256) + (ne > 512);
}

/**
 * Bit consumption of the arithmetic coder
 * dt, sr, nbytes  Duration, samplerate and size of the frame
 * return          Bit consumption of bitstream data
 */
static int get_nbits_ac(enum lc3_dt dt, enum lc3_srate sr, int nbytes)
{
    return get_nbits_nq(dt, sr) + 3 + LC3_MIN((nbytes-1) / 160, 2);
}

/**
 * Spectrum analysis
 */
void lc3_spec_analyze(
    enum lc3_dt dt, enum lc3_srate sr, int nbytes,
    bool pitch, const lc3_tns_data_t *tns,
    struct lc3_spec_analysis *spec,
    int32_t *x, int x_exp, struct lc3_spec_side *side)
{
    bool reset_off;

    /* --- Bit budget --- */

    const int nbits_gain = 8;
    const int nbits_nf = 3;

    int nbits_budget = 8*nbytes - get_nbits_ac(dt, sr, nbytes) -
        lc3_bwdet_get_nbits(sr) - lc3_ltpf_get_nbits(pitch) -
        lc3_sns_get_nbits() - lc3_tns_get_nbits(tns) - nbits_gain - nbits_nf;

    /* --- Global gain --- */

    int32_t nbits_off = spec->nbits_off + spec->nbits_spare * 65536;
    nbits_off = LC3_CLIP(nbits_off, -40 * 65536, 40 * 65536);
    nbits_off = (4 * spec->nbits_off + nbits_off) / 5;   /* within +/- 2^24 */

    int g_off = resolve_gain_offset(sr, nbytes);

    int g_min, g_int = estimate_gain(dt, sr, x, x_exp,
        nbytes, nbits_budget, nbits_off, g_off, &reset_off, &g_min);
    LC3FX_MARK(8);

    /* --- Quantization --- */

    quantize(dt, sr, g_int, x, x_exp, &side->nq);

        int raw[2];
    int nbits = compute_nbits(dt, sr, nbytes, x, &side->nq, 0, NULL, raw);
    LC3FX_MARK(9);

    spec->nbits_off = reset_off ? 0 : nbits_off;
    spec->nbits_spare = reset_off ? 0 : nbits_budget - nbits;

    /* --- Adjust gain and requantize --- */

    int g_adj = adjust_gain(dt, sr,
        g_off + g_int, nbits, nbits_budget, g_off + g_min);

        if (g_adj)
        quantize(dt, sr, g_adj, x, -15, &side->nq);

    side->g_idx = g_int + g_adj + g_off;

    /* Without gain adjustment, and within the budget, the bounded
     * bit consumption gives the same result as the unbounded one */

    if (!g_adj && raw[0] <= nbits_budget * 2048) {
        bool lsb_mode;
        resolve_modes(sr, nbytes, &lsb_mode);
        side->lsb_mode = lsb_mode &&
            raw[0] + raw[1] * 2048 > nbits_budget * 2048;
    } else
        nbits = compute_nbits(dt, sr, nbytes,
            x, &side->nq, nbits_budget, &side->lsb_mode, NULL);
}

/**
 * Put spectral quantization side data
 */
void lc3_spec_put_side(lc3_bits_t *bits,
    enum lc3_dt dt, enum lc3_srate sr,
    const struct lc3_spec_side *side)
{
    int nbits_nq = get_nbits_nq(dt, sr);

    lc3_put_bits(bits, LC3_MAX(side->nq >> 1, 1) - 1, nbits_nq);
    lc3_put_bits(bits, side->lsb_mode, 1);
    lc3_put_bits(bits, side->g_idx, 8);
}

/**
 * Encode spectral coefficients
 */
void lc3_spec_encode(lc3_bits_t *bits,
    enum lc3_dt dt, enum lc3_srate sr, enum lc3_bandwidth bw,
    int nbytes, const lc3_spec_side_t *side, int32_t *x)
{
    bool lsb_mode = side->lsb_mode;
    int nq = side->nq;

    put_noise_factor(bits, estimate_noise(dt, bw, x, nq));

    put_quantized(bits, dt, sr, nbytes, x, nq, lsb_mode);

    int nbits_left = lc3_get_bits_left(bits);

    if (lsb_mode)
        put_lsb(bits, nbits_left, x, nq);
    else
        put_residual(bits, nbits_left, x, nq);
}


/* ----------------------------------------------------------------------------
 *  Decoding
 * -------------------------------------------------------------------------- */

/**
 * Get spectral quantization side data
 */
int lc3_spec_get_side(lc3_bits_t *bits,
    enum lc3_dt dt, enum lc3_srate sr, struct lc3_spec_side *side)
{
    int nbits_nq = get_nbits_nq(dt, sr);
    int ne = lc3_ne(dt, sr);

    side->nq = (lc3_get_bits(bits, nbits_nq) + 1) << 1;
    side->lsb_mode = lc3_get_bit(bits);
    side->g_idx = lc3_get_bits(bits, 8);

    return side->nq > ne ? (side->nq = ne), -1 : 0;
}

/**
 * Decode spectral coefficients
 */
int lc3_spec_decode(lc3_bits_t *bits,
    enum lc3_dt dt, enum lc3_srate sr, enum lc3_bandwidth bw,
    int nbytes, const lc3_spec_side_t *side, int32_t *x, int *g_int)
{
    bool lsb_mode = side->lsb_mode;
    int nq = side->nq;
    int ret = 0;

    int nf = get_noise_factor(bits);
    uint16_t nf_seed;

    if ((ret = get_quantized(bits, dt, sr, nbytes,
                    nq, lsb_mode, x, &nf_seed)) < 0)
        return ret;

    int nbits_left = lc3_get_bits_left(bits);

    if (lsb_mode)
        get_lsb(bits, nbits_left, x, nq, &nf_seed);
    else
        get_residual(bits, nbits_left, x, nq);

    *g_int = side->g_idx - resolve_gain_offset(sr, nbytes);

    int ne = lc3_ne(dt, sr);
    for (int i = nq; i < ne; i++)
        x[i] = 0;

    if (nq > 2 || x[0] || x[1] || side->g_idx > 0 || nf < 7)
        fill_noise(dt, bw, nf, nf_seed, x, nq);

    return 0;
}
