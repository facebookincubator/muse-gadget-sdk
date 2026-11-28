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

/* Did a talk capture rise above room noise? (Bench safety: test and
 * automation builds drop a talk capture that never does, and show "Didn't
 * hear anything", so a scripted hold never sends a silent or room-only turn;
 * release builds send every hold.)
 *
 * The capture is cut into MG_VOICE_WIN_MS windows. The room's level is the
 * quietest window so far (a hold starts in the room, before anyone speaks;
 * speech with pauses keeps finding it). Voice is MG_VOICE_RUN windows in a row
 * at least MG_VOICE_RISE_DB over that floor and over MG_VOICE_MIN_DBFS, so a
 * click (the button's own, one window) or a steady hum never counts. Host
 * test_voice. */

#ifndef MG_VOICE_H
#define MG_VOICE_H

#include <stdbool.h>
#include <stdint.h>

#define MG_VOICE_WIN_MS 20
#define MG_VOICE_RUN 3          /* windows in a row: 60 ms */
#define MG_VOICE_RISE_DB 12     /* over the room */
#define MG_VOICE_MIN_DBFS (-50) /* and over this, whatever the room */
/* As mean squares of int16 samples: 12 dB is x15.849, and -50 dBFS is
 * (32768 * 10^(-50/20))^2 = 10737. */
#define MG_VOICE_RISE_X1000 15849u
#define MG_VOICE_MIN_MS 10737u

struct mg_voice {
	uint32_t win_samples;
	uint32_t n;      /* samples in the window so far */
	uint64_t sumsq;  /* of the window so far */
	uint64_t floor;  /* the quietest whole window's mean square (0: none yet) */
	bool have_floor;
	uint8_t run;     /* loud windows in a row */
	bool seen;
};

static inline void mg_voice_init(struct mg_voice *v, uint32_t rate)
{
	*v = (struct mg_voice){.win_samples = rate * MG_VOICE_WIN_MS / 1000};
	if (v->win_samples == 0) {
		v->win_samples = 1;
	}
}

/* One window's mean square: is it loud, then does it set the floor. */
static inline void mg_voice_window(struct mg_voice *v, uint64_t ms)
{
	bool loud = v->have_floor && ms >= MG_VOICE_MIN_MS &&
		    ms * 1000u >= v->floor * MG_VOICE_RISE_X1000;

	v->run = loud ? (uint8_t)(v->run + 1) : 0;
	if (v->run >= MG_VOICE_RUN) {
		v->seen = true;
	}
	if (!v->have_floor || ms < v->floor) {
		v->floor = ms;
		v->have_floor = true;
	}
}

/* Feeds samples (what the encoder gets); true once voice was seen. */
static inline bool mg_voice_add(struct mg_voice *v, const int16_t *pcm, uint32_t count)
{
	for (uint32_t i = 0; i < count && !v->seen; i++) {
		v->sumsq += (uint64_t)((int32_t)pcm[i] * (int32_t)pcm[i]);
		if (++v->n == v->win_samples) {
			mg_voice_window(v, v->sumsq / v->n);
			v->n = 0;
			v->sumsq = 0;
		}
	}
	return v->seen;
}

/* A finished talk turn is dropped unsent only in a test or automation build
 * and only when no voice was seen. */
static inline bool mg_voice_turn_dropped(bool automation, bool voice_seen)
{
	return automation && !voice_seen;
}

#endif /* MG_VOICE_H */
