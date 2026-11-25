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

#include "ltpf.h"
#include "tables.h"


/* ----------------------------------------------------------------------------
 *  Synthesis
 * -------------------------------------------------------------------------- */

/**
 * Width of synthesis filter
 */

#define MAX_FILTER_WIDTH  12   /* 48 kHz, whatever the samplerates of the build */


/**
 * Synthesis filter template
 * xh, nh          History ring buffer of filtered samples
 * lag             Lag parameter in the ring buffer
 * x0              w-1 previous input samples
 * x, n            Current samples as input, filtered as output
 * c, w            Coefficients `den` then `num`, and width of filter
 * fade            Fading mode of filter  -1: Out  1: In  0: None
 */
LC3_HOT static inline void synthesize_template(
    const int32_t *xh, int nh, int lag,
    const int32_t *x0, int32_t *x, int n,
    const int32_t *c, const int w, int fade)
{
    /* Samples in Q12, coefficients in Q31: the products `mulh(x, c)`
     * accumulate half of the filter contribution `u`, and the fading
     * gain `g` is Q30: `g * u` = mulh(u / 2, g) * 8 */

    int32_t g = fade <= 0 ? 1 << 30 : 0;
    int32_t g_incr = ((fade > 0) - (fade < 0)) * ((1 << 30) / n);
    int32_t u[MAX_FILTER_WIDTH];

    /* --- Load previous samples --- */

    lag += (w >> 1);

    const int32_t *y = x - xh < lag ? x + (nh - lag) : x - lag;
    const int32_t *y_end = xh + nh - 1;

    for (int j = 0; j < w-1; j++) {

        u[j] = 0;

        int32_t yi = *y, xi = *(x0++);
        y = y < y_end ? y + 1 : xh;

        for (int k = 0; k <= j; k++)
            u[j-k] -= lc3fx_mulh(yi, c[k]);

        for (int k = 0; k <= j; k++)
            u[j-k] += lc3fx_mulh(xi, c[w+k]);
    }

    u[w-1] = 0;

    /* --- Process by filter length --- */

    for (int i = 0; i < n; i += w)
        for (int j = 0; j < w; j++, g += g_incr) {

            int32_t yi = *y, xi = *x;
            y = y < y_end ? y + 1 : xh;

            for (int k = 0; k < w; k++)
                u[(j+(w-1)-k)%w] -= lc3fx_mulh(yi, c[k]);

            for (int k = 0; k < w; k++)
                u[(j+(w-1)-k)%w] += lc3fx_mulh(xi, c[w+k]);

            *(x++) = xi - LC3FX_SHL(lc3fx_mulh(u[j], g), 3);
            u[j] = 0;
        }
}

/**
 * Synthesis filter for each samplerates (width of filter)
 */


LC3_HOT static void synthesize_4(const int32_t *xh, int nh, int lag,
    const int32_t *x0, int32_t *x, int n, const int32_t *c, int fade)
{
    synthesize_template(xh, nh, lag, x0, x, n, c, 4, fade);
}

LC3_HOT static void synthesize_6(const int32_t *xh, int nh, int lag,
    const int32_t *x0, int32_t *x, int n, const int32_t *c, int fade)
{
    synthesize_template(xh, nh, lag, x0, x, n, c, 6, fade);
}

LC3_HOT static void synthesize_8(const int32_t *xh, int nh, int lag,
    const int32_t *x0, int32_t *x, int n, const int32_t *c, int fade)
{
    synthesize_template(xh, nh, lag, x0, x, n, c, 8, fade);
}

LC3_HOT static void synthesize_12(const int32_t *xh, int nh, int lag,
    const int32_t *x0, int32_t *x, int n, const int32_t *c, int fade)
{
    synthesize_template(xh, nh, lag, x0, x, n, c, 12, fade);
}

static void (* const synthesize[])(const int32_t *, int, int,
    const int32_t *, int32_t *, int, const int32_t *, int) =
{
    [LC3_SRATE_8K ] = synthesize_4,
    [LC3_SRATE_16K] = synthesize_4,
    [LC3_SRATE_24K] = synthesize_6,
    [LC3_SRATE_32K] = synthesize_8,
    [LC3_SRATE_48K] = synthesize_12,
};


/**
 * LTPF Synthesis
 */
void lc3_ltpf_synthesize(enum lc3_dt dt, enum lc3_srate sr, int nbytes,
    lc3_ltpf_synthesis_t *ltpf, const lc3_ltpf_data_t *data,
    const int32_t *xh, int32_t *x)
{
    int nh = lc3_ns(dt, sr) + lc3_nh(dt, sr);

    /* --- Filter parameters --- */

    int p_idx = data ? data->pitch_index : 0;
    int pitch =
        p_idx >= 440 ? (((p_idx     ) - 283) << 2)  :
        p_idx >= 380 ? (((p_idx >> 1) -  63) << 2) + (((p_idx & 1)) << 1) :
                       (((p_idx >> 2) +  32) << 2) + (((p_idx & 3)) << 0)  ;

    pitch = (pitch * lc3_ns(LC3_DT_10M, sr) + 64) / 128;

    int nbits = (nbytes*8 * (1 + LC3_DT_10M)) / (1 + dt);
    if (dt == LC3_DT_2M5)
      nbits = (6 * nbits + 5) / 10;
    if (dt == LC3_DT_5M)
      nbits -= 160;

    int g_idx = LC3_MAX(nbits / 80, (int)(3 + sr)) - (3 + sr);
    bool active = data && data->active && g_idx < 4;

    int w = LC3_MAX(4, lc3_ns_4m[sr] >> 4);
        int32_t c[2 * MAX_FILTER_WIDTH];

    /* Gain 0.4 - 0.05 * g_idx, and 0.85 times, in Q31 */
    int32_t g = active ? 858993459 - 107374182 * g_idx : 0;
    int32_t g85 = lc3fx_mul31(g, 1825361101);

    for (int i = 0; i < w; i++) {
        c[  i] = lc3fx_mul31(g, lc3_ltpf_cden[sr][pitch & 3][(w-1)-i]);
        c[w+i] = lc3fx_mul31(g85, lc3_ltpf_cnum[sr][LC3_MIN(g_idx, 3)][(w-1)-i]);
    }

    /* --- Transition handling --- */

    int ns = lc3_ns(dt, sr);
    int nt = ns / (1 + dt);
    int32_t x0[2][MAX_FILTER_WIDTH];

    memcpy(x0[0], ltpf->x, (w-1) * sizeof(int32_t));
    memcpy(ltpf->x, x + ns - (w-1), (w-1) * sizeof(int32_t));

    if (active)
        memcpy(x0[1], x + nt-(w-1), (w-1) * sizeof(int32_t));

    if (!ltpf->active && active)
        synthesize[sr](xh, nh, pitch/4, x0[0], x, nt, c, 1);
    else if (ltpf->active && !active)
        synthesize[sr](xh, nh, ltpf->pitch/4, x0[0], x, nt, ltpf->c, -1);
    else if (ltpf->active && active && ltpf->pitch == pitch)
        synthesize[sr](xh, nh, pitch/4, x0[0], x, nt, c, 0);
    else if (ltpf->active && active) {
        synthesize[sr](xh, nh, ltpf->pitch/4, x0[0], x, nt, ltpf->c, -1);
        synthesize[sr](xh, nh, pitch/4,
            (x <= xh ? x + nh : x) - (w-1), x, nt, c, 1);
    }

    /* --- Remainder --- */

    if (active && ns > nt)
        synthesize[sr](xh, nh, pitch/4, x0[1], x + nt, ns-nt, c, 0);

    /* --- Update state --- */

    ltpf->active = active;
    ltpf->pitch = pitch;
    memcpy(ltpf->c, c, 2*w * sizeof(*ltpf->c));
}


/* ----------------------------------------------------------------------------
 *  Bitstream data
 * -------------------------------------------------------------------------- */

/**
 * LTPF disable
 */
void lc3_ltpf_disable(struct lc3_ltpf_data *data)
{
    data->active = false;
}

/**
 * Return number of bits coding the bitstream data
 */
int lc3_ltpf_get_nbits(bool pitch)
{
    return 1 + 10 * pitch;
}

/**
 * Put bitstream data
 */
void lc3_ltpf_put_data(lc3_bits_t *bits,
    const struct lc3_ltpf_data *data)
{
    lc3_put_bit(bits, data->active);
    lc3_put_bits(bits, data->pitch_index, 9);
}

/**
 * Get bitstream data
 */
void lc3_ltpf_get_data(lc3_bits_t *bits,
    struct lc3_ltpf_data *data)
{
    data->active = lc3_get_bit(bits);
    data->pitch_index = lc3_get_bits(bits, 9);
}
