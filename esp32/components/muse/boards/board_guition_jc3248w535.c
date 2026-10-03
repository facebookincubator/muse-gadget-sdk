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

/*
 * Guition JC3248W535 (JC3248W535C_I_Y): ESP32-S3-WROOM-1 with 16 MB flash and
 * 8 MB octal PSRAM, 3.5" 320x480 IPS LCD on an AXS15231B over QSPI with the
 * controller's own capacitive touch on I2C, an NS4168 mono I2S amp for an
 * external speaker, and the BOOT button. No microphone and no battery gauge.
 *
 * Pins: LCD and touch from me-processware/JC3248W535-Driver
 * (src/JC3248W535_Display.h, src/JC3248W535_Touch.h), audio from
 * sirisakG2/JC3248W535C. The panel's init sequence is Arduino_GFX 1.4.9's
 * axs15231b_init_operations, which is what runs this board under Arduino.
 *
 * In QSPI mode the AXS15231B ignores the row address: a write lands at the
 * top of the screen (RAMWR, which esp_lcd_axs15231b sends when y is 0) or
 * straight after the previous one (RAMWRC). So LVGL renders in direct mode
 * into one full-screen buffer and every frame goes out whole, top to bottom.
 *
 * Push-to-talk needs an I2S MEMS mic (INMP441 or similar) wired to the
 * speaker's clocks, BCLK GPIO42 and WS GPIO2, with L/R to GND and SD on
 * CONFIG_MUSE_JC3248W535_MIC_GPIO. Without one the mic reads silence.
 */
#include <math.h>
#include <string.h>

#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_heap_caps.h"
#include "esp_lcd_axs15231b.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "muse_audio.h"
#include "muse_board.h"
#include "muse_mem.h"

static const char *TAG = "board";

#define LCD_W 320
#define LCD_H 480
#define LCD_HOST SPI2_HOST
#define LCD_CS GPIO_NUM_45
#define LCD_SCLK GPIO_NUM_47
#define LCD_D0 GPIO_NUM_21
#define LCD_D1 GPIO_NUM_48
#define LCD_D2 GPIO_NUM_40
#define LCD_D3 GPIO_NUM_39
#define LCD_BL GPIO_NUM_1          /* active high */
#define CHUNK_ROWS 20              /* rows per QSPI transfer, out of internal RAM */
#define CHUNK_BYTES (LCD_W * CHUNK_ROWS * 2)

#define TP_SDA GPIO_NUM_4
#define TP_SCL GPIO_NUM_8
#define TP_ADDR 0x3B

#define I2S_BCLK GPIO_NUM_42
#define I2S_WS GPIO_NUM_2
#define I2S_DOUT GPIO_NUM_41       /* NS4168 */
#define TALK_GPIO GPIO_NUM_0       /* BOOT */

/* An INMP441 is about -26 dBFS at 94 dB SPL; Muse's default 30 dB of gain
 * brings speech up to where the ES8311 boards have it. */
#define MIC_GAIN_OFFSET_DB 0

static const axs15231b_lcd_init_cmd_t s_lcd_init[] = {
    {0xBB, (uint8_t[]){0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x5A, 0xA5}, 8, 0},
    {0xA0, (uint8_t[]){0xC0, 0x10, 0x00, 0x02, 0x00, 0x00, 0x04, 0x3F, 0x20, 0x05, 0x3F, 0x3F, 0x00, 0x00, 0x00, 0x00, 0x00}, 17, 0},
    {0xA2, (uint8_t[]){0x30, 0x3C, 0x24, 0x14, 0xD0, 0x20, 0xFF, 0xE0, 0x40, 0x19, 0x80, 0x80, 0x80, 0x20, 0xF9, 0x10, 0x02, 0xFF, 0xFF, 0xF0, 0x90, 0x01, 0x32, 0xA0, 0x91, 0xE0, 0x20, 0x7F, 0xFF, 0x00, 0x5A}, 31, 0},
    {0xD0, (uint8_t[]){0xE0, 0x40, 0x51, 0x24, 0x08, 0x05, 0x10, 0x01, 0x20, 0x15, 0xC2, 0x42, 0x22, 0x22, 0xAA, 0x03, 0x10, 0x12, 0x60, 0x14, 0x1E, 0x51, 0x15, 0x00, 0x8A, 0x20, 0x00, 0x03, 0x3A, 0x12}, 30, 0},
    {0xA3, (uint8_t[]){0xA0, 0x06, 0xAA, 0x00, 0x08, 0x02, 0x0A, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x00, 0x55, 0x55}, 22, 0},
    {0xC1, (uint8_t[]){0x31, 0x04, 0x02, 0x02, 0x71, 0x05, 0x24, 0x55, 0x02, 0x00, 0x41, 0x00, 0x53, 0xFF, 0xFF, 0xFF, 0x4F, 0x52, 0x00, 0x4F, 0x52, 0x00, 0x45, 0x3B, 0x0B, 0x02, 0x0D, 0x00, 0xFF, 0x40}, 30, 0},
    {0xC3, (uint8_t[]){0x00, 0x00, 0x00, 0x50, 0x03, 0x00, 0x00, 0x00, 0x01, 0x80, 0x01}, 11, 0},
    {0xC4, (uint8_t[]){0x00, 0x24, 0x33, 0x80, 0x00, 0xEA, 0x64, 0x32, 0xC8, 0x64, 0xC8, 0x32, 0x90, 0x90, 0x11, 0x06, 0xDC, 0xFA, 0x00, 0x00, 0x80, 0xFE, 0x10, 0x10, 0x00, 0x0A, 0x0A, 0x44, 0x50}, 29, 0},
    {0xC5, (uint8_t[]){0x18, 0x00, 0x00, 0x03, 0xFE, 0x3A, 0x4A, 0x20, 0x30, 0x10, 0x88, 0xDE, 0x0D, 0x08, 0x0F, 0x0F, 0x01, 0x3A, 0x4A, 0x20, 0x10, 0x10, 0x00}, 23, 0},
    {0xC6, (uint8_t[]){0x05, 0x0A, 0x05, 0x0A, 0x00, 0xE0, 0x2E, 0x0B, 0x12, 0x22, 0x12, 0x22, 0x01, 0x03, 0x00, 0x3F, 0x6A, 0x18, 0xC8, 0x22}, 20, 0},
    {0xC7, (uint8_t[]){0x50, 0x32, 0x28, 0x00, 0xA2, 0x80, 0x8F, 0x00, 0x80, 0xFF, 0x07, 0x11, 0x9C, 0x67, 0xFF, 0x24, 0x0C, 0x0D, 0x0E, 0x0F}, 20, 0},
    {0xC9, (uint8_t[]){0x33, 0x44, 0x44, 0x01}, 4, 0},
    {0xCF, (uint8_t[]){0x2C, 0x1E, 0x88, 0x58, 0x13, 0x18, 0x56, 0x18, 0x1E, 0x68, 0x88, 0x00, 0x65, 0x09, 0x22, 0xC4, 0x0C, 0x77, 0x22, 0x44, 0xAA, 0x55, 0x08, 0x08, 0x12, 0xA0, 0x08}, 27, 0},
    {0xD5, (uint8_t[]){0x40, 0x8E, 0x8D, 0x01, 0x35, 0x04, 0x92, 0x74, 0x04, 0x92, 0x74, 0x04, 0x08, 0x6A, 0x04, 0x46, 0x03, 0x03, 0x03, 0x03, 0x82, 0x01, 0x03, 0x00, 0xE0, 0x51, 0xA1, 0x00, 0x00, 0x00}, 30, 0},
    {0xD6, (uint8_t[]){0x10, 0x32, 0x54, 0x76, 0x98, 0xBA, 0xDC, 0xFE, 0x93, 0x00, 0x01, 0x83, 0x07, 0x07, 0x00, 0x07, 0x07, 0x00, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x00, 0x84, 0x00, 0x20, 0x01, 0x00}, 30, 0},
    {0xD7, (uint8_t[]){0x03, 0x01, 0x0B, 0x09, 0x0F, 0x0D, 0x1E, 0x1F, 0x18, 0x1D, 0x1F, 0x19, 0x40, 0x8E, 0x04, 0x00, 0x20, 0xA0, 0x1F}, 19, 0},
    {0xD8, (uint8_t[]){0x02, 0x00, 0x0A, 0x08, 0x0E, 0x0C, 0x1E, 0x1F, 0x18, 0x1D, 0x1F, 0x19}, 12, 0},
    {0xD9, (uint8_t[]){0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F}, 12, 0},
    {0xDD, (uint8_t[]){0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F}, 12, 0},
    {0xDF, (uint8_t[]){0x44, 0x73, 0x4B, 0x69, 0x00, 0x0A, 0x02, 0x90}, 8, 0},
    {0xE0, (uint8_t[]){0x3B, 0x28, 0x10, 0x16, 0x0C, 0x06, 0x11, 0x28, 0x5C, 0x21, 0x0D, 0x35, 0x13, 0x2C, 0x33, 0x28, 0x0D}, 17, 0},
    {0xE1, (uint8_t[]){0x37, 0x28, 0x10, 0x16, 0x0B, 0x06, 0x11, 0x28, 0x5C, 0x21, 0x0D, 0x35, 0x14, 0x2C, 0x33, 0x28, 0x0F}, 17, 0},
    {0xE2, (uint8_t[]){0x3B, 0x07, 0x12, 0x18, 0x0E, 0x0D, 0x17, 0x35, 0x44, 0x32, 0x0C, 0x14, 0x14, 0x36, 0x3A, 0x2F, 0x0D}, 17, 0},
    {0xE3, (uint8_t[]){0x37, 0x07, 0x12, 0x18, 0x0E, 0x0D, 0x17, 0x35, 0x44, 0x32, 0x0C, 0x14, 0x14, 0x36, 0x32, 0x2F, 0x0F}, 17, 0},
    {0xE4, (uint8_t[]){0x3B, 0x07, 0x12, 0x18, 0x0E, 0x0D, 0x17, 0x39, 0x44, 0x2E, 0x0C, 0x14, 0x14, 0x36, 0x3A, 0x2F, 0x0D}, 17, 0},
    {0xE5, (uint8_t[]){0x37, 0x07, 0x12, 0x18, 0x0E, 0x0D, 0x17, 0x39, 0x44, 0x2E, 0x0C, 0x14, 0x14, 0x36, 0x3A, 0x2F, 0x0F}, 17, 0},
    {0xA4, (uint8_t[]){0x85, 0x85, 0x95, 0x82, 0xAF, 0xAA, 0xAA, 0x80, 0x10, 0x30, 0x40, 0x40, 0x20, 0xFF, 0x60, 0x30}, 16, 0},
    {0xA4, (uint8_t[]){0x85, 0x85, 0x95, 0x85}, 4, 0},
    {0xBB, (uint8_t[]){0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, 8, 0},
    {0x11, (uint8_t[]){0x00}, 0, 120},
    {0x29, (uint8_t[]){0x00}, 0, 100},
    {0x2C, (uint8_t[]){0x00, 0x00, 0x00, 0x00}, 4, 0},
};

static esp_lcd_panel_handle_t s_panel;
static i2c_master_dev_handle_t s_tp;
static lv_display_t *s_disp;
static lv_indev_t *s_indev;
static uint8_t *s_fb;                  /* the whole screen, in PSRAM */
static uint8_t *s_chunk[2];            /* DMA-capable copies on their way out */
static SemaphoreHandle_t s_chunk_free;
static SemaphoreHandle_t s_lv_lock;
static SemaphoreHandle_t s_started;
static esp_err_t s_start_err;
static muse_gpio_button_t s_talk;
static i2s_chan_handle_t s_tx, s_rx;
static int s_mic_gain_q8 = 256;
static int32_t s_mic_dc[2];            /* each slot's zero level, x256 */

static esp_err_t init(void)
{
    ESP_RETURN_ON_ERROR(muse_gpio_button_init(&s_talk, TALK_GPIO), TAG, "talk button");
    /* Held at reset to get here from the bootloader; don't count that as a press. */
    s_talk.pressed = gpio_get_level(TALK_GPIO) == 0;
    return ESP_OK;
}

static bool IRAM_ATTR on_chunk_sent(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *edata, void *ctx)
{
    (void)io;
    (void)edata;
    (void)ctx;
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_chunk_free, &woken);
    return woken == pdTRUE;
}

/*
 * Direct mode: LVGL redraws only what changed, in place in s_fb, and calls
 * this once per area. After the last one the whole frame goes out from row 0,
 * since the panel can't be sent a window lower down. Each piece is copied
 * into internal RAM while the one before is on the wire (the SPI DMA can't
 * keep up reading PSRAM while both cores draw).
 */
static void flush(lv_display_t *disp, const lv_area_t *area, uint8_t *px)
{
    (void)area;
    (void)px;
    if (lv_display_flush_is_last(disp)) {
        int k = 0;
        for (int y = 0; y < LCD_H; y += CHUNK_ROWS) {
            xSemaphoreTake(s_chunk_free, portMAX_DELAY);
            memcpy(s_chunk[k], s_fb + (size_t)y * LCD_W * 2, CHUNK_BYTES);
            if (esp_lcd_panel_draw_bitmap(s_panel, 0, y, LCD_W, y + CHUNK_ROWS, s_chunk[k]) != ESP_OK) {
                xSemaphoreGive(s_chunk_free);
            }
            k ^= 1;
        }
        /* Both pieces back: the frame is on the panel and s_fb is LVGL's again. */
        xSemaphoreTake(s_chunk_free, portMAX_DELAY);
        xSemaphoreTake(s_chunk_free, portMAX_DELAY);
        xSemaphoreGive(s_chunk_free);
        xSemaphoreGive(s_chunk_free);
    }
    lv_display_flush_ready(disp);
}

/* The AXS15231B's touch answers this 11-byte read command with 8 bytes:
 * gesture, point count, then X and Y in 12 bits each. */
static void touch_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    static const uint8_t cmd[11] = { 0xB5, 0xAB, 0xA5, 0x5A, 0x00, 0x00, 0x00, 0x08 };
    uint8_t buf[8];
    data->state = LV_INDEV_STATE_RELEASED;
    if (i2c_master_transmit(s_tp, cmd, sizeof(cmd), 20) != ESP_OK ||
        i2c_master_receive(s_tp, buf, sizeof(buf), 20) != ESP_OK || buf[0] != 0 || buf[1] == 0) {
        return;
    }
    int x = (buf[2] & 0x0F) << 8 | buf[3];
    int y = (buf[4] & 0x0F) << 8 | buf[5];
    data->point.x = x < LCD_W ? x : LCD_W - 1;
    data->point.y = y < LCD_H ? y : LCD_H - 1;
    data->state = LV_INDEV_STATE_PRESSED;
}

static uint32_t tick_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static esp_err_t touch_start(void)
{
    i2c_master_bus_handle_t bus;
    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = TP_SDA,
        .scl_io_num = TP_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &bus), TAG, "touch i2c");
    const i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = TP_ADDR,
        .scl_speed_hz = 400000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &dev_cfg, &s_tp), TAG, "touch device");
    return i2c_master_probe(bus, TP_ADDR, 50);
}

/* Runs on the LVGL task, so the panel's SPI interrupt lands on its core
 * (see muse_lcd_bands.h for why that matters). */
static esp_err_t lcd_start(void)
{
    const ledc_timer_config_t bl_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    const ledc_channel_config_t bl_ch = {
        .gpio_num = LCD_BL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&bl_timer), TAG, "backlight timer");
    ESP_RETURN_ON_ERROR(ledc_channel_config(&bl_ch), TAG, "backlight");

    s_fb = heap_caps_malloc(LCD_W * LCD_H * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_chunk[0] = heap_caps_malloc(CHUNK_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    s_chunk[1] = heap_caps_malloc(CHUNK_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    s_chunk_free = xSemaphoreCreateCounting(2, 2);
    ESP_RETURN_ON_FALSE(s_fb && s_chunk[0] && s_chunk[1] && s_chunk_free, ESP_ERR_NO_MEM, TAG, "frame buffers");
    memset(s_fb, 0, LCD_W * LCD_H * 2);

    const spi_bus_config_t bus = AXS15231B_PANEL_BUS_QSPI_CONFIG(LCD_SCLK, LCD_D0, LCD_D1, LCD_D2, LCD_D3, CHUNK_BYTES);
    ESP_RETURN_ON_ERROR(spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO), TAG, "lcd bus");
    esp_lcd_panel_io_handle_t io;
    const esp_lcd_panel_io_spi_config_t io_cfg = AXS15231B_PANEL_IO_QSPI_CONFIG(LCD_CS, on_chunk_sent, NULL);
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi(LCD_HOST, &io_cfg, &io), TAG, "lcd io");
    axs15231b_vendor_config_t vendor_cfg = {
        .init_cmds = s_lcd_init,
        .init_cmds_size = sizeof(s_lcd_init) / sizeof(s_lcd_init[0]),
        .flags.use_qspi_interface = 1,
    };
    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = GPIO_NUM_NC,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor_cfg,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_axs15231b(io, &panel_cfg, &s_panel), TAG, "lcd panel");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "lcd reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "lcd init");

    lv_init();
    lv_tick_set_cb(tick_ms);
    s_disp = lv_display_create(LCD_W, LCD_H);
    ESP_RETURN_ON_FALSE(s_disp, ESP_ERR_NO_MEM, TAG, "lv display");
    /* The panel takes RGB565 big-endian over SPI. */
    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565_SWAPPED);
    lv_display_set_buffers(s_disp, s_fb, NULL, LCD_W * LCD_H * 2, LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(s_disp, flush);

    if (touch_start() == ESP_OK) {
        s_indev = lv_indev_create();
        lv_indev_set_type(s_indev, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(s_indev, touch_read);
        lv_indev_set_display(s_indev, s_disp);
    } else {
        ESP_LOGW(TAG, "touch controller not answering at 0x%02x", TP_ADDR);
    }
    return ESP_OK;
}

static void lvgl_task(void *arg)
{
    (void)arg;
    s_start_err = lcd_start();
    xSemaphoreGive(s_started);
    if (s_start_err != ESP_OK) {
        vTaskDelete(NULL);
    }
    for (;;) {
        xSemaphoreTakeRecursive(s_lv_lock, portMAX_DELAY);
        uint32_t ms = lv_timer_handler();
        xSemaphoreGiveRecursive(s_lv_lock);
        ms = ms < 5 ? 5 : ms > 50 ? 50 : ms;
        vTaskDelay(pdMS_TO_TICKS(ms) ? pdMS_TO_TICKS(ms) : 1);
    }
}

static lv_display_t *display_start(lv_indev_t **touch)
{
    s_lv_lock = xSemaphoreCreateRecursiveMutex();
    s_started = xSemaphoreCreateBinary();
    if (!s_lv_lock || !s_started ||
        xTaskCreatePinnedToCoreWithCaps(lvgl_task, "lvgl", 8192, NULL, MUSE_UI_PRIORITY, NULL, MUSE_UI_CORE,
                                        MUSE_BIG_CAPS) != pdPASS) {
        return NULL;
    }
    xSemaphoreTake(s_started, portMAX_DELAY);
    if (s_start_err != ESP_OK) {
        return NULL;
    }
    *touch = s_indev;
    return s_disp;
}

static bool display_lock(int timeout_ms)
{
    return xSemaphoreTakeRecursive(s_lv_lock, timeout_ms < 0 ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

static void display_unlock(void)
{
    xSemaphoreGiveRecursive(s_lv_lock);
}

static void set_brightness(int pct)
{
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, pct * 1023 / 100);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

/* No codec: the NS4168 takes I2S as is, esp_codec_dev adds the volume in
 * software, and the mic gain is applied here. Both channels run all the time;
 * idle, the speaker sends zeros (auto_clear). */
static int data_enable(const audio_codec_data_if_t *h, esp_codec_dev_type_t type, bool on)
{
    (void)h;
    (void)type;
    (void)on;
    return ESP_CODEC_DEV_OK;
}

static int spk_write(const audio_codec_data_if_t *h, uint8_t *data, int size)
{
    (void)h;
    size_t wrote;
    return i2s_channel_write(s_tx, data, size, &wrote, portMAX_DELAY) == ESP_OK ? ESP_CODEC_DEV_OK
                                                                                : ESP_CODEC_DEV_WRITE_FAIL;
}

/* A MEMS mic's slot has a small DC offset, which the gain would turn into
 * clipping: each slot's zero level is tracked and taken off first. */
static int mic_read(const audio_codec_data_if_t *h, uint8_t *data, int size)
{
    (void)h;
    size_t got;
    if (i2s_channel_read(s_rx, data, size, &got, pdMS_TO_TICKS(1000)) != ESP_OK || got != (size_t)size) {
        return ESP_CODEC_DEV_READ_FAIL;
    }
    int16_t *s = (int16_t *)data;
    for (int i = 0; i < size / 2; i++) {
        int32_t *dc = &s_mic_dc[i & 1];
        *dc += (s[i] * 256 - *dc) >> 8;
        int v = (s[i] - (*dc >> 8)) * s_mic_gain_q8 >> 8;
        s[i] = v > INT16_MAX ? INT16_MAX : v < INT16_MIN ? INT16_MIN : v;
    }
    return ESP_CODEC_DEV_OK;
}

static void set_mic_gain(esp_codec_dev_handle_t mic, int db)
{
    (void)mic;
    s_mic_gain_q8 = (int)(256.0f * powf(10.0f, (db - MIC_GAIN_OFFSET_DB) / 20.0f));
}

static esp_err_t audio_init(esp_codec_dev_handle_t *spk, esp_codec_dev_handle_t *mic)
{
    const int din = CONFIG_MUSE_JC3248W535_MIC_GPIO;
    if (din < 0) {
        ESP_LOGW(TAG, "no mic (CONFIG_MUSE_JC3248W535_MIC_GPIO): push-to-talk records silence");
    }
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_tx, &s_rx), TAG, "i2s channel");
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(MUSE_AUDIO_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_BCLK,
            .ws = I2S_WS,
            .dout = I2S_DOUT,
            .din = din < 0 ? I2S_GPIO_UNUSED : (gpio_num_t)din,
        },
    };
    /* 32-bit slots (64 clocks a frame), which an INMP441 needs; the NS4168
     * takes them too. Each sample is the slot's top 16 bits. */
    std_cfg.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_32BIT;
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx, &std_cfg), TAG, "i2s tx");
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_rx, &std_cfg), TAG, "i2s rx");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx), TAG, "i2s tx on");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_rx), TAG, "i2s rx on");

    static const audio_codec_data_if_t spk_if = { .enable = data_enable, .write = spk_write };
    static const audio_codec_data_if_t mic_if = { .enable = data_enable, .read = mic_read };
    esp_codec_dev_cfg_t out_cfg = { .dev_type = ESP_CODEC_DEV_TYPE_OUT, .data_if = &spk_if };
    esp_codec_dev_cfg_t in_cfg = { .dev_type = ESP_CODEC_DEV_TYPE_IN, .data_if = &mic_if };
    *spk = esp_codec_dev_new(&out_cfg);
    *mic = esp_codec_dev_new(&in_cfg);
    return *spk && *mic ? ESP_OK : ESP_FAIL;
}

static unsigned poll_buttons(void)
{
    return muse_gpio_button_poll(&s_talk);
}

/* No power switch or latch: the screen goes dark and the chip sleeps until
 * BOOT is pressed. On USB it is still powered. */
static esp_err_t power_off(void)
{
    set_brightness(0);
    while (gpio_get_level(TALK_GPIO) == 0) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    esp_sleep_enable_ext0_wakeup(TALK_GPIO, 0);
    esp_deep_sleep_start();
    return ESP_FAIL;
}

static const muse_board_t s_board = {
    .name = "Guition JC3248W535",
    .width = LCD_W,
    .height = LCD_H,
    .round = false,
    .touch = true,
    .diagonal_in = 3.5f,
    /* BOOT is the only button, so it also wakes the screen and the chip. */
    .talk_button = "boot",
    .aux_button = "boot",
    .talk_hint = { LV_ALIGN_BOTTOM_RIGHT, -16, -8 },
    .frame_ms = 40,
    .init = init,
    .display_start = display_start,
    .display_lock = display_lock,
    .display_unlock = display_unlock,
    .set_brightness = set_brightness,
    .audio_init = audio_init,
    .mic_slot = 0,              /* INMP441 with L/R to GND: the left slot */
    .set_mic_gain = set_mic_gain,
    .poll_buttons = poll_buttons,
    .power_off = power_off,
};

/* Home Link's app_main starts Muse with this board (main/main.c). */
const muse_board_t *muse_board_get(void)
{
    return &s_board;
}
