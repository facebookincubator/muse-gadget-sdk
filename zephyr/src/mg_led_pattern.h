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

#ifndef MG_LED_PATTERN_H_
#define MG_LED_PATTERN_H_

#include <stdbool.h>
#include <stdint.h>

/*
 * The single-color LED language (the Single-color LED
 * column), as data, and the rule that picks one. Portable: mg_led.c plays
 * it, the host tests check it.
 *
 * Highest first:
 *   setup confirm      fast blink 50/50 (the Muse code owns it, first setup too);
 *                      setting a set-up gadget up again: solid on while the
 *                      confirming hold is under way
 *   mic captures       solid on (the app's capture, pre-roll included)
 *   recording offline  solid (the capture's light: any capture that leaves the gadget)
 *   device command     device.led over the device session: solid on or dark,
 *                      for its duration (shown once the mic is off)
 *   one-shot events    setup done 3 quick blinks, clip kept 2, not kept 4
 *   the turn           error 3 fast blinks (100/100), done 3 quick blinks,
 *                      thinking 150/850, responding 500/500
 *   low battery        one 80 ms flash every 30 s
 *   idle               dark (connected or not; CONFIG_MG_LED_IDLE_HEARTBEAT
 *                      adds a 20 ms blink every 4 s for bench boards)
 */

struct mg_led_pattern {
	const char *name;
	/* On/off durations in ms, starting with on; n entries. n == 0: dark;
	 * n == 1: solid on. */
	uint16_t ms[8];
	uint8_t n;
	/* Plays once (then the next pattern down shows), else repeats. */
	bool once;
};

/* Turn state (mg_command_assistant_state, and thinking at stop_mic). */
enum mg_led_face {
	MG_LED_FACE_IDLE,
	MG_LED_FACE_THINKING,   /* until responding, done or error; 60 s at most */
	MG_LED_FACE_RESPONDING, /* until done or error; 60 s at most */
	MG_LED_FACE_DONE,       /* 3 quick blinks, then idle */
	MG_LED_FACE_ERROR,      /* 3 fast blinks, then idle */
};

/* One-shot cues. */
enum mg_led_event {
	MG_LED_EVENT_NONE,
	MG_LED_EVENT_SETUP_DONE, /* Link setup stored the tokens: 3 quick blinks */
	MG_LED_EVENT_CLIP_KEPT,  /* an offline press was recorded: 2 blinks */
	MG_LED_EVENT_NOT_KEPT,   /* an offline press had nowhere to go: 4 blinks */
};

/* What a device command (device.led over the device session) shows, until
 * it ends after its duration. */
enum mg_led_command {
	MG_LED_COMMAND_NONE, /* back to the state's pattern */
	MG_LED_COMMAND_ON,   /* solid on */
	MG_LED_COMMAND_OFF,  /* dark */
};

/* Flags (mg_led_set()). */
#define MG_LED_STREAMING    (1u << 2) /* the mic captures for the app */
#define MG_LED_RECORDING    (1u << 3) /* recording an offline clip */
#define MG_LED_PAIR_PENDING (1u << 4) /* Link setup waits for the press */
#define MG_LED_LOW_BATTERY  (1u << 5) /* 10 % or less */
#define MG_LED_RESET_ARMED  (1u << 6) /* setup reset: release now */
/* Bit 9: 7 and 8 stay free for a board's own flags (e.g. charging, charged). */
#define MG_LED_SAFE_ARMED   (1u << 9) /* boot hold: release now for SAFE mode */
#define MG_LED_UPDATE_PROMPT (1u << 12) /* press once to allow a firmware update */
#define MG_LED_BENCH        (1u << 10) /* The bench switch is on (lowest priority) */

struct mg_led_inputs {
	uint32_t flags;
	enum mg_led_face face;
	enum mg_led_event event;
	enum mg_led_command command;
	bool heartbeat; /* CONFIG_MG_LED_IDLE_HEARTBEAT */
};

const struct mg_led_pattern *mg_led_pick(const struct mg_led_inputs *in);
/* The update prompt's cue is what the LED shows now (mg_led_pick()'s order:
 * no reset/SAFE cue, no setup prompt, no mic light above it): only then does
 * a press answer the update prompt, so a setup-prompt press never approves an
 * update the LED doesn't show. */
static inline bool mg_led_update_cue_shows(uint32_t flags)
{
	return (flags & MG_LED_UPDATE_PROMPT) &&
	       !(flags & (MG_LED_RESET_ARMED | MG_LED_SAFE_ARMED | MG_LED_PAIR_PENDING |
			  MG_LED_STREAMING | MG_LED_RECORDING));
}
/* The capture's pattern (solid; live capture and offline recording alike). */
const struct mg_led_pattern *mg_led_capture_pattern(void);
/* Total length of a pattern (one cycle) in ms. */
uint32_t mg_led_pattern_ms(const struct mg_led_pattern *p);
/* Whether the LED is on t ms into the pattern (a once pattern is dark
 * after its end). */
bool mg_led_pattern_on(const struct mg_led_pattern *p, uint32_t t);
/* ms from t until the LED next changes, or -1 if it never does (solid,
 * dark, or a once pattern that has ended). */
int32_t mg_led_pattern_next(const struct mg_led_pattern *p, uint32_t t);
const struct mg_led_pattern *mg_led_event_pattern(enum mg_led_event e);
const struct mg_led_pattern *mg_led_face_pattern(enum mg_led_face f);

#endif
