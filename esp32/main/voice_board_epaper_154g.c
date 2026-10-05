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
#include "freertos/semphr.h"
#include "freertos/task.h"

// Waveshare's S3_ePaper_1_54 codec_board record: one ES8311, one analog
// microphone. Both directions use the same 16 kHz clock pair and MCLK.
#define AUDIO_POWER 42  // active low
#define AUDIO_PA    46  // active high; controlled here, not by the codec
#define SETUP_BUTTON 18 // hold PWR to leave BOOT available for setup
#define AUDIO_CHUNK 320

static const char *TAG = "link.voice_board";
static SemaphoreHandle_t s_lock;
static i2s_chan_handle_t s_tx, s_rx;
static const audio_codec_ctrl_if_t *s_ctrl;
static const audio_codec_data_if_t *s_data;
static const audio_codec_if_t *s_codec;
static esp_codec_dev_handle_t s_dev;
static bool s_recording, s_playing;
static bool s_tx_logged;
static int s_volume = 60;
// Every use and conversion is under s_lock, including microphone unpacking.
static int16_t s_stereo[AUDIO_CHUNK * 2];

// ---- Audio lifetime (host-tested) ----
static void power_down(void) {
    gpio_set_level(AUDIO_PA, 0);
    if (s_dev) {
        esp_codec_dev_close(s_dev);
        esp_codec_dev_delete(s_dev);
        s_dev = NULL;
    }
    // open() can fail partway through enabling the codec and I2S.
    // Close both data directions explicitly, including that partial failure.
    if (s_data) s_data->enable(s_data, ESP_CODEC_DEV_TYPE_IN_OUT, false);
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
    if (!s_dev || esp_codec_dev_open(s_dev, &fs) != ESP_CODEC_DEV_OK) goto fail;
    if (esp_codec_dev_set_in_gain(s_dev, 30.0f) != ESP_CODEC_DEV_OK ||
        esp_codec_dev_set_out_vol(s_dev, s_volume) != ESP_CODEC_DEV_OK) goto fail;
    return ESP_OK;
fail:
    power_down();
    return ESP_FAIL;
}

esp_err_t voice_board_mic_start(void) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = power_up();
    if (err == ESP_OK) s_recording = true;
    xSemaphoreGive(s_lock);
    return err;
}

void voice_board_mic_stop(void) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_recording = false;
    if (!s_playing) power_down();
    xSemaphoreGive(s_lock);
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
    for (size_t i = 0; i < n; i++) {
        pcm[i] = stereo[2 * i]; // ES8311 ADC's left slot, not a second mic
        int value = pcm[i];
        if (value < 0) value = -value;
        if (value > *peak) *peak = value;
    }
    xSemaphoreGive(s_lock);
    return n;
}

static void speaker_state(void) {
    const int regs[] = {0x09, 0x12, 0x31, 0x32};
    int values[4] = {-1, -1, -1, -1};
    unsigned errors = 0;
    for (int i = 0; i < 4; i++) {
        if (esp_codec_dev_read_reg(s_dev, regs[i], &values[i]) != ESP_CODEC_DEV_OK) errors |= 1U << i;
    }
    ESP_LOGI(TAG, "speaker: rail=%d PA=%d volume=%d SDP=%02x DAC=%02x mute=%02x gain=%02x read_errors=%x",
             gpio_get_level(AUDIO_POWER), gpio_get_level(AUDIO_PA), s_volume,
             values[0], values[1], values[2], values[3], errors);
}

void voice_board_amp(bool on) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (on) {
        bool was_playing = s_playing;
        s_playing = power_up() == ESP_OK;
        gpio_set_level(AUDIO_PA, s_playing ? 1 : 0);
        if (s_playing && !was_playing) {
            // NS4150B startup is typically 120 ms. Queue no tone before it wakes.
            vTaskDelay(pdMS_TO_TICKS(150));
            s_tx_logged = false;
            speaker_state();
        }
    } else {
        gpio_set_level(AUDIO_PA, 0);
        s_playing = false;
        if (!s_recording) power_down();
    }
    xSemaphoreGive(s_lock);
}

esp_err_t voice_board_speaker_write(const int32_t *frames, size_t count) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool playing = s_playing;
    xSemaphoreGive(s_lock);
    if (!playing) {
        bool audible = false;
        for (size_t i = 0; i < count * 2; i++) audible |= frames[i] / 65536 != 0;
        if (!audible) {
            // Text replies supply paced zero PCM; preserve timing without powering the codec.
            vTaskDelay(pdMS_TO_TICKS(count * 1000 / VOICE_MIC_RATE));
            return ESP_OK;
        }
        voice_board_amp(true);
    }
    int16_t *stereo = s_stereo;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = s_playing ? ESP_OK : ESP_ERR_INVALID_STATE;
    while (count && err == ESP_OK) {
        size_t n = count > AUDIO_CHUNK ? AUDIO_CHUNK : count;
        int peak = 0;
        for (size_t i = 0; i < n * 2; i++) {
            stereo[i] = frames[i] / 65536;
            int value = stereo[i];
            if (value < 0) value = -value;
            if (value > peak) peak = value;
        }
        const uint8_t *p = (const uint8_t *)stereo;
        size_t left = n * sizeof(int16_t) * 2;
        while (left && err == ESP_OK) {
            size_t sent = 0;
            err = i2s_channel_write(s_tx, p, left, &sent, 200);
            if (err == ESP_OK && !sent) err = ESP_ERR_TIMEOUT;
            p += sent;
            left -= sent;
        }
        if (!s_tx_logged) {
            ESP_LOGI(TAG, "speaker TX: frames=%u peak=%d result=%s", (unsigned)n, peak, esp_err_to_name(err));
            s_tx_logged = true;
        }
        frames += n * 2;
        count -= n;
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

bool voice_board_muted(void) {
    return gpio_get_level(SETUP_BUTTON) == 0;
}

int voice_board_dial_steps(void) { return 0; }
// ---- End audio lifetime ----

void voice_board_log_idle(void) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    i2s_chan_info_t rx = {0}, tx = {0};
    esp_err_t rx_err = i2s_channel_get_info(s_rx, &rx);
    esp_err_t tx_err = i2s_channel_get_info(s_tx, &tx);
    ESP_LOGI(TAG, "audio idle: rail=%d PA=%d codec_closed=%d RX_enabled=%d TX_enabled=%d RX_query=%s TX_query=%s",
             gpio_get_level(AUDIO_POWER), gpio_get_level(AUDIO_PA), s_dev == NULL,
             rx.is_enabled, tx.is_enabled, esp_err_to_name(rx_err), esp_err_to_name(tx_err));
    xSemaphoreGive(s_lock);
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
    if (err == ESP_OK) ESP_LOGI(TAG, "ES8311 ready: 16000 Hz duplex; PWR+BOOT=setup; audio rail off");
    return err;
}

// A local cue only: no network turn and no microphone data is sent by this.
static esp_err_t cue(bool end, bool button) {
    static const int16_t wave[16] = {
        0, 383, 707, 924, 1000, 924, 707, 383,
        0, -383, -707, -924, -1000, -924, -707, -383,
    };
    int32_t stereo[160 * 2];
    voice_board_amp(true);
    esp_err_t err = ESP_OK;
    for (size_t chunk = 0; chunk < 5 && err == ESP_OK; chunk++) {
        if (button && gpio_get_level(CONFIG_HOMEHUB_BUTTON_GPIO) == 0) break;
        for (size_t i = 0; i < 160; i++) {
            size_t sample = chunk * 160 + i;
            // 1 kHz start, 500 Hz finish; 5 ms fades reduce the edges.
            int gain = sample < 80 ? sample : sample >= 720 ? 799 - sample : 80;
            int32_t value = wave[(end ? sample / 2 : sample) % 16] * 8192 * gain / 80000;
            stereo[2 * i] = stereo[2 * i + 1] = value * 65536;
        }
        err = voice_board_speaker_write(stereo, 160);
    }
    int64_t tail_until = esp_timer_get_time() + 120000;
    if (!end && err == ESP_OK) {
        // The mic is already powered. Drain its settling samples during the
        // cue tail, not after powering it up again once the user starts talking.
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (s_recording) {
            for (int i = 0; i < 4; i++) {
                size_t got = 0;
                esp_err_t read = i2s_channel_read(s_rx, s_stereo, sizeof(s_stereo), &got, 100);
                if (read != ESP_OK && read != ESP_ERR_TIMEOUT) { err = read; break; }
            }
        }
        xSemaphoreGive(s_lock);
    }
    int64_t left = tail_until - esp_timer_get_time();
    if (left > 0) vTaskDelay(pdMS_TO_TICKS((left + 999) / 1000));
    voice_board_amp(false);
    return err;
}

esp_err_t voice_board_cue(bool end) {
    return cue(end, false);
}

void voice_board_button_cue(bool long_hold) {
    if (gpio_get_level(CONFIG_HOMEHUB_BUTTON_GPIO) != 0) cue(long_hold, true);
}
