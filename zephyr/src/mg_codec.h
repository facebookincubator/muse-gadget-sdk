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

#ifndef MG_CODEC_H_
#define MG_CODEC_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mgcommands.h"

#if defined(CONFIG_MG_LC3_24K)
#define MG_CODEC_MAX_FRAME_SAMPLES 240 /* LC3, 10 ms at 24 kHz */
#else
#define MG_CODEC_MAX_FRAME_SAMPLES 160
#endif
#define MG_CODEC_MAX_FRAME_BYTES   400

/* Capture rates per codec: 16000 first; LC3 adds
 * 24000 with CONFIG_MG_LC3_24K. SBC is 16 kHz only. */
#define MG_CODEC_MAX_RATES 2
#define MG_LC3_MIN_KBPS    16 /* 20 B per 10 ms, MG_LC3_MIN_FRAME_BYTES */

/* Format parameters announced in change_data_type. */
struct mg_audio_format {
	uint8_t type;         /* mg_data_type_t */
	uint16_t sample_rate; /* Hz */
	uint8_t channels;
	uint16_t frame_bytes; /* 0 = variable */
	uint16_t frame_us;
	uint16_t frame_samples;
};

/* Fills in the format a codec encodes with at 16 kHz. Returns -ENOTSUP for
 * codecs this build lacks. */
int mg_codec_format(uint8_t type, struct mg_audio_format *fmt);
/* The same at a capture rate (default bitrate); -ENOTSUP for a rate the
 * codec doesn't list. */
int mg_codec_format_rate(uint8_t type, uint16_t rate, struct mg_audio_format *fmt);
/* At a rate and a bitrate in kb/s, clamped into the codec's range for that
 * rate (clamp, never refuse); a fixed-bitrate codec (SBC)
 * keeps its own. LC3 frame bytes = floor(kb/s * 10000 / 8000). */
int mg_codec_format_cfg(uint8_t type, uint16_t rate, uint16_t kbps, struct mg_audio_format *fmt);
/* The bitrate range in kb/s for a codec and rate (min == max: fixed). */
int mg_codec_kbps_range(uint8_t type, uint16_t rate, uint16_t *min, uint16_t *max);
/* The rates a codec captures at, 16000 first; returns the count (0: codec
 * not in this build). */
size_t mg_codec_rates(uint8_t type, uint16_t *rates, size_t max);
bool mg_codec_rate_supported(uint8_t type, uint16_t rate);

/* Encoder. One instance; not thread-safe. */
int mg_codec_open(uint8_t type);
/* Opens the encoder for a format from mg_codec_format*(). */
int mg_codec_open_fmt(const struct mg_audio_format *fmt);
/* Encodes fmt.frame_samples samples; returns bytes written or <0. */
int mg_codec_encode(const int16_t *pcm, uint8_t *out, size_t room);

/* Builds [change_data_type, type, rate, channels, frame_bytes, frame_us]. */
size_t mg_codec_change_data_type(const struct mg_audio_format *fmt, uint8_t *out);

#endif
