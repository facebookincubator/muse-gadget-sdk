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

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/ztest.h>

#include <lc3.h>
#include <sbc.h>

#include "mg_codec.h"

/*
 * Encodes one second of a two-tone signal with each codec, decodes it with
 * the vendored decoder and checks the frame format and the SNR at the best
 * alignment. The encoded streams are also printed (MGSTREAM lines) so
 * tests/tools/xcheck.py can decode them with the upstream reference
 * decoders.
 */

#define RATE    16000
#define SECONDS 1
#define N       (RATE * SECONDS)

static int16_t pcm_in[N];
static int16_t pcm_out[N + 400];
static uint8_t stream[16000];

static void make_signal(void)
{
	for (int i = 0; i < N; i++) {
		float t = (float)i / RATE;

		pcm_in[i] = (int16_t)(6000.0f * sinf(2 * 3.14159265f * 440.0f * t) +
				      3000.0f * sinf(2 * 3.14159265f * 1500.0f * t));
	}
}

/* SNR (dB) of out against in, at the lag in [0, max_lag] that fits best,
 * skipping the first 50 ms. */
static double best_snr(const int16_t *out, int n_out, int max_lag, int *lag_out)
{
	double best = -100;

	for (int lag = 0; lag <= max_lag; lag++) {
		double sig = 0, err = 0;

		for (int i = 800; i + lag < n_out && i < N; i++) {
			double d = (double)out[i + lag] - pcm_in[i];

			sig += (double)pcm_in[i] * pcm_in[i];
			err += d * d;
		}
		double snr = 10 * log10(sig / (err + 1e-9));

		if (snr > best) {
			best = snr;
			*lag_out = lag;
		}
	}
	return best;
}

static void dump(const char *name, const uint8_t *b, size_t n)
{
	char line[2 * 64 + 1];

	for (size_t off = 0; off < n; off += 64) {
		size_t m = MIN((size_t)64, n - off);

		for (size_t i = 0; i < m; i++) {
			snprintf(&line[2 * i], 3, "%02x", b[off + i]);
		}
		printk("MGSTREAM %s %s\n", name, line);
	}
}

ZTEST(codec, test_sbc_roundtrip)
{
	struct mg_audio_format f;
	size_t len = 0;
	int nframes = 0;

	make_signal();
	zassert_equal(mg_codec_format(mg_data_type_audio_sbc, &f), 0);
	zassert_equal(mg_codec_open(mg_data_type_audio_sbc), 0);
	for (int i = 0; i + f.frame_samples <= N; i += f.frame_samples) {
		int n = mg_codec_encode(&pcm_in[i], &stream[len], sizeof(stream) - len);

		zassert_equal(n, 60);
		/* Header: syncword, 16 kHz / 16 blocks / mono / loudness / 8 subbands,
		 * bitpool. */
		zassert_equal(stream[len], 0x9c);
		zassert_equal(stream[len + 1], 0x31, "got 0x%02x", stream[len + 1]);
		zassert_equal(stream[len + 2], CONFIG_MG_SBC_BITPOOL);
		len += n;
		nframes++;
	}
	dump("sbc", stream, len);

	/* Decode with the vendored decoder. */
	sbc_t dec;
	struct sbc_frame fr;
	int out = 0;

	sbc_reset(&dec);
	for (size_t off = 0; off < len; off += 60) {
		zassert_equal(sbc_probe(&stream[off], &fr), 0);
		zassert_false(fr.msbc);
		zassert_equal(fr.freq, SBC_FREQ_16K);
		zassert_equal(fr.mode, SBC_MODE_MONO);
		zassert_equal(fr.bam, SBC_BAM_LOUDNESS);
		zassert_equal(fr.nblocks, 16);
		zassert_equal(fr.nsubbands, 8);
		zassert_equal(sbc_get_frame_size(&fr), 60);
		zassert_equal(sbc_decode(&dec, &stream[off], 60, &fr, &pcm_out[out], 1, NULL, 0), 0);
		out += 128;
	}
	int lag = 0;
	double snr = best_snr(pcm_out, out, 200, &lag);

	printk("SBC: %d frames, %u bytes (%u kb/s), SNR %.1f dB at lag %d\n", nframes,
	       (unsigned)len, (unsigned)(len * 8 / SECONDS / 1000), snr, lag);
	zassert_true(snr > 20.0, "SBC SNR %.1f dB", snr);
}

ZTEST(codec, test_lc3_roundtrip)
{
	static LC3_DECODER_MEM_T(10000, 16000) dmem;
	struct mg_audio_format f;
	size_t len = 0;
	int out = 0;

	make_signal();
	zassert_equal(mg_codec_format(mg_data_type_audio_lc3, &f), 0);
	zassert_equal(mg_codec_open(mg_data_type_audio_lc3), 0);
	for (int i = 0; i + f.frame_samples <= N; i += f.frame_samples) {
		int n = mg_codec_encode(&pcm_in[i], &stream[len], sizeof(stream) - len);

		zassert_equal(n, 40);
		len += n;
	}
	dump("lc3", stream, len);

	lc3_decoder_t dec = lc3_setup_decoder(10000, 16000, 0, &dmem);

	zassert_not_null(dec);
	for (size_t off = 0; off < len; off += 40) {
		zassert_equal(lc3_decode(dec, &stream[off], 40, LC3_PCM_FORMAT_S16, &pcm_out[out], 1),
			      0, "frame at %u failed", (unsigned)off);
		out += 160;
	}
	int lag = 0;
	double snr = best_snr(pcm_out, out, 200, &lag);

	printk("LC3: %u frames, %u bytes (%u kb/s), SNR %.1f dB at lag %d\n",
	       (unsigned)(len / 40), (unsigned)len, (unsigned)(len * 8 / SECONDS / 1000), snr, lag);
	zassert_true(snr > 12.0, "LC3 SNR %.1f dB", snr);
}

/* LC3 at 16 kb/s (20 B frames), the low-bitrate clip format: decodes with
 * Google's liblc3 to the signal. */
ZTEST(codec, test_lc3_16kbps_roundtrip)
{
	static LC3_DECODER_MEM_T(10000, 16000) dmem;
	struct mg_audio_format f;
	size_t len = 0;
	int out = 0;

	make_signal();
	zassert_equal(mg_codec_format_cfg(mg_data_type_audio_lc3, 16000, 16, &f), 0);
	zassert_equal(mg_codec_open_fmt(&f), 0);
	for (int i = 0; i + f.frame_samples <= N; i += f.frame_samples) {
		int n = mg_codec_encode(&pcm_in[i], &stream[len], sizeof(stream) - len);

		zassert_equal(n, 20);
		len += n;
	}
	dump("lc3-16k", stream, len);

	lc3_decoder_t dec = lc3_setup_decoder(10000, 16000, 0, &dmem);

	zassert_not_null(dec);
	for (size_t off = 0; off < len; off += 20) {
		zassert_equal(lc3_decode(dec, &stream[off], 20, LC3_PCM_FORMAT_S16, &pcm_out[out], 1),
			      0, "frame at %u failed", (unsigned)off);
		out += 160;
	}
	int lag = 0;
	double snr = best_snr(pcm_out, out, 200, &lag);

	printk("LC3 16 kb/s: %u frames, %u bytes (%u kb/s), SNR %.1f dB at lag %d\n",
	       (unsigned)(len / 20), (unsigned)len, (unsigned)(len * 8 / SECONDS / 1000), snr, lag);
	zassert_equal(len * 8 / SECONDS / 1000, 16);
	zassert_true(snr > 6.0, "LC3 16 kb/s SNR %.1f dB", snr);
}

/* LC3 at 24 kHz: 240 samples into 40 B (32 kb/s), labeled 24000;
 * decodes with Google's liblc3 at 24 kHz to the signal. */
ZTEST(codec, test_lc3_24k_roundtrip)
{
	Z_TEST_SKIP_IFNDEF(CONFIG_MG_LC3_24K);
	static LC3_DECODER_MEM_T(10000, 24000) dmem;
	static int16_t in24[24000], out24[24000 + 480];
	struct mg_audio_format f;
	uint8_t lbl[9];
	size_t len = 0;
	int out = 0;

	for (int i = 0; i < 24000; i++) {
		float t = (float)i / 24000;

		in24[i] = (int16_t)(6000.0f * sinf(2 * 3.14159265f * 440.0f * t) +
				    3000.0f * sinf(2 * 3.14159265f * 9000.0f * t));
	}
	zassert_equal(mg_codec_format_rate(mg_data_type_audio_lc3, 24000, &f), 0);
	zassert_equal(f.frame_samples, 240);
	zassert_equal(f.frame_bytes, 40);
	zassert_equal(mg_codec_change_data_type(&f, lbl), 9);
	const uint8_t want[] = {mg_command_change_data_type, mg_data_type_audio_lc3, 0xc0, 0x5d, 1,
				40, 0, 0x10, 0x27};
	zassert_mem_equal(lbl, want, sizeof(want));
	zassert_equal(mg_codec_open_fmt(&f), 0);
	for (int i = 0; i + 240 <= 24000; i += 240) {
		zassert_equal(mg_codec_encode(&in24[i], &stream[len], sizeof(stream) - len), 40);
		len += 40;
	}
	dump("lc3-24k", stream, len);

	lc3_decoder_t dec = lc3_setup_decoder(10000, 24000, 0, &dmem);

	zassert_not_null(dec);
	for (size_t off = 0; off < len; off += 40) {
		zassert_equal(lc3_decode(dec, &stream[off], 40, LC3_PCM_FORMAT_S16, &out24[out], 1),
			      0);
		out += 240;
	}
	/* SNR at the best lag, skipping 50 ms (9 kHz needs 24 kHz to survive). */
	double best = -100;
	int best_lag = 0;

	for (int lag = 0; lag <= 300; lag++) {
		double sig = 0, err = 0;

		for (int i = 1200; i + lag < out && i < 24000; i++) {
			double d = (double)out24[i + lag] - in24[i];

			sig += (double)in24[i] * in24[i];
			err += d * d;
		}
		double snr = 10 * log10(sig / (err + 1e-9));

		if (snr > best) {
			best = snr;
			best_lag = lag;
		}
	}
	printk("LC3 24 kHz: %u frames, %u bytes, SNR %.1f dB at lag %d\n", (unsigned)(len / 40),
	       (unsigned)len, best, best_lag);
	zassert_true(best > 10.0, "LC3 24 kHz SNR %.1f dB", best);
	zassert_equal(mg_codec_format_rate(mg_data_type_audio_lc3, 32000, &f), -ENOTSUP);
}

ZTEST(codec, test_bad_codec)
{
	uint8_t out[64];
	int16_t pcm[160] = {0};

	zassert_equal(mg_codec_open(mg_data_type_audio_opus), -ENOTSUP);
	zassert_equal(mg_codec_open(mg_data_type_audio_sbc), 0);
	zassert_equal(mg_codec_encode(pcm, out, 10), -ENOMEM, "no room for a frame");
}

ZTEST_SUITE(codec, NULL, NULL, NULL, NULL, NULL);
