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


/* Experimental Seeed reSpeaker Lite port.
 * Hardware reference: https://github.com/respeaker/reSpeaker_Lite
 * I2S reference: https://wiki.seeedstudio.com/respeaker_record_and_play/
 * Requires the vendor's 16 kHz I2S XMOS firmware, 32-bit stereo slots.
 * XMOS supplies the clocks and initializes the AIC3204 itself.
 */
#include "voice_board.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define CHUNK 320
static const char *TAG = "link.respeaker";
static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_xmos, s_codec;
static SemaphoreHandle_t s_lock;
static i2s_chan_handle_t s_rx, s_tx;
static int32_t s_raw[CHUNK * 2];
static bool s_mic_on;

static esp_err_t xmos_read(uint8_t resource, uint8_t command, uint8_t *out, size_t len) {
    uint8_t req[] = {resource, (uint8_t)(command | 0x80), (uint8_t)(len + 1)};
    uint8_t resp[4];
    if (len > 3) return ESP_ERR_INVALID_SIZE;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    // Seeed's Wire.endTransmission() sends STOP before requestFrom(). XMOS
    // processes the command between these distinct transactions.
    esp_err_t err = i2c_master_transmit(s_xmos, req, sizeof(req), 100);
    if (err == ESP_OK) err = i2c_master_receive(s_xmos, resp, len + 1, 100);
    xSemaphoreGive(s_lock);
    if (err != ESP_OK) return err;
    if (resp[0]) return ESP_ERR_INVALID_RESPONSE;
    for (size_t i = 0; i < len; i++) out[i] = resp[i + 1];
    return ESP_OK;
}

void voice_board_set_volume(int percent) {
    if (!s_codec) return;
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    // Digital gain -63.5 dB to 0 dB. Never boost above unity.
    uint8_t page[] = {0, 0};
    uint8_t left[] = {0x41, (uint8_t)(int8_t)(-127 + percent * 127 / 100)};
    uint8_t right[] = {0x42, left[1]};
    uint8_t mute[] = {0x40, percent ? 0 : 0x0c};
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = i2c_master_transmit(s_codec, page, sizeof(page), 100);
    if (err == ESP_OK) err = i2c_master_transmit(s_codec, left, sizeof(left), 100);
    if (err == ESP_OK) err = i2c_master_transmit(s_codec, right, sizeof(right), 100);
    if (err == ESP_OK) err = i2c_master_transmit(s_codec, mute, sizeof(mute), 100);
    xSemaphoreGive(s_lock);
    if (err != ESP_OK) ESP_LOGW(TAG, "volume: %s", esp_err_to_name(err));
}

bool voice_board_muted(void) {
    uint8_t muted = 1;
    // Firmware >=1.0.9 exposes the hardware mute state. Fail closed if unreadable.
    return !s_xmos || xmos_read(0xf1, 0x01, &muted, 1) != ESP_OK || muted != 0;
}

int voice_board_dial_steps(void) { return 0; }
void voice_board_amp(bool on) {
    // XMOS owns amplifier enable and headphone detection; leave its routing intact.
    (void)on;
}

esp_err_t voice_board_mic_start(void) {
    if (!s_rx) return ESP_ERR_INVALID_STATE;
    esp_err_t err = i2s_channel_enable(s_rx);
    if (err == ESP_OK) s_mic_on = true;
    return err;
}
void voice_board_mic_stop(void) {
    if (s_mic_on) { i2s_channel_disable(s_rx); s_mic_on = false; }
}
size_t voice_board_mic_read(int16_t *pcm, size_t frames, int *peak) {
    *peak = 0;
    if (!s_mic_on) return 0;
    if (frames > CHUNK) frames = CHUNK;
    size_t bytes = 0;
    if (i2s_channel_read(s_rx, s_raw, frames * 2 * sizeof(int32_t), &bytes, 500) != ESP_OK) return 0;
    size_t count = bytes / (2 * sizeof(int32_t));
    for (size_t i = 0; i < count; i++) {
        pcm[i] = (int16_t)(s_raw[2 * i] >> 16);
        int magnitude = pcm[i] < 0 ? -(int)pcm[i] : pcm[i];
        if (magnitude > *peak) *peak = magnitude;
    }
    return count;
}

esp_err_t voice_board_speaker_write(const int32_t *frames, size_t count) {
    // Existing player produces 48 kHz. Average each three stereo frames to
    // the vendor's 16 kHz bus; player chunks are always a multiple of three.
    if (!s_tx) return ESP_ERR_INVALID_STATE;
    if (count % 3) return ESP_ERR_INVALID_SIZE;
    // Keep the player's 3 KB task stack free for I2S and logging.
    int32_t out[32 * 2];
    while (count) {
        size_t n = count / 3;
        if (n > 32) n = 32;
        for (size_t i = 0; i < n; i++) {
            for (size_t c = 0; c < 2; c++) {
                int64_t sum = (int64_t)frames[6*i+c] + frames[6*i+2+c] + frames[6*i+4+c];
                out[2*i+c] = (int32_t)(sum / 3);
            }
        }
        size_t written = 0;
        esp_err_t err = i2s_channel_write(s_tx, out, n * 2 * sizeof(int32_t), &written, 1000);
        if (err != ESP_OK) return err;
        if (written != n * 2 * sizeof(int32_t)) return ESP_ERR_TIMEOUT;
        frames += 6 * n;
        count -= 3 * n;
    }
    return ESP_OK;
}

esp_err_t voice_board_init(void) {
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;
    i2c_master_bus_config_t bus = {
        .i2c_port = 0, .sda_io_num = 5, .scl_io_num = 6,
        .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&bus, &s_bus);
    i2c_device_config_t dev = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = 0x42, .scl_speed_hz = 100000,
    };
    if (err == ESP_OK) err = i2c_master_bus_add_device(s_bus, &dev, &s_xmos);
    dev.device_address = 0x18;
    if (err == ESP_OK) err = i2c_master_bus_add_device(s_bus, &dev, &s_codec);
    if (err != ESP_OK) return err;
    gpio_config_t reset = {.pin_bit_mask = 1ULL << 2, .mode = GPIO_MODE_OUTPUT};
    err = gpio_config(&reset);
    if (err != ESP_OK) return err;
    gpio_set_level(2, 1);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(2, 0);
    vTaskDelay(pdMS_TO_TICKS(3000));
    uint8_t version[3];
    err = xmos_read(0xf0, 0x58, version, 3);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "XMOS not answering (%s): requires Seeed 16 kHz I2S firmware", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "XMOS firmware %u.%u.%u; expected 16 kHz I2S variant",
             version[0], version[1], version[2]);
    i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_SLAVE);
    chan.dma_desc_num = 6;
    chan.dma_frame_num = CHUNK;
    chan.auto_clear = true;
    err = i2s_new_channel(&chan, &s_tx, &s_rx);
    i2s_std_config_t cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(16000),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {.mclk = I2S_GPIO_UNUSED, .bclk = 8, .ws = 7, .dout = 43, .din = 44},
    };
    if (err == ESP_OK) err = i2s_channel_init_std_mode(s_tx, &cfg);
    if (err == ESP_OK) err = i2s_channel_init_std_mode(s_rx, &cfg);
    if (err == ESP_OK) err = i2s_channel_enable(s_tx);
    if (err != ESP_OK) return err;
    voice_board_set_volume(60);
    ESP_LOGI(TAG, "reSpeaker audio ready; XIAO BOOT is push-to-talk");
    return ESP_OK;
}
