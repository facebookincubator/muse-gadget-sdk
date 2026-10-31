/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * Copyright (c) 2026 SHENZHEN VIEWE TECHNOLOGY CO.,LTD.
 * SPDX-License-Identifier: Apache-2.0
 *
 * VIEWE SmartRing-Plus: ESP32-S3-N16R8, 360×360 round ST77916 QSPI LCD with
 * CST816 touch, ES8311 codec (speaker through NS4150B amp, one analog mic) and
 * V1 (GPIO1 ADC) or V2 (AXP2101) battery gauge.
 *
 * BOOT (GPIO0) is inside the case, so the mic icon is an on-screen talk button
 * (touch_talk): hold to talk and let go to send. A tap on the screen confirms
 * Muse app pairing. BOOT still talks if reachable.
 *
 * Display/touch/audio implemented directly following EchoEar-2ST pattern (no BSP).
 * Pins from VIEWE smartring_plus BSP board_config.h. Init sequence from BSP
 * lcd_init_seq.h B-version panel (confirmed on production hardware).
 *
 * Draw buffers: Two 10-line internal-RAM buffers, double-buffered. PSRAM
 * bounce-buffer path (40-line bands → 28.8 KB DMA staging) fails after
 * Wi-Fi/BLE/Muse sessions come up. See EchoEar-2ST for rationale.
 */
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_check.h"
#include "esp_codec_dev_defaults.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_st77916.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "muse_audio.h"
#include "muse_board.h"
#include "muse_mem.h"
#include "viewe_smartring_display_init.h"
#include "viewe_smartring_touch.h"

static const char *TAG = "smartring";

/* Display pins (ST77916 QSPI, no DC pin) */
#define LCD_HOST SPI2_HOST
#define LCD_RES 360
#define DRAW_BUF_LINES 10 /* Internal RAM only; EchoEar-2ST rationale applies */
#define LCD_SCLK 10
#define LCD_MOSI_D0 12
#define LCD_D1 13
#define LCD_D2 15
#define LCD_D3 14
#define LCD_CS 11
#define LCD_DC -1
#define LCD_RST 39
#define LCD_BL 46

/* Touch (CST816S at 0x15) */
#define I2C_PORT I2C_NUM_0
#define I2C_SDA 8
#define I2C_SCL 9
#define I2C_FREQ_HZ 400000
#define TOUCH_ADDR 0x15
#define TOUCH_RST 40
#define TOUCH_INT 41

/* Audio (ES8311 at 0x18, I2C shared) */
#define I2S_PORT I2S_NUM_0
#define I2S_MCLK 48
#define I2S_BCLK 21
#define I2S_WS 17
#define I2S_DOUT 16
#define I2S_DIN 18
#define PA_EN 45 /* NS4150B enable, active high */

/* Battery V1 ADC (GPIO1) */
#define BAT_ADC_GPIO GPIO_NUM_1
#define BAT_ADC_UNIT ADC_UNIT_1
#define BAT_ADC_CHAN ADC_CHANNEL_0
#define BAT_ADC_ATTEN ADC_ATTEN_DB_12
#define BAT_DIVIDER 2.0f /* R18/R21 10kΩ each = 1:2 divider */

/* V2 AXP2101 (if present) */
#define AXP2101_ADDR 0x34
#define AXP2101_CHIP_ID_REG 0x03
#define AXP2101_CHIP_ID 0x4A

static esp_lcd_panel_handle_t s_panel;
static i2c_master_dev_handle_t s_touch;
static i2c_master_bus_handle_t s_i2c;
static adc_oneshot_unit_handle_t s_adc1;
static bool s_has_axp2101; /* V2 power chip detected */

static esp_err_t init(void)
{
    /* I2C bus for touch, ES8311, and optional AXP2101 */
    i2c_master_bus_handle_t bus;
    const i2c_master_bus_config_t cfg = {.i2c_port = I2C_PORT,
                                         .scl_io_num = I2C_SCL,
                                         .sda_io_num = I2C_SDA,
                                         .clk_source = I2C_CLK_SRC_DEFAULT,
                                         .glitch_ignore_cnt = 7,
                                         .flags.enable_internal_pullup = true};
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&cfg, &bus), TAG, "I2C bus");
    s_i2c = bus;

    /* Touch device */
    const i2c_device_config_t dev = {.dev_addr_length = I2C_ADDR_BIT_LEN_7,
                                     .device_address = TOUCH_ADDR,
                                     .scl_speed_hz = I2C_FREQ_HZ};
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &dev, &s_touch), TAG, "touch device");
    ESP_RETURN_ON_ERROR(i2c_master_probe(bus, TOUCH_ADDR, 100), TAG, "touch probe");
    ESP_LOGI(TAG, "CST816 touch at 0x%02x", TOUCH_ADDR);

    /* Check for AXP2101 (V2 boards) */
    uint8_t chip_id_reg = AXP2101_CHIP_ID_REG, chip_id = 0;
    esp_err_t axp_probe =
        i2c_master_transmit_receive(bus, AXP2101_ADDR, &chip_id_reg, 1, &chip_id, 1, 100);
    if (axp_probe == ESP_OK && chip_id == AXP2101_CHIP_ID) {
        s_has_axp2101 = true;
        ESP_LOGI(TAG, "AXP2101 detected (V2 board)");
    } else {
        s_has_axp2101 = false;
        ESP_LOGI(TAG, "No AXP2101 (V1 board); using ADC battery gauge");
        /* V1 ADC battery gauge */
        const adc_oneshot_unit_init_cfg_t adc_cfg = {.unit_id = BAT_ADC_UNIT};
        ESP_RETURN_ON_ERROR(adc_oneshot_new_unit(&adc_cfg, &s_adc1), TAG, "ADC init");
        const adc_oneshot_chan_cfg_t chan_cfg = {.atten = BAT_ADC_ATTEN, .bitwidth = ADC_BITWIDTH_DEFAULT};
        ESP_RETURN_ON_ERROR(adc_oneshot_config_channel(s_adc1, BAT_ADC_CHAN, &chan_cfg), TAG,
                            "ADC channel");
    }

    return ESP_OK;
}

/* CST816 single-finger read via I2C; explicit press edges confirm pairing */
static void touch_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    static uint16_t x, y;
    uint8_t reg = 2, raw[6];
    bool pressed = i2c_master_transmit_receive(s_touch, &reg, 1, raw, sizeof(raw), 20) == ESP_OK &&
                   viewe_smartring_touch_decode(raw, &x, &y);
    data->state = pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    data->point.x = x;
    data->point.y = y;
}

static unsigned poll_buttons(void)
{
    return 0; /* BOOT is inside the case; on-screen mic button only */
}

/* ES8311 at 0x18, duplex I2S at 16 kHz (MUSE_AUDIO_RATE). Do NOT call any
 * bsp_audio_init(). Pattern from EchoEar-2ST / BOX-3 / StickS3. */
static esp_err_t audio_init(esp_codec_dev_handle_t *spk, esp_codec_dev_handle_t *mic)
{
    ESP_RETURN_ON_ERROR(i2c_master_probe(s_i2c, 0x18, 100), TAG, "ES8311 probe");

    i2s_chan_handle_t tx, rx;
    i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_PORT, I2S_ROLE_MASTER);
    chan.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan, &tx, &rx), TAG, "I2S channels");
    const i2s_std_config_t std = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(MUSE_AUDIO_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {.mclk = I2S_MCLK, .bclk = I2S_BCLK, .ws = I2S_WS, .dout = I2S_DOUT, .din = I2S_DIN},
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(tx, &std), TAG, "I2S TX");
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(rx, &std), TAG, "I2S RX");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(tx), TAG, "I2S TX enable");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(rx), TAG, "I2S RX enable");

    audio_codec_i2s_cfg_t data_cfg = {.port = I2S_PORT, .rx_handle = rx, .tx_handle = tx};
    const audio_codec_data_if_t *data = audio_codec_new_i2s_data(&data_cfg);
    audio_codec_i2c_cfg_t ctrl_cfg = {.port = I2C_PORT, .addr = 0x30, .bus_handle = s_i2c};
    const audio_codec_ctrl_if_t *ctrl = audio_codec_new_i2c_ctrl(&ctrl_cfg);
    const audio_codec_gpio_if_t *gpio = audio_codec_new_gpio();
    ESP_RETURN_ON_FALSE(data && ctrl && gpio, ESP_ERR_NO_MEM, TAG, "codec interfaces");

    es8311_codec_cfg_t es_cfg = {.ctrl_if = ctrl,
                                  .gpio_if = gpio,
                                  .codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH,
                                  .pa_pin = PA_EN,
                                  .use_mclk = true,
                                  .hw_gain = {.pa_voltage = 3.3, .codec_dac_voltage = 3.3}};
    const audio_codec_if_t *codec = es8311_codec_new(&es_cfg);
    ESP_RETURN_ON_FALSE(codec, ESP_FAIL, TAG, "ES8311 init");

    esp_codec_dev_cfg_t out_cfg = {.dev_type = ESP_CODEC_DEV_TYPE_OUT, .codec_if = codec, .data_if = data};
    esp_codec_dev_cfg_t in_cfg = {.dev_type = ESP_CODEC_DEV_TYPE_IN, .codec_if = codec, .data_if = data};
    *spk = esp_codec_dev_new(&out_cfg);
    *mic = esp_codec_dev_new(&in_cfg);
    ESP_RETURN_ON_FALSE(*spk && *mic, ESP_FAIL, TAG, "codec handles");
    ESP_LOGI(TAG, "ES8311 initialized; mic slot 0");
    return ESP_OK;
}

static lv_display_t *display_start(lv_indev_t **touch)
{
    *touch = NULL;

    /* Backlight PWM */
    const ledc_timer_config_t bl_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 4000,
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

    /* QSPI bus for ST77916 */
    const spi_bus_config_t bus = {
        .sclk_io_num = LCD_SCLK,
        .mosi_io_num = LCD_MOSI_D0,
        .miso_io_num = -1,
        .quadwp_io_num = LCD_D2,
        .quadhd_io_num = LCD_D3,
        .data1_io_num = LCD_D1,
        .max_transfer_sz = LCD_RES * DRAW_BUF_LINES * 2,
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
        .lcd_cmd_bits = 32,
        .lcd_param_bits = 8,
        .flags = {.quad_mode = true},
    };
    if (esp_lcd_new_panel_io_spi(LCD_HOST, &io_cfg, &io) != ESP_OK) {
        return NULL;
    }

    st77916_vendor_config_t vendor = {
        .init_cmds = smartring_init_cmds,
        .init_cmds_size = sizeof(smartring_init_cmds) / sizeof(smartring_init_cmds[0]),
        .flags = {.use_qspi_interface = true}};
    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor,
    };
    if (esp_lcd_new_panel_st77916(io, &panel_cfg, &s_panel) != ESP_OK) {
        return NULL;
    }
    if (esp_lcd_panel_reset(s_panel) != ESP_OK || esp_lcd_panel_init(s_panel) != ESP_OK ||
        esp_lcd_panel_invert_color(s_panel, false) != ESP_OK ||
        esp_lcd_panel_swap_xy(s_panel, false) != ESP_OK ||
        esp_lcd_panel_mirror(s_panel, false, false) != ESP_OK ||
        esp_lcd_panel_disp_on_off(s_panel, true) != ESP_OK) {
        return NULL;
    }

    /* LVGL adapter with internal-RAM draw buffers (no PSRAM) */
    esp_lv_adapter_config_t adapter_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    adapter_cfg.task_core_id = MUSE_UI_CORE;
    adapter_cfg.task_priority = MUSE_UI_PRIORITY;
    if (esp_lv_adapter_init(&adapter_cfg) != ESP_OK) {
        return NULL;
    }
    const esp_lv_adapter_display_config_t disp_cfg = {
        .panel = s_panel,
        .panel_io = io,
        .profile =
            {
                .interface = ESP_LV_ADAPTER_PANEL_IF_OTHER,
                .rotation = ESP_LV_ADAPTER_ROTATE_0,
                .hor_res = LCD_RES,
                .ver_res = LCD_RES,
                .buffer_height = DRAW_BUF_LINES,
                .use_psram = false, /* Critical: internal RAM only */
                .require_double_buffer = true,
            },
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE,
    };
    lv_display_t *disp = esp_lv_adapter_register_display(&disp_cfg);
    if (!disp) {
        return NULL;
    }

    *touch = lv_indev_create();
    if (!*touch) {
        return NULL;
    }
    lv_indev_set_type(*touch, LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(*touch, disp);
    lv_indev_set_read_cb(*touch, touch_read);

    if (esp_lv_adapter_start() != ESP_OK) {
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
    if (pct < 0)
        pct = 0;
    if (pct > 100)
        pct = 100;
    uint32_t duty = (uint32_t)pct * 255 / 100;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

static void panel_sleep(bool sleep)
{
    esp_lcd_panel_disp_sleep(s_panel, sleep); /* SLPIN/SLPOUT; GRAM kept */
}

/* V1: ADC reads VCC_OUT rail (R18/R21 1:2 divider). V2: placeholder for AXP2101 read.
 * TODO: Implement AXP2101 battery/charging registers for V2. */
static esp_err_t read_power(muse_power_t *out)
{
    if (s_has_axp2101) {
        /* TODO: Read AXP2101 battery voltage, percent, charging state */
        ESP_LOGW(TAG, "AXP2101 battery read not implemented yet");
        return ESP_ERR_NOT_SUPPORTED;
    }

    /* V1 ADC */
    int raw;
    ESP_RETURN_ON_ERROR(adc_oneshot_read(s_adc1, BAT_ADC_CHAN, &raw), TAG, "ADC read");
    int mv_rail;
    ESP_RETURN_ON_ERROR(adc_oneshot_get_calibrated_result(s_adc1, BAT_ADC_CHAN, raw, &mv_rail), TAG,
                        "ADC cal");
    int mv_bat = (int)(mv_rail * BAT_DIVIDER);

    /* Rough percentage from 3300 mV (0%) to 4200 mV (100%) */
    int pct = (mv_bat - 3300) * 100 / (4200 - 3300);
    if (pct < 0)
        pct = 0;
    if (pct > 100)
        pct = 100;

    out->battery_mv = mv_bat;
    out->battery_pct = pct;
    out->charging = false; /* V1 cannot detect charging */
    out->usb = false;
    return ESP_OK;
}

static const muse_board_t s_board = {
    .name = "VIEWE SmartRing-Plus",
    .width = 360,
    .height = 360,
    .round = true,
    .touch = true,
    .touch_talk = true,
    .diagonal_in = 1.39f,
    .talk_button = "screen microphone",
    .frame_ms = 40,
    .talk_hint = {LV_ALIGN_CENTER, 0, 132},
    .init = init,
    .display_start = display_start,
    .display_lock = display_lock,
    .display_unlock = esp_lv_adapter_unlock,
    .set_brightness = set_brightness,
    .panel_sleep = panel_sleep,
    .audio_init = audio_init,
    .mic_slot = 0,
    .poll_buttons = poll_buttons,
    .read_power = read_power,
    /* No .power_off: hardware switch only */
};

const muse_board_t *muse_board_get(void)
{
    return &s_board;
}
