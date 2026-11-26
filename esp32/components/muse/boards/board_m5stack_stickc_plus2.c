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
 * M5Stack StickC Plus2: ESP32-PICO-V3-02 (classic ESP32, 8 MB flash, 2 MB
 * PSRAM), 1.14" 135x240 ST7789 LCD (no touch), a PDM mic, a passive buzzer,
 * three buttons and a CH9102 USB bridge. There's no codec, PMU or charger
 * status: the power button switches the regulator on and the firmware holds it
 * on (GPIO4), and the battery is read through a divider on an ADC pin. Pins are
 * from M5Unified (src/M5Unified.inl, utility/Power_Class.inl) and M5GFX
 * (src/M5GFX.cpp, "M5StickCPlus2"), github.com/m5stack/M5Unified.
 */
#include <math.h>

#include "driver/i2s_pdm.h"
#include "driver/i2s_std.h"
#include "driver/ledc.h"
#include "driver/rtc_io.h"
#include "driver/spi_master.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_check.h"
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
#include "muse_input.h"
#include "muse_mem.h"
#include "muse_state.h"

static const char *TAG = "board";

#define LCD_W 135
#define LCD_H 240
#define LCD_GAP_X 52               /* the 135x240 window in the controller's 240x320 */
#define LCD_GAP_Y 40
#define LCD_HOST SPI2_HOST
#define LCD_SCLK GPIO_NUM_13
#define LCD_MOSI GPIO_NUM_15
#define LCD_CS GPIO_NUM_5
#define LCD_DC GPIO_NUM_14
#define LCD_RST GPIO_NUM_12
#define LCD_BL GPIO_NUM_27
#define BL_HZ 256                  /* M5GFX's backlight PWM */
#define BL_MIN 160                 /* of 1023: dimmer is dark (M5GFX's offset 40 of 255) */
#define DRAW_BUF_LINES 32

#define MIC_CLK GPIO_NUM_0
#define MIC_DATA GPIO_NUM_34
#define BUZZER GPIO_NUM_2
#define LED GPIO_NUM_19            /* red, active high; shares the pin with the IR LED */

#define TALK_GPIO GPIO_NUM_37      /* button A, the big one under the screen */
#define AUX_GPIO GPIO_NUM_39       /* button B, on the side */
#define PWR_GPIO GPIO_NUM_35       /* the power button */
#define POWER_HOLD GPIO_NUM_4
#define PWR_HOLD_MS 2000           /* holding the power button powers off */
#define BATT_ADC ADC_CHANNEL_2     /* GPIO38, behind a 1:2 divider */

/*
 * Audio without a codec. I2S0 turns the PDM mic into PCM in hardware. The
 * buzzer is driven as M5Unified drives it: a first-order 1-bit delta-sigma
 * stream on I2S1's data line alone, 32 bits per frame at three times Muse's
 * rate. esp_codec_dev adds the speaker volume in software; the mic gain is
 * applied here.
 */
#define SPK_UP 3                   /* 48 kHz frames, 1.536 Mbit/s */
#define SPK_GAIN 2                 /* the buzzer is quiet: louder, clipping the peaks */
#define SPK_BLOCK 64               /* frames converted per I2S write */
#define MIC_GAIN_OFFSET_DB 6       /* Muse's default 30 dB lands on M5Unified's x16 */

static esp_lcd_panel_handle_t s_panel;
static muse_gpio_button_t s_talk, s_aux, s_pwr;
static bool s_pwr_waking;          /* this press of the power button woke the screen */
static TickType_t s_pwr_down_at;
static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cali;
static i2s_chan_handle_t s_mic_rx, s_spk_tx;
static bool s_mic_on;
static int s_mic_gain_q8 = 256;
static int32_t s_mic_dc[2];        /* each slot's zero level, x256 */
static bool s_mic_settled;         /* s_mic_dc set from a first read */
static int32_t s_dsm;              /* delta-sigma error, 0..0xFFFF */
static int s_spk_prev;

static esp_err_t init(void)
{
    /* The power button only turns the regulator on while it's held: keep it
     * on. Held, since light sleep powers down the RTC pad registers. */
    rtc_gpio_init(POWER_HOLD);
    rtc_gpio_set_direction(POWER_HOLD, RTC_GPIO_MODE_OUTPUT_ONLY);
    rtc_gpio_set_level(POWER_HOLD, 1);
    rtc_gpio_hold_en(POWER_HOLD);

    const gpio_config_t led = { .pin_bit_mask = BIT64(LED), .mode = GPIO_MODE_OUTPUT };
    ESP_RETURN_ON_ERROR(gpio_config(&led), TAG, "led");
    gpio_set_level(LED, 0);

    /* All three have pull-ups on the board. */
    ESP_RETURN_ON_ERROR(muse_gpio_button_init(&s_talk, TALK_GPIO), TAG, "talk button");
    ESP_RETURN_ON_ERROR(muse_gpio_button_init(&s_aux, AUX_GPIO), TAG, "aux button");
    ESP_RETURN_ON_ERROR(muse_gpio_button_init(&s_pwr, PWR_GPIO), TAG, "power button");
    /* A button that turned the board on or woke it is still down; don't count it. */
    s_talk.pressed = gpio_get_level(TALK_GPIO) == 0;
    s_aux.pressed = gpio_get_level(AUX_GPIO) == 0;
    s_pwr.pressed = gpio_get_level(PWR_GPIO) == 0;

    const adc_oneshot_unit_init_cfg_t adc_cfg = { .unit_id = ADC_UNIT_1 };
    ESP_RETURN_ON_ERROR(adc_oneshot_new_unit(&adc_cfg, &s_adc), TAG, "adc");
    const adc_oneshot_chan_cfg_t ch_cfg = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_12 };
    ESP_RETURN_ON_ERROR(adc_oneshot_config_channel(s_adc, BATT_ADC, &ch_cfg), TAG, "battery adc");
    const adc_cali_line_fitting_config_t cali_cfg = {
        .unit_id = ADC_UNIT_1,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
        .default_vref = 1100,      /* chips without the reference in eFuse */
    };
    return adc_cali_create_scheme_line_fitting(&cali_cfg, &s_cali);
}

static lv_display_t *display_start(lv_indev_t **touch)
{
    (void)touch;
    const ledc_timer_config_t bl_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = BL_HZ,
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
            .use_psram = false,    /* the classic ESP32's SPI DMA can't reach PSRAM */
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
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, pct > 0 ? BL_MIN + pct * (1023 - BL_MIN) / 100 : 0);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

static void panel_sleep(bool sleep)
{
    esp_lcd_panel_disp_sleep(s_panel, sleep);   /* SLPIN/SLPOUT; GRAM is kept */
}

static int mic_enable(const audio_codec_data_if_t *h, esp_codec_dev_type_t type, bool on)
{
    (void)h;
    (void)type;
    if (on != s_mic_on) {
        if ((on ? i2s_channel_enable(s_mic_rx) : i2s_channel_disable(s_mic_rx)) != ESP_OK) {
            return ESP_CODEC_DEV_DRV_ERR;
        }
        s_mic_on = on;
        s_mic_settled = false;
    }
    return ESP_CODEC_DEV_OK;
}

/*
 * Both PDM slots, as the 2-slot stream muse_audio reads; the mic answers on
 * one. The ESP32's PDM-to-PCM filter leaves a large DC offset, which the gain
 * would clip: each slot's zero level is tracked (a 10 Hz high-pass; M5Unified
 * tracks it too) and taken off first.
 */
static int mic_read(const audio_codec_data_if_t *h, uint8_t *data, int size)
{
    (void)h;
    size_t got;
    if (i2s_channel_read(s_mic_rx, data, size, &got, pdMS_TO_TICKS(1000)) != ESP_OK || got != (size_t)size) {
        return ESP_CODEC_DEV_READ_FAIL;
    }
    int16_t *s = (int16_t *)data;
    int n = size / 2;
    if (!s_mic_settled) {
        int32_t sum[2] = { 0, 0 };
        for (int i = 0; i < n; i++) {
            sum[i & 1] += s[i];
        }
        s_mic_dc[0] = sum[0] / (n / 2) * 256;
        s_mic_dc[1] = sum[1] / (n / 2) * 256;
        s_mic_settled = true;
    }
    for (int i = 0; i < n; i++) {
        int32_t *dc = &s_mic_dc[i & 1];
        *dc += (s[i] * 256 - *dc) >> 8;
        int v = (s[i] - (*dc >> 8)) * s_mic_gain_q8 >> 8;
        s[i] = v > INT16_MAX ? INT16_MAX : v < INT16_MIN ? INT16_MIN : v;
    }
    return ESP_CODEC_DEV_OK;
}

static void set_mic_gain(esp_codec_dev_handle_t mic, int db)
{
    (void)mic;
    s_mic_gain_q8 = (int)(256.0f * powf(10.0f, (db - MIC_GAIN_OFFSET_DB) / 20.0f));
}

/* The buzzer's channel never stops: stopped, it could leave the line high and
 * current flowing through the coil. Idle, it sends zeros (auto_clear). */
static int spk_enable(const audio_codec_data_if_t *h, esp_codec_dev_type_t type, bool on)
{
    (void)h;
    (void)type;
    (void)on;
    s_dsm = 0x8000;
    s_spk_prev = 0;
    return ESP_CODEC_DEV_OK;
}

static int spk_write(const audio_codec_data_if_t *h, uint8_t *data, int size)
{
    (void)h;
    static uint16_t bits[SPK_BLOCK * SPK_UP * 2];
    const int16_t *in = (const int16_t *)data;
    for (int frames = size / 4; frames > 0;) {
        int n = frames > SPK_BLOCK ? SPK_BLOCK : frames;
        uint16_t *out = bits;
        for (int i = 0; i < n; i++) {
            int cur = in[2 * i] * SPK_GAIN;   /* both slots carry the same sample */
            for (int k = 1; k <= SPK_UP; k++) {
                int v = s_spk_prev + (cur - s_spk_prev) * k / SPK_UP;
                v = v > INT16_MAX ? INT16_MAX : v < INT16_MIN ? INT16_MIN : v;
                /* M5Unified's loop: a one each time the error wraps, so ones
                 * make up (v + 32768) / 65536 of the stream. Slots go out in
                 * memory order, most significant bit first. */
                int32_t d = INT16_MIN - v;
                for (int slot = 0; slot < 2; slot++) {
                    uint16_t w = 0;
                    for (uint16_t bit = 0x8000; bit; bit >>= 1) {
                        if ((s_dsm += d) < 0) {
                            s_dsm += 0x10000;
                            w |= bit;
                        }
                    }
                    *out++ = w;
                }
            }
            s_spk_prev = cur;
        }
        size_t wrote;
        if (i2s_channel_write(s_spk_tx, bits, (out - bits) * sizeof(bits[0]), &wrote, portMAX_DELAY) != ESP_OK) {
            return ESP_CODEC_DEV_WRITE_FAIL;
        }
        in += 2 * n;
        frames -= n;
    }
    return ESP_CODEC_DEV_OK;
}

static esp_err_t audio_init(esp_codec_dev_handle_t *spk, esp_codec_dev_handle_t *mic)
{
    /* Only I2S0 does PDM. The mic's clock is 2.048 MHz, as M5Unified runs it. */
    i2s_chan_config_t mic_chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    ESP_RETURN_ON_ERROR(i2s_new_channel(&mic_chan, NULL, &s_mic_rx), TAG, "mic channel");
    i2s_pdm_rx_config_t pdm_cfg = {
        .clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(MUSE_AUDIO_RATE),
        .slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = { .clk = MIC_CLK, .din = MIC_DATA },
    };
    pdm_cfg.clk_cfg.dn_sample_mode = I2S_PDM_DSR_16S;
    ESP_RETURN_ON_ERROR(i2s_channel_init_pdm_rx_mode(s_mic_rx, &pdm_cfg), TAG, "mic pdm");

    /* Data only: no bit clock or word select. */
    i2s_chan_config_t spk_chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    spk_chan.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&spk_chan, &s_spk_tx, NULL), TAG, "buzzer channel");
    const i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(MUSE_AUDIO_RATE * SPK_UP),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_GPIO_UNUSED,
            .ws = I2S_GPIO_UNUSED,
            .dout = BUZZER,
            .din = I2S_GPIO_UNUSED,
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_spk_tx, &std_cfg), TAG, "buzzer i2s");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_spk_tx), TAG, "buzzer on");

    static const audio_codec_data_if_t spk_if = { .enable = spk_enable, .write = spk_write };
    static const audio_codec_data_if_t mic_if = { .enable = mic_enable, .read = mic_read };
    esp_codec_dev_cfg_t out_cfg = { .dev_type = ESP_CODEC_DEV_TYPE_OUT, .data_if = &spk_if };
    esp_codec_dev_cfg_t in_cfg = { .dev_type = ESP_CODEC_DEV_TYPE_IN, .data_if = &mic_if };
    *spk = esp_codec_dev_new(&out_cfg);
    *mic = esp_codec_dev_new(&in_cfg);
    return *spk && *mic ? ESP_OK : ESP_FAIL;
}

/*
 * The power button: held PWR_HOLD_MS it powers off, and while the screen is
 * asleep a press wakes it, reported as the aux button's (menu_button in
 * muse_input.c only wakes on it then).
 */
static unsigned poll_power_button(void)
{
    unsigned ev = muse_gpio_button_poll(&s_pwr);
    unsigned out = 0;
    if (ev & MUSE_BTN_TALK_PRESS) {
        s_pwr_down_at = xTaskGetTickCount();
        s_pwr_waking = muse_state_asleep();
        out = s_pwr_waking ? MUSE_BTN_AUX_PRESS : 0;
    } else if (ev & MUSE_BTN_TALK_RELEASE) {
        out = s_pwr_waking ? MUSE_BTN_AUX_RELEASE : 0;
        s_pwr_waking = false;
    } else if (s_pwr.pressed && s_pwr_down_at && xTaskGetTickCount() - s_pwr_down_at >= pdMS_TO_TICKS(PWR_HOLD_MS)) {
        s_pwr_down_at = 0;
        muse_input_request_power_off();
    }
    if (!s_pwr.pressed) {
        s_pwr_down_at = 0;
    }
    return out;
}

static unsigned poll_buttons(void)
{
    return muse_gpio_button_poll(&s_talk) | muse_gpio_button_poll(&s_aux) << 2 | poll_power_button();
}

/* Battery voltage through a 1:2 divider on GPIO38. No charger status or USB
 * sense reaches the ESP32. */
static esp_err_t read_power(muse_power_t *out)
{
    int sum = 0;
    for (int i = 0; i < 8; i++) {
        int raw;
        ESP_RETURN_ON_ERROR(adc_oneshot_read(s_adc, BATT_ADC, &raw), TAG, "adc read");
        sum += raw;
    }
    int mv;
    ESP_RETURN_ON_ERROR(adc_cali_raw_to_voltage(s_cali, sum / 8, &mv), TAG, "adc calibration");
    int v = mv * 2;
    out->usb = false;
    out->charging = false;
    out->battery_mv = v;
    if (v < 2500) {
        out->battery_pct = -1;
        return ESP_OK;
    }
    /* The Watcher's LiPo curve, as on the StickS3. */
    int pct = (-v * v + 9016 * v - 19189000) / 10000;
    out->battery_pct = pct < 0 ? 0 : pct > 100 ? 100 : pct;
    return ESP_OK;
}

/*
 * Screen off and the power hold released: on battery the board goes dark once
 * the power button is let go. On USB it stays powered, so it deep-sleeps
 * until the power or front button is pressed.
 */
static esp_err_t power_off(void)
{
    set_brightness(0);
    esp_lcd_panel_disp_on_off(s_panel, false);
    rtc_gpio_set_level(POWER_HOLD, 0);
    rtc_gpio_hold_dis(POWER_HOLD);
    while (gpio_get_level(PWR_GPIO) == 0 || gpio_get_level(TALK_GPIO) == 0 || gpio_get_level(AUX_GPIO) == 0) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    vTaskDelay(pdMS_TO_TICKS(50));
    ESP_RETURN_ON_ERROR(esp_sleep_enable_ext0_wakeup(PWR_GPIO, 0), TAG, "power button wake");
    /* ext1 on the ESP32 can't wake on any-low; one pin all-low is the same. */
    ESP_RETURN_ON_ERROR(esp_sleep_enable_ext1_wakeup_io(BIT64(TALK_GPIO), ESP_EXT1_WAKEUP_ALL_LOW), TAG,
                        "front button wake");
    esp_deep_sleep_start();
    return ESP_FAIL;
}

/* SAFE mode (muse_board_t.safe_display_off): the backlight off (active high: its PWM's duty 0). */
static void safe_display_off(void)
{
    muse_board_pin_hold(LCD_BL, 0);
}

/* The SAFE key (muse_board_t.safe_key: the way into and out of SAFE): button A (GPIO37, the talk key), read
 * raw (the board's own pull-up: GPIO37 has none). */
static bool safe_key(void)
{
    return muse_gpio_safe_key(TALK_GPIO, false);
}

static const muse_board_t s_board = {
    .name = "M5Stack StickC Plus2",
    .width = LCD_W,
    .height = LCD_H,
    .round = false,
    .touch = false,
    .diagonal_in = 1.14f,
    .talk_button = "front",
    .aux_button = "side",
    .talk_hint = { LV_ALIGN_BOTTOM_MID, 0, -4 },    /* the front button is under the screen */
    .aux_hint = { LV_ALIGN_RIGHT_MID, -2, -40 },    /* button B, on the right edge */
    .frame_ms = 40,
    .init = init,
    .display_start = display_start,
    .display_lock = display_lock,
    .display_unlock = esp_lv_adapter_unlock,
    .set_brightness = set_brightness,
    .safe_display_off = safe_display_off,
    .panel_sleep = panel_sleep,
    .audio_init = audio_init,
    .mic_slot = 0,              /* M5Unified's right slot: the ESP32 stores each pair swapped */
    .set_mic_gain = set_mic_gain,
    .poll_buttons = poll_buttons,
    .read_power = read_power,
    .power_off = power_off,
    .safe_key = safe_key,
};

/* Home Link's app_main starts Muse with this board (main/main.c). */
const muse_board_t *muse_board_get(void)
{
    return &s_board;
}
