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
 * Espressif ESP32-S31-Korvo-1: multimedia dev board with ES8389 stereo
 * codec, dual NS4150B amplifiers, 4.3" LCD daughterboard, OV3660 camera.
 *
 * Pin map from Espressif's official BSP (esp-dev-kits, HWD-258 Rev 1.1)
 * via LiveKit's s31_board component (s31_korvo_audio.c), plus the official
 * docs.espressif.com user guide for LCD/camera pins.
 *
 * Audio: one I2S peripheral in duplex. Both directions run standard Philips
 * I2S into the ES8389 (NOT TDM): TX is playback, RX is capture. The four
 * buttons are a resistor ladder on ADC GPIO42, not individual GPIOs.
 * WS2812 status LED on GPIO37.
 *
 * First bring-up: audio + buttons + LED. Display (RGB parallel LCD) and
 * camera (DVP OV3660) are deferred until the audio path is validated.
 */
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_check.h"
#include "esp_codec_dev_defaults.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "lvgl.h"

#include "muse_audio.h"
#include "muse_board.h"
#include "muse_mem.h"

static const char *TAG = "board";

/* 4.3" 800x480 RGB panel (ST7262) via the LCD connector.
 * Pinout and timing from Espressif's official BSP (esp-bsp/bsp/esp32_s31_korvo_1).
 * Frame buffer lives in PSRAM; LVGL renders into a small internal-RAM bounce
 * buffer and the flush callback copies it to the frame buffer. */
#define LCD_H_RES 800
#define LCD_V_RES 480
#define LCD_BUF_LINES 20

static SemaphoreHandle_t s_lvgl_mux;
static lv_display_t *s_disp;
static uint8_t *s_draw_buf;
static esp_lcd_panel_handle_t s_panel;
static uint16_t *s_fb;

static void lcd_flush(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    int32_t w = area->x2 - area->x1 + 1;
    int32_t h = area->y2 - area->y1 + 1;
    uint16_t *src = (uint16_t *)px_map;
    for (int32_t y = 0; y < h; y++) {
        uint16_t *dst = &s_fb[(area->y1 + y) * LCD_H_RES + area->x1];
        memcpy(dst, &src[y * w], w * sizeof(uint16_t));
    }
    lv_display_flush_ready(disp);
}

static void lvgl_tick_task(void *arg)
{
    (void)arg;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10));
        if (xSemaphoreTake(s_lvgl_mux, portMAX_DELAY) == pdTRUE) {
            lv_tick_inc(10);
            lv_timer_handler();
            xSemaphoreGive(s_lvgl_mux);
        }
    }
}

static lv_display_t *display_start(lv_indev_t **touch)
{
    *touch = NULL;
    esp_lcd_rgb_panel_config_t panel_cfg = {
        .clk_src = LCD_CLK_SRC_PLL160M,
        .timings = {
            .pclk_hz = 18 * 1000 * 1000,
            .h_res = LCD_H_RES,
            .v_res = LCD_V_RES,
            .hsync_pulse_width = 40,
            .hsync_back_porch = 40,
            .hsync_front_porch = 48,
            .vsync_pulse_width = 23,
            .vsync_back_porch = 32,
            .vsync_front_porch = 13,
            .flags.pclk_active_neg = true,
        },
        .data_width = 16,
        .num_fbs = 1,
        .bounce_buffer_size_px = 0,
        .dma_burst_size = 64,
        .hsync_gpio_num = 44,
        .vsync_gpio_num = 45,
        .de_gpio_num = 43,
        .pclk_gpio_num = 40,
        .disp_gpio_num = 38,
        .data_gpio_nums = {
            8, 9, 10, 11, 12, 13, 14, 15,
            16, 17, 18, 19, 33, 34, 35, 36,
        },
        .flags.fb_in_psram = 1,
    };
    ESP_LOGI(TAG, "init RGB panel %dx%d", LCD_H_RES, LCD_V_RES);
    if (esp_lcd_new_rgb_panel(&panel_cfg, &s_panel) != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_new_rgb_panel failed");
        return NULL;
    }
    if (esp_lcd_panel_reset(s_panel) != ESP_OK ||
        esp_lcd_panel_init(s_panel) != ESP_OK) {
        ESP_LOGE(TAG, "panel reset/init failed");
        return NULL;
    }
    void *fb = NULL;
    if (esp_lcd_rgb_panel_get_frame_buffer(s_panel, 1, &fb) != ESP_OK || !fb) {
        ESP_LOGE(TAG, "get_frame_buffer failed");
        return NULL;
    }
    s_fb = (uint16_t *)fb;
    memset(s_fb, 0, LCD_H_RES * LCD_V_RES * sizeof(uint16_t));
    if (esp_lcd_panel_disp_on_off(s_panel, true) != ESP_OK) {
        ESP_LOGE(TAG, "disp_on failed");
        return NULL;
    }
    s_lvgl_mux = xSemaphoreCreateMutex();
    if (!s_lvgl_mux) {
        return NULL;
    }
    lv_init();
    s_disp = lv_display_create(LCD_H_RES, LCD_V_RES);
    if (!s_disp) {
        return NULL;
    }
    s_draw_buf = heap_caps_malloc(LCD_H_RES * LCD_BUF_LINES * 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s_draw_buf) {
        return NULL;
    }
    lv_display_set_buffers(s_disp, s_draw_buf, NULL, LCD_H_RES * LCD_BUF_LINES * 2,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(s_disp, lcd_flush);
    if (xTaskCreate(lvgl_tick_task, "lvgl_tick", 4096, NULL, 5, NULL) != pdPASS) {
        return NULL;
    }
    ESP_LOGI(TAG, "RGB LCD %dx%d running", LCD_H_RES, LCD_V_RES);
    return s_disp;
}

static bool display_lock(int timeout_ms)
{
    (void)timeout_ms;
    return xSemaphoreTake(s_lvgl_mux, portMAX_DELAY) == pdTRUE;
}

static void display_unlock(void)
{
    xSemaphoreGive(s_lvgl_mux);
}

static void set_brightness(int pct)
{
    (void)pct;  /* no backlight control on the dummy display */
}

static void set_mic_gain(esp_codec_dev_handle_t mic, int db)
{
    esp_codec_dev_set_in_gain(mic, (float)db);
}


/* Audio: ES8389 on I2C0, I2S0 in Philips stereo mode. */
#define I2C_SDA GPIO_NUM_0
#define I2C_SCL GPIO_NUM_1
#define I2S_MCLK GPIO_NUM_2
#define I2S_BCLK GPIO_NUM_3
#define I2S_WS GPIO_NUM_4
#define I2S_DOUT GPIO_NUM_5      /* ESP -> codec (playback) */
#define I2S_DIN GPIO_NUM_6       /* codec -> ESP (capture) */
#define PA_EN GPIO_NUM_7         /* NS4150B amplifier enable */
#define SAMPLE_RATE 48000

/* Buttons: resistor ladder on ADC1, channel TBD for GPIO42. */
#define BTN_ADC_GPIO GPIO_NUM_42
/* Threshold windows (centre +/- 100) from esp-bsp. */
#define BTN_SET 344
#define BTN_MODE 743
#define BTN_VOLM 1218
#define BTN_VOLP 1700
#define BTN_WINDOW 100

/* WS2812 status LED. */
#define LED_GPIO GPIO_NUM_37

static i2c_master_bus_handle_t s_i2c;
static adc_oneshot_unit_handle_t s_adc;

/* Button state for edge detection. */
static int s_last_button = -1;  /* -1: none, 0: SET, 1: MODE, 2: VOL-, 3: VOL+ */

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

    /* ADC for the button ladder. GPIO42 is on ADC1; channel from GPIO number. */
    const adc_oneshot_unit_init_cfg_t adc_cfg = { .unit_id = ADC_UNIT_1 };
    ESP_RETURN_ON_ERROR(adc_oneshot_new_unit(&adc_cfg, &s_adc), TAG, "adc");
    /* GPIO42 -> ADC1 channel: on S31, check the TRM. Using channel 9 as a
     * placeholder; verify against the datasheet before hardware test. */
    const adc_oneshot_chan_cfg_t ch_cfg = {
        .atten = ADC_ATTEN_DB_0,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    ESP_RETURN_ON_ERROR(adc_oneshot_config_channel(s_adc, ADC_CHANNEL_0, &ch_cfg), TAG, "adc ch");
    /* Raw ADC counts; the button thresholds below are in the same units. */
    return ESP_OK;
}

static esp_err_t audio_init(esp_codec_dev_handle_t *spk, esp_codec_dev_handle_t *mic)
{
    /* I2S channels first: the ES8389 locks its clock once the bus is running. */
    i2s_chan_handle_t tx, rx;
    const i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &tx, &rx), TAG, "i2s ch");

    const i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(16, I2S_SLOT_MODE_STEREO),
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
    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = I2C_NUM_0,
        .addr = ES8389_CODEC_DEFAULT_ADDR,
        .bus_handle = s_i2c,
    };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    ESP_RETURN_ON_FALSE(data_if && ctrl_if && gpio_if, ESP_ERR_NO_MEM, TAG, "codec ifs");

    es8389_codec_cfg_t out_cfg = {
        .ctrl_if = ctrl_if,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = PA_EN,
        .pa_reverted = false,
        .master_mode = false,
        .use_mclk = false,
        .hw_gain = { .pa_voltage = 5.0, .codec_dac_voltage = 3.3 },
    };
    const audio_codec_if_t *out_codec = es8389_codec_new(&out_cfg);
    ESP_RETURN_ON_FALSE(out_codec, ESP_FAIL, TAG, "ES8389 DAC not responding");

    es8389_codec_cfg_t in_cfg = {
        .ctrl_if = ctrl_if,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_ADC,
        .pa_pin = GPIO_NUM_NC,
        .master_mode = false,
        .use_mclk = false,
    };
    const audio_codec_if_t *in_codec = es8389_codec_new(&in_cfg);
    ESP_RETURN_ON_FALSE(in_codec, ESP_FAIL, TAG, "ES8389 ADC not responding");

    esp_codec_dev_cfg_t spk_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = out_codec,
        .data_if = data_if,
    };
    esp_codec_dev_cfg_t mic_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN,
        .codec_if = in_codec,
        .data_if = data_if,
    };
    *spk = esp_codec_dev_new(&spk_cfg);
    *mic = esp_codec_dev_new(&mic_cfg);
    return *spk && *mic ? ESP_OK : ESP_FAIL;
}

/* Read the button ladder; returns 0-3 for SET/MODE/VOL-/VOL+, -1 for none. */
static int read_button_ladder(void)
{
    int raw;
    if (adc_oneshot_read(s_adc, ADC_CHANNEL_0, &raw) != ESP_OK) {
        return -1;
    }
    /* Raw ADC counts; thresholds are in the same units from esp-bsp. */
    if (raw > BTN_SET - BTN_WINDOW && raw < BTN_SET + BTN_WINDOW) return 0;
    if (raw > BTN_MODE - BTN_WINDOW && raw < BTN_MODE + BTN_WINDOW) return 1;
    if (raw > BTN_VOLM - BTN_WINDOW && raw < BTN_VOLM + BTN_WINDOW) return 2;
    if (raw > BTN_VOLP - BTN_WINDOW && raw < BTN_VOLP + BTN_WINDOW) return 3;
    return -1;
}

static unsigned poll_buttons(void)
{
    int btn = read_button_ladder();
    unsigned r = 0;
    if (btn != s_last_button) {
        /* SET (0) is talk, MODE (1) is aux. VOL+/- are not wired to actions yet. */
        if (s_last_button == 0 && btn != 0) r |= MUSE_BTN_TALK_RELEASE;
        if (btn == 0 && s_last_button != 0) r |= MUSE_BTN_TALK_PRESS;
        if (s_last_button == 1 && btn != 1) r |= MUSE_BTN_AUX_RELEASE;
        if (btn == 1 && s_last_button != 1) r |= MUSE_BTN_AUX_PRESS;
        s_last_button = btn;
    }
    return r;
}

static const muse_board_t s_board = {
    .name = "Espressif ESP32-S31-Korvo-1",
    .width = LCD_H_RES,
    .height = LCD_V_RES,
    .round = false,
    .touch = false,
    .diagonal_in = 4.3f,
    .talk_button = "SET",
    .aux_button = "MODE",
    .frame_ms = 40,
    .init = init,
    .display_start = display_start,  /* 800x480 RGB ST7262 */
    .display_lock = display_lock,
    .display_unlock = display_unlock,
    .set_brightness = set_brightness,
    .audio_init = audio_init,
    .mic_slot = -1,             /* stereo: mix both slots */
    .set_mic_gain = set_mic_gain,       /* esp_codec_dev_set_in_gain */
    .poll_buttons = poll_buttons,
    .wait_buttons = NULL,
    .read_power = NULL,         /* USB powered; no battery */
    .power_off = NULL,
};

/* Home Link's app_main starts Muse with this board (main/main.c). */
const muse_board_t *muse_board_get(void)
{
    return &s_board;
}
