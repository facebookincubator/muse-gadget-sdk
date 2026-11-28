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

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mg_config.h"
#include "sbc.h"
#if MG_WITH_LC3
#include "lc3.h"
#endif

/*
 * Voice codecs for musegadgets BLE audio: mono, whole frames.
 *   SBC (default): 16 kHz, 16 blocks, 8 subbands, loudness, bitpool
 *                  MG_SBC_BITPOOL (26: 60-byte frames of 8 ms, 60 kb/s)
 *   LC3:           10 ms frames at 16 or 24 kHz, MG_LC3_DEFAULT_FRAME_BYTES
 *                  (40: 32 kb/s) by default, 16 to MG_CODEC_LC3_MAX_KBPS kb/s
 *                  (frame bytes = kb/s * 10 ms / 8)
 */

#define MG_CODEC_RATE 16000
#define MG_CODEC_RATE_HIGH 24000          /* LC3 only */
#define MG_CODEC_MAX_FRAME_SAMPLES 240    /* 10 ms at 24 kHz */
#define MG_CODEC_MAX_FRAME_BYTES 128
#define MG_CODEC_LC3_MIN_KBPS 16          /* 20 B frames */
#define MG_CODEC_LC3_MAX_KBPS 96          /* 120 B frames */

typedef struct {
    uint8_t type;             /* mg_data_type_t */
    uint16_t rate_hz;
    uint8_t channels;
    uint16_t frame_bytes;
    uint16_t frame_us;
    uint16_t frame_samples;
} mg_audio_format_t;

/* The codecs this build captures with, the default (SBC) first. Returns the count. */
size_t mg_codec_list(uint8_t *out, size_t cap);
bool mg_codec_supported(uint8_t type);
/* Format of a supported codec at 16 kHz and its default bitrate; false otherwise. */
bool mg_codec_format(uint8_t type, mg_audio_format_t *out);
/*
 * A supported codec at `rate_hz` and `kbps`: false if
 * the codec doesn't take that rate here (SBC: 16 kHz; LC3: 16 or 24 kHz). The
 * bitrate is clamped into the codec's range; SBC keeps its fixed bitpool.
 */
bool mg_codec_format_at(uint8_t type, uint16_t rate_hz, uint16_t kbps, mg_audio_format_t *out);
/* The kb/s range a codec captures at a rate (SBC: its fixed bitrate twice). */
bool mg_codec_kbps_range(uint8_t type, uint16_t rate_hz, uint16_t *min_kbps, uint16_t *max_kbps);

/*
 * change_data_type for a codec: [mg_command_change_data_type, type, rate,
 * channels, frame bytes, frame us]. Returns the length (9), or 2 for a type
 * without format parameters.
 */
size_t mg_codec_change_data_type(uint8_t type, uint8_t out[10]);
/* The same for a format: its rate, frame bytes and duration (the label). */
size_t mg_codec_label(const mg_audio_format_t *fmt, uint8_t out[10]);

typedef struct {
    mg_audio_format_t fmt;
    int16_t pending[MG_CODEC_MAX_FRAME_SAMPLES];
    size_t npending;
    union {
        struct {
            sbc_t state;
            struct sbc_frame frame;
        } sbc;
#if MG_WITH_LC3
        struct {
            lc3_encoder_t enc;
            LC3_ENCODER_MEM_T(10000, 24000) mem;   /* 16 or 24 kHz */
        } lc3;
#endif
    } u;
} mg_encoder_t;

bool mg_encoder_open(mg_encoder_t *e, uint8_t type);
/* At a format from mg_codec_format_at (its rate and frame bytes). */
bool mg_encoder_open_fmt(mg_encoder_t *e, const mg_audio_format_t *fmt);

/*
 * Encodes what it can of `pcm` into whole frames appended to `out`, keeping
 * the remainder for the next call. Stops early when `cap` has no room for
 * another frame (the samples stay pending). Returns the bytes written.
 */
size_t mg_encoder_feed(mg_encoder_t *e, const int16_t *pcm, size_t n, uint8_t *out, size_t cap);

/* Pads what's pending with silence into one last frame; returns its bytes (0 if none). */
size_t mg_encoder_flush(mg_encoder_t *e, uint8_t *out, size_t cap);

/* Whole frames that fit one notification payload of `payload` bytes (0 if none does). */
static inline size_t mg_codec_frames_per_chunk(const mg_audio_format_t *fmt, size_t payload)
{
    return fmt->frame_bytes ? payload / fmt->frame_bytes : 0;
}
