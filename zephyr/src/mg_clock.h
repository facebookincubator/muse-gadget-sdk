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

#ifndef MG_CLOCK_H_
#define MG_CLOCK_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * The wall clock the phone sets (mgcommands.h, Clock):
 * device_action set_unix_time 18, get_unix_time 19 -> report_unix_time 20.
 * The time is kept against the uptime counter, so it lasts across
 * disconnects but not a reset (no RTC kept through one): after a reset the
 * gadget is unsynced (INT64_MIN) until the next set. The UTC offset and the
 * 24-hour flag are kept when a set omits them; before any set they are 0
 * (UTC, 12-hour). They are persisted (settings key mg/clk) only when the
 * set may store (a token-proven connection, or a gadget that isn't set up)
 * and they differ from what is stored; the time, offset and flags always
 * apply in RAM.
 */

/* The report: [device_action, report_unix_time, int64 ms, int16 off, flags]. */
/* The clock's floor (mgcommands.h, Clock): a set before
 * 2024-01-01T00:00:00Z is refused (invalid_value). */
#define MG_CLOCK_FLOOR_MS 1704067200000LL
/* Its ceiling (mgcommands.h): the last millisecond of 9999
 * (9999-12-31T23:59:59.999Z); a later set is refused (invalid_value), so the
 * clock's base plus the uptime never overflows. */
#define MG_CLOCK_CEIL_MS 253402300799999LL
#define MG_CLOCK_REPORT_LEN 13

/* Loads the persisted offset and flags. */
void mg_clock_load(void);
/* Writes a waiting offset/flags change (setup's commit, a planned power off). */
void mg_clock_flush(void);
/* This connection's token proof matched: clock offset/flags it set before
 * the proof (on a set-up gadget) are saved now. */
void mg_clock_proof_matched(void);
/* mg_settings.c: an unset gadget's clock offset/flags changed; written in the
 * settings' save window (weak default in mg_clock.c: none), or by
 * mg_clock_flush() at setup's commit or a planned power off. */
void mg_settings_clock_changed(void);
/* Forgets the time (and, with offset_too, the offset and flags). */
void mg_clock_reset(bool offset_too);

/* unix_ms is UTC; offset and flags apply only when has_off / has_flags. */
void mg_clock_set(int64_t unix_ms, bool has_off, int16_t off, bool has_flags, uint8_t flags,
		  bool may_store);
/* unix_ms is INT64_MIN when never synced since boot. */
void mg_clock_get(int64_t *unix_ms, int16_t *off, uint8_t *flags);

/*
 * A device_action for the clock: a = [action, params...] (n bytes, action
 * set_unix_time or get_unix_time). Returns 0 with the reply in out (out_len
 * bytes, 0 for a set, which has no reply), or the mg_error_code to answer.
 * A set that may not store still applies and succeeds.
 */
uint16_t mg_clock_action(const uint8_t *a, size_t n, bool may_store, uint8_t *out,
			 size_t *out_len);

#endif
