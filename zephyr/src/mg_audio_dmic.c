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

#include <errno.h>
#include <string.h>

#include <zephyr/audio/dmic.h>
#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "mg_audio.h"
#if defined(CONFIG_MG_LC3_24K)
#include "mg_resample.h"
#endif

#if defined(CONFIG_MG_DMIC_HW_GAIN_DB) && DT_NODE_HAS_COMPAT(DT_NODELABEL(dmic_dev), nordic_nrf_pdm)
#include <hal/nrf_pdm.h>
#define HW_GAIN 1
#endif

LOG_MODULE_REGISTER(mg_dmic, CONFIG_MG_LOG_LEVEL);

/* The Sense's PDM microphone, mono 16-bit, in blocks of 160 samples: 10 ms
 * at 16 kHz. For 24 kHz (CONFIG_MG_LC3_24K) the PDM runs at 32 kHz,
 * which it makes exactly (32 MHz / 20 / ratio 50; 24 kHz has no exact
 * setting), two 5 ms blocks make 10 ms, and mg_resample.c brings them to
 * 240 samples at 24 kHz. Same slab either way: no RAM for the rate. */
#define BLOCK_BYTES (MG_AUDIO_BLOCK_SAMPLES * sizeof(int16_t))
#define BLOCKS      8

K_MEM_SLAB_DEFINE_STATIC(dmic_slab, BLOCK_BYTES, BLOCKS, 4);

static const struct device *const dmic = DEVICE_DT_GET(DT_NODELABEL(dmic_dev));
static bool running;
static int skip;
static uint16_t out_rate;
#if defined(CONFIG_MG_LC3_24K)
static struct mg_resample rs;
#endif

int mg_audio_src_probe(void)
{
	return device_is_ready(dmic) ? 0 : -ENODEV;
}

int mg_audio_src_start(uint16_t rate)
{
	uint32_t pdm_rate = rate;

	if (rate != 16000 && !(IS_ENABLED(CONFIG_MG_LC3_24K) && rate == 24000)) {
		return -ENOTSUP;
	}
	if (rate == 24000) {
		pdm_rate = 32000;
	}
	struct pcm_stream_cfg stream = {
		.pcm_rate = pdm_rate,
		.pcm_width = 16,
		.block_size = BLOCK_BYTES,
		.mem_slab = &dmic_slab,
	};
	struct dmic_cfg cfg = {
		.io = {
			.min_pdm_clk_freq = 1000000,
			.max_pdm_clk_freq = 3500000,
			.min_pdm_clk_dc = 40,
			.max_pdm_clk_dc = 60,
		},
		.streams = &stream,
		.channel = {
			.req_num_streams = 1,
			.req_num_chan = 1,
		},
	};
	int err;

	if (!device_is_ready(dmic)) {
		return -ENODEV;
	}
	cfg.channel.req_chan_map_lo = dmic_build_channel_map(0, 0, PDM_CHAN_LEFT);
	err = dmic_configure(dmic, &cfg);
	if (err) {
		LOG_ERR("dmic_configure: %d", err);
		return err;
	}
#if defined(HW_GAIN)
	/* The PDM's own gain, 0.5 dB steps around its 0 dB default; the driver
	 * leaves it at 0 dB. Set while stopped, before the start below. */
	nrf_pdm_gain_t g = NRF_PDM_GAIN_DEFAULT + 2 * CONFIG_MG_DMIC_HW_GAIN_DB;

	nrf_pdm_gain_set((NRF_PDM_Type *)DT_REG_ADDR(DT_NODELABEL(dmic_dev)), g, g);
#endif
	err = dmic_trigger(dmic, DMIC_TRIGGER_START);
	if (err) {
		LOG_ERR("dmic start: %d", err);
		return err;
	}
	running = true;
	out_rate = rate;
	/* The first 20 ms carry the mic's power-up transient. */
	skip = rate == 24000 ? 4 : 2;
#if defined(CONFIG_MG_LC3_24K)
	mg_resample_reset(&rs);
#endif
	return 0;
}

static int read_block(int16_t *pcm, int timeout_ms);

int mg_audio_src_read(int16_t *pcm, int timeout_ms)
{
#if defined(CONFIG_MG_LC3_24K)
	if (out_rate == 24000) {
		int16_t in32[2 * MG_AUDIO_BLOCK_SAMPLES]; /* 10 ms at 32 kHz */
		int err = read_block(in32, timeout_ms);

		if (err) {
			return err;
		}
		err = read_block(&in32[MG_AUDIO_BLOCK_SAMPLES], timeout_ms);
		if (err) {
			return err;
		}
		return (int)mg_resample_32_to_24(&rs, in32, ARRAY_SIZE(in32), pcm);
	}
#endif
	int err = read_block(pcm, timeout_ms);

	return err ? err : MG_AUDIO_BLOCK_SAMPLES;
}

static int read_block(int16_t *pcm, int timeout_ms)
{
	void *buf;
	uint32_t size;
	int err;

	do {
		err = dmic_read(dmic, 0, &buf, &size, timeout_ms);
		if (err) {
			return err;
		}
		if (skip > 0) {
			skip--;
			k_mem_slab_free(&dmic_slab, buf);
			continue;
		}
		break;
	} while (true);
	memcpy(pcm, buf, MIN(size, BLOCK_BYTES));
	if (size < BLOCK_BYTES) {
		memset((uint8_t *)pcm + size, 0, BLOCK_BYTES - size);
	}
	k_mem_slab_free(&dmic_slab, buf);
	return 0;
}

void mg_audio_src_stop(void)
{
	void *buf;
	uint32_t size;

	if (!running) {
		return;
	}
	(void)dmic_trigger(dmic, DMIC_TRIGGER_STOP);
	/* Give back whatever was still queued. */
	while (dmic_read(dmic, 0, &buf, &size, 0) == 0) {
		k_mem_slab_free(&dmic_slab, buf);
	}
	running = false;
}
