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

#ifndef MG_RESET_HOLD_H
#define MG_RESET_HOLD_H

#include <stdbool.h>
#include <stdint.h>

/*
 * The setup reset gesture (a 5 s talk hold from
 * System OFF erased the setup on the XIAO and other nRF54L15 boards).
 *
 * 1. Only on a cold boot: power-on (no reset reason), the reset pin, a
 *    power-on reset. Never on a wake from System OFF (a talk from off), a
 *    software reset (an update), a watchdog, a lockup or the debugger.
 * 2. It commits only on release: hold CONFIG_MG_RESET_HOLD_MS, the LED's own
 *    cue says "release now", then release within CONFIG_MG_RESET_RELEASE_MS.
 *    A release before the countdown or a hold past the window does nothing,
 *    so a long talk hold never resets. A board can add conditions
 *    (mg_board_reset_hold_allowed(): e.g. only with the charger present).
 * 3. On a board that allows it (mg_board_safe_hold_allowed(),
 *    CONFIG_MG_SAFE_HOLD; recovery tier 3 by button), the same hold passes a SAFE point
 *    first: at CONFIG_MG_SAFE_HOLD_MS (3 s) the SAFE cue, and a release
 *    before the reset's countdown ends boots once in SAFE mode. One
 *    timeline: release < 3 s nothing, [3, 5) s SAFE, [5, 10) s reset,
 *    10 s or more nothing. A release commits only what the cue showed.
 * Portable; host-tested (tests/host/test_gadget.c).
 */

/* Zephyr's hwinfo reset-cause flags (drivers/hwinfo.h), for host builds. */
#ifndef RESET_PIN
#define RESET_PIN            (1u << 0)
#define RESET_SOFTWARE       (1u << 1)
#define RESET_BROWNOUT       (1u << 2)
#define RESET_POR            (1u << 3)
#define RESET_WATCHDOG       (1u << 4)
#define RESET_DEBUG          (1u << 5)
#define RESET_LOW_POWER_WAKE (1u << 7)
#define RESET_CPU_LOCKUP     (1u << 8)
#endif

/* A boot where key gestures count: none of the warm causes. */
static inline bool mg_reset_hold_cold(uint32_t cause)
{
	const uint32_t warm = RESET_LOW_POWER_WAKE | RESET_SOFTWARE | RESET_WATCHDOG |
			      RESET_CPU_LOCKUP | RESET_DEBUG;

	return (cause & warm) == 0;
}

enum mg_rh_event {
	MG_RH_NONE,     /* keep going */
	MG_RH_SAFE_ARM, /* the SAFE point: show the SAFE cue */
	MG_RH_SAFE,     /* released after the SAFE cue: boot once in SAFE mode */
	MG_RH_ARM,      /* the countdown is over: show "release now" */
	MG_RH_COMMIT,   /* released in the window: reset */
	MG_RH_CANCEL,   /* released early, or held past the window: nothing */
};

struct mg_rh {
	bool safe_armed;
	bool armed;
	bool done;
	uint32_t armed_at;
};

/* One look at the key, t_ms after the countdown began. safe_ms 0: no SAFE
 * point (the default board). */
static inline enum mg_rh_event mg_reset_hold_step(struct mg_rh *h, bool held, uint32_t t_ms,
						  uint32_t safe_ms, uint32_t hold_ms,
						  uint32_t window_ms)
{
	if (h->done) {
		return MG_RH_NONE;
	}
	if (!h->armed) {
		if (!held) {
			h->done = true;
			/* What the cue showed: SAFE if its cue is up, else nothing. */
			return h->safe_armed ? MG_RH_SAFE : MG_RH_CANCEL;
		}
		if (t_ms >= hold_ms) {
			h->safe_armed = false;
			h->armed = true;
			h->armed_at = t_ms;
			return MG_RH_ARM;
		}
		if (safe_ms && !h->safe_armed && t_ms >= safe_ms) {
			h->safe_armed = true;
			return MG_RH_SAFE_ARM;
		}
		return MG_RH_NONE;
	}
	/* The window first: a release at its end (a 10 s hold) is too late. */
	if (t_ms - h->armed_at >= window_ms) {
		h->done = true;
		return MG_RH_CANCEL;
	}
	if (!held) {
		h->done = true;
		return MG_RH_COMMIT;
	}
	return MG_RH_NONE;
}

#endif /* MG_RESET_HOLD_H */
