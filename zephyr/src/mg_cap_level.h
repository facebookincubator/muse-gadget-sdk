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

#ifndef MG_CAP_LEVEL_H
#define MG_CAP_LEVEL_H

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/*
 * The capture level of each turn: peak and
 * rms of the samples the encoder gets (after the high-pass and the gain),
 * the floor (rms of the first 100 ms), the samples that clipped (the ADC's
 * rail or the gain's clamp), and the gain in effect. Numbers only, never
 * audio. The first real turn on any unit is then the close-talk anchor.
 * Portable, integer only (a fixed-point log2: no float library in shared
 * code), within 0.1 dB of the exact value.
 */
struct mg_cap_meter {
	uint32_t rate;
	uint32_t samples;
	uint64_t sumsq;
	uint32_t floor_n;
	uint64_t floor_sumsq;
	uint32_t peak;
	uint32_t clipped;
};

struct mg_cap_record {
	int16_t peak_ddb;   /* dBFS, tenths (-999.9 for silence) */
	int16_t rms_ddb;
	int16_t floor_ddb;
	int16_t gain_ddb;   /* the software gain, dB tenths */
	uint32_t clipped;   /* samples */
	uint32_t ms;        /* audio measured */
	uint8_t gain_level; /* set_mic_gain's level (50 = tuned) */
	uint8_t codec;
	bool live;
};

#define MG_CAP_FLOOR_MS 100

/* printf a dB tenths value: "%s%d.%d" with MG_DDB(v). */
#define MG_DDB(v) ((v) < 0 ? "-" : ""), abs(v) / 10, abs(v) % 10

static inline void mg_cap_begin(struct mg_cap_meter *m, uint32_t rate)
{
	memset(m, 0, sizeof(*m));
	m->rate = rate;
}

/* After the gain; clipped: samples this block clipped at the gain or the ADC. */
static inline void mg_cap_add(struct mg_cap_meter *m, const int16_t *pcm, uint32_t n,
			      uint32_t clipped)
{
	uint32_t floor_max = m->rate * MG_CAP_FLOOR_MS / 1000;

	for (uint32_t i = 0; i < n; i++) {
		int32_t v = pcm[i];
		uint32_t a = (uint32_t)(v < 0 ? -v : v);
		uint64_t sq = (uint64_t)((int64_t)v * v);

		m->peak = a > m->peak ? a : m->peak;
		m->sumsq += sq;
		if (m->floor_n < floor_max) {
			m->floor_sumsq += sq;
			m->floor_n++;
		}
	}
	m->samples += n;
	m->clipped += clipped;
}

/* log2(x) in Q16 for x >= 1 (the squaring method: 16 fraction bits). */
static inline int32_t mg_cap_log2_q16(uint64_t x)
{
	int msb = 63;

	while (!(x >> msb)) {
		msb--;
	}
	/* Normalize to [2^31, 2^32): z / 2^31 in [1, 2). */
	uint64_t z = msb >= 31 ? x >> (msb - 31) : x << (31 - msb);
	int32_t y = msb << 16;

	for (int i = 15; i >= 0; i--) {
		z = (z * z) >> 31;
		if (z >= (1ull << 32)) {
			z >>= 1;
			y += 1 << i;
		}
	}
	return y;
}

static inline int16_t mg_cap_div_round(int64_t num, int64_t den)
{
	return (int16_t)(num >= 0 ? (num + den / 2) / den : -((-num + den / 2) / den));
}

/* An amplitude's dBFS in tenths: 200 log10(a / 32768); -9999 under 1. */
static inline int16_t mg_cap_amp_ddb(uint64_t a)
{
	if (a < 1) {
		return -9999;
	}
	/* 200 log10(2) = 60.206 tenths per octave. */
	return mg_cap_div_round((int64_t)(mg_cap_log2_q16(a) - (15 << 16)) * 60206, 65536LL * 1000);
}

/* A mean square's dBFS in tenths: 100 log10(sumsq / n / 2^30); -9999 when
 * the rms is under 1. */
static inline int16_t mg_cap_pow_ddb(uint64_t sumsq, uint32_t n)
{
	if (n == 0 || sumsq < n) {
		return -9999;
	}
	int64_t l = (int64_t)mg_cap_log2_q16(sumsq) - mg_cap_log2_q16(n) - (30LL << 16);

	return mg_cap_div_round(l * 30103, 65536LL * 1000);
}

static inline void mg_cap_end(const struct mg_cap_meter *m, struct mg_cap_record *r,
			      uint8_t gain_level, uint32_t gain_q10, uint8_t codec, bool live)
{
	r->peak_ddb = mg_cap_amp_ddb(m->peak);
	r->rms_ddb = mg_cap_pow_ddb(m->sumsq, m->samples);
	r->floor_ddb = mg_cap_pow_ddb(m->floor_sumsq, m->floor_n);
	/* The gain: 200 log10(q10 / 1024); 0 dB for 1x. */
	r->gain_ddb = gain_q10 ? mg_cap_div_round((int64_t)(mg_cap_log2_q16(gain_q10) -
							     (10 << 16)) * 60206,
						    65536LL * 1000)
			       : -9999;
	r->clipped = m->clipped;
	r->ms = m->rate ? (uint32_t)((uint64_t)m->samples * 1000 / m->rate) : 0;
	r->gain_level = gain_level;
	r->codec = codec;
	r->live = live;
}

#endif /* MG_CAP_LEVEL_H */
