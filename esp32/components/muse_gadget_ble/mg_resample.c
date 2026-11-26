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

#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE 1   /* M_PI: glibc declares it only with this (macOS and newlib do anyway) */
#endif
#include "mg_resample.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define TAPS_K 24        /* taps per phase for 1:1 bandwidth; scaled by the decimation ratio */
#define CUTOFF 0.92      /* of the lower Nyquist frequency */
#define Q 14

static uint32_t gcd(uint32_t a, uint32_t b)
{
    while (b) {
        uint32_t t = a % b;
        a = b;
        b = t;
    }
    return a;
}

static void ratio(uint32_t in, uint32_t out, int *L, int *M, int *taps)
{
    uint32_t g = gcd(in, out);
    *L = (int)(out / g);
    *M = (int)(in / g);
    /* Decimating needs a longer filter (at the input rate) for the same transition. */
    int t = *M > *L ? (TAPS_K * *M + *L - 1) / *L : TAPS_K;
    *taps = t < 4 ? 4 : t;
}

size_t mg_resample_bytes(uint32_t rate_in, uint32_t rate_out)
{
    if (!rate_in || !rate_out || rate_in == rate_out) {
        return 0;
    }
    int L, M, taps;
    ratio(rate_in, rate_out, &L, &M, &taps);
    return (size_t)L * taps * sizeof(int16_t) + (taps + 2 * MG_RESAMPLE_MAX_IN) * sizeof(int16_t);
}

bool mg_resample_open(mg_resample_t *r, uint32_t rate_in, uint32_t rate_out)
{
    memset(r, 0, sizeof(*r));
    if (!rate_in || !rate_out) {
        return false;
    }
    r->rate_in = rate_in;
    r->rate_out = rate_out;
    if (rate_in == rate_out) {
        r->passthrough = true;
        return true;
    }
    ratio(rate_in, rate_out, &r->L, &r->M, &r->taps);
    const int L = r->L, T = r->taps, N = L * T;
    r->coef = malloc((size_t)N * sizeof(int16_t));
    r->hist = calloc((size_t)(T + 2 * MG_RESAMPLE_MAX_IN), sizeof(int16_t));
    if (!r->coef || !r->hist) {
        mg_resample_close(r);
        return false;
    }
    float *h = malloc((size_t)T * sizeof(float));
    if (!h) {
        mg_resample_close(r);
        return false;
    }
    /* Prototype at L * rate_in: cutoff in cycles per sample. */
    const float fc = 0.5f * (float)CUTOFF / (float)(L > r->M ? L : r->M);
    const float mid = (N - 1) / 2.0f;
    for (int p = 0; p < L; p++) {
        float sum = 0;
        for (int k = 0; k < T; k++) {
            int j = k * L + p;   /* prototype index of tap k in phase p */
            float x = j - mid;
            float s = fabsf(x) < 1e-6f ? 2 * fc : sinf(2 * (float)M_PI * fc * x) / ((float)M_PI * x);
            float w = 0.42f - 0.5f * cosf(2 * (float)M_PI * (j + 0.5f) / N) + 0.08f * cosf(4 * (float)M_PI * (j + 0.5f) / N);
            h[k] = s * w;
            sum += h[k];
        }
        /* Each phase sums to 1, so DC and in-band level are flat. */
        for (int k = 0; k < T; k++) {
            float v = h[k] / (sum != 0 ? sum : 1) * (1 << Q);
            /* Stored against x[i - k]: phase p, tap k weights the k-th newest sample. */
            r->coef[p * T + (T - 1 - k)] = (int16_t)lrintf(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
        }
    }
    free(h);
    r->nhist = T - 1;   /* zeros: the filter starts from silence */
    r->pos = 0;
    return true;
}

void mg_resample_close(mg_resample_t *r)
{
    free(r->coef);
    free(r->hist);
    r->coef = NULL;
    r->hist = NULL;
}

void mg_resample_reset(mg_resample_t *r)
{
    if (r->passthrough || !r->hist) {
        return;
    }
    memset(r->hist, 0, (size_t)(r->taps + 2 * MG_RESAMPLE_MAX_IN) * sizeof(int16_t));
    r->nhist = r->taps - 1;
    r->pos = 0;
}

size_t mg_resample_run(mg_resample_t *r, const int16_t *in, size_t n, int16_t *out, size_t cap)
{
    if (r->passthrough) {
        size_t m = n < cap ? n : cap;
        memcpy(out, in, m * sizeof(int16_t));
        return m;
    }
    if (!r->coef || !r->hist) {
        return 0;   /* its open failed */
    }
    const int T = r->taps, L = r->L;
    /* Room for it after what a short `cap` left unread (the caller should size cap). */
    int room = T - 1 + 2 * MG_RESAMPLE_MAX_IN - r->nhist;
    if ((int)n > room) {
        n = room > 0 ? (size_t)room : 0;
    }
    memcpy(r->hist + r->nhist, in, n * sizeof(int16_t));
    int avail = r->nhist + (int)n;   /* samples in hist; newest at avail - 1 */
    size_t o = 0;
    /* Output at pos/L input samples past hist[T - 1] needs hist[i - T + 1 .. i]. */
    for (;;) {
        int i = (int)(r->pos / L) + T - 1;
        if (i >= avail || o >= cap) {
            break;
        }
        const int16_t *c = r->coef + (r->pos % L) * T;
        const int16_t *x = r->hist + i - (T - 1);
        int32_t acc = 0;
        for (int k = 0; k < T; k++) {
            acc += (int32_t)c[k] * x[k];
        }
        acc = (acc + (1 << (Q - 1))) >> Q;
        out[o++] = (int16_t)(acc > 32767 ? 32767 : acc < -32768 ? -32768 : acc);
        r->pos += r->M;
    }
    /* Keep the last T - 1 samples before the next output's window. */
    int consumed = (int)(r->pos / L);
    int keep_from = consumed;   /* hist index of the oldest sample still needed */
    if (keep_from > avail) {
        keep_from = avail;
    }
    memmove(r->hist, r->hist + keep_from, (size_t)(avail - keep_from) * sizeof(int16_t));
    r->nhist = avail - keep_from;
    r->pos -= (uint32_t)keep_from * L;
    return o;
}
