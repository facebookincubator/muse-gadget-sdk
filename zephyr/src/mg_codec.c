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

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>

#include <sbc.h>
#if defined(CONFIG_MG_LC3)
#include <lc3.h>
#endif

/* SBC per mgcommands.h: 16 kHz mono, 16 blocks, 8 subbands, loudness. */
static const struct sbc_frame sbc_cfg = {
	.msbc = false,
	.freq = SBC_FREQ_16K,
	.mode = SBC_MODE_MONO,
	.bam = SBC_BAM_LOUDNESS,
	.nblocks = 16,
	.nsubbands = 8,
	.bitpool = CONFIG_MG_SBC_BITPOOL,
};

static uint8_t cur_type;
static uint16_t cur_bytes; /* LC3 frame bytes */
static sbc_t sbc;
#if defined(CONFIG_MG_LC3)
/* Sized for the highest rate this build captures at. */
#if defined(CONFIG_MG_LC3_24K)
static LC3_ENCODER_MEM_T(10000, 24000) lc3_mem;
#else
static LC3_ENCODER_MEM_T(10000, 16000) lc3_mem;
#endif
static lc3_encoder_t lc3_enc;
#endif

size_t mg_codec_rates(uint8_t type, uint16_t *rates, size_t max)
{
	size_t n = 0;

	switch (type) {
	case mg_data_type_audio_sbc:
		break;
#if defined(CONFIG_MG_LC3)
	case mg_data_type_audio_lc3:
		break;
#endif
	default:
		return 0;
	}
	if (n < max) {
		rates[n++] = MG_AUDIO_DEFAULT_SAMPLE_RATE_HZ;
	}
#if defined(CONFIG_MG_LC3_24K)
	if (type == mg_data_type_audio_lc3 && n < max) {
		rates[n++] = MG_LC3_HIGH_SAMPLE_RATE_HZ;
	}
#endif
	return n;
}

bool mg_codec_rate_supported(uint8_t type, uint16_t rate)
{
	uint16_t rates[MG_CODEC_MAX_RATES];
	size_t n = mg_codec_rates(type, rates, ARRAY_SIZE(rates));

	for (size_t i = 0; i < n; i++) {
		if (rates[i] == rate) {
			return true;
		}
	}
	return false;
}

int mg_codec_kbps_range(uint8_t type, uint16_t rate, uint16_t *min, uint16_t *max)
{
	if (!mg_codec_rate_supported(type, rate)) {
		return -ENOTSUP;
	}
	if (type == mg_data_type_audio_sbc) {
		/* Fixed: frame bytes * 8 / frame duration. */
		unsigned int us = sbc_cfg.nblocks * sbc_cfg.nsubbands * 1000000U / rate;

		*min = *max = (uint16_t)(sbc_get_frame_size(&sbc_cfg) * 8000U / us);
		return 0;
	}
#if defined(CONFIG_MG_LC3)
	*min = MG_LC3_MIN_KBPS;
	*max = CONFIG_MG_LC3_MAX_KBPS;
	return 0;
#else
	return -ENOTSUP;
#endif
}

int mg_codec_format_rate(uint8_t type, uint16_t rate, struct mg_audio_format *fmt)
{
	return mg_codec_format_cfg(type, rate, MG_CAPTURE_DEFAULT_BITRATE_KBPS, fmt);
}

int mg_codec_format_cfg(uint8_t type, uint16_t rate, uint16_t kbps, struct mg_audio_format *fmt)
{
	uint16_t lo, hi;

	memset(fmt, 0, sizeof(*fmt));
	if (mg_codec_kbps_range(type, rate, &lo, &hi)) {
		return -ENOTSUP;
	}
	kbps = CLAMP(kbps, lo, hi);
	fmt->type = type;
	fmt->sample_rate = rate;
	fmt->channels = MG_AUDIO_DEFAULT_CHANNELS;
	switch (type) {
	case mg_data_type_audio_sbc:
		fmt->frame_samples = sbc_cfg.nblocks * sbc_cfg.nsubbands;
		fmt->frame_bytes = sbc_get_frame_size(&sbc_cfg);
		fmt->frame_us = fmt->frame_samples * 1000000U / fmt->sample_rate;
		return 0;
#if defined(CONFIG_MG_LC3)
	case mg_data_type_audio_lc3:
		/* 10 ms frames; bytes = floor(kb/s * 10000 / 8000), 20..400
		 * (16 kb/s 20 B, 32 kb/s 40 B). */
		fmt->frame_samples = rate / 100;
		fmt->frame_bytes = CLAMP(kbps * MG_LC3_DEFAULT_FRAME_US / 8000U,
					 MG_LC3_MIN_FRAME_BYTES, MG_LC3_MAX_FRAME_BYTES);
		fmt->frame_us = MG_LC3_DEFAULT_FRAME_US;
		return 0;
#endif
	default:
		return -ENOTSUP;
	}
}

int mg_codec_format(uint8_t type, struct mg_audio_format *fmt)
{
	return mg_codec_format_rate(type, MG_AUDIO_DEFAULT_SAMPLE_RATE_HZ, fmt);
}

int mg_codec_open(uint8_t type)
{
	struct mg_audio_format fmt;
	int err = mg_codec_format(type, &fmt);

	return err ? err : mg_codec_open_fmt(&fmt);
}

int mg_codec_open_fmt(const struct mg_audio_format *fmt)
{
	uint8_t type = fmt->type;
	uint16_t rate = fmt->sample_rate;

	if (!mg_codec_rate_supported(type, rate)) {
		return -ENOTSUP;
	}
	switch (type) {
	case mg_data_type_audio_sbc:
		sbc_reset(&sbc);
		break;
#if defined(CONFIG_MG_LC3)
	case mg_data_type_audio_lc3:
		/* The library's own size: memory sized under other LC3 layout
		 * definitions would be overrun.
		 */
		if (lc3_encoder_size(MG_LC3_DEFAULT_FRAME_US, rate) > sizeof(lc3_mem)) {
			return -ENOMEM;
		}
		lc3_enc = lc3_setup_encoder(MG_LC3_DEFAULT_FRAME_US, rate, 0, &lc3_mem);
		if (lc3_enc == NULL) {
			return -EIO;
		}
		cur_bytes = fmt->frame_bytes;
		break;
#endif
	default:
		return -ENOTSUP;
	}
	cur_type = type;
	return 0;
}

int mg_codec_encode(const int16_t *pcm, uint8_t *out, size_t room)
{
	switch (cur_type) {
	case mg_data_type_audio_sbc: {
		unsigned int n = sbc_get_frame_size(&sbc_cfg);

		if (room < n) {
			return -ENOMEM;
		}
		if (sbc_encode(&sbc, pcm, 1, NULL, 0, &sbc_cfg, out, n) < 0) {
			return -EIO;
		}
		return n;
	}
#if defined(CONFIG_MG_LC3)
	case mg_data_type_audio_lc3:
		if (room < cur_bytes) {
			return -ENOMEM;
		}
		if (lc3_encode(lc3_enc, LC3_PCM_FORMAT_S16, pcm, 1, cur_bytes, out) < 0) {
			return -EIO;
		}
		return cur_bytes;
#endif
	default:
		return -EINVAL;
	}
}

size_t mg_codec_change_data_type(const struct mg_audio_format *fmt, uint8_t *out)
{
	out[0] = mg_command_change_data_type;
	out[1] = fmt->type;
	if (fmt->sample_rate == 0) {
		return 2; /* non-audio types carry no format */
	}
	sys_put_le16(fmt->sample_rate, &out[2]);
	out[4] = fmt->channels;
	sys_put_le16(fmt->frame_bytes, &out[5]);
	sys_put_le16(fmt->frame_us, &out[7]);
	return 9;
}
