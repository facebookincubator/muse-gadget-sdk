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

/* LC3 sample rates as the SDKs build liblc3: 16 and 24 kHz, 10 ms
 * frames, encode and decode a voiced signal with a sibilant band; every
 * other rate is refused at setup (its tables are NULL in tables.c). */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE 1   /* M_PI: glibc declares it only with this (macOS and newlib do anyway) */
#endif
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "lc3.h"

static double run(int rate, int kbps)
{
    int ns = lc3_frame_samples(10000, rate), nb = kbps * 10000 / 8000, frames = 200;
    void *em = malloc(lc3_encoder_size(10000, rate)), *dm = malloc(lc3_decoder_size(10000, rate));
    lc3_encoder_t e = lc3_setup_encoder(10000, rate, 0, em);
    lc3_decoder_t d = lc3_setup_decoder(10000, rate, 0, dm);
    if (!e || !d) return -1000;
    int16_t *x = malloc(sizeof(int16_t) * ns * frames), *y = malloc(sizeof(int16_t) * ns * frames);
    uint8_t b[400];
    double ph = 0;
    for (int i = 0; i < ns * frames; i++) {
        double t = (double)i / rate;
        ph += 2 * M_PI * (140 + 15 * sin(2 * M_PI * 3 * t)) / rate;
        double v = 0.5 * sin(ph) + 0.25 * sin(ph * 5.1) + 0.15 * sin(ph * 17.3)
                 + (rate > 16000 ? 0.05 * sin(2 * M_PI * 9500 * t) : 0);   /* above 8 kHz at 24 kHz */
        x[i] = (int16_t)(v * (0.3 + 0.7 * fabs(sin(2 * M_PI * 4 * t))) * 12000);
    }
    for (int f = 0; f < frames; f++) {
        if (lc3_encode(e, LC3_PCM_FORMAT_S16, x + f * ns, 1, nb, b) || lc3_decode(d, b, nb, LC3_PCM_FORMAT_S16, y + f * ns, 1) < 0)
            return -999;
    }
    double best = -100;
    for (int lag = 0; lag < 3 * ns; lag++) {
        double s = 0, n = 0;
        for (int i = 2 * ns; i + lag < ns * frames; i++) { double dd = y[i + lag] - x[i]; s += (double)x[i] * x[i]; n += dd * dd; }
        double snr = 10 * log10(s / (n + 1e-9));
        best = snr > best ? snr : best;
    }
    free(x); free(y); free(em); free(dm);
    return best;
}

int main(void)
{
    static const int ok[][2] = { { 16000, 32 }, { 24000, 24 }, { 24000, 32 }, { 24000, 48 } };
    for (unsigned i = 0; i < sizeof(ok) / sizeof(ok[0]); i++)
        printf("rate %d kbps %d snr %.1f\n", ok[i][0], ok[i][1], run(ok[i][0], ok[i][1]));
    static const int refused[] = { 8000, 32000, 48000 };
    for (unsigned i = 0; i < 3; i++)
        printf("refused %d %d\n", refused[i], lc3_setup_encoder(10000, refused[i], 0, malloc(lc3_encoder_size(10000, 48000) + 1)) == NULL);
    return 0;
}
