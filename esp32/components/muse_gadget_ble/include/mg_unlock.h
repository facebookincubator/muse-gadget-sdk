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

#pragma once

/*
 * The Nordic UART console's bench words (the console has no owner unlock: in
 * an automation build, CONFIG_MUSE_GADGET_AUTOMATION, every word works on any
 * connection; in a release build a state-changing word needs this
 * connection's proof, mg_unlock_remote_ok() below):
 *   ?UNLOCK, !UNLOCK ..., !LOCK -> "UNLOCK not needed" (old bench tools
 *                       that still send them get an answer and carry on; !LOCK
 *                       also drops a !RESTART schedule, as it did).
 *   !RESTART <ms>    -> one planned restart in 1..600 s, for
 *                       restart-to-connected runs while a phone holds the
 *                       link.
 *   !RESTART <interval_ms> <count> -> a bounded schedule: count <= 7
 *                       restarts, interval >= 30 s
 *                       (after the boot is healthy). The caller keeps it in
 *                       RTC no-init memory so it survives its own planned
 *                       restarts and dies on power loss; a crash, !RESTART 0,
 *                       !LOCK or the count running out clears it.
 *   !PAIR AUTO [0|1] -> the next Link setup confirmation is pressed for the
 *                       user, once, within 10 min, 6 s into its wait as a
 *                       person would press (RAM only; !PAIR AUTO 0 or a
 *                       reboot ends it); the confirm screen says
 *                       "Auto-confirm on" meanwhile. Open in automation
 *                       builds so benches stay automatable: it can complete
 *                       a setup with no press, a known trade-off. A release
 *                       build never arms it over the air, proven or not
 *                       (mg_unlock_remote_ok); !PAIR AUTO 0 still runs. The
 *                       USB console's >pair.auto does the same in every build
 *                       (mg_unlock_pair_auto()).
 * Pure (no IDF), for the host tests: the caller gives the clock. (The file
 * keeps its name from the owner unlock it once held.)
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MG_UNLOCK_AUTO_US (10LL * 60 * 1000 * 1000)

typedef struct {
    int64_t auto_until_us;    /* 0: auto-confirm not armed */
    uint32_t restart_ms;      /* !RESTART asked for (the caller takes it) */
    uint32_t restart_count;   /* 0: one restart; else a schedule of that many */
    bool restart_clear;       /* !RESTART 0 or !LOCK: drop a schedule */
} mg_unlock_t;

void mg_unlock_init(mg_unlock_t *u);
/*
 * One console line. Returns true if it was one of the words above and writes
 * the reply line into out (no newline); false otherwise.
 */
bool mg_unlock_line(mg_unlock_t *u, const char *line, int64_t now_us, char *out, size_t cap);
bool mg_unlock_auto_armed(const mg_unlock_t *u, int64_t now_us);
/* !PAIR AUTO's arm (on) or drop (off); also the USB console's >pair.auto / >pair.auto=0. */
void mg_unlock_pair_auto(mg_unlock_t *u, int64_t now_us, bool on);
/* Takes the auto-confirm if armed (once): true = press the confirmation. */
bool mg_unlock_auto_take(mg_unlock_t *u, int64_t now_us);
/*
 * ble.pause's argument (>ble.pause=..., !IN ble.pause=...): "SECS" or
 * "SECS,AA:BB:CC:DD:EE:FF" (hex either case). SECS is digits only, 1..600.
 * True with *secs, and *has_addr with addr (most significant byte first);
 * false for anything else.
 */
bool mg_unlock_pause_parse(const char *text, uint32_t *secs, uint8_t addr[6], bool *has_addr);
/*
 * A Nordic UART console line that changes state: every "!" or "?"
 * word, every !IN word but the read-only voice?, wake?, call?, vib?,
 * mic.gain?, ..., and the plain words that act (vib=, safe.off, the bench
 * tunings with "="). False for the reads (status, version, guard?, ota?,
 * vib?, ...).
 */
bool mg_unlock_automation_word(const char *line);
/*
 * Whether a Nordic UART line may run in a release build
 * (CONFIG_MUSE_GADGET_AUTOMATION off). A state-changing word
 * (mg_unlock_automation_word) runs only on a connection whose token proof
 * matched; in SAFE, safe.off / safe=0 (the way out of SAFE) also run
 * unproven. Reads always run. Arming the bench auto-confirm (!PAIR AUTO,
 * !PAIR AUTO 1) never runs, proven or not: it's for benches (the USB
 * console's >pair.auto arms it in every build). Automation builds don't ask:
 * every word runs.
 */
bool mg_unlock_remote_ok(const char *line, bool proven, bool safe_mode);
/* A Nordic UART read of the owner's content (!IN voice?, voice activity; !IN img?, the shown image's path): on a
 * release build of a set-up gadget it runs only on a connection whose token proof matched; every other line answers
 * as before (mg_unlock_remote_ok). */
bool mg_unlock_owner_word(const char *line);
bool mg_unlock_owner_ok_in(const char *line, bool proven, bool set_up, bool release);
/* A !RESTART the console asked for: its delay (or interval) in ms, once; 0 =
 * none. *count: 0 for one restart, else the schedule's length. *clear: drop
 * any schedule (!RESTART 0, !LOCK). */
uint32_t mg_unlock_restart_take(mg_unlock_t *u, uint32_t *count, bool *clear);
