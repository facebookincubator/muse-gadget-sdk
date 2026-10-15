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

#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/pulse_cnt.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "link.voice_board";

#define PIN_I2C_SDA       5
#define PIN_I2C_SCL       6
#define PIN_XMOS_RESET    4   // active high
#define PIN_MIC_BCLK      13
#define PIN_MIC_WS        14
#define PIN_MIC_DIN       15
#define PIN_SPK_BCLK      8
#define PIN_SPK_WS        7
#define PIN_SPK_DOUT      10
#define PIN_AMP_ENABLE    47
#define PIN_MUTE_SWITCH   3   // high when muted
#define PIN_DIAL_A        16
#define PIN_DIAL_B        18

#define XMOS_ADDR         0x42
#define AIC3204_ADDR      0x18

// Microphone DMA: 6 x 30 ms. The reader never waits on the network, and
// internal RAM is scarce (pairing needs an 8 KB block).
#define MIC_DMA_DESC      6
#define MIC_DMA_FRAMES    480
// Speaker DMA: 6 x ~10.6 ms (511 frames is the most per descriptor).
#define SPK_DMA_DESC      6
#define SPK_DMA_FRAMES    511
// Microphone frames read from DMA per call (both channels, 32-bit): 20 ms.
#define MIC_CHUNK_FRAMES  320

#define DEFAULT_VOLUME    60
// Quadrature counts per detent of the dial.
#define DIAL_COUNTS       4
// Recentre the counter well before its limits.
#define DIAL_LIMIT        1000
// The hardware counter wraps to zero at its limits. Keep them far past the
// recentre point: voice.c polls every 20 ms, and a quick turn moves a whole
// detent between two polls, which would skip a limit only one detent away.
#define DIAL_HW_LIMIT     INT16_MAX

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_xmos;
static i2c_master_dev_handle_t s_codec;
// The dial and voice.configure both set the volume: a page select plus writes.
static SemaphoreHandle_t s_codec_lock;
static pcnt_unit_handle_t s_dial;
static int s_dial_base;
static i2s_chan_handle_t s_mic;
static i2s_chan_handle_t s_spk;
static int32_t *s_mic_raw;

// ---- XMOS ----------------------------------------------------------------

// The XMOS control protocol: write {resource, command | 0x80 to read, length},
// then read a status byte and the payload.
static esp_err_t xmos_read(uint8_t resource, uint8_t command, uint8_t *out, size_t len) {
    uint8_t req[3] = {resource, (uint8_t)(command | 0x80), (uint8_t)(len + 1)};
    uint8_t resp[8] = {0};
    if (len + 1 > sizeof(resp)) return ESP_ERR_INVALID_SIZE;
    esp_err_t err = i2c_master_transmit_receive(s_xmos, req, sizeof(req), resp, len + 1, 100);
    if (err != ESP_OK) return err;
    if (resp[0] != 0) return ESP_ERR_INVALID_RESPONSE;
    memcpy(out, resp + 1, len);
    return ESP_OK;
}

static void xmos_init(void) {
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << PIN_XMOS_RESET,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&cfg);
    gpio_set_level(PIN_XMOS_RESET, 1);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(PIN_XMOS_RESET, 0);
    // It boots from its own flash, which takes over a second.
    vTaskDelay(pdMS_TO_TICKS(1500));

    uint8_t version[3];
    uint8_t stages[2];
    if (xmos_read(240, 88, version, sizeof(version)) == ESP_OK
        && xmos_read(241, 0x30, &stages[0], 1) == ESP_OK
        && xmos_read(241, 0x40, &stages[1], 1) == ESP_OK) {
        // Stages: 0 none, 1 AEC, 2 IC, 3 NS, 4 AGC.
        ESP_LOGI(TAG, "XMOS firmware %u.%u.%u, output stages %u/%u",
                 version[0], version[1], version[2], stages[0], stages[1]);
    } else {
        ESP_LOGW(TAG, "XMOS not answering; the microphones may not work");
    }
}

// ---- AIC3204 codec ------------------------------------------------------------

static esp_err_t codec_write(uint8_t reg, uint8_t val) {
    uint8_t buf[2] = {reg, val};
    esp_err_t err = i2c_master_transmit(s_codec, buf, sizeof(buf), 100);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "codec reg 0x%02x = 0x%02x: %s", reg, val, esp_err_to_name(err));
    }
    return err;
}

typedef struct {
    uint8_t reg, val;
} codec_reg_t;

// DAC from the XMOS's I2S at 48 kHz (32-bit words) to the line and
// headphone outputs, following the vendor firmware.
static const codec_reg_t s_codec_init[] = {
    {0x00, 0x00},  // page 0
    {0x0B, 0x82},  // NDAC on, /2
    {0x0C, 0x82},  // MDAC on, /2
    {0x0E, 0x80},  // DOSR 128
    {0x1B, 0x30},  // I2S, 32-bit words
    {0x38, 0x02},  // SCLK/MFP3 is the data input on this board
    {0x1F, 0x01},
    {0x20, 0x01},
    {0x3C, 0x01},  // DAC processing block PRB_P1
    {0x00, 0x01},  // page 1
    {0x02, 0x09},  // internal AVDD LDO on
    {0x01, 0x08},  // weak AVDD off now the LDO is up
    {0x02, 0x01},  // analog power on
    {0x0A, 0x40},  // common mode
    {0x03, 0x00},  // left DAC PowerTune, class AB
    {0x04, 0x00},  // right DAC the same
    {0x7B, 0x01},  // reference charges in 40 ms
    {0x14, 0x25},  // headphone soft start
    {0x0C, 0x08},  // DAC to HPL, HPR, LOL, LOR
    {0x0D, 0x08},
    {0x0E, 0x08},
    {0x0F, 0x08},
    {0x10, 0x3e},  // HP gain -2 dB, unmuted
    {0x11, 0x3e},
    {0x12, 0x00},  // line out 0 dB, unmuted
    {0x13, 0x00},
    {0x09, 0x3C},  // power up HP and line outputs
};

static esp_err_t codec_init(void) {
    esp_err_t err = codec_write(0x00, 0x00);
    if (err == ESP_OK) err = codec_write(0x01, 0x01);  // software reset
    if (err != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(10));
    for (size_t i = 0; i < sizeof(s_codec_init) / sizeof(s_codec_init[0]); i++) {
        err = codec_write(s_codec_init[i].reg, s_codec_init[i].val);
        if (err != ESP_OK) return err;
    }
    // Let the outputs ramp up before turning the DACs on.
    vTaskDelay(pdMS_TO_TICKS(2500));
    err = codec_write(0x00, 0x00);
    if (err == ESP_OK) err = codec_write(0x3F, 0xd4);  // both DACs on
    return err;
}

void voice_board_set_volume(int percent) {
    if (!s_codec) return;
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    // Digital volume in half-dB steps, -63.5 dB to +24 dB.
    int8_t level = (int8_t)(-127 + percent * 175 / 100);
    xSemaphoreTake(s_codec_lock, portMAX_DELAY);
    codec_write(0x00, 0x00);
    codec_write(0x41, (uint8_t)level);
    codec_write(0x42, (uint8_t)level);
    codec_write(0x40, percent == 0 ? 0x0c : 0x00);
    xSemaphoreGive(s_codec_lock);
}

// ---- Dial -----------------------------------------------------------------------

// The pulse counter decodes the dial's quadrature signal in hardware, counting
// every edge of both phases: up clockwise, down anticlockwise.
static esp_err_t dial_init(void) {
    pcnt_unit_config_t unit_cfg = {
        .low_limit = -DIAL_HW_LIMIT,
        .high_limit = DIAL_HW_LIMIT,
    };
    pcnt_unit_handle_t unit;
    esp_err_t err = pcnt_new_unit(&unit_cfg, &unit);
    if (err != ESP_OK) return err;
    pcnt_glitch_filter_config_t filter = {.max_glitch_ns = 1000};
    pcnt_chan_config_t a_cfg = {.edge_gpio_num = PIN_DIAL_A, .level_gpio_num = PIN_DIAL_B};
    pcnt_chan_config_t b_cfg = {.edge_gpio_num = PIN_DIAL_B, .level_gpio_num = PIN_DIAL_A};
    pcnt_channel_handle_t a, b;
    err = pcnt_unit_set_glitch_filter(unit, &filter);
    if (err == ESP_OK) err = pcnt_new_channel(unit, &a_cfg, &a);
    if (err == ESP_OK) err = pcnt_new_channel(unit, &b_cfg, &b);
    if (err == ESP_OK) {
        pcnt_channel_set_edge_action(a, PCNT_CHANNEL_EDGE_ACTION_DECREASE,
                                     PCNT_CHANNEL_EDGE_ACTION_INCREASE);
        pcnt_channel_set_level_action(a, PCNT_CHANNEL_LEVEL_ACTION_KEEP,
                                      PCNT_CHANNEL_LEVEL_ACTION_INVERSE);
        pcnt_channel_set_edge_action(b, PCNT_CHANNEL_EDGE_ACTION_INCREASE,
                                     PCNT_CHANNEL_EDGE_ACTION_DECREASE);
        pcnt_channel_set_level_action(b, PCNT_CHANNEL_LEVEL_ACTION_KEEP,
                                      PCNT_CHANNEL_LEVEL_ACTION_INVERSE);
        gpio_pullup_en(PIN_DIAL_A);
        gpio_pullup_en(PIN_DIAL_B);
        err = pcnt_unit_enable(unit);
    }
    if (err == ESP_OK) err = pcnt_unit_clear_count(unit);
    if (err == ESP_OK) err = pcnt_unit_start(unit);
    if (err == ESP_OK) s_dial = unit;
    return err;
}

int voice_board_dial_steps(void) {
    int count;
    if (!s_dial || pcnt_unit_get_count(s_dial, &count) != ESP_OK) return 0;
    // Whole detents only; a half-turned one stays in the count.
    int steps = (count - s_dial_base) / DIAL_COUNTS;
    s_dial_base += steps * DIAL_COUNTS;
    if (count > DIAL_LIMIT || count < -DIAL_LIMIT) {
        pcnt_unit_clear_count(s_dial);
        s_dial_base -= count;
    }
    return steps;
}

// ---- I2S ------------------------------------------------------------------------

static esp_err_t i2s_init(void) {
    i2s_chan_config_t mic_chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_SLAVE);
    mic_chan.dma_desc_num = MIC_DMA_DESC;
    mic_chan.dma_frame_num = MIC_DMA_FRAMES;
    i2s_std_config_t mic_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(VOICE_MIC_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = PIN_MIC_BCLK,
            .ws = PIN_MIC_WS,
            .dout = I2S_GPIO_UNUSED,
            .din = PIN_MIC_DIN,
        },
    };
    esp_err_t err = i2s_new_channel(&mic_chan, NULL, &s_mic);
    if (err == ESP_OK) err = i2s_channel_init_std_mode(s_mic, &mic_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "microphone I2S: %s", esp_err_to_name(err));
        return err;
    }

    i2s_chan_config_t spk_chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_SLAVE);
    spk_chan.dma_desc_num = SPK_DMA_DESC;
    spk_chan.dma_frame_num = SPK_DMA_FRAMES;
    // Silence whenever nothing is queued.
    spk_chan.auto_clear = true;
    i2s_std_config_t spk_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(VOICE_SPEAKER_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = PIN_SPK_BCLK,
            .ws = PIN_SPK_WS,
            .dout = PIN_SPK_DOUT,
            .din = I2S_GPIO_UNUSED,
        },
    };
    err = i2s_new_channel(&spk_chan, &s_spk, NULL);
    if (err == ESP_OK) err = i2s_channel_init_std_mode(s_spk, &spk_cfg);
    if (err == ESP_OK) err = i2s_channel_enable(s_spk);
    if (err != ESP_OK) ESP_LOGE(TAG, "speaker I2S: %s", esp_err_to_name(err));
    return err;
}

esp_err_t voice_board_mic_start(void) {
    return s_mic ? i2s_channel_enable(s_mic) : ESP_ERR_INVALID_STATE;
}

void voice_board_mic_stop(void) {
    if (s_mic) i2s_channel_disable(s_mic);
}

size_t voice_board_mic_read(int16_t *pcm, size_t frames, int *peak) {
    *peak = 0;
    if (frames > MIC_CHUNK_FRAMES) frames = MIC_CHUNK_FRAMES;
    size_t bytes = 0;
    if (i2s_channel_read(s_mic, s_mic_raw, frames * 2 * sizeof(int32_t), &bytes,
                         pdMS_TO_TICKS(500)) != ESP_OK) {
        return 0;
    }
    size_t n = bytes / (2 * sizeof(int32_t));
    for (size_t i = 0; i < n; i++) {
        // Left is the channel the XMOS runs through AGC, meant for speech
        // recognition; right is the echo-cancelled one.
        int16_t s = (int16_t)(s_mic_raw[2 * i] >> 16);
        pcm[i] = s;
        int a = s < 0 ? -s : s;
        if (a > *peak) *peak = a;
    }
    return n;
}

esp_err_t voice_board_speaker_write(const int32_t *frames, size_t count) {
    size_t written = 0;
    return i2s_channel_write(s_spk, frames, count * 2 * sizeof(int32_t), &written,
                             pdMS_TO_TICKS(1000));
}

void voice_board_amp(bool on) {
    gpio_set_level(PIN_AMP_ENABLE, on);
}

bool voice_board_muted(void) {
    return gpio_get_level(PIN_MUTE_SWITCH) == 1;
}

esp_err_t voice_board_init(void) {
    gpio_config_t amp_cfg = {
        .pin_bit_mask = 1ULL << PIN_AMP_ENABLE,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&amp_cfg);
    voice_board_amp(false);
    gpio_config_t mute_cfg = {
        .pin_bit_mask = 1ULL << PIN_MUTE_SWITCH,
        .mode = GPIO_MODE_INPUT,
    };
    gpio_config(&mute_cfg);

    s_mic_raw = heap_caps_malloc(MIC_CHUNK_FRAMES * 2 * sizeof(int32_t), MALLOC_CAP_SPIRAM);
    s_codec_lock = xSemaphoreCreateMutex();
    if (!s_mic_raw || !s_codec_lock) return ESP_ERR_NO_MEM;
    esp_err_t dial_err = dial_init();
    if (dial_err != ESP_OK) ESP_LOGW(TAG, "dial: %s", esp_err_to_name(dial_err));

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = 0,
        .sda_io_num = PIN_I2C_SDA,
        .scl_io_num = PIN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_bus);
    i2c_device_config_t xmos_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = XMOS_ADDR,
        .scl_speed_hz = 400000,
    };
    i2c_device_config_t codec_cfg = xmos_cfg;
    codec_cfg.device_address = AIC3204_ADDR;
    if (err == ESP_OK) err = i2c_master_bus_add_device(s_bus, &xmos_cfg, &s_xmos);
    if (err == ESP_OK) err = i2c_master_bus_add_device(s_bus, &codec_cfg, &s_codec);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C: %s", esp_err_to_name(err));
        return err;
    }

    // The XMOS drives the I2S clocks, so it comes up first.
    xmos_init();
    err = i2s_init();
    if (err == ESP_OK) err = codec_init();
    if (err != ESP_OK) return err;
    voice_board_set_volume(DEFAULT_VOLUME);
    ESP_LOGI(TAG, "audio ready (mute switch %s)", voice_board_muted() ? "on" : "off");
    return ESP_OK;
}
