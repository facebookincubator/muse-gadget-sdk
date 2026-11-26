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

#ifndef MG_SETTINGS_H_
#define MG_SETTINGS_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mgcommands.h"

struct mg_settings {
	uint8_t push_to_talk_enabled;
	uint8_t haptics_enabled;
	uint16_t ptt_buzz_freq_hz;
	uint16_t ptt_buzz_duration_ms;
	uint8_t ptt_buzz_volume_percent;
	uint8_t audio_codec;
	uint8_t audio_queue_enabled;
	uint16_t capture_sample_rate; /* setting 13, stored */
	uint16_t capture_bitrate_kbps; /* setting 14, stored */
};

/* Who holds the link (settings 18 and 19): what the
 * client wrote on this connection, NUL-terminated ("" when it wrote none). */
const char *mg_settings_client_name(void);
void mg_settings_client_app(const char **name, const char **version);

/* The live settings. Change them only through mg_settings_apply(). */
const struct mg_settings *mg_settings_get(void);
void mg_settings_defaults(struct mg_settings *s);

/* Value width in bytes for a parameter, or 0 if the parameter is unknown. */
size_t mg_settings_value_size(uint8_t param);

/* The device lists and accepts it in get_settings and set_settings: haptics
 * and the ptt_buzz_* settings only with a vibration motor (CONFIG_MG_HAPTICS). */
bool mg_settings_accepted(uint8_t param);
/* Named in the typed settings list and a get_settings with no list:
 * accepted, capture_sample_rate (13) only by a build that captures at more
 * than one rate (CONFIG_MG_LC3_24K) and capture_bitrate_kbps (14) only by
 * one with a choice of bitrates (LC3). A single-rate build
 * still accepts 13 = 16000. */
bool mg_settings_listed(uint8_t param);

/* Encodes one (param, value) pair; returns bytes written or 0 if unknown
 * or out of room. */
size_t mg_settings_encode(const struct mg_settings *s, uint8_t param, uint8_t *out, size_t room);

/*
 * Parses a set_settings payload (after the command byte) and applies each
 * pair in order. Pairs before an error stay applied. Returns 0 or a
 * mg_error_code_t; *bad_param gets the offending parameter.
 */
uint16_t mg_settings_apply(const uint8_t *buf, size_t len, uint8_t *bad_param);
/* As mg_settings_apply(); with skip_persisted every persisted pair is checked
 * for its size but left unchanged, and *skipped says one was. */
uint16_t mg_settings_apply_ex(const uint8_t *buf, size_t len, uint8_t *bad_param,
			      bool skip_persisted, bool *skipped);
/* A set_settings payload names a setting that is stored (survives the
 * connection and a restart). */
bool mg_settings_writes_persisted(const uint8_t *buf, size_t len);

/* True if codec is one this build can capture with. */
bool mg_settings_codec_supported(uint8_t codec);

/* Loads saved settings (call after settings_subsys_init()). */
int mg_settings_load(void);

/* push_to_talk_enabled and haptics_enabled are per connection: back to 0. */
void mg_settings_new_connection(void);
/* Push-to-talk off (per connection, not stored), the rest kept. */
void mg_settings_client_ready_reset(void);

/* Test hook: forget the live settings and reset to defaults, not saved. */
void mg_settings_reset(void);
/* Stores the settings now if a change waits (a planned power off; tests). On
 * a gadget that isn't set up changes reach flash at most once per 10 min. */
void mg_settings_flush(void);

#endif
