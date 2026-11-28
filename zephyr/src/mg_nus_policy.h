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

/* Which Nordic UART console words a connection may run. In a release build
 * (CONFIG_MG_AUTOMATION off) the lock is default-deny: only the read words
 * below run on a connection without the token proof; every other line (the
 * bench switch, the link-mode A/B, the reach, tx dead and att dead tests, and
 * an unknown line too) answers "locked". Every word mg_nus.c dispatches is in
 * mg_nus_words[] with its class, and the build fails on a word that isn't
 * (tools/check_nus_words.py). Automation builds, and the USB console, keep
 * everything open. Host test_nus_policy. */

#ifndef MG_NUS_POLICY_H
#define MG_NUS_POLICY_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

enum mg_nus_word_class {
	MG_NUS_UNKNOWN,
	MG_NUS_READ,       /* reads only: open on any connection */
	MG_NUS_CHANGE,     /* changes state: a release build needs the proof */
	MG_NUS_BENCH_MODE, /* a scripted human step: only in CONFIG_MG_BENCH_MODE images */
};

/* Every word mg_nus.c's run_cmd dispatches, exactly as it compares them. */
static const struct {
	const char *word;
	enum mg_nus_word_class cls;
} mg_nus_words[] = {
	{"status", MG_NUS_READ},         {"version", MG_NUS_READ},
	{"settings", MG_NUS_READ},       {"queue", MG_NUS_READ},
	{"battery", MG_NUS_READ},        {"pins", MG_NUS_READ},
	{"reset", MG_NUS_READ},          {"listen 0", MG_NUS_CHANGE},
	{"listen 1", MG_NUS_CHANGE},     {"mode 0", MG_NUS_CHANGE},
	{"mode 1", MG_NUS_CHANGE},       {"mode 2", MG_NUS_CHANGE},
	{"bench 0", MG_NUS_CHANGE},      {"bench 1", MG_NUS_CHANGE},
	{"reach test", MG_NUS_CHANGE},   {"att dead test", MG_NUS_CHANGE},
	{"tx dead test", MG_NUS_CHANGE},
	/* The update prompt's press, scripted: automation and bench-mode builds
	 * (a release build refuses it in mg_nus.c, proven or not). */
	{"update window", MG_NUS_CHANGE},
#if defined(CONFIG_MG_BENCH_MODE)
	/* Every human step scriptable over the Nordic UART too (boards without
	 * a console UART): never in an image without bench mode
	 * (tools/check_nus_words.py). */
	{"talk down", MG_NUS_BENCH_MODE},     {"talk up", MG_NUS_BENCH_MODE},
	{"pair confirm", MG_NUS_BENCH_MODE},
	{"safe next", MG_NUS_BENCH_MODE},     {"reset next", MG_NUS_BENCH_MODE},
#endif
};

static inline enum mg_nus_word_class mg_nus_word_class(const char *cmd)
{
	for (size_t i = 0; i < sizeof(mg_nus_words) / sizeof(mg_nus_words[0]); i++) {
		if (strcmp(cmd, mg_nus_words[i].word) == 0) {
			return mg_nus_words[i].cls;
		}
	}
	return MG_NUS_UNKNOWN;
}

/* Default-deny in a release build: anything but a read word needs the
 * proof. A bench-mode image (never shipped) keeps every word open, as an
 * automation build does. */
static inline bool mg_nus_word_locked(const char *cmd, bool automation, bool proven,
				      bool bench_mode)
{
	return !automation && !bench_mode && !proven && mg_nus_word_class(cmd) != MG_NUS_READ;
}

/* The log mirror (every INF line: talk presses, captures, setup and link
 * events with their times) goes to an unproven central only where the owner
 * has nothing to lose: a bench-mode image (never published) or a gadget that
 * isn't set up. A set-up gadget mirrors only to this connection's
 * token-proven central, in automation builds too (testers run them; console
 * reads would give away an input timeline). Word replies (`status`, the
 * reads) aren't the mirror and keep their own rules. */
static inline bool mg_nus_log_allowed(bool bench_mode, bool set_up, bool proven)
{
	return bench_mode || !set_up || proven;
}

/* A board's own words (mg_board_nus_word()) all count as changing state:
 * not offered at all where the rule above would lock them. */
static inline bool mg_nus_board_word_locked(bool automation, bool proven)
{
	return !automation && !proven;
}

/* The bench switch (mic output without a token proof) never opens a set-up
 * gadget wirelessly: a release build refuses it outright, an automation
 * build takes it only on a token-proven connection (automation images run
 * on testers' gadgets too); a gadget that isn't set up isn't locked here
 * (mg_bench_ble_allowed() still is). */
static inline bool mg_bench_switch_allowed(bool automation, bool set_up, bool wired,
					   bool bench_mode, bool proven)
{
	/* The wired USB console is the owner's action; a bench-mode image
	 * (never published) isn't locked. */
	return wired || bench_mode || !set_up || (automation && proven);
}

/* Over a wireless link (the Nordic UART's `bench 1`) the bench switch acts
 * only with this connection's token proof, in every build but a bench-mode
 * image; never on a press (an automation build's press window would let an
 * unproven central stream the mic while the owner pressed). Simulations
 * (sim) are exempt; a wired console is the owner's action. */
#define MG_PRESS_WINDOW_MS 5000

static inline bool mg_bench_ble_allowed(bool wired, bool sim, bool proven, bool bench_mode)
{
	/* A bench-mode image needs no proof. */
	return wired || sim || bench_mode || proven;
}

/* A real press (the button's own GPIO) at press_ms on connection press_gen
 * opens a window for that connection only, MG_PRESS_WINDOW_MS long. */
static inline bool mg_press_window_open(int64_t press_ms, uint32_t press_gen, int64_t now_ms,
					uint32_t conn_gen)
{
	return press_gen != 0 && press_gen == conn_gen && press_ms != 0 &&
	       now_ms - press_ms <= MG_PRESS_WINDOW_MS;
}

#endif /* MG_NUS_POLICY_H */
