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

// Waveshare ESP32-S3-1.54inch-ePaper V2: 200x200 black and white e-paper,
// SSD1681 controller on SPI3, active-low power enable, manual chip select.
//
// Init sequence and waveform LUT from the working XiaoZhi V2 driver
// (waveshareteam/ESP32-S3-ePaper-1.54). Pins from user_config.h.

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

#define EPD_HOST        SPI3_HOST
#define EPD_PIN_SCLK    12
#define EPD_PIN_MOSI    13
#define EPD_PIN_CS      11
#define EPD_PIN_DC      10
#define EPD_PIN_RST     9
#define EPD_PIN_BUSY    8
#define EPD_PIN_PWR     6
#define EPD_PWR_ON      0
#define EPD_SPI_HZ      (40 * 1000 * 1000)
#define EPD_W           200
#define EPD_H           200
#define EPD_ROW_BYTES   (EPD_W / 8)
#define EPD_FRAME_BYTES (EPD_ROW_BYTES * EPD_H)
#define EPD_CHUNK_BYTES (EPD_ROW_BYTES * 40)
#define EPD_BUSY_TIMEOUT_MS 10000

static spi_device_handle_t s_spi;
static uint8_t *s_chunk;

static esp_err_t epd_write(bool data, const uint8_t *buf, size_t len) {
    gpio_set_level(EPD_PIN_DC, data);
    gpio_set_level(EPD_PIN_CS, 0);
    while (len) {
        size_t n = len < EPD_CHUNK_BYTES ? len : EPD_CHUNK_BYTES;
        memcpy(s_chunk, buf, n);
        spi_transaction_t t = {.length = n * 8, .tx_buffer = s_chunk};
        esp_err_t err = spi_device_polling_transmit(s_spi, &t);
        if (err != ESP_OK) {
            gpio_set_level(EPD_PIN_CS, 1);
            return err;
        }
        buf += n;
        len -= n;
    }
    gpio_set_level(EPD_PIN_CS, 1);
    return ESP_OK;
}

static esp_err_t epd_cmd(uint8_t cmd, const uint8_t *data, size_t len) {
    esp_err_t err = epd_write(false, &cmd, 1);
    if (err == ESP_OK && len) err = epd_write(true, data, len);
    return err;
}

static esp_err_t epd_wait_idle(void) {
    int64_t give_up = esp_timer_get_time() + EPD_BUSY_TIMEOUT_MS * 1000LL;
    while (gpio_get_level(EPD_PIN_BUSY) == 1) {
        if (esp_timer_get_time() > give_up) return ESP_ERR_TIMEOUT;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return ESP_OK;
}

static const uint8_t s_lut_full[159] = {
    0x80,0x48,0x40,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x40,0x48,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x80,0x48,0x40,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x40,0x48,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x0A,0x00,0x00,0x00,0x00,0x00,0x00,
    0x08,0x01,0x00,0x08,0x01,0x00,0x02,
    0x0A,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x22,0x22,0x22,0x22,0x22,0x22,0x00,0x00,0x00,
    0x22,0x17,0x41,0x00,0x32,0x20,
};

static esp_err_t ssd1681_set_lut(const uint8_t *lut) {
    esp_err_t err = epd_cmd(0x32, lut, 153);
    if (err == ESP_OK) err = epd_wait_idle();
    uint8_t b;
    b = lut[153]; if (err == ESP_OK) err = epd_cmd(0x3F, &b, 1);
    b = lut[154]; if (err == ESP_OK) err = epd_cmd(0x03, &b, 1);
    uint8_t pwr[] = {lut[155], lut[156], lut[157]};
    if (err == ESP_OK) err = epd_cmd(0x04, pwr, 3);
    b = lut[158]; if (err == ESP_OK) err = epd_cmd(0x2C, &b, 1);
    return err;
}

static esp_err_t ssd1681_set_window(void) {
    uint8_t x_range[] = {0x00, (EPD_W / 8) - 1};
    esp_err_t err = epd_cmd(0x44, x_range, sizeof(x_range));
    if (err != ESP_OK) return err;
    uint8_t y_range[] = {(EPD_H - 1) & 0xFF, (EPD_H - 1) >> 8, 0x00, 0x00};
    return epd_cmd(0x45, y_range, sizeof(y_range));
}

static esp_err_t ssd1681_set_cursor(void) {
    uint8_t x = 0x00;
    esp_err_t err = epd_cmd(0x4E, &x, 1);
    if (err != ESP_OK) return err;
    uint8_t y[] = {(EPD_H - 1) & 0xFF, (EPD_H - 1) >> 8};
    return epd_cmd(0x4F, y, sizeof(y));
}

static esp_err_t ssd1681_init(void) {
    const gpio_config_t out = {
        .pin_bit_mask = 1ULL << EPD_PIN_DC | 1ULL << EPD_PIN_RST
                      | 1ULL << EPD_PIN_CS | 1ULL << EPD_PIN_PWR,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    const gpio_config_t busy = {
        .pin_bit_mask = 1ULL << EPD_PIN_BUSY,
        .mode = GPIO_MODE_INPUT,
    };
    esp_err_t err = gpio_config(&out);
    if (err == ESP_OK) err = gpio_config(&busy);
    gpio_set_level(EPD_PIN_PWR, EPD_PWR_ON);
    gpio_set_level(EPD_PIN_CS, 1);
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
        .spics_io_num = -1,
        .queue_size = 1,
    };
    s_chunk = heap_caps_malloc(EPD_CHUNK_BYTES, MALLOC_CAP_DMA);
    if (!s_chunk) return ESP_ERR_NO_MEM;
    if (err == ESP_OK) err = spi_bus_initialize(EPD_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err == ESP_OK) err = spi_bus_add_device(EPD_HOST, &dev, &s_spi);
    return err;
}

static esp_err_t ssd1681_update(const uint8_t *frame, const uint8_t *prev, bool full) {
    (void)prev;
    gpio_set_level(EPD_PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(50));
    gpio_set_level(EPD_PIN_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(EPD_PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(50));

    esp_err_t err = epd_wait_idle();
    if (err == ESP_OK) err = epd_cmd(0x12, NULL, 0);
    if (err == ESP_OK) err = epd_wait_idle();
    uint8_t drv[] = {0xC7, 0x00, 0x01};
    if (err == ESP_OK) err = epd_cmd(0x01, drv, sizeof(drv));
    uint8_t entry = 0x01;
    if (err == ESP_OK) err = epd_cmd(0x11, &entry, 1);
    if (err == ESP_OK) err = ssd1681_set_window();
    uint8_t border = 0x01;
    if (err == ESP_OK) err = epd_cmd(0x3C, &border, 1);
    uint8_t temp = 0x80;
    if (err == ESP_OK) err = epd_cmd(0x18, &temp, 1);
    uint8_t load = 0xB1;
    if (err == ESP_OK) err = epd_cmd(0x22, &load, 1);
    if (err == ESP_OK) err = epd_cmd(0x20, NULL, 0);
    if (err == ESP_OK) err = ssd1681_set_cursor();
    if (err == ESP_OK) err = epd_wait_idle();
    if (err == ESP_OK) err = ssd1681_set_lut(s_lut_full);

    if (err == ESP_OK) err = epd_cmd(0x24, frame, EPD_FRAME_BYTES);
    uint8_t seq = 0xC7;
    if (err == ESP_OK) err = epd_cmd(0x22, &seq, 1);
    int64_t start = esp_timer_get_time();
    if (err == ESP_OK) err = epd_cmd(0x20, NULL, 0);
    if (err == ESP_OK) err = epd_wait_idle();
    int ms = (int)((esp_timer_get_time() - start) / 1000);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "e-paper refresh failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "e-paper %s refresh in %d ms", full ? "full" : "fast", ms);
    return ESP_OK;
}

static const epd_panel_t s_panel = {
    .name            = "Waveshare ESP32-S3-1.54inch-ePaper SSD1681",
    .depth           = "1 bit per pixel",
    .width           = EPD_W,
    .height          = EPD_H,
    .bits_per_pixel  = 1,
    .title_y         = 4,
    .title_max_scale = 2,
    .anim_scale      = 1,
    .anim_y          = 30,
    .status_y        = 170,
    .status_scale    = 2,
    .init            = ssd1681_init,
    .update          = ssd1681_update,
};

const epd_panel_t *epd_panel_get(void) { return &s_panel; }
