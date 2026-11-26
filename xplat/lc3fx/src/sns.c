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
 * Spectral Noise Shaping, fixed-point port of liblc3 sns.c
 *
 * - Analysis works in the log2 domain (Q16), from band energies given on
 *   a common block exponent, which cancels out with the mean removal.
 * - Quantized scale factors are computed in Q27 (normative path, shared
 *   by the encoder and the decoder): codebooks Q27, DCT-16 matrix Q31,
 *   normalized PVQ vectors Q30, gains Q12 (exact).
 * - The shaping gains 2^(-/+scf) are mantissa / exponent pairs; the shaped
 *   spectrum is written on a common block exponent, chosen from the band
 *   peaks so that the loudest band uses the requested number of bits.
 */

#include "sns.h"
#include "tables.h"

#define mulh lc3fx_mulh


/* ----------------------------------------------------------------------------
 *  DCT-16
 * -------------------------------------------------------------------------- */

/**
 * Matrix of DCT-16 coefficients, Q31
 *
 * M[n][k] = 2f cos( Pi k (2n + 1) / 2N )
 *
 *   k = [0..N-1], n = [0..N-1], N = 16
 *   f = sqrt(1/4N) for k=0, sqrt(1/2N) otherwise
 */
static const int32_t dct16_m[16][16] = {
    {   536870912,   755594127,   744661346,   726557069,   701455650,   669598830,   631293408,   586908284,
        536870912,   481663181,   421816770,   357908032,   290552444,   220398678,   148122351,    74419526 },
    {   536870912,   726557069,   631293408,   481663181,   290552444,    74419526,  -148122351,  -357908032,
       -536870912,  -669598830,  -744661346,  -755594127,  -701455650,  -586908284,  -421816770,  -220398678 },
    {   536870912,   669598830,   421816770,    74419526,  -290552444,  -586908284,  -744661346,  -726557069,
       -536870912,  -220398678,   148122351,   481663181,   701455650,   755594127,   631293408,   357908032 },
    {   536870912,   586908284,   148122351,  -357908032,  -701455650,  -726557069,  -421816770,    74419526,
        536870912,   755594127,   631293408,   220398678,  -290552444,  -669598830,  -744661346,  -481663181 },
    {   536870912,   481663181,  -148122351,  -669598830,  -701455650,  -220398678,   421816770,   755594127,
        536870912,   -74419526,  -631293408,  -726557069,  -290552444,   357908032,   744661346,   586908284 },
    {   536870912,   357908032,  -421816770,  -755594127,  -290552444,   481663181,   744661346,   220398678,
       -536870912,  -726557069,  -148122351,   586908284,   701455650,    74419526,  -631293408,  -669598830 },
    {   536870912,   220398678,  -631293408,  -586908284,   290552444,   755594127,   148122351,  -669598830,
       -536870912,   357908032,   744661346,    74419526,  -701455650,  -481663181,   421816770,   726557069 },
    {   536870912,    74419526,  -744661346,  -220398678,   701455650,   357908032,  -631293408,  -481663181,
        536870912,   586908284,  -421816770,  -669598830,   290552444,   726557069,  -148122351,  -755594127 },
    {   536870912,   -74419526,  -744661346,   220398678,   701455650,  -357908032,  -631293408,   481663181,
        536870912,  -586908284,  -421816770,   669598830,   290552444,  -726557069,  -148122351,   755594127 },
    {   536870912,  -220398678,  -631293408,   586908284,   290552444,  -755594127,   148122351,   669598830,
       -536870912,  -357908032,   744661346,   -74419526,  -701455650,   481663181,   421816770,  -726557069 },
    {   536870912,  -357908032,  -421816770,   755594127,  -290552444,  -481663181,   744661346,  -220398678,
       -536870912,   726557069,  -148122351,  -586908284,   701455650,   -74419526,  -631293408,   669598830 },
    {   536870912,  -481663181,  -148122351,   669598830,  -701455650,   220398678,   421816770,  -755594127,
        536870912,    74419526,  -631293408,   726557069,  -290552444,  -357908032,   744661346,  -586908284 },
    {   536870912,  -586908284,   148122351,   357908032,  -701455650,   726557069,  -421816770,   -74419526,
        536870912,  -755594127,   631293408,  -220398678,  -290552444,   669598830,  -744661346,   481663181 },
    {   536870912,  -669598830,   421816770,   -74419526,  -290552444,   586908284,  -744661346,   726557069,
       -536870912,   220398678,   148122351,  -481663181,   701455650,  -755594127,   631293408,  -357908032 },
    {   536870912,  -726557069,   631293408,  -481663181,   290552444,   -74419526,  -148122351,   357908032,
       -536870912,   669598830,  -744661346,   755594127,  -701455650,   586908284,  -421816770,   220398678 },
    {   536870912,  -755594127,   744661346,  -726557069,   701455650,  -669598830,   631293408,  -586908284,
        536870912,  -481663181,   421816770,  -357908032,   290552444,  -220398678,   148122351,   -74419526 },
};

/**
 * Forward DCT-16 transformation
 * x, y            Input and output 16 values (y = DCT(x) / 2)
 */
LC3_HOT static void dct16_forward(const int32_t *x, int32_t *y)
{
        for (int i = 0, j; i < 16; i++) {
        int32_t v = 0;
        _Pragma("GCC unroll 16")
        for (j = 0; j < 16; j++)
            v += mulh(x[j], dct16_m[j][i]);
        y[i] = v;
    }
}

/**
 * Inverse DCT-16 transformation
 * x, y            Input and output 16 values (y = IDCT(x) / 2)
 */
LC3_HOT static void dct16_inverse(const int32_t *x, int32_t *y)
{
    /* The input is a pulse configuration (at most 10 non-zero values):
     * accumulate only the non-zero inputs */

    for (int i = 0; i < 16; i++)
        y[i] = 0;

    for (int j = 0; j < 16; j++) {
        int32_t xj = x[j];
        if (!xj)
            continue;

        _Pragma("GCC unroll 16")
        for (int i = 0; i < 16; i++)
            y[i] += mulh(xj, dct16_m[i][j]);
    }
}


/* ----------------------------------------------------------------------------
 *  Scale factors
 * -------------------------------------------------------------------------- */

/**
 * Scale factors
 * dt, sr          Duration and samplerate of the frame
 * nbytes          Size in bytes of the frame
 * eb, e_exp       Energy estimation per bands, and exponent
 * att             1: Attack detected  0: Otherwise
  * scf             Output 16 scale factors, Q27
 * scratch         Scratch buffer of 128 words
 */
LC3_HOT static void compute_scale_factors(
    enum lc3_dt dt, enum lc3_srate sr, int nbytes,
    const uint32_t *eb, int e_exp, bool att, int32_t *scf, int32_t *scratch)
{
    (void)nbytes;

    /* Pre-emphasis gain Ge[b] = 10 ^ (b * g_tilt / 630), b = [0..63]
     * given by the ratio 10 ^ (g_tilt / 630) in Q30,
     * and the log2 step g_tilt / 630 * log2(10) in Q20 */

    static const uint32_t ge_ratio[LC3_SRATE_48K + 1] = {
        1130113576, 1146756732, 1163644992, 1180781965, 1198171313 };

    static const int32_t ge_log2[LC3_SRATE_48K + 1] = {
        77407, 99523, 121639, 143755, 165871 };

        uint32_t *e = (uint32_t *)scratch;
    int32_t *l = scratch + LC3_MAX_BANDS;

    /* --- Copy and padding --- */

    int nb = lc3_num_bands[dt][sr];
    int n4 = nb < 32 ? 32 % nb : 0;
    int n2 = nb < 32 ? nb - n4 : LC3_MAX_BANDS - nb;

    for (int i4 = 0; i4 < n4; i4++)
        e[4*i4 + 0] = e[4*i4 + 1] =
        e[4*i4 + 2] = e[4*i4 + 3] = eb[i4];

    for (int i2 = n4; i2 < n4+n2; i2++)
        e[2*(n4+i2) + 0] = e[2*(n4+i2) + 1] = eb[i2];

    memcpy(e + 4*n4 + 2*n2, eb + n4 + n2, (nb - n4 - n2) * sizeof(*e));

    /* --- Smoothing, pre-emphasis and logarithm --- */

    uint32_t ge = 1 << 16;
    uint64_t e_sum = 0;

    for (int i = 0; i < LC3_MAX_BANDS; i++) {
        uint32_t e0 = e[i > 0 ? i-1 : 0], e1 = e[i];
        uint32_t es = i < LC3_MAX_BANDS-1 ?
            (e0 >> 2) + (e1 >> 1) + (e[i+1] >> 2) :
            (e0 >> 2) + e1 - (e1 >> 2);

        l[i] = lc3fx_log2_q16_u32(es) + ((i * ge_log2[sr]) >> 4);
        e_sum += (uint64_t)(es >> 6) * ge;
        ge = (uint32_t)(((uint64_t)ge * ge_ratio[sr]) >> 30);
    }

    /* Noise floor: e_sum * 1e-4 / 64, and 2^-32 absolute */

    int32_t floor_rel = lc3fx_log2_q16_u64(e_sum) + (6 - 16) * 65536 - 1264040;
    int32_t floor_abs = (-32 - e_exp) * 65536;
    int32_t floor = floor_rel > floor_abs ? floor_rel : floor_abs;

    for (int i = 0; i < LC3_MAX_BANDS; i++)
        l[i] = (l[i] > floor ? l[i] : floor) >> 1;

    /* --- Grouping & scaling --- */

    const int32_t k_12 = 357913941;  /* 1/12 in Q32 */
    int32_t s[16], scf_sum;

    s[0] = mulh((l[0] + l[4]) + 2 * (l[0] + l[3]) + 3 * (l[1] + l[2]), k_12);
    scf_sum = s[0];

    for (int i = 1; i < 15; i++) {
        s[i] = mulh(      (l[4*i-1] + l[4*i+4]) +
                      2 * (l[4*i  ] + l[4*i+3]) +
                      3 * (l[4*i+1] + l[4*i+2]), k_12);
        scf_sum += s[i];
    }

    s[15] = mulh((l[59] + l[63]) + 2 * (l[60] + l[63]) + 3 * (l[61] + l[62]), k_12);
    scf_sum += s[15];

    const int32_t k_0u85 = 1825361101;  /* 0.85 in Q31 */
    for (int i = 0; i < 16; i++)
        s[i] = mulh(LC3FX_SHL(s[i] - (scf_sum >> 4), 1), k_0u85);

    /* --- Attack handling --- */

    if (att) {
        const int32_t k_3 = 1431655765, k_5 = 858993459;  /* 1/3, 1/5 Q32 */

        int32_t s0, s1 = s[0], s2 = s[1], s3 = s[2], s4 = s[3];
        int32_t sn = s1 + s2;

        s[0] = mulh(sn += s3, k_3);
        s[1] = (sn += s4) >> 2;
        scf_sum = s[0] + s[1];

        for (int i = 2; i < 14; i++, sn -= s0) {
            s0 = s1, s1 = s2, s2 = s3, s3 = s4, s4 = s[i+2];
            s[i] = mulh(sn += s4, k_5);
            scf_sum += s[i];
        }

        s[14] = sn >> 2;
        s[15] = mulh(sn -= s1, k_3);
        scf_sum += s[14] + s[15];

        for (int i = 0; i < 16; i++) {
            int32_t v = s[i] - (scf_sum >> 4);
            s[i] = dt == LC3_DT_7M5 ? mulh(v, 1288490189) : v >> 1;
        }
    }

    /* --- Q16 to Q27 --- */

    for (int i = 0; i < 16; i++) {
        int32_t v = s[i];
        v = v > (15 << 16) ? 15 << 16 : v < -(15 << 16) ? -(15 << 16) : v;
        scf[i] = LC3FX_SHL(v, 11);
    }
}

/**
 * Codebooks
 * scf             Input 16 scale factors, Q27
 * lf/hfcb_idx     Output the low and high frequency codebooks index
 */
LC3_HOT static void resolve_codebooks(
    const int32_t *scf, int *lfcb_idx, int *hfcb_idx)
{
    uint32_t dlfcb_max = 0, dhfcb_max = 0;
    *lfcb_idx = *hfcb_idx = 0;

        /* Scale factors within +/-15, codebooks within +/-5: the 8 squares
     * of differences in Q10 fit in 32 bits. Partial distance search:
     * a candidate is dropped as soon as its partial sum is not better */

    int32_t sl[8], sh[8];
    for (int i = 0; i < 8; i++)
        sl[i] = scf[i] >> 17, sh[i] = scf[8+i] >> 17;

    dlfcb_max = dhfcb_max = UINT32_MAX;

    for (int icb = 0; icb < 32; icb++) {
        const int32_t *lfcb = lc3_sns_lfcb[icb];
        const int32_t *hfcb = lc3_sns_hfcb[icb];
        uint32_t dlfcb = 0, dhfcb = 0;
        int i;

        _Pragma("GCC unroll 8")
        for (i = 0; i < 8 && dlfcb < dlfcb_max; i++) {
            int32_t d = sl[i] - (lfcb[i] >> 17);         /* Q10 */
            dlfcb += (uint32_t)(d * d);
        }

        if (i == 8 && dlfcb < dlfcb_max)
            *lfcb_idx = icb, dlfcb_max = dlfcb;

        _Pragma("GCC unroll 8")
        for (i = 0; i < 8 && dhfcb < dhfcb_max; i++) {
            int32_t d = sh[i] - (hfcb[i] >> 17);
            dhfcb += (uint32_t)(d * d);
        }

        if (i == 8 && dhfcb < dhfcb_max)
            *hfcb_idx = icb, dhfcb_max = dhfcb;
    }
}

/**
 * 1 / sqrt(n), n = [0..101], Q30
 */
static const int32_t inv_sqrt[102] = {
             0, 1073741824,  759250125,  619925131,  536870912,  480191942,
     438353264,  405836263,  379625062,  357913941,  339546978,  323745341,
     309962566,  297802400,  286969573,  277238947,  268435456,  260420644,
     253083375,  246333269,  240095971,  234309675,  228922526,  223890647,
     219176632,  214748365,  210578097,  206641710,  202918131,  199388851,
     196037539,  192849725,  189812531,  186914460,  184145204,  181495494,
     178956971,  176522068,  174183925,  171936296,  169773489,  167690300,
     165681960,  163744093,  161872671,  160063981,  158314595,  156621342,
     154981283,  153391689,  151850025,  150353929,  148901200,  147489782,
     146117755,  144783318,  143484787,  142220579,  140989209,  139789279,
     138619473,  137478553,  136365348,  135278754,  134217728,  133181282,
     132168482,  131178442,  130210322,  129263325,  128336695,  127429710,
     126541687,  125671975,  124819952,  123985026,  123166634,  122364237,
     121577321,  120805393,  120047985,  119304647,  118574948,  117858477,
     117154838,  116463653,  115784559,  115117207,  114461263,  113816406,
     113182326,  112558727,  111945324,  111341840,  110748013,  110163587,
     109588316,  109021964,  108464304,  107915114,  107374182,  106841305
};

/**
 * Unit energy normalize pulse configuration
 * c               Pulse configuration
 * cn              Normalized pulse configuration, Q30
 */
LC3_HOT static void normalize(const int *c, int32_t *cn)
{
    int c2_sum = 0;
    for (int i = 0; i < 16; i++)
        c2_sum += c[i] * c[i];

    int32_t c_norm = inv_sqrt[c2_sum < 101 ? c2_sum : 101];

    for (int i = 0; i < 16; i++)
        cn[i] = c[i] * c_norm;
}

/**
 * Sub-procedure of `quantize()`, add unit pulse
 * x, y, n         Transformed residual (Q15), and vector of pulses with length
 * start, end      Current number of pulses, limit to reach
 * corr, energy    Correlation (x,y) and y energy, updated at output
 */
LC3_HOT static void add_pulse(const int32_t *x, int *y, int n,
    int start, int end, int32_t *corr, int *energy)
{
    /* The candidates without pulse share the same energy `energy + 1`:
     * among them the best is the largest `x` (corr + x >= 0), first in
     * order on ties. The others (at most the count of pulses) are compared
     * on the ratio. The result is the first index of the best ratio,
     * as with a scan of all the candidates */

    for (int k = start; k < end; k++) {
        int32_t xz = -1;
        int iz = -1, nbest = -1;
        int64_t best_c2 = 0;
        int best_e = 1;

        for (int i = 0; i < n; i++) {
            if (y[i] == 0) {
                if (x[i] > xz)
                    xz = x[i], iz = i;
                continue;
            }

            int64_t c2 = (int64_t)(*corr + x[i]) * (*corr + x[i]);
            int e = *energy + 2*y[i] + 1;

            if (nbest < 0 || c2 * best_e > e * best_c2)
                best_c2 = c2, best_e = e, nbest = i;
        }

        if (iz >= 0) {
            int64_t c2 = (int64_t)(*corr + xz) * (*corr + xz);
            int e = *energy + 1;
            int64_t l = c2 * best_e, r = e * best_c2;

            if (nbest < 0 || l > r || (l == r && iz < nbest))
                nbest = iz;
        }

        *corr += x[nbest];
        *energy += 2*y[nbest] + 1;
        y[nbest]++;
    }
}

/**
 * Quantization of codebooks residual
 * scf             Input 16 scale factors (Q27)
 * lf/hfcb_idx     Codebooks index
 * c               Output 4 pulse configurations candidates
 * shape/gain_idx  Output selected shape/gain indexes
 */
LC3_HOT static void quantize(const int32_t *scf, int lfcb_idx, int hfcb_idx,
    int (*c)[16], int *shape_idx, int *gain_idx)
{
    /* --- Residual --- */

    const int32_t *lfcb = lc3_sns_lfcb[lfcb_idx];
    const int32_t *hfcb = lc3_sns_hfcb[hfcb_idx];
    int32_t r[16], x[16];

    for (int i = 0; i < 8; i++) {
        r[  i] = (scf[  i] >> 3) - (lfcb[i] >> 3);      /* Q24 */
        r[8+i] = (scf[8+i] >> 3) - (hfcb[i] >> 3);
    }

    dct16_forward(r, x);                                /* Q23 */

    /* --- Shape 3 candidate ---
     * Project to or below pyramid N = 16, K = 6,
     * then add unit pulses until you reach K = 6, over N = 16 */

    int32_t xm[16];
    int32_t xm_sum = 0;

    for (int i = 0; i < 16; i++) {
        xm[i] = (x[i] < 0 ? -x[i] : x[i]) >> 8;         /* Q15 */
        xm_sum += xm[i];
    }

    int32_t corr = 0;
    int energy = 0;
    int npulses = 0;

    for (int i = 0; i < 16; i++) {
        c[3][i] = xm_sum > 0 ? (5 * xm[i]) / xm_sum : 0;
        npulses += c[3][i];
        corr    += c[3][i] * xm[i];
        energy  += c[3][i] * c[3][i];
    }

    add_pulse(xm, c[3], 16, npulses, 6, &corr, &energy);
    npulses = 6;

    /* --- Shape 2 candidate ---
     * Add unit pulses until you reach K = 8 on shape 3 */

    memcpy(c[2], c[3], sizeof(c[2]));

    add_pulse(xm, c[2], 16, npulses, 8, &corr, &energy);
    npulses = 8;

    /* --- Shape 1 candidate ---
     * Remove any unit pulses from shape 2 that are not part of 0 to 9
     * Update energy and correlation terms accordingly
     * Add unit pulses until you reach K = 10, over N = 10 */

    memcpy(c[1], c[2], sizeof(c[1]));

    for (int i = 10; i < 16; i++) {
        c[1][i] = 0;
        npulses -= c[2][i];
        corr    -= c[2][i] * xm[i];
        energy  -= c[2][i] * c[2][i];
    }

    add_pulse(xm, c[1], 10, npulses, 10, &corr, &energy);
    npulses = 10;

    /* --- Shape 0 candidate ---
     * Add unit pulses until you reach K = 1, on shape 1 */

    memcpy(c[0], c[1], sizeof(c[0]));

    add_pulse(xm + 10, c[0] + 10, 6, 0, 1, &corr, &energy);

    /* --- Add sign --- */

    for (int j = 0; j < 16; j++)
        for (int i = 0; i < 4; i++)
            c[i][j] = x[j] < 0 ? -c[i][j] : c[i][j];

    /* --- Determe shape & gain index ---
     * Search the Mean Square Error, within (shape, gain) combinations.
     * With `cn` of unit norm, MSE = |x|^2 - 2 g <x,cn> + g^2, and only
     * the 2 last terms vary with the candidates */

    int64_t mse_min = INT64_MAX;
    *shape_idx = *gain_idx = 0;

    for (int ic = 0; ic < 4; ic++) {
        const struct lc3_sns_vq_gains *cgains = lc3_sns_vq_gains + ic;
        int64_t cmse_min = INT64_MAX;
        int cgain_idx = 0;

        int32_t dot = 0;
        int c2_sum = 0;
        for (int i = 0; i < 16; i++) {
            dot += (x[i] >> 8) * c[ic][i];               /* Q15 */
            c2_sum += c[ic][i] * c[ic][i];
        }

        int64_t xc = ((int64_t)dot * inv_sqrt[c2_sum < 101 ? c2_sum : 101]) >> 22;  /* Q23 */

        for (int ig = 0; ig < cgains->count; ig++) {
            int64_t g = (int64_t)cgains->v[ig] << 11;    /* Q23 */
            int64_t mse = g * g - 2 * g * xc;

            if (mse < cmse_min) {
                cgain_idx = ig,
                cmse_min = mse;
            }
        }

        if (cmse_min < mse_min) {
            *shape_idx = ic, *gain_idx = cgain_idx;
            mse_min = cmse_min;
        }
    }
}

/**
 * Unquantization of codebooks residual
 * lf/hfcb_idx     Low and high frequency codebooks index
 * c               Table of normalized pulse configuration (Q30)
 * shape/gain      Selected shape/gain indexes
 * scf             Return unquantized scale factors (Q27)
 */
LC3_HOT static void unquantize(int lfcb_idx, int hfcb_idx,
    const int32_t *c, int shape, int gain, int32_t *scf)
{
    const int32_t *lfcb = lc3_sns_lfcb[lfcb_idx];
    const int32_t *hfcb = lc3_sns_hfcb[hfcb_idx];
    int64_t g = lc3_sns_vq_gains[shape].v[gain];        /* Q12 */
    int32_t d[16];

    dct16_inverse(c, d);                                /* Q29 */

    for (int i = 0; i < 8; i++)
        scf[i] = lfcb[i] + (int32_t)((g * d[i]) >> 14);

    for (int i = 8; i < 16; i++)
        scf[i] = hfcb[i-8] + (int32_t)((g * d[i]) >> 14);
}

/**
 * Sub-procedure of `sns_enumerate()`, enumeration of a vector
 * c, n            Table of pulse configuration, and length
 * idx, ls         Return enumeration set
 */
static void enum_mvpq(const int *c, int n, int *idx, bool *ls)
{
    int ci, i;

    /* --- Scan for 1st significant coeff --- */

    for (i = 0, c += n; (ci = *(--c)) == 0 && i < 15; i++);

    *idx = 0;
    *ls = ci < 0;

    /* --- Scan remaining coefficients --- */

    unsigned j = LC3_ABS(ci);

    for (i++; i < n; i++, j += LC3_ABS(ci)) {

        if ((ci = *(--c)) != 0) {
            *idx = (*idx << 1) | *ls;
            *ls = ci < 0;
        }

        *idx += lc3_sns_mpvq_offsets[i][LC3_MIN(j, 10)];
    }
}

/**
 * Sub-procedure of `sns_deenumerate()`, deenumeration of a vector
 * idx, ls         Enumeration set
 * npulses         Number of pulses in the set
 * c, n            Table of pulses configuration, and length
 */
static void deenum_mvpq(int idx, bool ls, int npulses, int *c, int n)
{
    int i;

    /* --- Scan for coefficients --- */

    for (i = n-1; i >= 0 && idx; i--) {

        int ci = 0;

        for (ci = 0; idx < lc3_sns_mpvq_offsets[i][npulses - ci]; ci++);
        idx -= lc3_sns_mpvq_offsets[i][npulses - ci];

        *(c++) = ls ? -ci : ci;
        npulses -= ci;
        if (ci > 0) {
            ls = idx & 1;
            idx >>= 1;
        }
    }

    /* --- Set last significant --- */

    int ci = npulses;

    if (i-- >= 0)
        *(c++) = ls ? -ci : ci;

    while (i-- >= 0)
        *(c++) = 0;
}

/**
 * SNS Enumeration of PVQ configuration
 * shape           Selected shape index
 * c               Selected pulse configuration
 * idx_a, ls_a     Return enumeration set A
 * idx_b, ls_b     Return enumeration set B (shape = 0)
 */
static void enumerate(int shape, const int *c,
    int *idx_a, bool *ls_a, int *idx_b, bool *ls_b)
{
    enum_mvpq(c, shape < 2 ? 10 : 16, idx_a, ls_a);

    if (shape == 0)
        enum_mvpq(c + 10, 6, idx_b, ls_b);
}

/**
 * SNS Deenumeration of PVQ configuration
 * shape           Selected shape index
 * idx_a, ls_a     enumeration set A
 * idx_b, ls_b     enumeration set B (shape = 0)
 * c               Return pulse configuration
 */
static void deenumerate(int shape,
    int idx_a, bool ls_a, int idx_b, bool ls_b, int *c)
{
    static const int8_t npulses_tab[] = { 10, 10, 8, 6 };
    int npulses_a = npulses_tab[shape];

    deenum_mvpq(idx_a, ls_a, npulses_a, c, shape < 2 ? 10 : 16);

    if (shape == 0)
        deenum_mvpq(idx_b, ls_b, 1, c + 10, 6);
    else if (shape == 1)
        memset(c + 10, 0, 6 * sizeof(*c));
}



/* ----------------------------------------------------------------------------
 *  Filtering
 * -------------------------------------------------------------------------- */

/**
 * Spectral shaping
 * dt, sr          Duration and samplerate of the frame
 * scf_q           Quantized scale factors (Q27)
 * inv             True on inverse shaping, False otherwise
 * add             Value added to the log2 gains (Q27, range 0 to 1)
 * x, x_exp        Spectral coefficients, and exponent
 * y               Return shapped coefficients
 * nbits           Number of bits of the shaped coefficients magnitude
  * scratch         Scratch buffer of 144 words
 * return          Exponent of the shaped coefficients
 *
 * `x` and `y` can be the same buffer
 */
LC3_HOT static int spectral_shaping(enum lc3_dt dt, enum lc3_srate sr,
    const int32_t *scf_q, bool inv, int32_t add,
    const int32_t *x, int x_exp, int32_t *y, int nbits, int32_t *scratch)
{
    /* --- Interpolate scale factors --- */

        /* In Q24, the scale factors (range -16 to 16) differences
     * divided by 8 are exact on 21 fractional bits */

        int32_t *scf = scratch;
    int32_t s0, s1 = (inv ? -scf_q[0] : scf_q[0]) >> 3, d;

    scf[0] = scf[1] = s1;
    for (int i = 0; i < 15; i++) {
        s0 = s1, s1 = (inv ? -scf_q[i+1] : scf_q[i+1]) >> 3;
        d = (s1 - s0) >> 3;
        scf[4*i+2] = s0 + 1 * d;
        scf[4*i+3] = s0 + 3 * d;
        scf[4*i+4] = s0 + 5 * d;
        scf[4*i+5] = s0 + 7 * d;
    }
    scf[62] = s1 + 1 * d;
    scf[63] = s1 + 3 * d;

    int nb = lc3_num_bands[dt][sr];
    int n4 = nb < 32 ? 32 % nb : 0;
    int n2 = nb < 32 ? nb - n4 : LC3_MAX_BANDS - nb;

    for (int i4 = 0; i4 < n4; i4++)
        scf[i4] = (scf[4*i4+0] + scf[4*i4+1] +
                   scf[4*i4+2] + scf[4*i4+3]) >> 2;

    for (int i2 = n4; i2 < n4+n2; i2++)
        scf[i2] = (scf[2*(n4+i2)] + scf[2*(n4+i2)+1]) >> 1;

    memmove(scf + n4 + n2, scf + 4*n4 + 2*n2, (nb - n4 - n2) * sizeof(*scf));

    /* --- Gains and band peaks --- */

        /* The peak magnitude of the output is the max over the bands of
     * `nbits(peak_b) + g_exp_b`: the band peaks are merged (bitwise or)
     * by gain exponent (range -16 to 16), then counted */

    const int *lim = lc3_band_lim[dt][sr];
        int32_t *g_mant = scratch + LC3_MAX_BANDS;
    int8_t *g_exp = (int8_t *)(scratch + 2*LC3_MAX_BANDS);
    uint32_t e_peak[34] = { 0 };
    int t_max = INT_MIN;

    /* The first bands hold one coefficient each (lim[b] = b, up to n1) */

    int n1 = 0;
    while (n1 < nb && lim[n1+1] == n1+1)
        n1++;

    for (int i = 0, ib = 0; ib < nb; ib++) {
        int32_t v = (add >> 3) - scf[ib];
        if (__builtin_expect((uint32_t)(v + (15 << 24)) > (uint32_t)(30 << 24), 0))
            v = v > 0 ? (15 << 24) : -(15 << 24);

        int e = lc3fx_pow2_q27(v * 8, &g_mant[ib]);
        g_exp[ib] = e;

        uint32_t m;
        if (ib < n1) {
            m = (uint32_t)(x[ib] ^ (x[ib] >> 31));
        } else {
            m = 0;
            for (i = lim[ib]; i < lim[ib+1]; i++)
                m |= (uint32_t)(x[i] ^ (x[i] >> 31));
        }

        e_peak[e + 17] |= m;
    }

    /* From the highest exponent: stop when no lower one can do better */

    for (int e = 33; e >= 0 && e - 17 + 32 > t_max; e--)
        if (e_peak[e]) {
            int t = lc3fx_nbits(e_peak[e]) + e - 17;
            t_max = t > t_max ? t : t_max;
        }

    if (t_max == INT_MIN) {
        memset(y, 0, lim[nb] * sizeof(*y));
        return 0;
    }

    /* --- Spectral shaping ---
     * y = x * mant * 2^k / 4, such that the shaped value is y * 2^y_exp */

    int y_exp = t_max + 1 + x_exp - nbits;

    for (int ib = 0; ib < n1; ib++) {
        int k = x_exp + g_exp[ib] - y_exp + 2;
        int32_t gm = g_mant[ib];

        y[ib] = k > 31 ? 0 : k >= 0 ? mulh(LC3FX_SHL(x[ib], k), gm) :
                k > -32 ? mulh(x[ib], gm) >> -k : 0;
    }

    for (int i = n1, ib = n1; ib < nb; ib++) {
        int k = x_exp + g_exp[ib] - y_exp + 2;
        int32_t gm = g_mant[ib];

        if (k > 31)   /* only a band of zeros has that much room */
            for ( ; i < lim[ib+1]; i++)
                y[i] = 0;
        else if (k >= 0)
            for ( ; i < lim[ib+1]; i++)
                y[i] = mulh(LC3FX_SHL(x[i], k), gm);
        else if (k > -32)
            for ( ; i < lim[ib+1]; i++)
                y[i] = mulh(x[i], gm) >> -k;
        else
            for ( ; i < lim[ib+1]; i++)
                y[i] = 0;
    }

    return y_exp;
}


/* ----------------------------------------------------------------------------
 *  Interface
 * -------------------------------------------------------------------------- */

/**
 * SNS analysis
 */
int lc3_sns_analyze(
    enum lc3_dt dt, enum lc3_srate sr, int nbytes,
    const uint32_t *eb, int e_exp, bool att, struct lc3_sns_data *data,
        const int32_t *x, int x_exp, int32_t *y, int nbits, int32_t *scratch)
{
    /* Processing steps :
     * - Determine 16 scale factors from bands energy estimation
     * - Get codebooks indexes that match thoses scale factors
     * - Quantize the residual with the selected codebook
     * - The pulse configuration `c[]` is enumerated
     * - Finally shape the spectrum coefficients accordingly */

    int32_t scf[16], cn[16];
    int c[4][16];

    compute_scale_factors(dt, sr, nbytes, eb, e_exp, att, scf, scratch);
    LC3FX_MARK(3);

    resolve_codebooks(scf, &data->lfcb, &data->hfcb);

    quantize(scf, data->lfcb, data->hfcb,
        c, &data->shape, &data->gain);

    normalize(c[data->shape], cn);

    unquantize(data->lfcb, data->hfcb,
        cn, data->shape, data->gain, scf);

    enumerate(data->shape, c[data->shape],
        &data->idx_a, &data->ls_a, &data->idx_b, &data->ls_b);
    LC3FX_MARK(4);

    return spectral_shaping(dt, sr, scf, false, 0, x, x_exp, y, nbits, scratch);
}

/**
 * SNS synthesis
 */
int lc3_sns_synthesize(
    enum lc3_dt dt, enum lc3_srate sr, const lc3_sns_data_t *data,
        int32_t add, const int32_t *x, int x_exp, int32_t *y, int nbits,
    int32_t *scratch)
{
    int32_t scf[16], cn[16];
    int c[16];

    deenumerate(data->shape,
        data->idx_a, data->ls_a, data->idx_b, data->ls_b, c);

    normalize(c, cn);

    unquantize(data->lfcb, data->hfcb, cn, data->shape, data->gain, scf);
    LC3FX_MARK(20);

    return spectral_shaping(dt, sr, scf, true, add, x, x_exp, y, nbits, scratch);
}

/**
 * Return number of bits coding the bitstream data
 */
int lc3_sns_get_nbits(void)
{
    return 38;
}

/**
 * Put bitstream data
 */
void lc3_sns_put_data(lc3_bits_t *bits, const struct lc3_sns_data *data)
{
    /* --- Codebooks --- */

    lc3_put_bits(bits, data->lfcb, 5);
    lc3_put_bits(bits, data->hfcb, 5);

    /* --- Shape, gain and vectors --- *
     * Write MSB bit of shape index, next LSB bits of shape and gain,
     * and MVPQ vectors indexes are muxed */

    int shape_msb = data->shape >> 1;
    lc3_put_bit(bits, shape_msb);

    if (shape_msb == 0) {
        const int size_a = 2390004;
        int submode = data->shape & 1;

        int mux_high = submode == 0 ?
            2 * (data->idx_b + 1) + data->ls_b : data->gain & 1;
        int mux_code = mux_high * size_a + data->idx_a;

        lc3_put_bits(bits, data->gain >> submode, 1);
        lc3_put_bits(bits, data->ls_a, 1);
        lc3_put_bits(bits, mux_code, 25);

    } else {
        const int size_a = 15158272;
        int submode = data->shape & 1;

        int mux_code = submode == 0 ?
            data->idx_a : size_a + 2 * data->idx_a + (data->gain & 1);

        lc3_put_bits(bits, data->gain >> submode, 2);
        lc3_put_bits(bits, data->ls_a, 1);
        lc3_put_bits(bits, mux_code, 24);
    }
}

/**
 * Get bitstream data
 */
int lc3_sns_get_data(lc3_bits_t *bits, struct lc3_sns_data *data)
{
    /* --- Codebooks --- */

    *data = (struct lc3_sns_data){
        .lfcb = lc3_get_bits(bits, 5),
        .hfcb = lc3_get_bits(bits, 5)
    };

    /* --- Shape, gain and vectors --- */

    int shape_msb = lc3_get_bit(bits);
    data->gain = lc3_get_bits(bits, 1 + shape_msb);
    data->ls_a = lc3_get_bit(bits);

    int mux_code = lc3_get_bits(bits, 25 - shape_msb);

    if (shape_msb == 0) {
        const int size_a = 2390004;

        if (mux_code >= size_a * 14)
            return -1;

        data->idx_a = mux_code % size_a;
        mux_code = mux_code / size_a;

        data->shape = (mux_code < 2);

        if (data->shape == 0) {
            data->idx_b = (mux_code - 2) / 2;
            data->ls_b  = (mux_code - 2) % 2;
        } else {
            data->gain = (data->gain << 1) + (mux_code % 2);
        }

    } else {
        const int size_a = 15158272;

        if (mux_code >= size_a + 1549824)
            return -1;

        data->shape = 2 + (mux_code >= size_a);
        if (data->shape == 2) {
            data->idx_a = mux_code;
        } else {
            mux_code -= size_a;
            data->idx_a = mux_code / 2;
            data->gain = (data->gain << 1) + (mux_code % 2);
        }
    }

    return 0;
}
