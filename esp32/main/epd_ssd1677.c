/*
 * MIT License
 *
 * Copyright (c) 2026 JoJang2u
 * Copyright (c) 2026 limengdu
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * SSD1677 panel protocol and refresh sequence reference:
 * Seeed-reTerminal-E10xx-Firmware factory_fw,
 * components/seeed_epaper/driver/ssd1677.c.
 */
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
#define EPD_PIN_SCLK    13
#define EPD_PIN_MOSI    14
#define EPD_PIN_CS      15
#define EPD_PIN_DC      16
#define EPD_PIN_RST     17
#define EPD_PIN_BUSY    18
#define EPD_PIN_EN      47
#define EPD_SPI_HZ      (20 * 1000 * 1000)
#define EPD_W           800
#define EPD_H           480
#define EPD_LOGICAL_W   EPD_H
#define EPD_LOGICAL_H   EPD_W
#define EPD_ROW_BYTES   (EPD_W / 8)
#define EPD_CHUNK_ROWS  40
#define EPD_CHUNK_BYTES (EPD_ROW_BYTES * EPD_CHUNK_ROWS)
#define EPD_BUSY_TIMEOUT_MS 30000

static spi_device_handle_t s_spi;
static uint8_t *s_chunk;
static bool s_full_needed = true;
static bool s_partial_prepared;

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
    while (gpio_get_level(EPD_PIN_BUSY) == 1) {
        if (esp_timer_get_time() > give_up) return ESP_ERR_TIMEOUT;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return ESP_OK;
}

static void ssd1677_transpose_rows(uint8_t *dst, const uint8_t *src,
                                   int first_row, int rows, int logical_width, int logical_height) {
    int src_stride = logical_width / 8;
    int dst_stride = logical_height / 8;
    for (int row = 0; row < rows; row++) {
        int src_x = first_row + row;
        uint8_t mask = (uint8_t)(0x80 >> (src_x & 7));
        uint8_t *out = dst + (size_t)row * dst_stride;
        for (int col = 0; col < dst_stride; col++) {
            uint8_t byte = 0;
            for (int bit = 0; bit < 8; bit++) {
                int src_y = col * 8 + bit;
                byte = (uint8_t)(byte << 1 | !!(src[(size_t)src_y * src_stride + src_x / 8] & mask));
            }
            out[col] = byte;
        }
    }
}

static void ssd1677_gray4_plane_rows(uint8_t *dst, const uint8_t *src,
                                      int first_row, int rows, int logical_width,
                                      int logical_height, int plane) {
    int src_stride = logical_width / 4;
    int dst_stride = logical_height / 8;
    for (int row = 0; row < rows; row++) {
        int src_x = first_row + row;
        uint8_t *out = dst + (size_t)row * dst_stride;
        for (int col = 0; col < dst_stride; col++) {
            uint8_t byte = 0;
            for (int bit = 0; bit < 8; bit++) {
                int src_y = col * 8 + bit;
                uint8_t gray = (uint8_t)(src[(size_t)src_y * src_stride + src_x / 4]
                                         >> (6 - 2 * (src_x & 3)) & 3);
                byte = (uint8_t)(byte << 1 | !((gray >> plane) & 1));
            }
            out[col] = byte;
        }
    }
}

static esp_err_t epd_frame(uint8_t cmd, const uint8_t *frame) {
    esp_err_t err = epd_write(false, &cmd, 1);
    if (err != ESP_OK) return err;
    gpio_set_level(EPD_PIN_DC, 1);
    for (int y = 0; y < EPD_H; y += EPD_CHUNK_ROWS) {
        ssd1677_transpose_rows(s_chunk, frame, y, EPD_CHUNK_ROWS, EPD_LOGICAL_W, EPD_LOGICAL_H);
        spi_transaction_t t = {.length = EPD_CHUNK_BYTES * 8, .tx_buffer = s_chunk};
        err = spi_device_polling_transmit(s_spi, &t);
        if (err != ESP_OK) return err;
    }
    return ESP_OK;
}

static esp_err_t epd_gray4_frame(uint8_t cmd, const uint8_t *frame, int plane) {
    esp_err_t err = epd_write(false, &cmd, 1);
    if (err != ESP_OK) return err;
    gpio_set_level(EPD_PIN_DC, 1);
    for (int y = 0; y < EPD_H; y += EPD_CHUNK_ROWS) {
        ssd1677_gray4_plane_rows(s_chunk, frame, y, EPD_CHUNK_ROWS,
                                 EPD_LOGICAL_W, EPD_LOGICAL_H, plane);
        spi_transaction_t t = {.length = EPD_CHUNK_BYTES * 8, .tx_buffer = s_chunk};
        err = spi_device_polling_transmit(s_spi, &t);
        if (err != ESP_OK) return err;
    }
    return ESP_OK;
}

static esp_err_t ssd1677_init(void) {
    const gpio_config_t out = {
        .pin_bit_mask = 1ULL << EPD_PIN_DC | 1ULL << EPD_PIN_RST | 1ULL << EPD_PIN_EN,
        .mode = GPIO_MODE_OUTPUT,
    };
    const gpio_config_t busy = {
        .pin_bit_mask = 1ULL << EPD_PIN_BUSY,
        .mode = GPIO_MODE_INPUT,
    };
    esp_err_t err = gpio_config(&out);
    if (err == ESP_OK) err = gpio_config(&busy);
    if (err != ESP_OK) return err;
    gpio_set_level(EPD_PIN_EN, 1);
    gpio_set_level(EPD_PIN_RST, 1);
    const spi_bus_config_t bus = {
        .sclk_io_num = EPD_PIN_SCLK,
        .mosi_io_num = EPD_PIN_MOSI,
        .miso_io_num = 12,
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
    err = spi_bus_initialize(EPD_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err == ESP_OK) err = spi_bus_add_device(EPD_HOST, &dev, &s_spi);
    return err;
}

static esp_err_t ssd1677_set_window(void) {
    const uint8_t x_range[] = {0, 0, (EPD_W - 1) & 0xFF, (EPD_W - 1) >> 8};
    const uint8_t y_range[] = {0, 0, (EPD_H - 1) & 0xFF, (EPD_H - 1) >> 8};
    const uint8_t cursor[] = {0, 0};
    esp_err_t err = epd_cmd(0x44, x_range, sizeof(x_range));
    if (err == ESP_OK) err = epd_cmd(0x45, y_range, sizeof(y_range));
    if (err == ESP_OK) err = epd_cmd(0x4E, cursor, sizeof(cursor));
    if (err == ESP_OK) err = epd_cmd(0x4F, cursor, sizeof(cursor));
    return err;
}

static esp_err_t ssd1677_update(const uint8_t *frame, const uint8_t *prev, bool full) {
    if (!frame || !prev) return ESP_ERR_INVALID_ARG;
    full = full || s_full_needed;
    gpio_set_level(EPD_PIN_EN, 1);
    esp_err_t err = ESP_OK;
    if (full || !s_partial_prepared) {
        gpio_set_level(EPD_PIN_RST, 0);
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level(EPD_PIN_RST, 1);
        vTaskDelay(pdMS_TO_TICKS(10));
        err = epd_wait_idle();
        if (err == ESP_OK && full) err = epd_cmd(0x12, NULL, 0);
        if (err == ESP_OK && full) err = epd_wait_idle();
        const uint8_t temp = 0x80, border = full ? 0x01 : 0x80;
        const uint8_t softstart[] = {0xAE, 0xC7, 0xC3, 0xC0, 0x80};
        const uint8_t driver[] = {(EPD_H - 1) & 0xFF, (EPD_H - 1) >> 8, 0x02};
        const uint8_t entry = 0x03;
        if (err == ESP_OK) err = epd_cmd(0x18, &temp, 1);
        if (err == ESP_OK) err = epd_cmd(0x3C, &border, 1);
        if (err == ESP_OK) err = epd_cmd(0x0C, softstart, sizeof(softstart));
        if (err == ESP_OK) err = epd_cmd(0x01, driver, sizeof(driver));
        if (err == ESP_OK) err = epd_cmd(0x11, &entry, 1);
        if (err == ESP_OK) err = epd_wait_idle();
    }
    if (err == ESP_OK) err = ssd1677_set_window();
    if (err == ESP_OK) err = epd_frame(0x26, full ? frame : prev);
    if (err == ESP_OK) err = epd_frame(0x24, frame);
    const uint8_t update_control[] = {0, 0};
    if (err == ESP_OK && !full) err = epd_cmd(0x21, update_control, sizeof(update_control));
    uint8_t refresh = full ? 0xF7 : 0xFF;
    if (err == ESP_OK) err = epd_cmd(0x22, &refresh, 1);
    int64_t start = esp_timer_get_time();
    if (err == ESP_OK) err = epd_cmd(0x20, NULL, 0);
    if (err == ESP_OK) err = epd_wait_idle();
    if (err == ESP_OK && !full) err = ssd1677_set_window();
    if (err == ESP_OK && !full) err = epd_frame(0x26, frame);
    if (err == ESP_OK && !full) err = epd_frame(0x24, frame);
    s_full_needed = err != ESP_OK;
    s_partial_prepared = err == ESP_OK && !full;
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "e-paper refresh failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "e-paper %s refresh in %d ms", full ? "full" : "fast",
             (int)((esp_timer_get_time() - start) / 1000));
    return ESP_OK;
}

static esp_err_t ssd1677_update_gray4(const uint8_t *frame) {
    if (!frame) return ESP_ERR_INVALID_ARG;
    gpio_set_level(EPD_PIN_EN, 1);
    gpio_set_level(EPD_PIN_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(EPD_PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
    esp_err_t err = epd_wait_idle();
    if (err == ESP_OK) err = epd_cmd(0x12, NULL, 0);
    if (err == ESP_OK) err = epd_wait_idle();
    const uint8_t temp = 0x80, border = 0x00;
    const uint8_t softstart[] = {0xAE, 0xC7, 0xC3, 0xC0, 0x80};
    const uint8_t driver[] = {(EPD_H - 1) & 0xFF, (EPD_H - 1) >> 8, 0x02};
    const uint8_t entry = 0x03;
    if (err == ESP_OK) err = epd_cmd(0x18, &temp, 1);
    if (err == ESP_OK) err = epd_cmd(0x3C, &border, 1);
    if (err == ESP_OK) err = epd_cmd(0x0C, softstart, sizeof(softstart));
    if (err == ESP_OK) err = epd_cmd(0x01, driver, sizeof(driver));
    if (err == ESP_OK) err = epd_cmd(0x11, &entry, 1);
    if (err == ESP_OK) err = epd_wait_idle();
    if (err == ESP_OK) err = ssd1677_set_window();
    if (err == ESP_OK) err = epd_gray4_frame(0x24, frame, 1);
    if (err == ESP_OK) err = epd_gray4_frame(0x26, frame, 0);
    const uint8_t forced_temp[] = {0x67, 0x00};
    if (err == ESP_OK) err = epd_cmd(0x1A, forced_temp, sizeof(forced_temp));
    const uint8_t refresh = 0xD7;
    if (err == ESP_OK) err = epd_cmd(0x22, &refresh, 1);
    int64_t start = esp_timer_get_time();
    if (err == ESP_OK) err = epd_cmd(0x20, NULL, 0);
    if (err == ESP_OK) err = epd_wait_idle();
    s_full_needed = true;
    s_partial_prepared = false;
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "e-paper gray4 refresh failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "e-paper gray4 refresh in %d ms",
             (int)((esp_timer_get_time() - start) / 1000));
    return ESP_OK;
}

static const epd_panel_t s_panel = {
    .name            = "reTerminal Sticky SSD1677",
    .depth           = "1 bit per pixel",
    .width           = EPD_LOGICAL_W,
    .height          = EPD_LOGICAL_H,
    .bits_per_pixel  = 1,
    .title_y         = 96,
    .title_max_scale = 5,
    .anim_scale      = 4,
    .anim_y          = 260,
    .status_y        = 660,
    .status_scale    = 3,
    .init            = ssd1677_init,
    .update          = ssd1677_update,
    .update_gray4    = ssd1677_update_gray4,
};

const epd_panel_t *epd_panel_get(void) { return &s_panel; }
