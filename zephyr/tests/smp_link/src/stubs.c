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

/* What src/mg_ota.c calls outside itself, for the harness. */

#include <zephyr/kernel.h>

#include "mg_app.h"
#include "mg_audio.h"
#include "mg_boot.h"
#include "mg_connparam.h"
#include "mg_transport.h"
#include "sim.h"

bool sim_safe_mode;
bool sim_release; /* --release: mg_ota.c's release rule (MG_SMP_LINK_SIM) */

static K_THREAD_STACK_DEFINE(app_stack, 4096);
static struct k_work_q app_wq;

void sim_app_wq_start(void)
{
	k_work_queue_start(&app_wq, app_stack, K_THREAD_STACK_SIZEOF(app_stack),
			   K_PRIO_PREEMPT(5), NULL);
	k_thread_name_set(&app_wq.thread, "mg_app");
}

struct k_work_q *mg_app_wq(void)
{
	return &app_wq;
}

const char *mg_app_version(void)
{
	return "smp_link";
}

bool mg_health_safe(void)
{
	return sim_safe_mode;
}

bool sim_safe_button;

bool mg_health_safe_by_button(void)
{
	return sim_safe_mode && sim_safe_button;
}

/* No watchdog here: the last feed before an SMP reset (mg_ota.c) does nothing. */
void mg_health_feed_now(void)
{
}

void mg_health_peer_bound_clear(const char *why)
{
	ARG_UNUSED(why);
}

void mg_button_flush_injected(void)
{
}

void mg_led_set(uint32_t flag, bool on)
{
	ARG_UNUSED(flag);
	ARG_UNUSED(on);
}

/* Set up (a proof key stored; the update gate no longer reads it) and this
 * connection proven (the update gate's input). */
bool sim_set_up;
bool sim_proven;
bool sim_proven_at_connect;

bool mg_setup_store_complete(void)
{
	return sim_set_up;
}

bool mg_token_proof_matched(void)
{
	return sim_proven;
}

void mg_audio_stop(void)
{
}

void mg_connparam_busy(enum mg_xfer what)
{
	ARG_UNUSED(what);
}

void mg_ble_hold_for_reset(void)
{
	link_hold_for_reset();
}
