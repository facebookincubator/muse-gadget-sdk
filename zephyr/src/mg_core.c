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

#include "mg_ota_policy.h"
#include "mg_ota.h"
#include "mg_owner_reads.h"
#include "mg_mic_policy.h"
#include "mg_settings_policy.h"
#include "mg_nus_policy.h"
#include "mg_evt_policy.h"
#include "mg_core.h"
#include "mg_button.h"
#include "mg_mic_gain.h"

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>

#include "mg_audio.h"
#include "mg_app.h"
#include "mg_boot.h"
#include "mg_clock.h"
#include "mg_connparam.h"
#include "mg_power.h"
#include "mg_led.h"
#include "mg_session.h"
#include "mg_settings.h"
#include "mg_setup.h"
#include "mg_setup_store.h"
#include "mg_throughput.h"
#include "mg_token_proof.h"
#include "mg_transport.h"
#if defined(CONFIG_MG_VM_SESSION)
#include "mg_vm_session.h"
#endif

/* An injected press stands in for the button: automation builds, and bench
 * mode on any build. */
#define MG_INJECTED_COUNTS (IS_ENABLED(CONFIG_MG_AUTOMATION) || IS_ENABLED(CONFIG_MG_BENCH_MODE))
#if defined(CONFIG_MG_AUDIO_QUEUE)
#include "mg_queue.h"
#endif

LOG_MODULE_REGISTER(mg_core, CONFIG_MG_LOG_LEVEL);

static struct {
	bool connected;
	bool ready;
	bool button;
	bool button_injected; /* the press now down was injected (a bench word), not the GPIO */
	bool update_press; /* the press now down answered the update prompt */
	/* gesture button_down went out for the current press. */
	bool down_sent;
	struct mg_rate status_rate; /* request_status answered this second */
	int64_t press_ms;   /* the last real press (mg_press_window_open()) */
	uint32_t press_gen; /* ... on this connection */
	/* Push-to-talk capture is running for the current press. */
	bool ptt_live;
	bool recording;
	uint8_t mic_gain;
	/* A live capture for this client is running: its end is a turn for the assistant. */
	bool turn;
	/* The client sent assistant_state on this connection. */
	bool reports_state;
	/* A client's session has been up since boot. */
	bool ever_ready;
	enum mg_led_link link;
	/* The hold contract: where this press goes, picked at the press,
	 * and whether it passed the hold threshold. */
	enum { ROUTE_NONE, ROUTE_LIVE, ROUTE_OFFLINE, ROUTE_HELD } route;
	bool held;
	/* The bench switch: this connection gets mic output without a
	 * proof (the USB console's >bench=1 or the Nordic UART's bench 1);
	 * off at every connect, disconnect and boot. */
	bool bench;
	uint32_t start_refused; /* start_mic answered proof_required */
	uint32_t privileged_refused; /* proof_required for a privileged command */
} c;

/* ---- Mic output only to a proven central ---- */

bool mg_core_clips_ready(void)
{
	/* Closed while an owner change's clips wait to be erased (a failed
	 * erase, or the boot's catch-up not run yet). */
#if defined(CONFIG_MG_AUDIO_QUEUE)
	return mg_queue_ready() && !mg_setup_store_clips_due();
#else
	return false; /* no clip store on this board */
#endif
}

bool mg_core_output_ok(void)
{
	return c.connected && (c.bench || mg_token_proof_matched());
}

bool mg_core_gate_open(enum mg_audio_gate g)
{
	switch (g) {
	case MG_AUDIO_GATE_PROOF:
		return mg_core_output_ok();
	case MG_AUDIO_GATE_TURN:
		/* A held live turn goes live once the client is proven and
		 * ready (subscribed, request_status, push-to-talk on). */
		return mg_core_output_ok() && c.ready && mg_settings_get()->push_to_talk_enabled;
	default:
		return true;
	}
}

/* A wake word the gadget heard itself (a board's detector): counts as a press
 * for the mic's press gate. */
void mg_core_wake_word(void)
{
	c.press_ms = k_uptime_get();
}

#if defined(CONFIG_MG_DFU) && defined(CONFIG_MG_UPDATE_KEY)
/* A board's dedicated update key answers the update prompt with one press,
 * like the talk button (no hold, nothing pre-armed). A board with one calls
 * mg_core_update_key() from its key's input callback. */
void mg_core_update_key(bool pressed, bool real)
{
	if (pressed) {
		(void)mg_ota_prompt_take(real, MG_INJECTED_COUNTS);
	}
}
#endif

#if defined(CONFIG_MG_AUTOMATION)
bool mg_core_press_window_open(void)
{
	return mg_press_window_open(c.press_ms, c.press_gen, k_uptime_get(), mg_transport_conn_gen());
}
#endif

/* A release build's lock on the wireless bench switch, a symbol of its own so
 * the post-link check finds the refusal itself, whatever the log level. */
__attribute__((noinline)) bool mg_core_ble_bench_locked(bool wired)
{
	return !mg_bench_switch_allowed(IS_ENABLED(CONFIG_MG_AUTOMATION), mg_setup_store_complete(),
					wired, IS_ENABLED(CONFIG_MG_BENCH_MODE),
					mg_token_proof_matched());
}

int mg_core_set_bench(bool on, const char *who, bool wired)
{
	if (on && !c.connected) {
		return -ENOTCONN;
	}
	if (on && mg_health_safe()) {
		return -EPERM; /* no mic in SAFE, bench or not */
	}
	if (on && !mg_bench_ble_allowed(wired, IS_ENABLED(CONFIG_ARCH_POSIX), mg_token_proof_matched(),
					IS_ENABLED(CONFIG_MG_BENCH_MODE))) {
		LOG_WRN("mg.ble: bench switch refused (%s): needs this connection's token proof",
			who);
		return -EACCES;
	}
	if (on && mg_core_ble_bench_locked(wired)) {
		LOG_WRN("mg.ble: bench switch refused (%s): a set-up gadget in a release build "
			"gives mic output only to a token-proven central", who);
		return -EPERM;
	}
	if (c.bench != on) {
		c.bench = on;
		LOG_WRN("mg.ble: bench switch %s for this connection (%s): mic output %s",
			on ? "on" : "off", who,
			on ? "without a token proof" : "needs the token proof again");
		mg_led_set(MG_LED_BENCH, on);
		if (!on) {
			mg_core_output_revoked();
		}
	}
	return 0;
}

bool mg_core_bench(void)
{
	return c.bench;
}

uint32_t mg_core_start_refused(void)
{
	return c.start_refused;
}

void mg_core_proof_result(bool matched)
{
	if (!matched) {
		/* A held capture is discarded unsent. */
		mg_audio_gate_fail();
	}
}

static void bench_off(void)
{
	if (c.bench) {
		c.bench = false;
		LOG_INF("mg.ble: bench switch off (the connection ended)");
		mg_led_set(MG_LED_BENCH, false);
	}
}

static struct k_work_delayable hold_work;

/* Ready only with a client connected, subscribed and push-to-talk on, as
 * on the ESP32 gadgets. */
/* After a live turn's button_up the capture runs on
 * CONFIG_MG_PTT_POSTROLL_MS (the word's last syllable), then the last frame
 * and stop_mic; the capture light stays until it stops. A turn the device
 * ends (stop_mic, a disconnect, not ready) stops at once. */
static struct k_work_delayable postroll_work;

static void postroll_fn(struct k_work *w)
{
	ARG_UNUSED(w);
	if (c.ptt_live) {
		c.ptt_live = false;
		/* The audio thread sends stop_mic after the last chunk. */
		mg_audio_stop();
	}
}

/* Ends a post-roll still running, at once (a new press, the device). */
static void postroll_end_now(void)
{
	if (k_work_delayable_is_pending(&postroll_work)) {
		(void)k_work_cancel_delayable(&postroll_work);
		postroll_fn(NULL);
	}
}

void mg_core_output_revoked(void)
{
	/* A disconnect ends its own output; a held capture waits for the gate. */
	if (!c.connected || mg_core_output_ok() || mg_audio_output_held()) {
		return;
	}
	enum mg_audio_activity a = mg_audio_activity();

	if (a == MG_AUDIO_LIVE || a == MG_AUDIO_CLIP) {
		LOG_INF("mg.ble: mic output no longer allowed on this connection: %s ended",
			a == MG_AUDIO_LIVE ? "capture" : "clip download");
		(void)k_work_cancel_delayable(&postroll_work);
		c.ptt_live = false;
		mg_audio_stop();
	}
}

void mg_core_proof_challenge(void)
{
	if (mg_settings_get()->push_to_talk_enabled) {
		mg_settings_client_ready_reset();
		LOG_INF("mg.ble: a new token proof challenge: push-to-talk off until the client "
			"enables it again");
		mg_core_link_refresh();
	}
}

void mg_core_link_refresh(void)
{
	enum mg_led_link l;

	if (c.connected && c.ready) {
		c.ever_ready = true;
		l = mg_settings_get()->push_to_talk_enabled ? MG_LED_LINK_READY : MG_LED_LINK_SESSION;
	} else if (c.connected) {
		l = MG_LED_LINK_CONNECTING;
	} else {
		l = c.ever_ready || mg_setup_store_complete() ? MG_LED_LINK_DISCONNECTED
							       : MG_LED_LINK_NEVER;
	}
	if (l != c.link) {
		LOG_INF("mg.ble: link %s -> %s", mg_led_link_name(c.link), mg_led_link_name(l));
		c.link = l;
	}
	mg_led_set_link(l);
}

#if defined(CONFIG_MG_ASSISTANT_STATE)
/* Clients that never send assistant_state: the turn the device started at
 * stop_mic (thinking) ends with done if nothing arrives in time. */
static struct k_work_delayable compat_work;

static void compat_fn(struct k_work *w)
{
	ARG_UNUSED(w);
	if (c.connected && !c.reports_state && mg_led_face() == MG_LED_FACE_THINKING) {
		mg_led_set_face(MG_LED_FACE_DONE, "client sent no assistant_state");
	}
}
#endif

static void compat_cancel(void)
{
#if defined(CONFIG_MG_ASSISTANT_STATE)
	(void)k_work_cancel_delayable(&compat_work);
#endif
}

static void on_assistant_state(const uint8_t *p, size_t len)
{
	if (len < 1) {
		mg_core_send_error(mg_command_assistant_state, mg_error_code_invalid_length, NULL);
		return;
	}
	switch (p[0]) {
	case mg_assistant_state_idle:
		mg_led_set_face(MG_LED_FACE_IDLE, NULL);
		break;
	case mg_assistant_state_thinking:
		mg_led_set_face(MG_LED_FACE_THINKING, NULL);
		break;
	case mg_assistant_state_responding:
		mg_led_set_face(MG_LED_FACE_RESPONDING, NULL);
		break;
	case mg_assistant_state_done:
		mg_led_set_face(MG_LED_FACE_DONE, NULL);
		break;
	case mg_assistant_state_error:
		mg_led_set_face(MG_LED_FACE_ERROR, NULL);
		break;
	default:
		mg_core_send_error(mg_command_assistant_state, mg_error_code_invalid_value, NULL);
		return;
	}
	/* The client reports the turn from now on: no compat fallback. No reply. */
	c.reports_state = true;
	compat_cancel();
}

static void send(const uint8_t *buf, size_t len)
{
	(void)mg_session_send_control(buf, len);
}

void mg_core_send_unsolicited(const uint8_t *buf, size_t len)
{
	send(buf, len);
}

#if defined(CONFIG_MG_BATTERY_STATUS)
void mg_core_battery_report(void)
{
	uint8_t out[MG_POWER_REPORT_LEN];

	if (c.connected) {
		send(out, mg_power_report(out));
	}
}
#endif

static size_t error_frame(uint8_t *buf, size_t cap, uint8_t cmd, uint16_t code, const char *log)
{
	size_t n = 0;

	buf[n++] = mg_command_error;
	buf[n++] = cmd;
	buf[n++] = mg_error_data_code;
	sys_put_le16(code, &buf[n]);
	n += 2;
	/* The optional log is cut to what the link's notification holds. */
	if (log != NULL && cap > n + 3) {
		size_t l = MIN(strlen(log), cap - n - 3);

		buf[n++] = mg_error_data_log;
		sys_put_le16(l, &buf[n]);
		n += 2;
		memcpy(&buf[n], log, l);
		n += l;
	}
	LOG_DBG("error cmd %u code %u", cmd, code);
	return n;
}

/* An error frame fits one notification on this link (the code itself
 * always does: 5 bytes under the smallest ATT MTU's 20). */
static size_t error_cap(size_t size)
{
	return MAX(MIN(size, mg_session_data_room()), (size_t)5);
}

void mg_core_send_error(uint8_t cmd, uint16_t code, const char *log)
{
	uint8_t buf[5 + 3 + 40];

	send(buf, error_frame(buf, error_cap(sizeof(buf)), cmd, code, log));
}

int mg_core_send_error_urgent(uint8_t cmd, uint16_t code, const char *log)
{
	uint8_t buf[5 + 3 + 40];

	return mg_session_send_urgent(buf, error_frame(buf, error_cap(sizeof(buf)), cmd, code,
						       log));
}

/* request_status's replies are built in one buffer: the longest is the
 * capture-rate list, 3 + codecs x (2 + 6 x MG_CODEC_MAX_RATES), or the
 * settings list, 3 + every listed setting (every reply fits). */
#define STATUS_BUF 48
BUILD_ASSERT(3 + 2 * (2 + 6 * MG_CODEC_MAX_RATES) <= STATUS_BUF, "capture-rate list");
BUILD_ASSERT(STATUS_BUF <= 244 /* MAX_ATT, mg_ble.c */, "a reply over one notification");

static unsigned int status_limited; /* request_status not answered (rate) */

void mg_core_send_status(void)
{
	struct mg_audio_format fmt;
	uint8_t buf[STATUS_BUF];
	size_t n;

	mg_audio_current_format(&fmt);
	n = mg_codec_change_data_type(&fmt, buf);
	send(buf, n);

	n = 0;
	buf[n++] = mg_command_supported_features;
	/* Only commands the device accepts; notify-only ones aren't listed. */
	buf[n++] = mg_command_start_mic;
	buf[n++] = mg_command_stop_mic;
	buf[n++] = mg_command_set_mic_gain;
	buf[n++] = mg_command_request_status;
	buf[n++] = mg_command_get_settings;
	buf[n++] = mg_command_set_settings;
#if defined(CONFIG_MG_AUDIO_QUEUE)
	/* What the build supports, never the boot's state: listed while a
	 * firmware update holds the storage, and
	 * when the store didn't mount (its commands answer busy, counted). */
	buf[n++] = mg_command_audio_queue;
#endif
	buf[n++] = mg_command_token_proof;
#if defined(CONFIG_MG_VM_SESSION)
	/* The device session; skills (mg_command_skill) are superseded by it. */
	buf[n++] = mg_command_session;
#endif
#if defined(CONFIG_MG_ASSISTANT_STATE)
	buf[n++] = mg_command_assistant_state;
#endif
	buf[n++] = mg_command_device_action;
	send(buf, n);

#if defined(CONFIG_MG_VM_SESSION)
	/* What the device session can do, after the command list: sealed
	 * credentials (no BLE bond needed), and the client refreshes the
	 * token pair since this gadget has no Wi-Fi to do it itself. */
	n = 0;
	buf[n++] = mg_command_supported_features;
	buf[n++] = mg_command_sub_feature;
	buf[n++] = mg_command_session;
	buf[n++] = mg_session_cap_sealed | mg_session_cap_client_refreshes |
		   (IS_ENABLED(CONFIG_MG_SESSION_SEALED_CLEAR) ? mg_session_cap_sealed_clear : 0);
	send(buf, n);
#endif

	n = 0;
	buf[n++] = mg_command_supported_features;
	buf[n++] = mg_command_sub_feature;
	buf[n++] = mg_command_start_mic;
	buf[n++] = mg_data_type_audio_sbc;
#if defined(CONFIG_MG_LC3)
	buf[n++] = mg_data_type_audio_lc3;
#endif
	send(buf, n);

	/* Capture rates per codec, 16000 first:
	 * [supported_features, sub_feature, change_data_type, (codec, count,
	 * count x (rate u16 Hz, min u16 kb/s, max u16 kb/s))...]. */
	{
		static const uint8_t codecs[] = {
			mg_data_type_audio_sbc,
#if defined(CONFIG_MG_LC3)
			mg_data_type_audio_lc3,
#endif
		};
		n = 0;
		buf[n++] = mg_command_supported_features;
		buf[n++] = mg_command_sub_feature;
		buf[n++] = mg_command_change_data_type;
		for (size_t i = 0; i < ARRAY_SIZE(codecs); i++) {
			uint16_t rates[MG_CODEC_MAX_RATES];
			size_t nr = mg_codec_rates(codecs[i], rates, ARRAY_SIZE(rates));

			buf[n++] = codecs[i];
			buf[n++] = (uint8_t)nr;
			for (size_t k = 0; k < nr; k++) {
				uint16_t lo = 0, hi = 0;

				(void)mg_codec_kbps_range(codecs[i], rates[k], &lo, &hi);
				sys_put_le16(rates[k], &buf[n]);
				sys_put_le16(lo, &buf[n + 2]);
				sys_put_le16(hi, &buf[n + 4]);
				n += 6;
			}
		}
		send(buf, n);
	}

	/* Device actions: every one this build implements. */
	n = 0;
	buf[n++] = mg_command_supported_features;
	buf[n++] = mg_command_sub_feature;
	buf[n++] = mg_command_device_action;
#if defined(CONFIG_MG_BATTERY_STATUS)
	buf[n++] = mg_device_action_request_battery_status;
#endif
#if defined(CONFIG_MG_POWER_OFF)
	buf[n++] = mg_device_action_power_off;
#endif
	buf[n++] = mg_device_action_throughput_test;
	buf[n++] = mg_device_action_set_unix_time;
	buf[n++] = mg_device_action_get_unix_time;
	send(buf, n);

	/* Every setting get_settings and set_settings accept. */
	n = 0;
	buf[n++] = mg_command_supported_features;
	buf[n++] = mg_command_sub_feature;
	buf[n++] = mg_command_set_settings;
	for (unsigned int s = 0; s <= 0xff; s++) {
		if (mg_settings_listed((uint8_t)s)) {
			if (n == sizeof(buf)) {
				mg_session_count_drop("the typed settings list");
				break;
			}
			buf[n++] = (uint8_t)s;
		}
	}
	send(buf, n);
}

/* mgcommands.h push-to-talk: a turn captures in setting 7's codec
 * (SBC by default for released apps; apps that decode LC3 set it), never in
 * the codec of the client's last start_mic (iOS can't decode LC3 on a turn). */
static uint8_t ptt_codec(void)
{
	return mg_settings_get()->audio_codec;
}

/* A turn captures at capture_sample_rate (13) where its codec lists
 * it (LC3), else 16000 (an SBC turn stays 16 kHz); the bitrate (14) is
 * clamped by mg_codec_format_cfg(). */
static uint16_t capture_rate(uint8_t codec, uint16_t want)
{
	return mg_codec_rate_supported(codec, want) ? want : MG_AUDIO_DEFAULT_SAMPLE_RATE_HZ;
}

#if defined(CONFIG_MG_AUDIO_QUEUE)
/* The gadget picks the clip format: SBC at 16 kHz, or LC3 at 16 kHz,
 * 16 kb/s with CONFIG_MG_CLIP_LC3_16KBPS (off by default). Never setting 7,
 * 13 or 14. */
#define CLIP_KBPS 16
static uint8_t clip_codec(void)
{
	return IS_ENABLED(CONFIG_MG_CLIP_LC3_16KBPS) ? mg_data_type_audio_lc3
						    : mg_data_type_audio_sbc;
}
#endif

static void buzz(void)
{
	const struct mg_settings *st = mg_settings_get();

	if (st->haptics_enabled) {
		mg_led_buzz(st->ptt_buzz_freq_hz, st->ptt_buzz_duration_ms,
			    st->ptt_buzz_volume_percent);
	}
}

static void on_start_mic(const uint8_t *p, size_t len)
{
	uint8_t codec = len >= 1 && p[0] != mg_data_type_unknown ? p[0]
								  : mg_settings_get()->audio_codec;
	/* Params 2 and 3: the rate (uint16 Hz, default 16000; one the
	 * codec doesn't list captures at 16000) and the bitrate (uint16 kb/s,
	 * default 32, clamped into the range). Never refused. */
	uint16_t rate = capture_rate(codec, len >= 3 ? sys_get_le16(&p[1])
						     : MG_AUDIO_DEFAULT_SAMPLE_RATE_HZ);
	uint16_t kbps = len >= 5 ? sys_get_le16(&p[3]) : MG_CAPTURE_DEFAULT_BITRATE_KBPS;

	if (!mg_settings_codec_supported(codec)) {
		mg_core_send_error(mg_command_start_mic, mg_error_code_invalid_value,
				   "codec not supported");
		return;
	}
	if (mg_audio_activity() == MG_AUDIO_RECORDING) {
		mg_core_send_error(mg_command_start_mic, mg_error_code_busy, "recording clip");
		return;
	}
	if (mg_tp_running()) {
		mg_core_send_error(mg_command_start_mic, mg_error_code_busy, "throughput test");
		return;
	}
	/* A held push-to-talk turn stays the press's until a client capture
	 * actually replaces it: a refused or failed start_mic leaves it, so the
	 * release still stops the mic. A release gadget that isn't set up
	 * holds no token, so no proof can match: proof_required (a bench
	 * connection aside, which release builds refuse). */
	if (!c.bench && mg_mic_start_needs_setup(!IS_ENABLED(CONFIG_MG_AUTOMATION) &&
					     !IS_ENABLED(CONFIG_MG_BENCH_MODE),
				     mg_setup_store_complete())) {
		c.start_refused++;
		mg_core_send_error(mg_command_start_mic, mg_error_code_proof_required,
				   "start_mic on a gadget that isn't set up");
		return;
	}
	if (!mg_core_output_ok() && !mg_token_proof_pending()) {
		c.start_refused++;
		mg_core_send_error(mg_command_start_mic, mg_error_code_proof_required,
				   "start_mic before the token proof");
		return;
	}
	if (!mg_mic_press_ok(!IS_ENABLED(CONFIG_MG_AUTOMATION), mg_setup_store_complete(),
			     c.press_ms, k_uptime_get())) {
		/* A release build of a set-up gadget opens the mic for a client
		 * only within 5 s of a press on the gadget. */
		c.start_refused++;
		mg_core_send_error(mg_command_start_mic, mg_error_code_press_required,
				   "start_mic needs a press on the gadget");
		return;
	}
	if (!mg_core_output_ok()) {
		/* Before the proof, only while a token-proof challenge is out on
		 * this connection (the app's own proof race; refused above
		 * otherwise), its output held until the proof matches. */
		if (mg_audio_start_live_gated(codec, rate, kbps)) {
			mg_core_send_error(mg_command_start_mic, mg_error_code_busy, NULL);
		} else {
			c.ptt_live = false;
			c.turn = true;
		}
		return;
	}
	/* Live audio wins over a clip download: starting abandons it. */
	if (mg_audio_start_live(codec, rate, kbps, false)) {
		mg_core_send_error(mg_command_start_mic, mg_error_code_busy, NULL);
	} else {
		c.ptt_live = false;
		c.turn = true;
	}
}

static void on_get_settings(const uint8_t *p, size_t len)
{
	static const uint8_t all[] = {
		mg_setting_parameter_spec_version,
		mg_setting_parameter_push_to_talk_enabled,
		mg_setting_parameter_haptics_enabled,
		mg_setting_parameter_ptt_buzz_freq_hz,
		mg_setting_parameter_ptt_buzz_duration_ms,
		mg_setting_parameter_ptt_buzz_volume_percent,
		mg_setting_parameter_audio_codec,
		mg_setting_parameter_audio_queue_enabled,
		mg_setting_parameter_capture_sample_rate,
		mg_setting_parameter_capture_bitrate_kbps,
		mg_setting_parameter_link_mode,
		mg_setting_parameter_link_params,
		mg_setting_parameter_client_name,
		mg_setting_parameter_client_app,
	};
	const struct mg_settings *st = mg_settings_get();
	/* link_params is 1 + 6; client_name 2 + 32, client_app 3 + 2 x 24. */
	uint8_t buf[1 + ARRAY_SIZE(all) * 7 + 34 + 51];

	BUILD_ASSERT(sizeof(buf) <= 244 /* MAX_ATT, mg_ble.c */,
		     "get_settings' longest answer must fit one notification");
	uint8_t mine[ARRAY_SIZE(all)];
	size_t n = 0;
	/* One notification on this link: a smaller ATT MTU holds less. */
	size_t cap = MIN(sizeof(buf), mg_session_data_room());

	if (len == 0) {
		/* No list: everything this device accepts. */
		len = 0;
		for (size_t i = 0; i < ARRAY_SIZE(all); i++) {
			if (mg_settings_listed(all[i])) {
				mine[len++] = all[i];
			}
		}
		p = mine;
	}
	buf[n++] = mg_command_get_settings;
	bool client_ok = MG_OWNER_READ("client");
	bool skipped = false;

	for (size_t i = 0; i < len; i++) {
		size_t w;

		if (!client_ok && (p[i] == mg_setting_parameter_client_name ||
				   p[i] == mg_setting_parameter_client_app)) {
			/* The phone's name and app wait for the proof on a release
			 * build: skipped, then proof_required once. */
			skipped = true;
			continue;
		}
		w = mg_settings_accepted(p[i])
			    ? mg_settings_encode(st, p[i], &buf[n], cap > n ? cap - n : 0)
			    : 0;

		if (w == 0) {
			if (!mg_settings_accepted(p[i])) {
				mg_core_send_error(mg_command_get_settings,
						   mg_error_code_unsupported, "unknown setting");
			} else {
				mg_core_send_error(mg_command_get_settings,
						   mg_error_code_invalid_length, "too many");
			}
			return;
		}
		n += w;
	}
	if (n > 1) {
		send(buf, n);
	}
	if (skipped) {
		mg_core_send_error(mg_command_get_settings, mg_error_code_proof_required, NULL);
	}
}

/* On a set-up gadget, commands that destroy or persist
 * something (clearing the clip queue, power off, a stored setting) need this
 * connection's token proof; a gadget that isn't set up has no proof key and
 * stays open, as before. Per-connection settings, reads and the mic's own
 * rules are unchanged. The bench switch doesn't count here. set_unix_time
 * always applies; its offset and flags are stored only when may_store(). */
static bool may_store(void)
{
	return !mg_setup_store_complete() || mg_token_proof_matched();
}

static bool privileged_ok(uint8_t cmd, const char *what)
{
	if (may_store()) {
		return true;
	}
	c.privileged_refused++;
	mg_core_send_error(cmd, mg_error_code_proof_required, what);
	return false;
}

uint32_t mg_core_privileged_refused(void)
{
	return c.privileged_refused;
}

bool mg_core_clips_visible(void)
{
	/* The owner's: on a set-up gadget only after this connection's token
	 * proof (the bench switch doesn't count). */
	return may_store();
}

static void on_set_settings(const uint8_t *p, size_t len)
{
	uint8_t bad = 0;

	if (len == 0) {
		mg_core_send_error(mg_command_set_settings, mg_error_code_invalid_length, NULL);
		return;
	}
	/* On a set-up gadget a central without this connection's token proof
	 * changes no persisted setting: those pairs are skipped, the others
	 * apply, then proof_required once (mg_settings_policy.h). */
	bool skipped = false;
	uint16_t err = mg_settings_apply_ex(
		p, len, &bad, mg_settings_skip_persisted(mg_setup_store_complete(),
							 mg_token_proof_matched()),
		&skipped);

	if (skipped) {
		(void)privileged_ok(mg_command_set_settings, "stored setting before the token proof");
	}

	if (!mg_settings_get()->push_to_talk_enabled && c.ptt_live) {
		/* Push-to-talk turned off mid-hold revokes the turn: before the
		 * hold threshold the pre-roll is discarded and the turn never
		 * starts; after it the capture ends at once, as a client's
		 * stop_mic would. */
		(void)k_work_cancel_delayable(&postroll_work);
		c.ptt_live = false;
		if (!c.held) {
			LOG_INF("push-to-talk off before the hold threshold: the pre-roll is dropped");
			mg_audio_discard();
			if (c.route == ROUTE_HELD) {
				/* A held turn exists only for push-to-talk: no button_down
				 * and no turn at the threshold either. */
				(void)k_work_cancel_delayable(&hold_work);
				c.route = ROUTE_NONE;
			}
		} else {
			LOG_INF("push-to-talk off mid-turn: the capture stops");
			mg_audio_stop();
		}
	}
	mg_core_link_refresh(); /* push-to-talk on or off */
	if (err) {
		char log[24];

		snprintk(log, sizeof(log), "setting %u", bad);
		mg_core_send_error(mg_command_set_settings, err, log);
	}
}

#if defined(CONFIG_MG_AUDIO_QUEUE)
static void send_queue_status(void)
{
	struct mg_queue_status st;
	uint8_t buf[2 + 1 + 4 + 4 + 2];

	mg_queue_get_status(&st);
	if (!mg_core_clips_visible()) {
		st.used_bytes = 0;
		st.clip_count = 0;
	}
	buf[0] = mg_command_audio_queue;
	buf[1] = mg_audio_queue_command_status;
	/* Off while a firmware update holds the storage (capacity 0 then too). */
	buf[2] = mg_settings_get()->audio_queue_enabled && mg_core_clips_ready();
	sys_put_le32(st.used_bytes, &buf[3]);
	sys_put_le32(st.capacity_bytes, &buf[7]);
	sys_put_le16(st.clip_count, &buf[11]);
	send(buf, sizeof(buf));
}

static void on_audio_queue(const uint8_t *p, size_t len)
{
	struct mg_clip_info ci;

	if (!mg_queue_present()) {
		/* Listed by the build: busy while the store isn't mounted. */
		static uint32_t unmounted;

		if (unmounted++ % 100 == 0) {
			LOG_WRN("audio_queue with the clip store not mounted: busy (%u)", unmounted);
		}
		mg_core_send_error(mg_command_audio_queue, mg_error_code_busy, "no storage");
		return;
	}
	if (len < 1) {
		mg_core_send_error(mg_command_audio_queue, mg_error_code_invalid_length, NULL);
		return;
	}
	if (!mg_core_clips_ready() && p[0] != mg_audio_queue_command_status) {
		mg_core_send_error(mg_command_audio_queue, mg_error_code_busy,
				   mg_queue_ready() ? "clips being erased" : "firmware update");
		return;
	}
	switch (p[0]) {
	case mg_audio_queue_command_status:
		send_queue_status();
		break;
	case mg_audio_queue_command_clear:
		if (!privileged_ok(mg_command_audio_queue, "clear before the token proof")) {
			return;
		}
		if (mg_audio_activity() == MG_AUDIO_RECORDING) {
			mg_core_send_error(mg_command_audio_queue, mg_error_code_busy, NULL);
			return;
		}
		if (mg_audio_activity() == MG_AUDIO_CLIP) {
			mg_audio_stop();
		}
		if (mg_queue_clear()) {
			mg_core_send_error(mg_command_audio_queue, mg_error_code_busy, "erase failed");
			return;
		}
		send_queue_status();
		break;
	case mg_audio_queue_command_clip_info:
	case mg_audio_queue_command_read_clip: {
		if (len < 3) {
			mg_core_send_error(mg_command_audio_queue, mg_error_code_invalid_length,
					   NULL);
			return;
		}
		uint16_t idx = sys_get_le16(&p[1]);

		/* Before the lookup, so an unproven central learns nothing. */
		if (p[0] == mg_audio_queue_command_clip_info ? !mg_core_clips_visible()
							     : !mg_core_output_ok()) {
			mg_core_send_error(mg_command_audio_queue, mg_error_code_proof_required,
					   p[0] == mg_audio_queue_command_clip_info
						   ? "clip_info before the token proof"
						   : "read_clip before the token proof");
			return;
		}
		if (mg_queue_clip_info(idx, &ci)) {
			mg_core_send_error(mg_command_audio_queue, mg_error_code_invalid_value,
					   "no such clip");
			return;
		}
		if (p[0] == mg_audio_queue_command_clip_info) {
			/* The clip's format follows (rate,
			 * channels, frame bytes, duration); a clip stored before
			 * the format was kept reads as its codec's default. */
			uint8_t buf[2 + 2 + 1 + 4 + 4 + 2 + 1 + 2 + 2 + 8];
			struct mg_audio_format def;

			if (mg_codec_format(ci.codec, &def)) {
				def = (struct mg_audio_format){.sample_rate = 16000, .channels = 1};
			}
			buf[0] = mg_command_audio_queue;
			buf[1] = mg_audio_queue_command_clip_info;
			sys_put_le16(idx, &buf[2]);
			buf[4] = ci.codec;
			sys_put_le32(ci.start_ms, &buf[5]);
			sys_put_le32(ci.bytes, &buf[9]);
			sys_put_le16(ci.rate ? ci.rate : def.sample_rate, &buf[13]);
			buf[15] = ci.channels ? ci.channels : def.channels;
			sys_put_le16(ci.frame_bytes ? ci.frame_bytes : def.frame_bytes, &buf[16]);
			sys_put_le16(ci.frame_us ? ci.frame_us : def.frame_us, &buf[18]);
			/* start_unix_ms (uint64), 0 when the clock wasn't set. */
			sys_put_le64((uint64_t)ci.unix_s * 1000U, &buf[20]);
			send(buf, sizeof(buf));
			return;
		}
		enum mg_audio_activity a = mg_audio_activity();

		if (a == MG_AUDIO_LIVE || a == MG_AUDIO_RECORDING) {
			mg_core_send_error(mg_command_audio_queue, mg_error_code_busy, "live audio");
			return;
		}
		if (mg_audio_read_clip(idx)) {
			mg_core_send_error(mg_command_audio_queue, mg_error_code_busy, NULL);
		}
		break;
	}
	default:
		mg_core_send_error(mg_command_audio_queue, mg_error_code_unsupported, NULL);
		break;
	}
}
#endif

static void on_device_action(const uint8_t *p, size_t len)
{
	if (len < 1) {
		mg_core_send_error(mg_command_device_action, mg_error_code_invalid_length, NULL);
		return;
	}
	switch (p[0]) {
	case mg_device_action_throughput_test:
		/* Open on every build, proof or not (mgcommands.h, Token proof:
		 * the display and test commands stay open). */
		mg_tp_command(&p[1], len - 1);
		break;
#if defined(CONFIG_MG_BATTERY_STATUS)
	case mg_device_action_request_battery_status: {
		uint8_t out[MG_POWER_REPORT_LEN];

		send(out, mg_power_report(out));
		break;
	}
#endif
#if defined(CONFIG_MG_POWER_OFF)
	case mg_device_action_power_off:
		if (!privileged_ok(mg_command_device_action, "power off before the token proof")) {
			break;
		}
		/* No reply: the link drops as the gadget powers off. */
		mg_app_power_off("the app asked");
		break;
#endif
	case mg_device_action_set_unix_time:
	case mg_device_action_get_unix_time: {
		uint8_t out[MG_CLOCK_REPORT_LEN];
		size_t n;

		if (p[0] == mg_device_action_get_unix_time && !MG_OWNER_READ("clock")) {
			/* The owner's local time and offset: proven only on a
			 * release build. */
			mg_core_send_error(mg_command_device_action, mg_error_code_proof_required,
					   NULL);
			break;
		}
		uint16_t err = mg_clock_action(p, len, may_store(), out, &n);

		if (err) {
			mg_core_send_error(mg_command_device_action, err, NULL);
		} else if (n) {
			send(out, n);
		}
		break;
	}
	default:
		mg_core_send_error(mg_command_device_action, mg_error_code_unsupported, NULL);
		break;
	}
}

void mg_core_control_rx(const uint8_t *buf, size_t len)
{
	if (len == 0) {
		return;
	}
	const uint8_t *p = &buf[1];
	size_t plen = len - 1;

	if (mg_health_safe()) {
		/* SAFE mode declares itself (mgcommands.h, Recovery mode):
		 * [supported_features, sub_feature, request_status, recovery,
		 * reason, flags] before the command list, reason user when the
		 * boot hold asked for it (its SMP takes image updates) and
		 * boot_loop when it came by itself (on a set-up gadget its SMP
		 * refuses image writes, rc 11); no reboot action, so no restart
		 * flag. Everything else answers recovery; firmware updates go
		 * over SMP. */
		if (buf[0] == mg_command_request_status) {
			const uint8_t d[] = {mg_command_supported_features, mg_command_sub_feature,
					     mg_command_request_status, mg_device_mode_recovery,
					     mg_health_safe_bench()       ? mg_recovery_reason_bench
					     : mg_health_safe_by_button() ? mg_recovery_reason_user
									  : mg_recovery_reason_boot_loop,
					     0};
			const uint8_t f[] = {mg_command_supported_features,
					     mg_command_request_status};

			send(d, sizeof(d));
			send(f, sizeof(f));
		} else {
			mg_core_send_error(buf[0], mg_error_code_recovery, "safe mode");
		}
		return;
	}
	switch (buf[0]) {
	case mg_command_request_status:
		/* It needs no proof to ask, so it is bounded: at most
		 * MG_STATUS_PER_WINDOW a second per connection (mg_evt_policy.h). */
		if (!mg_rate_take(&c.status_rate, k_uptime_get())) {
			status_limited++;
			if (status_limited == 1 || status_limited % 100 == 0) {
				LOG_WRN("request_status over %d a second: not answered (%u so far)",
					MG_STATUS_PER_WINDOW, status_limited);
			}
			break;
		}
		mg_core_send_status();
		mg_session_status_requested(); /* after its replies: ready now */
		break;
	case mg_command_start_mic:
		on_start_mic(p, plen);
		break;
	case mg_command_stop_mic:
		if (mg_audio_activity() == MG_AUDIO_LIVE) {
			(void)k_work_cancel_delayable(&postroll_work);
			c.ptt_live = false;
			mg_audio_stop();
		}
		break;
	case mg_command_set_mic_gain:
		if (plen < 1) {
			mg_core_send_error(buf[0], mg_error_code_invalid_length, NULL);
		} else if (!mg_mic_gain_level_ok(p[0])) {
			mg_core_send_error(buf[0], mg_error_code_invalid_value, NULL);
		} else {
			c.mic_gain = p[0];
			mg_audio_set_gain(p[0]);
		}
		break;
	case mg_command_get_settings:
		on_get_settings(p, plen);
		break;
	case mg_command_set_settings:
		on_set_settings(p, plen);
		break;
#if defined(CONFIG_MG_AUDIO_QUEUE)
	case mg_command_audio_queue:
		on_audio_queue(p, plen);
		break;
#endif
	case mg_command_token_proof:
		mg_token_proof_rx(p, plen);
		break;
#if defined(CONFIG_MG_VM_SESSION)
	case mg_command_session:
		mg_vm_session_rx(p, plen);
		break;
#endif
	case mg_command_device_action:
		on_device_action(p, plen);
		break;
	case mg_command_change_data_type:
		/* The Data channel's selector: it always switches, silently. Only
		 * the receive throughput test takes a client's Data here; any other
		 * type's Data is dropped (mgcommands.h). */
		if (plen < 1) {
			mg_core_send_error(buf[0], mg_error_code_invalid_length, NULL);
		} else {
			mg_tp_change_data_type(p[0]);
		}
		break;
#if defined(CONFIG_MG_ASSISTANT_STATE)
	case mg_command_assistant_state:
		on_assistant_state(p, plen);
		break;
#endif
	default:
		mg_core_send_error(buf[0], mg_error_code_unsupported, NULL);
		break;
	}
}

/* A press's gestures go through the audio thread's output gate:
 * held with a held capture's audio, in order, else sent at once. */
static void send_gesture(uint8_t g)
{
	const uint8_t buf[] = {mg_command_gesture, g};

	mg_audio_out_control(buf, sizeof(buf));
}

/*
 * The hold contract (mgcommands.h, Pre-roll):
 * capture starts at key-down (pre-roll, held back, the LED on at once);
 * button_down goes out only once the button has been held
 * CONFIG_MG_HOLD_MS (250 ms), then change_data_type, the pre-roll as a burst
 * and live audio. A tap (released sooner) sends nothing, keeps nothing and
 * shows nothing more than the light while it was held.
 */
static void hold_fn(struct k_work *w)
{
	ARG_UNUSED(w);
	if (!c.button || c.held) {
		return;
	}
	c.held = true;
	if (c.route == ROUTE_HELD) {
		/* The button_down and the audio wait, in order,
		 * until the client is proven and ready (or the clip queue takes
		 * the turn at its deadline). */
		send_gesture(mg_gesture_button_down);
		c.down_sent = true;
		buzz(); /* at the threshold */
		mg_audio_commit();
		c.turn = true;
	} else if (c.route == ROUTE_LIVE) {
		if (!c.ready) {
			/* The client went away during the pre-roll. */
			if (c.ptt_live) {
				c.ptt_live = false;
				mg_audio_discard();
			}
			c.route = ROUTE_NONE;
			return;
		}
		send_gesture(mg_gesture_button_down);
		c.down_sent = true;
		if (c.ptt_live) {
			buzz(); /* at the threshold */
			mg_audio_commit();
			c.turn = true;
		}
	} else if (c.route == ROUTE_OFFLINE && c.recording) {
		mg_audio_commit();
	}
}

/* A talk press while a client's link is up but the client
 * isn't proven or ready yet: a held live turn, if the board can hold it. A
 * gadget that isn't set up has no proven central (and no turn). */
static bool route_held(void)
{
	if (!mg_setup_store_complete() && !c.bench) {
		return false;
	}
	if (mg_tp_running()) {
		return false;
	}
	int err = mg_audio_start_turn_held();

	if (err) {
		LOG_WRN("held live turn not possible (%d)", err);
		return false;
	}
	LOG_INF("held live turn: the client isn't proven and ready yet; held up to %d ms",
		MG_AUDIO_HELD_TURN_MS);
	c.route = ROUTE_HELD;
	c.ptt_live = true;
	return true;
}

void mg_core_button(bool pressed)
{
	mg_core_button_ev(pressed, k_uptime_get(), true, true);
}

#if defined(CONFIG_ZTEST)
/* mg_owner_reads.h's test seam: the release rule, starting as the build's. */
bool mg_test_owner_release = !IS_ENABLED(CONFIG_MG_AUTOMATION) && !IS_ENABLED(CONFIG_MG_BENCH_MODE);
#endif

__weak bool mg_board_press_take(bool real, bool injected_counts)
{
	ARG_UNUSED(real);
	ARG_UNUSED(injected_counts);
	return false;
}

void mg_core_button_from(bool pressed, int64_t at_ms, bool real)
{
	mg_core_button_ev(pressed, at_ms, real, true);
}

static void button_at(bool pressed, int64_t at_ms, bool real, bool after_arm);

#if defined(CONFIG_MG_TAP_TO_TALK)
/* Tap to talk (CONFIG_MG_TAP_TO_TALK): the physical button's events mapped
 * onto the hold contract's. A press goes through as a press; its release is
 * kept back while the press started a turn (latched), and the next press
 * releases it instead (and is swallowed with its own release). */
static struct {
	bool latched; /* a turn is on with the button logically held */
	bool swallow; /* the press that ended it: its release is ignored */
} tap;
static struct k_work_delayable tap_max_work;

static void tap_end(int64_t at_ms, const char *why)
{
	tap.latched = false;
	(void)k_work_cancel_delayable(&tap_max_work);
	LOG_INF("tap to talk: turn ended (%s)", why);
	button_at(false, at_ms, true, true);
}

static void tap_max_fn(struct k_work *w)
{
	ARG_UNUSED(w);
	if (tap.latched) {
		tap_end(k_uptime_get(), "the longest turn");
	}
}

/* No latched turn or swallowed release is left (a press about to be
 * released by hand). */
static void tap_drop(void)
{
	tap.latched = false;
	tap.swallow = false;
	(void)k_work_cancel_delayable(&tap_max_work);
}

/* The turn is gone without the button (a disconnect, the client's
 * stop_mic): the logically held button is let go quietly, so the next press
 * starts a new turn. */
static void tap_reset(void)
{
	if (tap.latched) {
		c.button = false;
		c.held = false;
		c.route = ROUTE_NONE;
		c.down_sent = false;
	}
	tap_drop();
}

void mg_core_button_ev(bool pressed, int64_t at_ms, bool real, bool after_arm)
{
	if (pressed) {
		if (tap.latched) {
			tap.swallow = true;
			tap_end(at_ms, "the next press");
			return;
		}
		tap.swallow = false;
		button_at(true, at_ms, real, after_arm);
		return;
	}
	if (tap.swallow) {
		tap.swallow = false;
		return;
	}
	if (c.button && c.route != ROUTE_NONE) {
		/* The press started a turn: it stays on until the next press. */
		tap.latched = true;
		k_work_reschedule_for_queue(mg_app_wq(), &tap_max_work,
					    K_MSEC(CONFIG_MG_TAP_TO_TALK_MAX_MS));
		LOG_INF("tap to talk: turn on; the next press ends it");
		return;
	}
	button_at(false, at_ms, real, after_arm);
}
#else
static void tap_reset(void)
{
}

void mg_core_button_ev(bool pressed, int64_t at_ms, bool real, bool after_arm)
{
	button_at(pressed, at_ms, real, after_arm);
}
#endif

static void button_at(bool pressed, int64_t at_ms, bool real, bool after_arm)
{
	ARG_UNUSED(at_ms); /* a setup confirm takes the press whenever it began */
	/* The release of a press that answered the update prompt is swallowed,
	 * before anything compares button states (it once left a latch that ate
	 * the next turn's release). */
	if (!pressed && c.update_press) {
		c.update_press = false;
		return;
	}
	if (pressed == c.button) {
		return;
	}
	/* One press answers the update prompt (ours, or a board's own), in
	 * SAFE too. Only a press queued after the prompt was armed counts
	 * (after_arm, mg_button.c), and only while the LED shows the update cue:
	 * a pending setup confirm takes the press first, so a setup-prompt press
	 * never approves a hidden update. */
	if (pressed && after_arm && !mg_setup_confirm_pending() && mg_led_update_cue_showing() && (
#if defined(CONFIG_MG_DFU)
			       mg_ota_prompt_take(real, MG_INJECTED_COUNTS) ||
#endif
			       mg_board_press_take(real, MG_INJECTED_COUNTS))) {
		LOG_INF("press confirmed the firmware update");
		c.update_press = true;
		return;
	}
	if (mg_health_safe()) {
		LOG_INF("button %s ignored: SAFE mode", pressed ? "press" : "release");
		return;
	}
	c.button = pressed;
	c.button_injected = pressed && !real;
	if (pressed && (real || IS_ENABLED(CONFIG_MG_BENCH_MODE))) {
		/* A bench-mode image takes an injected press as the button's (the
		 * mic's press window). */
		c.press_ms = k_uptime_get();
		c.press_gen = mg_transport_conn_gen(); /* 0 with no link: opens nothing */
	}
	if (pressed) {
		/* Link modes: a user's press moves sleep to ready first. */
		mg_connparam_user_wake();
	}
	if (!pressed) {
		/* The press's activity ends at its release (a capture holds
		 * active on its own until it stops). */
		mg_connparam_busy(MG_XFER_PRESS);
	}

	if (pressed) {
		postroll_end_now(); /* the last turn's post-roll gives way */
		if (mg_setup_button(real)) {
			/* Link setup took it (one press confirms, or a press
			 * before its prompt is up); the release is swallowed too. */
			c.button = false;
			return;
		}
		c.held = false;
		c.route = ROUTE_NONE;
		c.down_sent = false;
		mg_connparam_busy(MG_XFER_PRESS); /* a talk press */
#if defined(CONFIG_MG_VM_SESSION)
		mg_vm_session_user_action(); /* a reason to be connected */
#endif
		if (!c.connected) {
			/* A press with no app connected advertises fast again,
			 * so the phone picks the gadget up at once. */
			mg_transport_adv_boost();
		}
		bool ptt = mg_settings_get()->push_to_talk_enabled;

		if (c.connected && c.ready && ptt && !mg_core_output_ok()) {
			/* The client is ready but not proven yet
			 * (the connect burst): a held live turn. */
			if (route_held()) {
				goto routed;
			}
		}
		if (c.ready && (mg_core_output_ok() || !ptt)) {
			/* Proven (or the bench switch), or gestures only (push-to-talk
			 * off: no mic output). */
			c.route = ROUTE_LIVE;
			if (mg_settings_get()->push_to_talk_enabled && mg_tp_running()) {
				LOG_WRN("push-to-talk skipped: a throughput test is running");
			} else if (mg_settings_get()->push_to_talk_enabled) {
				uint8_t pc = ptt_codec();

				const struct mg_settings *st = mg_settings_get();

				c.ptt_live = mg_audio_start_held(MG_AUDIO_LIVE, pc,
								 capture_rate(pc, st->capture_sample_rate),
								 st->capture_bitrate_kbps) == 0;
			}
		} else if (c.connected && route_held()) {
			/* The connect burst (not ready yet): a held live turn. */
		} else if (c.connected && !mg_setup_store_complete() && !c.bench) {
			/* Not set up, so no central can be proven: no mic
			 * output; the press goes nowhere ("Open Muse app"). */
			LOG_INF("press with a client connected but no setup: no mic output");
			mg_led_event(MG_LED_EVENT_NOT_KEPT);
			return;
		} else {
#if defined(CONFIG_MG_AUDIO_QUEUE)
			if (mg_settings_get()->audio_queue_enabled && !mg_core_clips_ready() &&
			    mg_queue_present()) {
				LOG_WRN("press not recorded: %s",
					mg_queue_ready() ? "the earlier owner's clips aren't erased yet"
							 : "a firmware update holds the clip store");
			} else if (mg_settings_get()->audio_queue_enabled && mg_core_clips_ready() &&
				   mg_audio_start_held(MG_AUDIO_RECORDING, clip_codec(),
						       MG_AUDIO_DEFAULT_SAMPLE_RATE_HZ, CLIP_KBPS) == 0) {
				/* No client, so no per-connection haptics: the LED blinks. */
				c.route = ROUTE_OFFLINE;
				c.recording = true;
			}
#endif
			if (c.route == ROUTE_NONE) {
				/* No route: the mic stays off; 4 blinks say so. */
				LOG_INF("press with nowhere to go: no app and no clip queue");
				mg_led_event(MG_LED_EVENT_NOT_KEPT);
				return;
			}
		}
routed:
		k_work_reschedule_for_queue(mg_app_wq(), &hold_work, K_MSEC(CONFIG_MG_HOLD_MS));
		return;
	}

	/* Released. */
	(void)k_work_cancel_delayable(&hold_work);
	if (!c.held) {
		/* A tap: nothing sent, nothing kept. */
		if (c.ptt_live || c.recording) {
			mg_audio_discard();
		}
		if (c.route != ROUTE_NONE) {
			LOG_INF("tap (under %d ms): nothing sent; hold to talk", CONFIG_MG_HOLD_MS);
		}
		/* (With tap to talk a turn's press never comes here released
		 * early: its release is latched.) */
		c.ptt_live = false;
		c.recording = false;
		c.route = ROUTE_NONE;
		return;
	}
	bool down_sent = c.down_sent;
	int route = c.route;

	c.held = false;
	c.route = ROUTE_NONE;
	c.down_sent = false;
	if (c.recording) {
		c.recording = false;
		mg_audio_stop();
	}
	if (down_sent && (c.ready || route == ROUTE_HELD)) {
		send_gesture(mg_gesture_button_up); /* held with the turn, or dropped if it's a clip */
	}
	if (c.ptt_live) {
		if (CONFIG_MG_PTT_POSTROLL_MS > 0) {
			k_work_reschedule_for_queue(mg_app_wq(), &postroll_work,
						    K_MSEC(CONFIG_MG_PTT_POSTROLL_MS));
		} else {
			postroll_fn(NULL);
		}
	}
}

void mg_core_audio_idle(void)
{
	/* Stale if something new was started after the audio thread went idle. */
	if (mg_audio_activity() != MG_AUDIO_IDLE) {
		return;
	}
	tap_reset(); /* the turn ended without the button (the client's stop_mic) */
	c.ptt_live = false;
	c.recording = false;
	if (c.turn) {
		/* stop_mic went out: the utterance is with the assistant. */
		c.turn = false;
#if defined(CONFIG_MG_ASSISTANT_STATE)
		if (mg_audio_last_turn_silent()) {
			/* Dropped unsent (nothing above room noise, a test build):
			 * no utterance, so no thinking; its cue already showed. */
			return;
		}
		if (c.ready) {
			/* The device shows thinking by itself (mgcommands.h); the
			 * client's assistant_state takes over. One that hasn't sent
			 * any on this connection gets done after a while instead. */
			mg_led_set_face(MG_LED_FACE_THINKING, "after stop_mic");
			if (!c.reports_state) {
				k_work_reschedule_for_queue(mg_app_wq(), &compat_work,
							    K_MSEC(CONFIG_MG_STATE_COMPAT_TIMEOUT_MS));
			}
		}
#endif
	}
}

/* set_mic_gain is per connection (mgcommands.h): the tuned gain again at
 * every connect and disconnect. */
static void mic_gain_tuned(void)
{
	c.mic_gain = CONFIG_MG_MIC_DEFAULT_GAIN;
	mg_audio_set_gain(c.mic_gain);
}

void mg_core_connected(void)
{
	c.connected = true;
	c.status_rate = (struct mg_rate){0};
	c.turn = false;
	c.reports_state = false;
	compat_cancel();
	bench_off();
	mg_token_proof_connection_reset();
#if defined(CONFIG_MG_VM_SESSION)
	mg_vm_session_connection_reset();
#endif
	mg_settings_new_connection();
	mic_gain_tuned();
	mg_core_link_refresh();
}

void mg_core_disconnected(void)
{
	/* An injected press (a bench-mode image's Nordic UART "talk down") ends
	 * with its link, as its release would: its central can't send "talk up"
	 * any more, and a bench unit with no hand on it would hold the press
	 * (and record offline) for good. A real press is untouched. */
	if (c.button && c.button_injected) {
		button_at(false, k_uptime_get(), false, true);
	}
	c.update_press = false; /* no release of it comes after the link */
#if defined(CONFIG_MG_DFU)
	mg_ota_link_ended(); /* the update prompt and its light end with the link */
#endif
	c.connected = false;
	mic_gain_tuned(); /* offline clips record at the tuned gain */
	c.ready = false;
	c.turn = false;
	c.reports_state = false;
	compat_cancel();
	tap_reset();
	mg_tp_reset();
	bench_off();
	mg_token_proof_connection_reset();
#if defined(CONFIG_MG_VM_SESSION)
	mg_vm_session_connection_reset();
#endif
	mg_settings_new_connection();
	if (mg_led_face() != MG_LED_FACE_IDLE) {
		mg_led_set_face(MG_LED_FACE_IDLE, "disconnected");
	}
	mg_core_link_refresh();
	enum mg_audio_activity a = mg_audio_activity();

	if (mg_audio_output_held()) {
		/* A held turn goes on into the clip queue (a held
		 * start_mic capture is discarded); the press still runs it. */
		mg_audio_gate_drop();
		if (c.route != ROUTE_HELD && a == MG_AUDIO_LIVE) {
			(void)k_work_cancel_delayable(&postroll_work);
			c.ptt_live = false;
			mg_audio_stop();
		}
		return;
	}
	(void)k_work_cancel_delayable(&postroll_work);
	if (a == MG_AUDIO_LIVE || a == MG_AUDIO_CLIP) {
		c.ptt_live = false;
		mg_audio_stop();
	}
}

void mg_core_set_ready(bool ready)
{
	c.ready = ready;
	LOG_INF("session %s", ready ? "ready" : "not ready");
	mg_core_link_refresh();
	if (ready) {
		return; /* a held turn's gate may open: the audio thread looks */
	}
	if (mg_audio_activity() == MG_AUDIO_LIVE && !mg_audio_output_held()) {
		(void)k_work_cancel_delayable(&postroll_work);
		c.ptt_live = false;
		mg_audio_stop();
	}
}

bool mg_core_is_ready(void)
{
	return c.ready;
}

/* "connected: <name> (<app> <version>)" (on a gadget without a status
 * page): the central's GAP Device Name or "phone", its app and
 * version (19) or "app unknown"; "connected: none" without a link. Bytes
 * outside printable ASCII show as '?'. */
static void printable(char *d, const char *s, size_t cap)
{
	size_t i = 0;

	for (; s[i] && i + 1 < cap; i++) {
		d[i] = s[i] >= 0x20 && s[i] < 0x7f ? s[i] : '?';
	}
	d[i] = '\0';
}

size_t mg_core_client_line(char *out, size_t cap, bool with_name)
{
	char name[MG_CLIENT_NAME_MAX + 1], app[MG_CLIENT_APP_FIELD_MAX + 1],
		ver[MG_CLIENT_APP_FIELD_MAX + 1];
	const char *a, *v;

	if (!c.connected) {
		return (size_t)snprintk(out, cap, "connected: none");
	}
	mg_settings_client_app(&a, &v);
	/* The name the gadget reads itself (GAP Device Name), else "phone":
	 * client_name (18) is reserved, the apps don't send it. */
	char seen[MG_CLIENT_NAME_MAX + 1];

	if (mg_transport_peer_name(seen, sizeof(seen)) > 0) {
		printable(name, seen, sizeof(name));
	} else {
		strcpy(name, "phone");
	}
	printable(app, a, sizeof(app));
	printable(ver, v, sizeof(ver));
	if (!with_name) {
		return (size_t)snprintk(out, cap, "connected: %s%s%s",
					app[0] ? app : (ver[0] ? "app" : "app unknown"),
					ver[0] ? " " : "", ver);
	}
	if (app[0] || ver[0]) {
		return (size_t)snprintk(out, cap, "connected: %s (%s%s%s)",
					name, app[0] ? app : "app",
					ver[0] ? " " : "", ver);
	}
	return (size_t)snprintk(out, cap, "connected: %s (app unknown)", name);
}

bool mg_core_is_connected(void)
{
	return c.connected;
}

void mg_core_init(void)
{
	memset(&c, 0, sizeof(c));
	c.link = MG_LED_LINK_NEVER;
	c.mic_gain = CONFIG_MG_MIC_DEFAULT_GAIN;
	mg_audio_set_gain(c.mic_gain);
#if defined(CONFIG_MG_ASSISTANT_STATE)
	k_work_init_delayable(&compat_work, compat_fn);
#endif
	k_work_init_delayable(&hold_work, hold_fn);
#if defined(CONFIG_MG_TAP_TO_TALK)
	k_work_init_delayable(&tap_max_work, tap_max_fn);
#endif
	k_work_init_delayable(&postroll_work, postroll_fn);
	mg_tp_init();
}
