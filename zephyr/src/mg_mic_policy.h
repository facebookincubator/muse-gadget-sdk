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
 * The microphone's press gate (mgcommands.h start_mic): on a release build
 * of a set-up gadget a client's start_mic needs, besides the token proof, a
 * press, gesture or wake word the gadget itself detected within the last
 * MG_MIC_PRESS_WINDOW_MS; console words, protocol commands (mg_session_wake)
 * and a charger never count. Otherwise press_required: no capture, no mic
 * ring. Within the window a start_mic after a stop_mic is taken again (not
 * one per press). Automation builds need the proof alone.
 */

#ifndef MG_MIC_POLICY_H
#define MG_MIC_POLICY_H

#include <stdbool.h>
#include <stdint.h>

#define MG_MIC_PRESS_WINDOW_MS 5000

/* press_ms: when the last real press, gesture or wake word came (0: none). */
static inline bool mg_mic_press_ok(bool release, bool set_up, int64_t press_ms, int64_t now_ms)
{
	return !release || !set_up ||
	       (press_ms != 0 && now_ms - press_ms <= MG_MIC_PRESS_WINDOW_MS);
}

/* A release gadget that isn't set up refuses a client's start_mic with proof_required (it
 * has no token to prove); bench, automation and test builds are exempt. */
static inline bool mg_mic_start_needs_setup(bool release, bool set_up)
{
	return release && !set_up;
}

#endif /* MG_MIC_POLICY_H */
