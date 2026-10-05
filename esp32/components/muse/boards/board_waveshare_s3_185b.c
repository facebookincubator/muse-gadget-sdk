/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * Licensed under the Apache License, Version 2.0.
 *
 * Waveshare ESP32-S3-Touch-LCD-1.85B. Pin map and ST77916 init sequence
 * follow the project's boards/waveshare/waveshare_esp32s3_touch_lcd_1_85b
 * reference firmware (setup_device.c); display 360x360 QSPI, CST816S touch.
 * Audio pins follow the supplied board hardware facts. No 1.75C BSP is used.
 */
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_codec_dev_defaults.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_st77916.h"
#include "esp_lcd_panel_io_additions.h"
#include "esp_lcd_touch.h"
#include "esp_lcd_touch_cst816s.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "driver/rtc_io.h"
#include "esp_lv_adapter.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "muse_audio.h"
#include "muse_board.h"
#include "muse_mem.h"

static const char *TAG = "board";

static const st77916_lcd_init_cmd_t s_init_v2[] = {
    {0xF0, (uint8_t []){0x28}, 1, 0},
    {0xF2, (uint8_t []){0x28}, 1, 0},
    {0x73, (uint8_t []){0xF0}, 1, 0},
    {0x7C, (uint8_t []){0xD1}, 1, 0},
    {0x83, (uint8_t []){0xE0}, 1, 0},
    {0x84, (uint8_t []){0x61}, 1, 0},
    {0xF2, (uint8_t []){0x82}, 1, 0},
    {0xF0, (uint8_t []){0x00}, 1, 0},
    {0xF0, (uint8_t []){0x01}, 1, 0},
    {0xF1, (uint8_t []){0x01}, 1, 0},
    {0xB0, (uint8_t []){0x56}, 1, 0},
    {0xB1, (uint8_t []){0x4D}, 1, 0},
    {0xB2, (uint8_t []){0x24}, 1, 0},
    {0xB4, (uint8_t []){0x87}, 1, 0},
    {0xB5, (uint8_t []){0x44}, 1, 0},
    {0xB6, (uint8_t []){0x8B}, 1, 0},
    {0xB7, (uint8_t []){0x40}, 1, 0},
    {0xB8, (uint8_t []){0x86}, 1, 0},
    {0xBA, (uint8_t []){0x00}, 1, 0},
    {0xBB, (uint8_t []){0x08}, 1, 0},
    {0xBC, (uint8_t []){0x08}, 1, 0},
    {0xBD, (uint8_t []){0x00}, 1, 0},
    {0xC0, (uint8_t []){0x80}, 1, 0},
    {0xC1, (uint8_t []){0x10}, 1, 0},
    {0xC2, (uint8_t []){0x37}, 1, 0},
    {0xC3, (uint8_t []){0x80}, 1, 0},
    {0xC4, (uint8_t []){0x10}, 1, 0},
    {0xC5, (uint8_t []){0x37}, 1, 0},
    {0xC6, (uint8_t []){0xA9}, 1, 0},
    {0xC7, (uint8_t []){0x41}, 1, 0},
    {0xC8, (uint8_t []){0x01}, 1, 0},
    {0xC9, (uint8_t []){0xA9}, 1, 0},
    {0xCA, (uint8_t []){0x41}, 1, 0},
    {0xCB, (uint8_t []){0x01}, 1, 0},
    {0xD0, (uint8_t []){0x91}, 1, 0},
    {0xD1, (uint8_t []){0x68}, 1, 0},
    {0xD2, (uint8_t []){0x68}, 1, 0},
    {0xF5, (uint8_t []){0x00, 0xA5}, 2, 0},
    {0xDD, (uint8_t []){0x4F}, 1, 0},
    {0xDE, (uint8_t []){0x4F}, 1, 0},
    {0xF1, (uint8_t []){0x10}, 1, 0},
    {0xF0, (uint8_t []){0x00}, 1, 0},
    {0xF0, (uint8_t []){0x02}, 1, 0},
    {0xE0, (uint8_t []){0xF0, 0x0A, 0x10, 0x09, 0x09, 0x36, 0x35, 0x33, 0x4A, 0x29, 0x15, 0x15, 0x2E, 0x34}, 14, 0},
    {0xE1, (uint8_t []){0xF0, 0x0A, 0x0F, 0x08, 0x08, 0x05, 0x34, 0x33, 0x4A, 0x39, 0x15, 0x15, 0x2D, 0x33}, 14, 0},
    {0xF0, (uint8_t []){0x10}, 1, 0},
    {0xF3, (uint8_t []){0x10}, 1, 0},
    {0xE0, (uint8_t []){0x07}, 1, 0},
    {0xE1, (uint8_t []){0x00}, 1, 0},
    {0xE2, (uint8_t []){0x00}, 1, 0},
    {0xE3, (uint8_t []){0x00}, 1, 0},
    {0xE4, (uint8_t []){0xE0}, 1, 0},
    {0xE5, (uint8_t []){0x06}, 1, 0},
    {0xE6, (uint8_t []){0x21}, 1, 0},
    {0xE7, (uint8_t []){0x01}, 1, 0},
    {0xE8, (uint8_t []){0x05}, 1, 0},
    {0xE9, (uint8_t []){0x02}, 1, 0},
    {0xEA, (uint8_t []){0xDA}, 1, 0},
    {0xEB, (uint8_t []){0x00}, 1, 0},
    {0xEC, (uint8_t []){0x00}, 1, 0},
    {0xED, (uint8_t []){0x0F}, 1, 0},
    {0xEE, (uint8_t []){0x00}, 1, 0},
    {0xEF, (uint8_t []){0x00}, 1, 0},
    {0xF8, (uint8_t []){0x00}, 1, 0},
    {0xF9, (uint8_t []){0x00}, 1, 0},
    {0xFA, (uint8_t []){0x00}, 1, 0},
    {0xFB, (uint8_t []){0x00}, 1, 0},
    {0xFC, (uint8_t []){0x00}, 1, 0},
    {0xFD, (uint8_t []){0x00}, 1, 0},
    {0xFE, (uint8_t []){0x00}, 1, 0},
    {0xFF, (uint8_t []){0x00}, 1, 0},
    {0x60, (uint8_t []){0x40}, 1, 0},
    {0x61, (uint8_t []){0x04}, 1, 0},
    {0x62, (uint8_t []){0x00}, 1, 0},
    {0x63, (uint8_t []){0x42}, 1, 0},
    {0x64, (uint8_t []){0xD9}, 1, 0},
    {0x65, (uint8_t []){0x00}, 1, 0},
    {0x66, (uint8_t []){0x00}, 1, 0},
    {0x67, (uint8_t []){0x00}, 1, 0},
    {0x68, (uint8_t []){0x00}, 1, 0},
    {0x69, (uint8_t []){0x00}, 1, 0},
    {0x6A, (uint8_t []){0x00}, 1, 0},
    {0x6B, (uint8_t []){0x00}, 1, 0},
    {0x70, (uint8_t []){0x40}, 1, 0},
    {0x71, (uint8_t []){0x03}, 1, 0},
    {0x72, (uint8_t []){0x00}, 1, 0},
    {0x73, (uint8_t []){0x42}, 1, 0},
    {0x74, (uint8_t []){0xD8}, 1, 0},
    {0x75, (uint8_t []){0x00}, 1, 0},
    {0x76, (uint8_t []){0x00}, 1, 0},
    {0x77, (uint8_t []){0x00}, 1, 0},
    {0x78, (uint8_t []){0x00}, 1, 0},
    {0x79, (uint8_t []){0x00}, 1, 0},
    {0x7A, (uint8_t []){0x00}, 1, 0},
    {0x7B, (uint8_t []){0x00}, 1, 0},
    {0x80, (uint8_t []){0x48}, 1, 0},
    {0x81, (uint8_t []){0x00}, 1, 0},
    {0x82, (uint8_t []){0x06}, 1, 0},
    {0x83, (uint8_t []){0x02}, 1, 0},
    {0x84, (uint8_t []){0xD6}, 1, 0},
    {0x85, (uint8_t []){0x04}, 1, 0},
    {0x86, (uint8_t []){0x00}, 1, 0},
    {0x87, (uint8_t []){0x00}, 1, 0},
    {0x88, (uint8_t []){0x48}, 1, 0},
    {0x89, (uint8_t []){0x00}, 1, 0},
    {0x8A, (uint8_t []){0x08}, 1, 0},
    {0x8B, (uint8_t []){0x02}, 1, 0},
    {0x8C, (uint8_t []){0xD8}, 1, 0},
    {0x8D, (uint8_t []){0x04}, 1, 0},
    {0x8E, (uint8_t []){0x00}, 1, 0},
    {0x8F, (uint8_t []){0x00}, 1, 0},
    {0x90, (uint8_t []){0x48}, 1, 0},
    {0x91, (uint8_t []){0x00}, 1, 0},
    {0x92, (uint8_t []){0x0A}, 1, 0},
    {0x93, (uint8_t []){0x02}, 1, 0},
    {0x94, (uint8_t []){0xDA}, 1, 0},
    {0x95, (uint8_t []){0x04}, 1, 0},
    {0x96, (uint8_t []){0x00}, 1, 0},
    {0x97, (uint8_t []){0x00}, 1, 0},
    {0x98, (uint8_t []){0x48}, 1, 0},
    {0x99, (uint8_t []){0x00}, 1, 0},
    {0x9A, (uint8_t []){0x0C}, 1, 0},
    {0x9B, (uint8_t []){0x02}, 1, 0},
    {0x9C, (uint8_t []){0xDC}, 1, 0},
    {0x9D, (uint8_t []){0x04}, 1, 0},
    {0x9E, (uint8_t []){0x00}, 1, 0},
    {0x9F, (uint8_t []){0x00}, 1, 0},
    {0xA0, (uint8_t []){0x48}, 1, 0},
    {0xA1, (uint8_t []){0x00}, 1, 0},
    {0xA2, (uint8_t []){0x05}, 1, 0},
    {0xA3, (uint8_t []){0x02}, 1, 0},
    {0xA4, (uint8_t []){0xD5}, 1, 0},
    {0xA5, (uint8_t []){0x04}, 1, 0},
    {0xA6, (uint8_t []){0x00}, 1, 0},
    {0xA7, (uint8_t []){0x00}, 1, 0},
    {0xA8, (uint8_t []){0x48}, 1, 0},
    {0xA9, (uint8_t []){0x00}, 1, 0},
    {0xAA, (uint8_t []){0x07}, 1, 0},
    {0xAB, (uint8_t []){0x02}, 1, 0},
    {0xAC, (uint8_t []){0xD7}, 1, 0},
    {0xAD, (uint8_t []){0x04}, 1, 0},
    {0xAE, (uint8_t []){0x00}, 1, 0},
    {0xAF, (uint8_t []){0x00}, 1, 0},
    {0xB0, (uint8_t []){0x48}, 1, 0},
    {0xB1, (uint8_t []){0x00}, 1, 0},
    {0xB2, (uint8_t []){0x09}, 1, 0},
    {0xB3, (uint8_t []){0x02}, 1, 0},
    {0xB4, (uint8_t []){0xD9}, 1, 0},
    {0xB5, (uint8_t []){0x04}, 1, 0},
    {0xB6, (uint8_t []){0x00}, 1, 0},
    {0xB7, (uint8_t []){0x00}, 1, 0},
    {0xB8, (uint8_t []){0x48}, 1, 0},
    {0xB9, (uint8_t []){0x00}, 1, 0},
    {0xBA, (uint8_t []){0x0B}, 1, 0},
    {0xBB, (uint8_t []){0x02}, 1, 0},
    {0xBC, (uint8_t []){0xDB}, 1, 0},
    {0xBD, (uint8_t []){0x04}, 1, 0},
    {0xBE, (uint8_t []){0x00}, 1, 0},
    {0xBF, (uint8_t []){0x00}, 1, 0},
    {0xC0, (uint8_t []){0x10}, 1, 0},
    {0xC1, (uint8_t []){0x47}, 1, 0},
    {0xC2, (uint8_t []){0x56}, 1, 0},
    {0xC3, (uint8_t []){0x65}, 1, 0},
    {0xC4, (uint8_t []){0x74}, 1, 0},
    {0xC5, (uint8_t []){0x88}, 1, 0},
    {0xC6, (uint8_t []){0x99}, 1, 0},
    {0xC7, (uint8_t []){0x01}, 1, 0},
    {0xC8, (uint8_t []){0xBB}, 1, 0},
    {0xC9, (uint8_t []){0xAA}, 1, 0},
    {0xD0, (uint8_t []){0x10}, 1, 0},
    {0xD1, (uint8_t []){0x47}, 1, 0},
    {0xD2, (uint8_t []){0x56}, 1, 0},
    {0xD3, (uint8_t []){0x65}, 1, 0},
    {0xD4, (uint8_t []){0x74}, 1, 0},
    {0xD5, (uint8_t []){0x88}, 1, 0},
    {0xD6, (uint8_t []){0x99}, 1, 0},
    {0xD7, (uint8_t []){0x01}, 1, 0},
    {0xD8, (uint8_t []){0xBB}, 1, 0},
    {0xD9, (uint8_t []){0xAA}, 1, 0},
    {0xF3, (uint8_t []){0x01}, 1, 0},
    {0xF0, (uint8_t []){0x00}, 1, 0},
    {0x35, (uint8_t []){0x00}, 1, 0},
    {0x21, (uint8_t []){0x00}, 1, 0},
    {0x11, (uint8_t []){0x00}, 1, 120},
    {0x29, (uint8_t []){0x00}, 1, 0},
};
#define RES 360
#define DRAW_LINES 24
#define LCD_HOST SPI2_HOST
#define I2C_SDA GPIO_NUM_11
#define I2C_SCL GPIO_NUM_10
#define BL GPIO_NUM_5
#define BOOT GPIO_NUM_0
static i2c_master_bus_handle_t s_i2c;
static esp_lcd_panel_handle_t s_panel;
static esp_lcd_panel_io_handle_t s_panel_io;
static esp_lcd_touch_handle_t s_touch;
static muse_gpio_button_t s_boot;

/* LVGL 从自己的任务里轮询这个回调。CST816S 报单点。 */
static void tp_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    esp_lcd_touch_point_data_t point = {0};
    uint8_t count = 0;

    data->state = LV_INDEV_STATE_RELEASED;
    if (!s_touch || esp_lcd_touch_read_data(s_touch) != ESP_OK) return;
    if (esp_lcd_touch_get_data(s_touch, &point, &count, 1) != ESP_OK || count == 0) return;
    data->point.x = point.x;
    data->point.y = point.y;
    data->state = LV_INDEV_STATE_PRESSED;
}

static esp_err_t init(void)
{
    const i2c_master_bus_config_t cfg = { .i2c_port=I2C_NUM_0, .sda_io_num=I2C_SDA,
        .scl_io_num=I2C_SCL, .clk_source=I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt=7,
        .flags.enable_internal_pullup=true };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&cfg, &s_i2c), TAG, "I2C");
    ESP_RETURN_ON_ERROR(muse_gpio_button_init(&s_boot, BOOT), TAG, "BOOT");
    /* CST816S reset: GPIO1; touch interrupt: GPIO4. */
    gpio_set_direction(GPIO_NUM_1, GPIO_MODE_OUTPUT);
    gpio_set_level(GPIO_NUM_1, 0); vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(GPIO_NUM_1, 1); vTaskDelay(pdMS_TO_TICKS(50));
    /* GPIO9 enables the speaker amplifier; held low until audio is implemented. */
    gpio_set_direction(GPIO_NUM_9, GPIO_MODE_OUTPUT);
    gpio_set_level(GPIO_NUM_9, 0);
    return ESP_OK;
}

static lv_display_t *display_start(lv_indev_t **touch)
{
    *touch = NULL;
    const ledc_timer_config_t timer = { .speed_mode=LEDC_LOW_SPEED_MODE,
        .duty_resolution=LEDC_TIMER_10_BIT, .timer_num=LEDC_TIMER_0,
        .freq_hz=20000, .clk_cfg=LEDC_AUTO_CLK };
    const ledc_channel_config_t channel = { .gpio_num=BL, .speed_mode=LEDC_LOW_SPEED_MODE,
        .channel=LEDC_CHANNEL_0, .timer_sel=LEDC_TIMER_0, .duty=0 };
    if (ledc_timer_config(&timer) != ESP_OK || ledc_channel_config(&channel) != ESP_OK) return NULL;
    /* Match the board firmware's visible startup brightness until settings load. */
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 512);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    /* ST77916 QSPI bus: CS21 SCK40 D0..D3 = 46/45/42/41, reset GPIO3.
     * Panel vendor init table is board-specific and must not use the 1.75C BSP. */
    const spi_bus_config_t bus = { .sclk_io_num=GPIO_NUM_40, .data0_io_num=GPIO_NUM_46,
        .data1_io_num=GPIO_NUM_45, .data2_io_num=GPIO_NUM_42, .data3_io_num=GPIO_NUM_41,
        .max_transfer_sz=RES*DRAW_LINES*2, .flags=SPICOMMON_BUSFLAG_QUAD };
    if (spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) return NULL;
    const esp_lcd_panel_io_spi_config_t io_cfg = { .cs_gpio_num=GPIO_NUM_21,
        .dc_gpio_num=GPIO_NUM_NC, .spi_mode=0, .pclk_hz=80*1000*1000,
        .trans_queue_depth=10, .lcd_cmd_bits=32, .lcd_param_bits=8,
        .flags.quad_mode=true };
    if (esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_cfg, &s_panel_io) != ESP_OK) return NULL;
    st77916_vendor_config_t vendor_cfg = { .flags = { .use_qspi_interface = 1 },
        .init_cmds = s_init_v2,
        .init_cmds_size = sizeof(s_init_v2) / sizeof(st77916_lcd_init_cmd_t) };
    const esp_lcd_panel_dev_config_t panel_cfg = { .reset_gpio_num=GPIO_NUM_3,
        .rgb_ele_order=LCD_RGB_ELEMENT_ORDER_RGB, .data_endian=LCD_RGB_DATA_ENDIAN_BIG,
        .bits_per_pixel=16, .vendor_config=&vendor_cfg };
    /* ST77916 v2 init table from the board project's factory driver. */
    if (esp_lcd_new_panel_st77916(s_panel_io, &panel_cfg, &s_panel) != ESP_OK) return NULL;
    if (esp_lcd_panel_reset(s_panel) != ESP_OK || esp_lcd_panel_init(s_panel) != ESP_OK ||
        esp_lcd_panel_disp_on_off(s_panel, true) != ESP_OK) return NULL;
    esp_lcd_panel_io_handle_t touch_io;
    /* esp_lcd_panel_io_i2c expects 7-bit; board YAML stores 0x2a shifted. */
    const esp_lcd_panel_io_i2c_config_t touch_io_cfg = {
        .dev_addr=0x15, .control_phase_bytes=1, .dc_bit_offset=0,
        /* 400kHz 在这条 6 器件总线上长读会失败（信号完整性不足，只靠内部上拉
         * ≈45kΩ）：单字节读 OK，6 字节读 ESP_ERR_INVALID_RESPONSE。降到 100kHz。 */
        .lcd_cmd_bits=8, .lcd_param_bits=8, .scl_speed_hz=100000,
        .flags.disable_control_phase=true,
    };
    if (esp_lcd_new_panel_io_i2c(s_i2c, &touch_io_cfg, &touch_io) != ESP_OK) return NULL;
    esp_lcd_touch_config_t tc = { .x_max=RES, .y_max=RES, .rst_gpio_num=GPIO_NUM_1,
        .int_gpio_num=GPIO_NUM_4, .levels={ .reset=0, .interrupt=0 } };
    if (esp_lcd_touch_new_i2c_cst816s(touch_io, &tc, &s_touch) != ESP_OK) {
        ESP_LOGW(TAG, "CST816S touch init failed");
        s_touch = NULL;
    }
    if (s_touch) {
        /* Match Waveshare's factory driver: keep CST816S awake between taps. */
        uint8_t disable_auto_sleep = 0x0A;
        esp_err_t touch_pm_err = esp_lcd_panel_io_tx_param(touch_io, 0xFE, &disable_auto_sleep, 1);
        ESP_LOGI(TAG, "CST816S auto-sleep disable: %s", esp_err_to_name(touch_pm_err));
    }
    esp_lv_adapter_config_t ac = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    ac.task_core_id=MUSE_UI_CORE; ac.task_priority=MUSE_UI_PRIORITY;
    if (esp_lv_adapter_init(&ac) != ESP_OK) return NULL;
    const esp_lv_adapter_display_config_t dc = { .panel=s_panel, .panel_io=s_panel_io,
        .profile={ .interface=ESP_LV_ADAPTER_PANEL_IF_OTHER, .rotation=ESP_LV_ADAPTER_ROTATE_0,
            .hor_res=RES, .ver_res=RES, .buffer_height=DRAW_LINES, .use_psram=false,
            .require_double_buffer=true }, .tear_avoid_mode=ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE };
    lv_display_t *disp=esp_lv_adapter_register_display(&dc);
    if (!disp) return NULL;
    if (s_touch) {
        /* 消融结束：把 CST816S 交回 LVGL（消融期用于分离 I2C 问题与手势识别）。 */
        *touch = lv_indev_create();
        if (!*touch) return NULL;
        lv_indev_set_type(*touch, LV_INDEV_TYPE_POINTER);
        lv_indev_set_display(*touch, disp);
        lv_indev_set_read_cb(*touch, tp_read);
    }
    if (esp_lv_adapter_start() != ESP_OK) return NULL;
    return disp;
}
static bool display_lock(int ms) { return esp_lv_adapter_lock(ms)==ESP_OK; }
static void brightness(int pct) { ledc_set_duty(LEDC_LOW_SPEED_MODE,LEDC_CHANNEL_0,pct*1023/100); ledc_update_duty(LEDC_LOW_SPEED_MODE,LEDC_CHANNEL_0); }
static void sleep_panel(bool sleep) { if (s_panel) esp_lcd_panel_disp_sleep(s_panel,sleep); }
static void pause_display(bool pause) { if(pause) esp_lv_adapter_pause(-1); else esp_lv_adapter_resume(); }
static unsigned poll_buttons(void) { return muse_gpio_button_poll(&s_boot); }
#define I2S_MCLK GPIO_NUM_2
#define I2S_BCLK GPIO_NUM_48
#define I2S_WS GPIO_NUM_38
#define I2S_DOUT GPIO_NUM_47
#define I2S_DIN GPIO_NUM_39
#define PA_EN GPIO_NUM_9

static esp_err_t audio_init(esp_codec_dev_handle_t *spk, esp_codec_dev_handle_t *mic)
{
    *spk = NULL;
    *mic = NULL;
    /* Fail closed: the external amplifier remains disabled unless the complete
     * codec and I2S path was created successfully. */
    gpio_set_level(PA_EN, 0);

    i2s_chan_handle_t tx = NULL, rx = NULL;
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    /* Keep the duplex DMA in internal SRAM: IDF defaults (6 x 240 per
     * direction) exceed the S3's remaining DMA-capable heap after LVGL starts. */
    chan_cfg.dma_desc_num = 3;
    chan_cfg.dma_frame_num = 160;
    chan_cfg.auto_clear = true;
    esp_err_t err = i2s_new_channel(&chan_cfg, &tx, &rx);
    if (err != ESP_OK) goto fail;

    const i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(MUSE_AUDIO_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = { .mclk=I2S_MCLK, .bclk=I2S_BCLK, .ws=I2S_WS, .dout=I2S_DOUT, .din=I2S_DIN },
    };
    err = i2s_channel_init_std_mode(tx, &std_cfg);
    if (err != ESP_OK) goto fail;
    err = i2s_channel_init_std_mode(rx, &std_cfg);
    if (err != ESP_OK) goto fail;
    err = i2s_channel_enable(tx);
    if (err != ESP_OK) goto fail;
    err = i2s_channel_enable(rx);
    if (err != ESP_OK) goto fail;

    audio_codec_i2s_cfg_t data_cfg = { .port=I2S_NUM_0, .rx_handle=rx, .tx_handle=tx };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&data_cfg);
    audio_codec_i2c_cfg_t dac_i2c = { .port=I2C_NUM_0, .addr=ES8311_CODEC_DEFAULT_ADDR, .bus_handle=s_i2c };
    audio_codec_i2c_cfg_t adc_i2c = { .port=I2C_NUM_0, .addr=ES7210_CODEC_DEFAULT_ADDR, .bus_handle=s_i2c };
    const audio_codec_ctrl_if_t *dac_ctrl = audio_codec_new_i2c_ctrl(&dac_i2c);
    const audio_codec_ctrl_if_t *adc_ctrl = audio_codec_new_i2c_ctrl(&adc_i2c);
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    if (!data_if || !dac_ctrl || !adc_ctrl || !gpio_if) { err = ESP_ERR_NO_MEM; goto fail; }

    es8311_codec_cfg_t dac_cfg = { .ctrl_if=dac_ctrl, .gpio_if=gpio_if,
        .codec_mode=ESP_CODEC_DEV_WORK_MODE_DAC, .pa_pin=PA_EN, .use_mclk=true,
        .hw_gain={ .pa_voltage=5.0, .codec_dac_voltage=3.3 } };
    const audio_codec_if_t *dac = es8311_codec_new(&dac_cfg);
    if (!dac) { err = ESP_FAIL; goto fail; }
    es7210_codec_cfg_t adc_cfg = { .ctrl_if=adc_ctrl,
        .mic_selected=ES7210_SEL_MIC1 | ES7210_SEL_MIC2 };
    const audio_codec_if_t *adc = es7210_codec_new(&adc_cfg);
    if (!adc) { err = ESP_FAIL; goto fail; }

    esp_codec_dev_cfg_t out_cfg = { .dev_type=ESP_CODEC_DEV_TYPE_OUT, .codec_if=dac, .data_if=data_if };
    esp_codec_dev_cfg_t in_cfg = { .dev_type=ESP_CODEC_DEV_TYPE_IN, .codec_if=adc, .data_if=data_if };
    *spk = esp_codec_dev_new(&out_cfg);
    *mic = esp_codec_dev_new(&in_cfg);
    if (!*spk || !*mic) { err = ESP_FAIL; goto fail; }
    return ESP_OK;

fail:
    gpio_set_level(PA_EN, 0);
    ESP_LOGE(TAG, "audio init failed: %s", esp_err_to_name(err));
    return err;
}

static void set_mic_gain(esp_codec_dev_handle_t mic, int db)
{
    /* ES7210 uses 3 dB PGA steps; 24 dB is a practical initial gain. */
    db = (db / 3) * 3;
    esp_codec_dev_set_in_gain(mic, db == 33 ? 34.5f : (float)db);
}
static esp_err_t power_off(void)
{
    brightness(0);
    if (s_panel) {
        esp_lcd_panel_disp_on_off(s_panel, false);
        esp_lcd_panel_disp_sleep(s_panel, true);
    }
    gpio_set_level(PA_EN, 0);
    while (gpio_get_level(BOOT) == 0) vTaskDelay(pdMS_TO_TICKS(20));
    ESP_RETURN_ON_ERROR(rtc_gpio_init(BOOT), TAG, "RTC BOOT");
    ESP_RETURN_ON_ERROR(rtc_gpio_set_direction(BOOT, RTC_GPIO_MODE_INPUT_ONLY), TAG, "RTC BOOT input");
    ESP_RETURN_ON_ERROR(rtc_gpio_pullup_en(BOOT), TAG, "RTC BOOT pull-up");
    ESP_RETURN_ON_ERROR(rtc_gpio_pulldown_dis(BOOT), TAG, "RTC BOOT pull-down");
    ESP_RETURN_ON_ERROR(esp_sleep_enable_ext0_wakeup(BOOT, 0), TAG, "BOOT wake");
    ESP_LOGI(TAG, "deep sleep; press BOOT to wake");
    esp_deep_sleep_start();
    return ESP_FAIL;
}
static const muse_board_t s_board = { .name="Waveshare ESP32-S3-Touch-LCD-1.85B",
    .width=RES,.height=RES,.round=true,.touch=true,.button_power_controls=false,.single_button=true,.manual_touch_swipes=true,.diagonal_in=1.85f,
    .talk_button="BOOT",.talk_hint={LV_ALIGN_BOTTOM_MID,-52,-24},.frame_ms=40,
    .init=init,.display_start=display_start,.display_lock=display_lock,
    .display_unlock=esp_lv_adapter_unlock,.set_brightness=brightness,
    .panel_sleep=sleep_panel,.display_pause=pause_display,
    .audio_init=audio_init,.mic_slot=-1,.set_mic_gain=set_mic_gain,.poll_buttons=poll_buttons,
    .power_off=power_off };
const muse_board_t *muse_board_get(void) { return &s_board; }
