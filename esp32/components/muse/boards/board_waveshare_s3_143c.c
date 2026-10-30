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
 * Waveshare ESP32-S3-Touch-AMOLED-1.43C: round 466 px SH8601 QSPI AMOLED with
 * CST820 touch over raw registers, ES8311 speaker DAC + ES7210 mic ADC on one
 * I2S bus, PA on GPIO46, codec LDO on GPIO18, battery divider on GPIO4
 * (ADC1 CH3), charge state on GPIO7, one BOOT button (GPIO0). No PMU and no
 * vendor BSP in the component registry: the panel runs on
 * espressif/esp_lcd_sh8601 with Waveshare's own init table, and touch is the
 * vendor's raw register protocol.
 *
 * Pins and sequences follow Waveshare's ESP32-S3-Touch-AMOLED-1.43C examples
 * and schematic: examples/esp-idf/<example>/components/port_bsp (bsp_config.h and
 * esp32_s3_touch_amoled_1.43c.cpp, also in examples/arduino) and
 * hardware/schematic: QSPI SCLK 14, D0-D3 9-12, CS 15,
 * RST 13, gap (8,0), brightness command 0x51; touch on I2C0 SDA 47/SCL 48 at
 * 0x15 with RST 16 and both axes mirrored; audio on I2S0 MCLK 38/BCLK 39/
 * WS 40/DOUT 41/DIN 42 with ES8311 out (PA 46, MCLK-fed) and ES7210 in
 * (MIC1 and MIC2 are the two microphones; MIC3 is the speaker loopback);
 * battery ADC1 CH3 with a x2 divider, GPIO7 low while charging.
 */
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/spi_master.h"
#include "driver/usb_serial_jtag.h"
#include "es8311_codec.h"
#include "es7210_adc.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_check.h"
#include "esp_codec_dev_defaults.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
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
#include "muse_ui.h"

static const char *TAG = "board";

/* Pins (Waveshare 1.43C BSP). */
#define LCD_SCLK GPIO_NUM_14
#define LCD_D0 GPIO_NUM_9
#define LCD_D1 GPIO_NUM_10
#define LCD_D2 GPIO_NUM_11
#define LCD_D3 GPIO_NUM_12
#define LCD_CS GPIO_NUM_15
#define LCD_RST GPIO_NUM_13
#define LCD_RES 466

#define I2C_SDA GPIO_NUM_47
#define I2C_SCL GPIO_NUM_48
#define TOUCH_ADDR 0x15
#define TOUCH_RST GPIO_NUM_16

#define I2S_MCLK GPIO_NUM_38
#define I2S_BCLK GPIO_NUM_39
#define I2S_WS GPIO_NUM_40
#define I2S_DOUT GPIO_NUM_41   /* to the ES8311 DAC */
#define I2S_DIN GPIO_NUM_42    /* from the ES7210 ADC */
#define PA_EN GPIO_NUM_46
#define CODEC_LDO_EN GPIO_NUM_18

#define BATT_ADC ADC_CHANNEL_3  /* GPIO4, x2 divider */
#define CHARGE_GPIO GPIO_NUM_7  /* low while charging */

#define TOUCH_I2C_TIMEOUT_MS 50  /* a stuck bus mustn't stall the UI task */

#define DRAW_BUF_LINES 118     /* four bands to the screen (muse_lcd_bands.h) */
#define LCD_CHUNK_BYTES (LCD_RES * 8 * 2)

static i2c_master_bus_handle_t s_i2c;
static i2c_master_dev_handle_t s_touch;
static esp_lcd_panel_handle_t s_panel;
static esp_lcd_panel_io_handle_t s_io;
static lv_indev_t *s_touch_indev;
static muse_gpio_button_t s_boot;
static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cali = NULL;

/* Waveshare's SH8601 init table (1.43C BSP). */
static const sh8601_lcd_init_cmd_t lcd_init_cmds[] = {
    { 0xFE, (uint8_t[]){ 0x00 }, 1, 0 },
    { 0xC4, (uint8_t[]){ 0x80 }, 1, 0 },
    { 0x3A, (uint8_t[]){ 0x55 }, 1, 0 },
    { 0x35, (uint8_t[]){ 0x00 }, 1, 0 },
    { 0x53, (uint8_t[]){ 0x20 }, 1, 0 },
    { 0x51, (uint8_t[]){ 0xFF }, 1, 0 },
    { 0x36, (uint8_t[]){ 0xC0 }, 1, 0 },
    { 0x63, (uint8_t[]){ 0xFF }, 1, 0 },
    { 0x2A, (uint8_t[]){ 0x00, 0x06, 0x01, 0xD7 }, 4, 0 },
    { 0x2B, (uint8_t[]){ 0x00, 0x00, 0x01, 0xD1 }, 4, 0 },
    { 0x11, (uint8_t[]){ 0x00 }, 0, 100 },
    { 0x29, (uint8_t[]){ 0x00 }, 0, 0 },
};

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

    const i2c_device_config_t tp_cfg = {
        .device_address = TOUCH_ADDR,
        .scl_speed_hz = 400000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_i2c, &tp_cfg, &s_touch), TAG, "touch dev");

    /* Reset pulse for the CST820, as the vendor BSP does. */
    const gpio_config_t rst = {
        .pin_bit_mask = 1ULL << TOUCH_RST,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&rst), TAG, "touch rst");
    ESP_RETURN_ON_ERROR(gpio_set_level(TOUCH_RST, 1), TAG, "touch rst high");
    vTaskDelay(pdMS_TO_TICKS(200));
    ESP_RETURN_ON_ERROR(gpio_set_level(TOUCH_RST, 0), TAG, "touch rst low");
    vTaskDelay(pdMS_TO_TICKS(200));
    ESP_RETURN_ON_ERROR(gpio_set_level(TOUCH_RST, 1), TAG, "touch rst high");
    vTaskDelay(pdMS_TO_TICKS(200));

    ESP_RETURN_ON_ERROR(muse_gpio_button_init(&s_boot, GPIO_NUM_0), TAG, "boot button");
    /* BOOT is held at flashing time; don't count a held button as a press. */
    s_boot.pressed = gpio_get_level(GPIO_NUM_0) == 0;

    /* Codec LDO first, then the speaker amp. */
    const gpio_config_t pwr = {
        .pin_bit_mask = (1ULL << CODEC_LDO_EN) | (1ULL << PA_EN),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&pwr), TAG, "codec pwr");
    ESP_RETURN_ON_ERROR(gpio_set_level(CODEC_LDO_EN, 1), TAG, "ldo on");
    ESP_RETURN_ON_ERROR(gpio_set_level(PA_EN, 1), TAG, "pa on");

    const gpio_config_t chg = {
        .pin_bit_mask = 1ULL << CHARGE_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&chg), TAG, "charge pin");

    const adc_oneshot_unit_init_cfg_t adc_cfg = { .unit_id = ADC_UNIT_1 };
    ESP_RETURN_ON_ERROR(adc_oneshot_new_unit(&adc_cfg, &s_adc), TAG, "adc");
    const adc_oneshot_chan_cfg_t ch_cfg = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_12 };
    ESP_RETURN_ON_ERROR(adc_oneshot_config_channel(s_adc, BATT_ADC, &ch_cfg), TAG, "adc channel");
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id = ADC_UNIT_1,
        .chan = BATT_ADC,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    if (adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_cali) != ESP_OK) {
        ESP_LOGW(TAG, "adc calibration unavailable; battery mV is uncalibrated");
    }
#endif
    return ESP_OK;
}

/* The SH8601 needs even-aligned update windows over the QSPI command path. */
static void round_area(lv_event_t *e)
{
    lv_area_t *a = lv_event_get_param(e);
    a->x1 &= ~1;
    a->y1 &= ~1;
    a->x2 |= 1;
    a->y2 |= 1;
}

static void touch_read(lv_indev_t *indev, lv_indev_data_t *data);

/* muse_lcd_bands_register takes over the panel IO's color-done callback and,
 * with the bands, needs the bus set up on the same core as the UI task. */
static lv_display_t *display_start(lv_indev_t **touch)
{
    const spi_bus_config_t bus = SH8601_PANEL_BUS_QSPI_CONFIG(LCD_SCLK, LCD_D0, LCD_D1, LCD_D2, LCD_D3,
                                                              LCD_CHUNK_BYTES);
    if (spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) {
        ESP_LOGE(TAG, "spi bus");
        return NULL;
    }
    const esp_lcd_panel_io_spi_config_t io_cfg = SH8601_PANEL_IO_QSPI_CONFIG(LCD_CS, NULL, NULL);
    if (esp_lcd_new_panel_io_spi(SPI2_HOST, &io_cfg, &s_io) != ESP_OK) {
        ESP_LOGE(TAG, "panel io");
        return NULL;
    }
    sh8601_vendor_config_t vendor_cfg = {
        .init_cmds = lcd_init_cmds,
        .init_cmds_size = sizeof(lcd_init_cmds) / sizeof(lcd_init_cmds[0]),
        .flags = { .use_qspi_interface = 1 },
    };
    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor_cfg,
    };
    if (esp_lcd_new_panel_sh8601(s_io, &panel_cfg, &s_panel) != ESP_OK) {
        ESP_LOGE(TAG, "sh8601");
        return NULL;
    }
    esp_lcd_panel_reset(s_panel);
    esp_lcd_panel_init(s_panel);
    esp_lcd_panel_set_gap(s_panel, 8, 0);
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

    /* Raw CST820 input: the vendor BSP's register protocol (0x02 count/event,
     * 0x03 coordinates), both axes mirrored. */
    lv_indev_t *indev = lv_indev_create();
    if (!indev) {
        return NULL;
    }
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(indev, disp);
    lv_indev_set_read_cb(indev, touch_read);
    *touch = indev;
    s_touch_indev = indev;
    if (esp_lv_adapter_start() != ESP_OK) {
        return NULL;
    }
    return disp;
}

/* Vendor touch protocol: register 0x02 holds the finger count and event,
 * register 0x03 the coordinates. Event 0x01 means released. */
static bool get_coords(uint16_t *x, uint16_t *y)
{
    uint8_t reg = 0x02;
    uint8_t hdr[2] = { 0, 0 };
    if (i2c_master_transmit_receive(s_touch, &reg, 1, hdr, sizeof(hdr), TOUCH_I2C_TIMEOUT_MS) != ESP_OK) {
        return false;
    }
    if (hdr[0] == 0 || (hdr[1] >> 6) == 0x01) {
        return false;
    }
    reg = 0x03;
    uint8_t pos[4] = { 0, 0, 0, 0 };
    if (i2c_master_transmit_receive(s_touch, &reg, 1, pos, sizeof(pos), TOUCH_I2C_TIMEOUT_MS) != ESP_OK) {
        return false;
    }
    *x = (uint16_t)(((uint16_t)(pos[0] & 0x0f) << 8) | pos[1]);
    *y = (uint16_t)(((uint16_t)(pos[2] & 0x0f) << 8) | pos[3]);
    return true;
}

/* Both axes mirrored; a raw reading past the panel's edge clamps to the
 * mirrored edge (0), not the opposite one. */
static int mirror_axis(uint16_t raw)
{
    return raw >= LCD_RES ? 0 : LCD_RES - 1 - raw;
}

static void touch_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    uint16_t tx = 0, ty = 0;
    if (get_coords(&tx, &ty)) {
        data->point.x = mirror_axis(tx);
        data->point.y = mirror_axis(ty);
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

static bool display_lock(int timeout_ms)
{
    return esp_lv_adapter_lock(timeout_ms) == ESP_OK;
}

static void send_brightness(void *level)
{
    /* SH8601 "write display brightness" (0x51), as the vendor BSP sends it. */
    esp_lcd_panel_io_tx_param(s_io, (0x02 << 24) | (0x51 << 8), level, 1);
}

static void set_brightness(int pct)
{
    uint8_t level = (uint8_t)(pct * 255 / 100);
    muse_lcd_bands_run(send_brightness, &level);
}

/* Panel commands go through the bands' send task, between bands, so they
 * never land in the middle of a pixel write (muse_lcd_bands.h). */
static void send_sleep(void *sleep)
{
    esp_lcd_panel_disp_sleep(s_panel, *(bool *)sleep);   /* SLPIN/SLPOUT; GRAM is kept */
}

static void send_off(void *arg)
{
    (void)arg;
    esp_lcd_panel_disp_on_off(s_panel, false);
    esp_lcd_panel_disp_sleep(s_panel, true);
}

static void panel_sleep(bool sleep)
{
    muse_lcd_bands_run(send_sleep, &sleep);
    vTaskDelay(pdMS_TO_TICKS(120));   /* settle before the next command */
}

static void display_pause(bool pause)
{
    if (pause) {
        esp_lv_adapter_pause(-1);
    } else {
        esp_lv_adapter_resume();
    }
}

/*
 * ES8311 speaker DAC + ES7210 mic ADC on one I2S bus, following the vendor
 * board config (MCLK 38, BCLK 39, WS 40, DOUT 41 to the ES8311, DIN 42 from
 * the ES7210; PA on 46, MCLK-fed ES8311). Dual mics selected and mixed.
 */
static esp_err_t audio_init(esp_codec_dev_handle_t *spk, esp_codec_dev_handle_t *mic)
{
    i2s_chan_handle_t tx, rx;
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &tx, &rx), TAG, "i2s chan");
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

    const audio_codec_data_if_t *spk_data = audio_codec_new_i2s_data(&(audio_codec_i2s_cfg_t){
        .port = I2S_NUM_0, .rx_handle = NULL, .tx_handle = tx });
    ESP_RETURN_ON_FALSE(spk_data, ESP_ERR_NO_MEM, TAG, "spk data if");
    const audio_codec_ctrl_if_t *spk_ctrl = audio_codec_new_i2c_ctrl(&(audio_codec_i2c_cfg_t){
        .port = I2C_NUM_0, .addr = ES8311_CODEC_DEFAULT_ADDR, .bus_handle = s_i2c });
    const audio_codec_gpio_if_t *spk_gpio = audio_codec_new_gpio();
    ESP_RETURN_ON_FALSE(spk_ctrl && spk_gpio, ESP_ERR_NO_MEM, TAG, "spk ctrl");
    const audio_codec_if_t *spk_codec = es8311_codec_new(&(es8311_codec_cfg_t){
        .ctrl_if = spk_ctrl,
        .gpio_if = spk_gpio,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = PA_EN,
        .use_mclk = true,
    });
    ESP_RETURN_ON_FALSE(spk_codec, ESP_FAIL, TAG, "ES8311 not responding");
    *spk = esp_codec_dev_new(&(esp_codec_dev_cfg_t){
        .dev_type = ESP_CODEC_DEV_TYPE_OUT, .codec_if = spk_codec, .data_if = spk_data });
    ESP_RETURN_ON_FALSE(*spk, ESP_FAIL, TAG, "spk dev");

    const audio_codec_data_if_t *mic_data = audio_codec_new_i2s_data(&(audio_codec_i2s_cfg_t){
        .port = I2S_NUM_0, .rx_handle = rx, .tx_handle = NULL });
    ESP_RETURN_ON_FALSE(mic_data, ESP_ERR_NO_MEM, TAG, "mic data if");
    const audio_codec_ctrl_if_t *mic_ctrl = audio_codec_new_i2c_ctrl(&(audio_codec_i2c_cfg_t){
        .port = I2C_NUM_0, .addr = ES7210_CODEC_DEFAULT_ADDR, .bus_handle = s_i2c });
    ESP_RETURN_ON_FALSE(mic_ctrl, ESP_ERR_NO_MEM, TAG, "mic ctrl");
    const audio_codec_if_t *mic_codec = es7210_codec_new(&(es7210_codec_cfg_t){
        .ctrl_if = mic_ctrl,
        .mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2,
    });
    ESP_RETURN_ON_FALSE(mic_codec, ESP_FAIL, TAG, "ES7210 not responding");
    *mic = esp_codec_dev_new(&(esp_codec_dev_cfg_t){
        .dev_type = ESP_CODEC_DEV_TYPE_IN, .codec_if = mic_codec, .data_if = mic_data });
    ESP_RETURN_ON_FALSE(*mic, ESP_FAIL, TAG, "mic dev");

    ESP_LOGI(TAG, "audio: ES8311 (spk) + ES7210 (mic) on I2S0");
    return ESP_OK;
}

/* Hold-to-talk on the face tile: a finger held on the avatar for
 * TOUCH_HOLD_MS acts as the talk button. Taps and touches on other tiles
 * (e.g. settings) never trigger talk, and neither does a slow swipe off
 * the face: once LVGL starts scrolling, or the face is no longer the
 * visible tile, the pending hold is dropped until the finger lifts. */
#define TOUCH_HOLD_MS 500

static unsigned poll_buttons(void)
{
    unsigned ev = muse_gpio_button_poll(&s_boot);

    static bool tp_down = false;
    static bool tp_talking = false;
    static TickType_t tp_press_tick = 0;
    static bool tp_on_face = false;

    bool pressed = false;
    if (s_touch_indev) {
        pressed = (lv_indev_get_state(s_touch_indev) == LV_INDEV_STATE_PRESSED);
    }

    if (pressed && !tp_down) {
        tp_down = true;
        tp_on_face = muse_ui_face_active();
        tp_press_tick = xTaskGetTickCount();
        tp_talking = false;
    } else if (!pressed && tp_down) {
        if (tp_talking) {
            ev |= MUSE_BTN_TALK_RELEASE;
        }
        tp_down = false;
        tp_talking = false;
    } else if (pressed && tp_down && tp_on_face && !tp_talking) {
        if (lv_indev_get_scroll_obj(s_touch_indev) || !muse_ui_face_active()) {
            tp_on_face = false;
        } else if ((xTaskGetTickCount() - tp_press_tick) >= pdMS_TO_TICKS(TOUCH_HOLD_MS)) {
            tp_talking = true;
            ev |= MUSE_BTN_TALK_PRESS;
        }
    }
    return ev;
}

static void wait_buttons(int timeout_ms)
{
    muse_gpio_buttons_wait((muse_gpio_button_t *const[]){ &s_boot }, 1, timeout_ms);
}

/*
 * Battery on GPIO4 (ADC1 CH3) through the board's x2 divider, as the vendor
 * BSP measures it. GPIO7 reads low while charging.
 */
static esp_err_t read_power(muse_power_t *out)
{
    int sum = 0;
    for (int i = 0; i < 8; i++) {
        int v;
        ESP_RETURN_ON_ERROR(adc_oneshot_read(s_adc, BATT_ADC, &v), TAG, "adc read");
        sum += v;
    }
    int raw = sum / 8;
    int mv;
    if (s_cali) {
        ESP_RETURN_ON_ERROR(adc_cali_raw_to_voltage(s_cali, raw, &mv), TAG, "adc cali");
    } else {
        mv = raw * 3300 / 4096;   /* 12 dB attenuation, full scale ~3.3 V */
    }
    out->battery_mv = mv * 2;
    /* Rough Li-ion window: 3.3 V empty, 4.2 V full. Far below empty, no
     * cell is fitted: report no battery rather than 0%. */
    int pct = (out->battery_mv - 3300) * 100 / (4200 - 3300);
    out->battery_pct = out->battery_mv < 2500 ? -1 : pct < 0 ? 0 : pct > 100 ? 100 : pct;
    out->charging = gpio_get_level(CHARGE_GPIO) == 0;
    /* No VBUS sense: a USB host on the chip's own port is the closest. */
    out->usb = usb_serial_jtag_is_connected();
    return ESP_OK;
}

static esp_err_t power_off(void)
{
    set_brightness(0);
    muse_lcd_bands_run(send_off, NULL);
    /* No PMU on this board; deep sleep is the closest thing to off. BOOT
     * wakes it. */
    esp_sleep_enable_ext0_wakeup(GPIO_NUM_0, 0);
    esp_deep_sleep_start();
    return ESP_FAIL;
}

static const muse_board_t s_board = {
    .name = "Waveshare ESP32-S3-Touch-AMOLED-1.43C",
    .width = LCD_RES,
    .height = LCD_RES,
    .round = true,
    .touch = true,
    .diagonal_in = 1.43f,
    .talk_button = "BOOT",
    /* BOOT is the only button, on the right rim of the puck. */
    .talk_hint = { LV_ALIGN_CENTER, 153, -129 },    /* 40 degrees above 3 o'clock */
    .frame_ms = 40,
    .init = init,
    .display_start = display_start,
    .display_lock = display_lock,
    .display_unlock = esp_lv_adapter_unlock,
    .set_brightness = set_brightness,
    .panel_sleep = panel_sleep,
    .display_pause = display_pause,
    .audio_init = audio_init,
    .mic_slot = -1,   /* dual mics, mixed */
    .set_mic_gain = NULL,   /* default: esp_codec_dev_set_in_gain */
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
