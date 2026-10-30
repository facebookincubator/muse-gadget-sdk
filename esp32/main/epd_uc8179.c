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

// Seeed reTerminal E1001: 7.5 inch 800x480 black and white e-paper,
// GDEY075T7 panel with a UC8179 controller on SPI2.
//
// Pins: Seeed's ESPHome cookbook for the reTerminal E series
// (wiki.seeedstudio.com/reterminal_e10xx_with_esphome). Controller commands
// and timings: GxEPD2's GxEPD2_750_GDEY075T7 (github.com/ZinggJM/GxEPD2).

#include "epd_panel.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "link.led";

#define EPD_HOST        SPI2_HOST
#define EPD_PIN_SCLK    7
#define EPD_PIN_MOSI    9
#define EPD_PIN_CS      10
#define EPD_PIN_DC      11
#define EPD_PIN_RST     12
#define EPD_PIN_BUSY    13
#define EPD_SPI_HZ      (10 * 1000 * 1000)
#define EPD_W           800
#define EPD_H           480
#define EPD_ROW_BYTES   (EPD_W / 8)
#define EPD_FRAME_BYTES (EPD_ROW_BYTES * EPD_H)
#define EPD_CHUNK_BYTES (EPD_ROW_BYTES * 40)
#define EPD_BUSY_TIMEOUT_MS 10000

static spi_device_handle_t s_spi;
static uint8_t *s_chunk;

static esp_err_t epd_write(bool data, const uint8_t *buf, size_t len) {
    gpio_set_level(EPD_PIN_DC, data);
    while (len) {
        size_t n = len < EPD_CHUNK_BYTES ? len : EPD_CHUNK_BYTES;
        memcpy(s_chunk, buf, n);
        spi_transaction_t t = {.length = n * 8, .tx_buffer = s_chunk};
        esp_err_t err = spi_device_polling_transmit(s_spi, &t);
        if (err != ESP_OK) return err;
        buf += n;
        len -= n;
    }
    return ESP_OK;
}

static esp_err_t epd_cmd(uint8_t cmd, const uint8_t *data, size_t len) {
    esp_err_t err = epd_write(false, &cmd, 1);
    if (err == ESP_OK && len) err = epd_write(true, data, len);
    return err;
}

static esp_err_t epd_wait_idle(void) {
    int64_t give_up = esp_timer_get_time() + EPD_BUSY_TIMEOUT_MS * 1000LL;
    while (gpio_get_level(EPD_PIN_BUSY) == 0) {
        if (esp_timer_get_time() > give_up) return ESP_ERR_TIMEOUT;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return ESP_OK;
}

typedef struct { uint8_t cmd, len; uint8_t data[5]; } init_cmd_t;

static const init_cmd_t s_init[] = {
    {0x00, 1, {0x1F}},
    {0x01, 5, {0x07, 0x07, 0x3F, 0x3F, 0x09}},
    {0x06, 4, {0x17, 0x17, 0x28, 0x17}},
    {0x61, 4, {EPD_W >> 8, EPD_W & 0xFF, EPD_H >> 8, EPD_H & 0xFF}},
    {0x15, 1, {0x00}},
    {0x50, 2, {0x29, 0x07}},
    {0x60, 1, {0x22}},
    {0xE3, 1, {0x22}},
    {0xE0, 1, {0x02}},
};

static esp_err_t uc8179_init(void) {
    const gpio_config_t out = {
        .pin_bit_mask = 1ULL << EPD_PIN_DC | 1ULL << EPD_PIN_RST,
        .mode = GPIO_MODE_OUTPUT,
    };
    const gpio_config_t busy = {
        .pin_bit_mask = 1ULL << EPD_PIN_BUSY,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    esp_err_t err = gpio_config(&out);
    if (err == ESP_OK) err = gpio_config(&busy);
    gpio_set_level(EPD_PIN_RST, 1);
    const spi_bus_config_t bus = {
        .sclk_io_num = EPD_PIN_SCLK,
        .mosi_io_num = EPD_PIN_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = EPD_CHUNK_BYTES,
    };
    const spi_device_interface_config_t dev = {
        .mode = 0,
        .clock_speed_hz = EPD_SPI_HZ,
        .spics_io_num = EPD_PIN_CS,
        .queue_size = 1,
    };
    s_chunk = heap_caps_malloc(EPD_CHUNK_BYTES, MALLOC_CAP_DMA);
    if (!s_chunk) return ESP_ERR_NO_MEM;
    if (err == ESP_OK) err = spi_bus_initialize(EPD_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err == ESP_OK) err = spi_bus_add_device(EPD_HOST, &dev, &s_spi);
    return err;
}

static esp_err_t uc8179_update(const uint8_t *frame, const uint8_t *prev, bool full) {
    gpio_set_level(EPD_PIN_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(EPD_PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
    esp_err_t err = epd_wait_idle();
    for (size_t i = 0; err == ESP_OK && i < sizeof(s_init) / sizeof(s_init[0]); i++) {
        err = epd_cmd(s_init[i].cmd, s_init[i].data, s_init[i].len);
    }
    uint8_t temp = full ? 0x5A : 0x6E;
    if (err == ESP_OK) err = epd_cmd(0xE5, &temp, 1);
    if (err == ESP_OK) err = epd_cmd(0x10, prev, EPD_FRAME_BYTES);
    if (err == ESP_OK) err = epd_cmd(0x13, frame, EPD_FRAME_BYTES);
    if (err == ESP_OK) err = epd_cmd(0x04, NULL, 0);
    if (err == ESP_OK) err = epd_wait_idle();
    int64_t start = esp_timer_get_time();
    if (err == ESP_OK) err = epd_cmd(0x12, NULL, 0);
    if (err == ESP_OK) err = epd_wait_idle();
    int ms = (int)((esp_timer_get_time() - start) / 1000);
    if (err == ESP_OK) err = epd_cmd(0x02, NULL, 0);
    if (err == ESP_OK) err = epd_wait_idle();
    static const uint8_t check = 0xA5;
    if (err == ESP_OK) err = epd_cmd(0x07, &check, 1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "e-paper refresh failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "e-paper %s refresh in %d ms", full ? "full" : "fast", ms);
    return ESP_OK;
}

static const epd_panel_t s_panel = {
    .name            = "reTerminal E1001 UC8179",
    .depth           = "1 bit per pixel",
    .width           = EPD_W,
    .height          = EPD_H,
    .bits_per_pixel  = 1,
    .title_y         = 40,
    .title_max_scale = 8,
    .anim_scale      = 4,
    .anim_y          = 128,
    .status_y        = 404,
    .status_scale    = 4,
    .init            = uc8179_init,
    .update          = uc8179_update,
};

const epd_panel_t *epd_panel_get(void) { return &s_panel; }
