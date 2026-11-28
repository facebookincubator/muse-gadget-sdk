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

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * A rational polyphase resampler for mono 16-bit audio: rate_in -> rate_out
 * as L/M in lowest terms, with a Blackman-windowed sinc low-pass cut at
 * 0.92 of the lower Nyquist frequency. Coefficients are Q14, computed once
 * at open; the filter runs in integer arithmetic (no FPU needed).
 */

#define MG_RESAMPLE_MAX_IN 1024   /* most input samples per mg_resample_run() call */

typedef struct {
    uint32_t rate_in, rate_out;
    int L, M;              /* rate_out / rate_in = L / M */
    int taps;              /* per phase */
    int16_t *coef;         /* [L][taps], taps reversed: coef[p*taps + k] pairs with x[i - k] */
    int16_t *hist;         /* taps - 1 samples of history, then new input */
    int nhist;             /* valid samples in hist */
    uint32_t pos;          /* output position in 1/L input samples, relative to hist[taps - 1] */
    bool passthrough;
} mg_resample_t;

/* Allocates the tables (bytes: see mg_resample_bytes). False if out of memory or rates are 0. */
bool mg_resample_open(mg_resample_t *r, uint32_t rate_in, uint32_t rate_out);
void mg_resample_close(mg_resample_t *r);
/* Starts over from silence, keeping the tables (allocates nothing). */
void mg_resample_reset(mg_resample_t *r);
/* Heap the tables for rate_in -> rate_out take. */
size_t mg_resample_bytes(uint32_t rate_in, uint32_t rate_out);
/*
 * Resamples `n` (<= MG_RESAMPLE_MAX_IN) input samples, writing at most `cap`
 * output samples; returns how many it wrote. `cap` must be at least
 * n * rate_out / rate_in + 2. A resampler whose open failed writes nothing.
 */
size_t mg_resample_run(mg_resample_t *r, const int16_t *in, size_t n, int16_t *out, size_t cap);
