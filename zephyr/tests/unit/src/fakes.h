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

#ifndef FAKES_H_
#define FAKES_H_

#include <stdbool.h>
#include <stdint.h>

#include "mg_audio.h"
#include "mg_transport.h"

struct fake_note {
	uint8_t ch;
	uint16_t len;
	uint8_t data[260];
};

#define FAKE_MAX_NOTES 64
extern struct fake_note fake_notes[FAKE_MAX_NOTES];
extern int fake_nnotes;
extern int fake_urgent_notes; /* mg_transport_notify_urgent */
extern char fake_peer_name[40]; /* the central's GAP Device Name */
extern int fake_urgent_fail;  /* the next N urgent notifications fail */
extern uint16_t fake_mtu;
extern int fake_disconnects;
extern int fake_adv_boosts; /* mg_transport_adv_boost */
extern int fake_buzzes;
extern int fake_power_offs;
extern int fake_audio_commits;
extern int fake_audio_gated;
extern int fake_audio_turns;
extern int fake_audio_gate_fails;
extern int fake_audio_gate_drops;
extern int fake_audio_discards;
extern int fake_led_events[4]; /* per enum mg_led_event */
extern uint32_t fake_led_flags;
extern int fake_led_link;
extern int fake_led_face;
extern int fake_led_faces; /* face changes */

/* Connection parameter requests (mg_transport_conn_params). */
struct fake_conn_req {
	uint16_t min, max, latency, timeout;
};
extern struct fake_conn_req fake_conn_reqs[16];
extern int fake_nconn_reqs;
extern int fake_latency_cancels; /* mg_transport_latency_cancel */

/* Audio calls mg_core made. */
extern int fake_audio_starts;
extern uint8_t fake_audio_codec;
extern uint16_t fake_audio_rate;
extern uint16_t fake_audio_kbps;
extern bool fake_audio_ptt;
extern int fake_audio_stops;
extern int fake_audio_recordings;
extern int fake_audio_clip_reads;
extern uint16_t fake_audio_clip;
extern uint8_t fake_audio_gain;
extern enum mg_audio_activity fake_audio_state;

/* Sleep this long in each Data notification (paces a sending thread). */
extern int fake_notify_delay_us;
/* Data notifications seen, and how many (counted even past FAKE_MAX_NOTES). */
extern int fake_data_notes;

void fake_reset(void);
void fake_settle_wipe(void);
/* nth (0-based) notification on ch whose first byte is cmd, or NULL. */
const struct fake_note *fake_find(uint8_t ch, uint8_t cmd, int nth);
int fake_count(uint8_t ch, uint8_t cmd);
/* Connects and subscribes the characteristics a session needs. */
void fake_connect(void);
/* mg_button_flush_injected() calls (mg_core.c's confirm-arming hook). */
extern int fake_button_flushes;
extern int fake_board_prompt_presses;
/* settings, queue, core and session, once. */
void fake_init_once(void);

extern k_timeout_t fake_last_control_wait; /* mg_transport_notify()'s last Control wait */
extern int fake_audio_start_fail; /* the next N mg_audio_start_live() calls fail */
extern bool fake_audio_last_silent; /* mg_audio_last_turn_silent() */

#endif
