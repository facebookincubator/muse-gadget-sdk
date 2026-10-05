/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * Licensed under the Apache License, Version 2.0.
 *
 * Waveshare ESP32-S3-Touch-LCD-7 (N16R8). Hardware facts and wiring come from
 * waveshareteam/ESP32-S3-Touch-LCD-7, examples/ESP-IDF/09_lvgl_v9_demo,
 * components/waveshare_rgb_lcd_port.[ch], and waveshareteam/waveshare_boards,
 * boards/esp32_s3_touch_lcd_7. The attached board's boot log reports this
 * exact model; esptool reports 16 MB flash and 8 MB embedded PSRAM.
 *
 * The RGB bus uses GPIO0, so the BOOT key cannot be polled while the panel
 * runs. Touch confirms Muse's pairing prompt instead. There is no onboard
 * audio hardware; this is a display and touch gadget.
 */
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "muse_board.h"
#include "muse_link.h"
#include "muse_mem.h"

#define LCD_W 800
#define LCD_H 480
#define EXP_CONFIG_ADDR 0x24
#define EXP_OUTPUT_ADDR 0x38
#define EXP_TOUCH_RST BIT(1)
#define EXP_BACKLIGHT BIT(2)
#define EXP_LCD_RST BIT(3)
#define EXP_SD_CS BIT(4)
#define EXP_IDLE (EXP_TOUCH_RST | EXP_LCD_RST | EXP_SD_CS)

static const char *TAG = "board";
static i2c_master_bus_handle_t s_i2c;
static i2c_master_dev_handle_t s_exp_config;
static i2c_master_dev_handle_t s_exp_output;
static uint8_t s_exp_state = EXP_IDLE;
static esp_lcd_panel_handle_t s_panel;
static esp_lcd_touch_handle_t s_touch;
static lv_display_t *s_disp;
static lv_indev_t *s_indev;
static SemaphoreHandle_t s_lv_lock;
static SemaphoreHandle_t s_started;
static esp_err_t s_start_err;
static bool s_was_pressed;

static esp_err_t exp_write(uint8_t state)
{
    ESP_RETURN_ON_ERROR(i2c_master_transmit(s_exp_output, &state, 1, 50), TAG, "CH422G output");
    s_exp_state = state;
    return ESP_OK;
}

static esp_err_t init(void)
{
    const i2c_master_bus_config_t bus = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = GPIO_NUM_8,
        .scl_io_num = GPIO_NUM_9,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus, &s_i2c), TAG, "I2C bus");
    const i2c_device_config_t config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = EXP_CONFIG_ADDR,
        .scl_speed_hz = 400000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_i2c, &config, &s_exp_config), TAG, "CH422G config");
    i2c_device_config_t output = config;
    output.device_address = EXP_OUTPUT_ADDR;
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_i2c, &output, &s_exp_output), TAG, "CH422G output");
    const uint8_t all_outputs = 0x01; /* Waveshare's CH422G initialization */
    ESP_RETURN_ON_ERROR(i2c_master_transmit(s_exp_config, &all_outputs, 1, 50), TAG, "CH422G mode");
    return exp_write(EXP_IDLE);
}

static esp_err_t panel_start(void)
{
    /* Reset the RGB panel through EXIO3. EXIO5 stays low so USB remains UART. */
    ESP_RETURN_ON_ERROR(exp_write(s_exp_state & ~EXP_LCD_RST), TAG, "LCD reset low");
    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_RETURN_ON_ERROR(exp_write(s_exp_state | EXP_LCD_RST), TAG, "LCD reset high");
    vTaskDelay(pdMS_TO_TICKS(100));

    const esp_lcd_rgb_panel_config_t cfg = {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .timings = {
            .pclk_hz = 16 * 1000 * 1000,
            .h_res = LCD_W,
            .v_res = LCD_H,
            .hsync_pulse_width = 4,
            .hsync_back_porch = 8,
            .hsync_front_porch = 8,
            .vsync_pulse_width = 4,
            .vsync_back_porch = 8,
            .vsync_front_porch = 8,
            .flags.pclk_active_neg = true,
        },
        .data_width = 16,
        .in_color_format = LCD_COLOR_FMT_RGB565,
        .out_color_format = LCD_COLOR_FMT_RGB565,
        .num_fbs = 1,
        .bounce_buffer_size_px = LCD_W * 10,
        .dma_burst_size = 64,
        .hsync_gpio_num = GPIO_NUM_46,
        .vsync_gpio_num = GPIO_NUM_3,
        .de_gpio_num = GPIO_NUM_5,
        .pclk_gpio_num = GPIO_NUM_7,
        .disp_gpio_num = GPIO_NUM_NC,
        .data_gpio_nums = {14, 38, 18, 17, 10, 39, 0, 45,
                          48, 47, 21, 1, 2, 42, 41, 40},
        .flags.fb_in_psram = true,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_rgb_panel(&cfg, &s_panel), TAG, "RGB panel");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "RGB start");
    return ESP_OK;
}

static esp_err_t touch_start(void)
{
    /* GT911 samples INT during reset to choose 0x5d. Waveshare drives it low. */
    gpio_config_t int_cfg = {
        .pin_bit_mask = BIT64(4),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&int_cfg), TAG, "touch INT output");
    ESP_RETURN_ON_ERROR(gpio_set_level(GPIO_NUM_4, 0), TAG, "touch address select");
    ESP_RETURN_ON_ERROR(exp_write(s_exp_state & ~EXP_TOUCH_RST), TAG, "touch reset low");
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_RETURN_ON_ERROR(exp_write(s_exp_state | EXP_TOUCH_RST), TAG, "touch reset high");
    vTaskDelay(pdMS_TO_TICKS(200));
    ESP_RETURN_ON_ERROR(gpio_reset_pin(GPIO_NUM_4), TAG, "touch INT input");

    uint8_t addr = 0x5d;
    if (i2c_master_probe(s_i2c, addr, 50) != ESP_OK) {
        addr = 0x14;
        ESP_RETURN_ON_ERROR(i2c_master_probe(s_i2c, addr, 50), TAG, "GT911 address");
    }
    ESP_LOGI(TAG, "GT911 at 0x%02x", addr);
    esp_lcd_panel_io_i2c_config_t io_cfg = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
    io_cfg.dev_addr = addr;
    io_cfg.scl_speed_hz = 400000;
    esp_lcd_panel_io_handle_t io;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(s_i2c, &io_cfg, &io), TAG, "touch I2C IO");
    esp_lcd_touch_io_gt911_config_t gt_cfg = {.dev_addr = addr};
    const esp_lcd_touch_config_t tp_cfg = {
        .x_max = LCD_W,
        .y_max = LCD_H,
        .rst_gpio_num = GPIO_NUM_NC,
        .int_gpio_num = GPIO_NUM_NC,
        .levels = {.reset = 0, .interrupt = 0},
        .driver_data = &gt_cfg,
    };
    return esp_lcd_touch_new_i2c_gt911(io, &tp_cfg, &s_touch);
}

static uint32_t tick_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void flush(lv_display_t *disp, const lv_area_t *area, uint8_t *px)
{
    (void)area;
    (void)px;
    /* LVGL renders directly into the RGB controller's continuously scanned FB. */
    lv_display_flush_ready(disp);
}

static void touch_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    data->state = LV_INDEV_STATE_RELEASED;
    if (esp_lcd_touch_read_data(s_touch) != ESP_OK) {
        return;
    }
    esp_lcd_touch_point_data_t point;
    uint8_t count = 0;
    if (esp_lcd_touch_get_data(s_touch, &point, &count, 1) != ESP_OK || !count) {
        s_was_pressed = false;
        return;
    }
    data->point.x = point.x < LCD_W ? point.x : LCD_W - 1;
    data->point.y = point.y < LCD_H ? point.y : LCD_H - 1;
    data->state = LV_INDEV_STATE_PRESSED;
    if (!s_was_pressed && muse_link_state() == MUSE_LINK_CONFIRM) {
        if (muse_link_talk_press()) {
            ESP_LOGI(TAG, "pairing confirmed by screen tap");
        }
    }
    s_was_pressed = true;
}

static esp_err_t display_init(void)
{
    ESP_RETURN_ON_ERROR(panel_start(), TAG, "panel");
    ESP_RETURN_ON_ERROR(touch_start(), TAG, "touch");

    void *fb = NULL;
    ESP_RETURN_ON_ERROR(esp_lcd_rgb_panel_get_frame_buffer(s_panel, 1, &fb), TAG, "frame buffer");
    ESP_RETURN_ON_FALSE(fb, ESP_ERR_NO_MEM, TAG, "no frame buffer");
    memset(fb, 0, LCD_W * LCD_H * 2);
    lv_init();
    lv_tick_set_cb(tick_ms);
    s_disp = lv_display_create(LCD_W, LCD_H);
    ESP_RETURN_ON_FALSE(s_disp, ESP_ERR_NO_MEM, TAG, "LVGL display");
    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(s_disp, fb, NULL, LCD_W * LCD_H * 2, LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(s_disp, flush);
    s_indev = lv_indev_create();
    ESP_RETURN_ON_FALSE(s_indev, ESP_ERR_NO_MEM, TAG, "LVGL touch");
    lv_indev_set_type(s_indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(s_indev, touch_read);
    lv_indev_set_display(s_indev, s_disp);
    return ESP_OK;
}

static void lvgl_task(void *arg)
{
    (void)arg;
    s_start_err = display_init();
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
        xTaskCreatePinnedToCoreWithCaps(lvgl_task, "lvgl", 8192, NULL, MUSE_UI_PRIORITY,
                                        NULL, MUSE_UI_CORE, MUSE_BIG_CAPS) != pdPASS) {
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
    return xSemaphoreTakeRecursive(s_lv_lock,
                                   timeout_ms < 0 ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

static void display_unlock(void)
{
    xSemaphoreGiveRecursive(s_lv_lock);
}

static void set_brightness(int pct)
{
    /* This board exposes a backlight switch, not PWM dimming. */
    uint8_t state = pct > 0 ? s_exp_state | EXP_BACKLIGHT : s_exp_state & ~EXP_BACKLIGHT;
    if (state != s_exp_state && exp_write(state) != ESP_OK) {
        ESP_LOGW(TAG, "backlight switch failed");
    }
}

static unsigned poll_buttons(void)
{
    return 0;
}

static esp_err_t power_off(void)
{
    set_brightness(0);
    /* GT911's active-low INT is an RTC-capable wake input on GPIO4. */
    ESP_RETURN_ON_ERROR(esp_sleep_enable_ext0_wakeup(GPIO_NUM_4, 0), TAG, "touch wake");
    esp_deep_sleep_start();
    return ESP_FAIL;
}

static const muse_board_t s_board = {
    .name = "Waveshare ESP32-S3-Touch-LCD-7",
    .width = LCD_W,
    .height = LCD_H,
    .round = false,
    .touch = true,
    .diagonal_in = 7.0f,
    .talk_button = "screen",
    .frame_ms = 40,
    .init = init,
    .display_start = display_start,
    .display_lock = display_lock,
    .display_unlock = display_unlock,
    .set_brightness = set_brightness,
    .poll_buttons = poll_buttons,
    .power_off = power_off,
};

const muse_board_t *muse_board_get(void)
{
    return &s_board;
}
