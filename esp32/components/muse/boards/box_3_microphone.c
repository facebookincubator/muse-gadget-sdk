/* Copyright (c) Meta Platforms, Inc. and affiliates.
 * SPDX-License-Identifier: Apache-2.0
 */

/* BOX-3 mute disconnects both ES7210 power and its I2S data output.
 * Recreate the ADC configuration after every unmute, including muted boots.
 * Source: espressif/esp-box hardware/SCH_ESP32-S3-BOX-3_V1.0/
 * SCH_ESP32-S3-BOX-3-MB_V1.1_20230808.pdf, sheets 4 and 7.
 * GPIO1 is status only: never drive it or bypass the hardware mute circuit.
 */
#include "box_3_microphone.h"

#include <stdatomic.h>
#include <string.h>
#include "bsp/esp-bsp.h"
#include "esp_codec_dev_defaults.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "box3_mic";
static const audio_codec_data_if_t *s_bus;
static const audio_codec_ctrl_if_t *s_ctrl;
static const audio_codec_if_t *s_adc;
static SemaphoreHandle_t s_lock;
static esp_codec_dev_sample_info_t s_fs;
static atomic_bool s_reset = true;
static bool s_enabled, s_ready;
static float s_gain;
static int s_discard_bytes;
static TickType_t s_retry_at;

static bool muted(void)
{
    return gpio_get_level(BSP_BUTTON_MUTE_IO) == 0;
}

void box_3_microphone_poll(void)
{
    /* Input task keeps observing power loss even during long speaker replies,
     * when the voice task isn't reading the microphone. */
    if (muted()) {
        atomic_store(&s_reset, true);
    }
}

esp_err_t box_3_microphone_init(void)
{
    const gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << BSP_BUTTON_MUTE_IO,
        .mode = GPIO_MODE_INPUT,
    };
    return gpio_config(&cfg);
}

/* Called with s_lock held. Codec I2C accesses only happen with mute off. */
static bool prepare(void)
{
    if (muted()) {
        atomic_store(&s_reset, true);
        s_ready = false;
        s_retry_at = xTaskGetTickCount() + pdMS_TO_TICKS(100);
        return false;
    }
    if (!s_enabled) {
        return false;
    }
    if (atomic_load(&s_reset)) {
        s_ready = false;
    }
    if (s_ready || (int32_t)(xTaskGetTickCount() - s_retry_at) < 0) {
        return s_ready;
    }
    atomic_store(&s_reset, false);
    if (s_adc) {
        audio_codec_delete_codec_if(s_adc);
    }
    es7210_codec_cfg_t cfg = { .ctrl_if = s_ctrl };
    s_adc = es7210_codec_new(&cfg);
    s_ready = s_adc && s_adc->set_fs(s_adc, &s_fs) == ESP_CODEC_DEV_OK
              && s_adc->enable(s_adc, true) == ESP_CODEC_DEV_OK
              && s_adc->set_mic_gain(s_adc, s_gain) == ESP_CODEC_DEV_OK
              && s_adc->mute_mic(s_adc, false) == ESP_CODEC_DEV_OK;
    /* Drain old DMA samples and ADC startup transients at the normal pace. */
    s_discard_bytes = s_fs.sample_rate / 5 * s_fs.channel * s_fs.bits_per_sample / 8;
    s_retry_at = xTaskGetTickCount() + pdMS_TO_TICKS(500);
    ESP_LOGI(TAG, "microphone %s", s_ready ? "ready after power restore" : "initialization failed; retrying");
    return s_ready;
}

static bool data_is_open(const audio_codec_data_if_t *h)
{
    (void)h;
    return s_bus->is_open(s_bus);
}

static int data_enable(const audio_codec_data_if_t *h, esp_codec_dev_type_t type, bool enable)
{
    (void)h;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_enabled = enable;
    if (!enable && s_adc && s_ready && !muted() && !atomic_load(&s_reset)) {
        s_adc->enable(s_adc, false);
    }
    s_ready = false;
    atomic_store(&s_reset, true);
    int ret = s_bus->enable(s_bus, type, enable);
    xSemaphoreGive(s_lock);
    return ret;
}

static int data_set_fmt(const audio_codec_data_if_t *h, esp_codec_dev_type_t type,
                        esp_codec_dev_sample_info_t *fs)
{
    (void)h;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_fs = *fs;
    atomic_store(&s_reset, true);
    int ret = s_bus->set_fmt(s_bus, type, fs);
    xSemaphoreGive(s_lock);
    return ret;
}

static int data_read(const audio_codec_data_if_t *h, uint8_t *data, int size)
{
    (void)h;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool ready = prepare();
    bool discard = s_discard_bytes > 0;
    if (discard) {
        s_discard_bytes -= size;
    }
    xSemaphoreGive(s_lock);
    /* Always drain I2S so muted recording retains its duration. */
    int ret = s_bus->read(s_bus, data, size);
    if (!ready || discard || muted() || atomic_load(&s_reset)) {
        memset(data, 0, size);
    }
    return ret;
}

void box_3_microphone_set_gain(int db)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    db = (db / 3) * 3;
    s_gain = db == 33 ? 34.5f : (float)db;
    if (s_adc && s_ready && !muted() && !atomic_load(&s_reset)) {
        s_adc->set_mic_gain(s_adc, s_gain);
    }
    xSemaphoreGive(s_lock);
}

esp_codec_dev_handle_t box_3_microphone_create(void)
{
    s_bus = bsp_audio_get_codec_itf();
    s_lock = xSemaphoreCreateMutex();
    if (!s_bus || !s_lock) {
        return NULL;
    }
    audio_codec_i2c_cfg_t i2c = {
        .port = BSP_I2C_NUM,
        .addr = ES7210_CODEC_DEFAULT_ADDR,
        .bus_handle = bsp_i2c_get_handle(),
    };
    s_ctrl = audio_codec_new_i2c_ctrl(&i2c);
    if (!s_ctrl) {
        return NULL;
    }
    static const audio_codec_data_if_t data = {
        .is_open = data_is_open,
        .enable = data_enable,
        .set_fmt = data_set_fmt,
        .read = data_read,
    };
    /* The adapter owns the ADC's power-dependent lifetime. The public handle
     * and shared I2S bus stay valid while the physical ADC is switched off. */
    esp_codec_dev_cfg_t cfg = { .dev_type = ESP_CODEC_DEV_TYPE_IN, .data_if = &data };
    return esp_codec_dev_new(&cfg);
}
