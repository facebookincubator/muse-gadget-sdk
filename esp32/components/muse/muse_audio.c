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

#include "muse_audio.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "muse_board.h"
#include "muse_mem.h"
#include "muse_settings.h"

static const char *TAG = "muse_audio";

#define CHANNELS 2
#define HPF_HZ 80.0f

static esp_codec_dev_handle_t s_spk;
static esp_codec_dev_handle_t s_mic;
static bool s_open;
static int16_t s_in_stereo[MUSE_AUDIO_CHUNK * CHANNELS];
static int16_t s_out_stereo[MUSE_AUDIO_CHUNK * CHANNELS];

/* One-pole high-pass on the mixed mic signal. */
static float s_hpf_a;
static float s_hpf_x1, s_hpf_y1;

static esp_err_t open_codecs(void)
{
    esp_codec_dev_sample_info_t fs = {
        .sample_rate = MUSE_AUDIO_RATE,
        .channel = CHANNELS,
        .bits_per_sample = 16,
    };
    ESP_RETURN_ON_FALSE(esp_codec_dev_open(s_spk, &fs) == ESP_CODEC_DEV_OK, ESP_FAIL, TAG, "open speaker");
    if (esp_codec_dev_open(s_mic, &fs) != ESP_CODEC_DEV_OK) {
        esp_codec_dev_close(s_spk);   /* none open on failure */
        ESP_LOGE(TAG, "open mic");
        return ESP_FAIL;
    }
    s_open = true;
    return ESP_OK;
}

esp_err_t muse_audio_init(int volume, int mic_gain_db)
{
    ESP_RETURN_ON_ERROR(muse_board->audio_init(&s_spk, &s_mic), TAG, "codec init failed");
    ESP_RETURN_ON_FALSE(s_spk && s_mic, ESP_FAIL, TAG, "codec init failed");

    ESP_RETURN_ON_ERROR(open_codecs(), TAG, "open codecs");
    muse_audio_set_volume(volume);
    muse_audio_set_mic_gain(mic_gain_db);

    float rc = 1.0f / (2.0f * (float)M_PI * HPF_HZ);
    float dt = 1.0f / MUSE_AUDIO_RATE;
    s_hpf_a = rc / (rc + dt);

    ESP_LOGI(TAG, "audio ready: %d Hz, %d ch, vol %d, mic gain %d dB", MUSE_AUDIO_RATE, CHANNELS, volume, mic_gain_db);
    return ESP_OK;
}

void muse_audio_power(bool on)
{
    if (!s_spk || on == s_open) {
        return;
    }
    /* The shared I2S bus gets disabled twice (once per codec), which logs a
     * harmless error. */
    esp_log_level_t lvl = esp_log_level_get("i2s_common");
    esp_log_level_set("i2s_common", ESP_LOG_NONE);
    /* The amp goes on once the codecs are open and set, and off before they
     * close, so it never passes on their power-up or power-down. */
    if (on) {
        if (open_codecs() == ESP_OK) {
            muse_audio_set_volume(muse_settings_volume());
            muse_audio_set_mic_gain(muse_settings_mic_gain());
            if (muse_board->audio_power) {
                muse_board->audio_power(true);
            }
        }
    } else {
        if (muse_board->audio_power) {
            muse_board->audio_power(false);
        }
        esp_codec_dev_close(s_spk);
        esp_codec_dev_close(s_mic);
        s_open = false;
    }
    esp_log_level_set("i2s_common", lvl);
}

void muse_audio_set_volume(int volume)
{
    if (s_spk) {
        esp_codec_dev_set_out_vol(s_spk, volume);
    }
}

void muse_audio_set_mic_gain(int db)
{
    if (!s_mic) {
        return;
    }
    if (muse_board->set_mic_gain) {
        muse_board->set_mic_gain(s_mic, db);
    } else {
        esp_codec_dev_set_in_gain(s_mic, (float)db);
    }
}

static float to_db(double mean_sq)
{
    return 10.0f * log10f((float)(mean_sq / (32768.0 * 32768.0)) + 1e-10f);
}

void muse_audio_selftest(void)
{
    enum { FLUSH = MUSE_AUDIO_RATE / 4, MEASURE = MUSE_AUDIO_RATE / 2 };

    /* Capture: drop the DMA backlog, then time a known number of frames. */
    for (int n = 0; n < FLUSH; n += MUSE_AUDIO_CHUNK) {
        esp_codec_dev_read(s_mic, s_in_stereo, sizeof(s_in_stereo));
    }
    double sl = 0, sr = 0, slr = 0, ml = 0, mr = 0;
    int pl = 0, pr = 0;
    int64_t t0 = esp_timer_get_time();
    for (int n = 0; n < MEASURE; n += MUSE_AUDIO_CHUNK) {
        esp_codec_dev_read(s_mic, s_in_stereo, sizeof(s_in_stereo));
        for (int i = 0; i < MUSE_AUDIO_CHUNK; i++) {
            int l = s_in_stereo[2 * i], r = s_in_stereo[2 * i + 1];
            ml += l;
            mr += r;
            sl += (double)l * l;
            sr += (double)r * r;
            slr += (double)l * r;
            pl = abs(l) > pl ? abs(l) : pl;
            pr = abs(r) > pr ? abs(r) : pr;
        }
    }
    float cap_hz = MEASURE * 1e6f / (float)(esp_timer_get_time() - t0);
    ml /= MEASURE;
    mr /= MEASURE;
    double vl = sl / MEASURE - ml * ml, vr = sr / MEASURE - mr * mr;
    double corr = (slr / MEASURE - ml * mr) / (sqrt(vl * vr) + 1e-9);

    /* Playback: fill the DMA queue with silence, then time more silence. */
    memset(s_out_stereo, 0, sizeof(s_out_stereo));
    for (int n = 0; n < FLUSH; n += MUSE_AUDIO_CHUNK) {
        esp_codec_dev_write(s_spk, s_out_stereo, sizeof(s_out_stereo));
    }
    t0 = esp_timer_get_time();
    for (int n = 0; n < MEASURE; n += MUSE_AUDIO_CHUNK) {
        esp_codec_dev_write(s_spk, s_out_stereo, sizeof(s_out_stereo));
    }
    float play_hz = MEASURE * 1e6f / (float)(esp_timer_get_time() - t0);

    ESP_LOGI(TAG, "self-test @ %d dB: mic L %.1f dBFS (peak %d, dc %d), R %.1f dBFS (peak %d, dc %d), corr %.2f",
             muse_settings_mic_gain(), to_db(vl), pl, (int)ml, to_db(vr), pr, (int)mr, corr);
    ESP_LOGI(TAG, "self-test: capture %.0f Hz, playback %.0f Hz (expected %d)", cap_hz, play_hz, MUSE_AUDIO_RATE);
}

/* Goertzel power of `f` in an interleaved stereo buffer, as dBFS of a sine. */
static float tone_db(const int16_t *st, int ch, size_t frames, float f)
{
    float k = 2.0f * cosf(2.0f * (float)M_PI * f / MUSE_AUDIO_RATE);
    float s1 = 0, s2 = 0;
    for (size_t i = 0; i < frames; i++) {
        float s0 = st[2 * i + ch] + k * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    float p = s1 * s1 + s2 * s2 - k * s1 * s2;
    float amp = 2.0f * sqrtf(p > 0 ? p : 0) / frames;   /* sine amplitude */
    return 20.0f * log10f(amp / 32768.0f + 1e-7f);
}

/*
 * Full-duplex loopback: plays tones on the speaker while recording both mics,
 * at each PGA step. Writes and reads alternate chunk by chunk; the DMA queues
 * absorb the phase offset.
 */
void muse_audio_loopback_test(int volume)
{
    enum { TONE = MUSE_AUDIO_RATE * 3 / 10, SKIP = MUSE_AUDIO_RATE / 10, MEAS = TONE - SKIP };
    static const float freqs[] = { 200, 400, 700, 1000, 2000, 3000, 4000, 6000 };
    static const int gains[] = { 0, 6, 12, 18, 24, 30, 33, 36 };
    const float amp = 0.25f * 32767.0f;   /* -12 dBFS tone */
    int16_t *cap = heap_caps_malloc(MEAS * CHANNELS * sizeof(int16_t), MUSE_BIG_CAPS);
    if (!cap) {
        return;
    }
    muse_audio_set_volume(volume);
    ESP_LOGI(TAG, "loopback: speaker vol %d, tone -12 dBFS; values = mic L/R tone level dBFS", volume);

    for (size_t g = 0; g < sizeof(gains) / sizeof(gains[0]); g++) {
        muse_audio_set_mic_gain(gains[g]);
        char line[256];
        int len = snprintf(line, sizeof(line), "gain %2d:", gains[g]);
        int peak = 0;
        /* Silence first for the noise floor, then each tone. */
        for (int t = -1; t < (int)(sizeof(freqs) / sizeof(freqs[0])); t++) {
            float f = t < 0 ? 0 : freqs[t];
            float phase = 0;
            size_t got = 0;
            for (int n = 0; n < TONE + SKIP; n += MUSE_AUDIO_CHUNK) {
                for (int i = 0; i < MUSE_AUDIO_CHUNK; i++) {
                    int16_t v = f ? (int16_t)(amp * sinf(phase)) : 0;
                    phase += 2.0f * (float)M_PI * f / MUSE_AUDIO_RATE;
                    s_out_stereo[2 * i] = s_out_stereo[2 * i + 1] = v;
                }
                esp_codec_dev_write(s_spk, s_out_stereo, sizeof(s_out_stereo));
                esp_codec_dev_read(s_mic, s_in_stereo, sizeof(s_in_stereo));
                /* Skip the first SKIP*2 frames: DMA latency plus the previous tone's tail. */
                if (n >= 2 * SKIP && got + MUSE_AUDIO_CHUNK <= MEAS) {
                    memcpy(cap + got * CHANNELS, s_in_stereo, sizeof(s_in_stereo));
                    got += MUSE_AUDIO_CHUNK;
                }
            }
            for (size_t i = 0; i < got * CHANNELS; i++) {
                peak = abs(cap[i]) > peak ? abs(cap[i]) : peak;
            }
            if (t < 0) {
                double sl = 0, sr = 0;
                for (size_t i = 0; i < got; i++) {
                    sl += (double)cap[2 * i] * cap[2 * i];
                    sr += (double)cap[2 * i + 1] * cap[2 * i + 1];
                }
                len += snprintf(line + len, sizeof(line) - len, " floor %.0f/%.0f |", to_db(sl / got), to_db(sr / got));
                peak = 0;
            } else {
                len += snprintf(line + len, sizeof(line) - len, " %.0f:%.0f/%.0f", f,
                                tone_db(cap, 0, got, f), tone_db(cap, 1, got, f));
            }
        }
        ESP_LOGI(TAG, "%s | peak %.1f dBFS", line, 20.0 * log10((peak + 1) / 32768.0));
    }
    free(cap);
    muse_audio_set_mic_gain(muse_settings_mic_gain());
    muse_audio_set_volume(muse_settings_volume());
}

esp_err_t muse_audio_read(int16_t *mono, size_t frames)
{
    while (frames) {
        size_t n = frames > MUSE_AUDIO_CHUNK ? MUSE_AUDIO_CHUNK : frames;
        if (esp_codec_dev_read(s_mic, s_in_stereo, n * CHANNELS * sizeof(int16_t)) != ESP_CODEC_DEV_OK) {
            return ESP_FAIL;
        }
        for (size_t i = 0; i < n; i++) {
            float x = muse_board->mic_slot < 0 ? 0.5f * ((float)s_in_stereo[2 * i] + (float)s_in_stereo[2 * i + 1])
                                               : (float)s_in_stereo[2 * i + muse_board->mic_slot];
            float y = s_hpf_a * (s_hpf_y1 + x - s_hpf_x1);
            s_hpf_x1 = x;
            s_hpf_y1 = y;
            mono[i] = (int16_t)(y > 32767 ? 32767 : (y < -32768 ? -32768 : y));
        }
        mono += n;
        frames -= n;
    }
    return ESP_OK;
}

esp_err_t muse_audio_write(const int16_t *mono, size_t frames)
{
    /* Speaker off still plays silence at the same pace, so captions timed to
     * the audio scroll as if it were heard. */
    bool mute = !muse_settings_speaker_on();
    while (frames) {
        size_t n = frames > MUSE_AUDIO_CHUNK ? MUSE_AUDIO_CHUNK : frames;
        for (size_t i = 0; i < n; i++) {
            s_out_stereo[2 * i] = mute ? 0 : mono[i];
            s_out_stereo[2 * i + 1] = mute ? 0 : mono[i];
        }
        if (esp_codec_dev_write(s_spk, s_out_stereo, n * CHANNELS * sizeof(int16_t)) != ESP_CODEC_DEV_OK) {
            return ESP_FAIL;
        }
        mono += n;
        frames -= n;
    }
    return ESP_OK;
}

float muse_audio_dbfs(const int16_t *mono, size_t frames)
{
    if (!frames) {
        return -100.0f;
    }
    double acc = 0;
    for (size_t i = 0; i < frames; i++) {
        acc += (double)mono[i] * mono[i];
    }
    float db = to_db(acc / frames);
    return db < -100.0f ? -100.0f : db;
}

float muse_audio_level(const int16_t *mono, size_t frames)
{
    /* Map roughly -54..-12 dBFS onto 0..1. */
    float l = (muse_audio_dbfs(mono, frames) + 54.0f) / 42.0f;
    return l < 0 ? 0 : (l > 1 ? 1 : l);
}

void muse_audio_chirp(int rising)
{
    enum { MS = 90, N = MUSE_AUDIO_RATE * MS / 1000 };
    static int16_t buf[N];
    float f0 = rising ? 620.0f : 980.0f;
    float f1 = rising ? 1180.0f : 560.0f;
    float phase = 0;
    for (int i = 0; i < N; i++) {
        float x = (float)i / N;
        float f = f0 + (f1 - f0) * x;
        phase += 2.0f * (float)M_PI * f / MUSE_AUDIO_RATE;
        float env = (x < 0.1f ? x / 0.1f : 1.0f) * (1.0f - x) * (1.0f - x);
        /* A touch of the 3rd harmonic for a soft 8-bit flavour. */
        float s = sinf(phase) + 0.3f * sinf(3.0f * phase);
        buf[i] = (int16_t)(s * env * 7000.0f);
    }
    muse_audio_write(buf, N);
}
