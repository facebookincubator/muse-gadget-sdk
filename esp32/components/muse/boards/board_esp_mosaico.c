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
 * Espressif ESP-Mosaico (ESP32-S31NRV16: two RISC-V cores at 320 MHz, 16 MB
 * in-package PSRAM, 16 MB NOR flash): a 2.16" 480x480 CO5300 QSPI AMOLED with
 * CST9217 touch, an ES8311 codec (mic + NS4150B speaker amp), a BQ27220 fuel
 * gauge, the AI button and BOOT.
 *
 * Pins and power sequencing come from Espressif's BSP, esp-mosaico/esp-mosaico-bsp
 * (components/esp-mosaico-bsp: include/bsp/esp_mosaico.h, onboard/esp_mosaico.c,
 * display.c, audio.c, battery.c). Muse drives the parts itself, as the
 * StopWatch does, rather than through that BSP: it pins esp_lvgl_adapter and
 * esp_codec_dev versions the other boards don't use. Two hardware revisions
 * differ in a few pins, told apart by the version Espressif writes to eFuse
 * USER_DATA (v1.0; v1.1 and v1.2 share a mapping):
 *
 *            v1.0     v1.2
 *   I2C SDA  GPIO0    GPIO56
 *   I2C SCL  GPIO1    GPIO3
 *   LCD SCL  GPIO44   GPIO42
 *   LCD RST  GPIO42   GPIO44
 *   codec 3V3 GPIO56  always on
 *
 * The rest: LCD CS 50, D0-D3 36/51/35/9, TE 43; touch INT 6 (reset not
 * wired); I2S MCLK 54, BCLK 37, WS 49, DOUT 52, DIN 40, amp enable 45; the
 * VCC_3V3 rail (display, touch, codec) on GPIO60, low = on; PWR_SW GPIO57,
 * open drain, low turns the board off; AI GPIO7 and BOOT GPIO61, active low.
 * GPIO7 held at reset also selects the retained Vibe Mode firmware.
 */

#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_codec_dev_defaults.h"
#include "esp_efuse.h"
#include "esp_efuse_table.h"
#include "esp_lcd_co5300.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch_cst9220.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "muse_audio.h"
#include "muse_board.h"
#include "muse_lcd_bands.h"
#include "muse_mem.h"

static const char *TAG = "board";

#define LCD_RES 480
#define LCD_HOST SPI2_HOST
#define LCD_CS GPIO_NUM_50
#define LCD_TE GPIO_NUM_43
#define LCD_D0 GPIO_NUM_36
#define LCD_D1 GPIO_NUM_51
#define LCD_D2 GPIO_NUM_35
#define LCD_D3 GPIO_NUM_9
#define DRAW_BUF_LINES 120              /* four bands to the screen (muse_lcd_bands.h) */
#define LCD_CHUNK_BYTES (LCD_RES * 8 * 2)

#define TP_INT GPIO_NUM_6

#define I2S_MCLK GPIO_NUM_54
#define I2S_BCLK GPIO_NUM_37
#define I2S_WS GPIO_NUM_49
#define I2S_DOUT GPIO_NUM_52
#define I2S_DIN GPIO_NUM_40
#define SPK_PA GPIO_NUM_45
#define ES8311_ADDR (0x19 << 1)         /* esp_codec_dev takes the 8-bit address */

#define VCC_3V3_EN GPIO_NUM_60          /* low = on */
#define PWR_SW GPIO_NUM_57              /* open drain; low turns the board off */
#define AI_GPIO GPIO_NUM_7
#define BOOT_GPIO GPIO_NUM_61

#define GAUGE_ADDR 0x55                 /* BQ27220 */
#define GAUGE_VOLTAGE 0x08              /* mV */
#define GAUGE_CURRENT 0x0C              /* mA, signed, negative discharging */
#define GAUGE_SOC 0x2C                  /* % */

typedef struct {
    gpio_num_t i2c_sda, i2c_scl, lcd_scl, lcd_rst, codec_en;
} pins_t;

static const pins_t s_v1_0 = { GPIO_NUM_0, GPIO_NUM_1, GPIO_NUM_44, GPIO_NUM_42, GPIO_NUM_56 };
static const pins_t s_v1_2 = { GPIO_NUM_56, GPIO_NUM_3, GPIO_NUM_42, GPIO_NUM_44, GPIO_NUM_NC };
static const pins_t *s_pins = &s_v1_2;

static i2c_master_bus_handle_t s_i2c;
static i2c_master_dev_handle_t s_gauge;
static esp_lcd_panel_io_handle_t s_io;
static esp_lcd_panel_handle_t s_panel;
static esp_lcd_touch_handle_t s_tp;
static muse_gpio_button_t s_ai, s_boot;

void muse_mosaico_iris_start(void) __attribute__((weak));

/* The BSP's vendor init: brightness, 16-bit colour, the 480x480 window. */
static const co5300_lcd_init_cmd_t s_lcd_init[] = {
    {0x11, NULL, 0, 120},
    {0xFE, (uint8_t[]){0x20}, 1, 0},
    {0x19, (uint8_t[]){0x10}, 1, 0},
    {0x1C, (uint8_t[]){0xA0}, 1, 0},
    {0xFE, (uint8_t[]){0x00}, 1, 0},
    {0xC4, (uint8_t[]){0x80}, 1, 0},
    {0x3A, (uint8_t[]){0x55}, 1, 0},
    {0x35, (uint8_t[]){0x00}, 1, 0},   /* tearing effect on (V-blank), GPIO43: muse_lcd_bands_set_te */
    {0x53, (uint8_t[]){0x20}, 1, 0},
    {0x51, (uint8_t[]){0xFF}, 1, 0},
    {0x63, (uint8_t[]){0xFF}, 1, 0},
    {0x2A, (uint8_t[]){0x00, 0x00, 0x01, 0xDF}, 4, 0},
    {0x2B, (uint8_t[]){0x00, 0x00, 0x01, 0xDF}, 4, 0},
    {0x29, NULL, 0, 20},
};

static esp_err_t output(gpio_num_t pin, int level, bool open_drain)
{
    const gpio_config_t cfg = {
        .pin_bit_mask = BIT64(pin),
        .mode = open_drain ? GPIO_MODE_OUTPUT_OD : GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_set_level(pin, level), TAG, "GPIO%d level", pin);
    ESP_RETURN_ON_ERROR(gpio_config(&cfg), TAG, "GPIO%d", pin);
    return gpio_hold_dis(pin);
}

static void detect_revision(void)
{
    uint16_t version = 0;
    if (esp_efuse_read_field_blob(ESP_EFUSE_USER_DATA, &version, 16) != ESP_OK) {
        ESP_LOGW(TAG, "hardware version unreadable: assuming v1.2");
        return;
    }
    s_pins = version == 0x0100 ? &s_v1_0 : &s_v1_2;
    ESP_LOGI(TAG, "ESP-Mosaico v%u.%u (%s pins)", version >> 8, version & 0xFF, s_pins == &s_v1_0 ? "v1.0" : "v1.2");
    if (version != 0x0100 && version != 0x0101 && version != 0x0102) {
        ESP_LOGW(TAG, "unknown hardware version 0x%04x: using the v1.2 pins", version);
    }
}

static esp_err_t init(void)
{
    /* ESP-Iris on the USB-C port, so Vibe Mode can install the next build
     * (components_mosaico/, in builds with Espressif's esp-mosaico-utils). */
    if (muse_mosaico_iris_start) {
        muse_mosaico_iris_start();
    }
    detect_revision();
    /* PWR_SW released, then the peripheral rail up (the BSP ramps it with
     * PWM to bound inrush; the retained bootloader has normally turned it on
     * already, and a plain switch is what the BSP does with ramping off). */
    ESP_RETURN_ON_ERROR(output(PWR_SW, 1, true), TAG, "power switch");
    ESP_RETURN_ON_ERROR(output(VCC_3V3_EN, 0, false), TAG, "VCC_3V3");
    if (s_pins->codec_en != GPIO_NUM_NC) {
        ESP_RETURN_ON_ERROR(output(s_pins->codec_en, 1, false), TAG, "codec power");
    }
    vTaskDelay(pdMS_TO_TICKS(10));

    const i2c_master_bus_config_t bus = {
        .i2c_port = I2C_NUM_0,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .sda_io_num = s_pins->i2c_sda,
        .scl_io_num = s_pins->i2c_scl,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus, &s_i2c), TAG, "i2c");
    const i2c_device_config_t gauge = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = GAUGE_ADDR,
        .scl_speed_hz = 100000,
    };
    if (i2c_master_probe(s_i2c, GAUGE_ADDR, 50) == ESP_OK) {
        i2c_master_bus_add_device(s_i2c, &gauge, &s_gauge);
    } else {
        ESP_LOGW(TAG, "fuel gauge not found: battery status disabled");
    }
    ESP_RETURN_ON_ERROR(muse_gpio_button_init(&s_ai, AI_GPIO), TAG, "AI button");
    ESP_RETURN_ON_ERROR(muse_gpio_button_init(&s_boot, BOOT_GPIO), TAG, "boot button");
    return ESP_OK;
}

/* The CO5300 over QSPI wants update windows on 4-pixel columns (the BSP's
 * rounder) and even rows. */
static void round_area(lv_event_t *e)
{
    lv_area_t *a = lv_event_get_param(e);
    a->x1 &= ~3;
    a->x2 |= 3;
    a->y1 &= ~1;
    a->y2 |= 1;
}

static lv_display_t *display_start(lv_indev_t **touch)
{
    const spi_bus_config_t bus =
        CO5300_PANEL_BUS_QSPI_CONFIG(s_pins->lcd_scl, LCD_D0, LCD_D1, LCD_D2, LCD_D3, LCD_CHUNK_BYTES);
    if (spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) {
        return NULL;
    }
    esp_lcd_panel_io_spi_config_t io_cfg = CO5300_PANEL_IO_QSPI_CONFIG(LCD_CS, NULL, NULL);
    if (esp_lcd_new_panel_io_spi(LCD_HOST, &io_cfg, &s_io) != ESP_OK) {
        return NULL;
    }
    co5300_vendor_config_t vendor_cfg = {
        .init_cmds = s_lcd_init,
        .init_cmds_size = sizeof(s_lcd_init) / sizeof(s_lcd_init[0]),
        .flags.use_qspi_interface = 1,
    };
    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = s_pins->lcd_rst,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor_cfg,
    };
    if (esp_lcd_new_panel_co5300(s_io, &panel_cfg, &s_panel) != ESP_OK) {
        return NULL;
    }
    esp_lcd_panel_reset(s_panel);
    esp_lcd_panel_init(s_panel);
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
        },
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE,
    };
    lv_display_t *disp = muse_lcd_bands_register(disp_cfg, DRAW_BUF_LINES, LCD_CHUNK_BYTES);
    if (!disp) {
        return NULL;
    }
    lv_display_add_event_cb(disp, round_area, LV_EVENT_INVALIDATE_AREA, NULL);
    muse_lcd_bands_set_te(LCD_TE);

    /* Touch: the BSP's CST9220 setup, at its base orientation (no swap or mirror). */
    const esp_lcd_touch_config_t tp_cfg = {
        .x_max = LCD_RES - 1,
        .y_max = LCD_RES - 1,
        .rst_gpio_num = GPIO_NUM_NC,
        .int_gpio_num = TP_INT,
        .levels = { .reset = 0, .interrupt = 0 },
    };
    esp_lcd_panel_io_handle_t tp_io;
    esp_lcd_panel_io_i2c_config_t tp_io_cfg = ESP_LCD_TOUCH_IO_I2C_CST9220_CONFIG();
    tp_io_cfg.scl_speed_hz = 400000;
    tp_io_cfg.transaction_timeout_ms = 100;   /* 0 waits forever, under the LVGL lock */
    if (esp_lcd_new_panel_io_i2c(s_i2c, &tp_io_cfg, &tp_io) == ESP_OK &&
        esp_lcd_touch_new_i2c_cst9220(tp_io, &tp_cfg, &s_tp) == ESP_OK) {
        const esp_lv_adapter_touch_config_t touch_cfg = ESP_LV_ADAPTER_TOUCH_DEFAULT_CONFIG(disp, s_tp);
        *touch = esp_lv_adapter_register_touch(&touch_cfg);
    } else {
        ESP_LOGW(TAG, "touch controller not answering: no touch");
    }
    if (esp_lv_adapter_start() != ESP_OK) {
        return NULL;
    }
    return disp;
}

static bool display_lock(int timeout_ms)
{
    return esp_lv_adapter_lock(timeout_ms) == ESP_OK;
}

static void send_brightness(void *level)
{
    /* CO5300 "write display brightness" (0x51). */
    esp_lcd_panel_io_tx_param(s_io, (0x02 << 24) | (0x51 << 8), level, 1);
}

static void set_brightness(int pct)
{
    uint8_t level = (uint8_t)(pct * 255 / 100);
    muse_lcd_bands_run(send_brightness, &level);
}

static void send_sleep(void *sleep)
{
    esp_lcd_panel_io_tx_param(s_io, (0x02 << 24) | ((*(bool *)sleep ? 0x10 : 0x11) << 8), NULL, 0);
}

static void panel_sleep(bool sleep)
{
    muse_lcd_bands_run(send_sleep, &sleep);
    vTaskDelay(pdMS_TO_TICKS(120));   /* settle before the next command */
}

static esp_err_t audio_init(esp_codec_dev_handle_t *spk, esp_codec_dev_handle_t *mic)
{
    i2s_chan_handle_t tx, rx;
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &tx, &rx), TAG, "i2s channel");
    i2s_std_config_t std_cfg = {
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
    std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(tx, &std_cfg), TAG, "i2s tx");
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(rx, &std_cfg), TAG, "i2s rx");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(tx), TAG, "i2s tx on");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(rx), TAG, "i2s rx on");

    audio_codec_i2s_cfg_t i2s_cfg = { .port = I2S_NUM_0, .rx_handle = rx, .tx_handle = tx };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);
    audio_codec_i2c_cfg_t i2c_cfg = { .port = I2C_NUM_0, .addr = ES8311_ADDR, .bus_handle = s_i2c };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    ESP_RETURN_ON_FALSE(data_if && ctrl_if && gpio_if, ESP_ERR_NO_MEM, TAG, "codec interfaces");

    es8311_codec_cfg_t es_cfg = {
        .ctrl_if = ctrl_if,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH,
        .pa_pin = SPK_PA,
        .use_mclk = true,
        .hw_gain = { .pa_voltage = 5.0, .codec_dac_voltage = 3.3 },
        .no_dac_ref = true,           /* as the BSP: the mic path has no DAC reference */
    };
    const audio_codec_if_t *codec = es8311_codec_new(&es_cfg);
    ESP_RETURN_ON_FALSE(codec, ESP_FAIL, TAG, "ES8311 not responding");

    esp_codec_dev_cfg_t out_cfg = { .dev_type = ESP_CODEC_DEV_TYPE_OUT, .codec_if = codec, .data_if = data_if };
    esp_codec_dev_cfg_t in_cfg = { .dev_type = ESP_CODEC_DEV_TYPE_IN, .codec_if = codec, .data_if = data_if };
    *spk = esp_codec_dev_new(&out_cfg);
    *mic = esp_codec_dev_new(&in_cfg);
    return *spk && *mic ? ESP_OK : ESP_FAIL;
}

/* AI is talk, BOOT is aux. */
static unsigned poll_buttons(void)
{
    return muse_gpio_button_poll(&s_ai) | muse_gpio_button_poll(&s_boot) << 2;
}

static void wait_buttons(int timeout_ms)
{
    muse_gpio_buttons_wait((muse_gpio_button_t *const[]){ &s_ai, &s_boot }, 2, timeout_ms);
}

static esp_err_t gauge_read(uint8_t reg, uint16_t *out)
{
    uint8_t b[2];
    ESP_RETURN_ON_ERROR(i2c_master_transmit_receive(s_gauge, &reg, 1, b, 2, 50), TAG, "gauge");
    *out = (uint16_t)(b[0] | b[1] << 8);
    return ESP_OK;
}

static esp_err_t read_power(muse_power_t *out)
{
    ESP_RETURN_ON_FALSE(s_gauge, ESP_ERR_NOT_SUPPORTED, TAG, "no gauge");
    uint16_t mv, soc, ma;
    ESP_RETURN_ON_ERROR(gauge_read(GAUGE_VOLTAGE, &mv), TAG, "voltage");
    ESP_RETURN_ON_ERROR(gauge_read(GAUGE_SOC, &soc), TAG, "soc");
    ESP_RETURN_ON_ERROR(gauge_read(GAUGE_CURRENT, &ma), TAG, "current");
    out->battery_pct = mv > 2500 ? (soc > 100 ? 100 : soc) : -1;
    out->battery_mv = mv;
    out->charging = (int16_t)ma > 0;
    out->usb = (int16_t)ma >= 0;
    return ESP_OK;
}

/* PWR_SW low drops the board's power latch, as the BSP's bsp_power_set_shutdown(true). */
static esp_err_t power_off(void)
{
    gpio_set_level(PWR_SW, 0);
    vTaskDelay(pdMS_TO_TICKS(500));
    gpio_set_level(PWR_SW, 1);
    return ESP_FAIL;   /* still running: USB power keeps it on */
}

static const muse_board_t s_board = {
    .name = "Espressif ESP-Mosaico",
    .width = LCD_RES, .height = LCD_RES, .round = false, .no_ring = true, .touch = true, .diagonal_in = 2.16f,
    .talk_button = "ai", .aux_button = "boot",
    /* Espressif's drawing, seen from the front with USB-C down: the AI key on
     * the top edge about 380 px from the left (the speaker grille left of
     * it); on the bottom edge BOOT left of the USB-C port (~125 px) and
     * ON/OFF right of it (~350 px). The mic icon marks the AI key. The power
     * icon marks ON/OFF, the switch that turns the board off, though that's
     * hardware the firmware can't read: BOOT, Muse's aux button (it puts
     * the screen to sleep), goes unmarked rather than show a second power
     * symbol beside the real one. */
    .talk_hint = { LV_ALIGN_TOP_MID, 140, 6 },
    .aux_hint = { LV_ALIGN_BOTTOM_MID, 111, -6 },
    .frame_ms = 40,
    .init = init,
    .display_start = display_start,
    .display_lock = display_lock,
    .display_unlock = esp_lv_adapter_unlock,
    .set_brightness = set_brightness,
    .panel_sleep = panel_sleep,
    .draw_frame = muse_lcd_bands_frame,
    .audio_init = audio_init,
    .mic_slot = 0,
    .poll_buttons = poll_buttons,
    .wait_buttons = wait_buttons,
    .read_power = read_power,
    .power_off = power_off,
};

/* Home Link's app_main starts Muse with this board (main/main.c). */
const muse_board_t *muse_board_get(void)
{
    return &s_board;
}
