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
 * DFRobot UNIHIKER K10 (DFR0992): ESP32-S3 N16R8, 16 MB flash, 8 MB octal
 * PSRAM, 2.8" 240x320 ILI9341, dual MEMS mics, 2 W speaker, 3 WS2812s,
 * buttons A and B. No touch.
 *
 * Pins are from DFRobot's Arduino core, not guessed:
 *   variants/unihiker_k10/pins_arduino.h
 *   libraries/unihiker_k10/src/unihiker_k10.h  (I2S, WS2812)
 *   tools/sdk/esp32s3/include/modules/lcd/who_lcd.h
 *   tools/sdk/esp32s3/include/modules/board/initBoard.h
 *   libraries/TFT_eSPI/User_Setup.h
 * in https://github.com/DFRobot/framework-arduinounihiker
 * Button A is expander pin 12 and button B is pin 2 (unihiker_k10.cpp).
 * The XL9535/TCA9555 at 0x20 matches a board I2C scan (IN port map in
 * MicroPythonOS unihiker_k10). ES7243E is the ADC at 0x11 (chip id 0x7A43
 * on that scan); DFRobot's libmodules.a calls es7243e_adc_init. The speaker
 * amp enable is expander pin 15, driven high while audio plays
 * (digital_write(eAmp_Gain, 1) in unihiker_k10.cpp).
 *
 * Not driven here: GC2145 camera (pins are in who_camera.h; Muse's
 * camera.capture path is the Watcher's), microSD, AHT20, LTR303ALS,
 * SC7A20H, and the battery connector. There is no confirmed battery-sense
 * pin in the vendor headers.
 */
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_codec_dev_defaults.h"
#include "esp_lcd_ili9341.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_strip.h"

#include "muse_audio.h"
#include "muse_board.h"
#include "muse_link.h"
#include "muse_mem.h"
#include "muse_state.h"

static const char *TAG = "board";

#define LCD_H_RES 240
#define LCD_V_RES 320
#define LCD_HOST SPI2_HOST
#define LCD_SCLK GPIO_NUM_12
#define LCD_MOSI GPIO_NUM_21
#define LCD_CS GPIO_NUM_14
#define LCD_DC GPIO_NUM_13
#define LCD_RST GPIO_NUM_NC
#define DRAW_BUF_LINES 32

#define I2C_SDA GPIO_NUM_47
#define I2C_SCL GPIO_NUM_48
#define EXP_ADDR 0x20
#define ES7243E_ADDR 0x11

/* initBoard.h enum order is the expander bit number. */
#define PIN_BL 0
#define PIN_CAM_RST 1
#define PIN_BTN_B 2
#define PIN_BTN_A 12
#define PIN_AMP 15

#define I2S_MCLK GPIO_NUM_3
#define I2S_BCLK GPIO_NUM_0
#define I2S_WS GPIO_NUM_38
#define I2S_DOUT GPIO_NUM_45
#define I2S_DIN GPIO_NUM_39

#define LED_GPIO GPIO_NUM_46
#define LED_COUNT 3
#define BOOT_WAKE_GPIO GPIO_NUM_0

#define DEBOUNCE_SAMPLES 3

static i2c_master_bus_handle_t s_i2c;
static i2c_master_dev_handle_t s_exp;
static SemaphoreHandle_t s_exp_mu;
static uint16_t s_out;
static uint16_t s_cfg = 0xFFFF;
static esp_lcd_panel_handle_t s_panel;
static led_strip_handle_t s_leds;
static i2s_chan_handle_t s_tx, s_rx;
static const audio_codec_data_if_t *s_i2s_if;

typedef struct {
    int pin;
    bool pressed;
    uint8_t stable;
} exp_btn_t;

static exp_btn_t s_talk = { .pin = PIN_BTN_A };
static exp_btn_t s_aux = { .pin = PIN_BTN_B };

static esp_err_t exp_commit_locked(void)
{
    uint8_t cfg[3] = { 0x06, (uint8_t)s_cfg, (uint8_t)(s_cfg >> 8) };
    uint8_t out[3] = { 0x02, (uint8_t)s_out, (uint8_t)(s_out >> 8) };
    esp_err_t err = i2c_master_transmit(s_exp, cfg, sizeof(cfg), 50);
    if (err == ESP_OK) {
        err = i2c_master_transmit(s_exp, out, sizeof(out), 50);
    }
    return err;
}

static esp_err_t exp_set(int pin, bool high)
{
    if (!s_exp_mu) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_exp_mu, portMAX_DELAY);
    s_cfg &= ~(1u << pin);
    if (high) {
        s_out |= (uint16_t)(1u << pin);
    } else {
        s_out &= (uint16_t) ~(1u << pin);
    }
    esp_err_t err = exp_commit_locked();
    xSemaphoreGive(s_exp_mu);
    return err;
}

static esp_err_t exp_read(uint16_t *pins)
{
    uint8_t reg = 0x00;
    uint8_t in[2];
    xSemaphoreTake(s_exp_mu, portMAX_DELAY);
    esp_err_t err = i2c_master_transmit_receive(s_exp, &reg, 1, in, sizeof(in), 50);
    xSemaphoreGive(s_exp_mu);
    if (err == ESP_OK) {
        *pins = (uint16_t)in[0] | ((uint16_t)in[1] << 8);
    }
    return err;
}

static void leds_show(uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_leds) {
        return;
    }
    for (int i = 0; i < LED_COUNT; i++) {
        if (led_strip_set_pixel(s_leds, i, r, g, b) != ESP_OK) {
            return;
        }
    }
    (void)led_strip_refresh(s_leds);
}

/* Same colours as Link's status light, held dim so the strip isn't a lamp.
 * The avatar is the status UI; these three LEDs follow the link state. */
static void led_task(void *arg)
{
    (void)arg;
    muse_link_state_t prev = (muse_link_state_t)-1;
    for (;;) {
        muse_link_state_t st = muse_link_state();
        if (st != prev) {
            prev = st;
            switch (st) {
            case MUSE_LINK_ONLINE:
                leds_show(0, 12, 0);
                break;
            case MUSE_LINK_CONNECTING:
            case MUSE_LINK_PAIRING:
            case MUSE_LINK_CONFIRM:
                leds_show(0, 0, 12);
                break;
            case MUSE_LINK_OFFLINE:
                leds_show(12, 8, 0);
                break;
            case MUSE_LINK_ERROR:
                leds_show(12, 0, 0);
                break;
            default:
                leds_show(12, 4, 0);
                break;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

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
    const i2c_device_config_t exp_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = EXP_ADDR,
        .scl_speed_hz = 400000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_i2c, &exp_cfg, &s_exp), TAG, "expander");
    s_exp_mu = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_exp_mu, ESP_ERR_NO_MEM, TAG, "expander mutex");
    /* Backlight off, camera held in reset, amp off. Other pins stay inputs. */
    ESP_RETURN_ON_ERROR(exp_set(PIN_BL, false), TAG, "backlight off");
    ESP_RETURN_ON_ERROR(exp_set(PIN_CAM_RST, false), TAG, "camera reset");
    ESP_RETURN_ON_ERROR(exp_set(PIN_AMP, false), TAG, "amp off");

    led_strip_config_t strip_cfg = {
        .strip_gpio_num = LED_GPIO,
        .max_leds = LED_COUNT,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags = {
            .invert_out = false,
        },
    };
    led_strip_rmt_config_t rmt_cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .mem_block_symbols = 0,
        .flags.with_dma = false,
    };
    esp_err_t err = led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_leds);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "RGB LEDs unavailable: %s", esp_err_to_name(err));
        s_leds = NULL;
    } else if (xTaskCreate(led_task, "k10_led", 3072, NULL, 2, NULL) != pdPASS) {
        ESP_LOGW(TAG, "RGB LED task not started");
    }
    return ESP_OK;
}

static lv_display_t *display_start(lv_indev_t **touch)
{
    const spi_bus_config_t bus = {
        .sclk_io_num = LCD_SCLK,
        .mosi_io_num = LCD_MOSI,
        .miso_io_num = GPIO_NUM_NC,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = LCD_H_RES * DRAW_BUF_LINES * 2,
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
    /* who_lcd.h leaves reset unconnected. TFT_eSPI rotation 2 is MADCTL MY
     * plus BGR (0x88): portrait, flipped on Y, blue-green-red. */
    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
    };
    if (esp_lcd_new_panel_ili9341(io, &panel_cfg, &s_panel) != ESP_OK) {
        return NULL;
    }
    esp_lcd_panel_reset(s_panel);
    esp_lcd_panel_init(s_panel);
    esp_lcd_panel_invert_color(s_panel, false);
    esp_lcd_panel_swap_xy(s_panel, false);
    esp_lcd_panel_mirror(s_panel, false, true);
    esp_lcd_panel_disp_on_off(s_panel, true);
    if (exp_set(PIN_BL, true) != ESP_OK) {
        ESP_LOGW(TAG, "backlight did not turn on");
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
            .hor_res = LCD_H_RES,
            .ver_res = LCD_V_RES,
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
    *touch = NULL;
    return disp;
}

static bool display_lock(int timeout_ms)
{
    return esp_lv_adapter_lock(timeout_ms) == ESP_OK;
}

/* The backlight is an expander bit, not PWM. Any non-zero level is on. */
static void set_brightness(int pct)
{
    (void)exp_set(PIN_BL, pct > 0);
}

static void panel_sleep(bool sleep)
{
    esp_lcd_panel_disp_sleep(s_panel, sleep);
    (void)exp_set(PIN_BL, !sleep);
}

static int wrap_open(const audio_codec_data_if_t *h, void *data_cfg, int cfg_size)
{
    (void)h;
    return s_i2s_if->open(s_i2s_if, data_cfg, cfg_size);
}

static bool wrap_is_open(const audio_codec_data_if_t *h)
{
    (void)h;
    return s_i2s_if->is_open && s_i2s_if->is_open(s_i2s_if);
}

static int wrap_enable(const audio_codec_data_if_t *h, esp_codec_dev_type_t type, bool on)
{
    (void)h;
    if (type == ESP_CODEC_DEV_TYPE_OUT) {
        if (exp_set(PIN_AMP, on) != ESP_OK) {
            ESP_LOGW(TAG, "amp %s failed", on ? "on" : "off");
        }
    }
    return s_i2s_if->enable ? s_i2s_if->enable(s_i2s_if, type, on) : ESP_CODEC_DEV_OK;
}

static int wrap_set_fmt(const audio_codec_data_if_t *h, esp_codec_dev_type_t type,
                        esp_codec_dev_sample_info_t *fs)
{
    (void)h;
    return s_i2s_if->set_fmt ? s_i2s_if->set_fmt(s_i2s_if, type, fs) : ESP_CODEC_DEV_OK;
}

static int wrap_read(const audio_codec_data_if_t *h, uint8_t *data, int size)
{
    (void)h;
    return s_i2s_if->read(s_i2s_if, data, size);
}

static int wrap_write(const audio_codec_data_if_t *h, uint8_t *data, int size)
{
    (void)h;
    return s_i2s_if->write(s_i2s_if, data, size);
}

static int wrap_close(const audio_codec_data_if_t *h)
{
    (void)h;
    return s_i2s_if->close ? s_i2s_if->close(s_i2s_if) : ESP_CODEC_DEV_OK;
}

static const audio_codec_data_if_t s_data_if = {
    .open = wrap_open,
    .is_open = wrap_is_open,
    .enable = wrap_enable,
    .set_fmt = wrap_set_fmt,
    .read = wrap_read,
    .write = wrap_write,
    .close = wrap_close,
};

/* Duplex I2S as DFRobot's initI2S(): Philips, 16-bit stereo, MCLK on GPIO3.
 * The speaker has no I2C codec (amp bit only). The ES7243E is the mic ADC. */
static esp_err_t audio_init(esp_codec_dev_handle_t *spk, esp_codec_dev_handle_t *mic)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_tx, &s_rx), TAG, "i2s channel");
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
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx, &std_cfg), TAG, "i2s tx");
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_rx, &std_cfg), TAG, "i2s rx");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx), TAG, "i2s tx on");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_rx), TAG, "i2s rx on");

    audio_codec_i2s_cfg_t i2s_cfg = { .port = I2S_NUM_0, .rx_handle = s_rx, .tx_handle = s_tx };
    s_i2s_if = audio_codec_new_i2s_data(&i2s_cfg);
    ESP_RETURN_ON_FALSE(s_i2s_if, ESP_ERR_NO_MEM, TAG, "i2s data");

    audio_codec_i2c_cfg_t i2c_cfg = { .port = I2C_NUM_0, .addr = ES7243E_ADDR, .bus_handle = s_i2c };
    const audio_codec_ctrl_if_t *ctrl = audio_codec_new_i2c_ctrl(&i2c_cfg);
    ESP_RETURN_ON_FALSE(ctrl, ESP_ERR_NO_MEM, TAG, "es7243e ctrl");
    es7243e_codec_cfg_t adc_cfg = { .ctrl_if = ctrl };
    const audio_codec_if_t *adc = es7243e_codec_new(&adc_cfg);
    ESP_RETURN_ON_FALSE(adc, ESP_FAIL, TAG, "ES7243E at 0x11 not responding");

    esp_codec_dev_cfg_t out_cfg = { .dev_type = ESP_CODEC_DEV_TYPE_OUT, .data_if = &s_data_if };
    esp_codec_dev_cfg_t in_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN, .codec_if = adc, .data_if = &s_data_if,
    };
    *spk = esp_codec_dev_new(&out_cfg);
    *mic = esp_codec_dev_new(&in_cfg);
    return *spk && *mic ? ESP_OK : ESP_FAIL;
}

static unsigned poll_one(exp_btn_t *b, bool raw)
{
    if (raw == b->pressed) {
        b->stable = 0;
        return 0;
    }
    if (++b->stable < DEBOUNCE_SAMPLES && !(raw && muse_state_asleep())) {
        return 0;
    }
    b->stable = 0;
    b->pressed = raw;
    return raw ? MUSE_BTN_TALK_PRESS : MUSE_BTN_TALK_RELEASE;
}

static unsigned poll_buttons(void)
{
    uint16_t pins = 0xFFFF;
    if (exp_read(&pins) != ESP_OK) {
        return 0;
    }
    bool a = (pins & (1u << s_talk.pin)) == 0;
    bool b = (pins & (1u << s_aux.pin)) == 0;
    return poll_one(&s_talk, a) | poll_one(&s_aux, b) << 2;
}

/* GPIO0 is the I2S bit clock after boot, and the ROM download strap. Menu
 * power-off gives it back to the BOOT button so a press can wake the chip.
 * Buttons A and B are on the expander, which does not wake the ESP32. */
static esp_err_t power_off(void)
{
    set_brightness(0);
    if (s_panel) {
        esp_lcd_panel_disp_on_off(s_panel, false);
    }
    (void)exp_set(PIN_AMP, false);
    leds_show(0, 0, 0);
    if (s_tx) {
        (void)i2s_channel_disable(s_tx);
    }
    if (s_rx) {
        (void)i2s_channel_disable(s_rx);
    }
    const gpio_config_t wake = {
        .pin_bit_mask = 1ULL << BOOT_WAKE_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&wake), TAG, "boot wake");
    while (gpio_get_level(BOOT_WAKE_GPIO) == 0) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    ESP_RETURN_ON_ERROR(esp_sleep_enable_ext0_wakeup(BOOT_WAKE_GPIO, 0), TAG, "wake");
    esp_deep_sleep_start();
    return ESP_FAIL;
}

static const muse_board_t s_board = {
    .name = "UNIHIKER K10",
    .width = LCD_H_RES,
    .height = LCD_V_RES,
    .round = false,
    .touch = false,
    .diagonal_in = 2.8f,
    .talk_button = "A",
    .aux_button = "B",
    .talk_hint = { LV_ALIGN_BOTTOM_LEFT, 8, -8 },
    .aux_hint = { LV_ALIGN_BOTTOM_RIGHT, -8, -8 },
    .frame_ms = 40,
    .init = init,
    .display_start = display_start,
    .display_lock = display_lock,
    .display_unlock = esp_lv_adapter_unlock,
    .set_brightness = set_brightness,
    .panel_sleep = panel_sleep,
    .audio_init = audio_init,
    .mic_slot = -1, /* two MEMS mics; mix both slots */
    .poll_buttons = poll_buttons,
    .power_off = power_off,
};

const muse_board_t *muse_board_get(void)
{
    return &s_board;
}
