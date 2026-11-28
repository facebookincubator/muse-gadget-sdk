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

#ifndef MG_RESAMPLE_H_
#define MG_RESAMPLE_H_

#include <stddef.h>
#include <stdint.h>

#include "mg_resample_taps.h"

/*
 * 32 kHz -> 24 kHz (up 3, down 4), streaming, mono 16-bit.
 * The nRF54L15's PDM has no exact 24 kHz setting but an exact 32 kHz
 * one, so 24 kHz capture runs the microphone at 32 kHz and this filter
 * after it. Polyphase FIR (mg_resample_taps.h, tools/gen_resampler.py):
 * pass 0-10.5 kHz (flat within 0.002 dB), stop from 12 kHz (aliases at
 * least 74 dB down on the host test); group delay 46.3 input samples
 * (1.45 ms); 93 multiply-adds per output sample (2.2 M/s at 24 kHz); 184 B
 * of state, 1.1 KB of taps. Pure C, no Zephyr: the host tests run it as is.
 *
 * Input comes in multiples of 4 samples (a 10 ms block: 320 in, 240 out).
 * One instance per stream; mg_resample_reset() at each capture start.
 */
struct mg_resample {
	/* The last TAPS_PER_PHASE - 1 inputs, oldest first. */
	int16_t hist[MG_RESAMPLE_TAPS_PER_PHASE - 1];
};

void mg_resample_reset(struct mg_resample *r);

/* Resamples n_in samples (a multiple of 4) into n_in * 3 / 4 samples. */
size_t mg_resample_32_to_24(struct mg_resample *r, const int16_t *in, size_t n_in, int16_t *out);

#endif
