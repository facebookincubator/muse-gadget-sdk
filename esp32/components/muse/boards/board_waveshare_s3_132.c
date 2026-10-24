/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

/*
 * Waveshare ESP32-S3-Touch-AMOLED-1.32.
 *
 * Hardware reference: the Waveshare ESP-IDF examples at
 * https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.32
 * commit b3d6546e39db6287bc643eb0b0ad796e0ff61315:
 *   Example/ESP-IDF/07_LVGL_V9_Test/main/user_config.h
 *   Example/ESP-IDF/07_LVGL_V9_Test/components/lvgl_port_bsp/lvgl_port_bsp.cpp
 *   Example/ESP-IDF/07_LVGL_V9_Test/components/lcd_touch_bsp/lcd_touch_bsp.cpp
 *   Example/ESP-IDF/05_Audio_Test/components/codec_board/board_cfg.txt
 *   Example/ESP-IDF/01_ADC_Test/components/vbat_bsp/vbat_bsp.cpp
 *
 * The product page calls the panel CO5300, while the vendor ESP-IDF example
 * uses esp_lcd_sh8601. The initialization sequence is adapted from the
 * Apache-2.0 Waveshare BSP v1.0.1; see waveshare_s3_132_display_init.h for
 * the fixed source and local changes. The tested SH8601 driver is retained.
 * The board has no published Muse BSP, so display, touch, audio, battery and
 * buttons are driven directly here.
 */

#include <stdlib.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/spi_master.h"
#include "driver/usb_serial_jtag.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_check.h"
#include "esp_codec_dev_defaults.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_sh8601.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "muse_audio.h"
#include "muse_board.h"
#include "muse_lcd_bands.h"
#include "muse_mem.h"
#include "waveshare_s3_132_display_init.h"

static const char *TAG = "board";

#define LCD_H_RES 466
#define LCD_V_RES 466
#define LCD_GAP_X 6
#define LCD_HOST SPI2_HOST
#define LCD_CS GPIO_NUM_10
#define LCD_SCLK GPIO_NUM_11
#define LCD_D0 GPIO_NUM_12
#define LCD_D1 GPIO_NUM_13
#define LCD_D2 GPIO_NUM_14
#define LCD_D3 GPIO_NUM_15
#define LCD_RST GPIO_NUM_8
#define LCD_TE GPIO_NUM_9
#define DRAW_BUF_LINES 30
#define LCD_CHUNK_BYTES (LCD_H_RES * 8 * 2)

#define TOUCH_SDA GPIO_NUM_47
#define TOUCH_SCL GPIO_NUM_48
#define TOUCH_RST GPIO_NUM_7
#define TOUCH_INT GPIO_NUM_6
#define TOUCH_ADDR 0x15
#define TOUCH_REG_POINTS 0x02

#define I2S_MCLK GPIO_NUM_38
#define I2S_BCLK GPIO_NUM_39
#define I2S_WS GPIO_NUM_41
#define I2S_DIN GPIO_NUM_40
#define I2S_DOUT GPIO_NUM_42
#define AUDIO_PA GPIO_NUM_46

#define PWR_BUTTON GPIO_NUM_17
#define BOOT_BUTTON GPIO_NUM_0
#define VBAT_ENABLE GPIO_NUM_18
#define VBAT_ADC ADC_CHANNEL_3

static i2c_master_bus_handle_t s_i2c;
static i2c_master_dev_handle_t s_touch;
static esp_lcd_panel_io_handle_t s_io;
static esp_lcd_panel_handle_t s_panel;
static muse_gpio_button_t s_pwr, s_boot;
static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_adc_cali;
static bool s_adc_calibrated;

static esp_err_t reg_read(uint8_t reg, uint8_t *buf, size_t len)
{
    return i2c_master_transmit_receive(s_touch, &reg, 1, buf, len, 50);
}

static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    /* Read count, event and coordinates together: two transactions can
     * otherwise combine the press state with the following release frame. */
    uint8_t point[5] = {0};
    data->state = LV_INDEV_STATE_RELEASED;
    if (reg_read(TOUCH_REG_POINTS, point, sizeof(point)) != ESP_OK ||
        (point[0] & 0x0F) != 1 || (point[1] >> 6) == 0x01 || (point[1] >> 6) == 0x03) {
        return;
    }
    int x = ((point[1] & 0x0F) << 8) | point[2];
    int y = ((point[3] & 0x0F) << 8) | point[4];
    if (x > LCD_H_RES || y > LCD_V_RES) {
        return;
    }
    /* Matches the vendor LVGL v9 example's coordinate transform. */
    x = LCD_H_RES - x;
    y = LCD_V_RES - y;
    data->point.x = (lv_coord_t)(x < 0 ? 0 : x >= LCD_H_RES ? LCD_H_RES - 1 : x);
    data->point.y = (lv_coord_t)(y < 0 ? 0 : y >= LCD_V_RES ? LCD_V_RES - 1 : y);
    data->state = LV_INDEV_STATE_PRESSED;
}

static void round_area(lv_event_t *event)
{
    lv_area_t *area = lv_event_get_param(event);
    area->x1 &= ~1;
    area->y1 &= ~1;
    area->x2 |= 1;
    area->y2 |= 1;
}

static esp_err_t init(void)
{
    /* GPIO18 holds the battery supply on. Assert it before the touch reset
     * delays so releasing the power key during boot cannot cut the rail. */
    const gpio_config_t vbat_gpio = {
        .pin_bit_mask = 1ULL << VBAT_ENABLE,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_set_level(VBAT_ENABLE, 1), TAG, "power hold level");
    ESP_RETURN_ON_ERROR(gpio_config(&vbat_gpio), TAG, "power hold");

    const i2c_master_bus_config_t i2c_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = TOUCH_SDA,
        .scl_io_num = TOUCH_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&i2c_cfg, &s_i2c), TAG, "i2c");

    const gpio_config_t touch_reset_gpio = {
        .pin_bit_mask = 1ULL << TOUCH_RST,
        .mode = GPIO_MODE_OUTPUT,
    };
    const gpio_config_t touch_int_gpio = {
        .pin_bit_mask = 1ULL << TOUCH_INT,
        .mode = GPIO_MODE_INPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&touch_reset_gpio), TAG, "touch reset pin");
    ESP_RETURN_ON_ERROR(gpio_config(&touch_int_gpio), TAG, "touch interrupt pin");
    /* Same reset waveform as Waveshare's LcdTouchPanel::ResetTouch(). */
    gpio_set_level(TOUCH_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(200));
    gpio_set_level(TOUCH_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(200));
    gpio_set_level(TOUCH_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(200));

    const i2c_device_config_t touch_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = TOUCH_ADDR,
        .scl_speed_hz = 300000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_i2c, &touch_cfg, &s_touch), TAG, "touch device");

    /* Both Waveshare keys are active low; GPIO17 is the PWR key input, not
     * an independent charging or USB-present indication. */
    ESP_RETURN_ON_ERROR(muse_gpio_button_init(&s_pwr, PWR_BUTTON), TAG, "pwr button");
    ESP_RETURN_ON_ERROR(muse_gpio_button_init(&s_boot, BOOT_BUTTON), TAG, "boot button");
    /* A key held while the board resets must not become a synthetic Muse edge. */
    s_pwr.pressed = gpio_get_level(PWR_BUTTON) == 0;
    s_boot.pressed = gpio_get_level(BOOT_BUTTON) == 0;

    const adc_oneshot_unit_init_cfg_t adc_cfg = { .unit_id = ADC_UNIT_1 };
    ESP_RETURN_ON_ERROR(adc_oneshot_new_unit(&adc_cfg, &s_adc), TAG, "adc");
    const adc_oneshot_chan_cfg_t adc_ch_cfg = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    ESP_RETURN_ON_ERROR(adc_oneshot_config_channel(s_adc, VBAT_ADC, &adc_ch_cfg), TAG, "adc channel");

    const adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id = ADC_UNIT_1,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    s_adc_calibrated = adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_adc_cali) == ESP_OK;
    if (!s_adc_calibrated) {
        ESP_LOGW(TAG, "battery ADC calibration unavailable; voltage and percentage unknown");
    }
    return ESP_OK;
}

static lv_display_t *display_start(lv_indev_t **touch)
{
    const spi_bus_config_t bus_cfg = {
        .sclk_io_num = LCD_SCLK,
        .data0_io_num = LCD_D0,
        .data1_io_num = LCD_D1,
        .data2_io_num = LCD_D2,
        .data3_io_num = LCD_D3,
        .max_transfer_sz = LCD_CHUNK_BYTES,
    };
    if (spi_bus_initialize(LCD_HOST, &bus_cfg, SPI_DMA_CH_AUTO) != ESP_OK) {
        return NULL;
    }
    const esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = LCD_CS,
        .dc_gpio_num = -1,
        .spi_mode = 0,
        .pclk_hz = 40 * 1000 * 1000,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 32,
        .lcd_param_bits = 8,
        .flags.quad_mode = true,
    };
    if (esp_lcd_new_panel_io_spi(LCD_HOST, &io_cfg, &s_io) != ESP_OK) {
        return NULL;
    }
    sh8601_vendor_config_t vendor_cfg = {
        .init_cmds = s_lcd_init,
        .init_cmds_size = sizeof(s_lcd_init) / sizeof(s_lcd_init[0]),
        .flags.use_qspi_interface = 1,
    };
    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor_cfg,
    };
    if (esp_lcd_new_panel_sh8601(s_io, &panel_cfg, &s_panel) != ESP_OK ||
        esp_lcd_panel_reset(s_panel) != ESP_OK ||
        esp_lcd_panel_init(s_panel) != ESP_OK) {
        return NULL;
    }
    esp_lcd_panel_set_gap(s_panel, LCD_GAP_X, 0);
    muse_lcd_bands_set_gap(LCD_GAP_X, 0);
    esp_lcd_panel_disp_on_off(s_panel, true);

    esp_lv_adapter_config_t adapter_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    adapter_cfg.task_core_id = MUSE_UI_CORE;
    adapter_cfg.task_priority = MUSE_UI_PRIORITY;
    if (esp_lv_adapter_init(&adapter_cfg) != ESP_OK) {
        return NULL;
    }
    const esp_lv_adapter_display_config_t display_cfg = {
        .panel = s_panel,
        .panel_io = s_io,
        .profile = {
            .interface = ESP_LV_ADAPTER_PANEL_IF_OTHER,
            .rotation = ESP_LV_ADAPTER_ROTATE_0,
            .hor_res = LCD_H_RES,
            .ver_res = LCD_V_RES,
        },
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE,
    };
    lv_display_t *display = muse_lcd_bands_register(display_cfg, DRAW_BUF_LINES, LCD_CHUNK_BYTES);
    if (!display) {
        return NULL;
    }
    muse_lcd_bands_set_te(LCD_TE);
    lv_display_add_event_cb(display, round_area, LV_EVENT_INVALIDATE_AREA, NULL);

    lv_indev_t *indev = lv_indev_create();
    if (!indev) {
        return NULL;
    }
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(indev, display);
    lv_indev_set_read_cb(indev, touch_read_cb);
    *touch = indev;
    if (esp_lv_adapter_start() != ESP_OK) {
        return NULL;
    }
    return display;
}

static bool display_lock(int timeout_ms)
{
    return esp_lv_adapter_lock(timeout_ms) == ESP_OK;
}

static void send_brightness(void *arg)
{
    uint8_t level = *(uint8_t *)arg;
    uint32_t cmd = (0x02 << 24) | (0x51 << 8);
    esp_lcd_panel_io_tx_param(s_io, cmd, &level, 1);
}

static void set_brightness(int pct)
{
    uint8_t level = (uint8_t)(pct < 0 ? 0 : pct > 100 ? 255 : pct * 255 / 100);
    muse_lcd_bands_run(send_brightness, &level);
}

static void send_sleep(void *arg)
{
    /* SH8601 versions without a disp_sleep callback still accept these DCS
     * commands. All panel IO after registration belongs to the send task. */
    uint32_t cmd = (0x02 << 24) | ((*(bool *)arg ? 0x10 : 0x11) << 8);
    esp_lcd_panel_io_tx_param(s_io, cmd, NULL, 0);
}

static void panel_sleep(bool sleep)
{
    muse_lcd_bands_run(send_sleep, &sleep);
    vTaskDelay(pdMS_TO_TICKS(120));
}

static void display_pause(bool pause)
{
    if (pause) {
        esp_lv_adapter_pause(-1);
    } else {
        esp_lv_adapter_resume();
    }
}

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
    ESP_RETURN_ON_ERROR(i2s_channel_enable(tx), TAG, "i2s tx enable");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(rx), TAG, "i2s rx enable");

    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = I2S_NUM_0,
        .rx_handle = rx,
        .tx_handle = tx,
    };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);
    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = I2C_NUM_0,
        .addr = ES8311_CODEC_DEFAULT_ADDR,
        .bus_handle = s_i2c,
    };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    ESP_RETURN_ON_FALSE(data_if && ctrl_if && gpio_if, ESP_ERR_NO_MEM, TAG, "codec interfaces");

    es8311_codec_cfg_t es_cfg = {
        .ctrl_if = ctrl_if,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH,
        .pa_pin = AUDIO_PA,
        .pa_reverted = false,
        .use_mclk = true,
        .hw_gain = { .pa_gain = 6.0f }, /* S3_AMOLED_1_32 in Waveshare board_cfg.txt */
    };
    const audio_codec_if_t *codec = es8311_codec_new(&es_cfg);
    ESP_RETURN_ON_FALSE(codec, ESP_FAIL, TAG, "ES8311 not responding");
    esp_codec_dev_cfg_t out_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = codec,
        .data_if = data_if,
    };
    esp_codec_dev_cfg_t in_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN,
        .codec_if = codec,
        .data_if = data_if,
    };
    *spk = esp_codec_dev_new(&out_cfg);
    *mic = esp_codec_dev_new(&in_cfg);
    return *spk && *mic ? ESP_OK : ESP_FAIL;
}

static unsigned poll_buttons(void)
{
    return muse_gpio_button_poll(&s_pwr) | (muse_gpio_button_poll(&s_boot) << 2);
}

static void wait_buttons(int timeout_ms)
{
    muse_gpio_buttons_wait((muse_gpio_button_t *const[]){ &s_pwr, &s_boot }, 2, timeout_ms);
}

static esp_err_t read_power(muse_power_t *out)
{
    *out = (muse_power_t){
        .battery_pct = -1,
        .usb = usb_serial_jtag_is_connected(),
    };
    /* USB powers the charger's VBAT node even without a cell (observed on
     * this board). No presence/status pin distinguishes it from a battery,
     * so report unknown while a USB data host is connected. This also clears
     * an earlier battery estimate when USB is plugged in. A standalone USB
     * charger cannot be detected by USB Serial/JTAG and remains ambiguous.
     * Without ADC calibration, counts must not be guessed as 0..3.3 V. */
    if (out->usb || !s_adc_calibrated) {
        return ESP_OK;
    }
    int raw = 0;
    ESP_RETURN_ON_ERROR(adc_oneshot_read(s_adc, VBAT_ADC, &raw), TAG, "battery adc");
    int mv = 0;
    ESP_RETURN_ON_ERROR(adc_cali_raw_to_voltage(s_adc_cali, raw, &mv), TAG, "battery calibration");
    mv *= 2; /* the vendor BatteryMonitor uses the board's 1:1 divider. */
    int pct = (mv - 3300) * 100 / 900;
    out->battery_mv = mv;
    out->battery_pct = pct < 0 ? 0 : pct > 100 ? 100 : pct;
    /* charging stays false: the vendor example has no charge-status GPIO.
     * USB Serial/JTAG detects a data host, not a standalone USB charger. */
    return ESP_OK;
}

static void send_display_off(void *arg)
{
    (void)arg;
    esp_lcd_panel_disp_on_off(s_panel, false);
}

static esp_err_t power_off(void)
{
    /* Waveshare's 04_BATT_PWR example calls this rail Vbat power and drives
     * GPIO18 low for power-off. Wait for a held PWR key to be released before
     * cutting the rail; deep sleep is a fallback if the rail does not drop
     * immediately on a particular hardware revision. */
    ESP_RETURN_ON_ERROR(esp_sleep_enable_ext0_wakeup(PWR_BUTTON, 0), TAG, "power wake");
    set_brightness(0);
    esp_lv_adapter_pause(-1);
    muse_lcd_bands_run(send_display_off, NULL);
    panel_sleep(true);
    while (gpio_get_level(PWR_BUTTON) == 0) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    gpio_set_level(AUDIO_PA, 0);
    gpio_set_level(VBAT_ENABLE, 0);
    esp_deep_sleep_start();
    return ESP_FAIL;
}

static const muse_board_t s_board = {
    .name = "Waveshare ESP32-S3-Touch-AMOLED-1.32",
    .width = LCD_H_RES,
    .height = LCD_V_RES,
    .round = true,
    .touch = true,
    .diagonal_in = 1.32f,
    .talk_button = "pwr",
    .aux_button = "boot",
    /* With USB at the bottom, PWR and BOOT sit along the upper-right rim
     * when viewed from the screen (mirror of Waveshare's rear resource photo).
     * Keep both hints clear of the top status and bottom page indicators. */
    .talk_hint = { LV_ALIGN_CENTER, 144, -134 },
    .aux_hint = { LV_ALIGN_CENTER, 190, -64 },
    .frame_ms = 40,
    .init = init,
    .display_start = display_start,
    .display_lock = display_lock,
    .display_unlock = esp_lv_adapter_unlock,
    .set_brightness = set_brightness,
    .panel_sleep = panel_sleep,
    .display_pause = display_pause,
    .draw_frame = muse_lcd_bands_frame,
    .audio_init = audio_init,
    /* ES8311's left slot is the mic; the default right slot is DAC reference.
     * Mixing them would feed speaker playback into captured voice. */
    .mic_slot = 0,
    .poll_buttons = poll_buttons,
    .wait_buttons = wait_buttons,
    .read_power = read_power,
    .power_off = power_off,
};

const muse_board_t *muse_board_get(void)
{
    return &s_board;
}
