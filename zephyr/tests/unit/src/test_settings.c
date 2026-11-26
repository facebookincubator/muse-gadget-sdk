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

#include <zephyr/settings/settings.h>
#include <zephyr/ztest.h>

#include "fakes.h"
#include "mg_settings.h"

static void *setup(void)
{
	fake_init_once();
	return NULL;
}

static void before(void *f)
{
	ARG_UNUSED(f);
	mg_settings_reset();
}

/* The settings backend never deletes an entry on a read error (ZMS's
 * listing would, without SETTINGS_ZMS_NO_LL_DELETE: Kconfig.mg,
 * MG_SETTINGS_KEEP_ON_READ_ERROR). */
ZTEST(settings, test_listing_never_deletes)
{
	zassert_true(IS_ENABLED(CONFIG_SETTINGS_ZMS_NO_LL_DELETE),
		     "a ZMS listing would delete an entry it couldn't read");
}

ZTEST(settings, test_unknown_stops_parsing)
{
	/* ptt=1 applies, then 9 is unknown and haptics=1 after it is not parsed. */
	const uint8_t in[] = {mg_setting_parameter_push_to_talk_enabled, 1, 9, 0,
			      mg_setting_parameter_haptics_enabled, 1};
	uint8_t bad;

	zassert_equal(mg_settings_apply(in, sizeof(in), &bad), mg_error_code_unsupported);
	zassert_equal(bad, 9);
	zassert_equal(mg_settings_get()->push_to_talk_enabled, 1);
	zassert_equal(mg_settings_get()->haptics_enabled, 0);
}

ZTEST(settings, test_invalid)
{
	Z_TEST_SKIP_IFNDEF(CONFIG_MG_HAPTICS);
	uint8_t bad;
	const uint8_t trunc[] = {mg_setting_parameter_ptt_buzz_freq_hz, 0x10};
	const uint8_t lowfreq[] = {mg_setting_parameter_ptt_buzz_freq_hz, 19, 0};
	const uint8_t longdur[] = {mg_setting_parameter_ptt_buzz_duration_ms, 0x89, 0x13}; /* 5001 */
	const uint8_t zerodur[] = {mg_setting_parameter_ptt_buzz_duration_ms, 0, 0};
	const uint8_t vol[] = {mg_setting_parameter_ptt_buzz_volume_percent, 101};
	const uint8_t boolv[] = {mg_setting_parameter_push_to_talk_enabled, 2};
	const uint8_t opus[] = {mg_setting_parameter_audio_codec, mg_data_type_audio_opus};
	const uint8_t ro[] = {mg_setting_parameter_spec_version, 2};

	zassert_equal(mg_settings_apply(trunc, sizeof(trunc), &bad), mg_error_code_invalid_length);
	zassert_equal(mg_settings_apply(lowfreq, sizeof(lowfreq), &bad),
		      mg_error_code_invalid_value);
	zassert_equal(mg_settings_apply(longdur, sizeof(longdur), &bad),
		      mg_error_code_invalid_value);
	zassert_equal(mg_settings_apply(zerodur, sizeof(zerodur), &bad),
		      mg_error_code_invalid_value);
	zassert_equal(mg_settings_apply(vol, sizeof(vol), &bad), mg_error_code_invalid_value);
	zassert_equal(mg_settings_apply(boolv, sizeof(boolv), &bad), mg_error_code_invalid_value);
	zassert_equal(mg_settings_apply(opus, sizeof(opus), &bad), mg_error_code_invalid_value);
	zassert_equal(mg_settings_apply(ro, sizeof(ro), &bad), mg_error_code_invalid_value);

	struct mg_settings d;

	mg_settings_defaults(&d);
	const struct mg_settings *s = mg_settings_get();

	zassert_true(s->push_to_talk_enabled == d.push_to_talk_enabled &&
		     s->haptics_enabled == d.haptics_enabled &&
		     s->ptt_buzz_freq_hz == d.ptt_buzz_freq_hz &&
		     s->ptt_buzz_duration_ms == d.ptt_buzz_duration_ms &&
		     s->ptt_buzz_volume_percent == d.ptt_buzz_volume_percent &&
		     s->audio_codec == d.audio_codec &&
		     s->audio_queue_enabled == d.audio_queue_enabled,
		     "nothing changed");
}

ZTEST(settings, test_persisted)
{
	Z_TEST_SKIP_IFNDEF(CONFIG_MG_HAPTICS); /* stores the ptt_buzz_* settings */
	const uint8_t in[] = {mg_setting_parameter_push_to_talk_enabled, 1,
			      mg_setting_parameter_haptics_enabled, 1,
			      mg_setting_parameter_ptt_buzz_volume_percent, 42,
			      mg_setting_parameter_audio_codec, mg_data_type_audio_lc3,
			      mg_setting_parameter_audio_queue_enabled, 1};
	uint8_t bad;

	zassert_equal(mg_settings_apply(in, sizeof(in), &bad), 0);
	mg_settings_flush(); /* not set up: it would wait up to 10 min */
	/* Forget the RAM copy, then load what was saved. */
	mg_settings_reset();
	zassert_equal(mg_settings_get()->push_to_talk_enabled, 0);
	zassert_equal(mg_settings_load(), 0);
	zassert_equal(mg_settings_get()->ptt_buzz_volume_percent, 42);
	zassert_equal(mg_settings_get()->audio_queue_enabled, 1);
	zassert_equal(mg_settings_get()->audio_codec, mg_data_type_audio_lc3, "format remembered");
	/* Per connection: never stored. */
	zassert_equal(mg_settings_get()->push_to_talk_enabled, 0);
	zassert_equal(mg_settings_get()->haptics_enabled, 0);

	/* Back to defaults in storage too, for the other suites. */
	const uint8_t back[] = {mg_setting_parameter_push_to_talk_enabled, 0,
				mg_setting_parameter_ptt_buzz_volume_percent, 80,
				mg_setting_parameter_audio_codec, mg_data_type_audio_sbc,
				mg_setting_parameter_audio_queue_enabled, 0};
	zassert_equal(mg_settings_apply(back, sizeof(back), &bad), 0);
	mg_settings_flush();
}

/* Settings client_name (18) and client_app (19), per connection, read back,
 * over-long refused, cut short refused, empty at a new connection. */
ZTEST(settings, test_client_name_and_app)
{
	uint8_t bad, out[64];
	const char *a, *v;
	const uint8_t in[] = {
		mg_setting_parameter_client_name, 8, 'P', 'i', 'x', 'e', 'l', ' ', '8', '!',
		mg_setting_parameter_client_app, 8, 'M', 'u', 's', 'e', ' ', 'a', 'p', 'p',
		9, '1', '6', '.', '0', '.', '0', '.', '8', '6',
		mg_setting_parameter_link_mode, mg_link_mode_ready,
	};

	zassert_equal(mg_settings_apply(in, sizeof(in), &bad), 0, "bad %u", bad);
	zassert_str_equal(mg_settings_client_name(), "Pixel 8!");
	mg_settings_client_app(&a, &v);
	zassert_str_equal(a, "Muse app");
	zassert_str_equal(v, "16.0.0.86");
	zassert_true(mg_settings_accepted(mg_setting_parameter_client_name));
	zassert_true(mg_settings_listed(mg_setting_parameter_client_app));
	/* get_settings' encoding: the same bytes back. */
	size_t n = mg_settings_encode(mg_settings_get(), mg_setting_parameter_client_name, out,
				      sizeof(out));
	zassert_equal(n, 10);
	zassert_mem_equal(out, in, 10);
	n = mg_settings_encode(mg_settings_get(), mg_setting_parameter_client_app, out, sizeof(out));
	zassert_equal(n, 20);
	zassert_mem_equal(out, &in[10], 20);
	/* Over-long: invalid_value, nothing changed. */
	uint8_t big[2 + 33] = {mg_setting_parameter_client_name, 33};

	memset(&big[2], 'x', 33);
	zassert_equal(mg_settings_apply(big, sizeof(big), &bad), mg_error_code_invalid_value);
	zassert_str_equal(mg_settings_client_name(), "Pixel 8!");
	uint8_t bigapp[3 + 25] = {mg_setting_parameter_client_app, 25};

	memset(&bigapp[2], 'a', 25);
	bigapp[27] = 0;
	zassert_equal(mg_settings_apply(bigapp, sizeof(bigapp), &bad),
		      mg_error_code_invalid_value);
	/* 32 is the most. */
	big[1] = 32;
	zassert_equal(mg_settings_apply(big, sizeof(big) - 1, &bad), 0);
	zassert_equal(strlen(mg_settings_client_name()), 32);
	/* Cut short: invalid_length. */
	const uint8_t cut[] = {mg_setting_parameter_client_name, 5, 'a', 'b'};
	const uint8_t cut2[] = {mg_setting_parameter_client_app, 1, 'a', 3, '1'};

	zassert_equal(mg_settings_apply(cut, sizeof(cut), &bad), mg_error_code_invalid_length);
	zassert_equal(mg_settings_apply(cut2, sizeof(cut2), &bad), mg_error_code_invalid_length);
	/* Empty values are fine; a new connection empties both. */
	const uint8_t empty[] = {mg_setting_parameter_client_app, 0, 0};

	zassert_equal(mg_settings_apply(empty, sizeof(empty), &bad), 0);
	mg_settings_client_app(&a, &v);
	zassert_str_equal(a, "");
	mg_settings_new_connection();
	zassert_str_equal(mg_settings_client_name(), "");
	n = mg_settings_encode(mg_settings_get(), mg_setting_parameter_client_name, out, sizeof(out));
	zassert_equal(n, 2);
	zassert_equal(out[1], 0);
}

ZTEST(settings, test_new_connection_resets_ptt_and_haptics)
{
	Z_TEST_SKIP_IFNDEF(CONFIG_MG_HAPTICS); /* stores the ptt_buzz_* settings */
	const uint8_t in[] = {mg_setting_parameter_push_to_talk_enabled, 1,
			      mg_setting_parameter_haptics_enabled, 1,
			      mg_setting_parameter_ptt_buzz_duration_ms, 50, 0};
	uint8_t bad;

	zassert_equal(mg_settings_apply(in, sizeof(in), &bad), 0);
	mg_settings_new_connection();
	zassert_equal(mg_settings_get()->push_to_talk_enabled, 0);
	zassert_equal(mg_settings_get()->haptics_enabled, 0);
	zassert_equal(mg_settings_get()->ptt_buzz_duration_ms, 50, "persisted ones stay");
	const uint8_t back[] = {mg_setting_parameter_ptt_buzz_duration_ms, 100, 0};

	zassert_equal(mg_settings_apply(back, sizeof(back), &bad), 0);
}

/* The capture format (7, 13, 14) is remembered across connections and
 * restarts; a stored value this build refuses is skipped
 * and its default stays. */
ZTEST(settings, test_format_remembered)
{
	Z_TEST_SKIP_IFNDEF(CONFIG_MG_LC3);
	const uint16_t rate = IS_ENABLED(CONFIG_MG_LC3_24K) ? 24000 : 16000;
	const uint8_t fmt[] = {mg_setting_parameter_audio_codec, mg_data_type_audio_lc3,
			       mg_setting_parameter_capture_sample_rate, rate & 0xff, rate >> 8,
			       mg_setting_parameter_capture_bitrate_kbps, 16, 0};
	const uint8_t defaults[] = {mg_setting_parameter_audio_codec, mg_data_type_audio_sbc,
				    mg_setting_parameter_capture_sample_rate, 0x80, 0x3e,
				    mg_setting_parameter_capture_bitrate_kbps, 32, 0};
	uint8_t bad;

	zassert_equal(mg_settings_apply(fmt, sizeof(fmt), &bad), 0);
	mg_settings_new_connection();
	zassert_equal(mg_settings_get()->audio_codec, mg_data_type_audio_lc3, "reconnect keeps it");
	zassert_equal(mg_settings_get()->capture_sample_rate, rate);
	zassert_equal(mg_settings_get()->capture_bitrate_kbps, 16);

	mg_settings_flush();
	mg_settings_reset();
	zassert_equal(mg_settings_get()->audio_codec, mg_data_type_audio_sbc, "defaults in RAM");
	zassert_equal(mg_settings_load(), 0);
	zassert_equal(mg_settings_get()->audio_codec, mg_data_type_audio_lc3, "restart keeps it");
	zassert_equal(mg_settings_get()->capture_sample_rate, rate);
	zassert_equal(mg_settings_get()->capture_bitrate_kbps, 16);

	/* A stored format this build refuses: codec 0x7e, 32 kHz, 500 kb/s. */
	const uint8_t blob[] = {1, mg_setting_parameter_audio_codec, 0x7e,
				mg_setting_parameter_capture_sample_rate, 0x00, 0x7d,
				mg_setting_parameter_capture_bitrate_kbps, 0xf4, 0x01,
				mg_setting_parameter_audio_queue_enabled, 1};

	zassert_equal(settings_save_one("mg/cfg", blob, sizeof(blob)), 0);
	mg_settings_reset();
	zassert_equal(mg_settings_load(), 0);
	zassert_equal(mg_settings_get()->audio_codec, mg_data_type_audio_sbc, "bad codec skipped");
	zassert_equal(mg_settings_get()->capture_sample_rate, 16000, "bad rate skipped");
	zassert_equal(mg_settings_get()->capture_bitrate_kbps, 32, "bad bitrate skipped");
	zassert_equal(mg_settings_get()->audio_queue_enabled, 1, "the rest still loads");

	/* Back to defaults in storage, for the other suites. */
	const uint8_t q0[] = {mg_setting_parameter_audio_queue_enabled, 0};

	zassert_equal(mg_settings_apply(defaults, sizeof(defaults), &bad), 0);
	zassert_equal(mg_settings_apply(q0, sizeof(q0), &bad), 0);
}

ZTEST_SUITE(settings, NULL, setup, before, NULL, NULL);
