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

/*
 * Encodes two seconds of a voice-like signal with the firmware's encoder
 * wrapper (components/muse_gadget_ble/mg_codec.c) in the Muse's 20 ms mic
 * chunks, checks the frames, and decodes them back with the vendored
 * decoders. Prints "SNR <codec> <dB>" for the test to judge. With path
 * arguments, also writes the SBC (and LC3) stream there, and the input to
 * input.raw, for an outside decoder.
 */

#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE 1   /* M_PI: glibc declares it only with this (macOS and newlib do anyway) */
#endif
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mg_codec.h"
#include "mgcommands.h"

#define RATE 16000
#define SECS 2
#define N (RATE * SECS)
#define CHUNK 320

static int failures, checks;
#define CHECK(c)                                                                  \
    do {                                                                          \
        checks++;                                                                 \
        if (!(c)) {                                                               \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); \
            failures++;                                                           \
        }                                                                         \
    } while (0)

static int16_t pcm[N];

/* A voiced sound: harmonics of a gliding 110-220 Hz pitch, syllable envelope, formant-ish tilt. */
static void make_signal(void)
{
    double phase = 0;
    unsigned seed = 1;
    for (int i = 0; i < N; i++) {
        double t = (double)i / RATE;
        double f0 = 150 + 50 * sin(2 * M_PI * 1.3 * t);
        phase += 2 * M_PI * f0 / RATE;
        double v = 0;
        for (int h = 1; h <= 20; h++) {
            double fh = h * f0;
            double formant = exp(-pow((fh - 700) / 400, 2)) + 0.6 * exp(-pow((fh - 1800) / 500, 2)) + 0.05;
            v += formant * sin(h * phase) / h;
        }
        double env = 0.5 + 0.5 * sin(2 * M_PI * 3 * t);
        seed = seed * 1103515245u + 12345u;
        double noise = ((int)(seed >> 16 & 0x7fff) - 16384) / 16384.0 * 0.01;
        pcm[i] = (int16_t)(9000 * (v * env + noise));
    }
}

/* SNR in dB of `out` against the input, at the best lag up to 1000 samples. */
static double snr(const int16_t *out, int n)
{
    double best = -100;
    for (int lag = 0; lag < 1000; lag++) {
        double sig = 0, err = 0;
        for (int i = 2000; i + lag < n && i < N; i++) {
            double d = (double)out[i + lag] - pcm[i];
            sig += (double)pcm[i] * pcm[i];
            err += d * d;
        }
        double s = 10 * log10(sig / (err + 1e-9));
        best = s > best ? s : best;
    }
    return best;
}

static uint8_t stream[N];   /* plenty for 2 s at 60 kb/s */
static int16_t out[N + 2000];

static size_t encode(uint8_t type, int *frames)
{
    static mg_encoder_t e;
    CHECK(mg_encoder_open(&e, type));
    size_t n = 0;
    for (int i = 0; i < N; i += CHUNK) {
        n += mg_encoder_feed(&e, pcm + i, CHUNK, stream + n, sizeof(stream) - n);
    }
    n += mg_encoder_flush(&e, stream + n, sizeof(stream) - n);
    *frames = (int)(n / e.fmt.frame_bytes);
    CHECK(n % e.fmt.frame_bytes == 0);
    return n;
}

static void test_sbc(const char *path)
{
    mg_audio_format_t f;
    CHECK(mg_codec_format(mg_data_type_audio_sbc, &f));
    CHECK(f.frame_bytes == 60 && f.frame_samples == 128 && f.frame_us == 8000 && f.rate_hz == 16000);
    int frames;
    size_t n = encode(mg_data_type_audio_sbc, &frames);
    CHECK(frames == (N + 127) / 128);
    /* Every frame is standard SBC: syncword, 16 kHz, 16 blocks, mono, loudness, 8 subbands, bitpool 26. */
    for (int i = 0; i < frames; i++) {
        const uint8_t *fr = stream + i * f.frame_bytes;
        CHECK(fr[0] == 0x9C && fr[1] == 0x31 && fr[2] == MG_SBC_BITPOOL);
        struct sbc_frame pf;
        CHECK(sbc_probe(fr, &pf) == 0 && pf.nblocks == 16 && pf.nsubbands == 8 && pf.mode == SBC_MODE_MONO
              && pf.freq == SBC_FREQ_16K && pf.bam == SBC_BAM_LOUDNESS && !pf.msbc);
    }
    static sbc_t d;
    sbc_reset(&d);
    int o = 0;
    for (int i = 0; i < frames; i++) {
        struct sbc_frame pf;
        CHECK(sbc_decode(&d, stream + i * f.frame_bytes, f.frame_bytes, &pf, out + o, 1, NULL, 0) == 0);
        o += 128;
    }
    printf("SNR sbc %.1f\n", snr(out, o));
    if (path) {
        FILE *fp = fopen(path, "wb");
        CHECK(fp && fwrite(stream, 1, n, fp) == n);
        if (fp) fclose(fp);
        fp = fopen("input.raw", "wb");
        if (fp) {
            fwrite(pcm, 2, N, fp);
            fclose(fp);
        }
    }

    /* Whole frames per notification: 1 at the minimum MTU (97 B), 4 at 247 (244 B). */
    CHECK(mg_codec_frames_per_chunk(&f, 97) == 1 && mg_codec_frames_per_chunk(&f, 244) == 4);
    CHECK(mg_codec_frames_per_chunk(&f, 97 - 21) == 1);   /* encrypted */

    /* Feeding with no room keeps the samples for later. */
    static mg_encoder_t e;
    mg_encoder_open(&e, mg_data_type_audio_sbc);
    uint8_t small[60];
    CHECK(mg_encoder_feed(&e, pcm, 100, small, 0) == 0 && e.npending == 100);
    CHECK(mg_encoder_feed(&e, pcm + 100, 28, small, sizeof(small)) == 60 && e.npending == 0);

    uint8_t m[10];
    CHECK(mg_codec_change_data_type(mg_data_type_audio_sbc, m) == 9);
    CHECK(!memcmp(m, "\x08\x01\x80\x3e\x01\x3c\x00\x40\x1f", 9));
    CHECK(mg_codec_change_data_type(mg_data_type_audio_queue, m) == 2 && m[1] == 23);
    CHECK(!mg_codec_supported(mg_data_type_audio_opus) && !mg_encoder_open(&e, mg_data_type_audio_pcm));
}

#if MG_WITH_LC3
static void test_lc3(const char *path)
{
    mg_audio_format_t f;
    CHECK(mg_codec_format(mg_data_type_audio_lc3, &f));
    CHECK(f.frame_bytes == MG_LC3_DEFAULT_FRAME_BYTES && f.frame_us == MG_LC3_DEFAULT_FRAME_US
          && f.frame_samples == 160);
    int frames;
    size_t n = encode(mg_data_type_audio_lc3, &frames);
    if (path) {
        FILE *fp = fopen(path, "wb");
        CHECK(fp && fwrite(stream, 1, n, fp) == n);
        if (fp) fclose(fp);
    }
    CHECK(frames == N / 160);
    static lc3_decoder_mem_16k_t mem;
    lc3_decoder_t d = lc3_setup_decoder(10000, 16000, 0, &mem);
    CHECK(d != NULL);
    int o = 0;
    for (int i = 0; d && i < frames; i++) {
        CHECK(lc3_decode(d, stream + i * f.frame_bytes, f.frame_bytes, LC3_PCM_FORMAT_S16, out + o, 1) == 0);
        o += 160;
    }
    printf("SNR lc3 %.1f\n", snr(out, o));
    uint8_t m[10];
    CHECK(mg_codec_change_data_type(mg_data_type_audio_lc3, m) == 9);
    CHECK(!memcmp(m, "\x08\x13\x80\x3e\x01\x28\x00\x10\x27", 9));
    uint8_t list[4];
    CHECK(mg_codec_list(list, 4) == 2 && list[0] == mg_data_type_audio_sbc && list[1] == mg_data_type_audio_lc3);
}
#endif

int main(int argc, char **argv)
{
    make_signal();
    test_sbc(argc > 1 ? argv[1] : NULL);
#if MG_WITH_LC3
    test_lc3(argc > 2 ? argv[2] : NULL);
#endif
    if (failures) {
        fprintf(stderr, "FAIL: %d checks\n", failures);
        return 1;
    }
    printf("PASS mg_codec (%d checks)\n", checks);
    return 0;
}
