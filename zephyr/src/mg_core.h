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

#ifndef MG_CORE_H_
#define MG_CORE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mg_audio.h"

#include "mgcommands.h"

/*
 * The protocol engine for mgcommands.h: command parsing, push-to-talk,
 * settings and offline clip commands. It runs on one thread (the app
 * thread in main.c, or the test thread) and talks to the rest through
 * small functions declared in the module headers, which the unit tests
 * replace with fakes.
 */

/* Commands arrive here only after mg_session has let them through. */
void mg_core_control_rx(const uint8_t *buf, size_t len);

/* A client connected, or its session became ready for gestures and audio
 * (Control and Data subscribed). */
void mg_core_connected(void);
void mg_core_disconnected(void);
void mg_core_set_ready(bool ready);
/* "connected: <name> (<app> <version>)" or "connected: none";
 * without the name ("connected: <app> <version>") for the Nordic UART,
 * whose output the Android app relays into its logs: the phone's name is
 * the user's. */
size_t mg_core_client_line(char *out, size_t cap, bool with_name);
bool mg_core_is_ready(void);
bool mg_core_is_connected(void);
/* Recomputes the link state (LED, "mg.ble: link" log). */
void mg_core_link_refresh(void);
/* A new token proof challenge on the link: the client's ready state
 * is cleared, push-to-talk off (the codec stays), until it enables it again;
 * presses meanwhile send gestures only. */
void mg_core_proof_challenge(void);
/* The token proof's result on this connection. */
void mg_core_proof_result(bool matched);
/* The clip queue owns its storage and no earlier owner's clips wait to be
 * erased: recording, clip reads and downloads may run. */
bool mg_core_clips_ready(void);
/* This connection may get mic output (its token proof matched, or
 * the bench switch); whether a gated capture's held output may go now. */
bool mg_core_output_ok(void);
/* Clip metadata (queue status' count and bytes, clip_info) may go to this
 * connection: a gadget not set up, or this connection's token proof
 * matched. */
bool mg_core_clips_visible(void);
bool mg_core_gate_open(enum mg_audio_gate g);
/* What let this connection get mic output no longer stands (its proof was
 * reset, the bench switch went off): a running live capture or clip
 * download that isn't held ends at once. */
void mg_core_output_revoked(void);
/* The bench switch for this connection (who: the console, for the
 * log); -ENOTCONN with no link. Off at every connect, disconnect, boot. */
int mg_core_set_bench(bool on, const char *who, bool wired);
/* True when a release build locks the bench switch for this caller: a
 * wireless one on a set-up gadget (the wired USB console never). */
bool mg_core_ble_bench_locked(bool wired);
/* A real press on this connection within the last 5 s (mg_nus_policy.h);
 * automation builds only (a release build's bench switch takes the proof). */
bool mg_core_press_window_open(void);
/* A wake word the gadget heard itself: a press for the mic's press gate. */
void mg_core_wake_word(void);
/* CONFIG_MG_UPDATE_KEY: a board's dedicated non-talk key (the update
 * window). */
void mg_core_update_key(bool pressed, bool real);
bool mg_core_bench(void);
uint32_t mg_core_start_refused(void);
/* Privileged commands answered proof_required. */
uint32_t mg_core_privileged_refused(void);

void mg_core_button(bool pressed);
/* The same for an event that happened at at_ms (k_uptime_get()); real: from
 * the button's GPIO (mg_button.c), carried with the event. An injected press
 * never confirms a setup outside automation builds (mg_input_policy.h). */
void mg_core_button_from(bool pressed, int64_t at_ms, bool real);
/* after_arm: queued after the last confirm prompt was armed (mg_button.c's
 * sequence numbers); a press queued before it never confirms. */
void mg_core_button_ev(bool pressed, int64_t at_ms, bool real, bool after_arm);

/* The audio thread finished a stream (live, offline recording or clip). */
void mg_core_audio_idle(void);

/* Notify [error, cmd, code, log] on Control. log may be NULL. */
void mg_core_send_error(uint8_t cmd, uint16_t code, const char *log);
/* The same, ahead of everything queued (mg_session_send_urgent). */
int mg_core_send_error_urgent(uint8_t cmd, uint16_t code, const char *log);

/* Composes the supported_features payloads; exposed for tests. */
void mg_core_send_status(void);
/* A frame the device sends on its own, e.g. [get_settings, link_mode, mode]. */
void mg_core_send_unsolicited(const uint8_t *buf, size_t len);
/* report_battery_status sent unsolicited (on a battery change). */
void mg_core_battery_report(void);

void mg_core_init(void);

#endif
