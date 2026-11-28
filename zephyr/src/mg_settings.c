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

#include "mg_ota.h"
#include "mg_settings.h"
#include "mg_settings_policy.h"
#include "mg_transport.h"
#include "mg_codec.h"
#include "mg_clock.h"
#include "mg_connparam.h"
#include "mg_setup_store.h"

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/byteorder.h>

LOG_MODULE_REGISTER(mg_settings, CONFIG_MG_LOG_LEVEL);

/* A change waiting for flash (changed(), below). */
static bool dirty;
static int64_t last_save_ms;
static void flush_fn(struct k_work *w);
static K_WORK_DELAYABLE_DEFINE(flush_work, flush_fn);

#define MG_SETTINGS_KEY     "mg/cfg"
#define MG_SETTINGS_VERSION 1

/* Stored blob: version byte, then the wire encoding of each persisted
 * parameter in this order. New parameters go at the end.
 * The capture format (audio_codec 7, capture_sample_rate 13,
 * capture_bitrate_kbps 14) is remembered across connections and restarts;
 * a fresh device or
 * a factory reset has SBC, 16000, 32. push_to_talk_enabled and
 * haptics_enabled are per connection and never stored. */
static const uint8_t persisted[] = {
	mg_setting_parameter_ptt_buzz_freq_hz,
	mg_setting_parameter_ptt_buzz_duration_ms,
	mg_setting_parameter_ptt_buzz_volume_percent,
	mg_setting_parameter_audio_codec,
	mg_setting_parameter_audio_queue_enabled,
	mg_setting_parameter_capture_sample_rate,
	mg_setting_parameter_capture_bitrate_kbps,
};

static bool per_connection(uint8_t param)
{
	return param == mg_setting_parameter_push_to_talk_enabled ||
	       param == mg_setting_parameter_haptics_enabled;
}

static struct mg_settings live;
static bool loaded;

/* Settings 18 and 19: per connection, never stored. */
static struct {
	char name[MG_CLIENT_NAME_MAX + 1];
	char app[MG_CLIENT_APP_FIELD_MAX + 1];
	char ver[MG_CLIENT_APP_FIELD_MAX + 1];
	uint8_t name_len, app_len, ver_len;
} client;

static bool variable(uint8_t param)
{
	return param == mg_setting_parameter_client_name ||
	       param == mg_setting_parameter_client_app;
}

/* A variable value's length at v (avail bytes there): 0 if it's cut short. */
static size_t variable_len(uint8_t param, const uint8_t *v, size_t avail)
{
	if (avail < 1 || avail < 1u + v[0]) {
		return 0;
	}
	if (param == mg_setting_parameter_client_name) {
		return 1u + v[0];
	}
	size_t a = 1u + v[0];

	if (avail < a + 1 || avail < a + 1 + v[a]) {
		return 0;
	}
	return a + 1 + v[a];
}

const char *mg_settings_client_name(void)
{
	return client.name;
}

void mg_settings_client_app(const char **name, const char **version)
{
	*name = client.app;
	*version = client.ver;
}

void mg_settings_defaults(struct mg_settings *s)
{
	s->push_to_talk_enabled = 0;
	s->haptics_enabled = 0;
	s->ptt_buzz_freq_hz = 210;
	s->ptt_buzz_duration_ms = 100;
	s->ptt_buzz_volume_percent = 80;
	s->audio_codec = mg_data_type_audio_sbc;
	s->audio_queue_enabled = 0;
	s->capture_sample_rate = MG_AUDIO_DEFAULT_SAMPLE_RATE_HZ;
	s->capture_bitrate_kbps = MG_CAPTURE_DEFAULT_BITRATE_KBPS;
}

const struct mg_settings *mg_settings_get(void)
{
	if (!loaded) {
		mg_settings_defaults(&live);
		loaded = true;
	}
	return &live;
}

void mg_settings_new_connection(void)
{
	struct mg_settings *s = (struct mg_settings *)mg_settings_get();

	s->push_to_talk_enabled = 0;
	s->haptics_enabled = 0;
	memset(&client, 0, sizeof(client));
}

void mg_settings_client_ready_reset(void)
{
	struct mg_settings *s = (struct mg_settings *)mg_settings_get();

	s->push_to_talk_enabled = 0;
}

void mg_settings_reset(void)
{
	(void)k_work_cancel_delayable(&flush_work);
	dirty = false;
	mg_settings_defaults(&live);
	loaded = true;
}

bool mg_settings_codec_supported(uint8_t codec)
{
	return codec == mg_data_type_audio_sbc ||
	       (IS_ENABLED(CONFIG_MG_LC3) && codec == mg_data_type_audio_lc3);
}

size_t mg_settings_value_size(uint8_t param)
{
	switch (param) {
	case mg_setting_parameter_spec_version:
	case mg_setting_parameter_push_to_talk_enabled:
	case mg_setting_parameter_haptics_enabled:
	case mg_setting_parameter_ptt_buzz_volume_percent:
	case mg_setting_parameter_audio_codec:
	case mg_setting_parameter_audio_queue_enabled:
	case mg_setting_parameter_link_mode:
		return 1;
	case mg_setting_parameter_link_params:
		return 6;
	case mg_setting_parameter_ptt_buzz_freq_hz:
	case mg_setting_parameter_ptt_buzz_duration_ms:
	case mg_setting_parameter_capture_sample_rate:
	case mg_setting_parameter_capture_bitrate_kbps:
		return 2;
	default:
		return 0;
	}
}

bool mg_settings_accepted(uint8_t param)
{
	if (variable(param)) {
		return true;
	}
	switch (param) {
	case mg_setting_parameter_haptics_enabled:
	case mg_setting_parameter_ptt_buzz_freq_hz:
	case mg_setting_parameter_ptt_buzz_duration_ms:
	case mg_setting_parameter_ptt_buzz_volume_percent:
		return IS_ENABLED(CONFIG_MG_HAPTICS);
	default:
		return mg_settings_value_size(param) != 0;
	}
}

bool mg_settings_listed(uint8_t param)
{
	if (param == mg_setting_parameter_client_name) {
		/* Reserved: the apps don't send it, so
		 * no board lists it; still accepted and read back by number. */
		return false;
	}
	if (param == mg_setting_parameter_capture_sample_rate) {
		return IS_ENABLED(CONFIG_MG_LC3_24K);
	}
	if (param == mg_setting_parameter_capture_bitrate_kbps) {
		return IS_ENABLED(CONFIG_MG_LC3);
	}
	return mg_settings_accepted(param);
}

/* Inside some listed codec and rate's bitrate range (else invalid_value). */
static bool kbps_listed(uint16_t kbps)
{
	static const uint8_t codecs[] = {mg_data_type_audio_sbc, mg_data_type_audio_lc3};

	for (size_t i = 0; i < ARRAY_SIZE(codecs); i++) {
		uint16_t rates[MG_CODEC_MAX_RATES];
		size_t n = mg_codec_rates(codecs[i], rates, ARRAY_SIZE(rates));

		for (size_t k = 0; k < n; k++) {
			uint16_t lo, hi;

			if (mg_codec_kbps_range(codecs[i], rates[k], &lo, &hi) == 0 && kbps >= lo &&
			    kbps <= hi) {
				return true;
			}
		}
	}
	return false;
}

/* A rate some codec captures at (else invalid_value). */
static bool rate_listed(uint16_t rate)
{
	return mg_codec_rate_supported(mg_data_type_audio_sbc, rate) ||
	       mg_codec_rate_supported(mg_data_type_audio_lc3, rate);
}

size_t mg_settings_encode(const struct mg_settings *s, uint8_t param, uint8_t *out, size_t room)
{
	size_t n = mg_settings_value_size(param);

	if (param == mg_setting_parameter_client_name) {
		if (room < 2u + client.name_len) {
			return 0;
		}
		out[0] = param;
		out[1] = client.name_len;
		memcpy(&out[2], client.name, client.name_len);
		return 2u + client.name_len;
	}
	if (param == mg_setting_parameter_client_app) {
		if (room < 3u + client.app_len + client.ver_len) {
			return 0;
		}
		out[0] = param;
		out[1] = client.app_len;
		memcpy(&out[2], client.app, client.app_len);
		out[2 + client.app_len] = client.ver_len;
		memcpy(&out[3 + client.app_len], client.ver, client.ver_len);
		return 3u + client.app_len + client.ver_len;
	}

	if (n == 0 || room < 1 + n) {
		return 0;
	}
	out[0] = param;
	switch (param) {
	case mg_setting_parameter_spec_version:
		out[1] = MG_SPEC_VERSION;
		break;
	case mg_setting_parameter_push_to_talk_enabled:
		out[1] = s->push_to_talk_enabled;
		break;
	case mg_setting_parameter_haptics_enabled:
		out[1] = s->haptics_enabled;
		break;
	case mg_setting_parameter_ptt_buzz_freq_hz:
		sys_put_le16(s->ptt_buzz_freq_hz, &out[1]);
		break;
	case mg_setting_parameter_ptt_buzz_duration_ms:
		sys_put_le16(s->ptt_buzz_duration_ms, &out[1]);
		break;
	case mg_setting_parameter_ptt_buzz_volume_percent:
		out[1] = s->ptt_buzz_volume_percent;
		break;
	case mg_setting_parameter_audio_codec:
		out[1] = s->audio_codec;
		break;
	case mg_setting_parameter_audio_queue_enabled:
		out[1] = s->audio_queue_enabled;
		break;
	case mg_setting_parameter_capture_sample_rate:
		sys_put_le16(s->capture_sample_rate, &out[1]);
		break;
	case mg_setting_parameter_capture_bitrate_kbps:
		sys_put_le16(s->capture_bitrate_kbps, &out[1]);
		break;
	case mg_setting_parameter_link_mode:
		/* The effective mode (Link modes, mg_connparam.c). */
		out[1] = mg_connparam_mode();
		break;
	case mg_setting_parameter_link_params: {
		uint16_t itvl, lat, tmo;

		mg_connparam_link_params(&itvl, &lat, &tmo);
		sys_put_le16(itvl, &out[1]);
		sys_put_le16(lat, &out[3]);
		sys_put_le16(tmo, &out[5]);
		break;
	}
	}
	return 1 + n;
}

/* Validates and stores one value. Returns 0 or a mg_error_code_t. */
static uint16_t set_one(struct mg_settings *s, uint8_t param, const uint8_t *v)
{
	/* Only 2-byte values are read as 2 bytes: a 1-byte value may be the
	 * last byte of the request (an ASan host test found the over-read in
	 * the shared code). */
	uint16_t u16 = mg_settings_value_size(param) == 2 ? sys_get_le16(v) : v[0];

	switch (param) {
	case mg_setting_parameter_client_name:
		/* The phone's name as the client knows it, this connection. */
		if (v[0] > MG_CLIENT_NAME_MAX) {
			return mg_error_code_invalid_value;
		}
		client.name_len = v[0];
		memcpy(client.name, &v[1], v[0]);
		client.name[v[0]] = '\0';
		return 0;
	case mg_setting_parameter_client_app: {
		size_t al = v[0], vl = v[1 + al];

		if (al > MG_CLIENT_APP_FIELD_MAX || vl > MG_CLIENT_APP_FIELD_MAX) {
			return mg_error_code_invalid_value;
		}
		client.app_len = (uint8_t)al;
		memcpy(client.app, &v[1], al);
		client.app[al] = '\0';
		client.ver_len = (uint8_t)vl;
		memcpy(client.ver, &v[2 + al], vl);
		client.ver[vl] = '\0';
		return 0;
	}
	case mg_setting_parameter_spec_version:
	case mg_setting_parameter_link_params:
		return mg_error_code_invalid_value; /* read-only */
	case mg_setting_parameter_link_mode:
		/* Per connection, never stored: the mode the client wants. */
		if (v[0] > mg_link_mode_active) {
			return mg_error_code_invalid_value;
		}
		mg_connparam_client_mode(v[0]);
		return 0;
	case mg_setting_parameter_push_to_talk_enabled:
	case mg_setting_parameter_haptics_enabled:
	case mg_setting_parameter_audio_queue_enabled:
		if (v[0] > 1) {
			return mg_error_code_invalid_value;
		}
		if (param == mg_setting_parameter_push_to_talk_enabled) {
			s->push_to_talk_enabled = v[0];
		} else if (param == mg_setting_parameter_haptics_enabled) {
			s->haptics_enabled = v[0];
		} else {
			s->audio_queue_enabled = v[0];
		}
		return 0;
	case mg_setting_parameter_ptt_buzz_freq_hz:
		if (u16 < 20 || u16 > 20000) {
			return mg_error_code_invalid_value;
		}
		s->ptt_buzz_freq_hz = u16;
		return 0;
	case mg_setting_parameter_ptt_buzz_duration_ms:
		if (u16 < 1 || u16 > 5000) {
			return mg_error_code_invalid_value;
		}
		s->ptt_buzz_duration_ms = u16;
		return 0;
	case mg_setting_parameter_ptt_buzz_volume_percent:
		if (v[0] > 100) {
			return mg_error_code_invalid_value;
		}
		s->ptt_buzz_volume_percent = v[0];
		return 0;
	case mg_setting_parameter_audio_codec:
		if (!mg_settings_codec_supported(v[0])) {
			return mg_error_code_invalid_value;
		}
		s->audio_codec = v[0];
		return 0;
	case mg_setting_parameter_capture_sample_rate:
		if (!rate_listed(u16)) {
			return mg_error_code_invalid_value;
		}
		s->capture_sample_rate = u16;
		return 0;
	case mg_setting_parameter_capture_bitrate_kbps:
		if (!kbps_listed(u16)) {
			return mg_error_code_invalid_value;
		}
		s->capture_bitrate_kbps = u16;
		return 0;
	default:
		return mg_error_code_unsupported;
	}
}

static void save(void)
{
	uint8_t blob[1 + ARRAY_SIZE(persisted) * 3];
	size_t n = 0;

	blob[n++] = MG_SETTINGS_VERSION;
	for (size_t i = 0; i < ARRAY_SIZE(persisted); i++) {
		n += mg_settings_encode(&live, persisted[i], &blob[n], sizeof(blob) - n);
	}
	int err = settings_save_one(MG_SETTINGS_KEY, blob, n);

	if (err) {
		LOG_ERR("saving settings failed (%d)", err);
	}
}

/* No flash write per command from a stranger: on a gadget that isn't set up
 * any central may change the stored settings, so they stay in RAM and reach
 * flash at most once per MG_SETTINGS_SAVE_GAP_MS (the window starting at
 * boot), at setup's commit, or before a planned power off. On a set-up
 * gadget changing them needs this connection's token proof, so they're saved
 * at once. All on the app work queue. */

static void save_now(void)
{
	(void)k_work_cancel_delayable(&flush_work);
	dirty = false;
	last_save_ms = k_uptime_get();
	save();
}

/* An unset gadget's clock change waits for the same window (mg_clock.c). */
static bool clock_waits;

static void clock_now(void)
{
	clock_waits = false;
	last_save_ms = k_uptime_get();
	mg_clock_flush();
}

static void flush_fn(struct k_work *w)
{
	ARG_UNUSED(w);
	if (dirty) {
		save_now();
	}
	if (clock_waits) {
		clock_now();
	}
}

void mg_settings_clock_changed(void)
{
	int64_t now = k_uptime_get();

	if (mg_settings_save_due(mg_setup_store_complete(), last_save_ms, now)) {
		clock_now();
		return;
	}
	clock_waits = true;
	k_work_schedule_for_queue(mg_app_wq(), &flush_work,
				  K_MSEC(last_save_ms + MG_SETTINGS_SAVE_GAP_MS - now));
}

static void changed(void)
{
	int64_t now = k_uptime_get();

	if (mg_settings_save_due(mg_setup_store_complete(), last_save_ms, now)) {
		save_now();
		return;
	}
	if (!dirty) {
		dirty = true;
		k_work_schedule_for_queue(mg_app_wq(), &flush_work,
					  K_MSEC(last_save_ms + MG_SETTINGS_SAVE_GAP_MS - now));
	}
}

void mg_settings_flush(void)
{
	if (dirty) {
		save_now();
	}
}

/* Setup committed (mg_setup_store.c): what a central set before is the
 * owner's now; stored at once. */
void mg_setup_store_committed(void)
{
	mg_settings_flush();
	clock_waits = false;
	mg_clock_flush();
#if defined(CONFIG_MG_DFU)
	mg_ota_setup_committed();
#endif
}

bool mg_settings_writes_persisted(const uint8_t *buf, size_t len)
{
	size_t i = 0;

	while (i < len) {
		uint8_t param = buf[i];
		size_t n = variable(param) ? variable_len(param, &buf[i + 1], len - i - 1)
					   : mg_settings_value_size(param);

		for (size_t k = 0; k < ARRAY_SIZE(persisted); k++) {
			if (persisted[k] == param) {
				return true;
			}
		}
		if (n == 0) {
			break; /* malformed: mg_settings_apply() reports it */
		}
		i += 1 + n;
	}
	return false;
}

uint16_t mg_settings_apply(const uint8_t *buf, size_t len, uint8_t *bad_param)
{
	bool skipped;

	return mg_settings_apply_ex(buf, len, bad_param, false, &skipped);
}

uint16_t mg_settings_apply_ex(const uint8_t *buf, size_t len, uint8_t *bad_param,
			      bool skip_persisted, bool *skipped)
{
	struct mg_settings *s = (struct mg_settings *)mg_settings_get();
	struct mg_settings before = *s;
	uint16_t err = 0;
	size_t i = 0;

	*skipped = false;

	while (i < len) {
		uint8_t param = buf[i];
		size_t n = mg_settings_value_size(param);

		*bad_param = param;
		if (variable(param)) {
			n = variable_len(param, &buf[i + 1], len - i - 1);
			if (n == 0) {
				err = mg_error_code_invalid_length;
				break;
			}
		} else if (n == 0 || !mg_settings_accepted(param)) {
			err = mg_error_code_unsupported;
			break;
		}
		if (i + 1 + n > len) {
			err = mg_error_code_invalid_length;
			break;
		}
		if (skip_persisted && !mg_settings_param_per_connection(param)) {
			*skipped = true; /* unchanged, proof_required once */
		} else {
			err = set_one(s, param, &buf[i + 1]);
			if (err) {
				break;
			}
		}
		i += 1 + n;
	}
	/* Store only when a persisted setting changed. */
	struct mg_settings after = *s;

	before.push_to_talk_enabled = after.push_to_talk_enabled = 0;
	before.haptics_enabled = after.haptics_enabled = 0;
	if (memcmp(&before, &after, sizeof(before)) != 0) {
		changed();
	}
	return err;
}

static int load_cb(const char *key, size_t len, settings_read_cb read_cb, void *cb_arg, void *param)
{
	uint8_t blob[64];
	struct mg_settings *s = param;

	ARG_UNUSED(key);
	if (len < 1 || len > sizeof(blob)) {
		return 0;
	}
	ssize_t n = read_cb(cb_arg, blob, len);

	if (n < 1 || blob[0] != MG_SETTINGS_VERSION) {
		LOG_WRN("ignoring saved settings (version %d)", n > 0 ? blob[0] : -1);
		return 0;
	}
	/* Replays the stored pairs through the same validation as the wire,
	 * skipping any per-connection setting an older build stored. A stored
	 * value this build refuses (a codec or rate it doesn't list, a bitrate
	 * outside its ranges, e.g. 24000 after 24 kHz is turned off) is skipped
	 * and the default stays. */
	for (size_t i = 1; i < (size_t)n;) {
		size_t w = mg_settings_value_size(blob[i]);

		if (w == 0 || i + 1 + w > (size_t)n) {
			break;
		}
		if (!per_connection(blob[i])) {
			(void)set_one(s, blob[i], &blob[i + 1]);
		}
		i += 1 + w;
	}
	return 0;
}

int mg_settings_load(void)
{
	struct mg_settings *s = (struct mg_settings *)mg_settings_get();

	mg_settings_defaults(s);
	int err = settings_load_subtree_direct(MG_SETTINGS_KEY, load_cb, s);

	LOG_INF("settings: codec %u queue %u buzz %u Hz %u ms %u%%", s->audio_codec,
		s->audio_queue_enabled, s->ptt_buzz_freq_hz, s->ptt_buzz_duration_ms,
		s->ptt_buzz_volume_percent);
	return err;
}
