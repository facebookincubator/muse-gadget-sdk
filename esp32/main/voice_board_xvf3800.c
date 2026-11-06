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

/* Experimental reSpeaker XVF3800 with pre-soldered XIAO ESP32-S3.
 * Requires same-major 16 kHz, two-channel I2S-slave XMOS firmware >= 1.0.9.
 * The vendor firmware retains ownership of XMOS reset and audio routing.
 * https://github.com/respeaker/reSpeaker_XVF3800_USB_4MIC_ARRAY
 */
#include "voice_board.h"

#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#ifndef CONFIG_HOMEHUB_VOICE_BOARD_RESPEAKER_XVF3800
#error "voice_board_xvf3800.c requires CONFIG_HOMEHUB_VOICE_BOARD_RESPEAKER_XVF3800"
#endif

#define XVF_I2C_SDA 5
#define XVF_I2C_SCL 6
#define XVF_I2S_BCLK 8
#define XVF_I2S_WS 7
#define XVF_I2S_DOUT 44
#define XVF_I2S_DIN 43
#define XVF_XMOS_ADDR 0x2c
#define XVF_CODEC_ADDR 0x18
#define XVF_VERSION_RESOURCE 48
#define XVF_VERSION_COMMAND 0
#define XVF_GPO_RESOURCE 20
#define XVF_GPO_READ_COMMAND 0
#define XVF_GPO_WRITE_COMMAND 1
#define XVF_AMP_PIN 31
#define XVF_CONTROL_ATTEMPTS 5
#define XVF_CONTROL_TIMEOUT_MS 100
#define XVF_CONTROL_RETRY_MS 10
#define XVF_BOOT_WAIT_MS 3000
#define XVF_BOOT_RETRY_MS 250
#define XVF_MIC_CHUNK_FRAMES 320
#define XVF_PLAYBACK_CHUNK_FRAMES 32
#define XVF_MIN_VERSION_MAJOR 1
#define XVF_MIN_VERSION_MINOR 0
#define XVF_MIN_VERSION_PATCH 9

static const char *TAG = "link.respeaker";
static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_xmos;
static i2c_master_dev_handle_t s_codec;
static SemaphoreHandle_t s_lock;
static i2s_chan_handle_t s_rx;
static i2s_chan_handle_t s_tx;
static int32_t s_raw[XVF_MIC_CHUNK_FRAMES * 2];
static bool s_mic_on;
static bool s_ready;

static esp_err_t xmos_read(uint8_t resource, uint8_t command, uint8_t *out, size_t len) {
    if (!s_xmos || !s_lock) return ESP_ERR_INVALID_STATE;
    if (len > 5) return ESP_ERR_INVALID_SIZE;
    uint8_t req[] = {resource, (uint8_t)(command | 0x80), (uint8_t)(len + 1)};
    uint8_t resp[6];
    esp_err_t err = ESP_ERR_TIMEOUT;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int attempt = 0; attempt < XVF_CONTROL_ATTEMPTS; attempt++) {
        err = i2c_master_transmit(s_xmos, req, sizeof(req), XVF_CONTROL_TIMEOUT_MS);
        if (err == ESP_OK) {
            err = i2c_master_receive(s_xmos, resp, len + 1, XVF_CONTROL_TIMEOUT_MS);
        }
        if (err == ESP_OK) {
            if (resp[0] == 0) {
                for (size_t i = 0; i < len; i++) out[i] = resp[i + 1];
                break;
            }
            if (resp[0] != 64) {
                err = ESP_ERR_INVALID_RESPONSE;
                break;
            }
            err = ESP_ERR_TIMEOUT;
        }
        if (attempt + 1 < XVF_CONTROL_ATTEMPTS) {
            vTaskDelay(pdMS_TO_TICKS(XVF_CONTROL_RETRY_MS));
        }
    }
    xSemaphoreGive(s_lock);
    return err;
}

static esp_err_t xmos_write(uint8_t resource, uint8_t command, const uint8_t *data, size_t len) {
    if (!s_xmos || !s_lock) return ESP_ERR_INVALID_STATE;
    if (len > 5) return ESP_ERR_INVALID_SIZE;
    uint8_t req[8] = {resource, command, (uint8_t)len};
    for (size_t i = 0; i < len; i++) req[i + 3] = data[i];
    esp_err_t err = ESP_ERR_TIMEOUT;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int attempt = 0; attempt < XVF_CONTROL_ATTEMPTS; attempt++) {
        err = i2c_master_transmit(s_xmos, req, len + 3, XVF_CONTROL_TIMEOUT_MS);
        if (err == ESP_OK) break;
        if (attempt + 1 < XVF_CONTROL_ATTEMPTS) {
            vTaskDelay(pdMS_TO_TICKS(XVF_CONTROL_RETRY_MS));
        }
    }
    xSemaphoreGive(s_lock);
    return err;
}

static esp_err_t amp_set(bool on) {
    uint8_t payload[] = {XVF_AMP_PIN, on ? 0 : 1};
    return xmos_write(XVF_GPO_RESOURCE, XVF_GPO_WRITE_COMMAND, payload, sizeof(payload));
}

void voice_board_amp(bool on) {
    if (!s_ready) return;
    esp_err_t err = amp_set(on);
    if (err != ESP_OK) ESP_LOGW(TAG, "amplifier: %s", esp_err_to_name(err));
}

void voice_board_set_volume(int percent) {
    if (!s_ready || !s_codec) return;
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    uint8_t level = percent == 0 ? 0x80 : (uint8_t)(127 - percent * 127 / 100);
    uint8_t page[] = {0x00, 0x00};
    uint8_t left[] = {0x2b, level};
    uint8_t right[] = {0x2c, level};
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = i2c_master_transmit(s_codec, page, sizeof(page), XVF_CONTROL_TIMEOUT_MS);
    if (err == ESP_OK) {
        err = i2c_master_transmit(s_codec, left, sizeof(left), XVF_CONTROL_TIMEOUT_MS);
    }
    if (err == ESP_OK) {
        err = i2c_master_transmit(s_codec, right, sizeof(right), XVF_CONTROL_TIMEOUT_MS);
    }
    xSemaphoreGive(s_lock);
    if (err != ESP_OK) ESP_LOGW(TAG, "volume: %s", esp_err_to_name(err));
}

bool voice_board_muted(void) {
    if (!s_ready) return true;
    uint8_t values[5];
    return xmos_read(XVF_GPO_RESOURCE, XVF_GPO_READ_COMMAND, values, sizeof(values)) != ESP_OK ||
           values[1] != 0;
}

bool voice_board_echo_cancelled(void) {
    return false;
}

int voice_board_dial_steps(void) {
    return 0;
}

esp_err_t voice_board_mic_start(void) {
    if (!s_ready || !s_rx) return ESP_ERR_INVALID_STATE;
    if (s_mic_on) return ESP_OK;
    esp_err_t err = i2s_channel_enable(s_rx);
    if (err == ESP_OK) s_mic_on = true;
    return err;
}

void voice_board_mic_stop(void) {
    if (!s_mic_on) return;
    i2s_channel_disable(s_rx);
    s_mic_on = false;
}

size_t voice_board_mic_read(int16_t *pcm, size_t frames, int *peak) {
    *peak = 0;
    if (!s_ready || !s_mic_on || frames == 0) return 0;
    if (frames > XVF_MIC_CHUNK_FRAMES) frames = XVF_MIC_CHUNK_FRAMES;
    size_t bytes = 0;
    esp_err_t err = i2s_channel_read(s_rx, s_raw, frames * 2 * sizeof(int32_t), &bytes, 500);
    if (err != ESP_OK) return 0;
    size_t count = bytes / (2 * sizeof(int32_t));
    if (voice_board_muted()) return 0;
    for (size_t i = 0; i < count; i++) {
        /* The XVF3800's processed ASR stream is the right slot's high 16 bits. */
        int16_t sample = (int16_t)(s_raw[2 * i + 1] >> 16);
        pcm[i] = sample;
        int magnitude = sample == INT16_MIN ? 32768 : sample < 0 ? -sample : sample;
        if (magnitude > *peak) *peak = magnitude;
    }
    return count;
}

esp_err_t voice_board_speaker_write(const int32_t *frames, size_t count) {
    if (!s_ready || !s_tx) return ESP_ERR_INVALID_STATE;
    if (count % 3) return ESP_ERR_INVALID_SIZE;
    int32_t out[XVF_PLAYBACK_CHUNK_FRAMES * 2];
    while (count) {
        size_t n = count / 3;
        if (n > XVF_PLAYBACK_CHUNK_FRAMES) n = XVF_PLAYBACK_CHUNK_FRAMES;
        for (size_t i = 0; i < n; i++) {
            for (size_t channel = 0; channel < 2; channel++) {
                int64_t sum = (int64_t)frames[6 * i + channel] +
                              frames[6 * i + 2 + channel] + frames[6 * i + 4 + channel];
                out[2 * i + channel] = (int32_t)(sum / 3);
            }
        }
        size_t bytes = n * 2 * sizeof(int32_t);
        size_t written = 0;
        esp_err_t err = i2s_channel_write(s_tx, out, bytes, &written, 1000);
        if (err != ESP_OK) return err;
        if (written != bytes) return ESP_ERR_TIMEOUT;
        frames += 6 * n;
        count -= 3 * n;
    }
    return ESP_OK;
}

static bool version_supported(const uint8_t version[3]) {
    if (version[0] != XVF_MIN_VERSION_MAJOR) return false;
    if (version[1] != XVF_MIN_VERSION_MINOR) {
        return version[1] > XVF_MIN_VERSION_MINOR;
    }
    return version[2] >= XVF_MIN_VERSION_PATCH;
}

esp_err_t voice_board_init(void) {
    s_ready = false;
    s_mic_on = false;
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;
    i2c_master_bus_config_t bus = {
        .i2c_port = 0,
        .sda_io_num = XVF_I2C_SDA,
        .scl_io_num = XVF_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&bus, &s_bus);
    i2c_device_config_t device = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = XVF_XMOS_ADDR,
        .scl_speed_hz = 100000,
    };
    if (err == ESP_OK) err = i2c_master_bus_add_device(s_bus, &device, &s_xmos);
    device.device_address = XVF_CODEC_ADDR;
    if (err == ESP_OK) err = i2c_master_bus_add_device(s_bus, &device, &s_codec);
    if (err != ESP_OK) return err;

    /* The XMOS boots from its own flash at power-up and may not answer (or may
     * NACK) yet, so keep asking for its version for a few seconds. */
    uint8_t version[3];
    for (int waited = 0;; waited += XVF_BOOT_RETRY_MS) {
        err = xmos_read(XVF_VERSION_RESOURCE, XVF_VERSION_COMMAND, version, sizeof(version));
        if (err == ESP_OK || waited >= XVF_BOOT_WAIT_MS) break;
        vTaskDelay(pdMS_TO_TICKS(XVF_BOOT_RETRY_MS));
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "reSpeaker XVF3800 VERSION: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "reSpeaker XVF3800 XMOS firmware %u.%u.%u",
             version[0], version[1], version[2]);
    if (!version_supported(version)) {
        ESP_LOGE(TAG, "requires same-major firmware >= %u.%u.%u",
                 XVF_MIN_VERSION_MAJOR, XVF_MIN_VERSION_MINOR, XVF_MIN_VERSION_PATCH);
        return ESP_ERR_INVALID_RESPONSE;
    }
    err = amp_set(false);
    if (err != ESP_OK) return err;

    i2s_chan_config_t channel = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    channel.dma_desc_num = 6;
    channel.dma_frame_num = XVF_MIC_CHUNK_FRAMES;
    channel.auto_clear = true;
    err = i2s_new_channel(&channel, &s_tx, &s_rx);
    i2s_std_config_t config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(VOICE_MIC_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = XVF_I2S_BCLK,
            .ws = XVF_I2S_WS,
            .dout = XVF_I2S_DOUT,
            .din = XVF_I2S_DIN,
        },
    };
    if (err == ESP_OK) err = i2s_channel_init_std_mode(s_tx, &config);
    if (err == ESP_OK) err = i2s_channel_init_std_mode(s_rx, &config);
    if (err == ESP_OK) err = i2s_channel_enable(s_tx);
    if (err != ESP_OK) return err;
    s_ready = true;
    ESP_LOGI(TAG, "reSpeaker XVF3800 audio ready; XIAO BOOT is push-to-talk");
    return ESP_OK;
}
