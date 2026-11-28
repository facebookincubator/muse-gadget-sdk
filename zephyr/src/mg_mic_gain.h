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


#ifndef MG_MIC_GAIN_H_
#define MG_MIC_GAIN_H_

#include <stdint.h>

/*
 * set_mic_gain's level (1-100, mgcommands.h) onto the software gain, in Q10
 * (1024 = unity; at most 16x so a sample times it fits an int32).
 * Monotonic and through the board's tuned gain: def_level gives def_q10
 * exactly; below it the gain falls linearly in dB to -30 dB at 1; above it
 * it rises linearly in dB to top_db at 100 (100 = the highest gain at which
 * normal speech 30 cm away doesn't clip). Integer only (no libm): 1 dB
 * steps from a table, interpolated within a step (under 0.06 dB off).
 * Levels outside 1-100 are the caller's to refuse (invalid_value).
 */
#define MG_MIC_GAIN_LOW_CDB 3000 /* -30.00 dB at level 1 */

/* A level set_mic_gain takes (else invalid_value). */
static inline int mg_mic_gain_level_ok(unsigned int level)
{
	return level >= 1 && level <= 100;
}

/* 10^(cdb / 2000) in Q12, for 0 <= cdb <= 3000 (hundredths of a dB). */
static inline uint32_t mg_mic_gain_up_q12(uint32_t cdb)
{
	static const uint32_t t[31] = {
		4096,  4596,  5157,  5786,  6492,  7284,  8173,  9170,   10289,  11544, 12953,
		14533, 16306, 18296, 20529, 23034, 25844, 28997, 32536,  36506,  40960, 45958,
		51566, 57858, 64917, 72838, 81726, 91698, 102887, 115441, 129527};
	uint32_t k = cdb / 100, f = cdb % 100;

	if (k >= 30) {
		return t[30];
	}
	return t[k] + (t[k + 1] - t[k]) * f / 100;
}

static inline uint32_t mg_mic_gain_q10(uint8_t level, uint8_t def_level, uint32_t def_q10,
				       unsigned int top_db)
{
	uint64_t q;

	if (level < 1) {
		level = 1;
	}
	if (level > 100) {
		level = 100;
	}
	if (level == def_level) {
		return def_q10;
	}
	if (level < def_level) {
		uint32_t cdb = MG_MIC_GAIN_LOW_CDB * (uint32_t)(def_level - level) /
			       (uint32_t)(def_level - 1);

		q = ((uint64_t)def_q10 * 4096U + mg_mic_gain_up_q12(cdb) / 2) /
		    mg_mic_gain_up_q12(cdb);
	} else {
		uint32_t cdb = top_db * 100U * (uint32_t)(level - def_level) /
			       (uint32_t)(100 - def_level);

		q = ((uint64_t)def_q10 * mg_mic_gain_up_q12(cdb) + 2048U) / 4096U;
	}
	if (q > 16U * 1024U) {
		q = 16U * 1024U;
	}
	return q < 1 ? 1U : (uint32_t)q;
}

#endif
