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

#include <errno.h>

#include <zephyr/kernel.h>

#include "mg_audio.h"

/*
 * Synthetic source: a 1 kHz tone (exactly 16 samples per period at 16 kHz,
 * 24 at 24 kHz) whose level steps every 100 ms, delivered in real time.
 * Simulation and the tests check for it after decoding.
 */

static const int16_t sine16[16] = {
	0, 957, 1768, 2310, 2500, 2310, 1768, 957, 0, -957, -1768, -2310, -2500, -2310, -1768, -957,
};

#if defined(CONFIG_MG_LC3_24K)
/* 2500 * sin(2 pi k / 24) */
static const int16_t sine24[24] = {
	0, 653, 1250, 1768, 2165, 2415, 2500, 2415, 2165, 1768, 1250, 653,
	0, -653, -1250, -1768, -2165, -2415, -2500, -2415, -2165, -1768, -1250, -653,
};
#endif

static int64_t t0;
static uint32_t n;
static uint16_t rate;

int mg_audio_src_start(uint16_t r)
{
	if (r != 16000 && !(IS_ENABLED(CONFIG_MG_LC3_24K) && r == 24000)) {
		return -ENOTSUP;
	}
	rate = r;
	t0 = k_uptime_get();
	n = 0;
	return 0;
}

int mg_audio_src_read(int16_t *pcm, int timeout_ms)
{
	int64_t due = t0 + (int64_t)(n + 1) * 10;
	int64_t now = k_uptime_get();

	if (due - now > timeout_ms) {
		k_sleep(K_MSEC(timeout_ms));
		return -EAGAIN;
	}
	if (due > now) {
		k_sleep(K_MSEC(due - now));
	}
	/* The room first (1/16 scale for 40 ms: the start of a hold, inside the
	 * central's 50 ms it skips), then the
	 * level cycles through 1/2, 3/4 and full scale every 100 ms, so a test
	 * build's silent-turn check (mg_voice.h) hears voice. */
	int step = (n / 10) % 3;
	int num = step == 0 ? 2 : (step == 1 ? 3 : 4);
	int den = n < 4 ? 32 : 4;

	int ns = rate / 100;

	for (int i = 0; i < ns; i++) {
#if defined(CONFIG_MG_LC3_24K)
		if (rate == 24000) {
			pcm[i] = (int16_t)(sine24[(n * ns + i) % 24] * num / den);
			continue;
		}
#endif
		pcm[i] = (int16_t)(sine16[(n * ns + i) & 15] * num / den);
	}
	n++;
	return ns;
}

void mg_audio_src_stop(void)
{
}
