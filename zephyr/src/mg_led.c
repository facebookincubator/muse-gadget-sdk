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

#include "mg_led.h"

#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(mg_led, CONFIG_MG_LOG_LEVEL);

#if DT_NODE_EXISTS(DT_ALIAS(led0))
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
#define HAVE_LED 1
#else
#define HAVE_LED 0
#endif

/* An assistant turn without another assistant_state falls back to idle
 * (mgcommands.h: 120 s, for tool gaps). */
#define TURN_MS 120000

/* Callers on any thread (the app work queue, the audio thread, setup) only
 * set atomics and kick the tick; the tick (one work item, never concurrent
 * with itself) is the only reader and writer of the plain state below them:
 * the until-times and the playing pattern (a race
 * between the audio thread and the work queue on pat and event_until,
 * found by ThreadSanitizer). tests/host/run.py checks the rule. */
static atomic_t flags;
static atomic_t link = MG_LED_LINK_NEVER;
static atomic_t face = MG_LED_FACE_IDLE;
static atomic_t face_new;    /* a face was set: the tick times it, restarts the pattern */
static atomic_t event = MG_LED_EVENT_NONE;
static atomic_t event_new;   /* an event cue was asked for */
static atomic_t command = MG_LED_COMMAND_NONE;
static atomic_t command_ms;
static atomic_t command_new;
static atomic_t buzz_ms;
static atomic_t buzz_new;
static atomic_t dark;
/* Tick-owned (tick_fn only). */
static int64_t face_until;
static int64_t event_until;
static int64_t command_until;
static int64_t buzz_until;
static const struct mg_led_pattern *pat;
static int64_t pat_start;
static const struct mg_led_pattern *logged;
static struct k_work_delayable tick;

static const char *const FACE_NAMES[] = {"idle", "thinking", "responding", "done", "error"};
static const char *const LINK_NAMES[] = {"never connected", "disconnected", "connecting",
					 "app connected", "ready"};

const char *mg_led_link_name(enum mg_led_link l)
{
	return l < ARRAY_SIZE(LINK_NAMES) ? LINK_NAMES[l] : "?";
}

static int out(bool on)
{
#if HAVE_LED
	return gpio_pin_set_dt(&led, on);
#else
	ARG_UNUSED(on);
	return 0;
#endif
}

/* The capture light is actually on (the light
 * shows before the first outbound real-mic frame): set by the tick once it
 * drove the capture pattern on, cleared when the capture flags go. */
static atomic_t capture_lit;
static K_SEM_DEFINE(capture_lit_sem, 0, 1);

/* Loops until the light is confirmed or the time is up: a give left over
 * from an earlier capture (confirmed before its wait began) must not end
 * this wait early. */
bool mg_led_update_cue_showing(void)
{
	return mg_led_update_cue_shows((uint32_t)atomic_get(&flags));
}

bool mg_led_capture_wait(int timeout_ms)
{
	int64_t end = k_uptime_get() + timeout_ms;

	while (!atomic_get(&capture_lit)) {
		int64_t left = end - k_uptime_get();

		if (left <= 0) {
			return false;
		}
		(void)k_sem_take(&capture_lit_sem, K_MSEC(left));
	}
	return true;
}

static int64_t soonest(int64_t a, int64_t b)
{
	if (a < 0) {
		return b;
	}
	if (b < 0) {
		return a;
	}
	return MIN(a, b);
}

static void tick_fn(struct k_work *w)
{
	int64_t now = k_uptime_get();

	ARG_UNUSED(w);
	if (atomic_get(&dark)) {
		out(false);
		return;
	}
	/* What the callers asked for since the last tick. */
	if (atomic_cas(&face_new, 1, 0)) {
		enum mg_led_face f = atomic_get(&face);

		if (f == MG_LED_FACE_THINKING || f == MG_LED_FACE_RESPONDING) {
			face_until = now + TURN_MS;
		} else if (f == MG_LED_FACE_DONE || f == MG_LED_FACE_ERROR) {
			face_until = now + mg_led_pattern_ms(mg_led_face_pattern(f));
		} else {
			face_until = 0;
		}
		pat = NULL; /* a new turn state starts its pattern over */
	}
	if (atomic_cas(&event_new, 1, 0)) {
		const struct mg_led_pattern *ep = mg_led_event_pattern(atomic_get(&event));

		event_until = ep ? now + mg_led_pattern_ms(ep) : 0;
		pat = NULL;
	}
	if (atomic_cas(&command_new, 1, 0)) {
		command_until = atomic_get(&command) == MG_LED_COMMAND_NONE
					? 0
					: now + MAX((uint32_t)atomic_get(&command_ms), 1U);
	}
	if (atomic_cas(&buzz_new, 1, 0)) {
		buzz_until = now + (uint32_t)atomic_get(&buzz_ms);
	}
	if (face_until && now >= face_until && !atomic_get(&face_new)) {
		enum mg_led_face was = atomic_get(&face);

		face_until = 0;
		/* Unless a caller set another face meanwhile. */
		if (atomic_cas(&face, was, MG_LED_FACE_IDLE)) {
			if (was == MG_LED_FACE_THINKING || was == MG_LED_FACE_RESPONDING) {
				LOG_INF("mg.face: idle (no assistant_state for %d s)",
					TURN_MS / 1000);
			} else {
				LOG_INF("mg.face: idle");
			}
		}
	}
	if (event_until && now >= event_until) {
		event_until = 0;
		atomic_set(&event, MG_LED_EVENT_NONE);
	}
	if (command_until && now >= command_until) {
		command_until = 0;
		atomic_set(&command, MG_LED_COMMAND_NONE);
		LOG_INF("mg.led: command over");
	}
	if (buzz_until > now) {
		out(true);
		k_work_reschedule(&tick, K_MSEC(buzz_until - now));
		return;
	}
	const struct mg_led_inputs in = {
		.flags = (uint32_t)atomic_get(&flags),
		.face = atomic_get(&face),
		.event = atomic_get(&event),
		.command = atomic_get(&command),
		.heartbeat = IS_ENABLED(CONFIG_MG_LED_IDLE_HEARTBEAT),
	};
	const struct mg_led_pattern *p = mg_led_pick(&in);

	if (p != pat) {
		pat = p;
		pat_start = now;
		if (p != logged) {
			logged = p;
			LOG_INF("mg.led: %s", p->name);
		}
	}
	uint32_t t = (uint32_t)(now - pat_start);
	bool on = mg_led_pattern_on(pat, t);
	int rc = out(on);
	bool capturing = (in.flags & (MG_LED_STREAMING | MG_LED_RECORDING)) != 0;

	if (!capturing) {
		atomic_set(&capture_lit, 0);
	} else if (on && rc == 0 && (p == mg_led_capture_pattern()) && !atomic_get(&capture_lit)) {
		atomic_set(&capture_lit, 1);
		k_sem_give(&capture_lit_sem);
	}
	int64_t next = mg_led_pattern_next(pat, t);

	next = soonest(next, face_until ? face_until - now : -1);
	next = soonest(next, event_until ? event_until - now : -1);
	next = soonest(next, command_until ? command_until - now : -1);
	if (next >= 0) {
		k_work_reschedule(&tick, K_MSEC(MAX(next, 1)));
	}
}

static void kick(void)
{
	k_work_reschedule(&tick, K_NO_WAIT);
}

void mg_led_set(uint32_t flag, bool on)
{
	if (flag == MG_LED_STREAMING && on && !(atomic_get(&flags) & MG_LED_STREAMING)) {
		LOG_INF("mg.face: listening");
	}
	if (on) {
		atomic_or(&flags, flag);
	} else {
		atomic_and(&flags, ~flag);
	}
	kick();
}

void mg_led_set_link(enum mg_led_link l)
{
	atomic_set(&link, l);
	kick();
}

void mg_led_set_face(enum mg_led_face f, const char *why)
{
	atomic_set(&face, f);
	atomic_set(&face_new, 1); /* the tick times it */
	if (why) {
		LOG_INF("mg.face: %s (%s)", FACE_NAMES[f], why);
	} else {
		LOG_INF("mg.face: %s", FACE_NAMES[f]);
	}
	kick();
}

enum mg_led_face mg_led_face(void)
{
	return atomic_get(&face);
}

void mg_led_command(enum mg_led_command c, uint32_t duration_ms)
{
	atomic_set(&command_ms, (atomic_val_t)duration_ms);
	atomic_set(&command, c);
	atomic_set(&command_new, 1);
	kick();
}

void mg_led_event(enum mg_led_event e)
{
	const struct mg_led_pattern *p = mg_led_event_pattern(e);

	if (!p) {
		return;
	}
	atomic_set(&event, e);
	atomic_set(&event_new, 1);
	kick();
}

void mg_led_buzz(uint16_t freq_hz, uint16_t duration_ms, uint8_t volume)
{
	/* No buzzer on this board: the LED lights for the buzz duration.
	 * Frequency and volume are accepted and stored but have no effect. */
	ARG_UNUSED(freq_hz);
	if (volume == 0) {
		return;
	}
	atomic_set(&buzz_ms, duration_ms);
	atomic_set(&buzz_new, 1);
	k_work_reschedule(&tick, K_NO_WAIT);
}

void mg_led_shutdown(void)
{
	atomic_set(&dark, 1);
	(void)k_work_cancel_delayable(&tick);
	out(false);
}

void mg_led_resume(void)
{
	atomic_set(&dark, 0);
	k_work_reschedule(&tick, K_NO_WAIT);
}

void mg_led_init(void)
{
	k_work_init_delayable(&tick, tick_fn);
#if HAVE_LED
	if (gpio_is_ready_dt(&led)) {
		gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);
	}
#endif
	k_work_reschedule(&tick, K_NO_WAIT);
}
