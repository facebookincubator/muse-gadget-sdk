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

#include "mg_clock.h"
#include "mg_setup_store.h"
#include "mg_transport.h"

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>

#include "mgcommands.h"

LOG_MODULE_REGISTER(mg_clock, CONFIG_MG_LOG_LEVEL);

#define CLOCK_KEY "mg/clk"

static struct {
	bool synced;
	int64_t base_ms; /* unix ms minus uptime ms */
	int16_t off;
	uint8_t flags;
	/* What mg/clk holds (0, 0 when nothing is stored). */
	int16_t stored_off;
	uint8_t stored_flags;
} clk;

/* An offset and flags waiting for flash (save(), below). */
static bool clk_dirty;
static int16_t pend_off;
static uint8_t pend_flags;
/* Offset/flags an unproven central set on a set-up gadget: that
 * connection's only. They're saved if its proof matches on the same
 * connection (a phone may send set_unix_time before the proof, never
 * after), and RAM drops back to what mg/clk holds once another connection
 * is the one asking. */
static bool unproven_pending;
static uint32_t unproven_gen;
static int16_t unproven_off;
static uint8_t unproven_flags;

static void drop_stale_unproven(void)
{
	if (unproven_pending && unproven_gen != mg_transport_conn_gen()) {
		clk.off = clk.stored_off;
		clk.flags = clk.stored_flags;
		unproven_pending = false;
	}
}

static void write_now(int16_t off, uint8_t flags)
{
	uint8_t b[3] = {(uint8_t)off, (uint8_t)((uint16_t)off >> 8), flags};
	int err = settings_save_one(CLOCK_KEY, b, sizeof(b));

	if (err) {
		LOG_WRN("clock: offset not saved (%d)", err);
		return;
	}
	clk.stored_off = off;
	clk.stored_flags = flags;
}

/* No flash write per command from a stranger: on a gadget that isn't set up
 * any central may set the clock, so the offset and flags stay in RAM until
 * the settings' 10-min save window, setup's commit or a planned power off
 * (mg_clock_flush()); a set-up gadget saves at once. */
/* A gadget that isn't set up: the change waits for the settings' save
 * window (at most once per 10 min, as set_settings), setup's commit or a
 * planned power off (mgcommands.h, Clock). mg_settings.c has the window;
 * weak, nothing (RAM until a flush) without it. */
__attribute__((weak)) void mg_settings_clock_changed(void)
{
}

static void save(int16_t off, uint8_t flags)
{
	if (!mg_setup_store_complete()) {
		pend_off = off;
		pend_flags = flags;
		clk_dirty = true;
		mg_settings_clock_changed();
		return;
	}
	clk_dirty = false;
	write_now(off, flags);
}

void mg_clock_proof_matched(void)
{
	drop_stale_unproven();
	if (unproven_pending && mg_setup_store_complete()) {
		unproven_pending = false;
		if (unproven_off != clk.stored_off || unproven_flags != clk.stored_flags) {
			write_now(unproven_off, unproven_flags); /* this connection's own, before its proof */
		}
	}
}

void mg_clock_flush(void)
{
	if (clk_dirty) {
		clk_dirty = false;
		write_now(pend_off, pend_flags);
	}
}

static int load_cb(const char *key, size_t len, settings_read_cb read_cb, void *cb_arg,
		   void *param)
{
	uint8_t b[3];

	(void)param;
	(void)key;
	if (len != sizeof(b)) {
		return 0;
	}
	if (read_cb(cb_arg, b, sizeof(b)) == (ssize_t)sizeof(b)) {
		clk.off = (int16_t)(b[0] | b[1] << 8);
		clk.flags = b[2] & mg_clock_flag_24_hour;
		clk.stored_off = clk.off;
		clk.stored_flags = clk.flags;
	}
	return 0;
}

void mg_clock_reset(bool offset_too)
{
	clk.synced = false;
	clk.base_ms = 0;
	if (offset_too) {
		clk.off = 0;
		clk.flags = 0;
	}
}

void mg_clock_load(void)
{
	mg_clock_reset(true);
	clk_dirty = false;
	unproven_pending = false;
	clk.stored_off = 0;
	clk.stored_flags = 0;
	(void)settings_load_subtree_direct(CLOCK_KEY, load_cb, NULL);
	LOG_INF("clock: unsynced, UTC%+d min, %s", clk.off,
		(clk.flags & mg_clock_flag_24_hour) ? "24 h" : "12 h");
}

void mg_clock_set(int64_t unix_ms, bool has_off, int16_t off, bool has_flags, uint8_t flags,
		  bool may_store)
{
	drop_stale_unproven();
	clk.base_ms = unix_ms - k_uptime_get();
	clk.synced = true;
	if (has_off) {
		clk.off = off;
	}
	if (has_flags) {
		clk.flags = flags & mg_clock_flag_24_hour;
	}
	bool differs = clk.off != clk.stored_off || clk.flags != clk.stored_flags;
	/* Only what this set carried is stored: values an earlier unproven set
	 * left in RAM stay out of storage (what waits for flash counts as
	 * stored). */
	int16_t base_off = clk_dirty ? pend_off : clk.stored_off;
	uint8_t base_flags = clk_dirty ? pend_flags : clk.stored_flags;
	int16_t s_off = has_off ? clk.off : base_off;
	uint8_t s_flags = has_flags ? clk.flags : base_flags;

	if (may_store && (s_off != clk.stored_off || s_flags != clk.stored_flags)) {
		save(s_off, s_flags);
	} else if (may_store) {
		clk_dirty = false; /* back to what is stored: nothing waits */
	}
	if (may_store) {
		unproven_pending = false;
	} else if (mg_setup_store_complete()) {
		/* What this unproven set carried, on top of what this connection
		 * set before it: saved if its proof matches (mg_clock_proof_matched()). */
		unproven_off = has_off ? clk.off : (unproven_pending ? unproven_off : clk.stored_off);
		unproven_flags = has_flags ? clk.flags : (unproven_pending ? unproven_flags : clk.stored_flags);
		unproven_pending = true;
		unproven_gen = mg_transport_conn_gen();
	}
	differs = clk.off != clk.stored_off || clk.flags != clk.stored_flags;
	LOG_INF("clock: set, UTC%+d min, %s%s", clk.off,
		(clk.flags & mg_clock_flag_24_hour) ? "24 h" : "12 h",
		differs && !may_store ? " (not stored: no token proof)" : "");
}

void mg_clock_get(int64_t *unix_ms, int16_t *off, uint8_t *flags)
{
	drop_stale_unproven();
	*unix_ms = clk.synced ? clk.base_ms + k_uptime_get() : INT64_MIN;
	*off = clk.off;
	*flags = clk.flags;
}

uint16_t mg_clock_action(const uint8_t *a, size_t n, bool may_store, uint8_t *out,
			 size_t *out_len)
{
	*out_len = 0;
	if (n < 1) {
		return mg_error_code_invalid_length;
	}
	if (a[0] == mg_device_action_get_unix_time) {
		int64_t ms;
		int16_t off;
		uint8_t flags;

		mg_clock_get(&ms, &off, &flags);
		out[0] = mg_command_device_action;
		out[1] = mg_device_action_report_unix_time;
		for (int i = 0; i < 8; i++) {
			out[2 + i] = (uint8_t)((uint64_t)ms >> (8 * i));
		}
		out[10] = (uint8_t)off;
		out[11] = (uint8_t)((uint16_t)off >> 8);
		out[12] = flags;
		*out_len = MG_CLOCK_REPORT_LEN;
		return 0;
	}
	if (a[0] != mg_device_action_set_unix_time) {
		return mg_error_code_unsupported;
	}
	if (n < 9) {
		return mg_error_code_invalid_length;
	}
	uint64_t u = 0;

	for (int i = 0; i < 8; i++) {
		u |= (uint64_t)a[1 + i] << (8 * i);
	}
	int64_t ms = (int64_t)u;

	if (ms == INT64_MIN || ms < MG_CLOCK_FLOOR_MS || ms > MG_CLOCK_CEIL_MS) {
		/* Never synced, before the floor that tells a set clock from
		 * an unset one (mgcommands.h, Clock), or past the ceiling:
		 * refused, the clock as it was. */
		return mg_error_code_invalid_value;
	}
	bool has_off = n >= 11, has_flags = n >= 12;
	int16_t off = has_off ? (int16_t)(a[9] | a[10] << 8) : 0;
	uint8_t flags = has_flags ? a[11] : 0;

	mg_clock_set(ms, has_off, off, has_flags, flags, may_store); /* no reply */
	return 0;
}
