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

/* The button's event path (src/mg_button.c) against the host kernel shim:
 * gpio-keys reports edges from boot, so an edge before mg_button_init() (the
 * core up) is dropped, never queued on a work item that isn't set up; from
 * mg_button_init() on, each edge reaches the core in order. */

#include <stdio.h>

#include <zephyr/input/input.h>
#include <zephyr/kernel.h>

#include "check.h"
#include "mg_button.h"

extern void (*const host_input_cb)(struct input_event *, void *);

static int core_events, core_pressed, core_real;

void mg_core_button_ev(bool pressed, int64_t at_ms, bool real, bool after_arm)
{
	(void)at_ms;
	(void)after_arm;
	core_events++;
	core_pressed = pressed;
	core_real = real;
}

static void edge(int value)
{
	struct input_event e = {.type = INPUT_EV_KEY, .code = INPUT_KEY_ENTER, .value = value};

	host_input_cb(&e, NULL);
}

static void test_edges_before_init(void)
{
	printf("  button: an edge before the core is up is dropped\n");
	edge(1);
	edge(0);
	mg_button_inject(true);
	CHECK(host_null_work_submits == 0);
	CHECK(core_events == 0);
	mg_button_init();
	edge(1);
	CHECK(host_null_work_submits == 0);
	CHECK(core_events == 1 && core_pressed && core_real);
	edge(0);
	CHECK(core_events == 2 && !core_pressed && core_real);
	mg_button_inject(true);
	CHECK(core_events == 3 && core_pressed && !core_real);
}

HOST_MAIN(test_edges_before_init)
