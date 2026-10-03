/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * Copyright (c) 2026 Mihir Jadhav.
 * SPDX-License-Identifier: Apache-2.0
 *
 * OSTB EchoEar-2ST, measured 16 MB flash / 8 MB octal PSRAM, native USB.
 * Variant-specific routes verified in running vendor firmware and its original
 * image: LCD reset9/backlight41, touch0x15, ES8311/ES7210, amplifier18.
 * Product: https://docs.yishierniao.cn/details/ai/echoear-2st/product.html
 * Panel sequence: public 78/xiaozhi-esp32 ESP-VoCat source (MIT), separately
 * credited in ostb_echoear_display_init.h. Stock ESP-VoCat GPIOs do not match.
 * LVGL/codec setup follows existing Muse AIPI and BOX-3 board patterns.
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
#include "esp_lcd_st77916.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "muse_audio.h"
#include "muse_board.h"
#include "muse_mem.h"
#include "ostb_echoear_display_init.h"
#include "ostb_echoear_touch.h"
#define LCD_HOST SPI2_HOST
#define LCD_RES 360
#define DRAW_BUF_LINES 10
#define LCD_SCLK 8
#define LCD_MOSI 4
#define LCD_CS 3
#define LCD_DC -1
#define LCD_RST 9
#define LCD_BL 41
static esp_lcd_panel_handle_t s_panel;
static i2c_master_dev_handle_t s_touch;
static i2c_master_bus_handle_t s_i2c;

static esp_err_t init(void)
{
    i2c_master_bus_handle_t bus;
    const i2c_master_bus_config_t cfg = {.i2c_port = I2C_NUM_0,
                                         .scl_io_num = 11,
                                         .sda_io_num = 12,
                                         .clk_source = I2C_CLK_SRC_DEFAULT,
                                         .glitch_ignore_cnt = 7,
                                         .flags.enable_internal_pullup = true};
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&cfg, &bus), "echoear", "I2C bus");
    s_i2c = bus;
    const i2c_device_config_t dev = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = 0x15, .scl_speed_hz = 400000};
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &dev, &s_touch), "echoear", "touch device");
    ESP_RETURN_ON_ERROR(i2c_master_probe(bus, 0x15, 100), "echoear", "touch probe");
    ESP_LOGI("echoear", "touch controller responds at 0x15");
    return ESP_OK;
}
/* Original driver reads register2, six bytes: count, X high/low, Y high/low.
 * Only explicit screen press edges can confirm an active Muse pairing. */
static void touch_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    static uint16_t x, y;
    uint8_t reg = 2, raw[6];
    bool pressed = i2c_master_transmit_receive(s_touch, &reg, 1, raw, sizeof(raw), 20) == ESP_OK &&
                   ostb_echoear_touch_decode(raw, &x, &y);
    data->state = pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    data->point.x = x;
    data->point.y = y;
}

static unsigned poll_buttons(void)
{
    return 0; /* This enclosure exposes a power switch, not a GPIO talk button. */
}

/* Pins and codec addresses recovered from original BoxAudioCodec constructor.
 * Stereo bus pattern reused from Muse BOX-3/StickS3 drivers.
 * ponytail: two-mic stereo capture; add the original four-slot TDM reference
 * path if live capture requires it or acoustic echo cancellation is added.
 */
static esp_err_t audio_init(esp_codec_dev_handle_t *spk, esp_codec_dev_handle_t *mic)
{
    ESP_RETURN_ON_ERROR(i2c_master_probe(s_i2c, 0x18, 100), "echoear", "ES8311 probe");
    ESP_RETURN_ON_ERROR(i2c_master_probe(s_i2c, 0x40, 100), "echoear", "ES7210 probe");
    i2s_chan_handle_t tx, rx;
    i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan, &tx, &rx), "echoear", "I2S channels");
    const i2s_std_config_t std = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(MUSE_AUDIO_RATE),
        .slot_cfg =
            I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {.mclk = 10, .bclk = 15, .ws = 16, .dout = 14, .din = 13},
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(tx, &std), "echoear", "I2S TX");
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(rx, &std), "echoear", "I2S RX");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(tx), "echoear", "I2S TX enable");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(rx), "echoear", "I2S RX enable");
    audio_codec_i2s_cfg_t data_cfg = {.port = I2S_NUM_0, .rx_handle = rx, .tx_handle = tx};
    const audio_codec_data_if_t *data = audio_codec_new_i2s_data(&data_cfg);
    audio_codec_i2c_cfg_t ctrl_cfg = {.port = I2C_NUM_0, .addr = 0x30, .bus_handle = s_i2c};
    const audio_codec_ctrl_if_t *dac_ctrl = audio_codec_new_i2c_ctrl(&ctrl_cfg);
    ctrl_cfg.addr = 0x80;
    const audio_codec_ctrl_if_t *adc_ctrl = audio_codec_new_i2c_ctrl(&ctrl_cfg);
    const audio_codec_gpio_if_t *gpio = audio_codec_new_gpio();
    ESP_RETURN_ON_FALSE(data && dac_ctrl && adc_ctrl && gpio, ESP_ERR_NO_MEM, "echoear",
                        "codec interfaces");
    es8311_codec_cfg_t dac = {.ctrl_if = dac_ctrl,
                              .gpio_if = gpio,
                              .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
                              .pa_pin = 18,
                              .use_mclk = true,
                              .hw_gain = {.pa_voltage = 5.0, .codec_dac_voltage = 3.3}};
    es7210_codec_cfg_t adc = {.ctrl_if = adc_ctrl,
                              .mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2};
    const audio_codec_if_t *out = es8311_codec_new(&dac), *in = es7210_codec_new(&adc);
    ESP_RETURN_ON_FALSE(out && in, ESP_FAIL, "echoear", "codec initialization");
    esp_codec_dev_cfg_t out_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT, .codec_if = out, .data_if = data};
    esp_codec_dev_cfg_t in_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN, .codec_if = in, .data_if = data};
    *spk = esp_codec_dev_new(&out_cfg);
    *mic = esp_codec_dev_new(&in_cfg);
    ESP_RETURN_ON_FALSE(*spk && *mic, ESP_FAIL, "echoear", "codec handles");
    ESP_LOGI("echoear", "ES8311/ES7210 initialized; live audio test still required");
    return ESP_OK;
}

static lv_display_t *display_start(lv_indev_t **touch)
{
    *touch = NULL;
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
        .miso_io_num = 5,
        .quadwp_io_num = 6,
        .quadhd_io_num = 7,
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
    st77916_vendor_config_t vendor = {.init_cmds = echoear_init_cmds,
                                      .init_cmds_size =
                                          sizeof(echoear_init_cmds) / sizeof(echoear_init_cmds[0]),
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
        esp_lcd_panel_invert_color(s_panel, true) != ESP_OK ||
        esp_lcd_panel_swap_xy(s_panel, false) != ESP_OK ||
        esp_lcd_panel_mirror(s_panel, false, false) != ESP_OK ||
        esp_lcd_panel_disp_on_off(s_panel, true) != ESP_OK) {
        return NULL;
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
        .profile =
            {
                .interface = ESP_LV_ADAPTER_PANEL_IF_OTHER,
                .rotation = ESP_LV_ADAPTER_ROTATE_0,
                .hor_res = LCD_RES,
                .ver_res = LCD_RES,
                .buffer_height = DRAW_BUF_LINES,
                .use_psram = false,
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

static bool display_lock(int timeout_ms) { return esp_lv_adapter_lock(timeout_ms) == ESP_OK; }

static void set_brightness(int pct)
{
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, pct * 1023 / 100);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

static void panel_sleep(bool sleep)
{
    esp_lcd_panel_disp_sleep(s_panel, sleep); /* SLPIN/SLPOUT; GRAM is kept */
}

static esp_err_t power_off(void)
{
    /* The physical bottom switch controls power; no verified software latch. */
    return ESP_ERR_NOT_SUPPORTED;
}

static const muse_board_t s_board = {
    .name = "OSTB EchoEar-2ST",
    .width = 360,
    .height = 360,
    .round = true,
    .touch = true,
    .touch_talk = true,
    .diagonal_in = 1.85f,
    .talk_button = "screen microphone",
    .frame_ms = 40,
    .talk_hint = {LV_ALIGN_BOTTOM_MID, 0, -40},
    .init = init,
    .display_start = display_start,
    .display_lock = display_lock,
    .display_unlock = esp_lv_adapter_unlock,
    .set_brightness = set_brightness,
    .panel_sleep = panel_sleep,
    .audio_init = audio_init,
    .mic_slot = -1,
    .poll_buttons = poll_buttons,
    .power_off = power_off,
};
const muse_board_t *muse_board_get(void) { return &s_board; }
