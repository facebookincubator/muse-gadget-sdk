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

// reTerminal E1003 V1.0 schematic sheets 4-8 govern all GPIOs below.
// IT8951 command framing and ED103TC2 one-bit mode are adapted from
// Seeed_GFX2's Driver_IT8951 and Driver_ED103TC2:
//
// MIT License
// Copyright (c) 2026 Seeed Studio
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
#include "epd_panel.h"
#include "reterminal_e100x_board.h"
#include "sdkconfig.h"
#if CONFIG_HOMEHUB_RETERMINAL_SHT4X
#include "reterminal_sht4x.h"
#endif

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#define PANEL_W E100X_WIDTH
#define PANEL_H E100X_HEIGHT
#define ROW_BYTES (PANEL_W / 8)
#define GRAY_ROW_BYTES (PANEL_W / 2)
#define TRANSFER_ROWS 14
#define SPI_HOST SPI2_HOST
#define PIN_SCK E100X_SPI_SCLK_GPIO
#define PIN_MISO E100X_SPI_MISO_GPIO
#define PIN_MOSI E100X_SPI_MOSI_GPIO
#define PIN_CS E100X_PANEL_CS_GPIO
#define PIN_DRIVE_EN E100X_PANEL_DC_OR_DRIVE_GPIO
#define PIN_RESET E100X_PANEL_RESET_GPIO
#define PIN_READY E100X_PANEL_READY_GPIO
#define PIN_SD_CS E100X_SD_CS_GPIO
#define PIN_SD_EN E100X_SD_ENABLE_GPIO
#define PIN_TCON_EN E100X_TCON_ENABLE_GPIO

#define CMD_REG_READ 0x0010
#define CMD_REG_WRITE 0x0011
#define CMD_LOAD_AREA 0x0021
#define CMD_LOAD_END 0x0022
#define CMD_DISPLAY_AREA 0x0034
#define CMD_GET_INFO 0x0302
#define CMD_SET_TEMPERATURE 0x0040
#define FALLBACK_TEMPERATURE_C 16
#define REG_PACKED_MODE 0x0004
#define REG_LISAR 0x0208
#define REG_ONE_BIT_MODE 0x113a
#define REG_LUT_ACTIVE 0x1224
#define REG_ONE_BIT_COLOURS 0x1250

static const char *TAG = "link.led";
static spi_device_handle_t s_spi;
static uint8_t *s_tx;
static uint32_t s_image_address;
static esp_err_t wait_ready(int timeout_ms) {
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    while (!gpio_get_level(PIN_READY)) {
        if (esp_timer_get_time() >= deadline) return ESP_ERR_TIMEOUT;
        vTaskDelay(1);
    }
    return ESP_OK;
}

// The caller owns the SPI bus. KEEP_ACTIVE retains CS between the 16-bit
// preamble and the following word; the final word releases CS.
static esp_err_t word(uint16_t tx, uint16_t *rx, bool keep_active) {
    spi_transaction_t t = {
        .flags = SPI_TRANS_USE_TXDATA | SPI_TRANS_USE_RXDATA
               | (keep_active ? SPI_TRANS_CS_KEEP_ACTIVE : 0),
        .length = 16,
        .rxlength = 16,
    };
    t.tx_data[0] = (uint8_t)(tx >> 8);
    t.tx_data[1] = (uint8_t)tx;
    esp_err_t err = spi_device_polling_transmit(s_spi, &t);
    if (err == ESP_OK && rx) *rx = (uint16_t)((uint16_t)t.rx_data[0] << 8 | t.rx_data[1]);
    return err;
}

static esp_err_t write_pair(uint16_t preamble, uint16_t value) {
    esp_err_t err = spi_device_acquire_bus(s_spi, portMAX_DELAY);
    if (err != ESP_OK) return err;
    err = wait_ready(10000);
    if (err == ESP_OK) err = word(preamble, NULL, true);
    if (err == ESP_OK) err = wait_ready(10000);
    if (err == ESP_OK) err = word(value, NULL, false);
    spi_device_release_bus(s_spi);
    return err;
}

static esp_err_t command(uint16_t code) { return write_pair(0x6000, code); }
static esp_err_t data_word(uint16_t value) { return write_pair(0x0000, value); }

static esp_err_t read_words(uint16_t *out, size_t count) {
    if (!count) return ESP_ERR_INVALID_ARG;
    esp_err_t err = spi_device_acquire_bus(s_spi, portMAX_DELAY);
    if (err != ESP_OK) return err;
    err = wait_ready(10000);
    if (err == ESP_OK) err = word(0x1000, NULL, true);
    if (err == ESP_OK) err = wait_ready(10000);
    if (err == ESP_OK) err = word(0, NULL, true); // IT8951 dummy read.
    for (size_t i = 0; err == ESP_OK && i < count; ++i) {
        err = wait_ready(10000);
        if (err == ESP_OK) err = word(0, &out[i], i + 1 < count);
    }
    spi_device_release_bus(s_spi);
    return err;
}

static esp_err_t read_reg(uint16_t address, uint16_t *value) {
    esp_err_t err = command(CMD_REG_READ);
    if (err == ESP_OK) err = data_word(address);
    if (err == ESP_OK) err = read_words(value, 1);
    return err;
}

static esp_err_t write_reg(uint16_t address, uint16_t value) {
    esp_err_t err = command(CMD_REG_WRITE);
    if (err == ESP_OK) err = data_word(address);
    if (err == ESP_OK) err = data_word(value);
    return err;
}

static esp_err_t wait_refresh(bool expect_start) {
    int64_t deadline = esp_timer_get_time() + 35000000LL;
    int64_t start_deadline = esp_timer_get_time() + 1000000LL;
    uint16_t active = 0;
    bool started = false;
    for (;;) {
        esp_err_t err = read_reg(REG_LUT_ACTIVE, &active);
        if (err != ESP_OK) return err;
        if (active) started = true;
        if (!active && (started || !expect_start || esp_timer_get_time() >= start_deadline)) {
            if (expect_start && !started) {
                ESP_LOGW(TAG, "IT8951 LUT never reported busy after display command");
            }
            return ESP_OK;
        }
        if (esp_timer_get_time() >= deadline) return ESP_ERR_TIMEOUT;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static esp_err_t load_frame_area(const uint8_t *frame, int x, int y, int w, int h) {
    // IT8951 accepts the packed one-bit image as 8bpp data with dimensions
    // reduced by eight horizontally. Its native image order mirrors each row.
    esp_err_t err = write_reg(REG_LISAR + 2, (uint16_t)(s_image_address >> 16));
    if (err == ESP_OK) err = write_reg(REG_LISAR, (uint16_t)s_image_address);
    if (err == ESP_OK) err = command(CMD_LOAD_AREA);
    const int row_bytes = w / 8;
    const uint16_t args[] = {0x0030, (uint16_t)((PANEL_W - x - w) / 8),
                             (uint16_t)y, (uint16_t)row_bytes, (uint16_t)h};
    for (size_t i = 0; err == ESP_OK && i < sizeof(args) / sizeof(args[0]); ++i) {
        err = data_word(args[i]);
    }
    if (err != ESP_OK) return err;

    err = spi_device_acquire_bus(s_spi, portMAX_DELAY);
    if (err != ESP_OK) return err;
    err = wait_ready(10000);
    if (err == ESP_OK) err = word(0x0000, NULL, true);
    if (err == ESP_OK) err = wait_ready(10000);
    for (int first = 0; err == ESP_OK && first < h; first += TRANSFER_ROWS) {
        int rows = h - first;
        if (rows > TRANSFER_ROWS) rows = TRANSFER_ROWS;
        for (int r = 0; r < rows; ++r) {
            const uint8_t *src = frame + (size_t)(y + first + r) * ROW_BYTES + x / 8;
            uint8_t *dst = s_tx + (size_t)r * row_bytes;
            for (int i = 0; i < row_bytes; i += 2) {
                // epaper_status uses 1=white; IT8951's 1bpp colour table uses
                // 0=white and 1=black. Reverse 16-bit words as in Seeed_GFX2.
                int from = row_bytes - 2 - i;
                dst[i] = (uint8_t)~src[from];
                dst[i + 1] = (uint8_t)~src[from + 1];
            }
        }
        bool last = first + rows == h;
        spi_transaction_t t = {
            .flags = last ? 0 : SPI_TRANS_CS_KEEP_ACTIVE,
            .length = (size_t)rows * row_bytes * 8,
            .tx_buffer = s_tx,
        };
        err = spi_device_polling_transmit(s_spi, &t);
    }
    spi_device_release_bus(s_spi);
    esp_err_t end_err = command(CMD_LOAD_END);
    return err == ESP_OK ? end_err : err;
}

static esp_err_t load_frame(const uint8_t *frame) {
    return load_frame_area(frame, 0, 0, PANEL_W, PANEL_H);
}

static esp_err_t load_gray16(const uint8_t *luma) {
    esp_err_t err = write_reg(REG_LISAR + 2, (uint16_t)(s_image_address >> 16));
    if (err == ESP_OK) err = write_reg(REG_LISAR, (uint16_t)s_image_address);
    if (err == ESP_OK) err = command(CMD_LOAD_AREA);
    const uint16_t args[] = {0x0020, 0, 0, PANEL_W, PANEL_H}; // IT8951 4bpp.
    for (size_t i = 0; err == ESP_OK && i < sizeof(args) / sizeof(args[0]); ++i)
        err = data_word(args[i]);
    if (err != ESP_OK) return err;

    err = spi_device_acquire_bus(s_spi, portMAX_DELAY);
    if (err != ESP_OK) return err;
    err = wait_ready(10000);
    if (err == ESP_OK) err = word(0x0000, NULL, true);
    if (err == ESP_OK) err = wait_ready(10000);
    for (int first = 0; err == ESP_OK && first < PANEL_H; first += TRANSFER_ROWS) {
        int rows = PANEL_H - first;
        if (rows > TRANSFER_ROWS) rows = TRANSFER_ROWS;
        for (int r = 0; r < rows; ++r) {
            const uint8_t *src = luma + (size_t)(first + r) * PANEL_W;
            uint8_t *dst = s_tx + (size_t)r * GRAY_ROW_BYTES;
            // Mirror complete 16-bit words, retaining each word's byte order,
            // as Seeed_GFX2 tconHostAreaPackedPixelWrite does for 4bpp.
            for (int i = 0; i < GRAY_ROW_BYTES; i += 2) {
                int from = PANEL_W - 4 - i * 2;
                uint8_t a = (uint8_t)((src[from] + 8) / 17);
                uint8_t b = (uint8_t)((src[from + 1] + 8) / 17);
                uint8_t c = (uint8_t)((src[from + 2] + 8) / 17);
                uint8_t d = (uint8_t)((src[from + 3] + 8) / 17);
                dst[i] = (a << 4) | b;
                dst[i + 1] = (c << 4) | d;
            }
        }
        spi_transaction_t t = {
            .flags = first + rows == PANEL_H ? 0 : SPI_TRANS_CS_KEEP_ACTIVE,
            .length = (size_t)rows * GRAY_ROW_BYTES * 8,
            .tx_buffer = s_tx,
        };
        err = spi_device_polling_transmit(s_spi, &t);
    }
    spi_device_release_bus(s_spi);
    esp_err_t end_err = command(CMD_LOAD_END);
    return err == ESP_OK ? end_err : err;
}

static esp_err_t set_refresh_temperature(void) {
    // Seeed_GFX2 Driver_ED103TC2/IT8951 uses 0x0040, 1, uint8_t(temp).
    // Set it before each display operation so the TCON selects a waveform.
    int8_t temperature_c = FALLBACK_TEMPERATURE_C;
    bool measured = false;
#if CONFIG_HOMEHUB_RETERMINAL_SHT4X
    float sensor_c;
    if (reterminal_sht4x_temperature(&sensor_c) && sensor_c >= -40.0f
        && sensor_c <= 85.0f) {
        temperature_c = (int8_t)sensor_c; // Match GFX2's truncation to int8_t.
        measured = true;
    }
#endif
    ESP_LOGI(TAG, "IT8951 refresh temperature %d C (%s)", temperature_c,
             measured ? "SHT40" : "fallback");
    esp_err_t err = command(CMD_SET_TEMPERATURE);
    if (err == ESP_OK) err = data_word(1); // Set temperature.
    if (err == ESP_OK) err = data_word((uint8_t)temperature_c);
    return err;
}

static esp_err_t display_area(int x, int y, int w, int h, int mode,
                              bool one_bit) {
    esp_err_t err = ESP_OK;
    uint16_t register_mode = 0;
    bool one_bit_enabled = false;
    if (one_bit) {
        err = read_reg(REG_ONE_BIT_MODE, &register_mode);
        if (err == ESP_OK) {
            err = write_reg(REG_ONE_BIT_MODE, register_mode | (1U << 2));
            one_bit_enabled = err == ESP_OK;
        }
        if (err == ESP_OK) err = write_reg(REG_ONE_BIT_COLOURS, 0xff00);
        x = PANEL_W - x - w; // GFX2's 1bpp display window is mirrored.
    }
    if (err == ESP_OK) err = command(CMD_DISPLAY_AREA);
    const uint16_t area[] = {(uint16_t)x, (uint16_t)y, (uint16_t)w,
                             (uint16_t)h, (uint16_t)mode};
    for (size_t i = 0; err == ESP_OK && i < sizeof(area) / sizeof(area[0]); ++i)
        err = data_word(area[i]);
    if (err == ESP_OK) err = wait_refresh(true);
    esp_err_t restore = one_bit_enabled
        ? write_reg(REG_ONE_BIT_MODE, register_mode & ~(1U << 2)) : ESP_OK;
    return err == ESP_OK ? restore : err;
}

static esp_err_t it8951_update(const uint8_t *frame, const uint8_t *prev, bool full) {
    (void)prev; // The TCON has one image buffer, not a UC8179 old-data plane.
    (void)full; // Status images remain full refreshes.
    if (!frame || !s_spi) return ESP_ERR_INVALID_STATE;
    if (!reterminal_e100x_spi_lock()) return ESP_ERR_INVALID_STATE;
    esp_err_t err = wait_refresh(false);
    if (err == ESP_OK) err = set_refresh_temperature();
    if (err == ESP_OK) err = load_frame(frame);
    if (err == ESP_OK) err = display_area(0, 0, PANEL_W, PANEL_H, 2, true);
    if (err != ESP_OK) ESP_LOGE(TAG, "IT8951 refresh failed: %s", esp_err_to_name(err));
    reterminal_e100x_spi_unlock();
    return err;
}

static esp_err_t it8951_update_gray16(const uint8_t *luma) {
    if (!luma || !s_spi) return ESP_ERR_INVALID_STATE;
    if (!reterminal_e100x_spi_lock()) return ESP_ERR_INVALID_STATE;
    esp_err_t err = wait_refresh(false);
    if (err == ESP_OK) err = set_refresh_temperature();
    if (err == ESP_OK) err = load_gray16(luma);
    if (err == ESP_OK) err = display_area(0, 0, PANEL_W, PANEL_H, 2, false);
    if (err != ESP_OK) ESP_LOGE(TAG, "IT8951 gray16 refresh failed: %s", esp_err_to_name(err));
    reterminal_e100x_spi_unlock();
    return err;
}

static esp_err_t it8951_update_partial(const uint8_t *frame, int x, int y, int w, int h) {
    if (!frame || !s_spi || x < 0 || y < 0 || w <= 0 || h <= 0 || (x & 15)
        || (w & 15) || x >= PANEL_W || y >= PANEL_H
        || w > PANEL_W - x || h > PANEL_H - y)
        return ESP_ERR_INVALID_ARG;
    if (!reterminal_e100x_spi_lock()) return ESP_ERR_INVALID_STATE;
    esp_err_t err = wait_refresh(false);
    if (err == ESP_OK) err = set_refresh_temperature();
    if (err == ESP_OK) err = load_frame_area(frame, x, y, w, h);
    // The panel's partial refresh fetches 32-pixel chunks. Align in mirrored
    // TCON coordinates, where this 1872-pixel panel's x origin differs by 16
    // pixels from the caller's 32-pixel grid. A 16-aligned window at (288,312)
    // otherwise leaves the rightmost strip unchanged. The expanded pixels
    // already contain the previous image in TCON RAM.
    int tcon_left = PANEL_W - x - w;
    int tcon_right = PANEL_W - x;
    int aligned_left = tcon_left & ~31;
    int aligned_right = (tcon_right + 31) & ~31;
    if (aligned_right > PANEL_W) aligned_right = PANEL_W;
    int refresh_x = PANEL_W - aligned_right;
    int refresh_w = aligned_right - aligned_left;
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "IT8951 partial window x=%d w=%d -> x=%d w=%d",
                 x, w, refresh_x, refresh_w);
        err = display_area(refresh_x, y, refresh_w, h, 1, true);
    }
    if (err != ESP_OK) ESP_LOGE(TAG, "IT8951 partial refresh failed: %s", esp_err_to_name(err));
    reterminal_e100x_spi_unlock();
    return err;
}

static esp_err_t it8951_init(void) {
    if (!reterminal_e100x_spi_prepare()) return ESP_ERR_NO_MEM;
    // GPIO39 gates the microSD slot, which shares MISO with the IT8951.
    const gpio_config_t outputs = {
        .pin_bit_mask = (1ULL << PIN_CS) | (1ULL << PIN_RESET)
                      | (1ULL << PIN_DRIVE_EN) | (1ULL << PIN_TCON_EN)
                      | (1ULL << PIN_SD_CS) | (1ULL << PIN_SD_EN),
        .mode = GPIO_MODE_OUTPUT,
    };
    const gpio_config_t ready = {
        .pin_bit_mask = 1ULL << PIN_READY,
        .mode = GPIO_MODE_INPUT,
    };
    esp_err_t err = gpio_config(&outputs);
    if (err == ESP_OK) err = gpio_config(&ready);
    if (err != ESP_OK) return err;
    gpio_set_level(PIN_CS, 1);
    gpio_set_level(PIN_SD_CS, 1);
    // The E1003 schematic's SD_EN is active high. The factory firmware and
    // Seeed_GFX2 enable it before starting the shared SPI bus; CS stays high.
    gpio_set_level(PIN_SD_EN, 1);
    gpio_set_level(PIN_RESET, 1);
    gpio_set_level(PIN_TCON_EN, 1);
    gpio_set_level(PIN_DRIVE_EN, 1);
    // E1003 A/B diagnostic: R43 pulls the IT8951 3.3 V switch EN low if
    // automatic Light Sleep selects GPIO21's isolated sleep configuration.
    // Keep this pin's normal output-high configuration during Light Sleep.
    err = gpio_sleep_sel_dis(PIN_TCON_EN);
    if (err != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(300));

    const spi_bus_config_t bus = {
        .sclk_io_num = PIN_SCK,
        .miso_io_num = PIN_MISO,
        .mosi_io_num = PIN_MOSI,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = TRANSFER_ROWS * GRAY_ROW_BYTES,
    };
    const spi_device_interface_config_t device = {
        .mode = 0,
        .clock_speed_hz = 4 * 1000 * 1000, // Seeed_GFX2's read clock.
        .spics_io_num = PIN_CS,
        .queue_size = 1,
    };
    s_tx = heap_caps_malloc(TRANSFER_ROWS * GRAY_ROW_BYTES, MALLOC_CAP_DMA);
    if (!s_tx) return ESP_ERR_NO_MEM;
    err = spi_bus_initialize(SPI_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err == ESP_OK) err = spi_bus_add_device(SPI_HOST, &device, &s_spi);
    if (err != ESP_OK) return err;

    gpio_set_level(PIN_RESET, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(PIN_RESET, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
    err = wait_ready(10000);
    // The official E1003 display initialization sets the TCON VCOM to 1400.
    if (err == ESP_OK) err = command(0x0039);
    if (err == ESP_OK) err = data_word(2);
    if (err == ESP_OK) err = data_word(1400);
    uint16_t info[20] = {0};
    if (err == ESP_OK) err = command(CMD_GET_INFO);
    if (err == ESP_OK) err = read_words(info, 20);
    if (err == ESP_OK && (info[0] != PANEL_W || info[1] != PANEL_H)) {
        ESP_LOGE(TAG, "unexpected IT8951 panel: %ux%u", info[0], info[1]);
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (err == ESP_OK) {
        s_image_address = (uint32_t)info[2] | (uint32_t)info[3] << 16;
        ESP_LOGI(TAG, "IT8951 panel %ux%u image address 0x%08lx",
                 info[0], info[1], (unsigned long)s_image_address);
        err = write_reg(REG_PACKED_MODE, 1);
    }
    if (err != ESP_OK) ESP_LOGE(TAG, "IT8951 init failed: %s", esp_err_to_name(err));
    return err;
}

static const epd_panel_t s_panel = {
    .name = "reTerminal E1003 IT8951",
    .depth = "1 bit per pixel",
    .width = PANEL_W,
    .height = PANEL_H,
    .bits_per_pixel = 1,
    .title_y = 96,
    .title_max_scale = 16,
    .anim_scale = 8,
    .anim_y = 320,
    .status_y = 1130,
    .status_scale = 8,
    .init = it8951_init,
    .update = it8951_update,
    .update_gray16 = it8951_update_gray16,
    .update_partial = it8951_update_partial,
};

const epd_panel_t *epd_panel_get(void) { return &s_panel; }
