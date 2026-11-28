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

#include "mg_codec.h"

#include <string.h>

#include "mgcommands.h"

#define SBC_BLOCKS 16
#define SBC_SUBBANDS 8

static void sbc_setup(struct sbc_frame *f)
{
    memset(f, 0, sizeof(*f));
    f->freq = SBC_FREQ_16K;
    f->mode = SBC_MODE_MONO;
    f->bam = SBC_BAM_LOUDNESS;
    f->nblocks = SBC_BLOCKS;
    f->nsubbands = SBC_SUBBANDS;
    f->bitpool = MG_SBC_BITPOOL;
}

size_t mg_codec_list(uint8_t *out, size_t cap)
{
    size_t n = 0;
    if (n < cap) {
        out[n++] = mg_data_type_audio_sbc;
    }
#if MG_WITH_LC3
    if (n < cap) {
        out[n++] = mg_data_type_audio_lc3;
    }
#endif
    return n;
}

bool mg_codec_supported(uint8_t type)
{
    return type == mg_data_type_audio_sbc || (MG_WITH_LC3 && type == mg_data_type_audio_lc3);
}

static uint16_t sbc_frame_bytes(void)
{
    struct sbc_frame f;
    sbc_setup(&f);
    return (uint16_t)sbc_get_frame_size(&f);
}

bool mg_codec_kbps_range(uint8_t type, uint16_t rate_hz, uint16_t *min_kbps, uint16_t *max_kbps)
{
    if (type == mg_data_type_audio_sbc && rate_hz == MG_CODEC_RATE) {
        /* Fixed bitpool: frame bytes x rate / 16000. */
        *min_kbps = *max_kbps = sbc_frame_bytes();
        return true;
    }
#if MG_WITH_LC3
    if (type == mg_data_type_audio_lc3 && (rate_hz == MG_CODEC_RATE || (MG_LC3_24K && rate_hz == MG_CODEC_RATE_HIGH))) {
        *min_kbps = MG_CODEC_LC3_MIN_KBPS;
        *max_kbps = MG_CODEC_LC3_MAX_KBPS;
        return true;
    }
#endif
    return false;
}

bool mg_codec_format_at(uint8_t type, uint16_t rate_hz, uint16_t kbps, mg_audio_format_t *out)
{
    uint16_t lo, hi;
    if (!mg_codec_supported(type) || !mg_codec_kbps_range(type, rate_hz, &lo, &hi)) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    out->type = type;
    out->rate_hz = rate_hz;
    out->channels = 1;
    if (type == mg_data_type_audio_sbc) {
        out->frame_bytes = sbc_frame_bytes();
        out->frame_samples = SBC_BLOCKS * SBC_SUBBANDS;
        out->frame_us = (uint16_t)(1000000u * out->frame_samples / rate_hz);
    } else {
        kbps = kbps < lo ? lo : kbps > hi ? hi : kbps;
        out->frame_us = MG_LC3_DEFAULT_FRAME_US;
        out->frame_bytes = (uint16_t)((uint32_t)kbps * MG_LC3_DEFAULT_FRAME_US / 8000);   /* floor */
        out->frame_samples = (uint16_t)(rate_hz / 100);
    }
    return true;
}

bool mg_codec_format(uint8_t type, mg_audio_format_t *out)
{
    return mg_codec_format_at(type, MG_CODEC_RATE, MG_CAPTURE_DEFAULT_BITRATE_KBPS, out);
}

size_t mg_codec_change_data_type(uint8_t type, uint8_t out[10])
{
    mg_audio_format_t f;
    if (!mg_codec_format(type, &f)) {
        out[0] = mg_command_change_data_type;
        out[1] = type;
        return 2;
    }
    return mg_codec_label(&f, out);
}

size_t mg_codec_label(const mg_audio_format_t *fmt, uint8_t out[10])
{
    const mg_audio_format_t f = *fmt;
    out[0] = mg_command_change_data_type;
    out[1] = f.type;
    out[2] = (uint8_t)f.rate_hz;
    out[3] = (uint8_t)(f.rate_hz >> 8);
    out[4] = f.channels;
    out[5] = (uint8_t)f.frame_bytes;
    out[6] = (uint8_t)(f.frame_bytes >> 8);
    out[7] = (uint8_t)f.frame_us;
    out[8] = (uint8_t)(f.frame_us >> 8);
    return 9;
}

bool mg_encoder_open(mg_encoder_t *e, uint8_t type)
{
    mg_audio_format_t f;
    return mg_codec_format(type, &f) && mg_encoder_open_fmt(e, &f);
}

bool mg_encoder_open_fmt(mg_encoder_t *e, const mg_audio_format_t *fmt)
{
    memset(e, 0, sizeof(*e));
    e->fmt = *fmt;
    if (fmt->frame_samples > MG_CODEC_MAX_FRAME_SAMPLES || fmt->frame_bytes > MG_CODEC_MAX_FRAME_BYTES) {
        return false;
    }
    if (fmt->type == mg_data_type_audio_sbc) {
        sbc_reset(&e->u.sbc.state);
        sbc_setup(&e->u.sbc.frame);
        return true;
    }
#if MG_WITH_LC3
    if (fmt->type != mg_data_type_audio_lc3) {
        return false;
    }
    /* The library's own size: memory sized under other LC3 layout definitions would be overrun. */
    if (lc3_encoder_size(e->fmt.frame_us, e->fmt.rate_hz) > sizeof(e->u.lc3.mem)) {
        return false;
    }
    e->u.lc3.enc = lc3_setup_encoder(e->fmt.frame_us, e->fmt.rate_hz, 0, &e->u.lc3.mem);
    return e->u.lc3.enc != NULL;
#else
    return false;
#endif
}

/* One frame of fmt.frame_samples at `pcm` into `out`. */
static bool encode_frame(mg_encoder_t *e, const int16_t *pcm, uint8_t *out)
{
    if (e->fmt.type == mg_data_type_audio_sbc) {
        return sbc_encode(&e->u.sbc.state, pcm, 1, NULL, 0, &e->u.sbc.frame, out, e->fmt.frame_bytes) == 0;
    }
#if MG_WITH_LC3
    return lc3_encode(e->u.lc3.enc, LC3_PCM_FORMAT_S16, pcm, 1, e->fmt.frame_bytes, out) == 0;
#else
    return false;
#endif
}

size_t mg_encoder_feed(mg_encoder_t *e, const int16_t *pcm, size_t n, uint8_t *out, size_t cap)
{
    const size_t fs = e->fmt.frame_samples, fb = e->fmt.frame_bytes;
    size_t written = 0;
    if (!fs) {
        return 0;
    }
    while (n) {
        if (written + fb > cap) {
            /* No room: keep what fits in the pending frame, drop the rest. */
            size_t take = fs - e->npending < n ? fs - e->npending : n;
            memcpy(e->pending + e->npending, pcm, take * sizeof(int16_t));
            e->npending += take;
            break;
        }
        if (e->npending == 0 && n >= fs) {
            /* Straight from the input, no copy. */
            if (encode_frame(e, pcm, out + written)) {
                written += fb;
            }
            pcm += fs;
            n -= fs;
            continue;
        }
        size_t take = fs - e->npending < n ? fs - e->npending : n;
        memcpy(e->pending + e->npending, pcm, take * sizeof(int16_t));
        e->npending += take;
        pcm += take;
        n -= take;
        if (e->npending == fs) {
            if (encode_frame(e, e->pending, out + written)) {
                written += fb;
            }
            e->npending = 0;
        }
    }
    return written;
}

size_t mg_encoder_flush(mg_encoder_t *e, uint8_t *out, size_t cap)
{
    if (!e->npending || cap < e->fmt.frame_bytes) {
        e->npending = 0;
        return 0;
    }
    memset(e->pending + e->npending, 0, (e->fmt.frame_samples - e->npending) * sizeof(int16_t));
    e->npending = 0;
    return encode_frame(e, e->pending, out) ? e->fmt.frame_bytes : 0;
}
