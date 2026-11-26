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
 * M5Stack StickS3: ESP32-S3-PICO-1-N8R8 (8 MB flash, 8 MB PSRAM), 1.14" 135x240
 * ST7789 LCD (no touch), ES8311 codec with one mic and an AW8737 speaker amp,
 * two buttons. M5's power chip (M5PM1, I2C 0x6E) owns the rails: its GPIO2
 * powers the LCD and the audio side (and connects the codec to I2C), its GPIO3
 * enables the amp, and it measures the battery and USB voltages. Pins are from
 * M5's K150 schematic (v0.6); the register bits were read back from UiFlow2.
 */
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_codec_dev_defaults.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "muse_audio.h"
#include "muse_board.h"
#include "muse_mem.h"
#include "muse_lvgl_hold.h"

static const char *TAG = "board";

#define LCD_W 135
#define LCD_H 240
#define LCD_GAP_X 52               /* the 135x240 window in the controller's 240x320 */
#define LCD_GAP_Y 40
#define LCD_HOST SPI3_HOST
#define LCD_SCLK GPIO_NUM_40
#define LCD_MOSI GPIO_NUM_39
#define LCD_CS GPIO_NUM_41
#define LCD_DC GPIO_NUM_45
#define LCD_RST GPIO_NUM_21
#define LCD_BL GPIO_NUM_38
#define DRAW_BUF_LINES 32

#define I2C_SDA GPIO_NUM_47
#define I2C_SCL GPIO_NUM_48
#define I2S_MCLK GPIO_NUM_18
#define I2S_BCLK GPIO_NUM_17
#define I2S_WS GPIO_NUM_15
#define I2S_DOUT GPIO_NUM_14
#define I2S_DIN GPIO_NUM_16

#define TALK_GPIO GPIO_NUM_11      /* KEY1, the big button under the screen */
#define AUX_GPIO GPIO_NUM_12       /* KEY2, on the side */

#define PMIC_ADDR 0x6E
#define PMIC_PWR_CFG 0x06          /* bit 3: 5 V out to Grove and IR */
#define PMIC_PWR_5V_OUT 0x08
#define PMIC_PWR_LED 0x10          /* the green status LED */
#define PMIC_GPIO_DIR 0x10
#define PMIC_GPIO_OUT 0x11
#define PMIC_GPIO_IN 0x12
#define PMIC_GPIO_OD 0x13          /* 1 = open drain, the PMIC's default */
#define PMIC_L3B BIT(2)            /* LCD and audio power */
#define PMIC_SPK BIT(3)            /* amp enable */
#define PMIC_CHG_IDLE BIT(0)       /* in PMIC_GPIO_IN: the charger's CHRG, low while charging */
#define PMIC_VBAT_MV 0x22          /* 16-bit little-endian millivolts */
#define PMIC_VIN_MV 0x24           /* USB */
#define SPK_PA_PIN 3               /* es8311's pa_pin, routed to the PMIC by s_pmic_gpio */

static i2c_master_bus_handle_t s_i2c;
static i2c_master_dev_handle_t s_pmic;
static esp_lcd_panel_handle_t s_panel;
static muse_gpio_button_t s_talk, s_aux;

/* The PMIC is a microcontroller and now and then misses a transfer; retry. */
static esp_err_t pmic_read(uint8_t reg, uint8_t *buf, size_t n)
{
    esp_err_t err = ESP_FAIL;
    for (int i = 0; i < 3 && err != ESP_OK; i++) {
        err = i2c_master_transmit_receive(s_pmic, &reg, 1, buf, n, 50);
    }
    return err;
}

static esp_err_t pmic_write(uint8_t reg, uint8_t val)
{
    const uint8_t buf[2] = { reg, val };
    esp_err_t err = ESP_FAIL;
    for (int i = 0; i < 3 && err != ESP_OK; i++) {
        err = i2c_master_transmit(s_pmic, buf, sizeof(buf), 50);
    }
    return err;
}

static esp_err_t pmic_update(uint8_t reg, uint8_t mask, bool on)
{
    uint8_t v;
    ESP_RETURN_ON_ERROR(pmic_read(reg, &v, 1), TAG, "pmic read %02x", reg);
    uint8_t want = on ? v | mask : v & ~mask;
    return want == v ? ESP_OK : pmic_write(reg, want);
}

static int pmic_mv(uint8_t reg)
{
    uint8_t b[2];
    return pmic_read(reg, b, sizeof(b)) == ESP_OK ? b[0] | b[1] << 8 : 0;
}

/* The codec driver switches the amp through this as the speaker opens and closes. */
static int pmic_gpio_setup(int16_t gpio, audio_gpio_dir_t dir, audio_gpio_mode_t mode)
{
    (void)gpio;
    (void)dir;
    (void)mode;
    return ESP_CODEC_DEV_OK;
}

static int pmic_gpio_set(int16_t gpio, bool high)
{
    (void)gpio;
    return pmic_update(PMIC_GPIO_OUT, PMIC_SPK, high) == ESP_OK ? ESP_CODEC_DEV_OK : ESP_CODEC_DEV_WRITE_FAIL;
}

static bool pmic_gpio_get(int16_t gpio)
{
    (void)gpio;
    uint8_t v = 0;
    pmic_read(PMIC_GPIO_OUT, &v, 1);
    return v & PMIC_SPK;
}

static const audio_codec_gpio_if_t s_pmic_gpio = {
    .setup = pmic_gpio_setup,
    .set = pmic_gpio_set,
    .get = pmic_gpio_get,
};

static esp_err_t init(void)
{
    const i2c_master_bus_config_t i2c_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = I2C_SDA,
        .scl_io_num = I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&i2c_cfg, &s_i2c), TAG, "i2c");
    const i2c_device_config_t pmic_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = PMIC_ADDR,
        .scl_speed_hz = 100000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_i2c, &pmic_cfg, &s_pmic), TAG, "pmic");

    /* LCD and audio power on, amp off until the speaker opens, and the 5 V
     * output off: nothing Muse uses hangs off it. The amp's enable has a
     * pull-down, so it needs driving high: open drain leaves the speaker
     * silent. The PMIC keeps this across ESP resets, so a board fresh from
     * UiFlow2 works without it until the PMIC itself resets. */
    ESP_RETURN_ON_ERROR(pmic_update(PMIC_GPIO_OD, PMIC_L3B | PMIC_SPK, false), TAG, "pmic gpio push-pull");
    ESP_RETURN_ON_ERROR(pmic_update(PMIC_GPIO_DIR, PMIC_L3B | PMIC_SPK, true), TAG, "pmic gpio dir");
    ESP_RETURN_ON_ERROR(pmic_update(PMIC_GPIO_OUT, PMIC_SPK, false), TAG, "amp off");
    ESP_RETURN_ON_ERROR(pmic_update(PMIC_GPIO_OUT, PMIC_L3B, true), TAG, "lcd and audio power");
    ESP_RETURN_ON_ERROR(pmic_update(PMIC_PWR_CFG, PMIC_PWR_5V_OUT, false), TAG, "5 V out off");
    /* The PMIC lights the LED by default, over a milliamp around the clock,
     * and Muse's screen already shows its state. */
    ESP_RETURN_ON_ERROR(pmic_update(PMIC_PWR_CFG, PMIC_PWR_LED, false), TAG, "led off");
    vTaskDelay(pdMS_TO_TICKS(20));

    ESP_RETURN_ON_ERROR(muse_gpio_button_init(&s_talk, TALK_GPIO), TAG, "talk button");
    ESP_RETURN_ON_ERROR(muse_gpio_button_init(&s_aux, AUX_GPIO), TAG, "aux button");
    /* A button that woke the board from power-off is still down; don't count it. */
    s_talk.pressed = gpio_get_level(TALK_GPIO) == 0;
    s_aux.pressed = gpio_get_level(AUX_GPIO) == 0;
    return ESP_OK;
}

static lv_display_t *display_start(lv_indev_t **touch)
{
    (void)touch;
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
        return NULL;
    }

    const spi_bus_config_t bus = {
        .sclk_io_num = LCD_SCLK,
        .mosi_io_num = LCD_MOSI,
        .miso_io_num = GPIO_NUM_NC,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = LCD_W * DRAW_BUF_LINES * 2,
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
        .reset_gpio_num = LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    if (esp_lcd_new_panel_st7789(io, &panel_cfg, &s_panel) != ESP_OK) {
        return NULL;
    }
    /* Portrait, the front button below the screen. */
    esp_lcd_panel_reset(s_panel);
    esp_lcd_panel_init(s_panel);
    esp_lcd_panel_invert_color(s_panel, true);
    esp_lcd_panel_set_gap(s_panel, LCD_GAP_X, LCD_GAP_Y);
    esp_lcd_panel_disp_on_off(s_panel, true);

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
            .hor_res = LCD_W,
            .ver_res = LCD_H,
            .buffer_height = DRAW_BUF_LINES,
            .use_psram = false,
            .require_double_buffer = true,
        },
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE,
    };
    lv_display_t *disp = esp_lv_adapter_register_display(&disp_cfg);
    if (!disp || esp_lv_adapter_start() != ESP_OK) {
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
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, pct * 1023 / 100);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

static void panel_sleep(bool sleep)
{
    esp_lcd_panel_disp_sleep(s_panel, sleep);   /* SLPIN/SLPOUT; GRAM is kept */
}

static void display_pause(bool pause)
{
    if (pause) {
        muse_lvgl_hold(MUSE_LVGL_HOLD_SCREEN, true);   /* bounded, shared with video */
    } else {
        muse_lvgl_hold(MUSE_LVGL_HOLD_SCREEN, false);
    }
}

/* One ES8311 does both directions over a duplex I2S bus, clocked from MCLK. */
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
            .mclk = I2S_MCLK,
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
    ESP_RETURN_ON_FALSE(data_if && ctrl_if, ESP_ERR_NO_MEM, TAG, "codec interfaces");

    es8311_codec_cfg_t es_cfg = {
        .ctrl_if = ctrl_if,
        .gpio_if = &s_pmic_gpio,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH,
        .pa_pin = SPK_PA_PIN,
        .use_mclk = true,
        .hw_gain = { .pa_voltage = 5.0, .codec_dac_voltage = 3.3 },
    };
    const audio_codec_if_t *codec = es8311_codec_new(&es_cfg);
    ESP_RETURN_ON_FALSE(codec, ESP_FAIL, TAG, "ES8311 not responding");

    esp_codec_dev_cfg_t out_cfg = { .dev_type = ESP_CODEC_DEV_TYPE_OUT, .codec_if = codec, .data_if = data_if };
    esp_codec_dev_cfg_t in_cfg = { .dev_type = ESP_CODEC_DEV_TYPE_IN, .codec_if = codec, .data_if = data_if };
    *spk = esp_codec_dev_new(&out_cfg);
    *mic = esp_codec_dev_new(&in_cfg);
    return *spk && *mic ? ESP_OK : ESP_FAIL;
}

static unsigned poll_buttons(void)
{
    return muse_gpio_button_poll(&s_talk) | muse_gpio_button_poll(&s_aux) << 2;
}

static void wait_buttons(int timeout_ms)
{
    muse_gpio_buttons_wait((muse_gpio_button_t *const[]){ &s_talk, &s_aux }, 2, timeout_ms);
}

/* The PMIC measures the battery and USB; the charger's CHRG pin reaches its GPIO0. */
static esp_err_t read_power(muse_power_t *out)
{
    uint8_t in;
    ESP_RETURN_ON_ERROR(pmic_read(PMIC_GPIO_IN, &in, 1), TAG, "pmic read");
    int v = pmic_mv(PMIC_VBAT_MV);
    out->usb = pmic_mv(PMIC_VIN_MV) > 4000;
    out->charging = out->usb && !(in & PMIC_CHG_IDLE);
    /* The charger releases CHRG once it terminates the charge; with
     * USB in and a cell on it, released is charge done. */
    out->charge_done = out->usb && v >= 2500 && (in & PMIC_CHG_IDLE);
    out->battery_mv = v;
    if (v < 2500) {
        out->battery_pct = -1;
        return ESP_OK;
    }
    /* The Watcher's LiPo curve; it agrees with UiFlow2's reading here. */
    int pct = (-v * v + 9016 * v - 19189000) / 10000;
    out->battery_pct = pct < 0 ? 0 : pct > 100 ? 100 : pct;
    return ESP_OK;
}

/*
 * Screen, audio and amp power off, then deep sleep until either button is
 * pressed; the ESP32-S3 draws microamps. Double-clicking the power button has
 * the PMIC cut the rest, and a click turns it back on.
 */
static esp_err_t power_off(void)
{
    set_brightness(0);
    esp_lcd_panel_disp_on_off(s_panel, false);
    pmic_update(PMIC_GPIO_OUT, PMIC_SPK | PMIC_L3B, false);
    while (gpio_get_level(TALK_GPIO) == 0 || gpio_get_level(AUX_GPIO) == 0) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    vTaskDelay(pdMS_TO_TICKS(50));
    /* The buttons' pull-ups are on the rail that stays up in deep sleep. */
    ESP_RETURN_ON_ERROR(esp_sleep_enable_ext1_wakeup_io(BIT64(TALK_GPIO) | BIT64(AUX_GPIO), ESP_EXT1_WAKEUP_ANY_LOW),
                        TAG, "button wake");
    esp_deep_sleep_start();
    return ESP_FAIL;
}

/* SAFE mode (muse_board_t.safe_display_off): the backlight off (active high: its PWM's duty 0). */
static void safe_display_off(void)
{
    muse_board_pin_hold(LCD_BL, 0);
}

/* The SAFE key (muse_board_t.safe_key: the way into and out of SAFE): KEY1 (the talk key), read raw. */
static bool safe_key(void)
{
    return muse_gpio_safe_key(TALK_GPIO, true);
}

static const muse_board_t s_board = {
    .name = "M5Stack StickS3",
    .width = LCD_W,
    .height = LCD_H,
    .round = false,
    .touch = false,
    .diagonal_in = 1.14f,
    .talk_button = "front",
    .aux_button = "side",
    /* Talk sits under the screen; the side button is on the right edge,
     * below the middle. */
    .talk_hint = { LV_ALIGN_BOTTOM_MID, 0, -4 },    /* the front button is under the screen */
    .aux_hint = { LV_ALIGN_RIGHT_MID, -2, 61 },     /* the side button spans y 142-229 */
    .frame_ms = 40,
    .init = init,
    .display_start = display_start,
    .display_lock = display_lock,
    .display_unlock = esp_lv_adapter_unlock,
    .set_brightness = set_brightness,
    .safe_display_off = safe_display_off,
    .panel_sleep = panel_sleep,
    .display_pause = display_pause,
    .audio_init = audio_init,
    .mic_slot = 0,              /* one mic, on the left slot */
    .poll_buttons = poll_buttons,
    .wait_buttons = wait_buttons,
    .read_power = read_power,
    .charged = MUSE_CHARGED_DONE,   /* the charger's CHRG through the PM1 */
    .power_off = power_off,
    .safe_key = safe_key,
};

/* Home Link's app_main starts Muse with this board (main/main.c). */
const muse_board_t *muse_board_get(void)
{
    return &s_board;
}
