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
 * Waveshare ESP32-S3-Touch-LCD-1.9: ESP32-S3 at 240 MHz, 16 MB flash, 8 MB
 * octal PSRAM, a 170x320 SH8601 panel on SPI3, CST816 touch on I2C, one I2S
 * microphone and one I2S amplifier, and the BOOT button on GPIO 0. No PMU or
 * battery gauge.
 *
 * Pins, the panel's init sequence and its 35 px column offset come from the
 * board's schematic and from the vendor firmware Waveshare ships for it. The
 * offset is the part that isn't guessable: the SH8601 is a 240-column
 * controller driving a 170-column panel, so columns 35..204 are the visible
 * ones. `esp_lcd_panel_set_gap()` applies it, which is why the draw path here
 * stays free of the manual `+35` the vendor firmware does in its flush.
 *
 * Audio has no codec chip to talk to: the amplifier is a MAX98357 and the
 * microphone an INMP441, both register-less I2S parts, and they are on
 * separate buses (amplifier tx on I2S0, microphone rx on I2S1) where Muse's
 * `audio_init` contract is one duplex bus. Both are therefore built on
 * `esp_codec_dev`'s dummy codec, one data interface per bus.
 */
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_codec_dev_defaults.h"   /* audio_codec_new_i2s_data, audio_codec_new_gpio */
#include "dummy_codec.h"              /* dummy_codec_new: the register-less codec */
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_sh8601.h"
#include "esp_lcd_touch.h"
#include "esp_lcd_touch_cst816s.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"

#include "muse_audio.h"   /* MUSE_AUDIO_RATE */
#include "muse_board.h"
#include "muse_mem.h"     /* MUSE_UI_CORE, MUSE_UI_PRIORITY */

static const char *TAG = "board";

#define LCD_HOST SPI3_HOST
#define LCD_SCLK GPIO_NUM_10
#define LCD_MOSI GPIO_NUM_13
#define LCD_CS GPIO_NUM_12
#define LCD_DC GPIO_NUM_11
#define LCD_RST GPIO_NUM_9
#define LCD_BL GPIO_NUM_14

#define LCD_H_RES 170
#define LCD_V_RES 320
/* SH8601 is a 240-column controller; this panel uses 35..204. */
#define LCD_X_GAP 35
#define LCD_PCLK_HZ (20 * 1000 * 1000)
/* Two of these: one draws while the other goes out over SPI. */
#define DRAW_BUF_LINES 40

#define BOOT_GPIO GPIO_NUM_0

/* Touch: a CST816S behind I2C, polled. The board wires no reset or interrupt
 * line to it, so both are left unconnected here and the adapter reads it on a
 * timer. */
#define TOUCH_SDA GPIO_NUM_47
#define TOUCH_SCL GPIO_NUM_48

/* Audio. Neither part has registers: the amplifier is a MAX98357 (I2S class D)
 * and the microphone an INMP441 (I2S MEMS), so there is nothing to configure
 * over I2C and no codec chip on the board at all. They also do not share a
 * bus - the amplifier is transmit-only on I2S0, the microphone receive-only on
 * I2S1 - which is why each gets its own data interface below. */
#define AMP_BCLK GPIO_NUM_15
#define AMP_WS GPIO_NUM_16
#define AMP_DOUT GPIO_NUM_18
#define MIC_BCLK GPIO_NUM_5
#define MIC_WS GPIO_NUM_6
#define MIC_DIN GPIO_NUM_1

#define BL_LEDC_MODE LEDC_LOW_SPEED_MODE
#define BL_LEDC_CHANNEL LEDC_CHANNEL_0
#define BL_LEDC_TIMER LEDC_TIMER_0
#define BL_LEDC_RES LEDC_TIMER_10_BIT
#define BL_DUTY_MAX ((1u << 10) - 1u)

/* Panel init, from the vendor firmware. The vendor config header warns that the
 * 36h command is overwritten by an external init sequence, so the one below is
 * the whole story rather than a partial override of the driver's defaults. */
static const sh8601_lcd_init_cmd_t s_init_cmds[] = {
    {0x36, (uint8_t[]){0x00}, 1, 0},
    {0xb2, (uint8_t[]){0x0c, 0x0c, 0x00, 0x33, 0x33}, 5, 0},
    {0xb7, (uint8_t[]){0x35}, 1, 0},
    {0xbb, (uint8_t[]){0x13}, 1, 0},
    {0xc0, (uint8_t[]){0x2c}, 1, 0},
    {0xc2, (uint8_t[]){0x01}, 1, 0},
    {0xc3, (uint8_t[]){0x0b}, 1, 0},
    {0xc4, (uint8_t[]){0x20}, 1, 0},
    {0xc6, (uint8_t[]){0x0f}, 1, 0},
    {0xd0, (uint8_t[]){0xa4, 0xa1}, 2, 0},
    {0xd6, (uint8_t[]){0xa1}, 1, 0},
    {0xe0, (uint8_t[]){0x00, 0x03, 0x07, 0x08, 0x07, 0x15, 0x2A, 0x44,
                       0x42, 0x0A, 0x17, 0x18, 0x25, 0x27}, 14, 0},
    {0xe1, (uint8_t[]){0x00, 0x03, 0x08, 0x07, 0x07, 0x23, 0x2A, 0x43,
                       0x42, 0x09, 0x18, 0x17, 0x25, 0x27}, 14, 0},
    {0x21, (uint8_t[]){0x21}, 0, 0},
    {0x11, (uint8_t[]){0x11}, 0, 120},
    {0x29, (uint8_t[]){0x29}, 0, 0},
};

static esp_lcd_panel_handle_t s_panel;
static esp_lcd_panel_io_handle_t s_io;
static i2c_master_bus_handle_t s_i2c;
static muse_gpio_button_t s_boot;

/* Brings up the CST816S and hands LVGL its input device. Failure is not fatal:
 * without it the BOOT button still opens and drives the menu. */
static esp_err_t touch_start(lv_display_t *disp, lv_indev_t **out)
{
    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = TOUCH_SDA,
        .scl_io_num = TOUCH_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &s_i2c), TAG, "touch i2c");

    const esp_lcd_panel_io_i2c_config_t io_cfg = ESP_LCD_TOUCH_IO_I2C_CST816S_CONFIG();
    esp_lcd_panel_io_handle_t io = NULL;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(s_i2c, &io_cfg, &io), TAG, "touch io");

    const esp_lcd_touch_config_t tp_cfg = {
        .x_max = LCD_H_RES,
        .y_max = LCD_V_RES,
        .rst_gpio_num = GPIO_NUM_NC,
        .int_gpio_num = GPIO_NUM_NC,
        .levels = { .reset = 0, .interrupt = 0 },
        .flags = { .swap_xy = 0, .mirror_x = 0, .mirror_y = 0 },
    };
    esp_lcd_touch_handle_t tp = NULL;
    ESP_RETURN_ON_ERROR(esp_lcd_touch_new_i2c_cst816s(io, &tp_cfg, &tp), TAG, "cst816s");

    const esp_lv_adapter_touch_config_t touch_cfg =
        ESP_LV_ADAPTER_TOUCH_DEFAULT_CONFIG(disp, tp);
    *out = esp_lv_adapter_register_touch(&touch_cfg);
    ESP_RETURN_ON_FALSE(*out, ESP_FAIL, TAG, "register touch");
    ESP_LOGI(TAG, "touch: CST816S at 0x15 on I2C0 (%d/%d)", TOUCH_SDA, TOUCH_SCL);
    return ESP_OK;
}

static esp_err_t init(void)
{
    /* The backlight is active low on this board: the vendor firmware drives
     * GPIO 14 to 0 to light the panel and leaves it there. So the duty cycle
     * below is inverted, and "off until the panel is up" means a full high. */
    const ledc_timer_config_t timer = {
        .speed_mode = BL_LEDC_MODE,
        .duty_resolution = BL_LEDC_RES,
        .timer_num = BL_LEDC_TIMER,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), TAG, "backlight timer");
    const ledc_channel_config_t channel = {
        .gpio_num = LCD_BL,
        .speed_mode = BL_LEDC_MODE,
        .channel = BL_LEDC_CHANNEL,
        .timer_sel = BL_LEDC_TIMER,
        .duty = BL_DUTY_MAX,        /* high = backlight off */
        .hpoint = 0,
    };
    ESP_RETURN_ON_ERROR(ledc_channel_config(&channel), TAG, "backlight channel");

    ESP_RETURN_ON_ERROR(muse_gpio_button_init(&s_boot, BOOT_GPIO), TAG, "boot button");
    return ESP_OK;
}

static lv_display_t *display_start(lv_indev_t **touch)
{
    *touch = NULL;

    const spi_bus_config_t bus = {
        .sclk_io_num = LCD_SCLK,
        .mosi_io_num = LCD_MOSI,
        .miso_io_num = GPIO_NUM_NC,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = LCD_H_RES * DRAW_BUF_LINES * (int)sizeof(uint16_t),
    };
    if (spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) {
        ESP_LOGE(TAG, "spi bus");
        return NULL;
    }
    const esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = LCD_CS,
        .dc_gpio_num = LCD_DC,
        .spi_mode = 0,
        .pclk_hz = LCD_PCLK_HZ,
        .trans_queue_depth = 32,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    if (esp_lcd_new_panel_io_spi(LCD_HOST, &io_cfg, &s_io) != ESP_OK) {
        ESP_LOGE(TAG, "panel io");
        return NULL;
    }
    /* Non-const: esp_lcd_panel_dev_config_t.vendor_config is a plain void *. */
    sh8601_vendor_config_t vendor = {
        .init_cmds = s_init_cmds,
        .init_cmds_size = sizeof(s_init_cmds) / sizeof(s_init_cmds[0]),
    };
    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor,
        .data_endian = LCD_RGB_DATA_ENDIAN_BIG,
    };
    if (esp_lcd_new_panel_sh8601(s_io, &panel_cfg, &s_panel) != ESP_OK) {
        ESP_LOGE(TAG, "panel");
        return NULL;
    }
    esp_lcd_panel_reset(s_panel);
    esp_lcd_panel_init(s_panel);
    /* The 35 invisible columns. Without this the image sits 35 px left. */
    esp_lcd_panel_set_gap(s_panel, LCD_X_GAP, 0);
    esp_lcd_panel_disp_on_off(s_panel, true);

    esp_lv_adapter_config_t adapter_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    adapter_cfg.task_core_id = MUSE_UI_CORE;
    adapter_cfg.task_priority = MUSE_UI_PRIORITY;
    if (esp_lv_adapter_init(&adapter_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "lv adapter");
        return NULL;
    }
    const esp_lv_adapter_display_config_t disp_cfg = {
        .panel = s_panel,
        .panel_io = s_io,
        .profile = {
            .interface = ESP_LV_ADAPTER_PANEL_IF_OTHER,
            .rotation = ESP_LV_ADAPTER_ROTATE_0,
            .hor_res = LCD_H_RES,
            .ver_res = LCD_V_RES,
            .buffer_height = DRAW_BUF_LINES,
            /* The vendor firmware keeps its draw buffers in internal DMA RAM
             * and notes that 80-line buffers starve NimBLE; 40 lines are what
             * it settled on for this panel. */
            .use_psram = false,
            .require_double_buffer = true,
        },
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE,
    };
    lv_display_t *disp = esp_lv_adapter_register_display(&disp_cfg);
    if (!disp || esp_lv_adapter_start() != ESP_OK) {
        ESP_LOGE(TAG, "lv display");
        return NULL;
    }
    if (touch_start(disp, touch) != ESP_OK) {
        ESP_LOGW(TAG, "no touch: the BOOT button drives the menu");
        *touch = NULL;
    }
    return disp;
}

static bool display_lock(int timeout_ms)
{
    return esp_lv_adapter_lock(timeout_ms) == ESP_OK;
}

static void set_brightness(int pct)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    /* Inverted: the backlight is on while the pin is low, so 100 % brightness
     * is a zero duty and 0 % is a full one. */
    ledc_set_duty(BL_LEDC_MODE, BL_LEDC_CHANNEL, (uint32_t)(100 - pct) * BL_DUTY_MAX / 100);
    ledc_update_duty(BL_LEDC_MODE, BL_LEDC_CHANNEL);
}

static void panel_sleep(bool sleep)
{
    esp_lcd_panel_disp_sleep(s_panel, sleep);   /* SLPIN/SLPOUT; GRAM is kept */
}

static void display_pause(bool pause)
{
    if (pause) {
        esp_lv_adapter_pause(-1);
    } else {
        esp_lv_adapter_resume();
    }
}

static unsigned poll_buttons(void)
{
    /* One button, so it is the talk button: it confirms pairing and opens the
     * menu, the way the two-button boards' talk button does. */
    return muse_gpio_button_poll(&s_boot);
}

static void wait_buttons(int timeout_ms)
{
    muse_gpio_button_t *const buttons[] = {&s_boot};
    muse_gpio_buttons_wait(buttons, 1, timeout_ms);
}

/* Both chips are register-less, so both devices are built on the dummy codec:
 * the I2S data interface does the work and `pa_pin = -1` says there is no
 * amplifier enable line to drive. Muse wants one duplex bus, but this board
 * has two single-direction ones, so each gets its own data interface and its
 * own esp_codec_dev handle. */
static esp_err_t audio_init(esp_codec_dev_handle_t *spk, esp_codec_dev_handle_t *mic)
{
    i2s_chan_handle_t amp_tx = NULL;
    i2s_chan_config_t amp_chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    amp_chan.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&amp_chan, &amp_tx, NULL), TAG, "amp channel");
    const i2s_std_config_t amp_std = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(MUSE_AUDIO_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = AMP_BCLK,
            .ws = AMP_WS,
            .dout = AMP_DOUT,
            .din = I2S_GPIO_UNUSED,
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(amp_tx, &amp_std), TAG, "amp std");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(amp_tx), TAG, "amp enable");

    i2s_chan_handle_t mic_rx = NULL;
    i2s_chan_config_t mic_chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    mic_chan.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&mic_chan, NULL, &mic_rx), TAG, "mic channel");
    const i2s_std_config_t mic_std = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(MUSE_AUDIO_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = MIC_BCLK,
            .ws = MIC_WS,
            .dout = I2S_GPIO_UNUSED,
            .din = MIC_DIN,
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(mic_rx, &mic_std), TAG, "mic std");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(mic_rx), TAG, "mic enable");

    audio_codec_i2s_cfg_t amp_i2s = { .port = I2S_NUM_0, .tx_handle = amp_tx };
    audio_codec_i2s_cfg_t mic_i2s = { .port = I2S_NUM_1, .rx_handle = mic_rx };
    const audio_codec_data_if_t *amp_data = audio_codec_new_i2s_data(&amp_i2s);
    const audio_codec_data_if_t *mic_data = audio_codec_new_i2s_data(&mic_i2s);
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    ESP_RETURN_ON_FALSE(amp_data && mic_data && gpio_if, ESP_ERR_NO_MEM, TAG, "codec interfaces");

    dummy_codec_cfg_t amp_dummy = { .gpio_if = gpio_if, .pa_pin = -1 };
    dummy_codec_cfg_t mic_dummy = { .gpio_if = gpio_if, .pa_pin = -1 };
    const audio_codec_if_t *amp_codec = dummy_codec_new(&amp_dummy);
    const audio_codec_if_t *mic_codec = dummy_codec_new(&mic_dummy);
    ESP_RETURN_ON_FALSE(amp_codec && mic_codec, ESP_FAIL, TAG, "dummy codec");

    /* Non-const: esp_codec_dev_new takes a plain esp_codec_dev_cfg_t *. */
    esp_codec_dev_cfg_t out_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = amp_codec,
        .data_if = amp_data,
    };
    esp_codec_dev_cfg_t in_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN,
        .codec_if = mic_codec,
        .data_if = mic_data,
    };
    *spk = esp_codec_dev_new(&out_cfg);
    *mic = esp_codec_dev_new(&in_cfg);
    ESP_RETURN_ON_FALSE(*spk && *mic, ESP_FAIL, TAG, "codec dev");
    ESP_LOGI(TAG, "audio: MAX98357 on I2S0 tx, INMP441 on I2S1 rx");
    return ESP_OK;
}

/* No PMU and no power latch: there is nothing to switch off. */
static esp_err_t power_off(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

static const muse_board_t s_board = {
    .name = "Waveshare ESP32-S3-Touch-LCD-1.9",
    .width = LCD_H_RES,
    .height = LCD_V_RES,
    .round = false,
    .touch = true,
    .diagonal_in = 1.9f,
    .keyboard = false,
    .talk_button = "boot",
    .aux_button = NULL,
    /* The BOOT button is on the right edge, roughly a third down. */
    .talk_hint = { LV_ALIGN_RIGHT_MID, -8, 0 },
    .aux_hint = { LV_ALIGN_RIGHT_MID, 0, 0 },
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
    .read_power = NULL,         /* no battery gauge */
    .power_off = power_off,
};

/* Home Link's app_main starts Muse with this board (main/main.c). */
const muse_board_t *muse_board_get(void)
{
    return &s_board;
}
