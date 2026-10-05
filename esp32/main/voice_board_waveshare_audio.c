/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *     http://www.apache.org/licenses/LICENSE-2.0
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/* Waveshare ESP32-S3-AUDIO-Board, without the optional external display.
 * Pin map and bus format: official schematic v1.1 and factory_01 demo at
 * https://docs.waveshare.com/ESP32-S3-AUDIO-Board/Resources-And-Documents
 * ES7210 carries four PCM16 channels in two 32-bit slots: reference, mic,
 * unused, mic (RMNM). Select the microphone, not the amplifier reference.
 * ES8311 shares clocks: TX stays at 16 kHz while recording. Codec register
 * programming uses Espressif's Apache-2.0 esp_codec_dev driver.
 */
#include "voice_board.h"
#include "waveshare_audio_pcm.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_codec_dev_defaults.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define I2C_SDA             11
#define I2C_SCL             10
#define I2S_MCLK            12
#define I2S_BCLK            13
#define I2S_WS              14
#define I2S_DIN             15
#define I2S_DOUT            16
#define IO_ADDRESS          0x20
#define IO_INPUT_HIGH       0x01
#define IO_OUTPUT_HIGH      0x03
#define IO_CONFIG_HIGH      0x07
#define IO_AMP              (1U << 0) /* EXIO8: PA_CTRL */
#define IO_MUTE             (1U << 1) /* EXIO9: K1, active low */
#define IO_VOLUME_DOWN      (1U << 2) /* EXIO10: K2 */
#define IO_VOLUME_UP        (1U << 3) /* EXIO11: K3 */
#define I2C_TIMEOUT_MS      100
#define AUDIO_CHUNK         320
#define MIC_GAIN_DB         30.0f
#define VOLUME_MIN_DB       (-50.0f)

static const char *TAG = "link.waveshare_audio";
static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_io;
static i2s_chan_handle_t s_tx, s_rx;
static const audio_codec_ctrl_if_t *s_dac_ctrl, *s_adc_ctrl;
static const audio_codec_gpio_if_t *s_gpio;
static const audio_codec_if_t *s_dac, *s_adc;
static SemaphoreHandle_t s_lock;
static int32_t s_raw[AUDIO_CHUNK * 2];
static bool s_tx_on, s_mic_on;
static uint8_t s_keys = 0xff, s_candidate_keys = 0xff;

static esp_err_t io_read(uint8_t reg, uint8_t *value) {
    return i2c_master_transmit_receive(s_io, &reg, 1, value, 1, I2C_TIMEOUT_MS);
}
static esp_err_t io_update(uint8_t reg, uint8_t mask, uint8_t value) {
    uint8_t data[2] = {reg, 0};
    esp_err_t err = io_read(reg, &data[1]);
    if (err == ESP_OK) {
        data[1] = (data[1] & ~mask) | (value & mask);
        err = i2c_master_transmit(s_io, data, sizeof(data), I2C_TIMEOUT_MS);
    }
    return err;
}
void voice_board_amp(bool on) {
    if (!s_io || !s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = io_update(IO_OUTPUT_HIGH, IO_AMP, on ? IO_AMP : 0);
    xSemaphoreGive(s_lock);
    if (err != ESP_OK) ESP_LOGW(TAG, "amplifier: %s", esp_err_to_name(err));
}
static esp_err_t set_volume(int percent) {
    if (!s_dac || !s_lock) return ESP_ERR_INVALID_STATE;
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    float db = VOLUME_MIN_DB * (100 - percent) / 100.0f;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int ret = s_dac->set_vol(s_dac, db);
    if (ret == ESP_CODEC_DEV_OK) ret = s_dac->mute(s_dac, percent == 0);
    xSemaphoreGive(s_lock);
    return ret == ESP_CODEC_DEV_OK ? ESP_OK : ESP_FAIL;
}
void voice_board_set_volume(int percent) {
    esp_err_t err = set_volume(percent);
    if (err != ESP_OK) {
        voice_board_amp(false);
        ESP_LOGW(TAG, "codec volume: %s", esp_err_to_name(err));
    }
}
bool voice_board_muted(void) {
    if (!s_io || !s_lock) return true;
    uint8_t keys = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = io_read(IO_INPUT_HIGH, &keys);
    xSemaphoreGive(s_lock);
    // K1 is momentary software mute and restores BOOT's setup role.
    // Fail closed when the expander cannot be read.
    return err != ESP_OK || !(keys & IO_MUTE);
}
int voice_board_dial_steps(void) {
    if (!s_io || !s_lock) return 0;
    uint8_t keys;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = io_read(IO_INPUT_HIGH, &keys);
    xSemaphoreGive(s_lock);
    if (err != ESP_OK) return 0;
    // Two matching 20 ms polls debounce the mechanical volume keys.
    if (keys != s_candidate_keys) { s_candidate_keys = keys; return 0; }
    uint8_t pressed = s_keys & ~keys;
    s_keys = keys;
    return ((pressed & IO_VOLUME_UP) ? 1 : 0) - ((pressed & IO_VOLUME_DOWN) ? 1 : 0);
}
esp_err_t voice_board_mic_start(void) {
    if (!s_rx || !s_adc || s_mic_on || voice_board_muted()) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int ret = s_adc->enable(s_adc, true);
    xSemaphoreGive(s_lock);
    if (ret != ESP_CODEC_DEV_OK) return ESP_FAIL;
    esp_err_t err = i2s_channel_enable(s_rx);
    if (err == ESP_OK) s_mic_on = true;
    else {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        (void)s_adc->enable(s_adc, false);
        xSemaphoreGive(s_lock);
    }
    return err;
}
void voice_board_mic_stop(void) {
    if (!s_mic_on) return;
    (void)i2s_channel_disable(s_rx);
    s_mic_on = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int ret = s_adc->enable(s_adc, false);
    xSemaphoreGive(s_lock);
    if (ret != ESP_CODEC_DEV_OK) ESP_LOGW(TAG, "microphone stop failed (%d)", ret);
}
size_t voice_board_mic_read(int16_t *pcm, size_t frames, int *peak) {
    *peak = 0;
    if (!s_mic_on || voice_board_muted()) return 0;
    if (frames > AUDIO_CHUNK) frames = AUDIO_CHUNK;
    size_t bytes = 0;
    esp_err_t err = i2s_channel_read(s_rx, s_raw, frames * 2 * sizeof(int32_t), &bytes, 500);
    if (err != ESP_OK || bytes % (2 * sizeof(int32_t))) return 0;
    size_t count = bytes / (2 * sizeof(int32_t));
    waveshare_audio_extract_mic(s_raw, pcm, count, peak);
    return count;
}
esp_err_t voice_board_speaker_write(const int32_t *frames, size_t count) {
    if (!s_tx || !s_tx_on) return ESP_ERR_INVALID_STATE;
    if (count % 3) return ESP_ERR_INVALID_SIZE;
    int32_t out[32 * 2];
    while (count) {
        size_t n = count / 3;
        if (n > 32) n = 32;
        waveshare_audio_downsample(frames, out, n);
        size_t written = 0;
        esp_err_t err = i2s_channel_write(s_tx, out, n * 2 * sizeof(int32_t), &written, 1000);
        if (err != ESP_OK) return err;
        if (written != n * 2 * sizeof(int32_t)) return ESP_ERR_TIMEOUT;
        frames += 6 * n;
        count -= 3 * n;
    }
    return ESP_OK;
}
static void cleanup(void) {
    voice_board_mic_stop();
    voice_board_amp(false);
    if (s_adc) { audio_codec_delete_codec_if(s_adc); s_adc = NULL; }
    if (s_dac) { audio_codec_delete_codec_if(s_dac); s_dac = NULL; }
    if (s_adc_ctrl) { audio_codec_delete_ctrl_if(s_adc_ctrl); s_adc_ctrl = NULL; }
    if (s_dac_ctrl) { audio_codec_delete_ctrl_if(s_dac_ctrl); s_dac_ctrl = NULL; }
    if (s_gpio) { audio_codec_delete_gpio_if(s_gpio); s_gpio = NULL; }
    if (s_rx) { i2s_del_channel(s_rx); s_rx = NULL; }
    if (s_tx) {
        if (s_tx_on) i2s_channel_disable(s_tx);
        i2s_del_channel(s_tx); s_tx = NULL; s_tx_on = false;
    }
    if (s_io) { i2c_master_bus_rm_device(s_io); s_io = NULL; }
    if (s_bus) { i2c_del_master_bus(s_bus); s_bus = NULL; }
    if (s_lock) { vSemaphoreDelete(s_lock); s_lock = NULL; }
}
esp_err_t voice_board_init(void) {
    if (s_bus) return ESP_ERR_INVALID_STATE;
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;
    i2c_master_bus_config_t bus = {
        .i2c_port = 0, .sda_io_num = I2C_SDA, .scl_io_num = I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&bus, &s_bus);
    if (err != ESP_OK) goto fail;
    i2c_device_config_t io = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = IO_ADDRESS, .scl_speed_hz = 100000,
    };
    err = i2c_master_bus_add_device(s_bus, &io, &s_io);
    if (err != ESP_OK) goto fail;
    // Clear amp latch BEFORE making EXIO8 an output. Preserve unrelated pins.
    err = io_update(IO_OUTPUT_HIGH, IO_AMP, 0);
    if (err == ESP_OK) err = io_update(IO_CONFIG_HIGH, IO_AMP | IO_MUTE | IO_VOLUME_DOWN | IO_VOLUME_UP,
                                     IO_MUTE | IO_VOLUME_DOWN | IO_VOLUME_UP);
    if (err != ESP_OK) goto fail;
    i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    chan.dma_desc_num = 6;
    chan.dma_frame_num = AUDIO_CHUNK;
    chan.auto_clear = true;
    err = i2s_new_channel(&chan, &s_tx, &s_rx);
    if (err != ESP_OK) goto fail;
    i2s_std_config_t cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(VOICE_MIC_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {.mclk = I2S_MCLK, .bclk = I2S_BCLK, .ws = I2S_WS, .dout = I2S_DOUT, .din = I2S_DIN},
    };
    err = i2s_channel_init_std_mode(s_tx, &cfg);
    if (err == ESP_OK) err = i2s_channel_init_std_mode(s_rx, &cfg);
    if (err == ESP_OK) err = i2s_channel_enable(s_tx);
    if (err != ESP_OK) goto fail;
    s_tx_on = true;
    audio_codec_i2c_cfg_t ctrl = {.addr = ES8311_CODEC_DEFAULT_ADDR, .bus_handle = s_bus};
    s_dac_ctrl = audio_codec_new_i2c_ctrl(&ctrl);
    ctrl.addr = ES7210_CODEC_DEFAULT_ADDR;
    s_adc_ctrl = audio_codec_new_i2c_ctrl(&ctrl);
    s_gpio = audio_codec_new_gpio();
    if (!s_dac_ctrl || !s_adc_ctrl || !s_gpio) { err = ESP_ERR_NO_MEM; goto fail; }
    es8311_codec_cfg_t dac = {
        .ctrl_if = s_dac_ctrl, .gpio_if = s_gpio,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC, .pa_pin = -1,
        .use_mclk = false, // factory_01 uses BCLK as the ES8311 clock source.
    };
    s_dac = es8311_codec_new(&dac);
    es7210_codec_cfg_t adc = {
        .ctrl_if = s_adc_ctrl,
        .mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2 | ES7210_SEL_MIC3 | ES7210_SEL_MIC4,
    };
    s_adc = es7210_codec_new(&adc);
    if (!s_dac || !s_adc) { err = ESP_FAIL; goto fail; }
    esp_codec_dev_sample_info_t fs = {
        .sample_rate = VOICE_MIC_RATE, .channel = 2, .bits_per_sample = 32,
    };
    // Drive I2S directly: codec open/close must not reconfigure shared clocks.
    // ES7210's driver halves 32-bit slots to PCM16 in its four-channel mode.
    int ret = s_dac->set_fs(s_dac, &fs);
    if (ret == ESP_CODEC_DEV_OK) ret = s_dac->enable(s_dac, true);
    if (ret == ESP_CODEC_DEV_OK) ret = s_adc->set_fs(s_adc, &fs);
    if (ret == ESP_CODEC_DEV_OK) ret = s_adc->set_mic_gain(s_adc, MIC_GAIN_DB);
    if (ret == ESP_CODEC_DEV_OK) ret = s_adc->enable(s_adc, false);
    if (ret != ESP_CODEC_DEV_OK) { err = ESP_FAIL; goto fail; }
    err = set_volume(60);
    if (err != ESP_OK) goto fail;
    ESP_LOGI(TAG, "Waveshare Audio Board ready: ES8311 + ES7210, 16 kHz; BOOT talk, K1 mute/setup, K2/K3 volume");
    return ESP_OK;
fail:
    ESP_LOGE(TAG, "audio init: %s", esp_err_to_name(err));
    cleanup();
    return err;
}
