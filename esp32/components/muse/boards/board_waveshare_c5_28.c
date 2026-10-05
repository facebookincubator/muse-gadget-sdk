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
 * Waveshare ESP32-C5-Touch-LCD-2.8: ST7789, CST3530 touch, ES8311 duplex
 * audio, and a CH32V003 expander for reset, backlight and amplifier enable.
 * Pins and reset sequences: https://github.com/waveshareteam/ESP32-C5-Touch-LCD-2.8
 * example/ESP-IDF-V554/02_2048/components/esp32_c5_touch_lcd_2_8/.
 * The CH32's factory firmware is retained. BOOT talks; settings use touch.
 */
#include "custom_io_expander_ch32v003.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_codec_dev_defaults.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_touch_cst3530.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "muse_audio.h"
#include "muse_board.h"
#include "muse_mem.h"

static const char *TAG = "board";

#define LCD_WIDTH 240
#define LCD_HEIGHT 320
#define DRAW_BUF_LINES 32
#define LCD_HOST SPI2_HOST
#define LCD_SCLK GPIO_NUM_6
#define LCD_MOSI GPIO_NUM_7
#define LCD_DC GPIO_NUM_9
#define LCD_CS GPIO_NUM_10
#define SD_CS GPIO_NUM_23
#define TOUCH_INT GPIO_NUM_5
#define BOOT_GPIO GPIO_NUM_28
#define I2C_SDA GPIO_NUM_0
#define I2C_SCL GPIO_NUM_1
#define I2S_BCLK GPIO_NUM_24
#define I2S_WS GPIO_NUM_25
#define I2S_DOUT GPIO_NUM_26
#define I2S_DIN GPIO_NUM_27
#define EX_TOUCH_RST IO_EXPANDER_PIN_NUM_0
#define EX_LCD_RST IO_EXPANDER_PIN_NUM_1
#define EX_PA_EN IO_EXPANDER_PIN_NUM_3

static i2c_master_bus_handle_t s_i2c;
static esp_io_expander_handle_t s_expander;
static esp_lcd_panel_handle_t s_panel;
static muse_gpio_button_t s_boot;

static esp_err_t init(void)
{
    const i2c_master_bus_config_t cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = I2C_SDA,
        .scl_io_num = I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&cfg, &s_i2c), TAG, "i2c");
    ESP_RETURN_ON_ERROR(custom_io_expander_new_i2c_ch32v003(
        s_i2c, CUSTOM_IO_EXPANDER_I2C_CH32V003_ADDRESS, &s_expander), TAG, "CH32V003");
    ESP_RETURN_ON_ERROR(esp_io_expander_set_dir(s_expander,
        EX_TOUCH_RST | EX_LCD_RST | EX_PA_EN, IO_EXPANDER_OUTPUT), TAG, "expander direction");
    ESP_RETURN_ON_ERROR(esp_io_expander_set_level(s_expander,
        EX_TOUCH_RST | EX_LCD_RST, 1), TAG, "release reset");
    ESP_RETURN_ON_ERROR(esp_io_expander_set_level(s_expander, EX_PA_EN, 0), TAG, "amplifier off");
    ESP_RETURN_ON_ERROR(custom_io_expander_set_pwm(s_expander, 0), TAG, "backlight off");
    /* The SD slot shares the LCD SPI bus. Keep an inserted card deselected. */
    const gpio_config_t sd = { .pin_bit_mask = 1ULL << SD_CS, .mode = GPIO_MODE_OUTPUT };
    ESP_RETURN_ON_ERROR(gpio_config(&sd), TAG, "SD CS");
    ESP_RETURN_ON_ERROR(gpio_set_level(SD_CS, 1), TAG, "SD deselect");
    return muse_gpio_button_init(&s_boot, BOOT_GPIO);
}

static esp_err_t reset_pin(uint32_t pin, unsigned delay_ms)
{
    ESP_RETURN_ON_ERROR(esp_io_expander_set_level(s_expander, pin, 0), TAG, "reset low");
    vTaskDelay(pdMS_TO_TICKS(delay_ms));
    ESP_RETURN_ON_ERROR(esp_io_expander_set_level(s_expander, pin, 1), TAG, "reset high");
    vTaskDelay(pdMS_TO_TICKS(delay_ms));
    return ESP_OK;
}

static lv_display_t *display_start(lv_indev_t **touch)
{
    *touch = NULL;
    if (reset_pin(EX_LCD_RST, 200) != ESP_OK) {
        return NULL;
    }
    const spi_bus_config_t bus = {
        .sclk_io_num = LCD_SCLK,
        .mosi_io_num = LCD_MOSI,
        .miso_io_num = GPIO_NUM_NC,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = LCD_WIDTH * DRAW_BUF_LINES * 2,
    };
    if (spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) {
        return NULL;
    }
    esp_lcd_panel_io_handle_t io;
    const esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = LCD_CS,
        .dc_gpio_num = LCD_DC,
        .spi_mode = 0,
        .pclk_hz = 40 * 1000 * 1000,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    if (esp_lcd_new_panel_io_spi(LCD_HOST, &io_cfg, &io) != ESP_OK) {
        return NULL;
    }
    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = GPIO_NUM_NC,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    if (esp_lcd_new_panel_st7789(io, &panel_cfg, &s_panel) != ESP_OK ||
        esp_lcd_panel_init(s_panel) != ESP_OK ||
        esp_lcd_panel_invert_color(s_panel, true) != ESP_OK ||
        esp_lcd_panel_disp_on_off(s_panel, true) != ESP_OK) {
        return NULL;
    }
    esp_lv_adapter_config_t adapter_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    adapter_cfg.task_core_id = MUSE_UI_CORE;
    adapter_cfg.task_priority = MUSE_UI_PRIORITY;
    if (esp_lv_adapter_init(&adapter_cfg) != ESP_OK) {
        return NULL;
    }
    const esp_lv_adapter_display_config_t disp_cfg = {
        .panel = s_panel,
        .panel_io = io,
        .profile = {
            .interface = ESP_LV_ADAPTER_PANEL_IF_OTHER,
            .rotation = ESP_LV_ADAPTER_ROTATE_0,
            .hor_res = LCD_WIDTH,
            .ver_res = LCD_HEIGHT,
            .buffer_height = DRAW_BUF_LINES,
            .use_psram = true,
            .require_double_buffer = true,
        },
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE,
    };
    lv_display_t *disp = esp_lv_adapter_register_display(&disp_cfg);
    if (!disp || reset_pin(EX_TOUCH_RST, 10) != ESP_OK) {
        return NULL;
    }
    esp_lcd_panel_io_handle_t touch_io;
    esp_lcd_panel_io_i2c_config_t touch_io_cfg = ESP_LCD_TOUCH_IO_I2C_CST3530_CONFIG();
    touch_io_cfg.scl_speed_hz = 400000;
    const esp_lcd_touch_config_t touch_cfg = {
        .x_max = LCD_WIDTH,
        .y_max = LCD_HEIGHT,
        .rst_gpio_num = GPIO_NUM_NC,
        .int_gpio_num = TOUCH_INT,
        .levels = { .reset = 0, .interrupt = 0 },
        /* CST3530 1.0.0 sends its 32-bit commands directly through this bus. */
        .driver_data = s_i2c,
    };
    esp_lcd_touch_handle_t tp;
    if (esp_lcd_new_panel_io_i2c(s_i2c, &touch_io_cfg, &touch_io) != ESP_OK ||
        esp_lcd_touch_new_i2c_cst3530(touch_io, &touch_cfg, &tp) != ESP_OK) {
        return NULL;
    }
    const esp_lv_adapter_touch_config_t indev_cfg = ESP_LV_ADAPTER_TOUCH_DEFAULT_CONFIG(disp, tp);
    *touch = esp_lv_adapter_register_touch(&indev_cfg);
    if (!*touch || esp_lv_adapter_start() != ESP_OK) {
        return NULL;
    }
    return disp;
}

static bool display_lock(int timeout_ms)
{
    return esp_lv_adapter_lock(timeout_ms) == ESP_OK;
}

static void set_brightness(int pct)
{
    pct = pct < 0 ? 0 : pct > 100 ? 100 : pct;
    ESP_ERROR_CHECK_WITHOUT_ABORT(custom_io_expander_set_pwm(s_expander, pct * 255 / 100));
}

static void panel_sleep(bool sleep)
{
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_lcd_panel_disp_sleep(s_panel, sleep));
}

static esp_err_t audio_init(esp_codec_dev_handle_t *spk, esp_codec_dev_handle_t *mic)
{
    i2s_chan_handle_t tx, rx;
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &tx, &rx), TAG, "i2s channel");
    const i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(MUSE_AUDIO_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = GPIO_NUM_NC,
            .bclk = I2S_BCLK,
            .ws = I2S_WS,
            .dout = I2S_DOUT,
            .din = I2S_DIN,
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(tx, &std_cfg), TAG, "i2s tx");
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(rx, &std_cfg), TAG, "i2s rx");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(tx), TAG, "i2s tx on");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(rx), TAG, "i2s rx on");
    audio_codec_i2s_cfg_t i2s_cfg = { .port = I2S_NUM_0, .rx_handle = rx, .tx_handle = tx };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);
    audio_codec_i2c_cfg_t i2c_cfg = { .port = I2C_NUM_0, .addr = ES8311_CODEC_DEFAULT_ADDR, .bus_handle = s_i2c };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    ESP_RETURN_ON_FALSE(data_if && ctrl_if && gpio_if, ESP_ERR_NO_MEM, TAG, "codec interfaces");
    es8311_codec_cfg_t es_cfg = {
        .ctrl_if = ctrl_if,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH,
        .pa_pin = GPIO_NUM_NC,
        .use_mclk = false,  /* ES8311 uses BCLK; there is no MCLK wire. */
        .hw_gain = { .pa_voltage = 5.0, .codec_dac_voltage = 3.3 },
    };
    const audio_codec_if_t *codec = es8311_codec_new(&es_cfg);
    ESP_RETURN_ON_FALSE(codec, ESP_FAIL, TAG, "ES8311");
    esp_codec_dev_cfg_t out_cfg = { .dev_type = ESP_CODEC_DEV_TYPE_OUT, .codec_if = codec, .data_if = data_if };
    esp_codec_dev_cfg_t in_cfg = { .dev_type = ESP_CODEC_DEV_TYPE_IN, .codec_if = codec, .data_if = data_if };
    *spk = esp_codec_dev_new(&out_cfg);
    *mic = esp_codec_dev_new(&in_cfg);
    ESP_RETURN_ON_FALSE(*spk && *mic, ESP_ERR_NO_MEM, TAG, "codec devices");
    return esp_io_expander_set_level(s_expander, EX_PA_EN, 1);
}

static unsigned poll_buttons(void)
{
    return muse_gpio_button_poll(&s_boot);
}

static esp_err_t power_off(void)
{
    set_brightness(0);
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, false), TAG, "display off");
    ESP_RETURN_ON_ERROR(esp_io_expander_set_level(s_expander, EX_PA_EN, 0), TAG, "amplifier off");
    /* BOOT (GPIO28) is not a deep-sleep wake pin on C5. RESET wakes the board. */
    esp_deep_sleep_start();
    return ESP_FAIL;
}

static const muse_board_t s_board = {
    .name = "Waveshare ESP32-C5-Touch-LCD-2.8",
    .width = LCD_WIDTH,
    .height = LCD_HEIGHT,
    .round = false,
    .touch = true,
    .diagonal_in = 2.8f,
    .talk_button = "boot",
    .aux_button = "",
    .talk_hint = { LV_ALIGN_BOTTOM_LEFT, 8, -4 },
    .frame_ms = 50,
    .init = init,
    .display_start = display_start,
    .display_lock = display_lock,
    .display_unlock = esp_lv_adapter_unlock,
    .set_brightness = set_brightness,
    .panel_sleep = panel_sleep,
    .audio_init = audio_init,
    .mic_slot = 0,
    .poll_buttons = poll_buttons,
    .power_off = power_off,
};

const muse_board_t *muse_board_get(void)
{
    return &s_board;
}
