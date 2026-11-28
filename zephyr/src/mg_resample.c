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

#include "mg_resample.h"

#include <string.h>

#define K MG_RESAMPLE_TAPS_PER_PHASE

void mg_resample_reset(struct mg_resample *r)
{
	memset(r->hist, 0, sizeof(r->hist));
}

static inline int16_t sat16(float v)
{
	if (v >= 32767.0f) {
		return 32767;
	}
	if (v <= -32768.0f) {
		return -32768;
	}
	return (int16_t)(v + (v >= 0 ? 0.5f : -0.5f));
}

size_t mg_resample_32_to_24(struct mg_resample *r, const int16_t *in, size_t n_in, int16_t *out)
{
	size_t n_out = n_in / 4 * 3;

	/* Output n sits at 4n in the 96 kHz domain: input i = 4n / 3 (block
	 * local: a block is a multiple of 4 inputs, so the phase restarts),
	 * phase p = 4n mod 3. y[n] = sum_j h[p + 3j] * x[i - j]. */
	for (size_t n = 0; n < n_out; n++) {
		size_t m = 4 * n;
		size_t i = m / 3;
		const float *h = mg_resample_taps[m % 3];
		float acc = 0.0f;
		size_t j_in = i + 1 < K ? i + 1 : K; /* taps reaching this block */

		for (size_t j = 0; j < j_in; j++) {
			acc += h[j] * (float)in[i - j];
		}
		/* The rest reach back into the previous block: x[-1] is
		 * hist[K - 2]. */
		for (size_t j = j_in; j < K; j++) {
			acc += h[j] * (float)r->hist[K - 1 + i - j];
		}
		out[n] = sat16(acc);
	}
	/* Keep the last K - 1 inputs. */
	if (n_in >= K - 1) {
		memcpy(r->hist, &in[n_in - (K - 1)], (K - 1) * sizeof(int16_t));
	} else {
		memmove(r->hist, &r->hist[n_in], (K - 1 - n_in) * sizeof(int16_t));
		memcpy(&r->hist[K - 1 - n_in], in, n_in * sizeof(int16_t));
	}
	return n_out;
}
