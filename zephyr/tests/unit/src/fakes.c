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

#include "fakes.h"
#include "mg_core.h"
#include "mg_queue.h"

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>

#include "mg_led.h"
#include "mg_session.h"
#include "mg_settings.h"

struct fake_note fake_notes[FAKE_MAX_NOTES];
int fake_nnotes;
uint16_t fake_mtu = 247;
int fake_disconnects;
int fake_buzzes;
uint32_t fake_led_flags;

int fake_audio_starts;
uint8_t fake_audio_codec;
uint16_t fake_audio_rate;
uint16_t fake_audio_kbps;
bool fake_audio_ptt;
int fake_audio_stops;
int fake_audio_recordings;
int fake_audio_clip_reads;
uint16_t fake_audio_clip;
uint8_t fake_audio_gain;
enum mg_audio_activity fake_audio_state;
int fake_led_events[4];

void fake_settle_wipe(void)
{
	/* An owner change's erase in the background (mg_queue_forget) from an
	 * earlier test ends, and its mark with it. */
	for (int i = 0; i < 10000 && mg_queue_wiping(); i++) {
		k_sleep(K_MSEC(1));
	}
	k_sleep(K_MSEC(1));
}

void fake_reset(void)
{
	fake_audio_start_fail = 0;
	fake_audio_last_silent = false;
	fake_settle_wipe();
	fake_nnotes = 0;
	fake_urgent_notes = 0;
	fake_peer_name[0] = '\0';
	fake_urgent_fail = 0;
	fake_mtu = 247;
	fake_disconnects = 0;
	fake_buzzes = 0;
	fake_audio_starts = 0;
	fake_audio_codec = 0;
	fake_audio_rate = 0;
	fake_audio_kbps = 0;
	fake_audio_ptt = false;
	fake_audio_stops = 0;
	fake_audio_recordings = 0;
	fake_audio_commits = 0;
	fake_audio_discards = 0;
	memset(fake_led_events, 0, sizeof(fake_led_events));
	fake_audio_clip_reads = 0;
	fake_audio_state = MG_AUDIO_IDLE;
	fake_audio_gated = 0;
	fake_audio_turns = 0;
	fake_audio_gate_fails = 0;
	fake_audio_gate_drops = 0;
}

const struct fake_note *fake_find(uint8_t ch, uint8_t cmd, int nth)
{
	for (int i = 0; i < fake_nnotes; i++) {
		if (fake_notes[i].ch == ch && fake_notes[i].len > 0 &&
		    fake_notes[i].data[0] == cmd && nth-- == 0) {
			return &fake_notes[i];
		}
	}
	return NULL;
}

int fake_count(uint8_t ch, uint8_t cmd)
{
	int n = 0;

	while (fake_find(ch, cmd, n)) {
		n++;
	}
	return n;
}

void fake_connect(void)
{
	mg_session_connected();
	mg_session_subscribed(MG_ATT_CONTROL, true);
	mg_session_subscribed(MG_ATT_DATA, true);
	mg_session_status_requested(); /* as a client's request_status */
	/* The suites' central is a bench client (mic output without a
	 * token proof); test_core's unproven-central cases turn it off. */
	(void)mg_core_set_bench(true, "unit test", true);
}

/* Transport */

int fake_notify_delay_us;
int fake_data_notes;

char fake_peer_name[40]; /* mg_transport_peer_name */

size_t mg_transport_peer_name(char *out, size_t cap)
{
	size_t n = MIN(strlen(fake_peer_name), cap - 1);

	memcpy(out, fake_peer_name, n);
	out[n] = '\0';
	return n;
}

bool mg_transport_tx_stalled(void)
{
	return false;
}

unsigned int mg_transport_notify_dropped(void)
{
	return 0;
}

bool mg_transport_tx_pending(void)
{
	return false;
}

bool mg_transport_evt_saturated(uint32_t stale_ms)
{
	ARG_UNUSED(stale_ms);
	return false;
}

bool mg_transport_tx_dead(void)
{
	return false;
}

void mg_transport_rx_request(void)
{
}

int fake_urgent_notes; /* mg_transport_notify_urgent */
int fake_urgent_fail;  /* the next N urgent notifications fail (-ENOMEM) */

int mg_transport_notify_urgent(enum mg_att_chan ch, const uint8_t *buf, size_t len)
{
	if (fake_urgent_fail > 0) {
		fake_urgent_fail--;
		return -ENOMEM;
	}
	fake_urgent_notes++;
	return mg_transport_notify(ch, buf, len, K_NO_WAIT);
}

k_timeout_t fake_last_control_wait;

int mg_transport_notify(enum mg_att_chan ch, const uint8_t *buf, size_t len, k_timeout_t wait)
{
	if (ch == MG_ATT_CONTROL) {
		fake_last_control_wait = wait;
	}
	if (len + 3 > fake_mtu) {
		return -EMSGSIZE; /* the stack refuses it (ATT MTU - 3) */
	}
	if (ch == MG_ATT_DATA) {
		fake_data_notes++;
		if (fake_notify_delay_us) {
			k_sleep(K_USEC(fake_notify_delay_us));
			return 0; /* not kept: a test stream would fill the list */
		}
	}
	if (fake_nnotes == FAKE_MAX_NOTES || len > sizeof(fake_notes[0].data)) {
		return -ENOMEM;
	}
	fake_notes[fake_nnotes].ch = ch;
	fake_notes[fake_nnotes].len = len;
	memcpy(fake_notes[fake_nnotes].data, buf, len);
	fake_nnotes++;
	return 0;
}

int fake_adv_boosts;

void mg_transport_adv_boost(void)
{
	fake_adv_boosts++;
}

bool mg_transport_link_up(void)
{
	return true;
}

bool mg_transport_att_ok(void)
{
	return true;
}

bool fake_audio_last_silent;

bool mg_audio_last_turn_silent(void)
{
	return fake_audio_last_silent;
}

int fake_button_flushes;
/* A board's update prompt (mg_board_press_take, the real core's route):
 * this many presses answer it. */
int fake_board_prompt_presses;

bool mg_board_press_take(bool real, bool injected_counts)
{
	if (fake_board_prompt_presses > 0 && (real || injected_counts)) {
		fake_board_prompt_presses--;
		return true;
	}
	return false;
}

void mg_button_flush_injected(void)
{
	fake_button_flushes++;
}

void mg_transport_disconnect(void)
{
	fake_disconnects++;
}

uint16_t mg_transport_att_mtu(void)
{
	return fake_mtu;
}

uint32_t mg_transport_conn_gen(void)
{
	return 0;
}

bool mg_transport_encrypted(void)
{
	return true;
}

const char *mg_transport_name(void)
{
	return "MuseGadget-A1B2C3";
}

int fake_power_offs;

void mg_app_power_off(const char *why)
{
	ARG_UNUSED(why);
	fake_power_offs++;
}

const char *mg_app_version(void)
{
	return "1.0.0";
}

void mg_ble_refresh_advertising(void)
{
}

struct k_work_q *mg_app_wq(void)
{
	return &k_sys_work_q;
}

/* LED */

void mg_led_set(uint32_t flag, bool on)
{
	if (on) {
		fake_led_flags |= flag;
	} else {
		fake_led_flags &= ~flag;
	}
}

bool mg_led_update_cue_showing(void)
{
	return mg_led_update_cue_shows(fake_led_flags);
}

void mg_led_event(enum mg_led_event e)
{
	if ((unsigned int)e < 4) {
		fake_led_events[e]++;
	}
}

struct fake_conn_req fake_conn_reqs[16];
int fake_nconn_reqs;

int fake_latency_cancels;

int mg_transport_latency_cancel(void)
{
	fake_latency_cancels++;
	return 0;
}

int mg_transport_conn_params(uint16_t interval_min, uint16_t interval_max, uint16_t latency,
			     uint16_t timeout)
{
	if (fake_nconn_reqs < (int)ARRAY_SIZE(fake_conn_reqs)) {
		fake_conn_reqs[fake_nconn_reqs] = (struct fake_conn_req){
			interval_min, interval_max, latency, timeout};
	}
	fake_nconn_reqs++;
	return 0;
}

int fake_led_link;
int fake_led_face;
int fake_led_faces;

void mg_led_set_link(enum mg_led_link link)
{
	fake_led_link = link;
}

void mg_led_set_face(enum mg_led_face face, const char *why)
{
	ARG_UNUSED(why);
	fake_led_face = face;
	fake_led_faces++;
}

enum mg_led_face mg_led_face(void)
{
	return fake_led_face;
}

const char *mg_led_link_name(enum mg_led_link link)
{
	static const char *const n[] = {"never", "disconnected", "connecting", "session", "ready"};

	return link < ARRAY_SIZE(n) ? n[link] : "?";
}

void mg_led_buzz(uint16_t freq_hz, uint16_t duration_ms, uint8_t volume)
{
	ARG_UNUSED(freq_hz);
	ARG_UNUSED(duration_ms);
	ARG_UNUSED(volume);
	fake_buzzes++;
}

/* Audio thread */

int fake_audio_start_fail; /* the next N mg_audio_start_live() calls fail (busy) */

int mg_audio_start_live(uint8_t codec, uint16_t rate, uint16_t kbps, bool ptt)
{
	if (fake_audio_start_fail > 0) {
		fake_audio_start_fail--;
		return -EBUSY;
	}
	fake_audio_starts++;
	fake_audio_codec = codec;
	fake_audio_rate = rate;
	fake_audio_kbps = kbps;
	fake_audio_ptt = ptt;
	fake_audio_state = MG_AUDIO_LIVE;
	return 0;
}

int mg_audio_start_recording(uint8_t codec, uint16_t kbps)
{
	fake_audio_recordings++;
	fake_audio_codec = codec;
	fake_audio_rate = MG_AUDIO_DEFAULT_SAMPLE_RATE_HZ;
	fake_audio_kbps = kbps;
	fake_audio_state = MG_AUDIO_RECORDING;
	return 0;
}

int mg_audio_read_clip(uint16_t index)
{
	fake_audio_clip_reads++;
	fake_audio_clip = index;
	fake_audio_state = MG_AUDIO_CLIP;
	return 0;
}

int fake_audio_commits;
int fake_audio_discards;

int mg_audio_start_held(enum mg_audio_activity what, uint8_t codec, uint16_t rate, uint16_t kbps)
{
	if (what == MG_AUDIO_RECORDING) {
		int err = mg_audio_start_recording(codec, kbps);

		fake_audio_rate = rate; /* what the core asked for */
		return err;
	}
	return mg_audio_start_live(codec, rate, kbps, true);
}

void mg_audio_commit(void)
{
	fake_audio_commits++;
}

void mg_audio_discard(void)
{
	fake_audio_discards++;
	fake_audio_state = MG_AUDIO_IDLE;
}

void mg_audio_stop(void)
{
	fake_audio_stops++;
	fake_audio_state = MG_AUDIO_IDLE;
}

enum mg_audio_activity mg_audio_activity(void)
{
	return fake_audio_state;
}

void mg_audio_current_format(struct mg_audio_format *fmt)
{
	(void)mg_codec_format(mg_settings_get()->audio_codec, fmt);
}

void mg_audio_set_gain(uint8_t gain)
{
	fake_audio_gain = gain;
}

/* The output gate: the fake holds nothing; it counts what the core asks
 * and passes gestures straight through (held ones are the audio thread's). */
int fake_audio_gated;
int fake_audio_turns;
int fake_audio_gate_fails;
int fake_audio_gate_drops;

int mg_audio_start_live_gated(uint8_t codec, uint16_t rate, uint16_t kbps)
{
	fake_audio_gated++;
	return mg_audio_start_live(codec, rate, kbps, false);
}

int mg_audio_start_turn_held(void)
{
	fake_audio_turns++;
	return mg_audio_start_live(mg_data_type_audio_sbc, MG_AUDIO_DEFAULT_SAMPLE_RATE_HZ, 0,
				   true);
}

void mg_audio_out_control(const uint8_t *buf, size_t len)
{
	(void)mg_session_send_control(buf, len);
}

void mg_audio_gate_fail(void)
{
	fake_audio_gate_fails++;
}

void mg_audio_gate_drop(void)
{
	fake_audio_gate_drops++;
}

bool mg_audio_output_held(void)
{
	return false;
}

void mg_audio_gate_stats(struct mg_audio_gate_stats *st)
{
	memset(st, 0, sizeof(*st));
}

/* One-time bring-up shared by the suites. */
#include <zephyr/settings/settings.h>

#include "mg_core.h"
#include "mg_identity.h"
#include "mg_queue.h"
#include "mg_setup.h"
#include "mg_setup_store.h"

void fake_init_once(void)
{
	static bool done;

	if (done) {
		return;
	}
	done = true;
	(void)settings_subsys_init();
	(void)mg_settings_load();
	(void)mg_setup_store_load();
	(void)mg_queue_init();
	mg_core_init();
	static const uint8_t addr[6] = {0x02, 0, 0, 0, 0, 0x01};

	mg_identity_init("MuseGadget-", "", addr);
	mg_setup_init();
	mg_session_init();
}
