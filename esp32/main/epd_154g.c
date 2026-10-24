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

// Waveshare ESP32-S3-ePaper-1.54G: 200x200 four-colour e-paper, full refreshes.
// Pins and commands: Waveshare's 09_E_Paper_Test and panel manual
// (github.com/waveshareteam/ESP32-S3-ePaper-1.54G).
// Ink codes: 0 black, 1 white, 2 yellow, 3 red; leftmost pixel in the top bits.
// Holding PWR turns the picture a quarter turn clockwise, kept across restarts.
// A change that leaves most of the panel alone refreshes only a window round
// it, so the rest doesn't flash.

#include "epd_panel.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

static const char *TAG = "link.led";

#define EPD_HOST        SPI2_HOST
#define EPD_PIN_SCLK    12
#define EPD_PIN_MOSI    13
#define EPD_PIN_CS      11
#define EPD_PIN_DC      10
#define EPD_PIN_RST     9
#define EPD_PIN_BUSY    8
// Panel supply is active low; the latch keeps battery power after PWR is released.
#define EPD_PIN_PWR     6
#define BOARD_PIN_LATCH 17
// The PWR key, active low. It also switches the board on from the battery.
#define BOARD_PIN_PWR_KEY 18
#define PWR_KEY_POLL_MS 50
#define EPD_SPI_HZ      (10 * 1000 * 1000)
#define EPD_W           200
#define EPD_H           200
#define EPD_ROW_BYTES   (EPD_W / 4)
#define EPD_FRAME_BYTES (EPD_ROW_BYTES * EPD_H)
#define EPD_CHUNK_BYTES 4000
#define EPD_BUSY_TIMEOUT_MS 45000

// ---- Rotation (host-tested) -------------------------------------------------

#define ROTATE_HOLD_US 1000000
#define TAP_MIN_US     50000

// Turn `src` by `turns` quarter turns clockwise into `dst`.
static void rotate_frame(const uint8_t *src, uint8_t *dst, unsigned turns) {
    memset(dst, 0, EPD_FRAME_BYTES);
    for (int y = 0; y < EPD_H; y++) {
        for (int x = 0; x < EPD_W; x++) {
            int px = x, py = y;
            switch (turns & 3) {
            case 1: px = EPD_W - 1 - y; py = x; break;
            case 2: px = EPD_W - 1 - x; py = EPD_H - 1 - y; break;
            case 3: px = y; py = EPD_H - 1 - x; break;
            }
            unsigned ink = src[y * EPD_ROW_BYTES + x / 4] >> (6 - 2 * (x % 4)) & 3;
            dst[py * EPD_ROW_BYTES + px / 4] |= (uint8_t)(ink << (6 - 2 * (px % 4)));
        }
    }
}

typedef enum { PWR_NONE, PWR_TAP, PWR_HOLD } pwr_action_t;

typedef struct {
    bool armed, pressed, fired;
    int64_t since_us;
} pwr_key_t;

// A hold fires once, after ROTATE_HOLD_US; a release before that is a tap.
// The press that switched the board on is neither: the key must be seen
// released first.
static pwr_action_t pwr_key_update(pwr_key_t *key, bool pressed, int64_t now_us) {
    if (!pressed) {
        bool tap = key->pressed && !key->fired && now_us - key->since_us >= TAP_MIN_US;
        key->armed = true;
        key->pressed = false;
        return tap ? PWR_TAP : PWR_NONE;
    }
    if (!key->armed) return PWR_NONE;
    if (!key->pressed) {
        key->pressed = true;
        key->fired = false;
        key->since_us = now_us;
    }
    if (!key->fired && now_us - key->since_us >= ROTATE_HOLD_US) {
        key->fired = true;
        return PWR_HOLD;
    }
    return PWR_NONE;
}

// ---- Window (host-tested) ----------------------------------------------------

typedef struct {
    int x, y, width, height;
    bool changed, full;
} epd_window_t;

// The window round every pixel that differs between what the panel shows and
// the next frame, in whole bytes across. Without a known picture, or with
// most of the panel changing, the whole panel.
static epd_window_t plan_window(const uint8_t *shown, const uint8_t *next) {
    const epd_window_t full = {0, 0, EPD_W, EPD_H, true, true};
    if (!shown) return full;
    int left = EPD_ROW_BYTES, right = -1, top = EPD_H, bottom = -1;
    for (int y = 0; y < EPD_H; y++) {
        for (int byte = 0; byte < EPD_ROW_BYTES; byte++) {
            if (shown[y * EPD_ROW_BYTES + byte] == next[y * EPD_ROW_BYTES + byte]) continue;
            if (byte < left) left = byte;
            if (byte > right) right = byte;
            if (y < top) top = y;
            if (y > bottom) bottom = y;
        }
    }
    if (right < 0) return (epd_window_t){0};
    // R83H wants the end row past the start row, even for one changed row.
    if (top == bottom) {
        if (bottom + 1 < EPD_H) bottom++;
        else top--;
    }
    epd_window_t window = {left * 4, top, (right - left + 1) * 4, bottom - top + 1, true, false};
    if (window.width * window.height * 4 > EPD_W * EPD_H * 3) return full;
    return window;
}

// R83H: inclusive start and end, then 1 to refresh inside the window only.
static void encode_window(epd_window_t window, uint8_t data[9]) {
    int end_x = window.x + window.width - 1, end_y = window.y + window.height - 1;
    const uint8_t bytes[9] = {
        window.x >> 8, window.x & 0xFF, end_x >> 8, end_x & 0xFF,
        window.y >> 8, window.y & 0xFF, end_y >> 8, end_y & 0xFF, !window.full,
    };
    memcpy(data, bytes, sizeof(bytes));
}

// ---- Panel -------------------------------------------------------------------

static spi_device_handle_t s_spi;
static uint8_t *s_chunk;
// The last frame from the status screen, unturned, and the turned copy.
static uint8_t *s_last;
static uint8_t *s_turned;
// What the panel shows, once a refresh of it has finished.
static uint8_t *s_shown;
static bool s_shown_valid;
static bool s_have_last;
static unsigned s_turns;
static SemaphoreHandle_t s_lock;

static void pwr_key_task(void *arg);

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
    // BUSY goes low a moment after the command; wait before polling it.
    vTaskDelay(pdMS_TO_TICKS(100));
    int64_t give_up = esp_timer_get_time() + EPD_BUSY_TIMEOUT_MS * 1000LL;
    while (gpio_get_level(EPD_PIN_BUSY) == 0) {
        if (esp_timer_get_time() > give_up) return ESP_ERR_TIMEOUT;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return ESP_OK;
}

typedef struct { uint8_t cmd, len; uint8_t data[7]; } init_cmd_t;

static const init_cmd_t s_init[] = {
    {0x4D, 1, {0x78}},
    {0x00, 2, {0x0F, 0x29}},
    {0x06, 7, {0x0D, 0x12, 0x30, 0x20, 0x19, 0x2A, 0x22}},
    {0x50, 1, {0x37}},
    {0x61, 4, {EPD_W >> 8, EPD_W & 0xFF, EPD_H >> 8, EPD_H & 0xFF}},
    {0xE9, 1, {0x01}},
    {0x30, 1, {0x08}},
};

static esp_err_t epd154g_init(void) {
    // Set levels before configuring the outputs so power comes up enabled.
    gpio_set_level(BOARD_PIN_LATCH, 1);
    gpio_set_level(EPD_PIN_PWR, 0);
    const gpio_config_t power = {
        .pin_bit_mask = 1ULL << BOARD_PIN_LATCH | 1ULL << EPD_PIN_PWR,
        .mode = GPIO_MODE_OUTPUT,
    };
    esp_err_t power_err = gpio_config(&power);
    // Hold through restarts: releasing the latch would turn off battery power.
    if (power_err == ESP_OK) power_err = gpio_hold_en(BOARD_PIN_LATCH);
    if (power_err != ESP_OK) return power_err;
    vTaskDelay(pdMS_TO_TICKS(50));  // the panel supply settles
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
    s_last = heap_caps_malloc(EPD_FRAME_BYTES, MALLOC_CAP_SPIRAM);
    s_turned = heap_caps_malloc(EPD_FRAME_BYTES, MALLOC_CAP_SPIRAM);
    s_shown = heap_caps_malloc(EPD_FRAME_BYTES, MALLOC_CAP_SPIRAM);
    s_lock = xSemaphoreCreateMutex();
    if (!s_chunk || !s_last || !s_turned || !s_shown || !s_lock) return ESP_ERR_NO_MEM;
    if (err == ESP_OK) err = spi_bus_initialize(EPD_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err == ESP_OK) err = spi_bus_add_device(EPD_HOST, &dev, &s_spi);
    if (err != ESP_OK) return err;

    nvs_handle_t nvs;
    uint8_t turns = 0;
    if (nvs_open("epd154g", NVS_READONLY, &nvs) == ESP_OK) {
        nvs_get_u8(nvs, "turns", &turns);
        nvs_close(nvs);
    }
    s_turns = turns & 3;
    const gpio_config_t key = {
        .pin_bit_mask = 1ULL << BOARD_PIN_PWR_KEY,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    err = gpio_config(&key);
    if (err == ESP_OK && xTaskCreate(pwr_key_task, "epd_pwr", 3072, NULL, 2, NULL) != pdPASS) {
        err = ESP_ERR_NO_MEM;
    }
    return err;
}

#if CONFIG_HOMEHUB_EPD154G_FAST_REFRESH
// Waveshare's EPD_1IN54G_Init_Fast, which forces the temperature the waveform
// is chosen for, then R30H = 07, a fixed 120 Hz frame rate that Waveshare's
// code does not set. After power on.
static esp_err_t epd_fast_waveform(void) {
    static const uint8_t fixed = 0x02, temp = 0x5D, zero = 0x00, pll = 0x07;
    esp_err_t err = epd_cmd(0xE0, &fixed, 1);
    if (err == ESP_OK) err = epd_cmd(0xE6, &temp, 1);
    if (err == ESP_OK) err = epd_cmd(0xA5, &zero, 1);
    if (err == ESP_OK) err = epd_wait_idle();
    if (err == ESP_OK) err = epd_cmd(0x30, &pll, 1);
    return err;
}
#endif

static esp_err_t epd_show(const uint8_t *frame, bool full) {
    epd_window_t window = plan_window(full || !s_shown_valid ? NULL : s_shown, frame);
    if (!window.changed) return ESP_OK;
    // A failed refresh leaves the picture unknown: the next one is full.
    s_shown_valid = false;
    // Deep sleep ends only with a reset; use the panel's wake timings.
    gpio_set_level(EPD_PIN_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(EPD_PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_err_t err = ESP_OK;
    for (size_t i = 0; err == ESP_OK && i < sizeof(s_init) / sizeof(s_init[0]); i++) {
        err = epd_cmd(s_init[i].cmd, s_init[i].data, s_init[i].len);
    }
    static const uint8_t zero = 0x00;
    if (err == ESP_OK) err = epd_cmd(0x04, NULL, 0);
    if (err == ESP_OK) err = epd_wait_idle();
#if CONFIG_HOMEHUB_EPD154G_FAST_REFRESH
    if (err == ESP_OK) err = epd_fast_waveform();
#endif
    uint8_t area[9];
    if (!window.full) {
        // Deep sleep loses the panel's RAM: load the whole frame, then the window.
        encode_window((epd_window_t){0, 0, EPD_W, EPD_H, true, true}, area);
        if (err == ESP_OK) err = epd_cmd(0x83, area, sizeof(area));
    }
    if (err == ESP_OK) err = epd_cmd(0x10, frame, EPD_FRAME_BYTES);
    if (!window.full) {
        encode_window(window, area);
        if (err == ESP_OK) err = epd_cmd(0x83, area, sizeof(area));
        // A window refresh leaves the border floating (R50H).
        static const uint8_t border = 0x97;
        if (err == ESP_OK) err = epd_cmd(0x50, &border, 1);
    }
    int64_t start = esp_timer_get_time();
    if (err == ESP_OK) err = epd_cmd(0x12, &zero, 1);
    // A controller that never starts the refresh keeps BUSY high.
    if (err == ESP_OK) {
        int64_t started_by = esp_timer_get_time() + 1000000;
        while (gpio_get_level(EPD_PIN_BUSY) != 0 && esp_timer_get_time() < started_by) {
            vTaskDelay(pdMS_TO_TICKS(1) + 1);
        }
        if (gpio_get_level(EPD_PIN_BUSY) != 0) err = ESP_ERR_TIMEOUT;
    }
    if (err == ESP_OK) err = epd_wait_idle();
    int ms = (int)((esp_timer_get_time() - start) / 1000);
    // Power off and sleep after a failure too. A controller that does not
    // answer is held in reset until the next update.
    esp_err_t off = epd_cmd(0x02, &zero, 1);
    if (off == ESP_OK) off = epd_wait_idle();
    static const uint8_t check = 0xA5;
    if (off == ESP_OK) off = epd_cmd(0x07, &check, 1);
    if (off != ESP_OK) gpio_set_level(EPD_PIN_RST, 0);
    if (err == ESP_OK) err = off;
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "e-paper refresh failed: %s", esp_err_to_name(err));
        return err;
    }
    memcpy(s_shown, frame, EPD_FRAME_BYTES);
    s_shown_valid = true;
    if (window.full) {
        ESP_LOGI(TAG, "e-paper full refresh in %d ms", ms);
    } else {
        ESP_LOGI(TAG, "e-paper window x=%d y=%d w=%d h=%d refresh in %d ms",
                 window.x, window.y, window.width, window.height, ms);
    }
    return ESP_OK;
}

// Call with s_lock held.
static esp_err_t epd_show_last(bool full) {
    if (s_turns == 0) return epd_show(s_last, full);
    rotate_frame(s_last, s_turned, s_turns);
    return epd_show(s_turned, full);
}

static esp_err_t epd154g_update(const uint8_t *frame, const uint8_t *prev, bool full) {
    (void)prev;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memcpy(s_last, frame, EPD_FRAME_BYTES);
    s_have_last = true;
    esp_err_t err = epd_show_last(full);
    xSemaphoreGive(s_lock);
    return err;
}

// Run on the PWR task, so they must not block. A board with voice beeps on a
// hold, before the slow redraw, and shows the reply's next page on a tap.
__attribute__((weak)) void epd154g_turn_feedback(void) {}
__attribute__((weak)) void epd154g_pwr_tap(void) {}

static void pwr_key_task(void *arg) {
    (void)arg;
    pwr_key_t key = {0};
    for (;;) {
        bool pressed = gpio_get_level(BOARD_PIN_PWR_KEY) == 0;
        pwr_action_t action = pwr_key_update(&key, pressed, esp_timer_get_time());
        if (action == PWR_TAP) epd154g_pwr_tap();
        if (action == PWR_HOLD) {
            epd154g_turn_feedback();
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_turns = (s_turns + 1) & 3;
            nvs_handle_t nvs;
            if (nvs_open("epd154g", NVS_READWRITE, &nvs) == ESP_OK) {
                nvs_set_u8(nvs, "turns", (uint8_t)s_turns);
                nvs_commit(nvs);
                nvs_close(nvs);
            }
            ESP_LOGI(TAG, "e-paper turned to %u degrees", s_turns * 90);
            if (s_have_last) epd_show_last(true);
            xSemaphoreGive(s_lock);
        }
        vTaskDelay(pdMS_TO_TICKS(PWR_KEY_POLL_MS));
    }
}

static const epd_panel_t s_panel = {
    .name            = "Waveshare ESP32-S3-ePaper-1.54G",
    .depth           = "4 colours, 2 bits per pixel",
    .width           = EPD_W,
    .height          = EPD_H,
    .bits_per_pixel  = 2,
    .title_y         = 1,
    .title_max_scale = 1,
    .anim_scale      = 3,
    .anim_y          = 0,
    .status_y        = 186,
    .status_scale    = 1,
    .init            = epd154g_init,
    .update          = epd154g_update,
};

const epd_panel_t *epd_panel_get(void) { return &s_panel; }
