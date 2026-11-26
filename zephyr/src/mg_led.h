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

#ifndef MG_LED_H_
#define MG_LED_H_

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/sys/util.h>

#include "mg_led_pattern.h"

/*
 * The status LED (single-color column; mg_led_pattern.h has
 * the table and the priorities). Each change is logged ("mg.led: <pattern>")
 * on the console and the Nordic UART mirror. The LED is a plain GPIO on the
 * XIAO, not dimmable, so there is no led_brightness setting (brightness applies
 * to dimmable lights only).
 */

/* Where the client is (mg_core.c decides; same states as the ESP32 gadgets).
 * Logged; the LED itself is dark whatever the link. */
enum mg_led_link {
	MG_LED_LINK_NEVER,        /* no client since boot, no device tokens */
	MG_LED_LINK_DISCONNECTED, /* advertising */
	MG_LED_LINK_CONNECTING,   /* connected, not subscribed */
	MG_LED_LINK_SESSION,      /* a client's session, push-to-talk off */
	MG_LED_LINK_READY,        /* ...and push-to-talk on */
};

void mg_led_set(uint32_t flag, bool on);
/* Wait up to timeout for the capture light to be driven on (after
 * mg_led_set(MG_LED_STREAMING or _RECORDING, true)); true once it is. */
bool mg_led_capture_wait(int timeout_ms);
/* The LED shows the update prompt's cue now (mg_led_update_cue_shows()). */
bool mg_led_update_cue_showing(void);
void mg_led_set_link(enum mg_led_link link);
/* Logs "mg.face: <name>" with why (may be NULL). */
void mg_led_set_face(enum mg_led_face face, const char *why);
enum mg_led_face mg_led_face(void);
/* A one-shot cue (setup done, clip kept / not kept). */
void mg_led_event(enum mg_led_event e);
/* A device command's LED (device.led): on or dark for duration_ms, under a
 * pending setup confirmation and the mic's light (capture, offline
 * recording); NONE ends it now. */
void mg_led_command(enum mg_led_command c, uint32_t duration_ms);
const char *mg_led_link_name(enum mg_led_link link);
/* The push-to-talk activation "buzz": the LED lights for duration_ms. */
void mg_led_buzz(uint16_t freq_hz, uint16_t duration_ms, uint8_t volume);
void mg_led_init(void);
/* Dark for good (power off). */
void mg_led_shutdown(void);
/* Undoes mg_led_shutdown() (a power off that couldn't go ahead). */
void mg_led_resume(void);

#endif
