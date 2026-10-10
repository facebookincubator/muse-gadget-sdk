/* SPDX-License-Identifier: Apache-2.0 */
/* KSDIY P4XC5, 4.3 inch 480x800 ST7102 MIPI + ST7123 touch.
 * Hardware reference: kevincoooool/ESP32P4_KSDIY,
 * P4_C5_4.3_Firmware/components/{ksdiy_p4c5_bsp,ksdiy_p4c5_audio} and
 * 50.application.music_performer/main/HAL/{HAL_Button,HAL_Audio}.c.
 * I2C0 GPIO7/8; backlight GPIO6; K1 GPIO35, K2/BOOT GPIO0.
 * ES8311 TX and ES7210 RX share clocks: TX 2x32bit = RX 4x16bit.
 * MIC1 is TDM slot0; slot2 is speaker reference, never mixed into voice.
 */
#include <stdint.h>
#include "muse_board.h"
#include "muse_audio.h"
#include "muse_mem.h"
#include "muse_pmu.h"
#include "esp_check.h"
#include "esp_lv_adapter.h"
#include "ksdiy_lvgl_port.h"
#include "ksdiy_p4c5_audio.h"
#include "audio_setup.h"

static const char *TAG = "ksdiy_p4xc5";
static muse_gpio_button_t s_talk, s_aux;

static esp_err_t init(void)
{
    /* Bare BSP setup powers PMIC rails and owns the shared I2C0 bus;
     * Muse starts exactly one LVGL task below, without the BSP demo screen. */
    ksdiy_panel_bare_init();
    ESP_RETURN_ON_ERROR(ksdiy_touch_bare_init(), TAG, "touch");
    ESP_RETURN_ON_ERROR(muse_pmu_init(touch_i2c_bus_, false), TAG, "PMU");
    ESP_RETURN_ON_ERROR(muse_gpio_button_init(&s_talk, GPIO_NUM_35), TAG, "K1");
    return muse_gpio_button_init(&s_aux, GPIO_NUM_0);
}

static lv_display_t *display_start(lv_indev_t **touch)
{
    *touch = NULL;
    esp_lv_adapter_config_t cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    cfg.task_core_id = MUSE_UI_CORE;
    cfg.task_priority = MUSE_UI_PRIORITY;
    cfg.task_stack_size = 32 * 1024;
    if (esp_lv_adapter_init(&cfg) != ESP_OK) return NULL;
    esp_lv_adapter_display_config_t dc = ESP_LV_ADAPTER_DISPLAY_MIPI_DEFAULT_CONFIG(
        ksdiy_lvgl_get_panel_handle(), NULL, 480, 800, ESP_LV_ADAPTER_ROTATE_0);
    lv_display_t *disp = esp_lv_adapter_register_display(&dc);
    if (!disp) return NULL;
    esp_lv_adapter_touch_config_t tc = ESP_LV_ADAPTER_TOUCH_DEFAULT_CONFIG(disp, ksdiy_touch_get_handle());
    *touch = esp_lv_adapter_register_touch(&tc);
    if (!*touch || esp_lv_adapter_start() != ESP_OK) return NULL;
    return disp;
}

static bool display_lock(int timeout_ms)
{
    return esp_lv_adapter_lock(timeout_ms) == ESP_OK;
}
static void brightness(int pct)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    ksdiy_lcd_set_brightness((uint8_t)((pct * 255 + 50) / 100));
}

static esp_err_t audio_init(esp_codec_dev_handle_t *spk, esp_codec_dev_handle_t *mic)
{
    Codec_I2S_init(touch_i2c_bus_, I2C_NUM_0);
    *spk = output_dev_;
    *mic = input_dev_;
    return *spk && *mic ? ESP_OK : ESP_FAIL;
}
static esp_err_t audio_open(esp_codec_dev_handle_t spk, esp_codec_dev_handle_t mic)
{
    (void)spk; (void)mic;
    esp_codec_dev_sample_info_t tx = {
        .sample_rate = MUSE_AUDIO_RATE, .bits_per_sample = 32, .channel = 2,
    };
    esp_codec_dev_sample_info_t rx = {
        .sample_rate = MUSE_AUDIO_RATE, .bits_per_sample = 16, .channel = 4,
        .channel_mask = ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0) | ESP_CODEC_DEV_MAKE_CHANNEL_MASK(2),
    };
    return audio_codec_set_fs(&tx, &rx);
}
static int audio_write(esp_codec_dev_handle_t spk, void *pcm, int bytes)
{
    if (!pcm || bytes <= 0 || bytes % 4) return ESP_ERR_INVALID_ARG;
    const int16_t *src = pcm;
    int32_t dma[256];
    int samples = bytes / sizeof(int16_t);
    while (samples > 0) {
        int n = samples > 256 ? 256 : samples;
        for (int i = 0; i < n; ++i) dma[i] = (int32_t)src[i] * 65536;
        int err = esp_codec_dev_write(spk, dma, n * sizeof(int32_t));
        if (err != ESP_CODEC_DEV_OK) return err;
        src += n; samples -= n;
    }
    return ESP_CODEC_DEV_OK;
}
static void mic_gain(esp_codec_dev_handle_t mic, int db)
{
    esp_codec_dev_set_in_channel_gain(mic, ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0), (float)db);
    esp_codec_dev_set_in_channel_gain(mic, ESP_CODEC_DEV_MAKE_CHANNEL_MASK(2), 0.0f);
}
static unsigned buttons(void)
{
    return muse_gpio_button_poll(&s_talk) | (muse_gpio_button_poll(&s_aux) << 2);
}
static void wait_buttons(int timeout_ms)
{
    muse_gpio_button_t *keys[] = { &s_talk, &s_aux };
    muse_gpio_buttons_wait(keys, 2, timeout_ms);
}
static esp_err_t power_off(void)
{
    brightness(0);
    return muse_pmu_power_off();
}
static const muse_board_t s_board = {
    .name = "KSDIY P4XC5", .width = 480, .height = 800,
    .touch = true, .round = false, .diagonal_in = 4.3f,
    .talk_button = "K1", .aux_button = "K2",
    .talk_hint = { LV_ALIGN_BOTTOM_LEFT, 12, -12 },
    .aux_hint = { LV_ALIGN_BOTTOM_RIGHT, -12, -12 },
    .frame_ms = 40, .init = init, .display_start = display_start,
    .display_lock = display_lock, .display_unlock = esp_lv_adapter_unlock,
    .set_brightness = brightness, .audio_init = audio_init,
    .audio_open = audio_open, .audio_write = audio_write,
    .mic_slot = 0, .set_mic_gain = mic_gain, .poll_buttons = buttons,
    .wait_buttons = wait_buttons, .read_power = muse_pmu_read_power, .power_off = power_off,
};
const muse_board_t *muse_board_get(void) { return &s_board; }
