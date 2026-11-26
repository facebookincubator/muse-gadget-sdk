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

#include <string.h>

#include <zephyr/sys/byteorder.h>
#include <zephyr/ztest.h>

#include "fakes.h"
#include "mg_boot.h"
#include "mg_core.h"
#include "mg_queue.h"
#include "mg_session.h"
#include "mg_evt_policy.h"
#include "mg_led.h"
#include "mg_nus_policy.h"
#include "mg_settings.h"
#include "mg_setup.h"

/* Protocol engine through mg_session. */


static void rx(const uint8_t *buf, size_t len)
{
	mg_session_rx(MG_ATT_CONTROL, buf, len);
}

#define RX(...)                                                                                    \
	do {                                                                                       \
		const uint8_t _b[] = {__VA_ARGS__};                                                \
		rx(_b, sizeof(_b));                                                                \
	} while (0)

static void *setup(void)
{
	fake_init_once();
	return NULL;
}

static void before(void *f)
{
	ARG_UNUSED(f);
	mg_settings_reset();
	fake_reset();
	fake_connect();
}

static void after(void *f)
{
	ARG_UNUSED(f);
	mg_core_button(false);
	mg_session_disconnected();
}

static void assert_error(uint8_t cmd, uint16_t code)
{
	const struct fake_note *n = fake_find(MG_ATT_CONTROL, mg_command_error, 0);

	zassert_not_null(n, "no error notified");
	zassert_true(n->len >= 5);
	zassert_equal(n->data[1], cmd);
	zassert_equal(n->data[2], mg_error_data_code);
	zassert_equal(sys_get_le16(&n->data[3]), code, "code %u", sys_get_le16(&n->data[3]));
}

/* Hold past the threshold, then release. */
static void hold(void)
{
	mg_core_button(true);
	k_sleep(K_MSEC(CONFIG_MG_HOLD_MS + 50));
}

static void talk(void)
{
	hold();
	mg_core_button(false);
}

/* Mic output only to a proven central (or
 * the bench switch). This gadget isn't set up here, so no central can be
 * proven: a press with a client connected goes nowhere, start_mic and
 * read_clip are proof_required; the bench switch lifts it, per connection. */
ZTEST(core, test_unproven_central_output_held)
{
	zassert_true(mg_core_bench());
	RX(mg_command_set_settings, mg_setting_parameter_push_to_talk_enabled, 1);
	zassert_ok(mg_core_set_bench(false, "test", true));
	zassert_false(mg_core_output_ok());
	fake_nnotes = 0;
	talk();
	zassert_equal(fake_audio_starts, 0, "no capture for an unproven central");
	zassert_equal(fake_audio_recordings, 0);
	zassert_is_null(fake_find(MG_ATT_CONTROL, mg_command_gesture, 0), "no gesture either");
	fake_nnotes = 0;
	RX(mg_command_start_mic, mg_data_type_audio_sbc);
	assert_error(mg_command_start_mic, mg_error_code_proof_required);
	zassert_equal(fake_audio_starts, 0);
	zassert_equal(mg_core_start_refused(), 1);
	/* The bench switch: mic output without a proof, this connection only. */
	zassert_ok(mg_core_set_bench(true, "test", true));
	zassert_true(mg_core_output_ok());
	fake_nnotes = 0;
	RX(mg_command_start_mic, mg_data_type_audio_sbc);
	zassert_equal(fake_audio_starts, 1);
	RX(mg_command_stop_mic);
	/* Push-to-talk off: a press is gestures only, which isn't mic output,
	 * so they go to an unproven central too. */
	zassert_ok(mg_core_set_bench(false, "test", true));
	RX(mg_command_set_settings, mg_setting_parameter_push_to_talk_enabled, 0);
	fake_nnotes = 0;
	fake_audio_starts = 0;
	talk();
	zassert_not_null(fake_find(MG_ATT_CONTROL, mg_command_gesture, 0));
	zassert_equal(fake_audio_starts, 0);
	mg_session_disconnected();
	zassert_false(mg_core_bench(), "off with the connection");
	zassert_equal(mg_core_set_bench(true, "test", true), -ENOTCONN);
	fake_connect(); /* the bench client again, for after() */
}

ZTEST(core, test_ready_after_subscribe)
{
	zassert_true(mg_core_is_ready());
	mg_session_subscribed(MG_ATT_CONTROL, false);
	zassert_false(mg_core_is_ready());
	mg_session_subscribed(MG_ATT_CONTROL, true);
	zassert_true(mg_core_is_ready());
	/* Data matters too: the audio goes there. */
	mg_session_subscribed(MG_ATT_DATA, false);
	zassert_false(mg_core_is_ready());
	mg_session_subscribed(MG_ATT_DATA, true);
	zassert_true(mg_core_is_ready());
	/* request_status is part of it, in either order. */
	mg_session_disconnected();
	mg_session_connected();
	mg_session_subscribed(MG_ATT_CONTROL, true);
	RX(mg_command_request_status); /* iOS: before its Data subscription lands */
	zassert_false(mg_core_is_ready());
	mg_session_subscribed(MG_ATT_DATA, true);
	zassert_true(mg_core_is_ready());
	mg_session_disconnected();
	mg_session_connected();
	mg_session_subscribed(MG_ATT_CONTROL, true);
	mg_session_subscribed(MG_ATT_DATA, true); /* Android: both first */
	zassert_false(mg_core_is_ready());
	RX(mg_command_request_status);
	zassert_true(mg_core_is_ready());
}

ZTEST(core, test_request_status)
{
	RX(mg_command_request_status);
	zassert_equal(fake_nnotes, 7);

	/* change_data_type with the SBC format. */
	const struct fake_note *n = &fake_notes[0];

	zassert_equal(n->len, 9);
	zassert_equal(n->data[0], mg_command_change_data_type);
	zassert_equal(n->data[1], mg_data_type_audio_sbc);
	zassert_equal(sys_get_le16(&n->data[2]), 16000);
	zassert_equal(n->data[4], 1);
	zassert_equal(sys_get_le16(&n->data[5]), 60);
	zassert_equal(sys_get_le16(&n->data[7]), 8000);

	/* Command list, then the typed start_mic codec list. */
	n = &fake_notes[1];
	zassert_equal(n->data[0], mg_command_supported_features);
	zassert_not_null(memchr(&n->data[1], mg_command_start_mic, n->len - 1));
	zassert_not_null(memchr(&n->data[1], mg_command_audio_queue, n->len - 1));
	zassert_not_null(memchr(&n->data[1], mg_command_set_settings, n->len - 1));
	zassert_is_null(memchr(&n->data[1], 0x80, n->len - 1), "0x80 is reserved");
	/* Only commands the device accepts. */
	zassert_is_null(memchr(&n->data[1], mg_command_gesture, n->len - 1));
	zassert_is_null(memchr(&n->data[1], mg_command_error, n->len - 1));
	zassert_is_null(memchr(&n->data[1], mg_command_change_data_type, n->len - 1));
	zassert_is_null(memchr(&n->data[1], mg_command_supported_features, n->len - 1));
	zassert_not_null(memchr(&n->data[1], mg_command_token_proof, n->len - 1));
	zassert_not_null(memchr(&n->data[1], mg_command_assistant_state, n->len - 1));
	zassert_not_null(memchr(&n->data[1], mg_command_device_action, n->len - 1));
	/* The device session, and not the skills it supersedes. */
	zassert_not_null(memchr(&n->data[1], mg_command_session, n->len - 1));
	zassert_is_null(memchr(&n->data[1], mg_command_skill, n->len - 1));
	zassert_equal(n->len, 1 + 11,
		      "start/stop_mic, gain, status, get/set_settings, queue, token_proof, "
		      "session, assistant_state, device_action");

	/* The device session's caps, right after the command list: sealed
	 * credentials, and the client refreshes (no Wi-Fi). */
	n = &fake_notes[2];
	const uint8_t caps[] = {mg_command_supported_features, mg_command_sub_feature,
				mg_command_session,
				mg_session_cap_sealed | mg_session_cap_client_refreshes};
	zassert_equal(n->len, sizeof(caps));
	zassert_mem_equal(n->data, caps, sizeof(caps));

	n = &fake_notes[3];
	const uint8_t codecs[] = {mg_command_supported_features, mg_command_sub_feature,
				  mg_command_start_mic, mg_data_type_audio_sbc,
				  mg_data_type_audio_lc3};
	zassert_equal(n->len, sizeof(codecs));
	zassert_mem_equal(n->data, codecs, sizeof(codecs));

	/* Capture rates per codec, 16000 first, each
	 * with its kb/s range: SBC 60 fixed, LC3 16 to CONFIG_MG_LC3_MAX_KBPS. */
	n = &fake_notes[4];
	const uint8_t rates[] = {mg_command_supported_features, mg_command_sub_feature,
				 mg_command_change_data_type,
				 mg_data_type_audio_sbc, 1, 0x80, 0x3e, 60, 0, 60, 0,
#if defined(CONFIG_MG_LC3_24K)
				 mg_data_type_audio_lc3, 2, 0x80, 0x3e, 16, 0, CONFIG_MG_LC3_MAX_KBPS, 0,
				 0xc0, 0x5d, 16, 0, CONFIG_MG_LC3_MAX_KBPS, 0,
#else
				 mg_data_type_audio_lc3, 1, 0x80, 0x3e, 16, 0, CONFIG_MG_LC3_MAX_KBPS, 0,
#endif
	};
	zassert_equal(n->len, sizeof(rates), "%u", n->len);
	zassert_mem_equal(n->data, rates, sizeof(rates));

	/* Device actions: battery and power off (both on in the unit
	 * configuration), the throughput test and the clock. */
	n = &fake_notes[5];
	const uint8_t actions[] = {mg_command_supported_features, mg_command_sub_feature,
				   mg_command_device_action,
				   mg_device_action_request_battery_status,
				   mg_device_action_power_off, mg_device_action_throughput_test,
				   mg_device_action_set_unix_time, mg_device_action_get_unix_time};
	zassert_equal(n->len, sizeof(actions));
	zassert_mem_equal(n->data, actions, sizeof(actions));

	/* The typed settings list: exactly what get/set_settings accept
	 * (capture_sample_rate only where more than one rate is listed). */
	n = &fake_notes[6];
#if defined(CONFIG_MG_HAPTICS)
	const uint8_t settings[] = {mg_command_supported_features, mg_command_sub_feature,
				    mg_command_set_settings, mg_setting_parameter_spec_version,
				    mg_setting_parameter_push_to_talk_enabled,
				    mg_setting_parameter_haptics_enabled,
				    mg_setting_parameter_ptt_buzz_freq_hz,
				    mg_setting_parameter_ptt_buzz_duration_ms,
				    mg_setting_parameter_ptt_buzz_volume_percent,
				    mg_setting_parameter_audio_codec,
				    mg_setting_parameter_audio_queue_enabled,
#if defined(CONFIG_MG_LC3_24K)
				    mg_setting_parameter_capture_sample_rate,
#endif
				    mg_setting_parameter_capture_bitrate_kbps,
				    mg_setting_parameter_link_mode,
				    mg_setting_parameter_link_params,
				    mg_setting_parameter_client_app, /* 18 reserved: unlisted */
	};
#else
	/* No vibration motor: no haptics or ptt_buzz_* settings. */
	const uint8_t settings[] = {mg_command_supported_features, mg_command_sub_feature,
				    mg_command_set_settings, mg_setting_parameter_spec_version,
				    mg_setting_parameter_push_to_talk_enabled,
				    mg_setting_parameter_audio_codec,
				    mg_setting_parameter_audio_queue_enabled,
#if defined(CONFIG_MG_LC3_24K)
				    mg_setting_parameter_capture_sample_rate,
#endif
				    mg_setting_parameter_capture_bitrate_kbps,
				    mg_setting_parameter_link_mode,
				    mg_setting_parameter_link_params,
				    mg_setting_parameter_client_app, /* 18 reserved: unlisted */
	};
#endif
	zassert_equal(n->len, sizeof(settings), "%u", n->len);
	zassert_mem_equal(n->data, settings, sizeof(settings));
}

/* Who holds the link, for the consoles; the Nordic UART's form never
 * carries the phone's name. */
ZTEST(core, test_client_line)
{
	char line[96];

	RX(mg_command_set_settings, mg_setting_parameter_client_name, 5, 'P', 'i', 'x', 'e', 'l',
	   mg_setting_parameter_client_app, 4, 'M', 'u', 's', 'e', 3, '1', '.', '2');
	(void)mg_core_client_line(line, sizeof(line), true);
	/* client_name (18) is reserved: without a GAP name, "phone". */
	zassert_str_equal(line, "connected: phone (Muse 1.2)");
	(void)mg_core_client_line(line, sizeof(line), false);
	zassert_str_equal(line, "connected: Muse 1.2");
	zassert_is_null(strstr(line, "Pixel"));
	RX(mg_command_set_settings, mg_setting_parameter_client_app, 0, 0);
	(void)mg_core_client_line(line, sizeof(line), false);
	zassert_str_equal(line, "connected: app unknown");
	(void)mg_core_client_line(line, sizeof(line), true);
	zassert_str_equal(line, "connected: phone (app unknown)");
	/* The name the gadget reads itself (GAP Device Name); still never on
	 * the NUS form. */
	strcpy(fake_peer_name, "Test phone S22");
	(void)mg_core_client_line(line, sizeof(line), true);
	zassert_str_equal(line, "connected: Test phone S22 (app unknown)");
	(void)mg_core_client_line(line, sizeof(line), false);
	zassert_is_null(strstr(line, "S22"));
	fake_peer_name[0] = '\0';
	/* 19 listed for set_settings (Android sends only listed settings); 18
	 * reserved, unlisted on every board, still accepted
	 * and read back by number. */
	zassert_false(mg_settings_listed(mg_setting_parameter_client_name));
	zassert_true(mg_settings_listed(mg_setting_parameter_client_app));
	fake_reset();
	RX(mg_command_get_settings, mg_setting_parameter_client_name);
	zassert_equal(fake_nnotes, 1);
	zassert_equal(fake_notes[0].data[0], mg_command_get_settings);
	zassert_equal(fake_notes[0].data[1], mg_setting_parameter_client_name);
	/* Not set up: the owner's content rule (mg_owner_reads.h) doesn't
	 * apply yet. */
	zassert_equal(fake_notes[0].data[2], 5, "Pixel, read back by number");
	mg_session_disconnected();
	(void)mg_core_client_line(line, sizeof(line), false);
	zassert_str_equal(line, "connected: none");
	fake_connect();
}

/* Every request_status reply fits one notification and is
 * built whole (nothing counted as cut short), with this configuration's
 * options (the plain one has every setting and codec). */
ZTEST(core, test_status_replies_fit)
{
	unsigned int before = mg_session_replies_dropped();

	RX(mg_command_request_status);
	zassert_true(fake_nnotes >= 6);
	for (int i = 0; i < fake_nnotes; i++) {
		zassert_true(fake_notes[i].len <= 244, "reply %d: %u B", i, fake_notes[i].len);
	}
	zassert_equal(mg_session_replies_dropped(), before, "nothing cut short or dropped");
	fake_reset();
	RX(mg_command_get_settings);
	zassert_equal(fake_nnotes, 1);
	zassert_true(fake_notes[0].len <= 244);
}

/* A central's traffic never holds the app work queue, through mg_core: a
 * connection's request_status is answered MG_STATUS_PER_WINDOW times a
 * window, the rest not, until the window turns. */
ZTEST(core, test_status_rate)
{
	k_sleep(K_MSEC(MG_STATUS_WINDOW_MS)); /* a fresh window */
	fake_reset();
	RX(mg_command_request_status);
	int per = fake_nnotes; /* the notifications of one answer */

	zassert_true(per > 0);
	for (int i = 1; i < 10; i++) {
		RX(mg_command_request_status);
	}
	zassert_equal(fake_nnotes, MG_STATUS_PER_WINDOW * per, "%d notes", fake_nnotes);
	k_sleep(K_MSEC(MG_STATUS_WINDOW_MS));
	fake_reset();
	RX(mg_command_request_status);
	zassert_equal(fake_nnotes, per, "answered again once the window turned");
}

ZTEST(core, test_unknown_command)
{
	RX(0x05);
	assert_error(0x05, mg_error_code_unsupported);
	fake_reset();
	RX(0x80, 0, 1, 1); /* reserved for a future security extension */
	assert_error(0x80, mg_error_code_unsupported);
}

#if !defined(CONFIG_MG_HAPTICS)
/* A board without a vibration motor refuses the haptics settings. */
ZTEST(core, test_no_motor)
{
	RX(mg_command_get_settings, mg_setting_parameter_ptt_buzz_freq_hz);
	assert_error(mg_command_get_settings, mg_error_code_unsupported);
	fake_reset();
	RX(mg_command_get_settings);
	zassert_equal(fake_notes[0].len,
		      1 + 4 * 2 + 3 + (IS_ENABLED(CONFIG_MG_LC3_24K) ? 3 : 0) + 2 + 7 + 3,
		      "spec, ptt, codec, queue, capture bitrate (and rate with 24 kHz), "
		      "link_mode, link_params, client_app (client_name unlisted)");
	zassert_is_null(memchr(&fake_notes[0].data[1], mg_setting_parameter_haptics_enabled, 8));
	for (uint8_t p = mg_setting_parameter_haptics_enabled;
	     p <= mg_setting_parameter_ptt_buzz_volume_percent; p++) {
		uint8_t v[3] = {p, 1, 0};

		fake_reset();
		const uint8_t m[] = {mg_command_set_settings, v[0], v[1], v[2]};

		rx(m, 2 + mg_settings_value_size(p));
		assert_error(mg_command_set_settings, mg_error_code_unsupported);
	}
	/* Push-to-talk works, without the activation buzz. */
	RX(mg_command_set_settings, mg_setting_parameter_push_to_talk_enabled, 1,
	   mg_setting_parameter_haptics_enabled, 1);
	zassert_equal(mg_settings_get()->push_to_talk_enabled, 1, "applied before the refusal");
	mg_core_button(true);
	zassert_equal(fake_audio_starts, 1);
	zassert_equal(fake_buzzes, 0);
	mg_core_button(false);
	RX(mg_command_set_settings, mg_setting_parameter_push_to_talk_enabled, 0);
}
#endif

ZTEST(core, test_get_settings)
{
	Z_TEST_SKIP_IFNDEF(CONFIG_MG_HAPTICS);
	RX(mg_command_get_settings, mg_setting_parameter_spec_version,
	   mg_setting_parameter_ptt_buzz_freq_hz, mg_setting_parameter_audio_codec);
	const uint8_t want[] = {mg_command_get_settings,
				mg_setting_parameter_spec_version, MG_SPEC_VERSION,
				mg_setting_parameter_ptt_buzz_freq_hz, 210, 0,
				mg_setting_parameter_audio_codec, mg_data_type_audio_sbc};

	zassert_equal(fake_nnotes, 1);
	zassert_equal(fake_notes[0].len, sizeof(want));
	zassert_mem_equal(fake_notes[0].data, want, sizeof(want));

	fake_reset();
	RX(mg_command_get_settings);
	zassert_equal(fake_notes[0].len,
		      1 + 6 * 2 + 3 * 3 + (IS_ENABLED(CONFIG_MG_LC3_24K) ? 3 : 0) + 2 + 7 + 3,
		      "all listed settings (capture_bitrate_kbps; capture_sample_rate with 24 kHz; "
		      "link_mode, link_params; client_app, empty; client_name unlisted)");

	/* Bounded by this link's ATT MTU: an answer that wouldn't fit one
	 * notification is refused, not sent and dropped by the stack. */
	fake_reset();
	fake_mtu = 23;
	RX(mg_command_get_settings);
	assert_error(mg_command_get_settings, mg_error_code_invalid_length);
	/* Error frames fit it too (their log cut short). */
	fake_reset();
	fake_mtu = 23;
	RX(mg_command_get_settings, 1);
	assert_error(mg_command_get_settings, mg_error_code_unsupported);
	fake_mtu = 247;

	fake_reset();
	RX(mg_command_get_settings, 1);
	assert_error(mg_command_get_settings, mg_error_code_unsupported);
}

ZTEST(core, test_set_settings)
{
	Z_TEST_SKIP_IFNDEF(CONFIG_MG_HAPTICS);
	RX(mg_command_set_settings, mg_setting_parameter_push_to_talk_enabled, 1,
	   mg_setting_parameter_audio_codec, mg_data_type_audio_lc3);
	zassert_equal(fake_nnotes, 0, "no reply on success");
	zassert_equal(mg_settings_get()->push_to_talk_enabled, 1);
	zassert_equal(mg_settings_get()->audio_codec, mg_data_type_audio_lc3);

	RX(mg_command_set_settings, 0x7f, 1);
	assert_error(mg_command_set_settings, mg_error_code_unsupported);
	fake_reset();
	RX(mg_command_set_settings, mg_setting_parameter_haptics_enabled);
	assert_error(mg_command_set_settings, mg_error_code_invalid_length);
	fake_reset();
	RX(mg_command_set_settings, mg_setting_parameter_ptt_buzz_volume_percent, 200);
	assert_error(mg_command_set_settings, mg_error_code_invalid_value);

	/* The saved codec is used for device-initiated capture. */
	fake_reset();
	mg_core_button(true);
	zassert_equal(fake_audio_codec, mg_data_type_audio_lc3);
	mg_core_button(false);

	RX(mg_command_set_settings, mg_setting_parameter_push_to_talk_enabled, 0,
	   mg_setting_parameter_audio_codec, mg_data_type_audio_sbc);
}

/* start_mic's rate (param 2): honored where the codec lists it, else
 * 16000, never refused. */
ZTEST(core, test_start_mic_rate)
{
	const uint16_t hi = IS_ENABLED(CONFIG_MG_LC3_24K) ? 24000 : 16000;

	RX(mg_command_start_mic, mg_data_type_audio_lc3, 0xc0, 0x5d); /* 24000 */
	zassert_equal(fake_nnotes, 0, "never refused for the rate");
	zassert_equal(fake_audio_codec, mg_data_type_audio_lc3);
	zassert_equal(fake_audio_rate, hi);
	RX(mg_command_stop_mic);

	RX(mg_command_start_mic, mg_data_type_audio_lc3, 0x00, 0x7d); /* 32000: unlisted */
	zassert_equal(fake_audio_rate, 16000);
	RX(mg_command_stop_mic);
	RX(mg_command_start_mic, mg_data_type_audio_sbc, 0xc0, 0x5d); /* SBC is 16 kHz only */
	zassert_equal(fake_audio_rate, 16000);
	RX(mg_command_stop_mic);
	RX(mg_command_start_mic, mg_data_type_audio_lc3); /* as today */
	zassert_equal(fake_audio_rate, 16000);
	RX(mg_command_stop_mic);
	zassert_equal(fake_nnotes, 0);
}

/* start_mic's bitrate (param 3): passed on to be clamped (mg_codec), never
 * refused; capture_bitrate_kbps (14): per connection, 32 at every connect,
 * invalid_value outside every listed range. */
ZTEST(core, test_capture_bitrate)
{
	const uint8_t get32[] = {mg_command_get_settings,
				 mg_setting_parameter_capture_bitrate_kbps, 32, 0};

	RX(mg_command_start_mic, mg_data_type_audio_lc3, 0x80, 0x3e, 16, 0);
	zassert_equal(fake_nnotes, 0);
	zassert_equal(fake_audio_kbps, 16);
	RX(mg_command_stop_mic);
	RX(mg_command_start_mic, mg_data_type_audio_lc3, 0x80, 0x3e);
	zassert_equal(fake_audio_kbps, 32, "the default");
	RX(mg_command_stop_mic);

	RX(mg_command_set_settings, mg_setting_parameter_capture_bitrate_kbps, 10, 0);
	assert_error(mg_command_set_settings, mg_error_code_invalid_value);
	fake_reset();
	RX(mg_command_set_settings, mg_setting_parameter_capture_bitrate_kbps,
	   CONFIG_MG_LC3_MAX_KBPS + 1, 0);
	assert_error(mg_command_set_settings, mg_error_code_invalid_value);
	fake_reset();
	RX(mg_command_set_settings, mg_setting_parameter_push_to_talk_enabled, 1,
	   mg_setting_parameter_audio_codec, mg_data_type_audio_lc3,
	   mg_setting_parameter_capture_bitrate_kbps, 16, 0);
	zassert_equal(fake_nnotes, 0);
	talk();
	zassert_equal(fake_audio_codec, mg_data_type_audio_lc3);
	zassert_equal(fake_audio_kbps, 16, "a turn at setting 14's bitrate");

	/* Remembered across a reconnect; 32 again once set back. */
	const uint8_t get16k[] = {mg_command_get_settings,
				  mg_setting_parameter_capture_bitrate_kbps, 16, 0};

	mg_session_disconnected();
	fake_connect();
	fake_reset();
	RX(mg_command_get_settings, mg_setting_parameter_capture_bitrate_kbps);
	zassert_equal(fake_nnotes, 1);
	zassert_mem_equal(fake_notes[0].data, get16k, sizeof(get16k));
	RX(mg_command_set_settings, mg_setting_parameter_capture_bitrate_kbps, 32, 0);
	fake_reset();
	RX(mg_command_get_settings, mg_setting_parameter_capture_bitrate_kbps);
	zassert_mem_equal(fake_notes[0].data, get32, sizeof(get32));
}

/* capture_sample_rate (13): per connection, for the gadget's own captures
 * where the codec takes it; a rate no codec lists is invalid_value. */
ZTEST(core, test_capture_sample_rate)
{
	const uint8_t get16[] = {mg_command_get_settings, mg_setting_parameter_capture_sample_rate,
				 0x80, 0x3e};

	RX(mg_command_set_settings, mg_setting_parameter_capture_sample_rate, 0x00, 0x7d);
	assert_error(mg_command_set_settings, mg_error_code_invalid_value);
	fake_reset();
	RX(mg_command_set_settings, mg_setting_parameter_capture_sample_rate, 0x80, 0x3e);
	zassert_equal(fake_nnotes, 0, "16000 is always accepted");

	RX(mg_command_set_settings, mg_setting_parameter_push_to_talk_enabled, 1,
	   mg_setting_parameter_audio_codec, mg_data_type_audio_lc3,
	   mg_setting_parameter_capture_sample_rate, 0xc0, 0x5d);
#if defined(CONFIG_MG_LC3_24K)
	zassert_equal(fake_nnotes, 0);
	talk();
	zassert_equal(fake_audio_codec, mg_data_type_audio_lc3);
	zassert_equal(fake_audio_rate, 24000, "an LC3 turn at 24 kHz");
	RX(mg_command_set_settings, mg_setting_parameter_audio_codec, mg_data_type_audio_sbc);
	talk();
	zassert_equal(fake_audio_rate, 16000, "an SBC turn stays 16 kHz");
#else
	assert_error(mg_command_set_settings, mg_error_code_invalid_value);
#endif
	/* Remembered across a reconnect (24000 where it was taken); 16000 once
	 * set back. */
	mg_session_disconnected();
	fake_connect();
	fake_reset();
	RX(mg_command_get_settings, mg_setting_parameter_capture_sample_rate);
	zassert_equal(fake_nnotes, 1);
	zassert_equal(fake_notes[0].len, sizeof(get16));
	zassert_equal(sys_get_le16(&fake_notes[0].data[2]),
		      IS_ENABLED(CONFIG_MG_LC3_24K) ? 24000 : 16000);
	/* A remembered rate applies only to a codec that lists
	 * it: an SBC turn captures at 16000 (and SBC's own bitrate). */
	RX(mg_command_set_settings, mg_setting_parameter_push_to_talk_enabled, 1,
	   mg_setting_parameter_audio_codec, mg_data_type_audio_sbc);
	talk();
	zassert_equal(fake_audio_codec, mg_data_type_audio_sbc);
	zassert_equal(fake_audio_rate, 16000, "remembered 24000 + SBC = 16000");
	RX(mg_command_set_settings, mg_setting_parameter_capture_sample_rate, 0x80, 0x3e,
	   mg_setting_parameter_audio_codec, mg_data_type_audio_sbc);
	fake_reset();
	RX(mg_command_get_settings, mg_setting_parameter_capture_sample_rate);
	zassert_mem_equal(fake_notes[0].data, get16, sizeof(get16));
}

/* An injected press (a bench word's "talk down") ends with its link: no
 * "talk up" can follow it, so the next turn is whole. */
ZTEST(core, test_injected_press_ends_with_link)
{
	mg_core_button_ev(true, k_uptime_get(), false, true); /* injected, no release follows */
	k_sleep(K_MSEC(CONFIG_MG_HOLD_MS + 50)); /* held: its button_down goes out */
	zassert_equal(fake_nnotes, 1);
	zassert_equal(fake_notes[0].data[1], mg_gesture_button_down);
	mg_session_disconnected();
	fake_reset();
	fake_connect();
	talk();
	zassert_equal(fake_nnotes, 2, "the next turn is whole (the injected press was released)");
	zassert_equal(fake_notes[0].data[1], mg_gesture_button_down);
	zassert_equal(fake_notes[1].data[1], mg_gesture_button_up);
}

/* The update prompt through the real core: a press queued before the
 * prompt doesn't answer it (it's a talk press); the prompt's own press and
 * its release make no turn; the next talk turn is whole (its release is its
 * own, not swallowed). */
ZTEST(core, test_update_press_then_talk)
{
	fake_reset();
	fake_board_prompt_presses = 1;
	mg_led_set(MG_LED_UPDATE_PROMPT, true); /* the update cue shows */
	mg_core_button_ev(true, k_uptime_get(), true, false); /* queued before the prompt */
	zassert_equal(fake_board_prompt_presses, 1, "an old press doesn't answer the prompt");
	mg_core_button_ev(false, k_uptime_get(), true, false);
	fake_reset();
	mg_core_button_ev(true, k_uptime_get(), true, true); /* the prompt's press */
	zassert_equal(fake_board_prompt_presses, 0, "it answered the prompt");
	mg_core_button_ev(false, k_uptime_get(), true, true); /* and its release */
	zassert_equal(fake_nnotes, 0, "no gesture, no turn");
	talk();
	zassert_equal(fake_nnotes, 2, "the next turn is whole");
	zassert_equal(fake_notes[0].data[1], mg_gesture_button_down);
	zassert_equal(fake_notes[1].data[1], mg_gesture_button_up);
	/* Under the setup prompt's cue the press never answers the update
	 * prompt. */
	fake_board_prompt_presses = 1;
	mg_led_set(MG_LED_PAIR_PENDING, true);
	fake_reset();
	mg_core_button_ev(true, k_uptime_get(), true, true);
	zassert_equal(fake_board_prompt_presses, 1, "no update press under another cue");
	mg_core_button_ev(false, k_uptime_get(), true, true);
	mg_led_set(MG_LED_PAIR_PENDING, false);
	mg_led_set(MG_LED_UPDATE_PROMPT, false);
	fake_board_prompt_presses = 0;
	fake_reset();
}

/* A new token proof challenge on the link clears the client's
 * ready state: push-to-talk off (the codec stays) until it's enabled again;
 * presses meanwhile are gestures only. */
ZTEST(core, test_proof_challenge_clears_ptt)
{
	RX(mg_command_set_settings, mg_setting_parameter_push_to_talk_enabled, 1,
	   mg_setting_parameter_audio_codec, mg_data_type_audio_lc3);
	zassert_equal(mg_settings_get()->push_to_talk_enabled, 1);
	fake_reset();
	RX(mg_command_token_proof, mg_token_proof_challenge, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12,
	   13, 14, 15, 16);
	zassert_equal(mg_settings_get()->push_to_talk_enabled, 0, "cleared at the challenge");
	zassert_equal(mg_settings_get()->audio_codec, mg_data_type_audio_lc3, "the codec stays");
	fake_reset();
	talk();
	zassert_equal(fake_audio_starts, 0, "gestures only");
	zassert_equal(fake_count(MG_ATT_CONTROL, mg_command_gesture), 2);
	RX(mg_command_set_settings, mg_setting_parameter_push_to_talk_enabled, 1);
	talk();
	zassert_equal(fake_audio_starts, 1, "the client enabled it again");
	RX(mg_command_set_settings, mg_setting_parameter_push_to_talk_enabled, 0,
	   mg_setting_parameter_audio_codec, mg_data_type_audio_sbc);
}

/* Push-to-talk turned off mid-hold revokes the turn (before the threshold
 * the pre-roll is dropped and no turn starts, after it the capture stops);
 * a client's start_mic that fails leaves the press's turn the press's, so
 * the release still stops the mic. */
ZTEST(core, test_ptt_off_mid_hold)
{
	RX(mg_command_set_settings, mg_setting_parameter_push_to_talk_enabled, 1);
	fake_reset();
	/* Before the threshold: discarded, nothing committed or started later. */
	mg_core_button(true);
	k_sleep(K_MSEC(CONFIG_MG_HOLD_MS / 2));
	RX(mg_command_set_settings, mg_setting_parameter_push_to_talk_enabled, 0);
	zassert_equal(fake_audio_discards, 1);
	k_sleep(K_MSEC(CONFIG_MG_HOLD_MS));
	zassert_equal(fake_audio_commits, 0, "no turn at the threshold");
	mg_core_button(false);
	zassert_equal(fake_audio_stops, 0);
	zassert_equal(fake_audio_discards, 1, "nothing to discard twice");

	/* After the threshold: the capture stops at once. */
	RX(mg_command_set_settings, mg_setting_parameter_push_to_talk_enabled, 1);
	fake_reset();
	hold();
	zassert_equal(fake_audio_commits, 1);
	RX(mg_command_set_settings, mg_setting_parameter_push_to_talk_enabled, 0);
	zassert_equal(fake_audio_stops, 1);
	mg_core_button(false);
	k_sleep(K_MSEC(CONFIG_MG_PTT_POSTROLL_MS + 20));
	zassert_equal(fake_audio_stops, 1, "no second stop, no restart");

	/* A held turn (the connect burst: no request_status yet), push-to-talk
	 * off before the threshold: no button_down and no turn later. */
	mg_session_disconnected();
	mg_session_connected();
	mg_session_subscribed(MG_ATT_CONTROL, true);
	mg_session_subscribed(MG_ATT_DATA, true);
	(void)mg_core_set_bench(true, "unit test", true);
	RX(mg_command_set_settings, mg_setting_parameter_push_to_talk_enabled, 1);
	fake_reset();
	mg_core_button(true);
	zassert_equal(fake_audio_turns, 1, "a held turn");
	RX(mg_command_set_settings, mg_setting_parameter_push_to_talk_enabled, 0);
	zassert_equal(fake_audio_discards, 1);
	k_sleep(K_MSEC(CONFIG_MG_HOLD_MS + 50));
	zassert_equal(fake_count(MG_ATT_CONTROL, mg_command_gesture), 0, "no button_down");
	zassert_equal(fake_audio_commits, 0);
	mg_core_button(false);
	zassert_equal(fake_count(MG_ATT_CONTROL, mg_command_gesture), 0, "no button_up");
	mg_session_status_requested(); /* ready again, as the suite's central */
}

ZTEST(core, test_held_turn_survives_a_failed_start_mic)
{
	RX(mg_command_set_settings, mg_setting_parameter_push_to_talk_enabled, 1);
	fake_reset();
	hold();
	zassert_equal(fake_audio_commits, 1);
	fake_audio_start_fail = 1;
	RX(mg_command_start_mic);
	assert_error(mg_command_start_mic, mg_error_code_busy);
	mg_core_button(false);
	k_sleep(K_MSEC(CONFIG_MG_PTT_POSTROLL_MS + 20));
	zassert_equal(fake_audio_stops, 1, "the release still stops the mic");
	RX(mg_command_set_settings, mg_setting_parameter_push_to_talk_enabled, 0);
}

ZTEST(core, test_audio_queue_commands)
{
	struct mg_queue_status st;

	zassert_equal(mg_queue_clear(), 0);
	zassert_equal(mg_queue_begin(mg_data_type_audio_sbc, 1234), 0);
	zassert_equal(mg_queue_append("0123456789", 10), 0);
	zassert_equal(mg_queue_end(), 0);
	mg_queue_get_status(&st);

	RX(mg_command_audio_queue, mg_audio_queue_command_status);
	const struct fake_note *n = fake_find(MG_ATT_CONTROL, mg_command_audio_queue, 0);

	zassert_not_null(n);
	zassert_equal(n->len, 13);
	zassert_equal(n->data[1], mg_audio_queue_command_status);
	zassert_equal(n->data[2], 0, "queue disabled");
	zassert_equal(sys_get_le32(&n->data[3]), st.used_bytes);
	zassert_equal(sys_get_le32(&n->data[7]), st.capacity_bytes);
	zassert_equal(sys_get_le16(&n->data[11]), 1);

	fake_reset();
	RX(mg_command_audio_queue, mg_audio_queue_command_clip_info, 0, 0);
	n = fake_find(MG_ATT_CONTROL, mg_command_audio_queue, 0);
	zassert_not_null(n);
	/* With the clip's format; a clip stored
	 * without one reads as its codec's default: SBC 16000, mono, 60 B,
	 * 8000 us. */
	zassert_equal(n->len, 28);
	zassert_equal(sys_get_le16(&n->data[13]), 16000);
	zassert_equal(n->data[15], 1);
	zassert_equal(sys_get_le16(&n->data[16]), 60);
	zassert_equal(sys_get_le16(&n->data[18]), 8000);
	zassert_equal(sys_get_le64(&n->data[20]), 0, "start_unix_ms 0: no clock");

	zassert_equal(n->data[1], mg_audio_queue_command_clip_info);
	zassert_equal(sys_get_le16(&n->data[2]), 0);
	zassert_equal(n->data[4], mg_data_type_audio_sbc);
	zassert_equal(sys_get_le32(&n->data[5]), 1234);
	zassert_equal(sys_get_le32(&n->data[9]), 10);

	fake_reset();
	RX(mg_command_audio_queue, mg_audio_queue_command_clip_info, 1, 0);
	assert_error(mg_command_audio_queue, mg_error_code_invalid_value);

	/* A 20 B-frame clip (LC3 16 kb/s) says frame bytes 20: the apps decode
	 * clips strictly by these fields, 40 B / 10 ms when absent. */
	zassert_equal(mg_queue_begin_fmt(mg_data_type_audio_lc3, 16000, 1, 20, 10000, 5678,
					 1793800000), 0);
	zassert_equal(mg_queue_append("01234567890123456789", 20), 0);
	zassert_equal(mg_queue_end(), 0);
	fake_reset();
	RX(mg_command_audio_queue, mg_audio_queue_command_clip_info, 1, 0);
	n = fake_find(MG_ATT_CONTROL, mg_command_audio_queue, 0);
	zassert_not_null(n);
	zassert_equal(n->len, 28);
	zassert_equal(n->data[4], mg_data_type_audio_lc3);
	zassert_equal(sys_get_le64(&n->data[20]), 1793800000ULL * 1000, "start_unix_ms");
	zassert_equal(sys_get_le32(&n->data[9]), 20);
	zassert_equal(sys_get_le16(&n->data[13]), 16000);
	zassert_equal(n->data[15], 1);
	zassert_equal(sys_get_le16(&n->data[16]), 20, "frame bytes 20");
	zassert_equal(sys_get_le16(&n->data[18]), 10000);
	fake_reset();
	RX(mg_command_audio_queue, mg_audio_queue_command_clip_info, 0);
	assert_error(mg_command_audio_queue, mg_error_code_invalid_length);
	fake_reset();
	RX(mg_command_audio_queue, 2);
	assert_error(mg_command_audio_queue, mg_error_code_unsupported);

	fake_reset();
	RX(mg_command_audio_queue, mg_audio_queue_command_read_clip, 0, 0);
	zassert_equal(fake_audio_clip_reads, 1);
	zassert_equal(fake_audio_clip, 0);

	/* Live audio wins: read_clip is busy while it runs, start_mic abandons
	 * a download. */
	RX(mg_command_start_mic);
	zassert_equal(fake_audio_state, MG_AUDIO_LIVE);
	fake_reset();
	fake_audio_state = MG_AUDIO_LIVE;
	RX(mg_command_audio_queue, mg_audio_queue_command_read_clip, 0, 0);
	assert_error(mg_command_audio_queue, mg_error_code_busy);
	RX(mg_command_stop_mic);

	fake_reset();
	RX(mg_command_audio_queue, mg_audio_queue_command_clear);
	n = fake_find(MG_ATT_CONTROL, mg_command_audio_queue, 0);
	zassert_not_null(n);
	zassert_equal(n->data[1], mg_audio_queue_command_status);
	zassert_equal(sys_get_le16(&n->data[11]), 0, "cleared");
	zassert_equal(sys_get_le32(&n->data[3]), 0);
}

/* An event's shared reply budget belongs
 * to the thread handling the event; another thread's Control send (the audio
 * thread's change_data_type) keeps its own 500 ms. */
static K_THREAD_STACK_DEFINE(budget_stack, 2048);
static struct k_thread budget_thread;

static void other_send(void *a, void *b, void *c)
{
	const uint8_t cdt[] = {mg_command_change_data_type, mg_data_type_audio_sbc};

	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
	(void)mg_session_send_control(cdt, sizeof(cdt));
}

ZTEST(core, test_reply_budget_per_thread)
{
	const uint8_t st[] = {mg_command_request_status};

	mg_session_reply_budget(k_uptime_get()); /* this event's budget: spent */
	(void)mg_session_send_control(st, sizeof(st));
	zassert_true(K_TIMEOUT_EQ(fake_last_control_wait, K_MSEC(0)), "the event's own: no wait left");
	k_thread_create(&budget_thread, budget_stack, K_THREAD_STACK_SIZEOF(budget_stack), other_send,
			NULL, NULL, NULL, K_PRIO_PREEMPT(5), 0, K_NO_WAIT);
	k_thread_join(&budget_thread, K_SECONDS(2));
	zassert_true(K_TIMEOUT_EQ(fake_last_control_wait, K_MSEC(MG_REPLY_EACH_MS)),
		     "another thread's send keeps its own wait");
	mg_session_reply_budget(0);
	fake_reset();
}

/* SAFE mode declares itself (mgcommands.h, Recovery mode): the
 * declaration, then a list of only request_status; everything else is
 * recovery, the button does nothing. Reason boot_loop when it came by
 * itself, user when the boot hold asked for it. */
ZTEST(core, test_safe_mode)
{
	mg_health_set_mode(MG_BOOT_SAFE);
	/* No SAFE reopens the mic: with the bench switch on (this suite's
	 * central has it) a self-entered SAFE refuses start_mic and clip reads
	 * too, and starts no capture. */
	zassert_true(mg_core_bench());
	RX(mg_command_start_mic);
	assert_error(mg_command_start_mic, mg_error_code_recovery);
	fake_reset();
	RX(mg_command_audio_queue, mg_audio_queue_command_read_clip, 0, 0, 0, 0, 0);
	assert_error(mg_command_audio_queue, mg_error_code_recovery);
	zassert_equal(fake_audio_starts, 0);
	fake_reset();
	RX(mg_command_request_status);
	zassert_equal(fake_nnotes, 2);
	const uint8_t decl[] = {mg_command_supported_features, mg_command_sub_feature,
				mg_command_request_status, mg_device_mode_recovery,
				mg_recovery_reason_boot_loop, 0};
	const uint8_t want[] = {mg_command_supported_features, mg_command_request_status};

	zassert_equal(fake_notes[0].len, sizeof(decl));
	zassert_mem_equal(fake_notes[0].data, decl, sizeof(decl));
	zassert_equal(fake_notes[1].len, sizeof(want));
	zassert_mem_equal(fake_notes[1].data, want, sizeof(want));
	fake_reset();
	mg_health_set_safe_by_button();
	RX(mg_command_request_status);
	zassert_equal(fake_notes[0].data[4], mg_recovery_reason_user, "the boot hold's SAFE");
	fake_reset();
	RX(mg_command_start_mic);
	assert_error(mg_command_start_mic, mg_error_code_recovery);
	zassert_equal(fake_audio_starts, 0);
	fake_reset();
	RX(mg_command_device_action, mg_device_action_get_unix_time);
	assert_error(mg_command_device_action, mg_error_code_recovery);
	fake_reset();
	mg_core_button(true);
	mg_core_button(false);
	zassert_equal(fake_nnotes, 0, "no gesture");
	zassert_equal(fake_audio_starts, 0, "no capture");
	mg_health_set_mode(MG_BOOT_NORMAL);
}

ZTEST(core, test_error_log_string)
{
	RX(mg_command_start_mic, mg_data_type_audio_pcm);
	const struct fake_note *n = fake_find(MG_ATT_CONTROL, mg_command_error, 0);

	zassert_not_null(n);
	zassert_true(n->len > 8);
	zassert_equal(n->data[5], mg_error_data_log);
	uint16_t l = sys_get_le16(&n->data[6]);

	zassert_equal(n->len, 8 + l);
	zassert_mem_equal(&n->data[8], "codec not supported", l);
}

/* The hold contract's tests: a board with tap to talk runs the tap suite. */
static bool hold_board(const void *state)
{
	ARG_UNUSED(state);
	return !IS_ENABLED(CONFIG_MG_TAP_TO_TALK);
}

ZTEST_SUITE(core, hold_board, setup, before, after, NULL);

#if defined(CONFIG_MG_TAP_TO_TALK)
/* Tap to talk (CONFIG_MG_TAP_TO_TALK): a press starts the turn, its
 * release changes nothing, the next press ends it (and its own release is
 * ignored); a turn ends by itself after the longest turn, or with the link. */
static int gestures(uint8_t g)
{
	int n = 0;

	for (int i = 0; i < fake_nnotes; i++) {
		if (fake_notes[i].ch == MG_ATT_CONTROL && fake_notes[i].len >= 2 &&
		    fake_notes[i].data[0] == mg_command_gesture && fake_notes[i].data[1] == g) {
			n++;
		}
	}
	return n;
}

static void tap_press(void)
{
	mg_core_button(true);
	k_sleep(K_MSEC(40));
	mg_core_button(false);
}

static void tap_before(void *f)
{
	before(f);
	RX(mg_command_set_settings, mg_setting_parameter_push_to_talk_enabled, 1);
	zassert_true(mg_core_output_ok(), "the bench switch: mic output");
	fake_nnotes = 0;
	fake_audio_starts = 0;
	fake_audio_commits = 0;
	fake_audio_discards = 0;
}

static void tap_after(void *f)
{
	ARG_UNUSED(f);
	mg_session_disconnected(); /* ends any turn still on */
	k_sleep(K_MSEC(300));
}

ZTEST(tap, test_tap_starts_next_press_ends)
{
	tap_press();
	zassert_equal(fake_audio_starts, 1, "capture at key-down");
	k_sleep(K_MSEC(CONFIG_MG_HOLD_MS + 50));
	zassert_equal(fake_audio_discards, 0, "a short press is not a discarded tap");
	zassert_equal(fake_audio_commits, 1, "committed at the hold threshold");
	zassert_equal(gestures(mg_gesture_button_down), 1);
	zassert_equal(gestures(mg_gesture_button_up), 0, "still on after the release");
	/* The next press ends it, and starts nothing. */
	mg_core_button(true);
	zassert_equal(gestures(mg_gesture_button_up), 1, "ended at the next press");
	k_sleep(K_MSEC(40));
	mg_core_button(false);
	zassert_equal(fake_audio_starts, 1, "the ending press starts no turn");
	zassert_equal(gestures(mg_gesture_button_down), 1);
	/* A third press starts a new turn. */
	k_sleep(K_MSEC(300));
	tap_press();
	zassert_equal(fake_audio_starts, 2);
}

ZTEST(tap, test_tap_longest_turn)
{
	tap_press();
	k_sleep(K_MSEC(CONFIG_MG_TAP_TO_TALK_MAX_MS - 200));
	zassert_equal(gestures(mg_gesture_button_up), 0);
	k_sleep(K_MSEC(400));
	zassert_equal(gestures(mg_gesture_button_up), 1, "ended by itself");
	/* The next press is a new turn, not an end. */
	k_sleep(K_MSEC(300));
	tap_press();
	zassert_equal(fake_audio_starts, 2);
}

ZTEST(tap, test_tap_link_drop_resets)
{
	tap_press();
	k_sleep(K_MSEC(CONFIG_MG_HOLD_MS + 50));
	mg_session_disconnected();
	fake_connect();
	RX(mg_command_set_settings, mg_setting_parameter_push_to_talk_enabled, 1);
	k_sleep(K_MSEC(300));
	tap_press();
	zassert_equal(fake_audio_starts, 2, "a press after the link's loss starts a turn");
}

ZTEST_SUITE(tap, NULL, setup, tap_before, tap_after, NULL);
#endif
