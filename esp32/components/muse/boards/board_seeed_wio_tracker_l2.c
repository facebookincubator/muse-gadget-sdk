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

/* Seeed Wio Tracker L2 with the 3.2-inch IPS assembly. Pin and controller
 * references: devices/seeed-wio-tracker-l2.md. */
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_codec_dev_defaults.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_sh8601.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_lv_adapter.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "muse_audio.h"
#include "muse_board.h"
#include "muse_mem.h"

static const char *TAG = "wio_l2";
#define LCD_W 320
#define LCD_H 240
#define DRAW_LINES 16
#define I2C_HZ 100000

/* PCA9555 port 0: WAKE, touch address select, panel select, rail and reset.
 * Port 1: touch reset and speaker amplifier. */
#define WAKE_KEY       BIT(0)
#define TP_ADDR_SELECT BIT(3)
#define PANEL_SELECT   BIT(4)
#define PANEL_ENABLE   BIT(5)
#define PANEL_RESET    BIT(6)
#define TOUCH_RESET    BIT(0)
#define SPEAKER_ENABLE BIT(4)
#define OUT0 2
#define OUT1 3
#define DIR0 6
#define DIR1 7

static i2c_master_bus_handle_t s_i2c;
static i2c_master_dev_handle_t s_expander, s_backlight;
static muse_gpio_button_t s_talk;
static bool s_aux_down;

static esp_err_t write_reg(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t value)
{
    const uint8_t data[] = { reg, value };
    return i2c_master_transmit(dev, data, sizeof(data), 50);
}

/* Keep the unrelated rail and direction bits when the expander survives RST.
 * After init the audio task is the only writer (to OUT1). */
static esp_err_t exp_update(uint8_t reg, uint8_t mask, uint8_t bits)
{
    uint8_t value;
    ESP_RETURN_ON_ERROR(i2c_master_transmit_receive(s_expander, &reg, 1, &value, 1, 50), TAG, "expander read");
    return write_reg(s_expander, reg, (value & ~mask) | (bits & mask));
}

static esp_err_t init(void)
{
    const i2c_master_bus_config_t bus = {
        .i2c_port = I2C_NUM_0, .sda_io_num = GPIO_NUM_47, .scl_io_num = GPIO_NUM_48,
        .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7, .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus, &s_i2c), TAG, "I2C");
    i2c_device_config_t dev = { .device_address = 0x21, .scl_speed_hz = I2C_HZ };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_i2c, &dev, &s_expander), TAG, "PCA9555");

    const uint8_t panel_lines = TP_ADDR_SELECT | PANEL_SELECT | PANEL_ENABLE | PANEL_RESET;
    const uint8_t audio_touch_lines = SPEAKER_ENABLE | TOUCH_RESET;
    /* Preload the latches before switching to outputs: both resets asserted,
     * GT911 INT low selects 0x5d (0x14 is occupied by the mic), amplifier off. */
    ESP_RETURN_ON_ERROR(exp_update(OUT0, panel_lines, PANEL_SELECT | PANEL_ENABLE), TAG, "panel rail");
    ESP_RETURN_ON_ERROR(exp_update(OUT1, audio_touch_lines, 0), TAG, "reset and mute");
    ESP_RETURN_ON_ERROR(exp_update(DIR0, panel_lines | WAKE_KEY, WAKE_KEY), TAG, "panel outputs");
    ESP_RETURN_ON_ERROR(exp_update(DIR1, audio_touch_lines, 0), TAG, "audio/touch outputs");
    vTaskDelay(pdMS_TO_TICKS(50));
    ESP_RETURN_ON_ERROR(exp_update(OUT0, PANEL_RESET, PANEL_RESET), TAG, "release LCD");
    ESP_RETURN_ON_ERROR(exp_update(OUT1, TOUCH_RESET, TOUCH_RESET), TAG, "release touch");
    vTaskDelay(pdMS_TO_TICKS(60));
    ESP_RETURN_ON_ERROR(exp_update(DIR0, TP_ADDR_SELECT, TP_ADDR_SELECT), TAG, "release touch INT");

    dev.device_address = 0x2c;
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_i2c, &dev, &s_backlight), TAG, "LP5814");
    ESP_RETURN_ON_ERROR(write_reg(s_backlight, 0x00, 1), TAG, "backlight enable");
    vTaskDelay(pdMS_TO_TICKS(5));
    ESP_RETURN_ON_ERROR(write_reg(s_backlight, 0x02, 0), TAG, "disable channels");
    ESP_RETURN_ON_ERROR(write_reg(s_backlight, 0x01, 1), TAG, "current range");
    ESP_RETURN_ON_ERROR(write_reg(s_backlight, 0x04, 0x4e), TAG, "dimming mode");
    ESP_RETURN_ON_ERROR(write_reg(s_backlight, 0x05, 0xf0), TAG, "manual drive");
    for (int channel = 0; channel < 4; channel++) {
        ESP_RETURN_ON_ERROR(write_reg(s_backlight, 0x14 + channel, 200), TAG, "LED current");
        ESP_RETURN_ON_ERROR(write_reg(s_backlight, 0x18 + channel, 0), TAG, "initial PWM");
    }
    ESP_RETURN_ON_ERROR(write_reg(s_backlight, 0x02, 0x0f), TAG, "enable channels");
    ESP_RETURN_ON_ERROR(write_reg(s_backlight, 0x0f, 0x55), TAG, "latch backlight");

    ESP_RETURN_ON_ERROR(muse_gpio_button_init(&s_talk, GPIO_NUM_0), TAG, "U/B button");
    s_talk.pressed = !gpio_get_level(GPIO_NUM_0);
    uint8_t reg = 0, inputs;
    ESP_RETURN_ON_ERROR(i2c_master_transmit_receive(s_expander, &reg, 1, &inputs, 1, 50), TAG, "WAKE button");
    s_aux_down = !(inputs & WAKE_KEY);
    return ESP_OK;
}

static void set_brightness(int pct)
{
    uint8_t pwm = pct * 255 / 100;
    for (int channel = 0; channel < 4; channel++) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(write_reg(s_backlight, 0x18 + channel, pwm));
    }
}

/* The NV3031B accepts the SH8601 driver's QSPI framing for register writes
 * (02h) and pixel data (32h). Supply the NV3031B register sequence instead
 * of the SH8601 defaults. Panel_NV3031B.hpp, copyright (c) 2020 lovyan03,
 * BSD-2-Clause; see devices/licenses/LovyanGFX.txt. */
static const sh8601_lcd_init_cmd_t panel_commands[] = {
    { 0x00, NULL, 0, 0 },
    { 0xFD, (uint8_t[]){ 0x06, 0x08 }, 2, 0 },
    { 0x60, (uint8_t[]){ 0x0C }, 1, 0 },
    { 0x61, (uint8_t[]){ 0x07, 0x04 }, 2, 0 },
    { 0xB4, (uint8_t[]){ 0x01 }, 1, 0 },
    { 0xB1, (uint8_t[]){ 0x0F, 0x02, 0x03 }, 3, 0 },
    { 0xB5, (uint8_t[]){ 0x02, 0x02, 0x0A, 0x14 }, 4, 0 },
    { 0xB6, (uint8_t[]){ 0x44, 0x01, 0x9F, 0x00, 0x02 }, 5, 0 },
    { 0xDF, (uint8_t[]){ 0x11 }, 1, 0 },
    { 0x67, (uint8_t[]){ 0x21 }, 1, 0 },
    { 0x68, (uint8_t[]){ 0x90, 0x4F, 0x27, 0x21 }, 4, 0 },
    { 0xE1, (uint8_t[]){ 0x20, 0x69 }, 2, 0 },
    { 0xE4, (uint8_t[]){ 0x69, 0x20 }, 2, 0 },
    { 0xE2, (uint8_t[]){ 0x10, 0x12, 0x12, 0x30, 0x39, 0x3F }, 6, 0 },
    { 0xE5, (uint8_t[]){ 0x3F, 0x33, 0x2D, 0x12, 0x12, 0x10 }, 6, 0 },
    { 0xE0, (uint8_t[]){ 0x06, 0x06, 0x0B, 0x12, 0x11, 0x11, 0x0E, 0x19 }, 8, 0 },
    { 0xE3, (uint8_t[]){ 0x19, 0x13, 0x14, 0x14, 0x14, 0x12, 0x08, 0x05 }, 8, 0 },
    { 0xE6, (uint8_t[]){ 0x00, 0xFF }, 2, 0 },
    { 0xE7, (uint8_t[]){ 0x01, 0x04, 0x03, 0x03, 0x00, 0x12 }, 6, 0 },
    { 0xE8, (uint8_t[]){ 0x00, 0x70, 0x00 }, 3, 0 },
    { 0xEC, (uint8_t[]){ 0x54 }, 1, 0 },
    { 0xFD, (uint8_t[]){ 0xFA, 0xFC }, 2, 0 },
    { 0x3A, (uint8_t[]){ 0x55 }, 1, 0 },
    { 0x11, NULL, 0, 120 },                       /* sleep out */
    { 0x36, (uint8_t[]){ 0xa0 }, 1, 0 },          /* MY | MV, landscape RGB */
    { 0x20, NULL, 0, 0 },                         /* inversion off */
    { 0x29, (uint8_t[]){ 0 }, 1, 0 },             /* NV3031B display-on dummy byte */
};

static esp_err_t panel_init(esp_lcd_panel_handle_t *panel, esp_lcd_panel_io_handle_t *io)
{
    const spi_bus_config_t bus = SH8601_PANEL_BUS_QSPI_CONFIG(42, 41, 40, 39, 38, LCD_W * DRAW_LINES * 2);
    ESP_RETURN_ON_ERROR(spi_bus_initialize(SPI3_HOST, &bus, SPI_DMA_CH_AUTO), TAG, "QSPI bus");
    const esp_lcd_panel_io_spi_config_t spi = SH8601_PANEL_IO_QSPI_CONFIG(46, NULL, NULL);
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi(SPI3_HOST, &spi, io), TAG, "QSPI IO");
    const sh8601_vendor_config_t vendor = {
        .init_cmds = panel_commands, .init_cmds_size = sizeof(panel_commands) / sizeof(panel_commands[0]),
        .flags.use_qspi_interface = true,
    };
    const esp_lcd_panel_dev_config_t cfg = {
        .reset_gpio_num = GPIO_NUM_NC, .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16, .vendor_config = (void *)&vendor,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_sh8601(*io, &cfg, panel), TAG, "QSPI panel");
    return esp_lcd_panel_init(*panel);  /* hardware reset through the expander in init() */
}

static esp_err_t touch_init(esp_lcd_touch_handle_t *touch)
{
    esp_lcd_panel_io_i2c_config_t cfg = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
    cfg.dev_addr = 0x5d;
    cfg.scl_speed_hz = I2C_HZ;
    esp_lcd_panel_io_handle_t io;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(s_i2c, &cfg, &io), TAG, "GT911 IO");
    const esp_lcd_touch_config_t tp = {
        /* The driver mirrors in the native portrait frame, then swaps axes.
         * Logical (x, y) = (319 - raw_y, raw_x). */
        .x_max = LCD_H - 1, .y_max = LCD_W - 1,
        .rst_gpio_num = GPIO_NUM_NC, .int_gpio_num = GPIO_NUM_NC,
        .flags = { .mirror_y = 1, .swap_xy = 1 },
    };
    return esp_lcd_touch_new_i2c_gt911(io, &tp, touch);
}

static lv_display_t *display_start(lv_indev_t **touch)
{
    esp_lcd_panel_handle_t panel;
    esp_lcd_panel_io_handle_t io;
    esp_lcd_touch_handle_t tp;
    if (panel_init(&panel, &io) != ESP_OK || touch_init(&tp) != ESP_OK) {
        return NULL;
    }
    esp_lv_adapter_config_t adapter = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    adapter.task_core_id = MUSE_UI_CORE;
    adapter.task_priority = MUSE_UI_PRIORITY;
    if (esp_lv_adapter_init(&adapter) != ESP_OK) {
        return NULL;
    }
    const esp_lv_adapter_display_config_t cfg = {
        .panel = panel, .panel_io = io,
        .profile = {
            .interface = ESP_LV_ADAPTER_PANEL_IF_OTHER, .rotation = ESP_LV_ADAPTER_ROTATE_0,
            .hor_res = LCD_W, .ver_res = LCD_H, .buffer_height = DRAW_LINES,
            .require_double_buffer = true,
        },
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE,
    };
    lv_display_t *disp = esp_lv_adapter_register_display(&cfg);
    if (!disp) {
        return NULL;
    }
    const esp_lv_adapter_touch_config_t input = ESP_LV_ADAPTER_TOUCH_DEFAULT_CONFIG(disp, tp);
    *touch = esp_lv_adapter_register_touch(&input);
    return *touch && esp_lv_adapter_start() == ESP_OK ? disp : NULL;
}

static bool display_lock(int timeout_ms)
{
    return esp_lv_adapter_lock(timeout_ms) == ESP_OK;
}

static esp_err_t audio_init(esp_codec_dev_handle_t *spk, esp_codec_dev_handle_t *mic)
{
    /* The two codecs share one I2S clock domain, as on the Watcher. */
    i2s_chan_handle_t tx, rx;
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &tx, &rx), TAG, "duplex I2S");
    const i2s_std_config_t cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(MUSE_AUDIO_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = GPIO_NUM_10, .bclk = GPIO_NUM_11, .ws = GPIO_NUM_12,
            .dout = GPIO_NUM_16, .din = GPIO_NUM_15,
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(tx, &cfg), TAG, "speaker I2S");
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(rx, &cfg), TAG, "microphone I2S");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(tx), TAG, "TX enable");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(rx), TAG, "RX enable");

    audio_codec_i2s_cfg_t bus = { .port = I2S_NUM_0, .tx_handle = tx, .rx_handle = rx };
    const audio_codec_data_if_t *data = audio_codec_new_i2s_data(&bus);
    audio_codec_i2c_cfg_t ctrl = { .port = I2C_NUM_0, .bus_handle = s_i2c, .addr = ES8311_CODEC_DEFAULT_ADDR };
    es8311_codec_cfg_t speaker = {
        .ctrl_if = audio_codec_new_i2c_ctrl(&ctrl), .gpio_if = audio_codec_new_gpio(),
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC, .pa_pin = GPIO_NUM_NC, .use_mclk = true,
    };
    ctrl.addr = 0x14 << 1;  /* esp_codec_dev takes an 8-bit address */
    es7243e_codec_cfg_t capture = { .ctrl_if = audio_codec_new_i2c_ctrl(&ctrl) };
    ESP_RETURN_ON_FALSE(data && speaker.ctrl_if && speaker.gpio_if && capture.ctrl_if,
                        ESP_ERR_NO_MEM, TAG, "codec interfaces");
    esp_codec_dev_cfg_t out = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT, .codec_if = es8311_codec_new(&speaker), .data_if = data,
    };
    esp_codec_dev_cfg_t in = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN, .codec_if = es7243e_codec_new(&capture), .data_if = data,
    };
    ESP_RETURN_ON_FALSE(out.codec_if && in.codec_if, ESP_FAIL, TAG, "ES8311/ES7243E init");
    *spk = esp_codec_dev_new(&out);
    *mic = esp_codec_dev_new(&in);
    return *spk && *mic ? ESP_OK : ESP_ERR_NO_MEM;
}

static void audio_power(bool on)
{
    ESP_ERROR_CHECK_WITHOUT_ABORT(exp_update(OUT1, SPEAKER_ENABLE, on ? SPEAKER_ENABLE : 0));
    if (on) {
        vTaskDelay(pdMS_TO_TICKS(250));  /* amplifier settling time in the L2 variant */
    }
}

static unsigned poll_buttons(void)
{
    static unsigned stable;
    unsigned edges = muse_gpio_button_poll(&s_talk);
    uint8_t reg = 0, inputs;
    if (i2c_master_transmit_receive(s_expander, &reg, 1, &inputs, 1, 20) != ESP_OK) {
        stable = 0;
        return edges;
    }
    bool down = !(inputs & WAKE_KEY);
    if (down == s_aux_down) {
        stable = 0;
    } else if (++stable >= 2) {
        s_aux_down = down;
        stable = 0;
        edges |= down ? MUSE_BTN_AUX_PRESS : MUSE_BTN_AUX_RELEASE;
    }
    return edges;
}

static esp_err_t power_off(void)
{
    return ESP_ERR_NOT_SUPPORTED;  /* physical power switch */
}

static const muse_board_t s_board = {
    .name = "Seeed Wio Tracker L2", .width = LCD_W, .height = LCD_H, .diagonal_in = 3.2f,
    .touch = true, .talk_button = "U/B", .aux_button = "WAKE",
    .talk_hint = { LV_ALIGN_RIGHT_MID, -4, 0 },
    .frame_ms = 40, .init = init, .display_start = display_start,
    .display_lock = display_lock, .display_unlock = esp_lv_adapter_unlock,
    .set_brightness = set_brightness, .poll_buttons = poll_buttons,
    .audio_init = audio_init, .audio_power = audio_power, .mic_slot = 1,
    .power_off = power_off,
};

const muse_board_t *muse_board_get(void)
{
    return &s_board;
}
