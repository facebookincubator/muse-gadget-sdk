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
 * Elecrow CrowPanel Advance 4.3 V1.3: ESP32-S3-WROOM-1-N16R8, 800x480 ST7265
 * RGB panel, GT911 touch, NS4168 speaker amplifier and PDM microphone.
 * Pins and RGB timing are from Elecrow's factory_sourcecode/LovyanGFX_Driver.h;
 * the V1.3 schematic/readme documents the STC8 commands and touch recovery:
 * https://github.com/Elecrow-RD/CrowPanel-Advance-4.3-HMI-ESP32-S3-AI-Powered-IPS-Touch-Screen-800x480
 */
#include "driver/i2c_master.h"
#include "driver/i2s_pdm.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_codec_dev_defaults.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <math.h>

#include "muse_audio.h"
#include "muse_board.h"
#include "muse_mem.h"

static const char *TAG = "board";

#define LCD_W 800
#define LCD_H 480
#define LCD_PCLK_HZ (16 * 1000 * 1000)
#define LCD_DRAW_LINES 40

#define I2C_SDA GPIO_NUM_15
#define I2C_SCL GPIO_NUM_16
#define I2C_HZ 400000
#define STC8_ADDR 0x30
#define GT911_ADDR ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS

#define TALK_GPIO GPIO_NUM_0
#define TOUCH_RECOVERY_GPIO GPIO_NUM_1

static i2c_master_bus_handle_t s_i2c;
static i2c_master_dev_handle_t s_stc8;
static esp_lcd_panel_handle_t s_panel;
static esp_lcd_panel_io_handle_t s_touch_io;
static esp_lcd_touch_handle_t s_touch;
static muse_gpio_button_t s_talk;
static i2s_chan_handle_t s_mic_rx;

static esp_err_t stc8_command(uint8_t command)
{
    return i2c_master_transmit(s_stc8, &command, sizeof(command), 100);
}

static esp_err_t init(void)
{
    const i2c_master_bus_config_t i2c_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = I2C_SDA,
        .scl_io_num = I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&i2c_cfg, &s_i2c), TAG, "i2c bus");

    const i2c_device_config_t stc8_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = STC8_ADDR,
        .scl_speed_hz = I2C_HZ,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_i2c, &stc8_cfg, &s_stc8), TAG, "STC8 device");
    ESP_RETURN_ON_ERROR(muse_gpio_button_init(&s_talk, TALK_GPIO), TAG, "talk button");
    s_talk.pressed = gpio_get_level(TALK_GPIO) == 0;

    /* Command 0 is maximum backlight; 245 is off. */
    ESP_RETURN_ON_ERROR(stc8_command(0), TAG, "backlight on");
    return ESP_OK;
}

static void set_brightness(int pct)
{
    pct = pct < 0 ? 0 : pct > 100 ? 100 : pct;
    const uint8_t command = (uint8_t)((100 - pct) * 245 / 100);
    esp_err_t err = stc8_command(command);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set backlight to %d%%: %s", pct, esp_err_to_name(err));
    }
}

static esp_err_t recover_touch(void)
{
    const gpio_config_t pulse_cfg = {
        .pin_bit_mask = BIT64(TOUCH_RECOVERY_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(stc8_command(250), TAG, "STC8 touch recovery");
    ESP_RETURN_ON_ERROR(gpio_config(&pulse_cfg), TAG, "touch recovery pin");
    ESP_RETURN_ON_ERROR(gpio_set_level(TOUCH_RECOVERY_GPIO, 0), TAG, "touch recovery low");
    vTaskDelay(pdMS_TO_TICKS(20));
    ESP_RETURN_ON_ERROR(gpio_reset_pin(TOUCH_RECOVERY_GPIO), TAG, "touch recovery release");
    vTaskDelay(pdMS_TO_TICKS(100));
    return i2c_master_probe(s_i2c, GT911_ADDR, 100);
}

static void start_touch(lv_display_t *display, lv_indev_t **touch)
{
    esp_err_t err = i2c_master_probe(s_i2c, GT911_ADDR, 100);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "GT911 not responding; requesting the documented recovery");
        err = recover_touch();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GT911 unavailable after recovery: %s", esp_err_to_name(err));
        return;
    }

    esp_lcd_panel_io_i2c_config_t io_cfg = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
    io_cfg.scl_speed_hz = I2C_HZ;
    err = esp_lcd_new_panel_io_i2c(s_i2c, &io_cfg, &s_touch_io);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "create GT911 I2C interface: %s", esp_err_to_name(err));
        return;
    }

    const esp_lcd_touch_config_t touch_cfg = {
        .x_max = LCD_W,
        .y_max = LCD_H,
        .rst_gpio_num = GPIO_NUM_NC,
        .int_gpio_num = GPIO_NUM_NC,
    };
    err = esp_lcd_touch_new_i2c_gt911(s_touch_io, &touch_cfg, &s_touch);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "create GT911 driver: %s", esp_err_to_name(err));
        return;
    }

    const esp_lv_adapter_touch_config_t adapter_touch_cfg = ESP_LV_ADAPTER_TOUCH_DEFAULT_CONFIG(display, s_touch);
    *touch = esp_lv_adapter_register_touch(&adapter_touch_cfg);
    if (!*touch) {
        ESP_LOGE(TAG, "register GT911 with LVGL failed");
    }
}

static lv_display_t *display_start(lv_indev_t **touch)
{
    const esp_lv_adapter_tear_avoid_mode_t tear_avoid_mode =
        ESP_LV_ADAPTER_TEAR_AVOID_MODE_DEFAULT_RGB;
    const esp_lv_adapter_rotation_t rotation = ESP_LV_ADAPTER_ROTATE_0;
    static const gpio_num_t data_pins[] = {
        GPIO_NUM_21, GPIO_NUM_47, GPIO_NUM_48, GPIO_NUM_45,
        GPIO_NUM_38, GPIO_NUM_9, GPIO_NUM_10, GPIO_NUM_11,
        GPIO_NUM_12, GPIO_NUM_13, GPIO_NUM_14, GPIO_NUM_7,
        GPIO_NUM_17, GPIO_NUM_18, GPIO_NUM_3, GPIO_NUM_46,
    };
    esp_lcd_rgb_panel_config_t panel_cfg = {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .timings = {
            .pclk_hz = LCD_PCLK_HZ,
            .h_res = LCD_W,
            .v_res = LCD_H,
            .hsync_pulse_width = 4,
            .hsync_back_porch = 8,
            .hsync_front_porch = 8,
            .vsync_pulse_width = 4,
            .vsync_back_porch = 8,
            .vsync_front_porch = 8,
            .flags = {
                .hsync_idle_low = 1,
                .vsync_idle_low = 1,
                .pclk_idle_high = 1,
            },
        },
        .data_width = 16,
        .in_color_format = LCD_COLOR_FMT_RGB565,
        .out_color_format = LCD_COLOR_FMT_RGB565,
        .num_fbs = esp_lv_adapter_get_required_frame_buffer_count(tear_avoid_mode, rotation),
        .bounce_buffer_size_px = LCD_W * 10,
        .dma_burst_size = 64,
        .hsync_gpio_num = GPIO_NUM_40,
        .vsync_gpio_num = GPIO_NUM_41,
        .de_gpio_num = GPIO_NUM_42,
        .pclk_gpio_num = GPIO_NUM_39,
        .disp_gpio_num = GPIO_NUM_NC,
        .flags.fb_in_psram = 1,
    };
    for (size_t i = 0; i < sizeof(data_pins) / sizeof(data_pins[0]); i++) {
        panel_cfg.data_gpio_nums[i] = data_pins[i];
    }

    esp_err_t err = esp_lcd_new_rgb_panel(&panel_cfg, &s_panel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "create RGB panel: %s", esp_err_to_name(err));
        return NULL;
    }
    if ((err = esp_lcd_panel_reset(s_panel)) != ESP_OK ||
        (err = esp_lcd_panel_init(s_panel)) != ESP_OK) {
        ESP_LOGE(TAG, "initialize RGB panel: %s", esp_err_to_name(err));
        return NULL;
    }

    esp_lv_adapter_config_t adapter_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    adapter_cfg.task_core_id = MUSE_UI_CORE;
    adapter_cfg.task_priority = MUSE_UI_PRIORITY;
    if ((err = esp_lv_adapter_init(&adapter_cfg)) != ESP_OK) {
        ESP_LOGE(TAG, "initialize LVGL adapter: %s", esp_err_to_name(err));
        return NULL;
    }
    const esp_lv_adapter_display_config_t display_cfg = {
        .panel = s_panel,
        .panel_io = NULL,
        .profile = {
            .interface = ESP_LV_ADAPTER_PANEL_IF_RGB,
            .rotation = rotation,
            .hor_res = LCD_W,
            .ver_res = LCD_H,
            .buffer_height = LCD_DRAW_LINES,
            .use_psram = true,
            .require_double_buffer = true,
        },
        .tear_avoid_mode = tear_avoid_mode,
    };
    lv_display_t *display = esp_lv_adapter_register_display(&display_cfg);
    if (!display) {
        ESP_LOGE(TAG, "register RGB display failed");
        return NULL;
    }
    if ((err = esp_lv_adapter_start()) != ESP_OK) {
        ESP_LOGE(TAG, "start LVGL display: %s", esp_err_to_name(err));
        return NULL;
    }

    *touch = NULL;
    start_touch(display, touch);
    return display;
}

static bool display_lock(int timeout_ms)
{
    return esp_lv_adapter_lock(timeout_ms) == ESP_OK;
}

static esp_err_t audio_init(esp_codec_dev_handle_t *spk, esp_codec_dev_handle_t *mic)
{
    i2s_chan_handle_t tx;
    i2s_chan_config_t tx_chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    tx_chan_cfg.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&tx_chan_cfg, &tx, NULL), TAG, "speaker I2S channel");
    i2s_chan_config_t rx_chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    rx_chan_cfg.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&rx_chan_cfg, NULL, &s_mic_rx), TAG, "microphone I2S channel");

    const i2s_std_config_t tx_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(MUSE_AUDIO_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = GPIO_NUM_5,
            .ws = GPIO_NUM_6,
            .dout = GPIO_NUM_4,
            .din = I2S_GPIO_UNUSED,
        },
    };
    const i2s_pdm_rx_config_t rx_cfg = {
        .clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(MUSE_AUDIO_RATE),
        .slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .clk = GPIO_NUM_19,
            .din = GPIO_NUM_20,
        },
    };
    /* Select the microphone path before starting its PDM clock. */
    ESP_RETURN_ON_ERROR(stc8_command(2), TAG, "microphone path");
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(tx, &tx_cfg), TAG, "speaker I2S");
    ESP_RETURN_ON_ERROR(i2s_channel_init_pdm_rx_mode(s_mic_rx, &rx_cfg), TAG, "microphone PDM");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(tx), TAG, "speaker I2S enable");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_mic_rx), TAG, "microphone PDM enable");

    /* STC8 command 248 enables the speaker amplifier. */
    ESP_RETURN_ON_ERROR(stc8_command(248), TAG, "speaker amplifier");
    audio_codec_i2s_cfg_t tx_data_cfg = { .port = I2S_NUM_1, .tx_handle = tx };
    const audio_codec_data_if_t *tx_data = audio_codec_new_i2s_data(&tx_data_cfg);
    ESP_RETURN_ON_FALSE(tx_data, ESP_ERR_NO_MEM, TAG, "speaker data interface");

    esp_codec_dev_cfg_t tx_dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = NULL,
        .data_if = tx_data,
    };
    *spk = esp_codec_dev_new(&tx_dev_cfg);
    *mic = NULL;
    return *spk ? ESP_OK : ESP_ERR_NO_MEM;
}

static esp_err_t read_mic(int16_t *mono, size_t frames, int gain_db)
{
    size_t bytes_read = 0;
    esp_err_t err = i2s_channel_read(s_mic_rx, mono, frames * sizeof(*mono), &bytes_read, pdMS_TO_TICKS(1000));
    if (err != ESP_OK) {
        return err;
    }
    if (bytes_read != frames * sizeof(*mono)) {
        return ESP_ERR_INVALID_SIZE;
    }
    float gain = powf(10.0f, (gain_db < 0 ? 0 : gain_db > 36 ? 36 : gain_db) / 20.0f);
    for (size_t i = 0; i < frames; i++) {
        float sample = mono[i] * gain;
        mono[i] = (int16_t)(sample > 32767.0f ? 32767 : sample < -32768.0f ? -32768 : sample);
    }
    return ESP_OK;
}

static unsigned poll_buttons(void)
{
    return muse_gpio_button_poll(&s_talk);
}

static esp_err_t power_off(void)
{
    set_brightness(0);
    while (gpio_get_level(TALK_GPIO) == 0) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    vTaskDelay(pdMS_TO_TICKS(50));
    ESP_RETURN_ON_ERROR(esp_sleep_enable_ext0_wakeup(TALK_GPIO, 0), TAG, "BOOT wake");
    esp_deep_sleep_start();
    return ESP_FAIL;
}

static const muse_board_t s_board = {
    .name = "Elecrow CrowPanel Advance 4.3 V1.3",
    .width = LCD_W,
    .height = LCD_H,
    .round = false,
    .touch = true,
    .diagonal_in = 4.3f,
    .talk_button = "boot",
    .talk_hint = { LV_ALIGN_TOP_LEFT, 8, 8 },
    .frame_ms = 40,
    .init = init,
    .display_start = display_start,
    .display_lock = display_lock,
    .display_unlock = esp_lv_adapter_unlock,
    .set_brightness = set_brightness,
    .audio_init = audio_init,
    .read_mic = read_mic,
    .mic_slot = 0,
    .poll_buttons = poll_buttons,
    .power_off = power_off,
};

const muse_board_t *muse_board_get(void)
{
    return &s_board;
}
