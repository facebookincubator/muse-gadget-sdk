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

/* The re-setup prompt's rate (README.md, Gadget setup v2): on a set-up
 * gadget a pairing_client_hello is never refused while the prompt is up on
 * its own connection: it takes the prompt over (bound to the newest hello,
 * no new flash), and the one press confirms that hello. A hello from another
 * connection while the prompt is up gets error_operation_in_progress and
 * leaves the prompt bound to the first. With no prompt up, only a hello on
 * the connection of a dismiss (the back gesture, or a talk press on a
 * re-setup prompt) within MG_RESETUP_PROMPT_MS of it is refused; test and bench builds are exempt
 * from that rule only. A gadget that isn't set up has no rate. Host
 * test_resetup_rate (mg-session-v1.json's re-setup prompt exchanges). */

#ifndef MG_RESETUP_POLICY_H
#define MG_RESETUP_POLICY_H

#include <stdbool.h>
#include <stdint.h>

#define MG_RESETUP_PROMPT_MS 10000

enum mg_resetup_hello {
	MG_RESETUP_SHOW,     /* the prompt shows, bound to this hello */
	MG_RESETUP_TAKEOVER, /* the prompt up on this connection is now this hello's */
	MG_RESETUP_BUSY,     /* error_operation_in_progress, nothing shows */
};

/* prompt_conn: the connection the prompt is up on (0: none up);
 * dismissed_conn and dismissed_ms: the connection and time of the last dismiss
 * (0: none). */
static inline enum mg_resetup_hello mg_resetup_hello(bool exempt, bool set_up,
						     uint32_t prompt_conn, uint32_t conn,
						     uint32_t dismissed_conn, int64_t dismissed_ms,
						     int64_t now_ms)
{
	if (prompt_conn != 0) {
		if (prompt_conn == conn) {
			return MG_RESETUP_TAKEOVER;
		}
		return set_up ? MG_RESETUP_BUSY : MG_RESETUP_SHOW;
	}
	if (set_up && !exempt && dismissed_ms != 0 && dismissed_conn == conn &&
	    now_ms - dismissed_ms < MG_RESETUP_PROMPT_MS) {
		return MG_RESETUP_BUSY;
	}
	return MG_RESETUP_SHOW;
}

/* Setup mode on a board whose only key is its talk key (README.md, The
 * press): it begins only on a normal boot whose button hold was released
 * before the SAFE cue (main.c), never in SAFE of either kind (the button's
 * or the gadget's own), which runs no Link setup; it lasts until until_ms
 * (MG_SETUP_MODE_MS after that boot) or until the confirmed re-setup (on
 * false). Test and bench builds (exempt) are always in it. */
static inline bool mg_setup_mode_begins(bool safe, bool released_early)
{
	return released_early && !safe;
}

static inline bool mg_setup_mode_now(bool exempt, bool on, int64_t until_ms, int64_t now_ms)
{
	return exempt || (on && now_ms < until_ms);
}

/* An ownership change's clip erase owed at boot (mg_setup_clips_resume(),
 * the setup store's clips mark): a build without a clip store owes nothing
 * and the mark goes; a store that didn't mount keeps the mark (a failed
 * mount is never taken for no store: the erase runs at a boot that mounts
 * it); a mounted one starts the erase unless it is already under way. The
 * mark goes only once the erase has run (mg_setup.c, clips_wiped_fn). */
enum mg_clips_owed {
	MG_CLIPS_OWED_KEEP,  /* the mark stays, nothing starts */
	MG_CLIPS_OWED_ERASE, /* start the erase (forget_clips) */
	MG_CLIPS_OWED_GONE,  /* nothing to erase: the mark goes */
};

static inline enum mg_clips_owed mg_clips_owed_step(bool store_built, bool mounted, bool wiping)
{
	if (!store_built) {
		return MG_CLIPS_OWED_GONE;
	}
	if (!mounted || wiping) {
		return MG_CLIPS_OWED_KEEP;
	}
	return MG_CLIPS_OWED_ERASE;
}

#endif /* MG_RESETUP_POLICY_H */
