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

/* The 32 -> 24 kHz resampler: passband, alias rejection and block
 * independence. */

#include <math.h>
#include <stdint.h>
#include <stdlib.h>

#include "check.h"
#include "mg_resample.h"

#define FS_IN  32000
#define FS_OUT 24000
#define N_IN   FS_IN /* 1 s */
#define N_OUT  (N_IN * 3 / 4)
#define AMP    10000.0

static int16_t in[N_IN];
static int16_t out[N_OUT], out2[N_OUT];

static int verbose(void)
{
	return getenv("HOST_VERBOSE") != NULL;
}

static void tone(double f)
{
	for (int i = 0; i < N_IN; i++) {
		in[i] = (int16_t)lrint(AMP * sin(2 * M_PI * f * i / FS_IN));
	}
}

static void run(size_t block, int16_t *o)
{
	struct mg_resample r;
	size_t n = 0;

	mg_resample_reset(&r);
	for (size_t i = 0; i < N_IN; i += block) {
		n += mg_resample_32_to_24(&r, &in[i], block, &o[n]);
	}
	CHECK(n == N_OUT);
}

/* Amplitude of frequency f in the output (least squares, skipping 50 ms). */
static double amp_at(const int16_t *o, double f)
{
	double c = 0, s = 0;
	int n0 = FS_OUT / 20, n = 0;

	for (int i = n0; i < N_OUT; i++, n++) {
		c += o[i] * cos(2 * M_PI * f * i / FS_OUT);
		s += o[i] * sin(2 * M_PI * f * i / FS_OUT);
	}
	return 2 * sqrt(c * c + s * s) / n;
}

static double rms(const int16_t *o)
{
	double e = 0;
	int n0 = FS_OUT / 20;

	for (int i = n0; i < N_OUT; i++) {
		e += (double)o[i] * o[i];
	}
	return sqrt(e / (N_OUT - n0));
}

static void test_passband(void)
{
	static const double fs[] = {100, 1000, 3000, 6000, 8000, 10000, 10500};
	double worst = 0;

	for (size_t k = 0; k < sizeof(fs) / sizeof(fs[0]); k++) {
		tone(fs[k]);
		run(320, out);
		double db = 20 * log10(amp_at(out, fs[k]) / AMP);

		worst = fmax(worst, fabs(db));
		if (verbose()) {
			printf("pass %5.0f Hz: %+.4f dB\n", fs[k], db);
		}
	}
	CHECK(worst < 0.05);
}

static void test_alias_rejection(void)
{
	/* Inputs between 12 and 16 kHz would fold to 24 - f kHz. */
	static const double fs[] = {12000, 12500, 13000, 14000, 15000, 15900};
	double worst = -200;

	for (size_t k = 0; k < sizeof(fs) / sizeof(fs[0]); k++) {
		tone(fs[k]);
		run(320, out);
		double db = 20 * log10(fmax(rms(out) * sqrt(2), 1e-3) / AMP);

		if (fs[k] > 12000) {
			worst = fmax(worst, db);
		}
		if (verbose()) {
			printf("stop %5.0f Hz (alias at %5.0f Hz): %.1f dB\n", fs[k], FS_OUT - fs[k],
			       db);
		}
	}
	CHECK(worst < -65);
}

static void test_blocks(void)
{
	tone(4321);
	run(320, out);
	run(4, out2);
	CHECK_MEM(out, out2, sizeof(out));
	run(N_IN, out2);
	CHECK_MEM(out, out2, sizeof(out));
}

HOST_MAIN(test_passband, test_alias_rejection, test_blocks)
