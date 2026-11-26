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

/*
 * The gadget's basics on the host: the clock device actions, the battery
 * report, the LED command against the capture light, low battery, the boot
 * record and SAFE mode, the setup reset hold, the held capture output and
 * the Nordic UART's word policy.
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>

#include "check.h"
#include "mg_boot.h"
#include "mg_clock.h"
#include "mg_led_pattern.h"
#include "mg_reach_policy.h"
#include "mg_input_policy.h"
#include "mg_evt_policy.h"
#include "mg_tx_progress.h"
#include <stdlib.h>

#include "mg_reset_hold.h"
#include "mg_out_hold.h"
#include "mg_power.h"
#include "mg_nus_policy.h"
#include "mg_owner_reads.h"
#include "mg_ota_policy.h"
#include "mg_settings_policy.h"
#include "mgcommands.h"
#include "mg_smp_depth.h"
#include "mg_mic_policy.h"
#include "mg_safe_policy.h"
#include "mg_resetup_policy.h"
#include "mg_health.h"


static int64_t le64(const uint8_t *p)
{
	uint64_t u = 0;

	for (int i = 0; i < 8; i++) {
		u |= (uint64_t)p[i] << (8 * i);
	}
	return (int64_t)u;
}

static uint16_t clock_rx(const uint8_t *a, size_t n, uint8_t *out, size_t *len)
{
	return mg_clock_action(a, n, true, out, len);
}

/* mg_clock.c asks whether the gadget is set up (mg_setup_store.c isn't in
 * this test). */
static bool host_set_up = true;

bool mg_setup_store_complete(void)
{
	return host_set_up;
}

extern uint32_t host_conn_gen; /* host_zephyr.c's mg_transport_conn_gen() */

static void test_clock(void)
{
	uint8_t out[16];
	size_t len;

	host_settings_clear();
	mg_clock_load();

	/* Never synced: INT64_MIN, offset 0, 12-hour. */
	const uint8_t get[] = {mg_device_action_get_unix_time};

	CHECK(clock_rx(get, sizeof(get), out, &len) == 0);
	CHECK(len == MG_CLOCK_REPORT_LEN);
	CHECK(out[0] == mg_command_device_action && out[1] == mg_device_action_report_unix_time);
	CHECK(le64(&out[2]) == INT64_MIN);
	CHECK(out[10] == 0 && out[11] == 0 && out[12] == 0);

	/* The ESP32 reference's frame: 1793380800000 ms, UTC+5:30 (330), 24 h. */
	const uint8_t set[] = {mg_device_action_set_unix_time, 0x00, 0x56, 0xd3, 0x8d, 0xa1,
			       0x01, 0x00, 0x00, 0x4a, 0x01, 0x03};

	CHECK(clock_rx(set, sizeof(set), out, &len) == 0);
	CHECK(len == 0); /* no reply */
	host_advance(2500);
	CHECK(clock_rx(get, sizeof(get), out, &len) == 0);
	CHECK(le64(&out[2]) == 1793380800000LL + 2500);
	CHECK(out[10] == 0x4a && out[11] == 0x01);
	CHECK(out[12] == mg_clock_flag_24_hour); /* other flag bits dropped */

	/* Persisted offset and flags, the time not (no RTC through a reset). */
	CHECK(host_settings_len("mg/clk") == 3);
	mg_clock_load();
	CHECK(clock_rx(get, sizeof(get), out, &len) == 0);
	CHECK(le64(&out[2]) == INT64_MIN);
	CHECK(out[10] == 0x4a && out[11] == 0x01 && out[12] == 1);

	/* Omitted offset / flags are kept; a negative offset round-trips. */
	const uint8_t set_time[] = {mg_device_action_set_unix_time, 1, 0, 0, 0, 0, 2, 0, 0};

	CHECK(clock_rx(set_time, sizeof(set_time), out, &len) == 0);
	CHECK(clock_rx(get, sizeof(get), out, &len) == 0);
	CHECK(le64(&out[2]) == 0x020000000001LL && out[10] == 0x4a && out[12] == 1);
	const uint8_t set_neg[] = {mg_device_action_set_unix_time, 0, 0, 0, 0, 0, 2, 0, 0, 0x5c,
				   0xfe};

	CHECK(clock_rx(set_neg, sizeof(set_neg), out, &len) == 0);
	CHECK(clock_rx(get, sizeof(get), out, &len) == 0);
	CHECK((int16_t)(out[10] | out[11] << 8) == -420 && out[12] == 1);

	/* A set that may not store (a set-up gadget, no token proof) applies in
	 * RAM, succeeds and stores nothing; a later set that may store saves what
	 * differs from the stored value, though RAM already holds it. */
	const uint8_t set_utc[] = {mg_device_action_set_unix_time, 0, 0, 0, 0, 0, 2, 0, 0, 0, 0, 0};
	const uint8_t neg_24h[] = {0x5c, 0xfe, 1}, utc_12h[] = {0, 0, 0};

	CHECK(mg_clock_action(set_utc, sizeof(set_utc), false, out, &len) == 0 && len == 0);
	CHECK(clock_rx(get, sizeof(get), out, &len) == 0);
	CHECK(out[10] == 0 && out[11] == 0 && out[12] == 0);
	CHECK(memcmp(host_settings_get("mg/clk"), neg_24h, 3) == 0);
	mg_clock_flush(); /* a planned power off writes nothing of it either */
	CHECK(memcmp(host_settings_get("mg/clk"), neg_24h, 3) == 0);
	CHECK(mg_clock_action(set_utc, sizeof(set_utc), true, out, &len) == 0);
	CHECK(memcmp(host_settings_get("mg/clk"), utc_12h, 3) == 0);
	mg_clock_load();
	CHECK(clock_rx(get, sizeof(get), out, &len) == 0);
	CHECK(out[10] == 0 && out[11] == 0 && out[12] == 0);
	CHECK(clock_rx(set_neg, sizeof(set_neg), out, &len) == 0);
	/* A set that may store saves only what it carries: an earlier unproven
	 * offset stays in RAM, out of storage. */
	const uint8_t neg_12h[] = {0x5c, 0xfe, 0};

	CHECK(memcmp(host_settings_get("mg/clk"), neg_12h, 3) == 0);
	CHECK(mg_clock_action(set_utc, sizeof(set_utc), false, out, &len) == 0);
	CHECK(clock_rx(set_time, sizeof(set_time), out, &len) == 0);
	CHECK(memcmp(host_settings_get("mg/clk"), neg_12h, 3) == 0);
	CHECK(clock_rx(set_neg, sizeof(set_neg), out, &len) == 0);

	/* mg-session-v1.json's settings section, its bytes copied here by hand
	 * (no generator reads that section): a change there needs this
	 * copy updated. set_unix_time from an unproven central on a set-up gadget
	 * is taken with no reply and no flash write; get_unix_time reads it
	 * back. */
	{
		static const uint8_t vset[] = {0x12, 0x00, 0x50, 0x5c, 0x18, 0xa3, 0x01, 0x00, 0x00,
					       0x20, 0xfe, 0x00};
		static const uint8_t vget[] = {0x13};
		static const uint8_t vrep[] = {0x1e, 0x14, 0x00, 0x50, 0x5c, 0x18, 0xa3, 0x01, 0x00,
					       0x00, 0x20, 0xfe, 0x00};

		host_settings_clear();
		mg_clock_load();
		CHECK(mg_clock_action(vset, sizeof(vset), false, out, &len) == 0 && len == 0);
		CHECK(host_settings_len("mg/clk") < 0);
		CHECK(clock_rx(vget, sizeof(vget), out, &len) == 0);
		CHECK(len == sizeof(vrep) && memcmp(out, vrep, len) == 0);
	}

	/* What this connection set before its proof is saved once the proof
	 * matches on the same connection; another connection's, or one that
	 * disconnected, never is. */
	{
		const uint8_t a[] = {mg_device_action_set_unix_time, 0, 0x10, 0, 0, 0, 2, 0, 0, 0x2c, 0x01, 1};
		const uint8_t b[] = {mg_device_action_set_unix_time, 0, 0x20, 0, 0, 0, 2, 0, 0, 0xd4, 0xfe, 0};
		const uint32_t gen0 = host_conn_gen;

		/* Same connection: unproven set, then its proof -> saved once. */
		host_settings_clear();
		mg_clock_load();
		host_set_up = true;
		host_conn_gen = 10;
		CHECK(mg_clock_action(a, sizeof(a), false, out, &len) == 0);
		CHECK(host_settings_len("mg/clk") < 0);
		mg_clock_proof_matched();
		CHECK(host_settings_len("mg/clk") == 3 &&
		      ((const uint8_t *)host_settings_get("mg/clk"))[0] == 0x2c);
		host_settings_clear();
		mg_clock_proof_matched(); /* a second match: nothing pending, no write */
		CHECK(host_settings_len("mg/clk") < 0);

		/* Unproven set on A, proof on B: not saved, and B reads mg/clk's values. */
		host_settings_clear();
		mg_clock_load();
		host_conn_gen = 11;
		CHECK(mg_clock_action(b, sizeof(b), false, out, &len) == 0);
		host_conn_gen = 12;
		mg_clock_proof_matched();
		CHECK(host_settings_len("mg/clk") < 0);
		CHECK(clock_rx(get, sizeof(get), out, &len) == 0);
		CHECK(out[10] == 0 && out[11] == 0 && out[12] == 0);
		/* ...and B's own time-only set doesn't carry A's offset to flash. */
		const uint8_t t[] = {mg_device_action_set_unix_time, 5, 0, 0, 0, 0, 2, 0, 0};

		CHECK(clock_rx(t, sizeof(t), out, &len) == 0);
		CHECK(host_settings_len("mg/clk") < 0);

		/* Unproven set, then a disconnect (a new connection's gen): not saved. */
		host_conn_gen = 13;
		CHECK(mg_clock_action(a, sizeof(a), false, out, &len) == 0);
		host_conn_gen = 14; /* the link ended; nothing proved on it */
		mg_clock_flush();   /* a planned power off */
		CHECK(host_settings_len("mg/clk") < 0);
		mg_clock_proof_matched(); /* a proof on the new link */
		CHECK(host_settings_len("mg/clk") < 0);
		host_conn_gen = gen0;
	}

	/* Not set up: any central's offset and flags stay in RAM (no flash
	 * write per command) until setup's commit or a planned power off;
	 * the last one waiting is what goes. */
	{
		const uint8_t a[] = {mg_device_action_set_unix_time, 0, 0, 0, 0, 0, 2, 0, 0, 0x10, 0};
		const uint8_t b[] = {mg_device_action_set_unix_time, 0, 0, 0, 0, 0, 2, 0, 0, 0x20, 0};
		const uint8_t b_24h[] = {0x20, 0, 1};

		host_set_up = false;
		host_settings_clear();
		mg_clock_load();
		for (int i = 0; i < 20; i++) {
			CHECK(clock_rx(i & 1 ? b : a, sizeof(a), out, &len) == 0);
		}
		CHECK(host_settings_len("mg/clk") < 0);
		CHECK(clock_rx(get, sizeof(get), out, &len) == 0 && out[10] == 0x20);
		/* A set back to what is stored leaves nothing waiting. */
		CHECK(clock_rx(set_utc, sizeof(set_utc), out, &len) == 0);
		mg_clock_flush();
		CHECK(host_settings_len("mg/clk") < 0);
		/* The latest set is what waits. */
		const uint8_t flags_24h[] = {mg_device_action_set_unix_time, 0, 0, 0, 0, 0, 2, 0, 0,
					     0x20, 0, 1};
		CHECK(clock_rx(b, sizeof(b), out, &len) == 0);
		CHECK(clock_rx(flags_24h, sizeof(flags_24h), out, &len) == 0);
		CHECK(host_settings_len("mg/clk") < 0);
		mg_clock_flush(); /* a planned power off (or setup's commit): one write */
		CHECK(host_settings_len("mg/clk") == 3 &&
		      memcmp(host_settings_get("mg/clk"), b_24h, 3) == 0);
		mg_clock_flush(); /* nothing waits: no second write */
		host_set_up = true;
		mg_clock_load();
		CHECK(clock_rx(set_neg, sizeof(set_neg), out, &len) == 0);
	}

	/* Refusals. */
	const uint8_t set_min[] = {mg_device_action_set_unix_time, 0, 0, 0, 0, 0, 0, 0, 0x80};
	const uint8_t set_short[] = {mg_device_action_set_unix_time, 0, 0, 0, 0, 0, 0, 0};
	const uint8_t other[] = {0x02};

	CHECK(clock_rx(set_min, sizeof(set_min), out, &len) == mg_error_code_invalid_value);
	CHECK(clock_rx(set_short, sizeof(set_short), out, &len) == mg_error_code_invalid_length);
	CHECK(clock_rx(other, sizeof(other), out, &len) == mg_error_code_unsupported);
	CHECK(clock_rx(other, 0, out, &len) == mg_error_code_invalid_length);
	/* A refused set changes nothing. */
	CHECK(clock_rx(get, sizeof(get), out, &len) == 0);
	CHECK((int16_t)(out[10] | out[11] << 8) == -420);
	/* The floor (mg-session-v1.json's clock_floor rows): 1000000000000 ms
	 * refused and the clock unchanged; the floor itself taken. */
	{
		static const uint8_t old_t[] = {0x12, 0x00, 0x10, 0xa5, 0xd4, 0xe8, 0x00, 0x00, 0x00,
						0x20, 0xfe, 0x00};
		static const uint8_t floor_t[] = {0x12, 0x00, 0xf4, 0x51, 0xc2, 0x8c, 0x01, 0x00, 0x00,
						  0x20, 0xfe, 0x00};
		int64_t before;

		CHECK(clock_rx(get, sizeof(get), out, &len) == 0);
		before = le64(&out[2]);
		CHECK(clock_rx(old_t, sizeof(old_t), out, &len) == mg_error_code_invalid_value);
		CHECK(clock_rx(get, sizeof(get), out, &len) == 0 && le64(&out[2]) == before);
		CHECK(clock_rx(floor_t, sizeof(floor_t), out, &len) == 0 && len == 0);
		CHECK(clock_rx(get, sizeof(get), out, &len) == 0 &&
		      le64(&out[2]) == MG_CLOCK_FLOOR_MS);
	}
	/* The ceiling: a time past 9999 (INT64_MAX here, which would overflow
	 * the clock's base plus the uptime) refused and the clock unchanged;
	 * the ceiling itself taken. */
	{
		uint8_t big[12] = {mg_device_action_set_unix_time}, ceil_t[12] = {
			mg_device_action_set_unix_time};
		int64_t before;

		for (int i = 0; i < 8; i++) {
			big[1 + i] = (uint8_t)((uint64_t)INT64_MAX >> (8 * i));
			ceil_t[1 + i] = (uint8_t)((uint64_t)MG_CLOCK_CEIL_MS >> (8 * i));
		}
		big[9] = ceil_t[9] = 0x20;
		big[10] = ceil_t[10] = 0xfe;
		CHECK(clock_rx(get, sizeof(get), out, &len) == 0);
		before = le64(&out[2]);
		CHECK(clock_rx(big, sizeof(big), out, &len) == mg_error_code_invalid_value);
		CHECK(clock_rx(get, sizeof(get), out, &len) == 0 && le64(&out[2]) == before);
		CHECK(clock_rx(ceil_t, sizeof(ceil_t), out, &len) == 0 && len == 0);
		CHECK(clock_rx(get, sizeof(get), out, &len) == 0 &&
		      le64(&out[2]) == MG_CLOCK_CEIL_MS);
	}
	/* mg-session-v1.json's settings exchange "a time past year 9999", its
	 * bytes: 253402300800000 ms (10000-01-01T00:00:00Z) refused and the clock
	 * unchanged; 253402300799000 ms (9999-12-31T23:59:59Z) taken. */
	{
		static const uint8_t past_t[] = {0x12, 0x00, 0xdc, 0x1f, 0xd2, 0x77, 0xe6, 0x00, 0x00,
						 0x20, 0xfe, 0x00};
		static const uint8_t last_t[] = {0x12, 0x18, 0xd8, 0x1f, 0xd2, 0x77, 0xe6, 0x00, 0x00,
						 0x20, 0xfe, 0x00};
		int64_t before;

		CHECK(clock_rx(get, sizeof(get), out, &len) == 0);
		before = le64(&out[2]);
		CHECK(clock_rx(past_t, sizeof(past_t), out, &len) == mg_error_code_invalid_value);
		CHECK(clock_rx(get, sizeof(get), out, &len) == 0 && le64(&out[2]) == before);
		CHECK(clock_rx(last_t, sizeof(last_t), out, &len) == 0 && len == 0);
		CHECK(clock_rx(get, sizeof(get), out, &len) == 0 &&
		      le64(&out[2]) == 253402300799000LL);
	}
}

static const char *pick(uint32_t flags, enum mg_led_face face, enum mg_led_event ev)
{
	const struct mg_led_inputs in = {.flags = flags, .face = face, .event = ev};

	return mg_led_pick(&in)->name;
}

/* A remote LED command (device.led) never darkens or changes the mic's
 * light: capture and offline recording win; the command shows otherwise. */
/* A stored setting on a gadget that isn't set up reaches flash at most once
 * per 10 min, the window starting at boot; a set-up gadget saves at once. */
static void test_settings_window(void)
{
	printf("  settings from a central before setup: at most one write per 10 min\n");
	CHECK(!mg_settings_save_due(false, 0, 5000)); /* a restart buys no write */
	CHECK(!mg_settings_save_due(false, 0, MG_SETTINGS_SAVE_GAP_MS - 1));
	CHECK(mg_settings_save_due(false, 0, MG_SETTINGS_SAVE_GAP_MS));
	CHECK(!mg_settings_save_due(false, 700000, 700000 + MG_SETTINGS_SAVE_GAP_MS - 1));
	CHECK(mg_settings_save_due(false, 700000, 700000 + MG_SETTINGS_SAVE_GAP_MS));
	CHECK(mg_settings_save_due(true, 0, 5000));
}

static void test_led_command_vs_capture(void)
{
	struct mg_led_inputs in = {.command = MG_LED_COMMAND_OFF};

	/* The update press counts only while the LED shows the update
	 * cue: never under a setup prompt, a reset/SAFE cue or the mic light. */
	CHECK(mg_led_update_cue_shows(MG_LED_UPDATE_PROMPT));
	CHECK(!mg_led_update_cue_shows(MG_LED_UPDATE_PROMPT | MG_LED_PAIR_PENDING));
	CHECK(!mg_led_update_cue_shows(MG_LED_UPDATE_PROMPT | MG_LED_STREAMING));
	CHECK(!mg_led_update_cue_shows(MG_LED_UPDATE_PROMPT | MG_LED_SAFE_ARMED));
	CHECK(!mg_led_update_cue_shows(0));
	printf("  device.led can't hide the capture light\n");
	CHECK(strcmp(mg_led_pick(&in)->name, "command: off") == 0);
	in.flags = MG_LED_STREAMING;
	CHECK(strcmp(mg_led_pick(&in)->name, pick(MG_LED_STREAMING, 0, 0)) == 0);
	CHECK(mg_led_pattern_on(mg_led_pick(&in), 0));
	in.flags = MG_LED_RECORDING;
	CHECK(strcmp(mg_led_pick(&in)->name, pick(MG_LED_RECORDING, 0, 0)) == 0);
	in.command = MG_LED_COMMAND_ON;
	in.flags = MG_LED_STREAMING;
	CHECK(strcmp(mg_led_pick(&in)->name, pick(MG_LED_STREAMING, 0, 0)) == 0);
	in.flags = 0;
	CHECK(strcmp(mg_led_pick(&in)->name, "command: on") == 0);
	/* Setup and boot cues still come first. */
	in.flags = MG_LED_STREAMING | MG_LED_PAIR_PENDING;
	CHECK(strcmp(mg_led_pick(&in)->name, pick(MG_LED_PAIR_PENDING, 0, 0)) == 0);
}

/* Runs a hold: the key down until down_ms (UINT32_MAX: never released),
 * looked at every 50 ms; the event that ended it. */
static enum mg_rh_event rh_run_safe(uint32_t down_ms, uint32_t safe_ms)
{
	struct mg_rh h = {0};

	for (uint32_t t = 50; t <= 30000; t += 50) {
		enum mg_rh_event e = mg_reset_hold_step(&h, t < down_ms, t, safe_ms, 5000, 5000);

		if (e == MG_RH_COMMIT || e == MG_RH_CANCEL || e == MG_RH_SAFE) {
			return e;
		}
	}
	return MG_RH_NONE;
}

static enum mg_rh_event rh_run(uint32_t down_ms)
{
	return rh_run_safe(down_ms, 0);
}

/* A talk from off never resets; the reset commits on release. */
static void test_reset_hold(void)
{
	/* Cold boots: power-on (no reason), the pin, a power-on reset. */
	CHECK(mg_reset_hold_cold(0));
	CHECK(mg_reset_hold_cold(RESET_PIN));
	CHECK(mg_reset_hold_cold(RESET_POR));
	/* Not: a wake from System OFF (alone or with another reason), an
	 * update's software reset, a watchdog, a lockup, the debugger. */
	CHECK(!mg_reset_hold_cold(RESET_LOW_POWER_WAKE));
	CHECK(!mg_reset_hold_cold(RESET_LOW_POWER_WAKE | RESET_PIN));
	CHECK(!mg_reset_hold_cold(RESET_SOFTWARE));
	CHECK(!mg_reset_hold_cold(RESET_WATCHDOG));
	CHECK(!mg_reset_hold_cold(RESET_CPU_LOCKUP));
	CHECK(!mg_reset_hold_cold(RESET_DEBUG));
	/* Released before 5 s: nothing. */
	CHECK(rh_run(100) == MG_RH_CANCEL);
	CHECK(rh_run(4950) == MG_RH_CANCEL);
	/* Held 5 s, released in the window: the reset. */
	CHECK(rh_run(5100) == MG_RH_COMMIT);
	CHECK(rh_run(9950) == MG_RH_COMMIT);
	/* Held 10 s or more (a long talk hold), or never released: nothing. */
	CHECK(rh_run(10000) == MG_RH_CANCEL);
	CHECK(rh_run(12000) == MG_RH_CANCEL);
	CHECK(rh_run(UINT32_MAX) == MG_RH_CANCEL);
	/* The arm event comes once, at 5 s. */
	{
		struct mg_rh h = {0};
		int arms = 0;

		for (uint32_t t = 50; t <= 7000; t += 50) {
			arms += mg_reset_hold_step(&h, true, t, 0, 5000, 5000) == MG_RH_ARM;
		}
		CHECK(arms == 1 && h.armed && h.armed_at == 5000);
	}
	/* No SAFE point by default: never a SAFE event, as above. */
	CHECK(rh_run(4000) == MG_RH_CANCEL);
	/* With the SAFE point (3 s): one timeline. */
	CHECK(rh_run_safe(2950, 3000) == MG_RH_CANCEL);
	CHECK(rh_run_safe(3100, 3000) == MG_RH_SAFE);
	CHECK(rh_run_safe(4950, 3000) == MG_RH_SAFE);
	CHECK(rh_run_safe(5100, 3000) == MG_RH_COMMIT);
	CHECK(rh_run_safe(9950, 3000) == MG_RH_COMMIT);
	CHECK(rh_run_safe(10000, 3000) == MG_RH_CANCEL);
	CHECK(rh_run_safe(UINT32_MAX, 3000) == MG_RH_CANCEL);
	/* The SAFE cue once at 3 s, then the reset cue once at 5 s. */
	{
		struct mg_rh h = {0};
		int safe_arms = 0, arms = 0;

		for (uint32_t t = 50; t <= 7000; t += 50) {
			enum mg_rh_event e = mg_reset_hold_step(&h, true, t, 3000, 5000, 5000);

			safe_arms += e == MG_RH_SAFE_ARM;
			arms += e == MG_RH_ARM;
			if (t == 3000) {
				CHECK(e == MG_RH_SAFE_ARM && h.safe_armed);
			}
			if (t == 5000) {
				CHECK(e == MG_RH_ARM && !h.safe_armed && h.armed);
			}
		}
		CHECK(safe_arms == 1 && arms == 1);
	}
	/* A SAFE-only board (reset not allowed: its point out of reach). */
	{
		struct mg_rh h = {0};
		enum mg_rh_event e = MG_RH_NONE;

		for (uint32_t t = 50; t <= 20000 && e != MG_RH_SAFE; t += 50) {
			e = mg_reset_hold_step(&h, t < 12000, t, 3000, UINT32_MAX / 2, 5000);
		}
		CHECK(e == MG_RH_SAFE);
	}
	/* The cues: reset over SAFE over everything else. */
	const struct mg_led_inputs safe_in = {.flags = MG_LED_SAFE_ARMED | MG_LED_STREAMING};
	const struct mg_led_inputs both_in = {.flags = MG_LED_SAFE_ARMED | MG_LED_RESET_ARMED};

	CHECK(strcmp(mg_led_pick(&safe_in)->name, "SAFE mode: release now") == 0);
	CHECK(strcmp(mg_led_pick(&both_in)->name, "setup reset: release now") == 0);
	/* The LED cue outranks everything. */
	const struct mg_led_inputs in = {.flags = MG_LED_RESET_ARMED | MG_LED_STREAMING};

	CHECK(strcmp(mg_led_pick(&in)->name, "setup reset: release now") == 0);
}

/* The hold keeps records whole and in order, refuses what doesn't
 * fit, and tells stale output. */
static void test_out_hold(void)
{
	static uint8_t buf[70];
	struct mg_out_hold h;
	uint8_t kind;
	const uint8_t *p;
	size_t n;
	const uint8_t cdt[] = {12, 1, 2, 3}, g[] = {7, 1}, d1[20] = {1}, d2[20] = {2};

	mg_out_hold_init(&h, buf, sizeof(buf));
	CHECK(!mg_out_hold_peek(&h, &kind, &p, &n));
	CHECK(mg_out_hold_push(&h, MG_OUT_CTRL, g, sizeof(g)));
	CHECK(mg_out_hold_push(&h, MG_OUT_CTRL, cdt, sizeof(cdt)));
	CHECK(mg_out_hold_push(&h, MG_OUT_DATA, d1, sizeof(d1)));
	CHECK(h.records == 3 && h.data_bytes == 20 && h.len == 5 + 7 + 23);
	/* 35 bytes left: a 20-byte Data record (23) fits, a second doesn't. */
	CHECK(mg_out_hold_push(&h, MG_OUT_DATA, d2, sizeof(d2)));
	CHECK(!mg_out_hold_push(&h, MG_OUT_DATA, d2, sizeof(d2)));
	CHECK(h.records == 4 && h.data_bytes == 40);
	/* In order. */
	CHECK(mg_out_hold_peek(&h, &kind, &p, &n) && kind == MG_OUT_CTRL && n == 2 && p[0] == 7);
	mg_out_hold_pop(&h);
	CHECK(mg_out_hold_peek(&h, &kind, &p, &n) && kind == MG_OUT_CTRL && n == 4 && p[0] == 12);
	mg_out_hold_pop(&h);
	/* Room again: the tail moves to the front when it must. */
	CHECK(mg_out_hold_push(&h, MG_OUT_DATA, d2, sizeof(d2)));
	CHECK(mg_out_hold_peek(&h, &kind, &p, &n) && kind == MG_OUT_DATA && n == 20 && p[0] == 1);
	mg_out_hold_pop(&h);
	CHECK(mg_out_hold_peek(&h, &kind, &p, &n) && kind == MG_OUT_DATA && p[0] == 2);
	mg_out_hold_pop(&h);
	CHECK(mg_out_hold_peek(&h, &kind, &p, &n) && kind == MG_OUT_DATA && p[0] == 2);
	mg_out_hold_pop(&h);
	CHECK(h.records == 0 && h.len == 0 && h.data_bytes == 0);
	/* No buffer lent: nothing held. */
	mg_out_hold_init(&h, NULL, 0);
	CHECK(!mg_out_hold_push(&h, MG_OUT_CTRL, g, sizeof(g)));
	/* Staleness: 3 s after the capture's end, never while capturing. */
	CHECK(!mg_out_hold_stale(100000, 0));
	CHECK(!mg_out_hold_stale(102999, 100000));
	CHECK(mg_out_hold_stale(103000, 100000));
	CHECK(!mg_out_hold_stale(5, 0xfffffff0u)); /* across the wrap: 21 ms */
	CHECK(mg_out_hold_stale(2990, 0xfffffff0u));
}

static void test_low_battery(void)
{
	uint32_t ev;

	mg_power_reset();
	mg_power_set_shutdown_mv(3300);

	/* Full, then down the curve: low at 15 %, critical at 10 %. */
	CHECK(mg_power_reading(4100, 0) == 0); /* the first reading: no report */
	CHECK((mg_power_reading(3700, 1) & ~MG_POWER_EV_REPORT) == 0); /* 27 % (5 % steps report) */
	ev = mg_power_reading(3640, 2);        /* 15 % */
	CHECK((ev & ~MG_POWER_EV_REPORT) == MG_POWER_EV_LOW && (ev & MG_POWER_EV_REPORT) && mg_power_battery()->low && !mg_power_battery()->critical);
	CHECK((mg_power_reading(3630, 3) & ~MG_POWER_EV_REPORT) == 0); /* still low: no repeat */
	ev = mg_power_reading(3610, 4);        /* 10 % */
	CHECK((ev & ~MG_POWER_EV_REPORT) == MG_POWER_EV_CRITICAL && (ev & MG_POWER_EV_REPORT) && mg_power_battery()->critical);
	/* Hysteresis: 16 % isn't recovered, 20 % is (charging). */
	CHECK((mg_power_reading(3646, 5) & ~MG_POWER_EV_REPORT) == 0 && mg_power_battery()->critical);
	ev = mg_power_reading(3670, 6);
	CHECK((ev & ~MG_POWER_EV_REPORT) == MG_POWER_EV_RECOVERED && (ev & MG_POWER_EV_REPORT) && !mg_power_battery()->low);

	/* Straight to the bottom in one reading: low and critical at once. */
	ev = mg_power_reading(3400, 7);
	CHECK((ev & ~MG_POWER_EV_REPORT) == (MG_POWER_EV_LOW | MG_POWER_EV_CRITICAL) && (ev & MG_POWER_EV_REPORT));

	/* Shutdown: two readings in a row at or under 3.3 V; one dip isn't. */
	CHECK((mg_power_reading(3300, 8) & ~MG_POWER_EV_REPORT) == 0);
	CHECK((mg_power_reading(3350, 9) & ~MG_POWER_EV_REPORT) == 0);
	CHECK((mg_power_reading(3290, 10) & ~MG_POWER_EV_REPORT) == 0);
	CHECK((mg_power_reading(3250, 11) & ~MG_POWER_EV_REPORT) == MG_POWER_EV_SHUTDOWN);

	/* No cell (USB without one, < 2.5 V) and the charger's 4.2 V: never low,
	 * never a shutdown; a warning that was on clears. */
	ev = mg_power_reading(120, 12);
	CHECK((ev & ~MG_POWER_EV_REPORT) == MG_POWER_EV_RECOVERED && !mg_power_battery()->low);
	for (int i = 0; i < 5; i++) {
		/* (the cell going is reported once, below) */
		CHECK((mg_power_reading(0, 13 + i) & ~MG_POWER_EV_REPORT) == 0);
		CHECK((mg_power_reading(4200, 20 + i) & ~MG_POWER_EV_REPORT) == 0);
	}
	CHECK(mg_power_reading(-5, 30) == 0); /* ADC failure */
	/* The threshold follows the setting. */
	mg_power_set_shutdown_mv(3500);
	mg_power_reading(3450, 31);
	CHECK(mg_power_reading(3450, 32) & MG_POWER_EV_SHUTDOWN);
	mg_power_set_shutdown_mv(3300);
	mg_power_reset();
}

/* The update prompt against protocols/test-vectors/mg-update-v2.json: every exchange
 * this app can play (no passcode lock, no cancel on a headless gadget), each
 * SMP request's rc and the prompt after every step, through struct mg_upd as
 * src/mg_ota.c drives it: each update write read from its request (an
 * upload's first chunk with its length and SHA-256, a later chunk with where
 * it ends, a state write, an erase or the reset with its group, id and body);
 * MG_UPD_VERIFY answered with whether slot1 then holds the bound image
 * (mg_ota.c hashes it; here the generator's slot as the vectors' uploads left
 * it); a SAFE the gadget entered itself refuses, mg_ota_smp_closed(). */
struct uv_step {
	int kind; /* 0 an SMP request, 1 time, 2 press, 3 cancel, 4 unlock, 5 reconnect,
		   * 6 an injected press (never a confirm on release), 7 Forget,
		   * 8 set up again (a new device token) */
	uint8_t smp[160]; /* the request: 8-byte header, CBOR body */
	uint16_t smp_len;
	int64_t ms;
	int rc, prompt; /* prompt: 0 none, 1 confirm, 2 keypad */
	uint32_t slot_len; /* slot1 once this upload write is in, or now */
	uint8_t slot_sha[32];
};
struct uv_ex {
	const char *name;
	bool release, set_up, proof, console_arm, button_recovery, self_unprompted, locked;
	const struct uv_step *steps;
	int n;
};
#include "update_vectors.h"

/* Hand-written update writes: an upload's first chunk (image length, SHA-256
 * or NULL; 64 bytes of data), a later chunk ending at end, a state write or
 * another change (its group, id and body). */
static const uint8_t uv_erase[] = {0x00, 0x01, 0x05, 0xa1, 0x64, 's', 'l', 'o', 't', 0x01};
static const uint8_t uv_reset[] = {0x00, 0x00, 0x05, 0xa0};
static const uint8_t uv_state[] = {0x00, 0x01, 0x00, 0xa1, 0x67, 'c', 'o', 'n', 'f', 'i', 'r', 'm', 0xf4};
#define UV_UP0(l, s) ((struct mg_upd_req){.kind = MG_UPD_UPLOAD0, .len = (l), .sha = (s), .end = 64})
#define UV_CHUNK(e)  ((struct mg_upd_req){.kind = MG_UPD_CHUNK, .end = (e)})
#define UV_STATE(k)  ((struct mg_upd_req){.kind = MG_UPD_STATE, .key = (k), .key_len = sizeof(k)})
#define UV_CHANGE(k) ((struct mg_upd_req){.kind = MG_UPD_OTHER, .key = (k), .key_len = sizeof(k)})

/* One update write through the gate, as mg_ota.c's on_cmd() runs it. */
static int uv_gate(struct mg_upd *u, uint32_t gen, int64_t now, const struct mg_upd_req *r,
		   uint32_t slot_len, const uint8_t *slot_sha, bool *started)
{
	int rc = mg_upd_write(u, gen, now, r, started);

	if (rc == MG_UPD_VERIFY) {
		rc = mg_upd_verified(u, gen,
				     slot_len == u->blen && memcmp(slot_sha, u->bsha, 32) == 0);
	}
	return rc;
}

static void test_update_vectors(void)
{
	int played = 0;

	printf("  mg-update-v2.json's exchanges (the press bound to its upload)\n");
	CHECK(UV_PROMPT_MS == MG_UPDATE_PROMPT_MS);
	CHECK(UV_CONFIRM_MS == MG_UPDATE_CONFIRM_MS);
	CHECK(UV_REARM_MS == MG_UPDATE_REARM_MS);
	for (size_t e = 0; e < ARRAY_SIZE(uv_exchanges); e++) {
		const struct uv_ex *x = &uv_exchanges[e];
		bool skip = x->locked;

		for (int i = 0; i < x->n; i++) {
			skip |= x->steps[i].kind == 3 || x->steps[i].kind == 4;
		}
		if (skip) {
			continue; /* a passcode or a cancel: not this app's */
		}
		struct mg_upd u = {0};
		uint32_t gen = 1;
		int64_t now = 1000;
		bool proof = x->proof, arm = x->console_arm;
		bool safe = x->button_recovery || x->self_unprompted;

		for (int i = 0; i < x->n; i++) {
			const struct uv_step *st = &x->steps[i];

			if (st->kind == 0) {
				const uint8_t *h = st->smp;
				uint16_t group = (uint16_t)(h[4] << 8 | h[5]);
				uint8_t op = h[0] & 7, id = h[7];
				struct mg_upd_req r;
				uint8_t key[MG_UPD_KEY_MAX];
				int w = mg_upd_parse(h, &h[8], st->smp_len - 8u, &r, key);
				int rc = 0;

				CHECK(w >= 0); /* the vectors' requests are well formed */
				if (w > 0 && mg_ota_smp_closed(group, id, op, safe, x->button_recovery,
							       x->release, arm)) {
					rc = MG_UPD_DENIED;
				} else if (w > 0 && !mg_ota_smp_allowed(group, id, op, proof, safe,
									x->button_recovery, x->release,
									arm)) {
					bool started;

					rc = uv_gate(&u, gen, now, &r, st->slot_len, st->slot_sha,
						     &started);
				}
				if (rc != st->rc) {
					printf("    %s, step %d: rc %d, want %d\n", x->name, i, rc, st->rc);
				}
				CHECK(rc == st->rc);
			} else if (st->kind == 1) {
				now += st->ms;
				mg_upd_expire(&u, now); /* the prompt's timer */
			} else if (st->kind == 2) {
				(void)mg_upd_press(&u, gen, now, true, false);
			} else if (st->kind == 5) {
				gen++; /* a new link: its proof and arm gone, the binding kept */
				proof = arm = false;
			} else if (st->kind == 6) {
				/* A console word or a scripted press: counts only where
				 * the build takes injected input (never on release). */
				(void)mg_upd_press(&u, gen, now, false, !x->release);
			} else if (st->kind == 7) {
				/* Forget: the device token erased (the press is still
				 * needed); the prompt, the binding and the covered
				 * change end, and so does the link's proof
				 * (mg_setup_factory_reset() resets it); the console's
				 * arm stays. */
				mg_upd_forget(&u); /* mg_setup_store_erased() */
				proof = false;
			} else if (st->kind == 8) {
				/* Set up again (a new device token): the setup's
				 * commit is an ownership change too
				 * (mg_ota_setup_committed()); it proves no link, so
				 * an unproven update still asks. */
				mg_upd_forget(&u);
			}
			int prompt = mg_upd_prompting(&u, gen, now) ? 1 : 0;

			if (prompt != st->prompt) {
				printf("    %s, step %d: prompt %d, want %d\n", x->name, i, prompt, st->prompt);
			}
			CHECK(prompt == st->prompt);
		}
		played++;
	}
	CHECK(played >= 17);
	{
		/* "a new prompt waits 10 s", up to its cancel (this app has none):
		 * a press bound to upload A; upload B gets 10 with no prompt (the
		 * last one under 10 s ago); 10 s after it, a retry shows the
		 * prompt. */
		struct mg_upd u = {0};
		uint8_t a[32], b[32];
		bool started;

		memset(a, 0xa1, sizeof(a));
		memset(b, 0xb2, sizeof(b));
		CHECK(mg_upd_write(&u, 1, 1000, &UV_UP0(4096, a), &started) == MG_UPD_BUSY && started);
		CHECK(mg_upd_press(&u, 1, 1500, true, false));
		CHECK(mg_upd_write(&u, 1, 2000, &UV_UP0(4096, b), &started) == MG_UPD_BUSY &&
		      !started && !mg_upd_prompting(&u, 1, 2000));
		CHECK(mg_upd_write(&u, 1, 11000, &UV_UP0(4096, b), &started) == MG_UPD_BUSY &&
		      started);
		/* A new connection may show one at once. */
		struct mg_upd v = {0};

		(void)mg_upd_write(&v, 1, 1000, &UV_CHANGE(uv_erase), &started);
		CHECK(mg_upd_write(&v, 2, 1500, &UV_CHANGE(uv_erase), &started) == MG_UPD_BUSY &&
		      started);
	}
	{
		/* "a press on an erase covers only that erase, on its link", up to
		 * its cancel: the same erase is taken, another change asks again
		 * (10 with no prompt within 10 s of the last), a chunk with no
		 * upload started on this link gets 11; the covered erase still goes
		 * after 10 s, never after 600 s, and a new link asks again. */
		struct mg_upd u = {0};
		bool started;

		CHECK(mg_upd_write(&u, 1, 1000, &UV_CHANGE(uv_erase), &started) == MG_UPD_BUSY &&
		      started);
		CHECK(mg_upd_press(&u, 1, 1500, true, false));
		CHECK(mg_upd_write(&u, 1, 2000, &UV_CHANGE(uv_erase), &started) == MG_UPD_OK);
		CHECK(mg_upd_write(&u, 1, 2100, &UV_CHANGE(uv_reset), &started) == MG_UPD_BUSY &&
		      !started);
		CHECK(mg_upd_write(&u, 1, 2200, &UV_CHUNK(64), &started) == MG_UPD_DENIED && !started);
		CHECK(mg_upd_write(&u, 1, 2300, &UV_STATE(uv_state), &started) == MG_UPD_BUSY &&
		      !started);
		CHECK(mg_upd_write(&u, 1, 12000, &UV_CHANGE(uv_erase), &started) == MG_UPD_OK);
		CHECK(mg_upd_write(&u, 1, 1500 + MG_UPDATE_CONFIRM_MS, &UV_CHANGE(uv_erase),
				   &started) == MG_UPD_BUSY &&
		      started);
		struct mg_upd w = {0};

		(void)mg_upd_write(&w, 1, 1000, &UV_CHANGE(uv_erase), &started);
		CHECK(mg_upd_press(&w, 1, 1500, true, false));
		CHECK(mg_upd_write(&w, 2, 2000, &UV_CHANGE(uv_erase), &started) == MG_UPD_BUSY &&
		      started);
		/* A change too long to cover: the press covers nothing. */
		struct mg_upd x = {0};
		uint8_t big[MG_UPD_KEY_MAX + 1] = {0};
		const struct mg_upd_req long_change = {
			.kind = MG_UPD_OTHER, .key = big, .key_len = sizeof(big)};

		(void)mg_upd_write(&x, 1, 1000, &long_change, &started);
		CHECK(mg_upd_press(&x, 1, 1500, true, false));
		CHECK(mg_upd_write(&x, 1, 2000, &long_change, &started) == MG_UPD_BUSY);
	}

	/* One refused write, then nothing until 35 s: on the same link the next
	 * write is 11 once (whether the prompt timer ran or not), then 10 with a
	 * new prompt; on a new link (the page reconnected) 10 with a new prompt,
	 * as the reference's reconnect drops the refusal. */
	for (int timer = 0; timer <= 1; timer++) {
		struct mg_upd u = {0};
		bool started;

		CHECK(mg_upd_write(&u, 1, 1000, &UV_CHANGE(uv_reset), &started) == MG_UPD_BUSY &&
		      started);
		if (timer) {
			mg_upd_expire(&u, 1000 + MG_UPDATE_PROMPT_MS); /* prompt_end_fn */
		}
		CHECK(mg_upd_write(&u, 1, 36000, &UV_CHANGE(uv_reset), &started) == MG_UPD_DENIED &&
		      !started);
		CHECK(mg_upd_write(&u, 1, 36500, &UV_CHANGE(uv_reset), &started) == MG_UPD_BUSY &&
		      started);
	}
	{
		/* A resume img_mgmt answers itself (its offset query, off 0 with
		 * the same len and sha): on a new link that query starts the bound
		 * upload again there (no prompt), and only then do its chunks go
		 * on; a chunk before it gets 11, another upload prompts, one with
		 * no sha gets 11. The last chunk asks for the slot check. */
		struct mg_upd u = {0};
		uint8_t a[32], b[32];
		bool started;

		memset(a, 0xa1, sizeof(a));
		memset(b, 0xb2, sizeof(b));
		(void)mg_upd_write(&u, 1, 1000, &UV_UP0(4096, a), &started);
		CHECK(mg_upd_press(&u, 1, 2000, true, false));
		CHECK(mg_upd_write(&u, 1, 3000, &UV_UP0(4096, a), &started) == MG_UPD_OK);
		CHECK(mg_upd_write(&u, 2, 9000, &UV_CHUNK(1024), &started) == MG_UPD_DENIED &&
		      !started);
		CHECK(mg_upd_write(&u, 2, 9050, &UV_UP0(4096, a), &started) == MG_UPD_OK);
		CHECK(mg_upd_write(&u, 2, 9100, &UV_CHUNK(2048), &started) == MG_UPD_OK);
		CHECK(mg_upd_write(&u, 2, 9150, &UV_CHUNK(4096), &started) == MG_UPD_VERIFY);
		CHECK(mg_upd_verified(&u, 2, true) == MG_UPD_OK);
		CHECK(mg_upd_write(&u, 3, 9500, &UV_UP0(4096, b), &started) == MG_UPD_BUSY &&
		      started);
		CHECK(mg_upd_write(&u, 4, 9600, &UV_UP0(4096, NULL), &started) == MG_UPD_DENIED &&
		      !started && !mg_upd_prompting(&u, 4, 9600));
		/* A mismatch at the last chunk ends the binding: the same upload
		 * on a new link prompts again. */
		CHECK(mg_upd_write(&u, 5, 9700, &UV_UP0(4096, a), &started) == MG_UPD_OK);
		CHECK(mg_upd_write(&u, 5, 9750, &UV_CHUNK(4096), &started) == MG_UPD_VERIFY);
		CHECK(mg_upd_verified(&u, 5, false) == MG_UPD_DENIED && !u.bound);
		CHECK(mg_upd_write(&u, 5, 9800, &UV_CHUNK(4096), &started) == MG_UPD_DENIED);
		CHECK(mg_upd_write(&u, 6, 9900, &UV_UP0(4096, a), &started) == MG_UPD_BUSY &&
		      started);
		/* Forget between the check and its answer: refused. */
		struct mg_upd v = {0};

		(void)mg_upd_write(&v, 1, 1000, &UV_UP0(4096, a), &started);
		CHECK(mg_upd_press(&v, 1, 2000, true, false));
		CHECK(mg_upd_write(&v, 1, 3000, &UV_UP0(4096, a), &started) == MG_UPD_OK);
		CHECK(mg_upd_write(&v, 1, 3100, &UV_CHUNK(4096), &started) == MG_UPD_VERIFY);
		mg_upd_forget(&v);
		CHECK(mg_upd_verified(&v, 1, true) == MG_UPD_DENIED);
	}
	{
		/* mg_upd_parse(): an upload read from its request, its map definite
		 * or indefinite (MCUmgr clients send either), the same fields;
		 * a malformed body, a negative integer or a repeated key is -1
		 * (refused); a read is 0; a sha that isn't 32 bytes names none. */
		static const uint8_t h_up[8] = {2, 0, 0, 0, 0, 1, 0, 1};
		static const uint8_t h_read[8] = {0, 0, 0, 0, 0, 1, 0, 0};
		static const uint8_t h_state[8] = {2, 0, 0, 0, 0, 1, 7, 0};
		/* {"off": 0, "len": 300, "sha": 32 x 0xaa, "data": h'0102'} */
		uint8_t def[64] = {0xa4, 0x63, 'o', 'f', 'f', 0x00, 0x63, 'l', 'e', 'n', 0x19, 0x01,
				   0x2c, 0x63, 's', 'h', 'a', 0x58, 0x20};
		uint8_t ind[64];
		size_t n = 19;
		struct mg_upd_req r, q;
		uint8_t key[MG_UPD_KEY_MAX];

		memset(&def[n], 0xaa, 32);
		n += 32;
		memcpy(&def[n], (const uint8_t[]){0x64, 'd', 'a', 't', 'a', 0x42, 1, 2}, 8);
		n += 8;
		ind[0] = 0xbf;
		memcpy(&ind[1], &def[1], n - 1);
		ind[n] = 0xff;
		CHECK(mg_upd_parse(h_up, def, n, &r, key) == 1);
		CHECK(r.kind == MG_UPD_UPLOAD0 && r.len == 300 && r.sha == &def[19] && r.end == 2 &&
		      r.data_len == 2 && r.data[0] == 1);
		CHECK(mg_upd_parse(h_up, ind, n + 1, &q, key) == 1);
		CHECK(q.kind == MG_UPD_UPLOAD0 && q.len == 300 && q.sha == &ind[19] && q.end == 2);
		CHECK(mg_upd_parse(h_up, ind, n, &q, key) == -1);  /* no break */
		CHECK(mg_upd_parse(h_up, def, n - 1, &q, key) == -1); /* cut short */
		CHECK(mg_upd_parse(h_read, def, n, &q, key) == 0);
		def[18] = 0x1f; /* a 31-byte sha (and one byte of it left over: malformed) */
		CHECK(mg_upd_parse(h_up, def, n, &q, key) == -1);
		def[18] = 0x20;
		def[5] = 0x20; /* off: -1 */
		CHECK(mg_upd_parse(h_up, def, n, &q, key) == -1);
		def[5] = 0x00;
		memcpy(&def[6], (const uint8_t[]){0x63, 'o', 'f', 'f'}, 4); /* "off" twice */
		CHECK(mg_upd_parse(h_up, def, n, &q, key) == -1);
		/* {"off": 64, "data": h'0102'}: a later chunk, ending at 66 */
		static const uint8_t chunk[] = {0xa2, 0x63, 'o', 'f', 'f', 0x18, 0x40, 0x64,
						'd',  'a',  't', 'a', 0x42, 1,    2};

		CHECK(mg_upd_parse(h_up, chunk, sizeof(chunk), &q, key) == 1);
		CHECK(q.kind == MG_UPD_CHUNK && q.end == 66 && q.sha == NULL);
		/* {"off": 0, "len": 300, "sha": h'aa', "data": h''}: a short sha names none */
		static const uint8_t short_sha[] = {0xa4, 0x63, 'o', 'f', 'f', 0x00, 0x63, 'l', 'e',
						    'n',  0x19, 0x01, 0x2c, 0x63, 's', 'h', 'a', 0x41,
						    0xaa, 0x64, 'd',  'a',  't',  'a', 0x40};

		CHECK(mg_upd_parse(h_up, short_sha, sizeof(short_sha), &q, key) == 1);
		CHECK(q.kind == MG_UPD_UPLOAD0 && q.sha == NULL);
		/* A state write: its group, id and body, seq left out. */
		CHECK(mg_upd_parse(h_state, chunk, sizeof(chunk), &q, key) == 1);
		CHECK(q.kind == MG_UPD_STATE && q.key == key && q.key_len == 3 + sizeof(chunk) &&
		      key[0] == 0 && key[1] == 1 && key[2] == 0 && memcmp(&key[3], chunk, sizeof(chunk)) == 0);
	}
	/* A SAFE the gadget entered itself on a release build refuses update
	 * writes (rc 11, never asked), set up or not (a gadget with no device
	 * token too); the button's SAFE, a bench or automation build and the
	 * wired arm don't; reads stay open. */
	CHECK(mg_ota_smp_closed(MG_SMP_GROUP_IMAGE, 1, MG_SMP_OP_WRITE, true, false, true,
				false));
	CHECK(mg_ota_smp_closed(MG_SMP_GROUP_OS, MG_SMP_OS_RESET, MG_SMP_OP_WRITE, true, false,
				true, false));
	CHECK(!mg_ota_smp_closed(MG_SMP_GROUP_IMAGE, 1, MG_SMP_OP_WRITE, true, true, true,
				 false));
	CHECK(!mg_ota_smp_closed(MG_SMP_GROUP_IMAGE, 1, MG_SMP_OP_WRITE, true, false, false,
				 false));
	CHECK(!mg_ota_smp_closed(MG_SMP_GROUP_IMAGE, 1, MG_SMP_OP_WRITE, true, false, true,
				 true));
	CHECK(!mg_ota_smp_closed(MG_SMP_GROUP_IMAGE, 0, MG_SMP_OP_READ, true, false, true,
				 false));
	CHECK(!mg_ota_smp_closed(MG_SMP_GROUP_IMAGE, 1, MG_SMP_OP_WRITE, false, false, true,
				 false));

	/* Which link and build ask for the press, release build unless said:
	 * smp_allowed(group, id, op, proven, safe, button_safe, release, window).
	 * An unproven link asks, set up or not (a gadget with no device token,
	 * not set up or after Forget, asks the same); a proven link (the owner's
	 * app) and a bench or automation build never ask. */
	CHECK(!mg_ota_smp_allowed(MG_SMP_GROUP_IMAGE, 1, MG_SMP_OP_WRITE, false, false, false,
				  true, false));
	CHECK(mg_ota_smp_allowed(MG_SMP_GROUP_IMAGE, 1, MG_SMP_OP_WRITE, true, false, false,
				 true, false));
	CHECK(mg_ota_smp_allowed(MG_SMP_GROUP_IMAGE, 1, MG_SMP_OP_WRITE, false, false, false,
				 false, false));
	{
		/* A release gadget with no device token: its unproven upload gets
		 * 10 and the prompt, an injected press confirms nothing, a real
		 * press does (then 0); after Forget the press is still needed. A
		 * bench or automation build takes it with none. */
		struct mg_upd u = {0};
		uint8_t a[32];
		bool started;

		memset(a, 0xa1, sizeof(a));
		CHECK(!mg_ota_smp_allowed(MG_SMP_GROUP_IMAGE, 1, MG_SMP_OP_WRITE, false, false,
					  false, mg_ota_release_build(false, false), false));
		CHECK(mg_upd_write(&u, 1, 1000, &UV_UP0(4096, a), &started) == MG_UPD_BUSY && started);
		CHECK(!mg_upd_press(&u, 1, 1500, false, false)); /* injected */
		CHECK(mg_upd_write(&u, 1, 2000, &UV_UP0(4096, a), &started) == MG_UPD_BUSY);
		CHECK(mg_upd_press(&u, 1, 2500, true, false));
		CHECK(mg_upd_write(&u, 1, 3000, &UV_UP0(4096, a), &started) == MG_UPD_OK);
		mg_upd_forget(&u); /* Forget: the binding ends, the prompt's rate stays */
		CHECK(mg_upd_write(&u, 1, 4000, &UV_UP0(4096, a), &started) == MG_UPD_BUSY &&
		      !started);
		CHECK(mg_upd_write(&u, 1, 11000, &UV_UP0(4096, a), &started) == MG_UPD_BUSY &&
		      started);
		CHECK(mg_ota_smp_allowed(MG_SMP_GROUP_IMAGE, 1, MG_SMP_OP_WRITE, false, false, false,
					 mg_ota_release_build(true, false), false));
		CHECK(mg_ota_smp_allowed(MG_SMP_GROUP_IMAGE, 1, MG_SMP_OP_WRITE, false, false, false,
					 mg_ota_release_build(false, true), false));
	}
	{
		/* After Forget the prompt ends and nothing it admitted carries
		 * over (mg_setup_store_erased()). */
		struct mg_upd u = {0};
		bool started;

		CHECK(mg_upd_write(&u, 1, 1000, &UV_CHANGE(uv_erase), &started) == MG_UPD_BUSY &&
		      started);
		CHECK(mg_upd_press(&u, 1, 2000, true, false));
		CHECK(mg_upd_allowed(&u, 1));
		mg_upd_forget(&u);
		CHECK(!mg_upd_prompting(&u, 1, 2001) && !mg_upd_allowed(&u, 1));
		CHECK(mg_upd_write(&u, 1, 2100, &UV_CHANGE(uv_erase), &started) == MG_UPD_BUSY);
	}
}

static void test_boot_record(void)
{
	struct mg_boot_rec r;
	struct mg_boot_crash c;
	bool has;
	char text[96];

	/* Power-on: garbage RAM fails the check and starts from zero. */
	memset(&r, 0xa5, sizeof(r));
	CHECK(mg_boot_start(&r, 3, &c, &has) == MG_BOOT_NORMAL);
	CHECK(!has && r.unhealthy == 1 && r.boots == 1 && r.magic == MG_BOOT_MAGIC);

	/* Three boots in a row never get healthy: the fourth runs SAFE. */
	CHECK(mg_boot_start(&r, 3, &c, &has) == MG_BOOT_NORMAL); /* 1 before */
	CHECK(mg_boot_start(&r, 3, &c, &has) == MG_BOOT_NORMAL); /* 2 before */
	CHECK(mg_boot_start(&r, 3, &c, &has) == MG_BOOT_SAFE);   /* 3 before */
	CHECK(mg_boot_start(&r, 3, &c, &has) == MG_BOOT_SAFE);
	/* A healthy (SAFE) boot clears it: the next reset is normal again. */
	mg_boot_healthy(&r);
	CHECK(r.unhealthy == 0);
	CHECK(mg_boot_start(&r, 3, &c, &has) == MG_BOOT_NORMAL);
	mg_boot_healthy(&r);
	/* 0 turns SAFE mode off. */
	for (int i = 0; i < 6; i++) {
		CHECK(mg_boot_start(&r, 0, &c, &has) == MG_BOOT_NORMAL);
	}
	mg_boot_healthy(&r);

	/* A breadcrumb survives the reset once, then it's gone. */
	mg_boot_crash(&r, 3, 0x0001a2b4, 42, "fault in mg_audio");
	mg_boot_crash(&r, 0x100, 0, 50, "watchdog: later"); /* the first one stays */
	CHECK(mg_boot_start(&r, 3, &c, &has) == MG_BOOT_NORMAL);
	CHECK(has && c.reason == 3 && c.pc == 0x0001a2b4 && c.uptime_s == 42);
	CHECK_STR(c.what, "fault in mg_audio");
	mg_boot_crash_text(&c, text, sizeof(text));
	CHECK_STR(text, "fault in mg_audio (reason 3) at pc 0x0001a2b4, up 42 s");
	CHECK(mg_boot_start(&r, 3, &c, &has) == MG_BOOT_NORMAL && !has);

	/* A watchdog breadcrumb, a long name cut to fit. */
	mg_boot_crash(&r, MG_BOOT_REASON_WATCHDOG, 0, 9,
		      "watchdog: a thread with a very long name indeed stuck");
	CHECK(mg_boot_start(&r, 3, &c, &has) == MG_BOOT_NORMAL && has); /* 2 before */
	CHECK(strlen(c.what) == MG_BOOT_WHAT - 1);
	mg_boot_crash_text(&c, text, sizeof(text));
	CHECK(strstr(text, "watchdog: a thread") == text && strstr(text, ", up 9 s"));

	/* A corrupted record (one flipped bit) is dropped, not trusted. */
	r.unhealthy = 7;
	CHECK(mg_boot_start(&r, 3, &c, &has) == MG_BOOT_NORMAL && r.unhealthy == 1);

	/* The early watchdog: a hang before the supervisor leaves its
	 * breadcrumb even when the record isn't valid yet (power-on RAM with a
	 * stale magic), and never seals garbage counts into it. */
	memset(&r, 0x5a, sizeof(r));
	r.magic = MG_BOOT_MAGIC; /* the right magic, a wrong CRC */
	mg_boot_crash(&r, MG_BOOT_REASON_WATCHDOG, 0, 0, "watchdog: hung before the supervisor");
	CHECK(r.unhealthy == 0 && r.reach_resets == 0 && r.att_dead == 0);
	CHECK(mg_boot_start(&r, 3, &c, &has) == MG_BOOT_NORMAL);
	CHECK(has && strcmp(c.what, "watchdog: hung before the supervisor") == 0);

	/* The mode the firmware reads. */
	mg_health_set_mode(MG_BOOT_SAFE);
	CHECK(mg_health_safe());
	mg_health_set_mode(MG_BOOT_NORMAL);
	CHECK(!mg_health_safe());
}

/* The BLE TX path is progress too: with a link up, something owed and
 * nothing completed, the supervisor restarts the gadget as a peer loss. */
static void test_tx_progress(void)
{
	struct mg_txp n, a;
	unsigned int done = 0;
	const uint32_t sup = 4000; /* the link's supervision timeout */

	printf("  TX path progress: sent callbacks and ACL credits, 10 s / silent clock\n");
	/* Notifications in flight, the peer writing meanwhile, nothing completes:
	 * dead at 10 s, not before. */
	mg_txp_init(&n);
	mg_txp_owe(&n, 1000);
	CHECK(!mg_txp_dead(&n, 10999, true, 5000, sup));
	CHECK(mg_txp_dead(&n, 11000, true, 5000, sup));
	/* The same with no link: never. */
	CHECK(!mg_txp_dead(&n, 60000, false, 5000, sup));
	/* Slow but moving (a completion every 9 s, a slow system work queue):
	 * never dead. */
	mg_txp_init(&n);
	mg_txp_owe(&n, 0);
	for (uint32_t t = 9000; t <= 90000; t += 9000) {
		CHECK(!mg_txp_dead(&n, t, true, t, sup));
		mg_txp_done(&n, t, true);
	}
	/* All completed: idle, never dead however long. */
	mg_txp_done(&n, 100000, false);
	CHECK(!mg_txp_dead(&n, 200000, true, 150000, sup));
	/* The peer silent since the stall began (it walked away): the
	 * controller's supervision timeout disconnects at 4 s, so the link is
	 * down and nothing trips; a link still up after 4 s + 2 s trips. */
	mg_txp_init(&n);
	mg_txp_owe(&n, 1000);
	CHECK(mg_txp_limit(&n, 500, sup) == 6000);
	CHECK(!mg_txp_dead(&n, 5000, false, 500, sup)); /* disconnected at 4 s: no reset */
	CHECK(!mg_txp_dead(&n, 6999, true, 500, sup));
	CHECK(mg_txp_dead(&n, 7000, true, 500, sup));
	/* A long supervision timeout is capped at 30 s (a link
	 * held up while our sends fail ends within 30 s); the peer speaking
	 * during the stall brings it to 10 s. */
	CHECK(mg_txp_limit(&n, 500, 32000) == MG_TX_SILENT_MAX_MS);
	CHECK(mg_txp_limit(&n, 1000, 32000) == MG_TX_DEAD_MS);
	/* The ACL leg, no notification at all (a CCC write whose response the
	 * controller never completes): one packet held, sampled every 100 ms,
	 * none completed, the peer alive: dead at 10 s. */
	mg_txp_init(&a);
	for (uint32_t t = 0; t < 10000; t += 100) {
		mg_txp_acl(&a, t, 1, done, &done);
		CHECK(!mg_txp_dead(&a, t, true, 50, sup));
	}
	mg_txp_acl(&a, 10000, 1, done, &done);
	CHECK(mg_txp_dead(&a, 10000, true, 50, sup));
	/* Completions coming back, or none held: moving / idle. */
	mg_txp_init(&a);
	done = 0;
	mg_txp_acl(&a, 0, 3, 0, &done);
	mg_txp_acl(&a, 9000, 3, 1, &done);
	CHECK(!mg_txp_dead(&a, 18000, true, 9500, sup));
	mg_txp_acl(&a, 18100, 0, 1, &done);
	CHECK(!mg_txp_dead(&a, 60000, true, 50000, sup));
}

static void test_reach_att(void)
{
	struct mg_reach_links l = {0};

	printf("  reachability: a link counts once ATT works; ATT-dead links\n");
	/* On a set-up gadget only a link past its proof counts; a stranger's, its requests refused,
	 * moves nothing; a gadget that isn't set up has no gate. */
	CHECK(!mg_reach_exchange_counts(true, true, false)); /* refused traffic */
	CHECK(mg_reach_exchange_counts(true, true, true));
	CHECK(mg_reach_exchange_counts(true, false, false));
	CHECK(!mg_reach_exchange_counts(false, false, true));
	{
		struct mg_reach_links s2 = {.dead = 2};

		/* A stranger's link whose ATT works (mg_ble.c: not gated, and not
		 * failed since its ATT worked): the run of 2 stays 2. */
		CHECK(!mg_reach_link_ended(&s2, mg_reach_exchange_counts(true, true, false), false));
		CHECK(s2.dead == 2);
		CHECK(!mg_reach_link_ended(&s2, mg_reach_exchange_counts(true, true, true), false));
		CHECK(s2.dead == 0); /* the owner's link clears it */
	}
	/* Three links in a row ending with no exchange while ours failed: a loss
	 * on the third. */
	CHECK(!mg_reach_link_ended(&l, false, true));
	CHECK(!mg_reach_link_ended(&l, false, true));
	CHECK(mg_reach_link_ended(&l, false, true));
	/* One good exchange starts the run over. */
	l = (struct mg_reach_links){0};
	CHECK(!mg_reach_link_ended(&l, false, true));
	CHECK(!mg_reach_link_ended(&l, false, true));
	CHECK(!mg_reach_link_ended(&l, true, false));
	CHECK(l.dead == 0);
	CHECK(!mg_reach_link_ended(&l, false, true));
	CHECK(!mg_reach_link_ended(&l, false, true));
	/* A link that drops normally (an exchange completed; or short, nothing
	 * failed) never counts. */
	l = (struct mg_reach_links){0};
	for (int i = 0; i < 10; i++) {
		CHECK(!mg_reach_link_ended(&l, false, false));
		CHECK(!mg_reach_link_ended(&l, true, true));
	}
	CHECK(l.dead == 0);
	/* Our own request: failed, or unanswered 5 s when the link ended. */
	CHECK(mg_reach_own_failed(true, false, 0, 100));
	CHECK(!mg_reach_own_failed(false, true, 1000, 5999));
	CHECK(mg_reach_own_failed(false, true, 1000, 6000));
	CHECK(!mg_reach_own_failed(false, false, 0, 60000));
	/* A link without ATT is unreachable time: up but not counted, so 30 s
	 * of it after reachability is a loss as before. */
	struct mg_reach r;

	mg_reach_init(&r, 0);
	CHECK(mg_reach_step(&r, 1000, true, false) == MG_REACH_NONE);   /* advertising */
	CHECK(mg_reach_step(&r, 30999, false, false) == MG_REACH_NONE); /* link up, ATT dead */
	CHECK(mg_reach_step(&r, 31000, false, false) == MG_REACH_RESET_LOST);
	/* Only the gadget's own path counts: the host answering a
	 * peer's MTU request alone never marks a link reachable. */
	CHECK(!mg_reach_att_counts(MG_ATT_STACK_ANSWERED));
	CHECK(mg_reach_att_counts(MG_ATT_OWN_ANSWERED));
	/* On-air evidence only. A peer's first Write Request proves nothing
	 * about our answers; the next one does. A completed notification is
	 * host-side (it may be a flushed packet). Only an exchange or the loss
	 * clears the dead-link run; healthy never does (the boot-record rows
	 * below). */
	CHECK(!mg_reach_att_counts(MG_ATT_PEER_FIRST_REQUEST));
	CHECK(mg_reach_att_counts(MG_ATT_PEER_NEXT_REQUEST));
	CHECK(!mg_reach_att_counts(MG_ATT_NOTIFY_COMPLETED));
	/* The trial window: an image still on test 30 min after boot restarts
	 * on its own (MCUboot reverts it); a confirmed one never does. */
	CHECK(!mg_ota_trial_expired(false, MG_OTA_TRIAL_MS - 1, 0));
	CHECK(mg_ota_trial_expired(false, MG_OTA_TRIAL_MS, 0));
	CHECK(!mg_ota_trial_expired(true, 10 * MG_OTA_TRIAL_MS, 0));
	/* The Keep signal: kept when healthy (board-ready) AND a proven link OR
	 * any central's image-list read answered after the boot (the web
	 * updater's post-install read, unproven); no exchange, or a dead (never
	 * healthy) image, never: the trial deadline reverts it.
	 * confirm_ready(healthy, link_proven, update_read) */
	CHECK(!mg_ota_confirm_ready(true, false, false)); /* no exchange */
	CHECK(!mg_ota_confirm_ready(false, true, true));  /* dead image */
	CHECK(mg_ota_confirm_ready(true, true, false));   /* a proven link */
	CHECK(mg_ota_confirm_ready(true, false, true));   /* the web updater's read */
	{
		/* No exchange: the trial runs out and reverts. */
		int64_t counted = 0;

		for (int64_t t = 0; t <= MG_OTA_TRIAL_MS + 10000; t += 10000) {
			counted = mg_ota_trial_count(counted, 10000, false);
		}
		CHECK(mg_ota_trial_expired(mg_ota_confirm_ready(true, false, false), counted, 0));
		CHECK(!mg_ota_trial_expired(mg_ota_confirm_ready(true, false, true), counted, 0));
	}
	/* The run lives in the boot record: a reset keeps it, a healthy boot
	 * doesn't clear it, and the third ATT-dead link is a peer restart,
	 * which starts it over and never counts toward SAFE. (A link held up
	 * while our sends fail is a peer restart of its own: the TX check,
	 * test_tx_progress.) */
	{
		struct mg_boot_rec r = {0};
		struct mg_boot_crash c;
		bool has;
		struct mg_reach_links run;

		(void)mg_boot_start(&r, 3, &c, &has);
		/* two links ended ATT-dead */
		for (int i = 0; i < 2; i++) {
			run = (struct mg_reach_links){.dead = r.att_dead};
			CHECK(!mg_reach_link_ended(&run, false, true));
			mg_boot_att_dead_set(&r, run.dead);
		}
		mg_boot_healthy(&r); /* healthy in between: the run stays */
		CHECK(r.att_dead == 2);
		(void)mg_boot_start(&r, 3, &c, &has); /* a reset: kept */
		CHECK(r.att_dead == 2);
		/* the third: a peer restart */
		run = (struct mg_reach_links){.dead = r.att_dead};
		CHECK(mg_reach_link_ended(&run, false, true));
		uint8_t before = r.unhealthy;

		mg_boot_peer_restart(&r);
		CHECK(r.reach_resets == 0 && r.att_dead == 0 && r.unhealthy == before - 1);
	}
}

/* A boot that isn't reachable is a failed boot; a gadget stays reachable;
 * the loss count escalates across boots and is bounded in SAFE. */
static void test_reach(void)
{
	struct mg_reach r;
	struct mg_boot_rec b;
	struct mg_boot_crash c;
	bool has;

	printf("  reachability: failed boots, losses, the SAFE bound\n");
	/* Never reachable: reset at 30 s, once. */
	mg_reach_init(&r, 1000);
	CHECK(mg_reach_step(&r, 30999, false, false) == MG_REACH_NONE);
	CHECK(mg_reach_step(&r, 31000, false, false) == MG_REACH_RESET_BOOT);
	CHECK(mg_reach_step(&r, 32000, false, false) == MG_REACH_NONE);
	/* A link before any advertising counts. */
	mg_reach_init(&r, 0);
	CHECK(mg_reach_step(&r, 5000, true, false) == MG_REACH_NONE);
	CHECK(mg_reach_step(&r, 34999, false, false) == MG_REACH_NONE);
	CHECK(mg_reach_step(&r, 35000, false, false) == MG_REACH_RESET_LOST);
	/* Brief gaps (an advertiser restart) are nothing; a 10 min stretch
	 * clears the count once. */
	mg_reach_init(&r, 0);
	CHECK(mg_reach_step(&r, 1000, true, false) == MG_REACH_NONE);
	CHECK(mg_reach_step(&r, 2000, false, false) == MG_REACH_NONE);
	CHECK(mg_reach_step(&r, 3000, true, false) == MG_REACH_NONE);
	CHECK(mg_reach_step(&r, 3000 + MG_REACH_CLEAR_MS - 1, true, false) == MG_REACH_NONE);
	CHECK(mg_reach_step(&r, 3000 + MG_REACH_CLEAR_MS, true, false) == MG_REACH_CLEAR);
	CHECK(mg_reach_step(&r, 4000 + MG_REACH_CLEAR_MS, true, false) == MG_REACH_NONE);
	/* Power off on its way to System OFF: exempt. */
	CHECK(mg_reach_step(&r, 5000 + MG_REACH_CLEAR_MS, false, true) == MG_REACH_NONE);
	CHECK(mg_reach_step(&r, 90000 + MG_REACH_CLEAR_MS, false, true) == MG_REACH_NONE);

	/* Losses escalate across boots: each boot after one gets healthy, yet
	 * the third loss in a row reaches SAFE; a 10 min stretch clears it. */
	memset(&b, 0, sizeof(b));
	CHECK(mg_boot_start(&b, 3, &c, &has) == MG_BOOT_NORMAL);
	for (int i = 1; i <= 3; i++) {
		mg_boot_healthy(&b);
		CHECK(mg_boot_reach_reset(&b, false, true));
		CHECK(b.reach_resets == i && b.unhealthy == i);
		CHECK(mg_boot_start(&b, 3, &c, &has) == (i < 3 ? MG_BOOT_NORMAL : MG_BOOT_SAFE));
	}
	/* The breadcrumb says it plainly, with no fault reason or pc. */
	char text[96];

	mg_boot_crash(&b, MG_BOOT_REASON_UNREACHABLE, 0, 82, "unreachable 30 s");
	CHECK(mg_boot_start(&b, 0, &c, &has) == MG_BOOT_NORMAL && has);
	mg_boot_crash_text(&c, text, sizeof(text));
	CHECK_STR(text, "unreachable 30 s, up 82 s");
	b.unhealthy = 3;
	/* Inside SAFE: three unreachable restarts, then it stays up. The
	 * healthy timer no longer clears that bound (it can fire during an
	 * unreachable stretch); only reachability does. */
	CHECK(mg_boot_reach_reset(&b, true, false));
	CHECK(mg_boot_reach_reset(&b, true, false));
	CHECK(mg_boot_reach_reset(&b, true, false));
	CHECK(!mg_boot_reach_reset(&b, true, false));
	CHECK(!mg_boot_reach_reset(&b, true, true));
	mg_boot_healthy(&b);
	CHECK(b.safe_reach == 3 && !mg_boot_reach_reset(&b, true, false));
	(void)mg_boot_reach_clear(&b, false);
	CHECK(b.reach_resets == 0 && b.safe_reach == 0);
	/* The record still checks out across a reset. */
	uint8_t before = b.unhealthy;

	CHECK(mg_boot_start(&b, 0, &c, &has) == MG_BOOT_NORMAL && b.unhealthy == before + 1);
}

/* Losses a peer could cause restart the gadget but never reach SAFE; past
 * three since the owner's last clear (a token proof, the button, a new
 * image), the link goes instead; a SAFE from reachability goes back by
 * itself. */
static void test_peer_losses(void)
{
	struct mg_boot_rec b;
	struct mg_boot_crash c;
	bool has;
	struct mg_reach r;

	printf("  peer-caused losses: restart, never SAFE; the restart bound; SAFE from reach exits\n");
	/* An attacker-style peer: connects, never answers ATT. Each boot it
	 * holds the link before the healthy mark; twenty restarts, never SAFE. */
	memset(&b, 0, sizeof(b));
	for (int i = 0; i < 20; i++) {
		CHECK(mg_boot_start(&b, 3, &c, &has) == MG_BOOT_NORMAL);
		mg_boot_peer_restart(&b);
		CHECK(b.unhealthy == 0 && b.reach_resets == 0);
	}
	/* A crash loop still reaches SAFE. */
	memset(&b, 0, sizeof(b));
	for (int i = 0; i < 3; i++) {
		CHECK(mg_boot_start(&b, 3, &c, &has) == MG_BOOT_NORMAL);
	}
	CHECK(mg_boot_start(&b, 3, &c, &has) == MG_BOOT_SAFE);
	/* The stretch: a link up in it is a peer's doing; none is the gadget's. */
	mg_reach_init(&r, 0);
	CHECK(mg_reach_step_link(&r, 1000, true, false, false) == MG_REACH_NONE);
	CHECK(mg_reach_step_link(&r, 2000, false, false, true) == MG_REACH_NONE);   /* ATT-silent link */
	CHECK(mg_reach_step_link(&r, 31000, false, false, true) == MG_REACH_RESET_LOST);
	CHECK(mg_reach_peer_loss(&r));
	mg_reach_init(&r, 0);
	CHECK(mg_reach_step_link(&r, 1000, true, false, false) == MG_REACH_NONE);
	CHECK(mg_reach_step_link(&r, 31000, false, false, false) == MG_REACH_RESET_LOST); /* no advertising, no link */
	CHECK(!mg_reach_peer_loss(&r));
	mg_reach_init(&r, 0); /* a link holding it from boot */
	CHECK(mg_reach_step_link(&r, 30000, false, false, true) == MG_REACH_RESET_BOOT);
	CHECK(mg_reach_peer_loss(&r));
	/* Not-advertising losses still escalate across boots. */
	memset(&b, 0, sizeof(b));
	CHECK(mg_boot_start(&b, 3, &c, &has) == MG_BOOT_NORMAL);
	for (int i = 1; i <= 3; i++) {
		mg_boot_healthy(&b);
		CHECK(mg_boot_reach_reset(&b, false, true));
		CHECK(mg_boot_start(&b, 3, &c, &has) == (i < 3 ? MG_BOOT_NORMAL : MG_BOOT_SAFE));
	}
	CHECK(b.flags & MG_BOOT_F_SAFE_REACH);
	/* That SAFE goes back by itself after 10 min reachable; the button's
	 * SAFE doesn't. */
	CHECK(!mg_boot_reach_clear(&b, true));
	CHECK(b.unhealthy != 0 && b.reach_resets == 0);
	memset(&b, 0, sizeof(b)); /* the same SAFE again, through the record's API */
	(void)mg_boot_start(&b, 3, &c, &has);
	for (int i = 1; i <= 3; i++) {
		mg_boot_healthy(&b);
		CHECK(mg_boot_reach_reset(&b, false, true));
		(void)mg_boot_start(&b, 3, &c, &has);
	}
	CHECK(mg_boot_valid(&b) && (b.flags & MG_BOOT_F_SAFE_REACH));
	CHECK(mg_boot_reach_clear(&b, false));
	CHECK(b.unhealthy == 0 && b.reach_resets == 0 && !(b.flags & MG_BOOT_F_SAFE_REACH));
	CHECK(mg_boot_start(&b, 3, &c, &has) == MG_BOOT_NORMAL);
	/* The restart rate bound: three peer restarts since the owner's last
	 * clear, then the link goes instead (still advertising); once per boot
	 * after 10 min up a restart again; only the owner's clear resets it,
	 * never a plain exchange, a forced restart or a healthy boot. */
	memset(&b, 0, sizeof(b));
	for (int i = 0; i < 3; i++) {
		(void)mg_boot_start(&b, 3, &c, &has);
		CHECK(mg_reach_peer_restart_ok(mg_boot_peer_restarts(&b), 40000, false, false));
		mg_boot_peer_restart(&b);
	}
	(void)mg_boot_start(&b, 3, &c, &has);
	mg_boot_healthy(&b);
	CHECK(mg_boot_peer_restarts(&b) == 3);
	CHECK(!mg_reach_peer_restart_ok(mg_boot_peer_restarts(&b), 40000, false, false)); /* drop the link */
	CHECK(mg_reach_peer_restart_ok(mg_boot_peer_restarts(&b), MG_REACH_PEER_LATE_MS, false, false));
	CHECK(!mg_reach_peer_restart_ok(mg_boot_peer_restarts(&b), MG_REACH_PEER_LATE_MS, true, false));
	mg_boot_peer_restart(&b);
	CHECK(mg_boot_peer_restarts(&b) == 3); /* capped */
	/* A plain peer exchange clears the ATT-dead run but not the bound:
	 * only a token proof, the user's button or a new image does. */
	mg_boot_att_dead_set(&b, 2); /* through the record's own writer: sealed */
	mg_boot_exchange(&b);
	CHECK(mg_boot_peer_restarts(&b) == 3 && b.att_dead == 0);
	mg_boot_peer_bound_clear(&b);
	CHECK(mg_boot_peer_restarts(&b) == 0);
	CHECK(mg_reach_peer_restart_ok(mg_boot_peer_restarts(&b), 1000, false, false));
	/* A peer restart counts toward nothing: never while an update runs on
	 * test (MCUboot would revert it at the restart: the link goes instead),
	 * and never in the unhealthy count (its breadcrumb is kept as a crash's
	 * is, and reads as a peer's). */
	CHECK(!mg_reach_peer_restart_ok(0, 1000, false, true));
	CHECK(!mg_reach_peer_restart_ok(0, MG_REACH_PEER_LATE_MS, false, true));
	memset(&b, 0, sizeof(b));
	(void)mg_boot_start(&b, 3, &c, &has);
	(void)mg_boot_start(&b, 3, &c, &has); /* one unhealthy boot before */
	{
		uint8_t u = b.unhealthy;

		mg_boot_peer_restart(&b);
		CHECK(b.unhealthy == u - 1);
		mg_boot_crash(&b, MG_BOOT_REASON_PEER_LOSS, 0, 5, "peer: TX path stuck");
		CHECK(b.unhealthy == u - 1);
		CHECK(mg_boot_start(&b, 3, &c, &has) == MG_BOOT_NORMAL && has &&
		      c.reason == MG_BOOT_REASON_PEER_LOSS);
	}
	/* Its breadcrumb reads plainly (no fault reason or pc). */
	{
		char text[96];

		memset(&b, 0, sizeof(b));
		mg_boot_crash(&b, MG_BOOT_REASON_PEER_LOSS, 0, 99, "peer: ATT dead on 3 links");
		CHECK(mg_boot_start(&b, 0, &c, &has) == MG_BOOT_NORMAL && has);
		mg_boot_crash_text(&c, text, sizeof(text));
		CHECK_STR(text, "peer: ATT dead on 3 links, up 99 s");
	}
	/* A torn seal is refused everywhere alike. */
	memset(&b, 0, sizeof(b));
	(void)mg_boot_start(&b, 3, &c, &has);
	CHECK(mg_boot_valid(&b));
	b.crc ^= 1;
	CHECK(!mg_boot_valid(&b));
	CHECK(mg_boot_reach_reset(&b, false, true) && b.reach_resets == 1 && b.unhealthy == 1);
}

/* The update prompt's input order (mg_input_policy.h); a setup confirm takes
 * any one press. */
static void test_input_policy(void)
{
	printf("  the update prompt's input order (a setup confirm takes any press)\n");
	/* The update prompt: injected input queued before it goes, real stays in
	 * order. */
	CHECK(mg_input_keep_at_prompt(true, false));          /* real events keep their order */
	CHECK(!mg_input_keep_at_prompt(false, false));        /* injected before the prompt */
	CHECK(mg_input_keep_at_prompt(false, true));
}

/* The trial tick as trial_fn runs it (mg_ota_trial_tick()). */
static void test_trial_tick(void)
{
	int64_t c = 0, t;
	enum mg_ota_trial_act a = MG_OTA_TRIAL_GO_ON;

	printf("  the trial tick (REVERT goes on, staged stands down, cap wins, accepted chunks only)\n");
	/* A trial image that just swapped in reads REVERT: it keeps counting and
	 * reverts at 30 min. */
	for (t = 10000; t <= MG_OTA_TRIAL_MS && a == MG_OTA_TRIAL_GO_ON; t += 10000) {
		a = mg_ota_trial_tick(&c, 10000, t, false, MG_OTA_SWAP_REVERT, false, 0);
	}
	CHECK(a == MG_OTA_TRIAL_REVERT && t - 10000 == MG_OTA_TRIAL_MS);
	/* A newer image staged (TEST/PERM), or a confirm: done. */
	c = 0;
	CHECK(mg_ota_trial_tick(&c, 10000, 10000, false, MG_OTA_SWAP_TEST, false, 0) ==
	      MG_OTA_TRIAL_DONE);
	CHECK(mg_ota_trial_tick(&c, 10000, 10000, false, MG_OTA_SWAP_PERM, false, 0) ==
	      MG_OTA_TRIAL_DONE);
	CHECK(mg_ota_trial_tick(&c, 10000, 10000, true, MG_OTA_SWAP_NONE, false, 0) ==
	      MG_OTA_TRIAL_DONE);
	/* An upload moving defers the 30 min restart, never the 4 h cap. */
	c = MG_OTA_TRIAL_MS;
	CHECK(mg_ota_trial_tick(&c, 10000, 60 * 60 * 1000, false, MG_OTA_SWAP_REVERT, false,
				60 * 60 * 1000 - 5000) == MG_OTA_TRIAL_GO_ON);
	CHECK(mg_ota_trial_tick(&c, 10000, MG_OTA_TRIAL_CAP_MS, false, MG_OTA_SWAP_REVERT, false,
				MG_OTA_TRIAL_CAP_MS - 1000) == MG_OTA_TRIAL_REVERT);
	/* A peer seen in each interval (dropping around the ticks) is never
	 * charged: only the cap reverts. */
	c = 0;
	a = MG_OTA_TRIAL_GO_ON;
	for (t = 10000; t < MG_OTA_TRIAL_CAP_MS && a == MG_OTA_TRIAL_GO_ON; t += 10000) {
		a = mg_ota_trial_tick(&c, 10000, t, false, MG_OTA_SWAP_REVERT, true, 0);
	}
	CHECK(c == 0 && a == MG_OTA_TRIAL_GO_ON);
	CHECK(mg_ota_trial_tick(&c, 10000, MG_OTA_TRIAL_CAP_MS, false, MG_OTA_SWAP_REVERT, true, 0) ==
	      MG_OTA_TRIAL_REVERT);
	/* What pauses a tick (mg_ota_trial_unproven()): one unproven link
	 * connects before tick 10, is still up at tick 10 and drops just before
	 * tick 20. Ticks 10 and 20 both pause. */
	CHECK(mg_ota_trial_unproven(true, true, false, false));  /* tick 10: connected in it */
	CHECK(mg_ota_trial_unproven(true, false, false, true));  /* tick 20: gone, the drop latched */
	CHECK(mg_ota_trial_unproven(false, false, false, true)); /* even without the latch */
	CHECK(!mg_ota_trial_unproven(false, false, false, false)); /* tick 30: nothing */
	CHECK(!mg_ota_trial_unproven(false, true, true, false));   /* a proven link counts */
}

static void test_reach_safe_trial(void)
{
	struct mg_boot_rec b;
	struct mg_boot_crash c;
	bool has;
	struct mg_reach r;

	printf("  links between samples, reach-SAFE exits, no peer restart in SAFE, trial clock\n");
	/* A link wholly between two samples is still a peer's stretch: the
	 * sampler passes the latched edge (mg_transport_link_seen_take()). */
	mg_reach_init(&r, 0);
	CHECK(mg_reach_step_link(&r, 1000, true, false, false) == MG_REACH_NONE);
	CHECK(mg_reach_step_link(&r, 2000, false, false, false) == MG_REACH_NONE);
	CHECK(mg_reach_step_link(&r, 3000, false, false, true) == MG_REACH_NONE); /* edge latched */
	CHECK(mg_reach_step_link(&r, 31000, false, false, false) == MG_REACH_RESET_LOST);
	CHECK(mg_reach_peer_loss(&r));
	/* Three never-reachable boots: SAFE, marked reach-SAFE, which exits. */
	memset(&b, 0, sizeof(b));
	CHECK(mg_boot_start(&b, 3, &c, &has) == MG_BOOT_NORMAL);
	for (int i = 1; i <= 3; i++) {
		CHECK(mg_boot_reach_reset(&b, false, false));
		CHECK(mg_boot_start(&b, 3, &c, &has) == (i < 3 ? MG_BOOT_NORMAL : MG_BOOT_SAFE));
	}
	CHECK(b.flags & MG_BOOT_F_SAFE_REACH);
	CHECK(mg_boot_reach_clear(&b, false));
	/* Mostly crashes, one reach loss: not a reach-SAFE. */
	memset(&b, 0, sizeof(b));
	(void)mg_boot_start(&b, 3, &c, &has);
	(void)mg_boot_start(&b, 3, &c, &has); /* two crash boots */
	CHECK(mg_boot_reach_reset(&b, false, true));
	CHECK(mg_boot_start(&b, 3, &c, &has) == MG_BOOT_NORMAL);
	CHECK(mg_boot_start(&b, 3, &c, &has) == MG_BOOT_SAFE);
	CHECK(!(b.flags & MG_BOOT_F_SAFE_REACH));
	/* SAFE (either kind) and an image on test never peer-restart. */
	CHECK(!mg_reach_peer_restart_ok(0, 1000, false, true));
	CHECK(mg_reach_peer_restart_ok(0, 1000, false, false));
	/* The trial clock (it pauses while an unproven peer holds the link; a
	 * 4 h cap): a stranger 2 h, then the owner proves and confirms: no
	 * revert. */
	{
		int64_t counted = 0, t = 0;

		for (; t < 2 * 60 * 60 * 1000; t += 10000) {
			counted = mg_ota_trial_count(counted, 10000, true); /* unproven peer */
			CHECK(!mg_ota_trial_expired(false, counted, t));
		}
		CHECK(counted == 0);
		for (int i = 0; i < 6; i++, t += 10000) {
			counted = mg_ota_trial_count(counted, 10000, false); /* proven */
		}
		CHECK(counted == 60000 && !mg_ota_trial_expired(false, counted, t));
		CHECK(!mg_ota_trial_expired(true, counted, t)); /* confirmed: never */
		/* Unproven links nonstop: the 4 h cap reverts. */
		counted = 0;
		CHECK(!mg_ota_trial_expired(false, counted, MG_OTA_TRIAL_CAP_MS - 1));
		CHECK(mg_ota_trial_expired(false, counted, MG_OTA_TRIAL_CAP_MS));
		/* No link: 30 min counted reverts. */
		counted = 0;
		for (t = 0; t < MG_OTA_TRIAL_MS; t += 10000) {
			counted = mg_ota_trial_count(counted, 10000, false);
		}
		CHECK(mg_ota_trial_expired(false, counted, t));
		/* A moving upload defers; one stopped a minute ago doesn't. */
		CHECK(mg_ota_trial_upload_moving(t - 5000, t));
		CHECK(!mg_ota_trial_upload_moving(t - MG_OTA_TRIAL_UPLOAD_IDLE_MS, t));
		CHECK(!mg_ota_trial_upload_moving(0, t));
		/* A crash during the trial reverts at once: MCUboot does that at
		 * the reset (no app policy; smp_link's revert row). */
	}
}

/* The TX path's ACL leg from a completion counter: one packet always held
 * with completions between samples is progress. */
static void test_tx_acl_counter(void)
{
	struct mg_txp a;
	unsigned int last = 0, done = 0;

	printf("  TX path: the ACL leg counts completions, not occupancy\n");
	mg_txp_init(&a);
	for (uint32_t t = 0; t <= 60000; t += 100) {
		done += 1; /* one completed, one queued behind it: held stays 1 */
		mg_txp_acl(&a, t, 1, done, &last);
		CHECK(!mg_txp_dead(&a, t, true, t, 4000));
	}
	/* Then none completes with one held: dead at 10 s (the peer spoke). */
	for (uint32_t t = 60100; t <= 70100; t += 100) {
		mg_txp_acl(&a, t, 1, done, &last);
	}
	CHECK(mg_txp_dead(&a, 70100, true, 70100, 4000));
	/* Nothing held: idle. */
	mg_txp_acl(&a, 70200, 0, done, &last);
	CHECK(!mg_txp_dead(&a, 900000, true, 0, 4000));
}

/* The update gate's carry-on: an allowed transfer finishes on its connection. */
static void test_smp_carry(void)
{
	uint8_t sha[32], other[32];

	printf("  an allowed transfer finishes on its connection after the proof lapses\n");
	for (int i = 0; i < 32; i++) {
		sha[i] = (uint8_t)i;
		other[i] = (uint8_t)i;
	}
	other[31] ^= 1;
	/* Its chunks on its connection, staged or not. */
	CHECK(mg_ota_smp_carries(MG_SMP_GROUP_IMAGE, MG_SMP_IMG_UPLOAD, MG_SMP_OP_WRITE, true, false));
	CHECK(mg_ota_smp_carries(MG_SMP_GROUP_IMAGE, MG_SMP_IMG_UPLOAD, MG_SMP_OP_WRITE, true, true));
	/* The state write and the reset only once the last chunk is in. */
	CHECK(!mg_ota_smp_carries(MG_SMP_GROUP_IMAGE, MG_SMP_IMG_STATE, MG_SMP_OP_WRITE, true, false));
	CHECK(mg_ota_smp_carries(MG_SMP_GROUP_IMAGE, MG_SMP_IMG_STATE, MG_SMP_OP_WRITE, true, true));
	CHECK(!mg_ota_smp_carries(MG_SMP_GROUP_OS, MG_SMP_OS_RESET, MG_SMP_OP_WRITE, true, false));
	CHECK(mg_ota_smp_carries(MG_SMP_GROUP_OS, MG_SMP_OS_RESET, MG_SMP_OP_WRITE, true, true));
	/* Never an erase; never on another connection. */
	CHECK(!mg_ota_smp_carries(MG_SMP_GROUP_IMAGE, 5, MG_SMP_OP_WRITE, true, true));
	CHECK(!mg_ota_smp_carries(MG_SMP_GROUP_IMAGE, MG_SMP_IMG_UPLOAD, MG_SMP_OP_WRITE, false, true));
	CHECK(!mg_ota_smp_carries(MG_SMP_GROUP_IMAGE, MG_SMP_IMG_STATE, MG_SMP_OP_WRITE, false, true));
	CHECK(!mg_ota_smp_carries(MG_SMP_GROUP_OS, MG_SMP_OS_RESET, MG_SMP_OP_WRITE, false, true));
	/* Reads aren't asked anyway: nothing to carry. */
	CHECK(!mg_ota_smp_carries(MG_SMP_GROUP_IMAGE, MG_SMP_IMG_STATE, 0, true, true));
	/* The same transfer: its size and sha; a retry from 0 included. */
	CHECK(mg_ota_smp_same_transfer(1000, sha, 32, 1000, sha, 32));
	CHECK(!mg_ota_smp_same_transfer(1000, sha, 32, 1001, sha, 32));
	CHECK(!mg_ota_smp_same_transfer(1000, sha, 32, 1000, other, 32));
	CHECK(!mg_ota_smp_same_transfer(1000, sha, 32, 1000, sha, 0));
	CHECK(!mg_ota_smp_same_transfer(1000, sha, 0, 1000, sha, 0)); /* no sha: nothing to tie it to */
}

/* An SMP request's source is its first fragment's connection. */
static void test_smp_source(void)
{
	printf("  a partly received SMP request from a connection that's gone is dropped\n");
	/* A partial request from connection 3 (it never subscribed, so nothing
	 * else drops it), then connection 5 writes: dropped before 5's bytes are
	 * collected, so 5 starts its own request. */
	CHECK(mg_smp_rx_orphan(40, 3, 5));
	CHECK(mg_smp_rx_orphan(0, 3, 5));
	CHECK(!mg_smp_rx_orphan(40, 5, 5)); /* the same connection goes on */
	CHECK(!mg_smp_rx_orphan(-1, 3, 5)); /* nothing partly received */
	CHECK(mg_smp_rx_orphan(40, 3, 0));  /* no link at all: dropped too */
}

/* Plugging in turns it on; a power off on USB stays off until a key
 * or an unplug and replug; a low cell charges dark on USB, stays off without. */
static void test_plug_in(void)
{
	const int R = 3500;

	printf("  plugging in turns it on\n");
	/* A plug after a power off on battery, a healthy cell: runs. */
	CHECK(mg_power_boot_decision(true, MG_OFF_PLAIN, 1, 3900, R) == MG_BOOT_RUN);
	/* A key press after a power off on battery: runs. */
	CHECK(mg_power_boot_decision(true, MG_OFF_PLAIN, 0, 3900, R) == MG_BOOT_RUN);
	/* Off on USB, then the unplug: back off (only to arm the plug). */
	CHECK(mg_power_boot_decision(true, MG_OFF_ON_USB, 0, 3900, R) == MG_BOOT_OFF);
	/* Off on USB, a key press while still plugged in: runs. */
	CHECK(mg_power_boot_decision(true, MG_OFF_ON_USB, 1, 3900, R) == MG_BOOT_RUN);
	/* Off on USB, then a reset that isn't a wake (a power cycle): runs. */
	CHECK(mg_power_boot_decision(false, MG_OFF_ON_USB, 0, 3900, R) == MG_BOOT_RUN);
	/* A low cell: dark on USB, off without it, whatever woke it. */
	CHECK(mg_power_boot_decision(true, MG_OFF_PLAIN, 1, 3350, R) == MG_BOOT_CHARGE_DARK);
	CHECK(mg_power_boot_decision(false, MG_OFF_NONE, 1, 3350, R) == MG_BOOT_CHARGE_DARK);
	CHECK(mg_power_boot_decision(true, MG_OFF_PLAIN, 0, 3350, R) == MG_BOOT_OFF);
	CHECK(mg_power_boot_decision(false, MG_OFF_NONE, 0, 3350, R) == MG_BOOT_OFF);
	/* At the resume voltage it starts. */
	CHECK(mg_power_boot_decision(false, MG_OFF_NONE, 1, R, R) == MG_BOOT_RUN);
	/* No cell (USB only, or a dead one): never held. */
	CHECK(mg_power_boot_decision(false, MG_OFF_NONE, 1, 1200, R) == MG_BOOT_RUN);
	CHECK(mg_power_boot_decision(false, MG_OFF_NONE, 0, -19, R) == MG_BOOT_RUN);
	/* A board that can't tell USB: a low cell never holds the boot. */
	CHECK(mg_power_boot_decision(true, MG_OFF_PLAIN, -134, 3350, R) == MG_BOOT_RUN);
	CHECK(mg_power_boot_decision(true, MG_OFF_ON_USB, -134, 3900, R) == MG_BOOT_RUN);
}

/* In a release build the Nordic UART's state-changing words run
 * only on a token-proven connection; reads, and automation builds, stay
 * open. */
/* A client's image state write: only a confirm of the running image before
 * healthy is held; tests, permanent marks and anything after healthy pass. In
 * SAFE mode (either kind) a confirm of the running image is refused. */
static void test_set_next(void)
{
	printf("  image state writes: a confirm before healthy held, refused in SAFE\n");
	CHECK(mg_ota_set_next(false, true, true, false) == MG_OTA_SET_NEXT_LATCH);
	CHECK(mg_ota_set_next(false, false, false, false) == MG_OTA_SET_NEXT_PASS);
	CHECK(mg_ota_set_next(false, false, true, false) == MG_OTA_SET_NEXT_PASS);
	CHECK(mg_ota_set_next(false, true, false, false) == MG_OTA_SET_NEXT_PASS);
	CHECK(mg_ota_set_next(true, true, true, false) == MG_OTA_SET_NEXT_PASS);
	CHECK(mg_ota_set_next(true, false, false, false) == MG_OTA_SET_NEXT_PASS);
	CHECK(mg_ota_set_next(true, true, true, true) == MG_OTA_SET_NEXT_REFUSE);
	CHECK(mg_ota_set_next(false, true, true, true) == MG_OTA_SET_NEXT_REFUSE);
	CHECK(mg_ota_set_next(true, false, true, true) == MG_OTA_SET_NEXT_PASS);
	CHECK(mg_ota_set_next(true, false, false, true) == MG_OTA_SET_NEXT_PASS);
}

/* Slot1 shared with the clips: an upload holds it from its first chunk until
 * a reset or the client's erase, so the image isn't wiped between its last
 * chunk and the state write that stages it, nor while a stopped upload may
 * resume. mg_ota.c's slot_needed() with no swap pending, running confirmed. */
static void test_ota_slot_held(void)
{
	bool held = false;

	printf("  slot1: an upload holds it until staged, erased or a reset\n");
	held = mg_ota_slot_held(held, MG_OTA_SLOT_STARTED);
	CHECK(held);
	held = mg_ota_slot_held(held, MG_OTA_SLOT_PENDING); /* not staged yet */
	CHECK(held);
	held = mg_ota_slot_held(held, MG_OTA_SLOT_ERASED);
	CHECK(!held);
	held = mg_ota_slot_held(held, MG_OTA_SLOT_STARTED);
	held = mg_ota_slot_held(held, MG_OTA_SLOT_STOPPED); /* it may resume */
	CHECK(held);
	held = mg_ota_slot_held(held, MG_OTA_SLOT_STARTED); /* resumed from 0 */
	CHECK(held);
	CHECK(!mg_ota_slot_held(false, MG_OTA_SLOT_PENDING));
	CHECK(!mg_ota_slot_held(false, MG_OTA_SLOT_STOPPED));
}

/* A stall the supervisor marked (starving) that recovers before the watchdog
 * fires ends the mark, so threads count as alive again and the boot can be
 * marked healthy; while a thread is still stale the mark stays. */
static void test_starving_recovers(void)
{
	printf("  watchdog: a recovered stall clears the starving mark\n");
	CHECK(!mg_health_starving_after(true, false));
	CHECK(mg_health_starving_after(true, true));
	CHECK(!mg_health_starving_after(false, false));
}

/* A real press counts after the prompt only if it began after it (its
 * callback time less the debounce). */
static void test_press_bound(void)
{
	printf("  press bound: a press that began before the prompt doesn't count\n");
	CHECK(!mg_input_after_arm(false, true, true, 2000, 1000, 30)); /* queued before */
	CHECK(!mg_input_after_arm(true, true, true, 1010, 1000, 30));  /* edge before the prompt */
	CHECK(mg_input_after_arm(true, true, true, 1030, 1000, 30));   /* began at the prompt */
	CHECK(mg_input_after_arm(true, true, false, 1005, 1000, 30));  /* a release: by order */
	CHECK(mg_input_after_arm(true, false, true, 1001, 1000, 30));  /* injected: by order */
	CHECK(mg_input_after_arm(true, true, true, 5, 0xfffffff0u, 20)); /* across the wrap */
}

/* A central's traffic never holds the app work queue long enough to starve
 * the watchdog into a crash. */
static void test_evt_policy(void)
{
	printf("  peer traffic: a capped event run, a reply budget, saturation\n");
	CHECK(mg_evt_run_more(0, 0) && mg_evt_run_more(3, 49));
	CHECK(!mg_evt_run_more(4, 0) && !mg_evt_run_more(1, 50));
	/* One event's replies share 1 s, each at most 500 ms. */
	CHECK(mg_reply_wait_ms(1000, 0) == 500); /* no event's budget: as before */
	CHECK(mg_reply_wait_ms(1000, 2000) == 500);
	CHECK(mg_reply_wait_ms(1800, 2000) == 200);
	CHECK(mg_reply_wait_ms(2000, 2000) == 0 && mg_reply_wait_ms(2500, 2000) == 0);
	/* The 7 notifications of one status request on a link that takes one a
	 * second wait at most the budget together. */
	int64_t t = 0, dl = MG_REPLY_BUDGET_MS, waited = 0;

	for (int i = 0; i < 7; i++) {
		int32_t w = mg_reply_wait_ms(t, dl);

		waited += w;
		t += w;
	}
	CHECK(waited <= MG_REPLY_BUDGET_MS);
	/* request_status's 4 a second runs through mg_core in the unit suite
	 * (test_core test_status_rate). */
	/* Backed up 4 s with the handler still taking events: the peer's. */
	CHECK(mg_evt_saturated(true, 1000, 4800, 5000, 8000));
	CHECK(!mg_evt_saturated(true, 1000, 4800, 4999, 8000));
	CHECK(!mg_evt_saturated(false, 1000, 8800, 9000, 8000)); /* no link: a crash */
	CHECK(!mg_evt_saturated(true, 0, 8800, 9000, 8000));     /* drained */
	/* The queue stuck elsewhere, one event waiting, the handler not running
	 * (it last took one 4 s ago, or never): a crash, not a peer loss. */
	CHECK(!mg_evt_saturated(true, 1000, 1000, 5000, 8000));
	CHECK(!mg_evt_saturated(true, 1000, 0, 9000, 8000));
}

/* Update writes: without the proof (on a release build, set up or not, and
 * in the gadget's own SAFE) they need one confirm press,
 * which admits the connection's next transfer (the window); the button's
 * SAFE stays open. What admits it: one press during the update prompt on
 * that connection, or the wired console. */
static void test_update_window(void)
{
	static const struct {
		uint16_t g;
		uint8_t id;
	} w[] = {{MG_SMP_GROUP_IMAGE, 1}, {MG_SMP_GROUP_OS, MG_SMP_OS_RESET}};

	printf("  update writes: which need the press\n");
	for (size_t i = 0; i < ARRAY_SIZE(w); i++) {
		uint16_t g = w[i].g;
		uint8_t id = w[i].id;

		/* automation build: open with no press, set up or not set up */
		CHECK(mg_ota_smp_allowed(g, id, MG_SMP_OP_WRITE, false, false, false, false,
					 false));
		CHECK(mg_ota_smp_allowed(g, id, MG_SMP_OP_WRITE, false, true, false, false,
					 false)); /* its own SAFE too */
		/* release build, set up or not (no device token, or after Forget,
		 * included): the proof, the arm or the button's SAFE (else the
		 * prompt) */
		CHECK(mg_ota_smp_allowed(g, id, MG_SMP_OP_WRITE, false, true, true, true,
					 false));
		CHECK(!mg_ota_smp_allowed(g, id, MG_SMP_OP_WRITE, false, false, false, true,
					  false));
		CHECK(mg_ota_smp_allowed(g, id, MG_SMP_OP_WRITE, true, false, false, true,
					 false));
		CHECK(mg_ota_smp_allowed(g, id, MG_SMP_OP_WRITE, false, false, false, true,
					 true));
		CHECK(mg_ota_smp_allowed(g, id, MG_SMP_OP_WRITE, false, true, false, false,
					 false)); /* automation: its own SAFE too */
		CHECK(!mg_ota_smp_allowed(g, id, MG_SMP_OP_WRITE, false, true, false, true,
					  false)); /* release: its own SAFE asks for the press */
		CHECK(mg_ota_smp_allowed(g, id, MG_SMP_OP_WRITE, true, true, false, true,
					 false)); /* ... unless proven */
		CHECK(mg_ota_smp_allowed(g, id, 0, false, false, false, true, false)); /* a read */
	}
	CHECK(mg_ota_smp_allowed(MG_SMP_GROUP_OS, 0, MG_SMP_OP_WRITE, false, false, false,
				 true, false)); /* echo */
	/* What admits a transfer: ONE press during the prompt (30 s) on the
	 * prompt's connection; real on a release, injected where the build takes
	 * it; no prompt, a late press or another connection's never.
	 * prompt_press(prompt_gen, until, conn_gen, now, real, injected_counts) */
	CHECK(mg_ota_prompt_press(7, 31000, 7, 1000, true, false));   /* a real press */
	CHECK(mg_ota_prompt_press(7, 31000, 7, 30999, true, false));
	CHECK(!mg_ota_prompt_press(7, 31000, 7, 31000, true, false)); /* after 30 s */
	CHECK(!mg_ota_prompt_press(7, 31000, 8, 1000, true, false));  /* another connection */
	CHECK(!mg_ota_prompt_press(0, 31000, 0, 1000, true, false));  /* no prompt */
	CHECK(!mg_ota_prompt_press(7, 31000, 7, 1000, false, false)); /* injected, release */
	CHECK(mg_ota_prompt_press(7, 31000, 7, 1000, false, true));   /* injected, automation */
	/* Then the write is allowed, set up and in the gadget's own SAFE too; a
	 * proven one needs no press. */
	CHECK(mg_ota_smp_allowed(MG_SMP_GROUP_IMAGE, 1, MG_SMP_OP_WRITE, false, true, false,
				 true, true));
	CHECK(mg_ota_smp_allowed(MG_SMP_GROUP_IMAGE, 1, MG_SMP_OP_WRITE, true, false, false,
				 true, false));
	/* The window: its connection only, MG_UPDATE_WINDOW_MS. */
	CHECK(mg_ota_window_open_now(7, 61000, 7, 60999));
	CHECK(!mg_ota_window_open_now(7, 61000, 7, 61000));
	CHECK(!mg_ota_window_open_now(7, 61000, 8, 2000));
	CHECK(!mg_ota_window_open_now(0, 61000, 0, 2000));
}

/* With CONFIG_MG_BENCH_MODE on, every human gate takes the script, on a set-up
 * and a not-set-up unit, with MG_AUTOMATION on and off (a release-configured
 * bench image); without it a release build keeps every gate (the release
 * profile refuses injected input). */
static void test_bench_mode_flows(void)
{
	printf("  bench mode drives every flow with no human; a release build refuses it\n");
	/* The helpers' rows, bench mode on vs a release build without it (the
	 * wiring itself is mg_core.c's, mg_nus.c's and mg_ota.c's). */
	CHECK(mg_mic_press_ok(true, true, 1000, 2000)); /* an injected press stamps the window */
	/* A release gadget that isn't set up refuses start_mic (no token to
	 * prove); set up, or any bench, automation or test build, goes on. */
	CHECK(mg_mic_start_needs_setup(true, false));
	CHECK(!mg_mic_start_needs_setup(true, true));
	CHECK(!mg_mic_start_needs_setup(false, false));
	/* Setup mode on a talk-key-only board: a boot hold released before the
	 * SAFE cue begins it on a normal boot; a normal boot without the hold
	 * and SAFE (the button's or the gadget's own, which runs no Link setup)
	 * never do. It lasts 5 min from that boot (or until the confirmed
	 * re-setup clears it); test and bench builds are always in it. */
	CHECK(mg_setup_mode_begins(false, true));
	CHECK(!mg_setup_mode_begins(false, false));
	CHECK(!mg_setup_mode_begins(true, true));
	CHECK(!mg_setup_mode_begins(true, false));
	CHECK(mg_setup_mode_now(false, true, 1000 + 300000, 1000 + 300000 - 1));
	CHECK(!mg_setup_mode_now(false, true, 1000 + 300000, 1000 + 300000));
	CHECK(!mg_setup_mode_now(false, false, 1000 + 300000, 2000));
	CHECK(mg_setup_mode_now(true, false, 0, 2000));
	CHECK(mg_bench_ble_allowed(false, false, false, true));
	CHECK(!mg_bench_ble_allowed(false, false, false, false));
	CHECK(!mg_nus_word_locked("bench 1", false, false, true));
	CHECK(mg_nus_word_locked("bench 1", false, false, false));
	CHECK(!mg_nus_word_locked("reach test", false, false, true));
	CHECK(mg_bench_switch_allowed(false, true, false, true, false));
	CHECK(!mg_bench_switch_allowed(false, true, false, false, true));
	/* mg-session-v1.json's re-setup prompt exchanges, as decisions:
	 * hello(exempt, set_up, prompt_conn, conn, dismissed_conn, dismissed_ms,
	 * now). "the re-setup prompt's rate": hello 1 shows; hello 2 on the same
	 * connection 4 s on takes it over; after the press (no prompt up) hello 3
	 * shows; dismissed, a hello on that connection 4 s later is refused, 10 s
	 * after the dismiss it shows; another connection shows at once. */
	CHECK(mg_resetup_hello(false, true, 0, 1, 0, 0, 1000) == MG_RESETUP_SHOW);
	CHECK(mg_resetup_hello(false, true, 1, 1, 0, 0, 5000) == MG_RESETUP_TAKEOVER);
	CHECK(mg_resetup_hello(false, true, 0, 1, 0, 0, 6000) == MG_RESETUP_SHOW);
	CHECK(mg_resetup_hello(false, true, 0, 1, 1, 7000, 11000) == MG_RESETUP_BUSY);
	CHECK(mg_resetup_hello(false, true, 0, 1, 1, 7000, 17000) == MG_RESETUP_SHOW);
	CHECK(mg_resetup_hello(false, true, 0, 2, 1, 7000, 7001) == MG_RESETUP_SHOW);
	/* "across two connections": connection 2's hello while connection 1's
	 * prompt is up is refused; with no prompt up it shows. */
	CHECK(mg_resetup_hello(false, true, 1, 2, 0, 0, 3000) == MG_RESETUP_BUSY);
	CHECK(mg_resetup_hello(false, true, 0, 2, 0, 0, 4000) == MG_RESETUP_SHOW);
	/* "a takeover hello that fails validation ends the prompt": hello 2 on
	 * the prompt's connection takes it over and fails validation (a bad
	 * mobile_pub), which ends the prompt (mg_setup.c drops prompt_conn, its
	 * error goes out; host test_resetup_rate_flow plays it); no dismiss came,
	 * so the next valid hello shows afresh, and from another connection too. */
	CHECK(mg_resetup_hello(false, true, 0, 1, 0, 0, 1000) == MG_RESETUP_SHOW);
	CHECK(mg_resetup_hello(false, true, 1, 1, 0, 0, 2000) == MG_RESETUP_TAKEOVER);
	CHECK(mg_resetup_hello(false, true, 0, 1, 0, 0, 2500) == MG_RESETUP_SHOW);
	CHECK(mg_resetup_hello(false, true, 0, 2, 0, 0, 2500) == MG_RESETUP_SHOW);
	{
		/* "an update press opens no window" (a set-up release build, an
		 * unproven link): the owner's press confirms the prompt's own
		 * upload (len 64, its SHA-256) and nothing else. Another image's
		 * upload asks again (10, no new prompt within 10 s), an injected
		 * tap confirms nothing, and the link's re-setup hello still shows
		 * a prompt of its own, which no injected tap answers. */
		struct mg_upd u = {0};
		uint8_t a[32], b[32];
		bool started;
		bool inj = !mg_ota_release_build(false, false); /* injected input counts: never here */
		const struct mg_upd_req up_a = {
			.kind = MG_UPD_UPLOAD0, .len = 64, .sha = a, .end = 32};
		const struct mg_upd_req up_b = {
			.kind = MG_UPD_UPLOAD0, .len = 96, .sha = b, .end = 32};

		memset(a, 0xa1, sizeof(a));
		memset(b, 0xb2, sizeof(b));
		CHECK(mg_upd_write(&u, 1, 1000, &up_a, &started) == MG_UPD_BUSY && started);
		CHECK(mg_upd_prompting(&u, 1, 1000));
		CHECK(mg_upd_press(&u, 1, 2000, true, inj));
		CHECK(mg_upd_write(&u, 1, 3000, &up_a, &started) == MG_UPD_OK);
		CHECK(mg_upd_write(&u, 1, 4000, &up_b, &started) == MG_UPD_BUSY && !started);
		CHECK(!mg_upd_prompting(&u, 1, 4000));
		CHECK(!inj && !mg_upd_press(&u, 1, 5000, false, inj));
		CHECK(!mg_upd_prompting(&u, 1, 5000));
		CHECK(mg_resetup_hello(false, true, 0, 1, 0, 0, 6000) == MG_RESETUP_SHOW);
		CHECK(!mg_upd_press(&u, 1, 7000, false, inj) && !mg_upd_prompting(&u, 1, 7000));
		CHECK(mg_upd_write(&u, 1, 7500, &up_b, &started) == MG_UPD_BUSY && !started);
	}
	/* "on an automation build": takeover the same; a hello right after a
	 * dismiss shows (exempt from the dismiss rule only). */
	CHECK(mg_resetup_hello(true, true, 1, 1, 0, 0, 2000) == MG_RESETUP_TAKEOVER);
	CHECK(mg_resetup_hello(true, true, 0, 1, 1, 2000, 2001) == MG_RESETUP_SHOW);
	CHECK(mg_resetup_hello(true, true, 1, 2, 0, 0, 2000) == MG_RESETUP_BUSY);
	/* A gadget that isn't set up has no rate. */
	CHECK(mg_resetup_hello(false, false, 0, 1, 1, 7000, 8000) == MG_RESETUP_SHOW);
	/* The update gate's build rule (the expression mg_ota.c uses):
	 * automation off with bench mode on takes an unproven upload on a set-up
	 * gadget with no press; only a build with neither prompts. */
	CHECK(!mg_ota_release_build(false, true));
	CHECK(!mg_ota_release_build(true, false));
	CHECK(mg_ota_release_build(false, false));
	CHECK(mg_ota_smp_allowed(MG_SMP_GROUP_IMAGE, MG_SMP_IMG_UPLOAD, MG_SMP_OP_WRITE, false,
				 false, false, mg_ota_release_build(false, true), false));
	CHECK(!mg_ota_smp_allowed(MG_SMP_GROUP_IMAGE, MG_SMP_IMG_UPLOAD, MG_SMP_OP_WRITE,
				  false, false, false, mg_ota_release_build(false, false), false));
	/* The bench requests for the next boot (mg_boot.c): kept across a reset
	 * in a sealed record (an unsealed one reads as power-on and drops the
	 * request), taken once at the next boot, and only by a bench-mode image;
	 * a stale one is dropped. */
	{
		struct mg_boot_rec r = {0};
		struct mg_boot_crash cr;
		bool hc;

		(void)mg_boot_start(&r, 3, &cr, &hc); /* a boot */
		mg_boot_peer_restart(&r);           /* a peer restart's bits, kept below */
		uint8_t others = r.flags & ~MG_BOOT_F_BENCH_MASK;

		mg_boot_bench_set(&r, MG_BOOT_F_BENCH_SAFE);
		CHECK(mg_boot_valid(&r)); /* sealed: survives the warm reboot */
		CHECK((r.flags & ~MG_BOOT_F_BENCH_MASK) == others);
		(void)mg_boot_start(&r, 3, &cr, &hc); /* the next boot */
		CHECK(mg_boot_valid(&r));
		CHECK(mg_boot_bench_take(&r, true) == MG_BOOT_F_BENCH_SAFE);
		CHECK(mg_boot_valid(&r));
		CHECK(r.boots == 2); /* the boot count kept: the record wasn't reset */
		CHECK(mg_boot_bench_take(&r, true) == 0); /* once */
		mg_boot_bench_set(&r, MG_BOOT_F_BENCH_RESET);
		(void)mg_boot_start(&r, 3, &cr, &hc);
		CHECK(mg_boot_bench_take(&r, true) == MG_BOOT_F_BENCH_RESET);
		mg_boot_bench_set(&r, MG_BOOT_F_BENCH_RESET);
		(void)mg_boot_start(&r, 3, &cr, &hc);
		CHECK(mg_boot_bench_take(&r, false) == 0); /* no bench mode: dropped */
		CHECK((r.flags & MG_BOOT_F_BENCH_MASK) == 0);
		CHECK(mg_boot_valid(&r));
	}
	/* A bench SAFE is the button's SAFE (updates open), declared as bench. */
	mg_health_set_safe_bench();
	CHECK(mg_health_safe() && mg_health_safe_by_button() && mg_health_safe_bench());
	mg_health_set_mode(MG_BOOT_NORMAL);
	CHECK(!mg_health_safe_bench() && !mg_health_safe_by_button());
}

static void test_safe_idle(void)
{
	printf("  SAFE: a central holding the one slot and writing nothing is dropped after 60 s\n");
	/* A silent link in SAFE: kept until 60 s after its last write (or its start), then dropped. */
	CHECK(!mg_safe_idle_drop(true, true, 159999, 100000, false));
	CHECK(mg_safe_idle_drop(true, true, 160000, 100000, false));
	/* Any write starts the minute over. */
	CHECK(!mg_safe_idle_drop(true, true, 160000, 150000, false));
	/* Never while SMP handles a request or holds an upload's writer (an upload or erase). */
	CHECK(!mg_safe_idle_drop(true, true, 900000, 100000, true));
	/* Only in SAFE, only with a link. */
	CHECK(!mg_safe_idle_drop(false, true, 900000, 100000, false));
	CHECK(!mg_safe_idle_drop(true, false, 900000, 100000, false));
	/* Across the 32-bit uptime wrap. */
	CHECK(!mg_safe_idle_drop(true, true, 1000u, 0xfffff000u, false));  /* 5.1 s */
	CHECK(mg_safe_idle_drop(true, true, 60000u, 0xfffff000u, false));  /* 64.1 s */
}

static void test_bench_switch(void)
{
	printf("  bench switch: never on a set-up gadget in a release build\n");
	/* switch(automation, set_up, wired, bench_mode, proven) */
	CHECK(!mg_bench_switch_allowed(false, true, false, false, false)); /* release, set up, wireless */
	CHECK(!mg_bench_switch_allowed(false, true, false, false, true));  /* ... even proven */
	CHECK(mg_bench_switch_allowed(false, true, true, false, false));   /* the wired USB console */
	CHECK(mg_bench_switch_allowed(false, false, false, false, false)); /* release, not set up */
	/* An automation build, set up, wireless: proven only. */
	CHECK(!mg_bench_switch_allowed(true, true, false, false, false));
	CHECK(mg_bench_switch_allowed(true, true, false, false, true));
	CHECK(mg_bench_switch_allowed(true, true, false, true, false)); /* bench mode: open */
	printf("  bench switch over BLE: the proof, never a press, every build\n");
	/* ble(wired, sim, proven, bench_mode) */
	CHECK(!mg_bench_ble_allowed(false, false, false, false)); /* unproven (a press changes nothing) */
	CHECK(mg_bench_ble_allowed(false, false, true, false));   /* proven */
	CHECK(mg_bench_ble_allowed(false, false, false, true));   /* bench mode, unproven */
	CHECK(mg_bench_ble_allowed(true, false, false, false));   /* wired console */
	CHECK(mg_bench_ble_allowed(false, true, false, false));   /* simulations */
	/* The log mirror (an input timeline): set up -> proven only, automation
	 * builds included; bench mode open. log(bench_mode, set_up, proven) */
	CHECK(!mg_nus_log_allowed(false, true, false));
	CHECK(mg_nus_log_allowed(false, true, true));
	CHECK(mg_nus_log_allowed(false, false, false)); /* not set up: no owner yet */
	CHECK(mg_nus_log_allowed(true, true, false));   /* bench mode */
	/* Reachability and the update prompt: an unproven link on a set-up gadget
	 * whose update path is live (a chunk it took within 10 s, or a press
	 * admitting one; never the prompt alone, below) counts as reachable, so
	 * a press-admitted upload longer than
	 * 30 s isn't cut by "peer: link without ATT 30 s"; the dead-link run and
	 * the image's keep still take only the gate (exchange_counts). */
	CHECK(!mg_reach_exchange_counts(true, true, false));
	CHECK(!mg_reach_link_counts(false, false));
	CHECK(mg_reach_link_counts(false, true));
	CHECK(mg_reach_link_counts(true, false));
	{
		struct mg_reach rs, ctl;
		int64_t t0 = 1000;
		bool lost = false;

		mg_reach_init(&rs, t0);
		mg_reach_init(&ctl, t0);
		/* 40 s on a link that never passes the gate, an upload moving: no
		 * loss at any second; the same link with no update path is one. */
		for (int64_t t = t0; t <= t0 + 40000; t += 1000) {
			CHECK(mg_reach_step_link(&rs, t, mg_reach_link_counts(false, true), false,
						 true) == MG_REACH_NONE);
			lost |= mg_reach_step_link(&ctl, t, mg_reach_link_counts(false, false), false,
						   true) != MG_REACH_NONE;
		}
		CHECK(lost);
	}
	/* The update prompt alone holds nothing: a central that never proves
	 * anything sends an unproven update write once a second for 2 min,
	 * re-arming the prompt every time it ends (a release build, set up).
	 * Its link must still be dropped as a peer's (owner locked out
	 * otherwise); a press admitting an update on it, or a chunk moving,
	 * keeps it. */
	{
		struct mg_upd u = {0};
		struct mg_reach rs;
		int64_t t0 = 1000;
		bool started, lost = false, prompted = false;

		mg_reach_init(&rs, t0);
		CHECK(mg_reach_step_link(&rs, t0, true, false, false) == MG_REACH_NONE); /* advertising */
		for (int64_t t = t0 + 1000; t <= t0 + 120000 && !lost; t += 1000) {
			const struct mg_upd_req rr = UV_CHANGE(uv_reset);

			(void)mg_upd_write(&u, 1, t, &rr, &started);
			prompted |= started;
			bool live = mg_reach_update_live(false, mg_upd_prompting(&u, 1, t),
							 mg_upd_admitted_live(&u, 1, t));
			lost = mg_reach_step_link(&rs, t, mg_reach_link_counts(false, live), false,
						  true) != MG_REACH_NONE;
		}
		CHECK(prompted);
		CHECK(lost);
		CHECK(!mg_reach_update_live(false, true, false));
		CHECK(mg_reach_update_live(true, false, false)); /* a chunk moved */
		CHECK(mg_reach_update_live(false, true, true));  /* the press admitted it */
		/* The press on the prompt: admitted, the link counts until the
		 * upload's chunks take over. */
		memset(&u, 0, sizeof(u));
		const struct mg_upd_req rr2 = UV_CHANGE(uv_reset);

		(void)mg_upd_write(&u, 2, 5000, &rr2, &started);
		CHECK(started && mg_upd_press(&u, 2, 9000, true, false));
		CHECK(mg_reach_update_live(false, mg_upd_prompting(&u, 2, 9500),
					   mg_upd_admitted_live(&u, 2, 9500)));
	}
	printf("  reachability: an unproven update prompt alone doesn't hold the link\n");
	/* Nor does a press past its window: an erase the press covered (or the
	 * upload it bound) counts for the link only within MG_UPDATE_CONFIRM_MS
	 * of the press; an idle unproven link after that is a peer's again and
	 * is dropped, though the change stays covered for the status. */
	{
		const struct mg_upd_req er = UV_CHANGE(uv_erase);
		const uint8_t sha[32] = {7};
		const struct mg_upd_req up = {.kind = MG_UPD_UPLOAD0, .len = 4096, .sha = sha};
		int64_t press = 6000, end = press + MG_UPDATE_CONFIRM_MS;
		bool started, lost = false;
		struct mg_upd u = {0};
		struct mg_reach rs;

		(void)mg_upd_write(&u, 3, 5000, &er, &started);
		CHECK(started && mg_upd_press(&u, 3, press, true, false));
		CHECK(mg_upd_write(&u, 3, press + 1000, &er, &started) == MG_UPD_OK); /* covered */
		CHECK(mg_upd_admitted_live(&u, 3, end - 1));
		CHECK(!mg_upd_admitted_live(&u, 3, end));
		CHECK(mg_upd_allowed(&u, 3)); /* the Nordic UART's status: still admitted */
		mg_reach_init(&rs, end - 1000);
		CHECK(mg_reach_step_link(&rs, end - 1000, true, false, false) == MG_REACH_NONE);
		for (int64_t t = end; t <= end + 40000 && !lost; t += 1000) {
			bool live = mg_reach_update_live(false, mg_upd_prompting(&u, 3, t),
							 mg_upd_admitted_live(&u, 3, t));

			lost = mg_reach_step_link(&rs, t, mg_reach_link_counts(false, live), false,
						  true) != MG_REACH_NONE;
		}
		CHECK(lost);
		/* The upload a press bound, its link idle: the same window. */
		memset(&u, 0, sizeof(u));
		(void)mg_upd_write(&u, 4, 5000, &up, &started);
		CHECK(started && mg_upd_press(&u, 4, press, true, false));
		CHECK(mg_upd_admitted_live(&u, 4, end - 1));
		CHECK(!mg_upd_admitted_live(&u, 4, end));
	}
	printf("  reachability: a press's admission holds the link only within its window\n");
	/* Owner content on a set-up release build, proven only; test builds
	 * and a gadget that isn't set up unchanged.
	 * owner_read(release, set_up, proven) */
	CHECK(!mg_owner_read_allowed(true, true, false));
	CHECK(mg_owner_read_allowed(true, true, true));
	CHECK(mg_owner_read_allowed(true, false, false));
	CHECK(mg_owner_read_allowed(false, true, false));
	/* The window: this connection only, 5 s, a press with a link. */
	CHECK(mg_press_window_open(1000, 7, 6000, 7));
	CHECK(!mg_press_window_open(1000, 7, 6001, 7));
	CHECK(!mg_press_window_open(1000, 7, 2000, 8)); /* another connection */
	CHECK(!mg_press_window_open(1000, 0, 2000, 0)); /* a press with no link */
	CHECK(!mg_press_window_open(0, 7, 2000, 7));    /* no press yet */
}

static void test_nus_policy(void)
{
	static const char *const change[] = {"bench 1", "bench 0", "listen 1", "listen 0", "mode 0",
					     "mode 1", "mode 2", "reach test", "tx dead test",
					     "att dead test", "update window"};
	static const char *const reads[] = {"status", "version", "settings", "queue",
					    "battery", "reset", "pins"};
	/* Default-deny: unknown lines, prefixes and near misses are locked
	 * unproven in a release build. */
	static const char *const unknown[] = {"bogus", "", "bench", "bench 2", "bench 1 ",
					      "mode 3", "listen", "STATUS", "status x", "reach"};

	printf("  console words over Bluetooth: release locks changes until the proof\n");
	for (size_t i = 0; i < sizeof(change) / sizeof(change[0]); i++) {
		CHECK(mg_nus_word_locked(change[i], false, false, false)); /* release, unproven */
		CHECK(!mg_nus_word_locked(change[i], false, true, false)); /* release, proven */
		CHECK(!mg_nus_word_locked(change[i], true, false, false)); /* automation */
		CHECK(!mg_nus_word_locked(change[i], true, true, false));
	}
	for (size_t i = 0; i < sizeof(reads) / sizeof(reads[0]); i++) {
		CHECK(!mg_nus_word_locked(reads[i], false, false, false));
		CHECK(!mg_nus_word_locked(reads[i], true, false, false));
	}
	for (size_t i = 0; i < sizeof(unknown) / sizeof(unknown[0]); i++) {
		CHECK(mg_nus_word_locked(unknown[i], false, false, false)); /* release, unproven */
		CHECK(!mg_nus_word_locked(unknown[i], true, false, false)); /* automation: the help line */
	}
	/* Every table word is classified, and every change word is in the list
	 * above (a new state-changing word needs a row here too). */
	for (size_t i = 0; i < sizeof(mg_nus_words) / sizeof(mg_nus_words[0]); i++) {
		bool listed = false;

		CHECK(mg_nus_words[i].cls != MG_NUS_UNKNOWN);
		for (size_t j = 0; j < sizeof(change) / sizeof(change[0]); j++) {
			listed |= strcmp(change[j], mg_nus_words[i].word) == 0;
		}
		CHECK(listed == (mg_nus_words[i].cls != MG_NUS_READ));
	}
	/* The scripted human steps (talk down, talk up, pair confirm: the
	 * button and the setup confirm over Bluetooth) are in a bench-mode image
	 * only. A release build doesn't know them, and locks them on an unproven
	 * connection like any unknown line. */
#if !defined(CONFIG_MG_BENCH_MODE)
	static const char *const human[] = {"talk down", "talk up", "pair confirm"};

	for (size_t i = 0; i < sizeof(human) / sizeof(human[0]); i++) {
		CHECK(mg_nus_word_class(human[i]) == MG_NUS_UNKNOWN);
		CHECK(mg_nus_word_locked(human[i], false, false, false)); /* release, unproven */
	}
#endif
	/* A board's own words (mg_board_nus_word()) change state. */
	CHECK(mg_nus_board_word_locked(false, false));
	CHECK(!mg_nus_board_word_locked(false, true));
	CHECK(!mg_nus_board_word_locked(true, false));
	CHECK(!mg_nus_board_word_locked(true, true));
}

/* An SMP request nesting arrays ~500 deep overflowed the work queue's
 * stack in zcbor's skip before any proof; smp.c drops one deeper than
 * MG_SMP_MAX_DEPTH first. Real requests pass, definite and
 * indefinite alike. */
static void test_smp_depth(void)
{
	static uint8_t pkt[8 + 600];
	/* An image upload: {"off":0,"data":h'..',"len":4096,"sha":h'..'}. */
	static const uint8_t upload[] = {0xa4, 0x63, 'o', 'f', 'f', 0x00, 0x64, 'd', 'a', 't', 'a',
					 0x42, 1, 2, 0x63, 'l', 'e', 'n', 0x19, 0x10, 0x00,
					 0x63, 's', 'h', 'a', 0x41, 9};
	/* Jackson-style indefinite map holding a definite array: {"a":[1,[2]]}. */
	static const uint8_t indef[] = {0xbf, 0x61, 'a', 0x82, 0x01, 0x81, 0x02, 0xff};
	uint8_t deep[20];

	printf("  SMP: a request nesting deeper than %d dropped before decoding\n",
	       MG_SMP_MAX_DEPTH);
	CHECK(mg_cbor_depth_ok(upload, sizeof(upload), MG_SMP_MAX_DEPTH));
	CHECK(mg_cbor_depth_ok(indef, sizeof(indef), MG_SMP_MAX_DEPTH));
	/* Exactly 8 deep passes, 9 doesn't (definite and indefinite). */
	memset(deep, 0x81, sizeof(deep));
	deep[8] = 0x00;
	CHECK(mg_cbor_depth_ok(deep, 9, 8));
	deep[8] = 0x81;
	deep[9] = 0x00;
	CHECK(!mg_cbor_depth_ok(deep, 10, 8));
	memset(deep, 0x9f, sizeof(deep));
	CHECK(!mg_cbor_depth_ok(deep, 9, 8));
	/* Siblings don't add depth: [[1],[1],[1],...] at depth 2. */
	{
		uint8_t sib[1 + 3 * 20] = {0x94};

		for (int k = 0; k < 20; k++) {
			sib[1 + 2 * k] = 0x81;
			sib[2 + 2 * k] = 0x01;
		}
		CHECK(mg_cbor_depth_ok(sib, 41, 8));
	}
	/* The attack: an echo of ~500 nested arrays in one packet, and as the
	 * second message after a good one. */
	memset(pkt, 0, sizeof(pkt));
	pkt[0] = 2;
	pkt[2] = (uint8_t)(500 >> 8);
	pkt[3] = (uint8_t)500;
	memset(pkt + 8, 0x81, 500);
	CHECK(!mg_smp_depth_ok(pkt, 8 + 500));
	{
		uint8_t two[8 + sizeof(upload) + 8 + 20];

		memset(two, 0, sizeof(two));
		two[0] = 2;
		two[3] = sizeof(upload);
		memcpy(two + 8, upload, sizeof(upload));
		two[8 + sizeof(upload) + 3] = 20;
		memset(two + 16 + sizeof(upload), 0x81, 20);
		CHECK(!mg_smp_depth_ok(two, sizeof(two)));
		CHECK(mg_smp_depth_ok(two, 8 + sizeof(upload)));
	}
	/* The echo as sent: {"d":"x","z":[[...]]}, whose "z" zcbor skips:
	 * the map and 7 arrays (8 deep) pass, 8 arrays don't, 493 don't. */
	{
		static uint8_t echo[7 + 493 + 1] = {0xa2, 0x61, 'd', 0x61, 'x', 0x61, 'z'};

		memset(echo + 7, 0x81, 493);
		echo[7 + 7] = 0x00;
		CHECK(mg_cbor_depth_ok(echo, 7 + 7 + 1, MG_SMP_MAX_DEPTH));
		echo[7 + 7] = 0x81;
		echo[7 + 8] = 0x00;
		CHECK(!mg_cbor_depth_ok(echo, 7 + 8 + 1, MG_SMP_MAX_DEPTH));
		echo[7 + 8] = 0x81;
		echo[7 + 493] = 0x00;
		CHECK(!mg_cbor_depth_ok(echo, sizeof(echo), MG_SMP_MAX_DEPTH));
	}
	/* Zephyr's reader decodes a body to the packet's end, past its own
	 * length: a nest cut at each body's length (8 deep in each) and carried
	 * on through the next header (8 more items to zcbor) would be one nest.
	 * Each body must be whole inside its length, so none of it passes. */
	{
		static const uint8_t hdr[8] = {0x82, 0x82, 0x00, 0x08, 0x82, 0x82, 0x82, 0x82};
		static uint8_t chain[8 + 10 + 4 * 16];
		size_t at = 8 + 10;

		memset(chain, 0, 8);
		chain[0] = 2;
		chain[3] = 10;
		memcpy(chain + 8, (const uint8_t[]){0xa1, 0x61, 'z'}, 3);
		memset(chain + 11, 0x82, 7);
		CHECK(!mg_cbor_depth_ok(chain + 8, 10, MG_SMP_MAX_DEPTH)); /* 8 deep, not whole */
		for (int k = 0; k < 4; k++, at += 16) {
			memcpy(chain + at, hdr, 8);
			memset(chain + at + 8, 0x82, 8);
		}
		CHECK(!mg_smp_depth_ok(chain, sizeof(chain)));
	}
	/* Fails closed. The 32-bit length: {"d":"x","z":<text of length
	 * 0x1_0000_0000>,"y":<480 nested arrays>}. On the nRF54L15 zcbor casts
	 * that length to a 32-bit size_t (0), skips nothing and decodes the nest
	 * as "y"'s value; the walk refuses any length, count or tag of 2^32 or
	 * more, whatever the host's size_t. */
	{
		static const uint8_t head[] = {0xa3, 0x61, 'd', 0x61, 'x', 0x61, 'z', 0x7b, 0, 0, 0,
					       1, 0, 0, 0, 0, 0x61, 'y'};
		static uint8_t v[8 + sizeof(head) + 480 + 1];
		size_t body = sizeof(head) + 480 + 1;

		memset(v, 0, 8);
		v[0] = 2;
		v[2] = (uint8_t)(body >> 8);
		v[3] = (uint8_t)body;
		memcpy(v + 8, head, sizeof(head));
		memset(v + 8 + sizeof(head), 0x81, 480);
		v[8 + body - 1] = 0x00;
		CHECK(!mg_cbor_depth_ok(v + 8, body, MG_SMP_MAX_DEPTH));
		CHECK(!mg_smp_depth_ok(v, sizeof(v)));
	}
	{
		/* Each 8-byte head (byte strings, text, arrays, maps, tags) of
		 * 2^32 + k, followed by what a length truncated to k would take:
		 * refused. */
		static const uint8_t majors[] = {0x5b, 0x7b, 0x9b, 0xbb, 0xdb};

		for (size_t m = 0; m < sizeof(majors); m++) {
			for (uint8_t k = 0; k < 3; k++) {
				uint8_t b[9 + 2 * 3] = {majors[m], 0, 0, 0, 1, 0, 0, 0, k};

				memset(b + 9, 0x00, 2 * k + 1); /* items (ints) for any reading */
				CHECK(!mg_cbor_depth_ok(b, 9 + k, MG_SMP_MAX_DEPTH));
				CHECK(!mg_cbor_depth_ok(b, sizeof(b), MG_SMP_MAX_DEPTH));
			}
		}
		/* 64-bit integers and doubles stay values, not lengths. */
		CHECK(mg_cbor_depth_ok((const uint8_t[]){0x1b, 1, 2, 3, 4, 5, 6, 7, 8}, 9, 8));
		CHECK(mg_cbor_depth_ok((const uint8_t[]){0xfb, 0x40, 9, 0x21, 0xfb, 0x54, 0x44, 0x2d,
							 0x18},
				       9, 8));
	}
	/* Truncated or malformed: refused, never left to the handler. */
	CHECK(!mg_cbor_depth_ok((const uint8_t[]){0x5a, 0xff}, 2, 8));      /* length past the end */
	CHECK(!mg_cbor_depth_ok((const uint8_t[]){0x62, 'a'}, 2, 8));       /* text past the end */
	CHECK(!mg_cbor_depth_ok((const uint8_t[]){0xff}, 1, 8));            /* a stray break */
	CHECK(!mg_cbor_depth_ok((const uint8_t[]){0x82, 0x01}, 2, 8));      /* an array cut short */
	CHECK(!mg_cbor_depth_ok((const uint8_t[]){0x9f, 0x01}, 2, 8));      /* no break */
	CHECK(!mg_cbor_depth_ok((const uint8_t[]){0x99, 0x01, 0x00, 0x01}, 4, 8)); /* 256 items */
	CHECK(!mg_cbor_depth_ok((const uint8_t[]){0x1c}, 1, 8));            /* reserved */
	CHECK(!mg_cbor_depth_ok((const uint8_t[]){0x5f, 0x41, 0x00, 0xff}, 4, 8)); /* indefinite bytes */
	CHECK(!mg_cbor_depth_ok((const uint8_t[]){0xc1}, 1, 8));            /* a tag with no item */
	CHECK(!mg_cbor_depth_ok((const uint8_t[]){0x81, 0xc1}, 2, 8));
	CHECK(!mg_cbor_depth_ok((const uint8_t[]){0x18}, 1, 8));            /* a head cut short */
	CHECK(mg_cbor_depth_ok((const uint8_t[]){0xc1, 0x1a, 0, 0, 0, 1}, 6, 8)); /* a tagged time */
	CHECK(mg_cbor_depth_ok((const uint8_t[]){0xa0}, 1, 8));
	CHECK(mg_cbor_depth_ok(NULL, 0, 8));
	/* Packets: each request whole inside its own length; an empty body
	 * only last (its decode would start on the next request's header). */
	{
		static const uint8_t empty[8] = {0, 0, 0, 0, 0, 1, 1, 0};
		static const uint8_t two[8 + 8 + 1] = {0, 0, 0, 0, 0, 1, 1, 0,
						       0, 0, 0, 1, 0, 1, 2, 0, 0xa0};
		static const uint8_t cut[8 + 1] = {0, 0, 0, 2, 0, 1, 1, 0, 0xa0};
		static const uint8_t tail[8 + 1 + 3] = {0, 0, 0, 1, 0, 1, 1, 0, 0xa0, 0, 0, 0};

		CHECK(mg_smp_depth_ok(empty, sizeof(empty)));
		CHECK(!mg_smp_depth_ok(two, sizeof(two)));
		CHECK(mg_smp_depth_ok(two + 8, 9));
		CHECK(!mg_smp_depth_ok(cut, sizeof(cut)));
		CHECK(!mg_smp_depth_ok(tail, sizeof(tail)));
		CHECK(mg_smp_depth_ok(tail, 9));
	}
}

HOST_MAIN(test_smp_carry, test_smp_depth, test_smp_source, test_reach, test_reach_att, test_peer_losses,
	  test_reach_safe_trial, test_trial_tick, test_input_policy, test_tx_acl_counter,
	  test_tx_progress, test_bench_mode_flows, test_update_vectors, test_nus_policy, test_bench_switch, test_safe_idle, test_update_window, test_evt_policy, test_press_bound, test_set_next, test_settings_window, test_led_command_vs_capture, test_plug_in, test_clock,
	  test_low_battery, test_boot_record, test_reset_hold, test_out_hold, test_ota_slot_held, test_starving_recovers)
