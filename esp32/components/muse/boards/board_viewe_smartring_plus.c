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
 * VIEWESMART SmartRing-Plus: ESP32-S3-N16R8, 360x360 round ST77916 QSPI LCD
 * with CST816 touch, an ES8311 codec (speaker through an NS4150B amp, one
 * analog mic) and a V1 (GPIO1 ADC) or V2 (AXP2101) battery gauge. BOOT
 * (GPIO0) is the only user button, and it is inside the case, so the mic icon
 * is an on-screen talk button (touch_talk): hold it to talk and let go to
 * send. A tap on the screen confirms a Muse app pairing. BOOT still talks if
 * reachable.
 *
 * Display, touch, the shared I2C bus and the battery come from the vendor BSP
 * (viewesmart/smartring_plus, vendored in
 * components_vendor/viewesmart__smartring_plus and added to the build only for
 * this board); pins are from its board_config.h.
 *
 * Audio skips bsp_audio_init(): it opens the codec itself and leaves the
 * speaker muted, while Muse wants unopened speaker and mic handles on one
 * 16 kHz duplex bus. The ES8311 is set up here as on the other single-ES8311
 * boards, MCLK = 256 x Fs (the rate Muse's esp_codec_dev_open() asks of I2S).
 *
 * Power: the hardware switch only, so there is no power_off and Settings has
 * no Power off.
 *
 * No display_pause: pausing LVGL on battery would stop the touch input that
 * is this board's only reachable way to wake the screen, since BOOT is inside
 * the case. LVGL keeps running while the screen is dark, so a tap wakes it.
 */
#include "bsp/smartring_plus.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_codec_dev_defaults.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"

#include "muse_audio.h"
#include "muse_board.h"
#include "muse_mem.h"

static const char *TAG = "board";

#define TALK_GPIO GPIO_NUM_0    /* BOOT */

static muse_gpio_button_t s_talk;
static bool s_battery;

static esp_err_t init(void)
{
    /* The shared I2C bus comes up with the display, in display_start(). */
    return muse_gpio_button_init(&s_talk, TALK_GPIO);
}

static lv_display_t *display_start(lv_indev_t **touch)
{
    /* This runs on Muse's boot task, pinned to MUSE_UI_CORE, so the panel's
     * SPI interrupt lands there; keep LVGL's task on the same core. */
    if (bsp_display_init_with_task(MUSE_UI_CORE, MUSE_UI_PRIORITY) != ESP_OK) {
        return NULL;
    }
    lv_display_t *disp = bsp_display_get_handle();
    if (!disp) {
        return NULL;
    }
    /* The BSP registers the CST816 as LVGL's only input device. */
    if (esp_lv_adapter_lock(-1) == ESP_OK) {
        *touch = lv_indev_get_next(NULL);
        esp_lv_adapter_unlock();
    }

    /* V2's AXP2101 sits on the I2C bus the display just created. */
    esp_err_t err = bsp_battery_init();
    s_battery = err == ESP_OK;
    if (!s_battery) {
        ESP_LOGW(TAG, "battery gauge unavailable (%s)", esp_err_to_name(err));
    }
    return disp;
}

static bool display_lock(int timeout_ms)
{
    return esp_lv_adapter_lock(timeout_ms) == ESP_OK;
}

static void set_brightness(int pct)
{
    bsp_display_backlight_set((uint8_t)(pct < 0 ? 0 : pct > 100 ? 100 : pct));
}

/* One ES8311 does both directions over a duplex I2S bus, clocked from MCLK. */
static esp_err_t audio_init(esp_codec_dev_handle_t *spk, esp_codec_dev_handle_t *mic)
{
    i2c_master_bus_handle_t i2c = bsp_i2c_get_bus();
    ESP_RETURN_ON_FALSE(i2c, ESP_ERR_INVALID_STATE, TAG, "i2c bus not up");

    i2s_chan_handle_t tx, rx;
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(BOARD_I2S_PORT, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &tx, &rx), TAG, "i2s channel");
    const i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(MUSE_AUDIO_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = BOARD_I2S_MCLK_IO,
            .bclk = BOARD_I2S_BCLK_IO,
            .ws = BOARD_I2S_WS_IO,
            .dout = BOARD_I2S_DOUT_IO,
            .din = BOARD_I2S_DIN_IO,
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(tx, &std_cfg), TAG, "i2s tx");
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(rx, &std_cfg), TAG, "i2s rx");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(tx), TAG, "i2s tx on");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(rx), TAG, "i2s rx on");

    audio_codec_i2s_cfg_t i2s_cfg = { .port = BOARD_I2S_PORT, .rx_handle = rx, .tx_handle = tx };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);
    audio_codec_i2c_cfg_t i2c_cfg = { .port = BOARD_I2C_PORT, .addr = ES8311_CODEC_DEFAULT_ADDR, .bus_handle = i2c };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    ESP_RETURN_ON_FALSE(data_if && ctrl_if && gpio_if, ESP_ERR_NO_MEM, TAG, "codec interfaces");

    es8311_codec_cfg_t es_cfg = {
        .ctrl_if = ctrl_if,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH,
        .pa_pin = BOARD_PA_CTRL_IO,     /* NS4150B enable, active high */
        .use_mclk = true,
        .hw_gain = { .pa_voltage = 3.3, .codec_dac_voltage = 3.3 },   /* as the BSP */
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
    return muse_gpio_button_poll(&s_talk);
}

/* The BSP samples the gauge in its own task; V1 reads the VCC_OUT rail, V2 the AXP2101. */
static esp_err_t read_power(muse_power_t *out)
{
    if (!s_battery) {
        return ESP_ERR_INVALID_STATE;
    }
    bsp_battery_data_t bat;
    bsp_battery_get_data(&bat);
    if (bat.voltage_v <= 0.0f) {
        return ESP_ERR_INVALID_STATE;   /* no sample yet */
    }
    out->battery_mv = (int)(bat.voltage_v * 1000.0f + 0.5f);
    out->battery_pct = bat.percent;
    out->charging = bat.charging;
    out->usb = bat.charging;            /* the BSP folds "on external power" into charging */
    return ESP_OK;
}

static const muse_board_t s_board = {
    .name = "VIEWE SmartRing-Plus",
    .width = BOARD_LCD_H_RES,
    .height = BOARD_LCD_V_RES,
    .round = true,
    .touch = true,
    .diagonal_in = 1.39f,
    .touch_talk = true,         /* BOOT is inside the case */
    .talk_button = "boot",
    /* The talk button's centre: under the caption, above the page dots. */
    .talk_hint = { LV_ALIGN_CENTER, 0, 132 },
    .frame_ms = 40,
    .init = init,
    .display_start = display_start,
    .display_lock = display_lock,
    .display_unlock = esp_lv_adapter_unlock,
    .set_brightness = set_brightness,
    .audio_init = audio_init,
    .mic_slot = 0,              /* one mic, on the left slot */
    .poll_buttons = poll_buttons,
    .read_power = read_power,
    /* No .power_off: the hardware switch controls power, so Settings leaves
     * Power off out. */
};

/* Home Link's app_main starts Muse with this board (main/main.c). */
const muse_board_t *muse_board_get(void)
{
    return &s_board;
}
