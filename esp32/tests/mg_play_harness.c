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
 * Playback (components/muse_gadget_ble/mg_play.c, mg_resample.c) on the host:
 * the resampler's frequency response and aliasing at every supported rate,
 * the ring's flow control, prebuffer, underruns, drain and drop, and the
 * decoder end to end on SBC, PCM and LC3 streams made with the real encoders.
 */

#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE 1   /* M_PI: glibc declares it only with this (macOS and newlib do anyway) */
#endif
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mg_play.h"
#include "mg_resample.h"
#include "mgcommands.h"
#include "sbc.h"

static int failures, checks;
#define CHECK(c)                                                                  \
    do {                                                                          \
        checks++;                                                                 \
        if (!(c)) {                                                               \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); \
            failures++;                                                           \
        }                                                                         \
    } while (0)

/* Amplitude of `f` in x (Goertzel), as a fraction of full scale. */
static double tone(const int16_t *x, size_t n, double f, double rate)
{
    double k = 2 * cos(2 * M_PI * f / rate), s1 = 0, s2 = 0;
    for (size_t i = 0; i < n; i++) {
        double s0 = x[i] + k * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    double p = s1 * s1 + s2 * s2 - k * s1 * s2;
    return 2 * sqrt(p > 0 ? p : 0) / n / 32768.0;
}

static void sine(int16_t *x, size_t n, double f, double rate, double amp)
{
    for (size_t i = 0; i < n; i++) {
        x[i] = (int16_t)lrint(amp * 32767 * sin(2 * M_PI * f * i / rate));
    }
}

/* Resamples a whole signal in odd-sized chunks. Returns output samples. */
static size_t resample_all(uint32_t in_rate, const int16_t *in, size_t n, int16_t *out, size_t cap)
{
    mg_resample_t r;
    CHECK(mg_resample_open(&r, in_rate, 16000));
    static const size_t steps[] = { 7, 100, 1024, 333, 1 };
    size_t i = 0, o = 0, s = 0;
    while (i < n) {
        size_t k = steps[s++ % 5];
        k = k < n - i ? k : n - i;
        o += mg_resample_run(&r, in + i, k, out + o, cap - o);
        i += k;
    }
    mg_resample_close(&r);
    return o;
}

static void test_resampler(void)
{
    static const uint32_t rates[] = { 8000, 16000, 32000, 44100, 48000 };
    static int16_t in[48000], out[20000];
    for (size_t ri = 0; ri < 5; ri++) {
        uint32_t rate = rates[ri];
        sine(in, rate, 1000, rate, 0.5);
        size_t o = resample_all(rate, in, rate, out, 20000);
        /* One second in is one second out, less the filter's delay at most. */
        CHECK(o <= 16001 && o >= 15900);
        double a = tone(out + 2000, o - 4000, 1000, 16000);
        double off = tone(out + 2000, o - 4000, 1300, 16000);
        printf("resample %u->16000: %zu samples, 1 kHz at %.3f (want 0.5), 1.3 kHz %.4f, %zu B of tables\n", rate, o, a,
               off, mg_resample_bytes(rate, 16000));
        CHECK(fabs(a - 0.5) < 0.02);
        CHECK(off < 0.005);
        /* A 3 kHz tone passes too (inside 0.92 of 4 kHz for the 8 kHz input). */
        sine(in, rate, 3000, rate, 0.5);
        o = resample_all(rate, in, rate, out, 20000);
        double a3 = tone(out + 2000, o - 4000, 3000, 16000);
        CHECK(a3 > (rate == 8000 ? 0.3 : 0.47) && a3 < 0.52);
    }
    /* Aliasing: tones above 8 kHz must not fold back into the output. */
    static const struct { uint32_t rate; double f, alias; } al[] = {
        { 48000, 12000, 4000 }, { 32000, 11000, 5000 }, { 44100, 10000, 6000 }, { 48000, 9500, 6500 },
    };
    for (size_t i = 0; i < sizeof(al) / sizeof(al[0]); i++) {
        sine(in, al[i].rate, al[i].f, al[i].rate, 0.5);
        size_t o = resample_all(al[i].rate, in, al[i].rate, out, 20000);
        double a = tone(out + 2000, o - 4000, al[i].alias, 16000);
        printf("alias %u Hz tone at %u -> %.0f Hz: %.1f dB\n", (unsigned)al[i].f, al[i].rate, al[i].alias,
               20 * log10(a / 0.5 + 1e-12));
        CHECK(20 * log10(a / 0.5 + 1e-12) < -50);
    }
    /* Upsampling 8 kHz: the image at 16 - 1 = 15 kHz doesn't exist at 16 kHz, but 7 kHz images of 1 kHz do. */
    sine(in, 8000, 1000, 8000, 0.5);
    size_t o = resample_all(8000, in, 8000, out, 20000);
    CHECK(20 * log10(tone(out + 2000, o - 4000, 7000, 16000) / 0.5 + 1e-12) < -50);
    {
        /* A reset starts over from silence on the same tables: its output is a fresh resampler's. A resampler
         * whose open failed writes nothing. */
        static int16_t a[1024], b[1024];
        mg_resample_t r, f;
        sine(in, 32000, 1000, 32000, 0.5);
        CHECK(mg_resample_open(&r, 32000, 16000) && mg_resample_open(&f, 32000, 16000));
        CHECK(mg_resample_run(&r, in, 1000, a, 1024) > 0);
        const int16_t *coef = r.coef, *hist = r.hist;
        mg_resample_reset(&r);
        CHECK(r.coef == coef && r.hist == hist);
        size_t na = mg_resample_run(&r, in, 1000, a, 1024), nb = mg_resample_run(&f, in, 1000, b, 1024);
        CHECK(na == nb && na > 0 && memcmp(a, b, na * sizeof(int16_t)) == 0);
        mg_resample_close(&r);
        mg_resample_close(&f);
        CHECK(mg_resample_run(&r, in, 1000, a, 1024) == 0);   /* closed (a failed open leaves it so) */
        printf("resample reset: the tables kept, output as a fresh one's; none after a failed open\n");
    }
}

/* SBC frames of a 1 kHz tone, mono, at `rate`. Returns bytes; *fbytes the frame size. */
static size_t sbc_tone(uint32_t rate, int bitpool, uint8_t *out, size_t cap, size_t *fbytes, double secs)
{
    sbc_t s;
    struct sbc_frame f = { 0 };
    f.freq = rate == 16000 ? SBC_FREQ_16K : rate == 32000 ? SBC_FREQ_32K : rate == 44100 ? SBC_FREQ_44K1 : SBC_FREQ_48K;
    f.mode = SBC_MODE_MONO;
    f.bam = SBC_BAM_LOUDNESS;
    f.nblocks = 16;
    f.nsubbands = 8;
    f.bitpool = bitpool;
    sbc_reset(&s);
    size_t fb = sbc_get_frame_size(&f), n = 0;
    *fbytes = fb;
    int16_t pcm[128];
    size_t total = (size_t)(secs * rate);
    for (size_t t = 0; t + 128 <= total && n + fb <= cap; t += 128) {
        for (int i = 0; i < 128; i++) {
            pcm[i] = (int16_t)lrint(0.5 * 32767 * sin(2 * M_PI * 1000.0 * (t + i) / rate));
        }
        CHECK(sbc_encode(&s, pcm, 1, NULL, 0, &f, out + n, (unsigned)fb) == 0);
        n += fb;
    }
    return n;
}

static void test_check(void)
{
    mg_play_params_t p = { .codec = mg_data_type_audio_sbc };
    CHECK(mg_play_check(&p) == 0 && p.rate == 16000 && p.channels == 1);
    p = (mg_play_params_t){ .codec = mg_data_type_audio_sbc, .rate = 44100 };
    CHECK(mg_play_check(&p) == 0);
    p = (mg_play_params_t){ .codec = mg_data_type_audio_sbc, .rate = 8000 };
    CHECK(mg_play_check(&p) != 0);   /* SBC has no 8 kHz */
    p = (mg_play_params_t){ .codec = mg_data_type_audio_sbc, .rate = 22050 };
    CHECK(mg_play_check(&p) != 0);
    p = (mg_play_params_t){ .codec = mg_data_type_audio_sbc, .channels = 2 };
    CHECK(mg_play_check(&p) != 0);
    p = (mg_play_params_t){ .codec = mg_data_type_audio_sbc, .bitrate_kbps = 400 };
    CHECK(mg_play_check(&p) != 0);
    p = (mg_play_params_t){ .codec = mg_data_type_audio_pcm, .rate = 16000 };
    CHECK(mg_play_check(&p) == 0);
    p = (mg_play_params_t){ .codec = mg_data_type_audio_pcm, .rate = 48000 };
    CHECK(mg_play_check(&p) != 0);   /* 768 kb/s */
    p = (mg_play_params_t){ .codec = mg_data_type_audio_opus };
    CHECK(mg_play_check(&p) != 0);
#if MG_WITH_LC3
    p = (mg_play_params_t){ .codec = mg_data_type_audio_lc3 };
    CHECK(mg_play_check(&p) == 0 && p.frame_bytes == 40);
    p = (mg_play_params_t){ .codec = mg_data_type_audio_lc3, .bitrate_kbps = 64 };
    CHECK(mg_play_check(&p) == 0 && p.frame_bytes == 80);
    p = (mg_play_params_t){ .codec = mg_data_type_audio_lc3, .rate = 48000 };
    CHECK(mg_play_check(&p) != 0);
#if MG_LC3_24K
    p = (mg_play_params_t){ .codec = mg_data_type_audio_lc3, .rate = 24000 };
    CHECK(mg_play_check(&p) == 0 && p.frame_bytes == 40);
#endif
#else
    p = (mg_play_params_t){ .codec = mg_data_type_audio_lc3 };
    CHECK(mg_play_check(&p) != 0);
#endif
    uint8_t c[4];
    size_t nc = mg_play_codecs(c, 4);
    CHECK(nc == (MG_WITH_LC3 ? 3u : 2u) && c[0] == mg_data_type_audio_sbc && c[nc - 1] == mg_data_type_audio_pcm);
}

static void test_ring(void)
{
    static uint8_t sbc[64 * 1024];
    size_t fb;
    size_t n = sbc_tone(16000, 26, sbc, sizeof(sbc), &fb, 2.0);
    CHECK(fb == 60 && n == 250 * 60);
    mg_play_t p;
    mg_play_init(&p, 4096);
    CHECK(!mg_play_active(&p) && mg_play_available(&p) == 0);
    CHECK(!mg_play_write(&p, sbc, 60));   /* not streaming */
    mg_play_params_t pp = { .codec = mg_data_type_audio_sbc };
    CHECK(mg_play_check(&pp) == 0 && mg_play_start(&p, &pp));
    CHECK(mg_play_available(&p) == 4096 && mg_play_received(&p) == 0);
    uint8_t out[MG_PLAY_MAX_POP];
    /* Prebuffering at the start: silence until 30 ms (4 frames of 8 ms) are in (at most 40). */
    CHECK(MG_PLAY_PREBUFFER_US <= 40000);
    size_t off = 0;
    for (int i = 0; i < 3; i++) {   /* 3 frames: 24 ms */
        CHECK(mg_play_write(&p, sbc + off, 60));
        off += 60;
        CHECK(mg_play_pop(&p, out, sizeof(out), 20000) == 0 && !mg_play_concealing(&p));
    }
    CHECK(p.state == MG_PLAY_PREBUFFER && p.st.silence_us == 60000 && p.st.underruns == 0);
    CHECK(mg_play_available(&p) == 4096 - 180 && mg_play_received(&p) == 180);
    CHECK(mg_play_write(&p, sbc + off, 60));
    off += 60;
    size_t got = mg_play_pop(&p, out, sizeof(out), 20000);
    CHECK(got == 180 && memcmp(out, sbc, 180) == 0 && p.state == MG_PLAY_PLAYING);   /* 3 frames: 24 ms */
    /* A write that isn't whole frames, or doesn't fit, is dropped whole but counted as received. */
    CHECK(!mg_play_write(&p, sbc + off, 90));
    CHECK(p.st.bad_frames == 1 && p.st.bytes_dropped == 90 && mg_play_received(&p) == 330);
    uint32_t before = mg_play_available(&p);
    static uint8_t junk[4096];
    memcpy(junk, sbc, sizeof(junk) / 60 * 60);
    CHECK(!mg_play_write(&p, junk, 4080));
    CHECK(mg_play_available(&p) == before && p.st.writes_dropped == 2);
    /* A 32 kHz frame in a 16 kHz stream is rejected. */
    size_t fb32;
    static uint8_t sbc32[1024];
    sbc_tone(32000, 26, sbc32, sizeof(sbc32), &fb32, 0.05);
    CHECK(!mg_play_write(&p, sbc32, fb32) && p.st.bad_frames == 2);
    /* Pops follow the writes in order, across the ring's wrap. */
    size_t pos = 180;
    for (int round = 0; round < 40; round++) {
        if (mg_play_available(&p) >= 240 && off + 240 <= n) {
            CHECK(mg_play_write(&p, sbc + off, 240));
            off += 240;
        }
        got = mg_play_pop(&p, out, sizeof(out), 20000);
        if (got) {
            CHECK(memcmp(out, sbc + pos, got) == 0);
            pos += got;
        }
    }
    CHECK(p.st.underruns == 0);
    /* Starve it: concealed (never re-buffered), the first frame that arrives
     * plays at once, and the gap counts as one underrun then (it was short). */
    while (mg_play_pop(&p, out, sizeof(out), 20000)) {
    }
    CHECK(p.st.underruns == 0 && p.state == MG_PLAY_PLAYING && mg_play_concealing(&p));
    CHECK(mg_play_pop(&p, out, sizeof(out), 20000) == 0 && p.st.underruns == 0 && mg_play_concealing(&p));
    CHECK(mg_play_write(&p, sbc, 60));   /* one 8 ms frame, far under the start's 30 ms */
    CHECK(mg_play_pop(&p, out, sizeof(out), 20000) == 60 && !mg_play_concealing(&p) && p.st.underruns == 1);
    CHECK(mg_play_pop(&p, out, sizeof(out), 20000) == 0 && p.st.underruns == 1);   /* a new gap: counted when audio comes back */
    /* stop keep: plays out what's left, then DONE with keep. */
    CHECK(mg_play_write(&p, sbc, 120));
    mg_play_stop(&p, true);
    CHECK(p.state == MG_PLAY_DRAINING && mg_play_available(&p) == 0 && !mg_play_write(&p, sbc, 60));
    CHECK(mg_play_pop(&p, out, sizeof(out), 20000) == 120);   /* no prebuffer while draining */
    CHECK(mg_play_pop(&p, out, sizeof(out), 20000) == 0 && p.state == MG_PLAY_DONE && p.done_keep);
    /* stop drop empties at once. */
    uint32_t gen = p.generation;
    CHECK(mg_play_start(&p, &pp) && p.generation != gen && mg_play_received(&p) == 0);
    CHECK(mg_play_write(&p, sbc, 600));
    mg_play_stop(&p, false);
    CHECK(p.state == MG_PLAY_IDLE && p.len == 0);
    mg_play_release(&p);
    CHECK(p.buf == NULL);
    /* A client that goes quiet without stop_streaming: the device ends it after 10 s,
     * before its first audio and in a gap alike. */
    CHECK(mg_play_start(&p, &pp));
    for (int i = 0; i < 499; i++) {
        mg_play_pop(&p, out, sizeof(out), 20000);
    }
    CHECK(p.state == MG_PLAY_PREBUFFER);
    mg_play_pop(&p, out, sizeof(out), 20000);
    CHECK(p.state == MG_PLAY_DONE && !p.done_keep);
    CHECK(mg_play_start(&p, &pp) && mg_play_write(&p, sbc, 240));
    while (mg_play_pop(&p, out, sizeof(out), 20000)) {
    }
    for (int i = 0; i < 498; i++) {   /* the gap's first chunk was the loop's last pop */
        mg_play_pop(&p, out, sizeof(out), 20000);
    }
    CHECK(p.state == MG_PLAY_PLAYING && mg_play_concealing(&p));
    mg_play_pop(&p, out, sizeof(out), 20000);
    CHECK(p.state == MG_PLAY_DONE && !p.done_keep);
    p.state = MG_PLAY_IDLE;
    mg_play_release(&p);
}

static double snr_db(const int16_t *ref, const int16_t *x, size_t n, int max_lag)
{
    double best = -100;
    for (int lag = 0; lag < max_lag; lag++) {
        double s = 0, e = 0;
        for (size_t i = 500; i + lag < n; i++) {
            double d = (double)x[i + lag] - ref[i];
            s += (double)ref[i] * ref[i];
            e += d * d;
        }
        double v = 10 * log10(s / (e + 1e-9));
        best = v > best ? v : best;
    }
    return best;
}

/* Plays a whole stream through ring and decoder, as the playback task does. */
static size_t play_stream(const mg_play_params_t *pp0, const uint8_t *data, size_t n, size_t write, int16_t *pcm,
                          size_t cap, mg_play_stats_t *st, uint32_t *frames)
{
    mg_play_params_t pp = *pp0;
    CHECK(mg_play_check(&pp) == 0);
    static mg_play_t p;
    mg_play_init(&p, 16384);
    CHECK(mg_play_start(&p, &pp));
    static mg_dec_t d;
    CHECK(mg_dec_open(&d, &pp));
    size_t off = 0, o = 0;
    uint8_t buf[MG_PLAY_MAX_POP];
    bool stopped = false;
    while (p.state != MG_PLAY_DONE && o < cap) {
        while (off < n && mg_play_available(&p) >= write) {
            size_t w = n - off < write ? n - off : write;
            CHECK(mg_play_write(&p, data + off, w));
            off += w;
        }
        if (off >= n && !stopped) {
            mg_play_stop(&p, true);
            stopped = true;
        }
        size_t got = mg_play_pop(&p, buf, sizeof(buf), 20000);
        if (got) {
            o += mg_dec_run(&d, buf, got, pcm + o, cap - o);
        } else if (p.state != MG_PLAY_DONE) {
            size_t z = 320 < cap - o ? 320 : cap - o;
            memset(pcm + o, 0, z * sizeof(int16_t));   /* silence, as the speaker would get */
            o += z;
        }
    }
    CHECK(p.state == MG_PLAY_DONE && p.done_keep);
    *st = p.st;
    *frames = d.frames;
    CHECK(d.bad == 0);
    mg_dec_close(&d);
    p.state = MG_PLAY_IDLE;
    mg_play_release(&p);
    return o;
}

static void test_decode(void)
{
    static uint8_t data[256 * 1024];
    static int16_t pcm[64000], ref[48000];
    mg_play_stats_t st;
    uint32_t frames;
    static const uint32_t rates[] = { 16000, 32000, 44100, 48000 };
    for (size_t i = 0; i < 4; i++) {
        size_t fb;
        size_t n = sbc_tone(rates[i], rates[i] == 16000 ? 26 : 35, data, sizeof(data), &fb, 1.0);
        mg_play_params_t pp = { .codec = mg_data_type_audio_sbc, .rate = rates[i] };
        size_t write = fb * (240 / fb ? 240 / fb : 1);
        size_t o = play_stream(&pp, data, n, write, pcm, 64000, &st, &frames);
        /* Skip the prebuffer's silence and the codec delay; then it's a 1 kHz tone at half scale. */
        size_t start = 1600 + 800;
        double a = tone(pcm + start, 8000, 1000, 16000);
        printf("SBC %u Hz: %u frames in, %u decoded, %zu samples out, 1 kHz at %.3f, %u underruns, %u bytes\n",
               rates[i], st.frames_in, frames, o, a, st.underruns, st.bytes_received);
        CHECK(frames == n / fb && st.frames_in == frames && st.bytes_received == n && st.underruns == 0);
        CHECK(fabs(a - 0.5) < 0.05);
        CHECK(tone(pcm + start, 8000, 1500, 16000) < 0.01);
    }
    /* PCM at 8 kHz, upsampled. */
    sine(ref, 8000, 440, 8000, 0.3);
    for (int i = 0; i < 8000; i++) {
        data[2 * i] = (uint8_t)ref[i];
        data[2 * i + 1] = (uint8_t)(ref[i] >> 8);
    }
    mg_play_params_t pp = { .codec = mg_data_type_audio_pcm, .rate = 8000 };
    size_t o = play_stream(&pp, data, 16000, 244, pcm, 64000, &st, &frames);
    printf("PCM 8 kHz: %zu samples out, %u frames, 440 Hz at %.3f, %u dropped, %u bad\n", o, frames,
           tone(pcm + 3000, 8000, 440, 16000), st.bytes_dropped, st.bad_frames);
    CHECK(o >= 15990 && fabs(tone(pcm + 3000, 8000, 440, 16000) - 0.3) < 0.02);
    /* PCM at 16 kHz is sample exact after the prebuffer. */
    sine(ref, 16000, 700, 16000, 0.4);
    for (int i = 0; i < 16000; i++) {
        data[2 * i] = (uint8_t)ref[i];
        data[2 * i + 1] = (uint8_t)(ref[i] >> 8);
    }
    pp = (mg_play_params_t){ .codec = mg_data_type_audio_pcm, .rate = 16000 };
    o = play_stream(&pp, data, 32000, 244, pcm, 64000, &st, &frames);
    CHECK(o == 16000 && snr_db(ref, pcm, 16000, 2000) > 80);   /* all buffered at once: no prebuffer silence */
#if MG_WITH_LC3
    {
        static lc3_encoder_mem_16k_t mem;
        lc3_encoder_t e = lc3_setup_encoder(10000, 16000, 0, &mem);
        sine(ref, 16000, 1000, 16000, 0.5);
        size_t n = 0;
        for (int t = 0; t + 160 <= 16000; t += 160) {
            CHECK(lc3_encode(e, LC3_PCM_FORMAT_S16, ref + t, 1, 40, data + n) == 0);
            n += 40;
        }
        pp = (mg_play_params_t){ .codec = mg_data_type_audio_lc3 };
        o = play_stream(&pp, data, n, 200, pcm, 64000, &st, &frames);
        double a = tone(pcm + 3000, 8000, 1000, 16000);
        printf("LC3: %u frames decoded, 1 kHz at %.3f\n", frames, a);
        CHECK(frames == 100 && fabs(a - 0.5) < 0.05);
    }
#if MG_LC3_24K
    {
        /* LC3 at 24 kHz, 32 kb/s (40 B): a 1 kHz tone and a 9.5 kHz one
         * that the 16 kHz output can't carry (resampled away, not aliased). */
        static LC3_ENCODER_MEM_T(10000, 24000) mem24;
        static int16_t r24[24000], t2[24000];
        lc3_encoder_t e = lc3_setup_encoder(10000, 24000, 0, &mem24);
        sine(r24, 24000, 1000, 24000, 0.5);
        sine(t2, 24000, 9500, 24000, 0.1);
        for (int i = 0; i < 24000; i++) r24[i] = (int16_t)(r24[i] + t2[i]);
        size_t n = 0;
        for (int t = 0; t + 240 <= 24000; t += 240) {
            CHECK(lc3_encode(e, LC3_PCM_FORMAT_S16, r24 + t, 1, 40, data + n) == 0);
            n += 40;
        }
        pp = (mg_play_params_t){ .codec = mg_data_type_audio_lc3, .rate = 24000, .bitrate_kbps = 32 };
        CHECK(mg_play_check(&pp) == 0);
        o = play_stream(&pp, data, n, 200, pcm, 64000, &st, &frames);
        double a = tone(pcm + 3000, 8000, 1000, 16000);
        double alias = tone(pcm + 3000, 8000, 6500, 16000);   /* where 9.5 kHz would fold to */
        printf("LC3 24 kHz: %u frames decoded, %zu samples out, 1 kHz at %.3f, 6.5 kHz alias %.4f\n", frames, o, a, alias);
        CHECK(frames == 100 && fabs(a - 0.5) < 0.05 && alias < 0.005);
    }
#endif
#endif
}

/* ---- the speaker boost: makeup gain, then a peak limiter ---- */

static double tone_db(double dbfs, double gain_db_expect, int db10, double *peak_out)
{
    static int16_t x[16000];
    double amp = 32767.0 * pow(10.0, dbfs / 20.0);
    for (int i = 0; i < 16000; i++) {
        x[i] = (int16_t)lround(amp * sin(2 * 3.14159265358979 * 440.0 * i / 16000.0));
    }
    mg_boost_t b = { 0 };
    mg_boost_set(&b, db10);
    for (int i = 0; i < 16000; i += 320) {
        mg_boost_run(&b, x + i, 320);
    }
    double sq = 0, peak = 0;
    for (int i = 8000; i < 16000; i++) {   /* the second half: settled */
        sq += (double)x[i] * x[i];
        peak = fabs((double)x[i]) > peak ? fabs((double)x[i]) : peak;
    }
    (void)gain_db_expect;
    *peak_out = peak;
    return 20 * log10(sqrt(sq / 8000) / (amp / sqrt(2)));   /* the gain it got, dB */
}

static void test_boost(void)
{
    double peak, g;
    /* Quiet speech-level audio: the whole +6 dB, untouched by the limiter. */
    g = tone_db(-30, 6, 60, &peak);
    CHECK(fabs(g - 6.0) < 0.1);
    /* Muse's loudest (-4 dBFS peaks): limited to the ceiling, never over it. */
    g = tone_db(-4, 6, 60, &peak);
    CHECK(peak <= MG_BOOST_CEILING && peak > MG_BOOST_CEILING - 600);
    CHECK(g > -0.5 && g < 0.5);   /* the ceiling is -4 dBFS: the tone comes out about as it went in */
    /* A full-scale clip at full volume: still no more than the ceiling. */
    g = tone_db(0, 6, 60, &peak);
    CHECK(peak <= MG_BOOST_CEILING);
    /* 0 dB: passed through untouched, full scale included. */
    g = tone_db(0, 0, 0, &peak);
    CHECK(fabs(g) < 0.01 && peak >= 32766);
    /* The ramp: nothing up to half volume, then up to the board's maximum. */
    CHECK(mg_boost_for_volume(60, 0) == 0 && mg_boost_for_volume(60, 50) == 0);
    CHECK(mg_boost_for_volume(60, 75) == 30 && mg_boost_for_volume(60, 100) == 60);
    CHECK(mg_boost_for_volume(60, 120) == 60 && mg_boost_for_volume(0, 100) == 0);
    /* The gain follows a volume change. */
    mg_boost_t b = { 0 };
    mg_boost_set(&b, 60);
    CHECK(b.gain_q12 == 8173);   /* 10^(6/20) * 4096 */
    mg_boost_set(&b, 30);
    CHECK(b.gain_q12 == 5786);
    mg_boost_set(&b, 0);
    int16_t one[1] = { 1234 };
    mg_boost_run(&b, one, 1);
    CHECK(one[0] == 1234);
}

/* The stream's clock, the latest underrun's time on it, and the once-a-second line. */
static void test_tick(void)
{
    static uint8_t sbc[16 * 1024];
    size_t fb;
    sbc_tone(16000, 26, sbc, sizeof(sbc), &fb, 1.0);   /* 8 ms frames */
    mg_play_t p;
    mg_play_init(&p, 4096);
    mg_play_params_t pp = { .codec = mg_data_type_audio_sbc };
    CHECK(mg_play_check(&pp) == 0 && mg_play_start(&p, &pp));
    CHECK(p.st.clock_us == 0 && p.st.last_underrun_us == MG_PLAY_NO_UNDERRUN);
    uint8_t out[MG_PLAY_MAX_POP];
    char line[160];
    int n = mg_play_format_tick(line, sizeof(line), false, &p.st, 0, 0, 0, 0, 1, 1);
    CHECK(strcmp(line, "av play: t=0 span=0 underruns=0 new=0 last=-1 buf=0 queued=0 int=1 low=1") == 0);
    /* 13 frames (104 ms) start it; each pop is a 20 ms chunk on the clock, played or
     * not; the gap after them is concealed, the next frames play at once, and the
     * gap counts as an underrun then, at its start on the clock. */
    for (int round = 0; round < 3; round++) {
        uint32_t gap = p.gap_at;
        CHECK(mg_play_write(&p, sbc, (round < 2 ? 13 : 1) * fb));
        uint32_t at = 0;
        size_t got;
        int i = 0;
        do {
            at = p.st.clock_us;
            got = mg_play_pop(&p, out, sizeof(out), 20000);
            CHECK(p.st.clock_us == at + 20000);
            if (i == 0 && round) {
                CHECK(got && p.st.underruns == (uint32_t)round && p.st.last_underrun_us == gap);   /* the gap, counted */
            }
        } while (got && ++i < 12 && round < 2);
        if (round < 2) {
            /* Five chunks play the 104 ms, the sixth runs dry: at 100 ms, then 220 ms (after the first 200);
             * not an underrun yet. */
            CHECK(!got && p.starving && p.gap_at == at && p.st.underruns == (uint32_t)round);
            CHECK(at == (round ? 220000u : 100000u));
        }
    }
    p.out_us = 30000;
    mg_play_set_pending(&p, 0);
    n = mg_play_format_tick(line, sizeof(line), false, &p.st, 1, 1002, p.buffered_us, mg_play_queued_ms(&p), 23456,
                            21000);
    CHECK(n > 0 && n < 130);   /* fits the Nordic UART mirror's 160-character line with its prefix */
    CHECK(strcmp(line, "av play: t=260 span=1002 underruns=2 new=1 last=220 buf=0 queued=30 int=23456 low=21000") == 0);
    printf("SAMPLE I (5260) mg.play: %s\n", line);
    /* Silence while starved still moves the clock; the end line says so. */
    mg_play_pop(&p, out, sizeof(out), 20000);
    CHECK(p.st.clock_us == 280000 && p.st.underruns == 2);
    mg_play_format_tick(line, sizeof(line), true, &p.st, 2, 20, 0, MG_PLAY_QUEUED_UNKNOWN, 23456, 20900);
    CHECK(strcmp(line, "av play end: t=280 span=20 underruns=2 new=0 last=220 buf=0 queued=-1 int=23456 low=20900") == 0);
    printf("SAMPLE I (5280) mg.play: %s\n", line);
    /* A new stream starts its clock over. */
    CHECK(mg_play_start(&p, &pp) && p.st.clock_us == 0 && p.st.underruns == 0 &&
          p.st.last_underrun_us == MG_PLAY_NO_UNDERRUN);
    mg_play_stop(&p, false);
    mg_play_release(&p);
    /* Idle: no clock. */
    mg_play_pop(&p, out, sizeof(out), 20000);
    CHECK(p.st.clock_us == 0);
}

/* ---- live reply audio: start threshold, concealment, depth ---- */

static double rms(const int16_t *x, size_t n)
{
    double s = 0;
    for (size_t i = 0; i < n; i++) s += (double)x[i] * x[i];
    return n ? sqrt(s / n) : 0;
}

static void test_live(void)
{
    /* buffer_update's queued ms: ring + pending + output path, rounded; 0 idle; unknown without a depth. */
    static mg_play_t q;
    mg_play_init(&q, 8192);
    CHECK(mg_play_queued_ms(&q) == MG_PLAY_QUEUED_UNKNOWN);
    q.out_us = 30000;
    CHECK(mg_play_queued_ms(&q) == 0);   /* no stream */
    mg_play_params_t pcm16 = { .codec = mg_data_type_audio_pcm, .rate = 16000 };
    CHECK(mg_play_check(&pcm16) == 0 && mg_play_start(&q, &pcm16));
    CHECK(mg_play_queued_ms(&q) == 30);   /* the DMA, before any audio */
    static uint8_t z[8192];
    CHECK(mg_play_write(&q, z, 640));     /* 20 ms */
    CHECK(mg_play_queued_ms(&q) == 50);
    mg_play_set_pending(&q, 7600);
    CHECK(mg_play_queued_ms(&q) == 58);   /* 57.6 rounds up */
    q.out_us = MG_PLAY_DEPTH_UNKNOWN;
    CHECK(mg_play_queued_ms(&q) == MG_PLAY_QUEUED_UNKNOWN);
    mg_play_stop(&q, false);
    CHECK(q.pending_us == 0);
    mg_play_release(&q);

    /* The client's start burst (60 ms) then 40 ms writes as audio arrives, the
     * player taking 10 ms chunks: it starts at once, never runs dry, and the
     * whole depth (with 30 ms of DMA) stays at or under 100 ms. */
    static mg_play_t p;
    mg_play_init(&p, 16384);
    p.out_us = 30000;
    CHECK(mg_play_start(&p, &pcm16));
    uint8_t out[MG_PLAY_MAX_POP];
    int maxq = 0, first_audio = -1;
    for (int t = 0; t < 3000; t += 10) {
        if (t == 0) {
            CHECK(mg_play_write(&p, z, 1920));   /* 60 ms */
        } else if (t % 40 == 0) {
            CHECK(mg_play_write(&p, z, 1280));   /* 40 ms */
        }
        int qm = mg_play_queued_ms(&p);
        CHECK(qm == (int)((p.buffered_us + p.pending_us + p.out_us + 500) / 1000));
        maxq = qm > maxq ? qm : maxq;
        size_t got = mg_play_pop(&p, out, sizeof(out), MG_PLAY_CHUNK_US);
        if (got && first_audio < 0) first_audio = t;
        qm = mg_play_queued_ms(&p);
        maxq = qm > maxq ? qm : maxq;
        mg_play_set_pending(&p, 0);   /* decoded and written to the speaker */
    }
    printf("live: first audio at %d ms, deepest %d ms, %u underruns\n", first_audio, maxq, p.st.underruns);
    CHECK(first_audio == 0 && maxq <= 100 && p.st.underruns == 0);
    /* A gap: concealed, the next write plays at the next chunk, and the gap counts once then. */
    while (mg_play_pop(&p, out, sizeof(out), MG_PLAY_CHUNK_US)) {
    }
    for (int i = 0; i < 9; i++) {   /* 100 ms of gap in all */
        CHECK(mg_play_pop(&p, out, sizeof(out), MG_PLAY_CHUNK_US) == 0 && mg_play_concealing(&p));
    }
    CHECK(p.st.underruns == 0);
    CHECK(mg_play_write(&p, z, 320));   /* 10 ms: no re-buffering to a larger threshold */
    CHECK(mg_play_pop(&p, out, sizeof(out), MG_PLAY_CHUNK_US) == 320 && !mg_play_concealing(&p));
    CHECK(p.st.underruns == 1);
    mg_play_stop(&p, false);
    mg_play_release(&p);

    /* SBC (and PCM): the last 10 ms once more, faded out, then silence; a frame ends the gap. */
    {
        static uint8_t sbc[16 * 1024];
        static int16_t o[MG_PLAY_MAX_OUT];
        size_t fb;
        size_t n = sbc_tone(16000, 26, sbc, sizeof(sbc), &fb, 1.0);
        mg_play_params_t pp = { .codec = mg_data_type_audio_sbc };
        CHECK(mg_play_check(&pp) == 0);
        static mg_dec_t d;
        CHECK(mg_dec_open(&d, &pp));
        size_t got = mg_dec_run(&d, sbc, 12 * fb, o, MG_PLAY_MAX_OUT);   /* 96 ms of 1 kHz */
        CHECK(got == 1536 && d.nlast == 160);
        int16_t last[160];
        memcpy(last, o + got - 160, sizeof(last));
        int16_t c[160];
        CHECK(mg_dec_conceal(&d, c, 160, MG_PLAY_CHUNK_US) == 160);
        CHECK(c[0] == last[0] && abs(c[159]) <= abs(last[159]) / 100 + 1 && rms(c, 160) > 0.3 * rms(last, 160));
        int faded = 1;
        for (int i = 0; i < 160; i++) faded &= abs(c[i]) <= abs(last[i]);
        CHECK(faded);
        CHECK(mg_dec_conceal(&d, c, 160, MG_PLAY_CHUNK_US) == 160 && rms(c, 160) == 0);   /* then silence */
        CHECK(mg_dec_run(&d, sbc + 12 * fb, fb, o, MG_PLAY_MAX_OUT) == 128 && d.conceal_us == 0);
        CHECK(mg_dec_conceal(&d, c, 160, MG_PLAY_CHUNK_US) == 160 && rms(c, 160) > 0);   /* a new gap repeats again */
        CHECK(d.concealed == 3);
        (void)n;
        mg_dec_close(&d);
        /* A resampled stream (32 kHz SBC): the flush keeps the resampler's tables (allocates nothing), and what
         * follows decodes as on a fresh decoder. */
        static uint8_t s32[16 * 1024];
        static int16_t o2[MG_PLAY_MAX_OUT];
        sbc_tone(32000, 26, s32, sizeof(s32), &fb, 0.5);
        pp = (mg_play_params_t){ .codec = mg_data_type_audio_sbc, .rate = 32000 };
        CHECK(mg_play_check(&pp) == 0);
        static mg_dec_t f;
        CHECK(mg_dec_open(&d, &pp) && mg_dec_open(&f, &pp) && !d.rs.passthrough);
        CHECK(mg_dec_run(&d, s32, 6 * fb, o, MG_PLAY_MAX_OUT) > 0);
        const int16_t *coef = d.rs.coef, *hist = d.rs.hist;
        CHECK(mg_dec_flush(&d));
        CHECK(d.rs.coef == coef && d.rs.hist == hist);
        size_t na = mg_dec_run(&d, s32 + 6 * fb, 4 * fb, o, MG_PLAY_MAX_OUT);
        size_t nb = mg_dec_run(&f, s32 + 6 * fb, 4 * fb, o2, MG_PLAY_MAX_OUT);
        CHECK(na == nb && na > 0 && memcmp(o, o2, na * sizeof(int16_t)) == 0);
        mg_dec_close(&d);
        mg_dec_close(&f);
        printf("flush: the resampler's tables kept, decoding as a fresh decoder\n");
    }
#if MG_WITH_LC3
    /* LC3: the decoder's own concealment for the gap's first 60 ms (the tone
     * carries on, attenuating), then silence; decoding resumes cleanly. */
    {
        static lc3_encoder_mem_16k_t mem;
        static int16_t ref[16000], o[MG_PLAY_MAX_OUT];
        static uint8_t data[8000];
        lc3_encoder_t e = lc3_setup_encoder(10000, 16000, 0, &mem);
        sine(ref, 16000, 1000, 16000, 0.5);
        size_t n = 0;
        for (int t = 0; t + 160 <= 16000; t += 160) {
            CHECK(lc3_encode(e, LC3_PCM_FORMAT_S16, ref + t, 1, 40, data + n) == 0);
            n += 40;
        }
        mg_play_params_t pp = { .codec = mg_data_type_audio_lc3 };
        CHECK(mg_play_check(&pp) == 0);
        static mg_dec_t d;
        CHECK(mg_dec_open(&d, &pp));
        size_t got = mg_dec_run(&d, data, 12 * 40, o, MG_PLAY_MAX_OUT);
        CHECK(got == 1920);
        double before = rms(o + got - 160, 160);
        static int16_t c[160 * 8];
        for (int i = 0; i < 8; i++) {
            CHECK(mg_dec_conceal(&d, c + 160 * i, 160, MG_PLAY_CHUNK_US) == 160);
        }
        double plc0 = rms(c, 160), plc5 = rms(c + 160 * 5, 160), after = rms(c + 160 * 6, 320);
        printf("LC3 PLC: before %.0f, first concealed %.0f, sixth %.0f, then %.0f\n", before, plc0, plc5, after);
        CHECK(plc0 > 0.3 * before && plc0 < 1.5 * before && after == 0);
        got = mg_dec_run(&d, data + 12 * 40, 10 * 40, o, MG_PLAY_MAX_OUT);
        CHECK(got == 1600 && d.conceal_us == 0 && d.bad == 0 && rms(o + 800, 800) > 0.5 * before);
        mg_dec_close(&d);
    }
#endif
}

/* ---- a flush resyncs the decoder ----
 *
 * A gadget's bug: a barge-in's flush left part of an LC3 frame in the
 * decoder's input, and every frame of the next reply decoded shifted (noise,
 * no decoder error). Here a reply is barged in on mid-way, with the decoder
 * holding state (frames decoded, a gap concealed) and the client's half-written
 * frames around the flush; the next reply must then decode bit-identical to
 * the same reply through a fresh decoder, for every codec this build plays.
 * The player's order is mg_ble.c's: what was popped before the flush decodes
 * on the old state, then flush_seq moves and mg_dec_flush runs before any
 * frame popped after it. */

/* A voiced, amplitude-modulated signal: harmonics of f0, different per reply. */
static void voice(int16_t *x, size_t n, double rate, int seed)
{
    double f0 = seed ? 190 : 120;
    for (size_t i = 0; i < n; i++) {
        double t = i / rate, v = 0;
        for (int h = 1; h <= 8; h++) {
            v += sin(2 * M_PI * f0 * h * t + h * seed) / h;
        }
        x[i] = (int16_t)lrint((0.55 + 0.45 * sin(2 * M_PI * 3 * t + seed)) * v * 6000);
    }
}

/* `secs` of voice(seed) in the stream's format. Returns bytes; *fb the frame bytes (PCM: 2). */
static size_t resync_reply(const mg_play_params_t *pp, int seed, double secs, uint8_t *out, size_t cap, size_t *fb)
{
    static int16_t x[48000];
    size_t n = (size_t)(secs * pp->rate), bytes = 0;
    voice(x, n, pp->rate, seed);
    if (pp->codec == mg_data_type_audio_sbc) {
        sbc_t s;
        struct sbc_frame f = { 0 };
        f.freq = SBC_FREQ_16K;
        f.mode = SBC_MODE_MONO;
        f.bam = SBC_BAM_LOUDNESS;
        f.nblocks = 16;
        f.nsubbands = 8;
        f.bitpool = 26;
        sbc_reset(&s);
        *fb = sbc_get_frame_size(&f);
        for (size_t t = 0; t + 128 <= n && bytes + *fb <= cap; t += 128) {
            CHECK(sbc_encode(&s, x + t, 1, NULL, 0, &f, out + bytes, (unsigned)*fb) == 0);
            bytes += *fb;
        }
        return bytes;
    }
#if MG_WITH_LC3
    if (pp->codec == mg_data_type_audio_lc3) {
        static LC3_ENCODER_MEM_T(10000, 24000) mem;
        lc3_encoder_t e = lc3_setup_encoder(10000, (int)pp->rate, 0, &mem);
        size_t fs = pp->rate / 100;
        *fb = pp->frame_bytes;
        for (size_t t = 0; t + fs <= n && bytes + *fb <= cap; t += fs) {
            CHECK(lc3_encode(e, LC3_PCM_FORMAT_S16, x + t, 1, (int)*fb, out + bytes) == 0);
            bytes += *fb;
        }
        return bytes;
    }
#endif
    *fb = 2;
    for (size_t i = 0; i < n && bytes + 2 <= cap; i++) {
        out[bytes++] = (uint8_t)x[i];
        out[bytes++] = (uint8_t)((uint16_t)x[i] >> 8);
    }
    return bytes;
}

/* Writes `n` bytes in writes of whole frames (about 200 bytes each). */
static void resync_write(mg_play_t *p, const uint8_t *d, size_t n, size_t fb)
{
    size_t w = fb * (200 / fb ? 200 / fb : 1), off = 0;
    while (off < n) {
        size_t k = n - off < w ? n - off : w;
        CHECK(mg_play_write(p, d + off, k));
        off += k;
    }
}

/* Pops and decodes until the ring is empty (a call's stream: it stays open). The popped bytes go to `raw`. */
static size_t resync_drain(mg_play_t *p, mg_dec_t *d, int16_t *pcm, size_t cap, uint8_t *raw, size_t *nraw)
{
    uint8_t buf[MG_PLAY_MAX_POP];
    size_t o = 0;
    for (int guard = 0; guard < 2000 && (p->len || p->state == MG_PLAY_PREBUFFER); guard++) {
        size_t got = mg_play_pop(p, buf, sizeof(buf), MG_PLAY_CHUNK_US);
        if (got) {
            memcpy(raw + *nraw, buf, got);
            *nraw += got;
            o += mg_dec_run(d, buf, got, pcm + o, cap - o);
        }
    }
    return o;
}

/* Reply B through a fresh ring and decoder: the reference. */
static size_t resync_fresh(const mg_play_params_t *pp, const uint8_t *b, size_t nb, size_t fb, int16_t *pcm, size_t cap)
{
    static mg_play_t p;
    static mg_dec_t d;
    static uint8_t raw[64 * 1024];
    size_t nraw = 0;
    mg_play_init(&p, 32768);
    CHECK(mg_play_start(&p, pp) && mg_dec_open(&d, pp));
    p.keep_open = true;
    resync_write(&p, b, nb, fb);
    size_t o = resync_drain(&p, &d, pcm, cap, raw, &nraw);
    CHECK(nraw == nb && memcmp(raw, b, nb) == 0 && d.bad == 0);
    mg_dec_close(&d);
    mg_play_stop(&p, false);
    mg_play_release(&p);
    return o;
}

/* Reply A barged in on mid-way, then reply B: B's decoded PCM. reset = false leaves the decoder's state
 * (the defect the flush's reset rules out), to show the comparison sees it. */
static size_t resync_barge(const mg_play_params_t *pp, const uint8_t *a, size_t na, const uint8_t *b, size_t nb,
                         size_t fb, bool reset, int16_t *pcm, size_t cap)
{
    static mg_play_t p;
    static mg_dec_t d;
    static int16_t junk[MG_PLAY_MAX_OUT * 4];
    static uint8_t raw[64 * 1024];
    uint8_t buf[MG_PLAY_MAX_POP];
    mg_play_init(&p, 32768);
    CHECK(mg_play_start(&p, pp) && mg_dec_open(&d, pp));
    p.keep_open = true;   /* a Live call's one reply stream: the flush keeps it open */
    size_t half = na / fb / 2 * fb;   /* reply A's first half is written ... */
    resync_write(&p, a, half, fb);
    uint32_t seen = p.flush_seq;
    for (int i = 0; i < 12; i++) {   /* ... and partly decoded, with a gap concealed in between (PLC history) */
        size_t got = mg_play_pop(&p, buf, sizeof(buf), MG_PLAY_CHUNK_US);
        if (got) {
            mg_dec_run(&d, buf, got, junk, sizeof(junk) / sizeof(junk[0]));
        }
        if (i == 6) {
            mg_dec_conceal(&d, junk, sizeof(junk) / sizeof(junk[0]), MG_PLAY_CHUNK_US);
        }
    }
    CHECK(p.len > 0 && d.frames > 0);
    /* The client's next write ends mid-frame (one and a half frames): refused whole, the ring stays aligned. */
    uint32_t bad = p.st.bad_frames;
    if (fb > 2) {
        CHECK(!mg_play_write(&p, a + half, fb + fb / 2) && p.st.bad_frames == bad + 1);
    }
    /* The player's pop in flight when the flush lands: taken before it, so it decodes on the old state. */
    size_t inflight = mg_play_pop(&p, buf, sizeof(buf), MG_PLAY_CHUNK_US);
    CHECK(inflight > 0);
    CHECK(mg_play_flush(&p));
    CHECK(p.len == 0 && p.buffered_us == 0 && p.pending_us == 0 && p.flush_seq != seen);
    mg_dec_run(&d, buf, inflight, junk, sizeof(junk) / sizeof(junk[0]));
    /* Half a frame of the old reply still arriving after the flush: refused whole too. */
    if (fb > 2) {
        CHECK(!mg_play_write(&p, a + half, fb / 2) && p.st.bad_frames == bad + 2);
    }
    /* The player sees flush_seq move: the decoder starts over before anything popped after the flush. */
    if (p.flush_seq != seen && reset) {
        CHECK(mg_dec_flush(&d));
    }
    resync_write(&p, b, nb, fb);
    size_t nraw = 0;
    size_t o = resync_drain(&p, &d, pcm, cap, raw, &nraw);
    CHECK(nraw == nb && memcmp(raw, b, nb) == 0);   /* nothing of reply A decodes after the flush */
    CHECK(d.bad == 0);
    mg_dec_close(&d);
    mg_play_stop(&p, false);
    mg_play_release(&p);
    return o;
}

static void test_flush_resync(void)
{
    static uint8_t a[64 * 1024], b[64 * 1024];
    static int16_t ref[40000], got[40000], stale[40000];
    struct {
        const char *name;
        mg_play_params_t pp;
    } cases[] = {
        { "SBC 16 kHz", { .codec = mg_data_type_audio_sbc, .rate = 16000 } },
        { "PCM 8 kHz", { .codec = mg_data_type_audio_pcm, .rate = 8000 } },
#if MG_WITH_LC3
        { "LC3 16 kHz", { .codec = mg_data_type_audio_lc3, .rate = 16000, .frame_bytes = 40 } },
#if MG_LC3_24K
        { "LC3 24 kHz", { .codec = mg_data_type_audio_lc3, .rate = 24000, .frame_bytes = 60 } },
#endif
#endif
    };
    for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        mg_play_params_t pp = cases[c].pp;
        CHECK(mg_play_check(&pp) == 0);
        size_t fb;
        size_t na = resync_reply(&pp, 0, 0.6, a, sizeof(a), &fb);
        size_t nb = resync_reply(&pp, 1, 0.8, b, sizeof(b), &fb);
        size_t nr = resync_fresh(&pp, b, nb, fb, ref, 40000);
        size_t ng = resync_barge(&pp, a, na, b, nb, fb, true, got, 40000);
        size_t ns = resync_barge(&pp, a, na, b, nb, fb, false, stale, 40000);
        bool same = ng == nr && memcmp(got, ref, nr * sizeof(int16_t)) == 0;
        size_t first = 0;   /* the first sample the stale decoder gets wrong */
        while (first < ns && first < nr && stale[first] == ref[first]) {
            first++;
        }
        bool differs = ns != nr || first < nr;
        printf("flush resync %s: next reply %s a fresh decoder's (%zu samples); without the reset it differs from "
               "sample %zu\n", cases[c].name, same ? "bit-identical to" : "DIFFERS from", nr, first);
        CHECK(nr > 10000 && rms(ref, nr) > 1000);
        CHECK(same);
        CHECK(differs);   /* the comparison sees a decoder left with the old reply's state */
    }
}

/* Flushes past what's queued: no count in mgcommands.h's flush (it takes everything), and nothing the flush
 * zeroes may wrap: a second flush on an emptied stream, the player's leftover larger than the ring, a pop
 * after both. */
static void test_flush_signed(void)
{
    static uint8_t z[3200];
    for (size_t i = 0; i < sizeof(z) / 2; i++) {
        int16_t v = (int16_t)(i % 40 < 20 ? 2000 : -2000);
        z[2 * i] = (uint8_t)v;
        z[2 * i + 1] = (uint8_t)((uint16_t)v >> 8);
    }
    for (int live = 0; live < 2; live++) {
        mg_play_params_t pp = { .codec = mg_data_type_audio_pcm, .rate = 8000, .live_depth_ms = live ? 100 : 0 };
        CHECK(mg_play_check(&pp) == 0);
        static mg_play_t p;
        uint8_t out[MG_PLAY_MAX_POP];
        mg_play_init(&p, 16384);
        p.out_us = 30000;
        CHECK(mg_play_start(&p, &pp));
        p.keep_open = true;
        CHECK(mg_play_write(&p, z, 480));   /* 30 ms */
        CHECK(mg_play_pop(&p, out, sizeof(out), MG_PLAY_CHUNK_US) == 160);
        mg_play_set_pending(&p, 400000);   /* the task says it holds far more than was ever queued */
        uint32_t held = p.buffered_us + p.pending_us;
        CHECK(mg_play_flush(&p) && p.flush_us == held);
        CHECK(mg_play_flush(&p) && p.flush_us == 0 && p.st.flushes == 2 && p.st.flushed_us == held);
        CHECK(p.len == 0 && p.buffered_us == 0 && p.pending_us == 0 && p.rd_total == p.wr_total);
        CHECK(mg_play_available(&p) == 16384 && mg_play_queued_ms(&p) == 0);
        uint32_t credit = mg_play_credit(&p, 0);
        CHECK(credit <= 16384 && (live ? credit > 0 && credit < 16384 : credit == 16384));
        CHECK(mg_play_pop(&p, out, sizeof(out), MG_PLAY_CHUNK_US) == 0 && p.buffered_us == 0 && !mg_play_concealing(&p));
        CHECK(mg_play_write(&p, z, 160) && p.buffered_us == 10000);
        CHECK(mg_play_pop(&p, out, sizeof(out), MG_PLAY_CHUNK_US) == 160 && p.buffered_us == 0 && p.st.underruns == 0);
        printf("flush past the queue%s: 2 flushes, %u us discarded, credit %u of 16384, queued 0, no wrap\n",
               live ? " (live depth 100 ms)" : "", (unsigned)p.st.flushed_us, (unsigned)credit);
        mg_play_stop(&p, false);
        mg_play_release(&p);
    }
}

#if MG_WITH_LC3
/* A live stream at the floor (70 ms) after the start rose to 70 ms: its credit with nothing in flight and no
 * allowance lets 40 ms in (70 less the 30 ms output path), so it starts at 40 ms instead of waiting for more. */
static void test_live_start_within_credit(void)
{
    static mg_play_t q;
    static uint8_t z[3200];
    uint8_t out[MG_PLAY_MAX_POP];
    mg_play_init(&q, 16384);
    q.out_us = 30000;
    q.start_us = 70000;
    q.live_allow_us = 0;
    mg_play_params_t lc3 = { .codec = mg_data_type_audio_lc3, .rate = 16000, .frame_bytes = 40, .live_depth_ms = 50 };
    CHECK(mg_play_start(&q, &lc3) && q.live_us == 70000 && mg_play_start_threshold_us(&q) == 70000);
    uint32_t c = mg_play_credit(&q, 0);
    CHECK(c == 160);   /* 40 ms of LC3 at 32 kb/s */
    CHECK(mg_play_write(&q, z, c) && mg_play_credit(&q, 0) == 0);
    mg_play_pop(&q, out, sizeof(out), 10000);
    CHECK(q.state == MG_PLAY_PLAYING);
    printf("live start within its credit: depth 70 ms, output 30 ms, start 70 ms: playing at 40 ms held\n");
}
#endif

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "resync") == 0) {
        /* The flush resync alone (test_mg_play runs it on each LC3 build, the fixed-point lc3fx included). */
        test_flush_resync();
        test_flush_signed();
        if (failures) {
            fprintf(stderr, "FAIL: %d of %d checks\n", failures, checks);
            return 1;
        }
        printf("PASS mg_play resync (%d checks)\n", checks);
        return 0;
    }
    test_resampler();
    test_check();
    test_ring();
    test_decode();
    test_boost();
    test_tick();
    test_live();
#if MG_WITH_LC3
    test_live_start_within_credit();
#endif
    test_flush_resync();
    test_flush_signed();
    if (failures) {
        fprintf(stderr, "FAIL: %d of %d checks\n", failures, checks);
        return 1;
    }
    printf("PASS mg_play (%d checks)\n", checks);
    return 0;
}
