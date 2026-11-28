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

#include "mg_button.h"

#include <zephyr/drivers/gpio.h>
#include <zephyr/input/input.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "mg_core.h"
#include "mg_input_policy.h"
#include "mg_transport.h"

LOG_MODULE_REGISTER(mg_button, CONFIG_MG_LOG_LEVEL);

/* Button events are queued so none is lost between work runs, each with
 * the time it happened (a setup press must begin after the prompt), where it
 * came from (mg_input_policy.h) and a sequence number taken when it was
 * queued: a confirm prompt drops injected events queued before it and tells
 * a real press queued before it from one after, filtered as they come out,
 * in order (never drained and queued again, which could reorder them). */
struct button_evt {
	int64_t at;
	uint32_t seq;
	uint8_t pressed;
	uint8_t real; /* from the button's GPIO, not injected */
};
K_MSGQ_DEFINE(button_q, sizeof(struct button_evt), 8, 8);
/* gpio-keys reports a press this long after its edge. */
#if DT_NODE_EXISTS(DT_ALIAS(sw0))
#define BUTTON_DEBOUNCE_MS DT_PROP_OR(DT_PARENT(DT_ALIAS(sw0)), debounce_interval_ms, 30)
#else
#define BUTTON_DEBOUNCE_MS 30
#endif
static atomic_t seq_next = ATOMIC_INIT(1);
/* Events count from mg_button_init(), once the core is up: gpio-keys reports
 * edges from boot, before the app's state exists (an edge then is dropped,
 * never queued for a core that can't take it). */
static atomic_t taking;
static atomic_t armed_seq; /* the first sequence number queued after the last prompt */
static atomic_t armed_ms;  /* when the last prompt was armed (uptime, low 32 bits) */

static void button_fn(struct k_work *w)
{
	struct button_evt e;

	ARG_UNUSED(w);
	while (k_msgq_get(&button_q, &e, K_NO_WAIT) == 0) {
		bool after_arm = mg_input_after_arm(
			(int32_t)(e.seq - (uint32_t)atomic_get(&armed_seq)) >= 0, e.real, e.pressed,
			(uint32_t)e.at, (uint32_t)atomic_get(&armed_ms), BUTTON_DEBOUNCE_MS);

		if (!mg_input_keep_at_prompt(e.real, after_arm)) {
			LOG_INF("button: an injected %s queued before a confirm prompt dropped",
				e.pressed ? "press" : "release");
			continue;
		}
		LOG_INF("button %s%s", e.pressed ? "down" : "up", e.real ? "" : " (injected)");
		mg_core_button_ev(e.pressed, e.at, e.real, after_arm);
	}
}

/* Defined, not initialized at run time: an edge can come before any init. */
static K_WORK_DEFINE(button_work, button_fn);

static void put(bool pressed, bool real)
{
	if (!atomic_get(&taking)) {
		LOG_INF("button: %s%s before the core is up: dropped", pressed ? "press" : "release",
			real ? "" : " (injected)");
		return;
	}
	struct button_evt v = {.at = k_uptime_get(),
			       .seq = (uint32_t)atomic_inc(&seq_next),
			       .pressed = pressed,
			       .real = real};

	if (k_msgq_put(&button_q, &v, K_NO_WAIT) == 0) {
		k_work_submit_to_queue(mg_app_wq(), &button_work);
	}
}

/* A confirm prompt was armed (mg_setup.c via mg_core.c): everything queued
 * before now counts as before the prompt. */
void mg_button_flush_injected(void)
{
	atomic_set(&armed_ms, (atomic_val_t)k_uptime_get_32());
	atomic_set(&armed_seq, atomic_get(&seq_next));
}

void mg_button_inject(bool pressed)
{
	put(pressed, false); /* the bench console: never the GPIO */
}

static void input_cb(struct input_event *evt, void *user_data)
{
	ARG_UNUSED(user_data);
	if (evt->type == INPUT_EV_KEY && evt->code == INPUT_KEY_ENTER) {
		put(evt->value != 0, true); /* gpio-keys: the real button */
	}
}

INPUT_CALLBACK_DEFINE(NULL, input_cb, NULL);

bool mg_button_held_at_boot(void)
{
#if DT_NODE_EXISTS(DT_ALIAS(sw0))
	static const struct gpio_dt_spec sw = GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios);

	if (!gpio_is_ready_dt(&sw)) {
		return false;
	}
	if (!MG_SW0_GPIO_KEYS) {
		(void)gpio_pin_configure_dt(&sw, GPIO_INPUT);
	}
	return gpio_pin_get_dt(&sw) == 1;
#else
	return false;
#endif
}

void mg_button_init(void)
{
	atomic_set(&taking, 1);
}
