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

#include "mg_led_pattern.h"

#include <stddef.h>

static const struct mg_led_pattern pat_dark = {"idle (dark)", {0}, 0, false};
static const struct mg_led_pattern pat_heartbeat = {"idle (heartbeat)", {20, 3980}, 2, false};
static const struct mg_led_pattern pat_pending = {"confirm setup", {50, 50}, 2, false};
/* The setup reset's own cue, a 10 Hz flicker unlike any other. */
static const struct mg_led_pattern pat_reset_armed = {"setup reset: release now", {30, 70}, 2, false};
/* The boot hold's SAFE point: a double blink, unlike any other. */
static const struct mg_led_pattern pat_safe_armed = {
	"SAFE mode: release now", {100, 100, 100, 700}, 4, false};
/* The bench switch's visible "Bench" mark on a screenless gadget: two
 * 30 ms flashes 100 ms apart every 4 s,
 * lowest priority. */
static const struct mg_led_pattern pat_bench = {"bench", {30, 100, 30, 3840}, 4, false};
/* device.led (a device command over the device session). */
static const struct mg_led_pattern pat_command_on = {"command: on", {1}, 1, false};
static const struct mg_led_pattern pat_command_off = {"command: off", {0}, 0, false};
static const struct mg_led_pattern pat_capture = {"mic on", {1}, 1, false};
/* The update prompt: two short blinks every 0.8 s, unlike the capture light (solid) and
 * the setup prompt (50/50). */
static const struct mg_led_pattern pat_update_prompt = {
	"update confirm", {120, 120, 120, 440}, 4, false};
static const struct mg_led_pattern pat_setup_done = {
	"setup done", {60, 140, 60, 140, 60, 140}, 6, true};
static const struct mg_led_pattern pat_kept = {"clip kept", {100, 150, 100, 150}, 4, true};
static const struct mg_led_pattern pat_not_kept = {
	"clip not kept", {100, 150, 100, 150, 100, 150, 100, 150}, 8, true};
static const struct mg_led_pattern pat_error = {
	"error", {100, 100, 100, 100, 100, 100}, 6, true};
static const struct mg_led_pattern pat_done = {"done", {60, 140, 60, 140, 60, 140}, 6, true};
static const struct mg_led_pattern pat_thinking = {"thinking", {150, 850}, 2, false};
static const struct mg_led_pattern pat_responding = {"responding", {500, 500}, 2, false};
static const struct mg_led_pattern pat_low_battery = {"low battery", {80, 29920}, 2, false};

const struct mg_led_pattern *mg_led_event_pattern(enum mg_led_event e)
{
	switch (e) {
	case MG_LED_EVENT_SETUP_DONE:
		return &pat_setup_done;
	case MG_LED_EVENT_CLIP_KEPT:
		return &pat_kept;
	case MG_LED_EVENT_NOT_KEPT:
		return &pat_not_kept;
	default:
		return NULL;
	}
}

const struct mg_led_pattern *mg_led_face_pattern(enum mg_led_face f)
{
	switch (f) {
	case MG_LED_FACE_THINKING:
		return &pat_thinking;
	case MG_LED_FACE_RESPONDING:
		return &pat_responding;
	case MG_LED_FACE_DONE:
		return &pat_done;
	case MG_LED_FACE_ERROR:
		return &pat_error;
	default:
		return NULL;
	}
}

/* The pattern the capture shows (live or offline: both solid). */
const struct mg_led_pattern *mg_led_capture_pattern(void)
{
	return &pat_capture;
}

const struct mg_led_pattern *mg_led_pick(const struct mg_led_inputs *in)
{
	const struct mg_led_pattern *p;

	if (in->flags & MG_LED_RESET_ARMED) {
		return &pat_reset_armed;
	}
	if (in->flags & MG_LED_SAFE_ARMED) {
		return &pat_safe_armed;
	}
	if (in->flags & MG_LED_PAIR_PENDING) {
		return &pat_pending;
	}
	/* The mic's light wins over a remote LED command (device.led): it can't
	 * be darkened or changed while the mic captures or records offline;
	 * the command shows once the capture ends, if its time remains. */
	if (in->flags & MG_LED_STREAMING) {
		return &pat_capture;
	}
	if (in->flags & MG_LED_RECORDING) {
		return &pat_capture; /* solid, as the live capture's */
	}
	/* The update prompt never covers the mic's light: it shows
	 * once the capture ends, within its 30 s. */
	if (in->flags & MG_LED_UPDATE_PROMPT) {
		return &pat_update_prompt;
	}
	if (in->command == MG_LED_COMMAND_ON) {
		return &pat_command_on;
	}
	if (in->command == MG_LED_COMMAND_OFF) {
		return &pat_command_off;
	}
	p = mg_led_event_pattern(in->event);
	if (p) {
		return p;
	}
	p = mg_led_face_pattern(in->face);
	if (p) {
		return p;
	}
	if (in->flags & MG_LED_LOW_BATTERY) {
		return &pat_low_battery;
	}
	if (in->flags & MG_LED_BENCH) {
		return &pat_bench;
	}
	return in->heartbeat ? &pat_heartbeat : &pat_dark;
}

uint32_t mg_led_pattern_ms(const struct mg_led_pattern *p)
{
	uint32_t t = 0;

	if (p->n < 2) {
		return 0;
	}
	for (int i = 0; i < p->n; i++) {
		t += p->ms[i];
	}
	return t;
}

bool mg_led_pattern_on(const struct mg_led_pattern *p, uint32_t t)
{
	uint32_t total = mg_led_pattern_ms(p);

	if (p->n == 0) {
		return false;
	}
	if (p->n == 1) {
		return true;
	}
	if (p->once && t >= total) {
		return false;
	}
	t %= total;
	for (int i = 0; i < p->n; i++) {
		if (t < p->ms[i]) {
			return (i & 1) == 0;
		}
		t -= p->ms[i];
	}
	return false;
}

int32_t mg_led_pattern_next(const struct mg_led_pattern *p, uint32_t t)
{
	uint32_t total = mg_led_pattern_ms(p);

	if (p->n < 2 || (p->once && t >= total)) {
		return -1;
	}
	uint32_t c = t % total;

	for (int i = 0; i < p->n; i++) {
		if (c < p->ms[i]) {
			uint32_t left = p->ms[i] - c;

			return (int32_t)left;
		}
		c -= p->ms[i];
	}
	return 1;
}
