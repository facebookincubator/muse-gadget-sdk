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
 * The detector tap's 24 to 16 kHz resampler, in C: the same 63-tap
 * Hann-windowed sinc (low-pass at 7.2 kHz, two phases) as
 * musegadget.wakeword_input.Pcm24To16, which takes about 10% of a server
 * core in Python, many times the detector itself. Independent of chunk
 * boundaries.
 */

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define TAPS 63

typedef struct muse_ww_resampler {
    double taps[2][TAPS];
    int16_t history[TAPS];   /* newest at history[head] */
    int head;
    long long input_index;
    long long output_index;
} muse_ww_resampler_t;

void muse_ww_resampler_reset(muse_ww_resampler_t *r)
{
    memset(r->history, 0, sizeof r->history);
    r->head = 0;
    r->input_index = -1;
    r->output_index = 0;
}

muse_ww_resampler_t *muse_ww_resampler_create(void)
{
    muse_ww_resampler_t *r = calloc(1, sizeof *r);
    if (r == NULL) {
        return NULL;
    }
    for (int phase = 0; phase < 2; phase++) {
        double total = 0;
        for (int k = 0; k < TAPS; k++) {
            const double offset = k + 0.5 * phase - 31;
            const double sinc = offset == 0 ? 0.6 : sin(M_PI * 0.6 * offset) / (M_PI * offset);
            r->taps[phase][k] = sinc * (0.5 - 0.5 * cos(2 * M_PI * k / 62));
            total += r->taps[phase][k];
        }
        for (int k = 0; k < TAPS; k++) {
            r->taps[phase][k] /= total;
        }
    }
    muse_ww_resampler_reset(r);
    return r;
}

void muse_ww_resampler_destroy(muse_ww_resampler_t *r)
{
    free(r);
}

/* Writes at most n * 2 / 3 + 1 samples to out; returns how many. */
size_t muse_ww_resampler_feed(muse_ww_resampler_t *r, const int16_t *in, size_t n, int16_t *out)
{
    size_t written = 0;
    for (size_t i = 0; i < n; i++) {
        r->input_index++;
        r->head = (r->head + TAPS - 1) % TAPS;
        r->history[r->head] = in[i];
        if (r->input_index != r->output_index * 3 / 2) {
            continue;
        }
        const double *taps = r->taps[r->output_index % 2];
        double acc = 0;
        for (int k = 0, j = r->head; k < TAPS; k++, j = j + 1 == TAPS ? 0 : j + 1) {
            acc += r->history[j] * taps[k];
        }
        const double v = nearbyint(acc);
        out[written++] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
        r->output_index++;
    }
    return written;
}
