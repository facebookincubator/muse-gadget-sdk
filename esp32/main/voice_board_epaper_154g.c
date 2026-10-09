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

#include "voice_board.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "link_pairing.h"
#include "voice_epaper_154g.h"

// Waveshare's S3_ePaper_1_54 codec_board record: one ES8311, one analog
// microphone. Both directions use the same 16 kHz clock pair and MCLK.
#define AUDIO_POWER 42  // active low
#define AUDIO_PA    46  // active high; controlled here, not by the codec
#define SETUP_BUTTON 18 // hold PWR to leave BOOT available for setup
#define AUDIO_CHUNK 320
// Above the network tasks, so a TLS handshake cannot starve the microphone.
#define CAPTURE_PRIORITY 6
// voice.c drops presses shorter than its CAPTURE_MIN_MS (300 ms): no end cue for those.
#define END_CUE_FRAMES   (VOICE_MIC_RATE * 300 / 1000)

static const char *TAG = "link.voice_board";
static SemaphoreHandle_t s_lock, s_cue_lock;
static TaskHandle_t s_cue_task;
static i2s_chan_handle_t s_tx, s_rx;
static const audio_codec_ctrl_if_t *s_ctrl;
static const audio_codec_data_if_t *s_data;
static const audio_codec_if_t *s_codec;
static esp_codec_dev_handle_t s_dev;
static bool s_recording, s_playing, s_cue;
static UBaseType_t s_priority;
static size_t s_frames; // read since voice_board_mic_start(), under s_lock
static int s_volume = 60;
// Every use and conversion is under s_lock, including microphone unpacking.
static int16_t s_stereo[AUDIO_CHUNK * 2];

static void power_down(void) {
    gpio_set_level(AUDIO_PA, 0);
    if (s_dev) {
        // Also disables the I2S data after an open() that failed partway.
        esp_codec_dev_close(s_dev);
        esp_codec_dev_delete(s_dev);
        s_dev = NULL;
    }
    if (s_codec) {
        audio_codec_delete_codec_if(s_codec);
        s_codec = NULL;
    }
    gpio_set_level(AUDIO_POWER, 1);
}

static esp_err_t power_up(void) {
    if (s_dev) return ESP_OK;
    gpio_set_level(AUDIO_PA, 0);
    gpio_set_level(AUDIO_POWER, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    // Recreate the codec after each rail power cycle: its register contents
    // were lost. A cached, already-open codec interface would skip its init.
    es8311_codec_cfg_t cfg = {
        .ctrl_if = s_ctrl,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH,
        .pa_pin = -1,
        .use_mclk = true,
        .hw_gain = { .pa_voltage = 3.3, .codec_dac_voltage = 3.3 },
    };
    s_codec = es8311_codec_new(&cfg);
    if (!s_codec) goto fail;
    esp_codec_dev_cfg_t dev = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN_OUT,
        .codec_if = s_codec, .data_if = s_data,
    };
    s_dev = esp_codec_dev_new(&dev);
    esp_codec_dev_sample_info_t fs = {
        .sample_rate = VOICE_MIC_RATE, .channel = 2, .bits_per_sample = 16,
    };
    if (!s_dev) goto fail;
    // open() disables both channels before it sets the format; that logs an
    // error for a channel that is not running yet.
    i2s_channel_enable(s_tx);
    i2s_channel_enable(s_rx);
    if (esp_codec_dev_open(s_dev, &fs) != ESP_CODEC_DEV_OK) goto fail;
    if (esp_codec_dev_set_in_gain(s_dev, 30.0f) != ESP_CODEC_DEV_OK ||
        esp_codec_dev_set_out_vol(s_dev, s_volume) != ESP_CODEC_DEV_OK) goto fail;
    return ESP_OK;
fail:
    power_down();
    return ESP_FAIL;
}

// Under s_lock.
static bool speaker_on(void) {
    if (power_up() != ESP_OK) return false;
    if (!gpio_get_level(AUDIO_PA)) {
        gpio_set_level(AUDIO_PA, 1);
        // NS4150B startup is typically 120 ms. Queue no tone before it wakes.
        vTaskDelay(pdMS_TO_TICKS(150));
    }
    return true;
}

// Under s_lock: switch off whatever nothing uses any more.
static void release(void) {
    if (!s_playing && !s_cue) gpio_set_level(AUDIO_PA, 0);
    if (!s_playing && !s_cue && !s_recording) power_down();
}

// Under s_lock.
static esp_err_t write_stereo(const int16_t *stereo, size_t frames) {
    const uint8_t *p = (const uint8_t *)stereo;
    size_t left = frames * sizeof(int16_t) * 2;
    esp_err_t err = ESP_OK;
    while (left && err == ESP_OK) {
        size_t sent = 0;
        err = i2s_channel_write(s_tx, p, left, &sent, 200);
        if (err == ESP_OK && !sent) err = ESP_ERR_TIMEOUT;
        p += sent;
        left -= sent;
    }
    return err;
}

// A local cue only: no network turn and no microphone data is sent by this.
// The caller holds s_cue_lock.
static void cue(bool end, bool button) {
    static const int16_t wave[16] = {
        0, 383, 707, 924, 1000, 924, 707, 383,
        0, -383, -707, -924, -1000, -924, -707, -383,
    };
    static int16_t tone[160 * 2];
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_cue = true;
    esp_err_t err = speaker_on() ? ESP_OK : ESP_FAIL;
    xSemaphoreGive(s_lock);
    for (size_t chunk = 0; chunk < 5 && err == ESP_OK; chunk++) {
        if (button && gpio_get_level(CONFIG_HOMEHUB_BUTTON_GPIO) == 0) break;
        for (size_t i = 0; i < 160; i++) {
            size_t sample = chunk * 160 + i;
            // 1 kHz start, 500 Hz finish; 5 ms fades reduce the edges.
            int gain = sample < 80 ? sample : sample >= 720 ? 799 - sample : 80;
            tone[2 * i] = tone[2 * i + 1] = wave[(end ? sample / 2 : sample) % 16] * 8192 * gain / 80000;
        }
        xSemaphoreTake(s_lock, portMAX_DELAY);
        err = write_stereo(tone, 160);
        xSemaphoreGive(s_lock);
    }
    int64_t tail_until = esp_timer_get_time() + 120000;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!end && err == ESP_OK && s_recording) {
        // The mic is already powered. Drain its settling samples during the
        // cue tail, not after the user starts talking.
        for (int i = 0; i < 4 && err == ESP_OK; i++) {
            size_t got = 0;
            err = i2s_channel_read(s_rx, s_stereo, sizeof(s_stereo), &got, 100);
            if (err == ESP_ERR_TIMEOUT) err = ESP_OK;
        }
    }
    xSemaphoreGive(s_lock);
    int64_t left = tail_until - esp_timer_get_time();
    if (left > 0) vTaskDelay(pdMS_TO_TICKS((left + 999) / 1000));
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_cue = false;
    release();
    xSemaphoreGive(s_lock);
    if (err != ESP_OK) ESP_LOGW(TAG, "cue failed: %s", esp_err_to_name(err));
}

// voice.c reads the mic from a priority 4 task, below the network tasks. On
// battery, with the CPU scaled down, a TLS handshake there dropped speech, so
// the reader runs at CAPTURE_PRIORITY until voice_board_mic_stop(). The start
// cue and the codec warm-up take about 0.3 s after voice.c shows LISTENING;
// the LED turns steady only once they are done.
esp_err_t voice_board_mic_start(void) {
    xSemaphoreTake(s_cue_lock, portMAX_DELAY);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = power_up();
    s_recording = err == ESP_OK;
    s_frames = 0;
    xSemaphoreGive(s_lock);
    if (err == ESP_OK) {
        s_priority = uxTaskPriorityGet(NULL);
        vTaskPrioritySet(NULL, CAPTURE_PRIORITY);
        cue(false, false);
        voice_epaper_154g_mic_ready(true);
    }
    xSemaphoreGive(s_cue_lock);
    return err;
}

void voice_board_mic_stop(void) {
    xSemaphoreTake(s_cue_lock, portMAX_DELAY);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool recording = s_recording;
    bool long_enough = s_frames >= END_CUE_FRAMES;
    xSemaphoreGive(s_lock);
    if (recording) {
        voice_epaper_154g_mic_ready(false);
        if (long_enough) cue(true, false);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_recording = false;
        release();
        xSemaphoreGive(s_lock);
    }
    xSemaphoreGive(s_cue_lock);
    if (recording) vTaskPrioritySet(NULL, s_priority);
}

size_t voice_board_mic_read(int16_t *pcm, size_t frames, int *peak) {
    int16_t *stereo = s_stereo;
    if (frames > AUDIO_CHUNK) frames = AUDIO_CHUNK;
    *peak = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    size_t bytes = 0;
    esp_err_t err = s_recording ?
        i2s_channel_read(s_rx, stereo, frames * sizeof(int16_t) * 2, &bytes, 100) :
        ESP_ERR_INVALID_STATE;
    // TIMEOUT can include valid samples. Never drop those or end a turn early.
    if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
        xSemaphoreGive(s_lock);
        return 0;
    }
    size_t n = bytes / (sizeof(int16_t) * 2);
    if (n > frames) n = frames;
    s_frames += n;
    for (size_t i = 0; i < n; i++) {
        pcm[i] = stereo[2 * i]; // ES8311 ADC's left slot, not a second mic
        int value = pcm[i];
        if (value < 0) value = -value;
        if (value > *peak) *peak = value;
    }
    xSemaphoreGive(s_lock);
    return n;
}

// The player calls this around every reply. The speaker powers up on the
// first audible frame instead, so a silent text reply leaves the codec off.
void voice_board_amp(bool on) {
    if (on) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_playing = false;
    release();
    xSemaphoreGive(s_lock);
}

esp_err_t voice_board_speaker_write(const int32_t *frames, size_t count) {
    // The player produces 48 kHz. Average each three stereo frames to the
    // codec's 16 kHz bus; player chunks are always a multiple of three.
    if (count % 3) return ESP_ERR_INVALID_SIZE;
    bool audible = false;
    for (size_t i = 0; i < count * 2; i++) audible |= frames[i] / 65536 != 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    // A cue owns the speaker; a reply keeps its pace without writing over it.
    if (!s_playing && audible && !s_cue) s_playing = speaker_on();
    bool playing = s_playing && !s_cue;
    xSemaphoreGive(s_lock);
    if (!playing) {
        vTaskDelay(pdMS_TO_TICKS(count * 1000 / VOICE_SPEAKER_RATE));
        return ESP_OK;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = ESP_OK;
    for (size_t at = 0; at < count && err == ESP_OK;) {
        size_t n = 0;
        for (; n < AUDIO_CHUNK && at < count; n++, at += 3) {
            for (size_t c = 0; c < 2; c++) {
                int64_t sum = (int64_t)frames[2 * at + c] + frames[2 * at + 2 + c] + frames[2 * at + 4 + c];
                s_stereo[2 * n + c] = sum / 3 / 65536;
            }
        }
        err = write_stereo(s_stereo, n);
    }
    xSemaphoreGive(s_lock);
    return err;
}

void voice_board_set_volume(int percent) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_volume = percent < 0 ? 0 : percent > 100 ? 100 : percent;
    if (s_dev) esp_codec_dev_set_out_vol(s_dev, s_volume);
    xSemaphoreGive(s_lock);
}

// BOOT keeps its setup role while PWR is held or a pairing waits for its
// confirmation.
bool voice_board_muted(void) {
    return gpio_get_level(SETUP_BUTTON) == 0 || link_pairing_confirmation_required();
}

int voice_board_dial_steps(void) { return 0; }

// PWR cues play here, so the button task keeps polling during the tone.
static void cue_task(void *arg) {
    for (;;) {
        uint32_t long_hold = 0;
        xTaskNotifyWait(0, UINT32_MAX, &long_hold, portMAX_DELAY);
        // BOOT down means a turn is starting; its own cue follows.
        if (gpio_get_level(CONFIG_HOMEHUB_BUTTON_GPIO) == 0) continue;
        if (xSemaphoreTake(s_cue_lock, 0) != pdTRUE) continue;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        bool recording = s_recording;
        xSemaphoreGive(s_lock);
        if (!recording) cue(long_hold, true);
        xSemaphoreGive(s_cue_lock);
    }
}

void voice_epaper_154g_button_cue(bool long_hold) {
    if (s_cue_task) xTaskNotify(s_cue_task, long_hold, eSetValueWithOverwrite);
}

esp_err_t voice_board_init(void) {
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;
    gpio_set_level(AUDIO_POWER, 1);
    gpio_set_level(AUDIO_PA, 0);
    const gpio_config_t power = {
        .pin_bit_mask = 1ULL << AUDIO_POWER | 1ULL << AUDIO_PA,
        .mode = GPIO_MODE_INPUT_OUTPUT,
    };
    esp_err_t err = gpio_config(&power);
    if (err != ESP_OK) return err;
    i2c_master_bus_handle_t bus;
    err = i2c_master_get_bus_handle(I2C_NUM_0, &bus);
    if (err != ESP_OK) return err;
    i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan.auto_clear = true;
    // 180 ms of microphone, so a busy network task does not drop speech.
    chan.dma_desc_num = 12;
    err = i2s_new_channel(&chan, &s_tx, &s_rx);
    if (err != ESP_OK) return err;
    const i2s_std_config_t std = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(VOICE_MIC_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                       I2S_SLOT_MODE_STEREO),
        .gpio_cfg = { .mclk = 14, .bclk = 15, .ws = 38, .dout = 45, .din = 16 },
    };
    err = i2s_channel_init_std_mode(s_tx, &std);
    if (err == ESP_OK) err = i2s_channel_init_std_mode(s_rx, &std);
    if (err != ESP_OK) return err;
    audio_codec_i2s_cfg_t data = { .port = I2S_NUM_0, .tx_handle = s_tx, .rx_handle = s_rx };
    s_data = audio_codec_new_i2s_data(&data);
    audio_codec_i2c_cfg_t ctrl = {
        .port = I2C_NUM_0, .addr = ES8311_CODEC_DEFAULT_ADDR, .bus_handle = bus,
    };
    s_ctrl = audio_codec_new_i2c_ctrl(&ctrl);
    if (!s_data || !s_ctrl) return ESP_ERR_NO_MEM;
    // Probe at boot with the PA low, then leave the entire audio rail off.
    err = power_up();
    power_down();
    if (err != ESP_OK) return err;
    s_cue_lock = xSemaphoreCreateMutex();
    if (!s_cue_lock) return ESP_ERR_NO_MEM;
    // The stack is in PSRAM: a cue never touches flash.
    if (xTaskCreateWithCaps(cue_task, "cue", 4096, NULL, 3, &s_cue_task, MALLOC_CAP_SPIRAM) != pdPASS)
        return ESP_ERR_NO_MEM;
    ESP_LOGI(TAG, "ES8311 ready: 16000 Hz duplex; PWR+BOOT=setup; audio rail off");
    return ESP_OK;
}
