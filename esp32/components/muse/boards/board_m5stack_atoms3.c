/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

/*
 * M5Stack AtomS3 + Atomic Voice Base: ESP32-S3, 8 MB flash, no PSRAM,
 * 128x128 GC9107 LCD and ES8311 duplex audio. Hardware pins are from
 * M5Stack's M5Unified/M5GFX sources and Atomic Voice Base pin map:
 * https://github.com/m5stack/M5Unified
 * https://github.com/m5stack/M5GFX
 * https://docs.m5stack.com/en/atom/Atomic_Voice_Base
 * The GC9107 initialization table below follows M5GFX's Panel_GC9107 and
 * retains its upstream FreeBSD license; see the root README's attribution.
 */
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_codec_dev_defaults.h"
#include "esp_lcd_gc9107.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "muse_audio.h"
#include "muse_board.h"
#include "muse_input.h"
#include "muse_mem.h"

static const char *TAG = "board";

#define LCD_W 128
#define LCD_H 128
#define LCD_HOST SPI3_HOST
#define LCD_SCLK GPIO_NUM_17
#define LCD_MOSI GPIO_NUM_21
#define LCD_CS GPIO_NUM_15
#define LCD_DC GPIO_NUM_33
#define LCD_RST GPIO_NUM_34
#define LCD_BL GPIO_NUM_16
#define LCD_Y_OFFSET 32
#define DRAW_BUF_LINES 8

/* AtomS3's GC9107 needs M5Stack's panel-specific setup. The row offset is
 * also deliberate: its 128x128 glass is backed by 128x160 controller RAM. */
static const uint8_t s_gamma_positive[] = {
    0x01, 0x2b, 0x23, 0x3c, 0xb7, 0x12, 0x17,
    0x60, 0x00, 0x06, 0x0c, 0x17, 0x12, 0x1f,
};
static const uint8_t s_gamma_negative[] = {
    0x05, 0x2e, 0x2d, 0x44, 0xd6, 0x15, 0x17,
    0xa0, 0x02, 0x0d, 0x0d, 0x1a, 0x18, 0x1f,
};
static const gc9107_lcd_init_cmd_t s_lcd_init_cmds[] = {
    { 0xfe, NULL, 0, 5 },
    { 0xef, NULL, 0, 5 },
    { 0xb0, (uint8_t[]){ 0xc0 }, 1, 0 },
    { 0xb2, (uint8_t[]){ 0x2f }, 1, 0 },
    { 0xb3, (uint8_t[]){ 0x03 }, 1, 0 },
    { 0xb6, (uint8_t[]){ 0x19 }, 1, 0 },
    { 0xb7, (uint8_t[]){ 0x01 }, 1, 0 },
    { 0xac, (uint8_t[]){ 0xcb }, 1, 0 },
    { 0xab, (uint8_t[]){ 0x0e }, 1, 0 },
    { 0xb4, (uint8_t[]){ 0x04 }, 1, 0 },
    { 0xa8, (uint8_t[]){ 0x19 }, 1, 0 },
    { 0xb8, (uint8_t[]){ 0x08 }, 1, 0 },
    { 0xe8, (uint8_t[]){ 0x24 }, 1, 0 },
    { 0xe9, (uint8_t[]){ 0x48 }, 1, 0 },
    { 0xea, (uint8_t[]){ 0x22 }, 1, 0 },
    { 0xc6, (uint8_t[]){ 0x30 }, 1, 0 },
    { 0xc7, (uint8_t[]){ 0x18 }, 1, 0 },
    { 0xf0, s_gamma_positive, sizeof(s_gamma_positive), 0 },
    { 0xf1, s_gamma_negative, sizeof(s_gamma_negative), 0 },
    { 0x11, NULL, 0, 120 },
    { 0x29, NULL, 0, 0 },
};
static const gc9107_vendor_config_t s_lcd_vendor_cfg = {
    .init_cmds = s_lcd_init_cmds,
    .init_cmds_size = sizeof(s_lcd_init_cmds) / sizeof(s_lcd_init_cmds[0]),
};

#define I2C_SDA GPIO_NUM_38
#define I2C_SCL GPIO_NUM_39
#define I2S_BCLK GPIO_NUM_8
#define I2S_WS GPIO_NUM_6
#define I2S_DOUT GPIO_NUM_5
#define I2S_DIN GPIO_NUM_7
#define TALK_GPIO GPIO_NUM_41

static i2c_master_bus_handle_t s_i2c;
static esp_lcd_panel_handle_t s_panel;
static esp_lcd_panel_io_handle_t s_panel_io;
static muse_gpio_button_t s_talk;

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
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&cfg, &s_i2c), TAG, "I2C bus");
    ESP_RETURN_ON_ERROR(muse_gpio_button_init(&s_talk, TALK_GPIO), TAG, "talk button");
    s_talk.pressed = gpio_get_level(TALK_GPIO) == 0;
    return ESP_OK;
}

static lv_display_t *display_start(lv_indev_t **touch)
{
    *touch = NULL;
    const ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 256,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    const ledc_channel_config_t channel = {
        .gpio_num = LCD_BL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
    };
    if (ledc_timer_config(&timer) != ESP_OK || ledc_channel_config(&channel) != ESP_OK) {
        ESP_LOGE(TAG, "backlight PWM init failed");
        return NULL;
    }

    const spi_bus_config_t bus = {
        .sclk_io_num = LCD_SCLK,
        .mosi_io_num = LCD_MOSI,
        .miso_io_num = GPIO_NUM_NC,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = LCD_W * DRAW_BUF_LINES * sizeof(uint16_t),
    };
    if (spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) {
        ESP_LOGE(TAG, "SPI bus init failed");
        return NULL;
    }
    const esp_lcd_panel_io_spi_config_t io_cfg = GC9107_PANEL_IO_SPI_CONFIG(LCD_CS, LCD_DC, NULL, NULL);
    if (esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_cfg, &s_panel_io) != ESP_OK) {
        ESP_LOGE(TAG, "GC9107 panel IO init failed");
        return NULL;
    }
    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = (void *)&s_lcd_vendor_cfg,
    };
    if (esp_lcd_new_panel_gc9107(s_panel_io, &panel_cfg, &s_panel) != ESP_OK ||
        esp_lcd_panel_reset(s_panel) != ESP_OK || esp_lcd_panel_init(s_panel) != ESP_OK ||
        esp_lcd_panel_set_gap(s_panel, 0, LCD_Y_OFFSET) != ESP_OK ||
        esp_lcd_panel_disp_on_off(s_panel, true) != ESP_OK) {
        ESP_LOGE(TAG, "GC9107 init failed");
        return NULL;
    }

    esp_lv_adapter_config_t adapter_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    adapter_cfg.task_core_id = MUSE_UI_CORE;
    adapter_cfg.task_priority = MUSE_UI_PRIORITY;
    if (esp_lv_adapter_init(&adapter_cfg) != ESP_OK) return NULL;
    const esp_lv_adapter_display_config_t disp_cfg = {
        .panel = s_panel,
        .panel_io = s_panel_io,
        .profile = {
            .interface = ESP_LV_ADAPTER_PANEL_IF_OTHER,
            .rotation = ESP_LV_ADAPTER_ROTATE_0,
            .hor_res = LCD_W,
            .ver_res = LCD_H,
            .buffer_height = DRAW_BUF_LINES,
            .use_psram = false,
            .require_double_buffer = true,
        },
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE,
    };
    lv_display_t *disp = esp_lv_adapter_register_display(&disp_cfg);
    if (!disp || esp_lv_adapter_start() != ESP_OK) return NULL;
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

static void panel_sleep(bool sleep)
{
    esp_lcd_panel_disp_sleep(s_panel, sleep);
}

static void display_pause(bool pause)
{
    if (pause) esp_lv_adapter_pause(-1);
    else esp_lv_adapter_resume();
}

static esp_err_t audio_init(esp_codec_dev_handle_t *spk, esp_codec_dev_handle_t *mic)
{
    i2s_chan_handle_t tx, rx;
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    chan_cfg.dma_desc_num = 4;
    chan_cfg.dma_frame_num = 160;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &tx, &rx), TAG, "I2S channels");
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
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(tx, &std_cfg), TAG, "I2S TX");
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(rx, &std_cfg), TAG, "I2S RX");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(tx), TAG, "enable I2S TX");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(rx), TAG, "enable I2S RX");

    audio_codec_i2s_cfg_t data_cfg = { .port = I2S_NUM_0, .rx_handle = rx, .tx_handle = tx };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&data_cfg);
    audio_codec_i2c_cfg_t ctrl_cfg = {
        .port = I2C_NUM_0,
        .addr = ES8311_CODEC_DEFAULT_ADDR,
        .bus_handle = s_i2c,
    };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&ctrl_cfg);
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    ESP_RETURN_ON_FALSE(data_if && ctrl_if && gpio_if, ESP_ERR_NO_MEM, TAG, "codec interfaces");

    es8311_codec_cfg_t es_cfg = {
        .ctrl_if = ctrl_if,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH,
        .pa_pin = -1,
        .use_mclk = false,
        .hw_gain = { .pa_voltage = 5.0, .codec_dac_voltage = 3.3 },
    };
    const audio_codec_if_t *codec = es8311_codec_new(&es_cfg);
    ESP_RETURN_ON_FALSE(codec, ESP_FAIL, TAG, "ES8311 not detected");
    esp_codec_dev_cfg_t out_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT, .codec_if = codec, .data_if = data_if,
    };
    esp_codec_dev_cfg_t in_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN, .codec_if = codec, .data_if = data_if,
    };
    *spk = esp_codec_dev_new(&out_cfg);
    *mic = esp_codec_dev_new(&in_cfg);
    return *spk && *mic ? ESP_OK : ESP_FAIL;
}

static unsigned poll_buttons(void)
{
    return muse_gpio_button_poll(&s_talk);
}

static void wait_buttons(int timeout_ms)
{
    muse_gpio_button_t *buttons[] = { &s_talk };
    muse_gpio_buttons_wait(buttons, 1, timeout_ms);
}

static const muse_board_t s_board = {
    .name = "M5Stack AtomS3 + Atomic Voice Base",
    .width = LCD_W,
    .height = LCD_H,
    .round = false,
    .touch = false,
    .talk_button = "front",
    .frame_ms = 40,
    .init = init,
    .display_start = display_start,
    .display_lock = display_lock,
    .display_unlock = esp_lv_adapter_unlock,
    .set_brightness = set_brightness,
    .panel_sleep = panel_sleep,
    .display_pause = display_pause,
    .audio_init = audio_init,
    .mic_slot = 0,
    .poll_buttons = poll_buttons,
    .wait_buttons = wait_buttons,
};

const muse_board_t *muse_board_get(void)
{
    return &s_board;
}
