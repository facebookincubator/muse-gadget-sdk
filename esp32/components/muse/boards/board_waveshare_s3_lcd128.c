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
 * Waveshare ESP32-S3-Touch-LCD-1.28: ESP32-S3R2 (2 MB quad PSRAM), 16 MB
 * flash, round 240x240 GC9A01A SPI LCD, CST816S touch, QMI8658 IMU (unused),
 * battery on GPIO1 through a 200K/100K divider, CH343P USB-UART bridge on
 * UART0. No microphone or speaker: this is a display and touch gadget, and a
 * screen tap confirms pairing.
 *
 * Sources: https://www.waveshare.com/wiki/ESP32-S3-Touch-LCD-1.28 (pins,
 * backlight on GPIO2, battery divider, I2C on GPIO6/7), and
 * waveshareteam/RP2040-Touch-LCD-1.28 c/lib/LCD/LCD_1in28.c for the same
 * panel module's scan setup (B6 0x00 0x20, MADCTL 0xC8). The pin map matches
 * p8952/ESP32-S3-Touch-LCD-1.28 and IniterWorker/esp32-s3-touch-lcd-1-28.
 * Orientation of both the picture and touch was checked on hardware.
 */
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_check.h"
#include "esp_lcd_gc9a01.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch_cst816s.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "muse_board.h"
#include "muse_link.h"
#include "muse_mem.h"

static const char *TAG = "board";

#define LCD_RES 240
#define LCD_HOST SPI2_HOST
#define LCD_DC GPIO_NUM_8
#define LCD_CS GPIO_NUM_9
#define LCD_SCLK GPIO_NUM_10
#define LCD_MOSI GPIO_NUM_11
#define LCD_RST GPIO_NUM_14
#define LCD_BL GPIO_NUM_2
#define DRAW_BUF_LINES 40

#define I2C_SDA GPIO_NUM_6   /* shared by the CST816S and the QMI8658 */
#define I2C_SCL GPIO_NUM_7
#define TOUCH_INT GPIO_NUM_5 /* active low; RTC-capable, so it wakes deep sleep */
#define TOUCH_RST GPIO_NUM_13

#define BATT_ADC ADC_CHANNEL_0   /* GPIO1 */

static i2c_master_bus_handle_t s_i2c;
static esp_lcd_panel_handle_t s_panel;
static esp_lcd_panel_io_handle_t s_io;
static esp_lcd_touch_handle_t s_tp;
static lv_indev_t *s_touch_indev;
static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cali;

static esp_err_t init(void)
{
    const i2c_master_bus_config_t i2c_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = I2C_SDA,
        .scl_io_num = I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&i2c_cfg, &s_i2c), TAG, "i2c");

    const adc_oneshot_unit_init_cfg_t adc_cfg = { .unit_id = ADC_UNIT_1 };
    ESP_RETURN_ON_ERROR(adc_oneshot_new_unit(&adc_cfg, &s_adc), TAG, "adc");
    const adc_oneshot_chan_cfg_t ch_cfg = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_12 };
    ESP_RETURN_ON_ERROR(adc_oneshot_config_channel(s_adc, BATT_ADC, &ch_cfg), TAG, "adc channel");
    const adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id = ADC_UNIT_1,
        .chan = BATT_ADC,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    if (adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_cali) != ESP_OK) {
        ESP_LOGW(TAG, "adc calibration unavailable; battery mV is uncalibrated");
    }
    return ESP_OK;
}

static lv_display_t *display_start(lv_indev_t **touch)
{
    const ledc_timer_config_t bl_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 20000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    const ledc_channel_config_t bl_ch = {
        .gpio_num = LCD_BL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
    };
    if (ledc_timer_config(&bl_timer) != ESP_OK || ledc_channel_config(&bl_ch) != ESP_OK) {
        ESP_LOGE(TAG, "backlight pwm");
        return NULL;
    }

    const spi_bus_config_t bus = {
        .sclk_io_num = LCD_SCLK,
        .mosi_io_num = LCD_MOSI,
        .miso_io_num = GPIO_NUM_NC,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = LCD_RES * DRAW_BUF_LINES * 2,
    };
    if (spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) {
        ESP_LOGE(TAG, "spi bus");
        return NULL;
    }
    const esp_lcd_panel_io_spi_config_t io_cfg = GC9A01_PANEL_IO_SPI_CONFIG(LCD_CS, LCD_DC, NULL, NULL);
    if (esp_lcd_new_panel_io_spi(LCD_HOST, &io_cfg, &s_io) != ESP_OK) {
        ESP_LOGE(TAG, "panel io");
        return NULL;
    }
    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
    };
    if (esp_lcd_new_panel_gc9a01(s_io, &panel_cfg, &s_panel) != ESP_OK) {
        ESP_LOGE(TAG, "gc9a01");
        return NULL;
    }
    esp_lcd_panel_reset(s_panel);
    esp_lcd_panel_init(s_panel);
    esp_lcd_panel_invert_color(s_panel, true);
    /* Waveshare's scan setup: Display Function Control 0x20 reverses the
     * source scan, which the driver's init leaves at its reset value (the
     * picture shows mirrored without it), then MADCTL 0xC8 (MX | MY | BGR). */
    esp_lcd_panel_io_tx_param(s_io, 0xB6, (uint8_t[]){ 0x00, 0x20 }, 2);
    esp_lcd_panel_mirror(s_panel, true, true);
    esp_lcd_panel_disp_on_off(s_panel, true);

    esp_lv_adapter_config_t adapter_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    adapter_cfg.task_core_id = MUSE_UI_CORE;
    adapter_cfg.task_priority = MUSE_UI_PRIORITY;
    if (esp_lv_adapter_init(&adapter_cfg) != ESP_OK) {
        return NULL;
    }
    const esp_lv_adapter_display_config_t disp_cfg = {
        .panel = s_panel,
        .panel_io = s_io,
        .profile = {
            .interface = ESP_LV_ADAPTER_PANEL_IF_OTHER,
            .rotation = ESP_LV_ADAPTER_ROTATE_0,
            .hor_res = LCD_RES,
            .ver_res = LCD_RES,
            .buffer_height = DRAW_BUF_LINES,
            .use_psram = false,
            .require_double_buffer = true,
        },
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE,
    };
    lv_display_t *disp = esp_lv_adapter_register_display(&disp_cfg);
    if (!disp) {
        return NULL;
    }

    esp_lcd_panel_io_handle_t tp_io;
    const esp_lcd_panel_io_i2c_config_t tp_io_cfg = ESP_LCD_TOUCH_IO_I2C_CST816S_CONFIG();
    if (esp_lcd_new_panel_io_i2c(s_i2c, &tp_io_cfg, &tp_io) != ESP_OK) {
        ESP_LOGE(TAG, "touch io");
        return NULL;
    }
    const esp_lcd_touch_config_t tp_cfg = {
        .x_max = LCD_RES,
        .y_max = LCD_RES,
        .rst_gpio_num = TOUCH_RST,
        .int_gpio_num = TOUCH_INT,
        .levels = { .reset = 0, .interrupt = 0 },
        /* Against the picture as scanned above, the CST816S frame is turned
         * 180 degrees. */
        .flags = { .mirror_x = 1, .mirror_y = 1 },
    };
    if (esp_lcd_touch_new_i2c_cst816s(tp_io, &tp_cfg, &s_tp) != ESP_OK) {
        ESP_LOGE(TAG, "cst816s");
        return NULL;
    }
    const esp_lv_adapter_touch_config_t tp_lvgl_cfg = ESP_LV_ADAPTER_TOUCH_DEFAULT_CONFIG(disp, s_tp);
    *touch = esp_lv_adapter_register_touch(&tp_lvgl_cfg);
    if (!*touch || esp_lv_adapter_start() != ESP_OK) {
        return NULL;
    }
    s_touch_indev = *touch;
    return disp;
}

static bool display_lock(int timeout_ms)
{
    return esp_lv_adapter_lock(timeout_ms) == ESP_OK;
}

static void set_brightness(int pct)
{
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, pct * 1023 / 100);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

/* The GC9A01 driver doesn't implement disp_sleep, so send SLPIN/SLPOUT
 * ourselves. GRAM is kept. */
static void panel_sleep(bool sleep)
{
    esp_lcd_panel_io_tx_param(s_io, sleep ? 0x10 : 0x11, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(120));   /* settle before the next command */
}

/*
 * No buttons reach the UI and there's no microphone, so the only talk edge is
 * a screen tap while the Muse app waits for pairing confirmation. Touch state
 * comes from LVGL's input device, which owns the CST816S handle.
 */
static unsigned poll_buttons(void)
{
    static bool down;
    static bool confirming;
    bool pressed = s_touch_indev && lv_indev_get_state(s_touch_indev) == LV_INDEV_STATE_PRESSED;
    unsigned ev = 0;
    if (pressed && !down && muse_link_state() == MUSE_LINK_CONFIRM) {
        confirming = true;
        ev |= MUSE_BTN_TALK_PRESS;
    } else if (!pressed && down && confirming) {
        confirming = false;
        ev |= MUSE_BTN_TALK_RELEASE;
    }
    down = pressed;
    return ev;
}

/* GPIO1 sees the battery through a 200K/100K divider (Waveshare wiki). */
static esp_err_t read_power(muse_power_t *out)
{
    int sum = 0;
    for (int i = 0; i < 8; i++) {
        int v;
        ESP_RETURN_ON_ERROR(adc_oneshot_read(s_adc, BATT_ADC, &v), TAG, "adc read");
        sum += v;
    }
    int raw = sum / 8;
    int mv;
    if (s_cali) {
        ESP_RETURN_ON_ERROR(adc_cali_raw_to_voltage(s_cali, raw, &mv), TAG, "adc cali");
    } else {
        mv = raw * 3300 / 4096;
    }
    out->battery_mv = mv * 3;
    /* No percentage until the meter is checked with a cell: on USB alone the
     * divider reads the charger's ~4.2 V, which looks like a full cell, and
     * nothing tells USB apart, so a percentage would run the battery policy
     * (light sleep, battery UI) while plugged in. The voltage stays for
     * diagnostics. */
    out->battery_pct = -1;
    /* No charge-state or VBUS line reaches the ESP32, and the console is on
     * the CH343P rather than the chip's own USB. */
    out->charging = false;
    out->usb = false;
    return ESP_OK;
}

static esp_err_t power_off(void)
{
    set_brightness(0);
    esp_lcd_panel_disp_on_off(s_panel, false);
    panel_sleep(true);
    /* No PMU: deep sleep is the closest thing to off, and a touch wakes it. */
    ESP_RETURN_ON_ERROR(esp_sleep_enable_ext0_wakeup(TOUCH_INT, 0), TAG, "touch wake");
    esp_deep_sleep_start();
    return ESP_FAIL;
}

static const muse_board_t s_board = {
    .name = "Waveshare ESP32-S3-Touch-LCD-1.28",
    .width = LCD_RES,
    .height = LCD_RES,
    .round = true,
    .touch = true,
    .diagonal_in = 1.28f,
    .talk_button = "screen",
    .frame_ms = 40,
    .init = init,
    .display_start = display_start,
    .display_lock = display_lock,
    .display_unlock = esp_lv_adapter_unlock,
    .set_brightness = set_brightness,
    .panel_sleep = panel_sleep,
    .poll_buttons = poll_buttons,
    .read_power = read_power,
    .power_off = power_off,
};

/* Home Link's app_main starts Muse with this board (main/main.c). */
const muse_board_t *muse_board_get(void)
{
    return &s_board;
}
