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
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "muse_audio_dma.h"
#include "muse_board.h"
#include "muse_mem.h"
#include "muse_mirror_audio.h"
#include "muse_settings.h"
#include "muse_motion.h"
#include "muse_spk_dsp.h"   /* the speech chain, on a board with muse_board_t.speaker_speech_chain */

static const char *TAG = "muse_audio";

#define CHANNELS 2
#define HPF_HZ 80.0f

static esp_codec_dev_handle_t s_spk;
static esp_codec_dev_handle_t s_mic;
static bool s_open;
/* The codecs should be open (initialized, not resting) but aren't: their DMA
 * buffers didn't fit (open_codecs). Reads and writes try again, at most once
 * a second, and wait out their audio meanwhile. */
static bool s_want_open;
static int64_t s_retry_us;
static bool s_short;   /* the last open was refused for memory */
static int64_t s_short_logged_us;   /* each refused open logs, at most once in 10 s (a mirror joining late sees it) */
/* Opening, closing and the volume and gain setters, which other tasks call:
 * with a board-switched codec supply, none may reach a codec mid-switch. */
static SemaphoreHandle_t s_lock;
static int16_t s_in_stereo[MUSE_AUDIO_CHUNK * CHANNELS];
static int16_t s_in_mono[MUSE_AUDIO_CHUNK];
static int16_t s_out_stereo[MUSE_AUDIO_CHUNK * CHANNELS];

/* One-pole high-pass on the mixed mic signal. */
static float s_hpf_a;
static float s_mic_scale = 1.0f;
static float s_hpf_x1, s_hpf_y1;

/* Which function holds the lock (its name, NULL: free), for a stuck-task
 * report (muse_audio_lock_holder). Each lock() caller names itself after. */
static const char *volatile s_lock_what;

static void lock(void)
{
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    s_lock_what = "?";
}
#define LOCK() do { lock(); s_lock_what = __func__; } while (0)

/* The read and write paths' take (ensure_open): never unbounded. False after
 * timeout_ms, logging who holds it (the read or write then fails). */
static bool lock_within(int timeout_ms, const char *who)
{
    if (s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        const char *holder = s_lock_what;
        ESP_LOGE(TAG, "%s: the audio lock is still held by %s after %d ms", who, holder ? holder : "?", timeout_ms);
        return false;
    }
    s_lock_what = who;
    return true;
}

static void unlock(void)
{
    s_lock_what = NULL;
    if (s_lock) {
        xSemaphoreGive(s_lock);
    }
}

const char *muse_audio_lock_holder(void)
{
    return s_lock_what;
}

static void codec_power(bool on)
{
    if (muse_board->codec_power) {
        muse_board->codec_power(on);
    }
}

/* A closed codec on a board-switched supply has no power. */
static bool codecs_reachable(void)
{
    return s_open || !muse_board->codec_power;
}

static void apply_volume(int volume)
{
    if (s_spk && codecs_reachable()) {
        esp_codec_dev_set_out_vol(s_spk, volume);
    }
}

/* set_mic_gain's per-connection gain (muse_audio_mic_gain_override), dB, or
 * -1: none, the user's Settings gain. Set from any task without the lock
 * (the BLE host at a disconnect); the next read applies it (dirty). */
static volatile int s_mic_gain_override = -1;
static volatile bool s_mic_gain_dirty;

int muse_audio_mic_gain(void)
{
    int o = s_mic_gain_override;
    return o >= 0 ? o : muse_settings_mic_gain();
}

static void apply_mic_gain(int db)
{
    if (muse_board->mic_init || (!s_mic && muse_board->mic_read)) {
        s_mic_scale = powf(10.0f, (float)db / 20.0f);   /* no codec gain: scale samples */
        return;
    }
    if (!s_mic || !codecs_reachable()) {
        return;
    }
    if (muse_board->set_mic_gain) {
        muse_board->set_mic_gain(s_mic, db);
    } else {
        esp_codec_dev_set_in_gain(s_mic, (float)db);
    }
}

/* Each DMA buffer the I2S channels hold since the codecs last opened (0:
 * none known yet). Closing (resting) disables the channels but keeps their
 * buffers, and ESP-IDF reallocates them only for a new size (i2s_std_set_slot),
 * so reopening in the same format needs no new memory. */
static size_t s_held_buf;

static muse_audio_dma_t dma_budget(void)
{
    return muse_audio_dma_budget(muse_board->i2s_dma_desc, muse_board->i2s_dma_frames, CHANNELS * sizeof(int16_t), 2);
}

/*
 * Whether the DMA buffers opening the codecs takes fit in internal memory
 * (muse_audio_dma.h): if ESP-IDF can't allocate them, esp_codec_dev_open()
 * enables the I2S channel anyway and it faults. Buffers the channels already
 * hold in this format need nothing new (on the StopWatch on battery, each
 * wake from rest asked for 12.5 KB more that a BLE video build doesn't have
 * free, so the codecs never opened again and every capture read nothing).
 * Logs a refusal once, until an open succeeds again.
 */
static bool dma_fits(void)
{
    const muse_audio_dma_t b = dma_budget();
    if (s_held_buf && b.buf == s_held_buf) {
        return true;   /* the same buffers again: no reallocation */
    }
    const size_t free = heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (muse_audio_dma_fits(&b, free, largest)) {
        return true;
    }
    int64_t now = esp_timer_get_time();
    if (!s_short || now - s_short_logged_us >= 10 * 1000 * 1000) {
        s_short_logged_us = now;
        ESP_LOGE(TAG, "audio unavailable: %u B DMA needed (%u B buffers), %u B free, largest %u B; voice stays off",
                 (unsigned)b.need, (unsigned)b.buf, (unsigned)free, (unsigned)largest);
    }
    s_short = true;
    return false;
}

/* ESP_ERR_NO_MEM: not opened, their DMA buffers wouldn't fit (try again later). */
static esp_err_t open_codecs(void)
{
    esp_codec_dev_sample_info_t fs = {
        .sample_rate = MUSE_AUDIO_RATE,
        .channel = CHANNELS,
        .bits_per_sample = 16,
    };
    if (!dma_fits()) {
        return ESP_ERR_NO_MEM;
    }
    ESP_RETURN_ON_FALSE(esp_codec_dev_open(s_spk, &fs) == ESP_CODEC_DEV_OK, ESP_FAIL, TAG, "open speaker");
    if (s_mic) {
        if (esp_codec_dev_open(s_mic, &fs) != ESP_CODEC_DEV_OK) {
            esp_codec_dev_close(s_spk);
            ESP_LOGE(TAG, "open mic");
            return ESP_FAIL;
        }
    }
    s_open = true;
    s_held_buf = dma_budget().buf;   /* the channels hold buffers of this size from here on */
    if (s_short) {
        ESP_LOGI(TAG, "audio available again");
    }
    s_short = false;
    return ESP_OK;
}

/* The device's own sounds' speech chain (muse_board_t.speaker_speech_chain; muse_audio_write's callers, all on
 * the voice task), and when the last of them ended: after a gap the speaker played out, it starts afresh. */
static muse_spk_chain_t *s_chain;
static int64_t s_chain_us;
#define CHAIN_IDLE_US 100000   /* longer than the DMA's 30 ms and a 20 ms piece: the speaker ran dry */

static void speech_chain_init(void)
{
    if (!muse_board->speaker_speech_chain || s_chain) {
        return;
    }
    /* In PSRAM where there is some; its settings read from NVS now, at boot, not on a playback task's first
     * chunk. */
    s_chain = heap_caps_malloc(sizeof(*s_chain), MUSE_BIG_CAPS);
    if (s_chain) {
        memset(s_chain, 0, sizeof(*s_chain));
    }
    bool on;
    muse_spk_dsp_config(NULL, &on, NULL);
    ESP_LOGI(TAG, "speaker speech chain %s%s", on ? "on" : "off (>spk.dsp)",
             s_chain ? "" : " (no memory: the device's sounds unchained)");
}

static esp_err_t read_input(size_t frames)
{
    if (s_mic) {
        return esp_codec_dev_read(s_mic, s_in_stereo, frames * CHANNELS * sizeof(int16_t)) == ESP_CODEC_DEV_OK
            ? ESP_OK : ESP_FAIL;
    }
    /* A mic without a codec beside a speaker codec, for the self-test and
     * loopback test (muse_audio_read reads it directly): mono, its DC
     * dropped before the gain so an offset doesn't saturate the samples. */
    if (!muse_board->mic_read || muse_board->mic_read(s_in_mono, frames) != ESP_OK) {
        return ESP_FAIL;
    }
    float dc = 0;
    for (size_t i = 0; i < frames; i++) {
        dc += s_in_mono[i];
    }
    dc /= (float)frames;
    for (size_t i = 0; i < frames; i++) {
        float x = ((float)s_in_mono[i] - dc) * s_mic_scale;
        int16_t v = (int16_t)(x > 32767 ? 32767 : (x < -32768 ? -32768 : x));
        s_in_stereo[2 * i] = v;
        s_in_stereo[2 * i + 1] = v;
    }
    return ESP_OK;
}

esp_err_t muse_audio_init(int volume, int mic_gain_db)
{
    s_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_NO_MEM, TAG, "audio lock");
    esp_err_t err;
    if (muse_board->mic_init) {
        err = muse_board->mic_init();
        if (err == ESP_OK) err = muse_board->mic_power(true);
        if (err != ESP_OK) return err;
        s_open = true;
    } else {
        /* A board-switched supply goes off again on any failure: nothing will
         * open the codecs, and the setters skip them while closed. */
        codec_power(true);
        err = muse_board->audio_init(&s_spk, &s_mic);
        if (err == ESP_OK && !(s_spk && (s_mic || muse_board->mic_read))) {
            err = ESP_FAIL;
        }
        if (err == ESP_OK && muse_board->i2s_ready_at_init) {
            s_held_buf = dma_budget().buf;   /* the board's channels already hold these buffers */
        }
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "codec init failed");
        } else {
            err = open_codecs();   /* logs its own failure */
        }
        if (err == ESP_ERR_NO_MEM) {
            /* No room for the DMA buffers yet: closed and unpowered, voice
             * off, everything else carries on; reads, writes and the next
             * wake try again. */
            codec_power(false);
            err = ESP_OK;
        } else if (err != ESP_OK) {
            codec_power(false);
            return err;
        }
        s_want_open = true;
    }
    muse_audio_set_volume(volume);
    muse_audio_set_mic_gain(mic_gain_db);
    speech_chain_init();

    float rc = 1.0f / (2.0f * (float)M_PI * HPF_HZ);
    float dt = 1.0f / MUSE_AUDIO_RATE;
    s_hpf_a = rc / (rc + dt);

    ESP_LOGI(TAG, "audio ready: %d Hz, %d ch, vol %d, mic gain %d dB", MUSE_AUDIO_RATE, CHANNELS, volume, mic_gain_db);
    return ESP_OK;
}

/* Codecs closed: their supply up, open, the settings applied, the amp on;
 * or, if they won't open, the supply down again (open_codecs closes the
 * speaker if the mic fails, and the amp is still off). Call with the lock. */
static void open_locked(void)
{
    codec_power(true);
    if (open_codecs() == ESP_OK) {
        apply_volume(muse_settings_volume());
        apply_mic_gain(muse_audio_mic_gain());
        if (muse_board->audio_power) {
            muse_board->audio_power(true);
        }
    } else {
        codec_power(false);
    }
}

void muse_audio_power(bool on)
{
    LOCK();
    if (muse_board->mic_init) {
        if (on != s_open && muse_board->mic_power(on) == ESP_OK) s_open = on;
        unlock();
        return;
    }
    if (!s_spk) {
        unlock();
        return;
    }
    s_want_open = on;   /* resting: no read or write opens them again */
    if (on == s_open) {
        unlock();
        return;
    }
    /* The shared I2S bus gets disabled twice (once per codec), which logs a
     * harmless error. */
    esp_log_level_t lvl = esp_log_level_get("i2s_common");
    esp_log_level_set("i2s_common", ESP_LOG_NONE);
    /* The codecs' supply comes up before they open and goes once both have
     * closed. The amp goes on once they're open and set, and off before they
     * close, so it never passes on their power-up or power-down. If they
     * won't open, the next wake (or read or write) tries again. */
    if (on) {
        open_locked();
    } else {
        if (muse_board->audio_power) {
            muse_board->audio_power(false);
        }
        esp_codec_dev_close(s_spk);
        if (s_mic) {
            esp_codec_dev_close(s_mic);
        }
        s_open = false;
        codec_power(false);
    }
    esp_log_level_set("i2s_common", lvl);
    unlock();
}

/*
 * Reads and writes: the codecs are open, or open now. While they should be
 * open but their DMA buffers don't fit (open_codecs), they try again at most
 * once a second; false meanwhile.
 */
static bool ensure_open(void)
{
    if (s_open || muse_board->mic_init) {
        return true;
    }
    if (!s_want_open || esp_timer_get_time() < s_retry_us) {
        return false;
    }
    if (!lock_within(5000, __func__)) {
        return false;
    }
    if (!s_open && s_want_open && s_spk) {
        s_retry_us = esp_timer_get_time() + 1000 * 1000;
        open_locked();
    }
    bool open = s_open;
    unlock();
    return open;
}

/* Closed codecs: no audio, at the audio's own pace, so a caller's loop that
 * counts on reads and writes blocking doesn't spin. */
static esp_err_t closed_wait(size_t frames)
{
    TickType_t t = pdMS_TO_TICKS((frames * 1000 + MUSE_AUDIO_RATE - 1) / MUSE_AUDIO_RATE);
    vTaskDelay(t ? t : 1);
    return ESP_ERR_INVALID_STATE;
}

void muse_audio_set_volume(int volume)
{
    LOCK();
    apply_volume(volume);
    unlock();
}

void muse_audio_set_mic_gain(int db)
{
    LOCK();
    apply_mic_gain(db);
    unlock();
}

int muse_audio_mic_gain_override_db(void)
{
    return s_mic_gain_override;
}

void muse_audio_mic_gain_override(int db)
{
    s_mic_gain_override = db < 0 ? -1 : (db > MUSE_MIC_GAIN_MAX ? MUSE_MIC_GAIN_MAX : db);
    s_mic_gain_dirty = true;
}

/* A gain muse_audio_mic_gain_override changed reaches the codec at the next
 * read, on the reading task (it may wait for the codec's bus). */
static void apply_pending_mic_gain(void)
{
    if (!s_mic_gain_dirty || !lock_within(1000, __func__)) {
        return;
    }
    s_mic_gain_dirty = false;
    apply_mic_gain(muse_audio_mic_gain());
    unlock();
}

static float to_db(double mean_sq)
{
    return 10.0f * log10f((float)(mean_sq / (32768.0 * 32768.0)) + 1e-10f);
}

void muse_audio_selftest(void)
{
    if (muse_board->mic_init) {
        int16_t mono[MUSE_AUDIO_CHUNK];
        if (muse_audio_read(mono, MUSE_AUDIO_CHUNK) == ESP_OK) {
            ESP_LOGI(TAG, "PDM mic capture: %.1f dBFS", muse_audio_dbfs(mono, MUSE_AUDIO_CHUNK));
        }
        return;
    }
    if (!s_open) {
        ESP_LOGW(TAG, "self-test skipped: the codecs aren't open");
        return;
    }
    enum { FLUSH = MUSE_AUDIO_RATE / 4, MEASURE = MUSE_AUDIO_RATE / 2 };

    /* Capture: drop the DMA backlog, then time a known number of frames. */
    for (int n = 0; n < FLUSH; n += MUSE_AUDIO_CHUNK) {
        read_input(MUSE_AUDIO_CHUNK);
    }
    double sl = 0, sr = 0, slr = 0, ml = 0, mr = 0;
    int pl = 0, pr = 0;
    int64_t t0 = esp_timer_get_time();
    for (int n = 0; n < MEASURE; n += MUSE_AUDIO_CHUNK) {
        read_input(MUSE_AUDIO_CHUNK);
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
    if (muse_board->mic_init) {
        ESP_LOGW(TAG, "no speaker for loopback test");
        return;
    }
    if (!s_open) {
        ESP_LOGW(TAG, "loopback test skipped: the codecs aren't open");
        return;
    }
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
                read_input(MUSE_AUDIO_CHUNK);
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
    muse_audio_set_mic_gain(muse_audio_mic_gain());
    muse_audio_set_volume(muse_settings_volume());
}

esp_err_t muse_audio_read(int16_t *mono, size_t frames)
{
    if (!ensure_open()) {
        return closed_wait(frames);
    }
    apply_pending_mic_gain();
    while (frames) {
        size_t n = frames > MUSE_AUDIO_CHUNK ? MUSE_AUDIO_CHUNK : frames;
        /* A mic without a codec: its gain applies in float, ahead of the
         * high-pass, so a DC offset can't saturate the samples first. */
        bool direct = muse_board->mic_init || (!s_mic && muse_board->mic_read);
        if (direct) {
            if (muse_board->mic_read(mono, n) != ESP_OK) return ESP_FAIL;
        } else if (read_input(n) != ESP_OK) {
            return ESP_FAIL;
        }
        for (size_t i = 0; i < n; i++) {
            float x = direct ? (float)mono[i] * s_mic_scale
                     : muse_board->mic_slot < 0 ? 0.5f * ((float)s_in_stereo[2 * i] + (float)s_in_stereo[2 * i + 1])
                                                : (float)s_in_stereo[2 * i + muse_board->mic_slot];
            float y = s_hpf_a * (s_hpf_y1 + x - s_hpf_x1);
            s_hpf_x1 = x;
            s_hpf_y1 = y;
            mono[i] = (int16_t)(y > 32767 ? 32767 : (y < -32768 ? -32768 : y));
        }
        muse_mirror_audio_capture(mono, n);
        mono += n;
        frames -= n;
    }
    return ESP_OK;
}

esp_err_t muse_audio_write(const int16_t *mono, size_t frames)
{
    if (!s_chain || !frames) {
        return muse_audio_write_chained(mono, frames);
    }
    int64_t now = esp_timer_get_time();
    if (now - s_chain_us > CHAIN_IDLE_US) {
        muse_spk_chain_reset(s_chain);   /* (a tail it still held was the end of a sound long gone) */
    }
    esp_err_t err = ESP_OK;
    while (frames && err == ESP_OK) {
        int16_t piece[MUSE_AUDIO_CHUNK];
        size_t n = frames > MUSE_AUDIO_CHUNK ? MUSE_AUDIO_CHUNK : frames;
        memcpy(piece, mono, n * sizeof(int16_t));
        muse_spk_chain_run(s_chain, piece, n, MUSE_AUDIO_RATE);
        err = muse_audio_write_chained(piece, n);
        mono += n;
        frames -= n;
    }
    s_chain_us = esp_timer_get_time();
    return err;
}

void muse_audio_write_end(void)
{
    if (s_chain) {
        static const int16_t zeros[MUSE_SPK_DSP_LOOKAHEAD];
        muse_audio_write(zeros, MUSE_SPK_DSP_LOOKAHEAD);   /* the look-ahead's last 2 ms out */
    }
}

esp_err_t muse_audio_write_chained(const int16_t *mono, size_t frames)
{
    bool mirror_only = muse_mirror_audio_playback(mono, frames);
    if (muse_board->mic_init) {
        (void)mono;
        vTaskDelay(pdMS_TO_TICKS((frames * 1000 + MUSE_AUDIO_RATE - 1) / MUSE_AUDIO_RATE));
        return ESP_OK;
    }
    if (!ensure_open()) {
        return closed_wait(frames);
    }
    /* Speaker off still plays silence at the same pace, so captions timed to
     * the audio scroll as if it were heard. */
    while (frames) {
        bool mute = mirror_only || !muse_settings_speaker_on() || muse_motion_quiet();
        /* Pieces that stay in one DMA buffer, so a barge-in's clear knows where each went (muse_audio_tx.h). */
        size_t n = muse_audio_tx_note_begin(frames > MUSE_AUDIO_CHUNK ? MUSE_AUDIO_CHUNK : frames);
        for (size_t i = 0; i < n; i++) {
            s_out_stereo[2 * i] = mute ? 0 : mono[i];
            s_out_stereo[2 * i + 1] = mute ? 0 : mono[i];
        }
        if (esp_codec_dev_write(s_spk, s_out_stereo, n * CHANNELS * sizeof(int16_t)) != ESP_CODEC_DEV_OK) {
            return ESP_FAIL;
        }
        muse_audio_tx_note_write(mono, n, mute);   /* in the DMA now: what a clear can hand back */
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
    muse_audio_write_end();
}

#if CONFIG_MUSE_REPLY_NOTIFY
void muse_audio_notify(void)
{
    /* E6 then A6, 110 ms each with a bell-like decay: short and clear on the
     * Watcher's small speaker, quieter than speech. */
    enum { MS = 110, N = MUSE_AUDIO_RATE * MS / 1000 };
    static int16_t buf[N];
    static const float NOTES[] = { 1318.5f, 1760.0f };
    for (size_t k = 0; k < sizeof(NOTES) / sizeof(NOTES[0]); k++) {
        float phase = 0;
        for (int i = 0; i < N; i++) {
            float x = (float)i / N;
            phase += 2.0f * (float)M_PI * NOTES[k] / MUSE_AUDIO_RATE;
            float env = (x < 0.05f ? x / 0.05f : 1.0f) * expf(-4.0f * x);
            float v = sinf(phase) + 0.25f * sinf(2.0f * phase);
            buf[i] = (int16_t)(v * env * 6000.0f);
        }
        muse_audio_write(buf, N);
    }
}
#endif
