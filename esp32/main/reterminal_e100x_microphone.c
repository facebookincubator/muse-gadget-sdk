// SPDX-License-Identifier: Apache-2.0

#include "reterminal_e100x_microphone.h"
#include "reterminal_e100x.h"
#include "link_power.h"

#include "reterminal_e100x_board.h"
#include <stdlib.h>

#include "driver/gpio.h"
#include "driver/i2s_pdm.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"

// E1001/E1002 V1.2 and E1003 V1.0 board profiles: PDM mic on 38/42/41.
#define MIC_EN E100X_MIC_ENABLE_GPIO
#define MIC_CLK E100X_MIC_CLOCK_GPIO
#define MIC_DATA E100X_MIC_DATA_GPIO
#define SAMPLE_RATE 16000
#define MAX_DURATION_MS 500

bool reterminal_e100x_microphone_init(void) {
    const gpio_config_t power = {
        .pin_bit_mask = 1ULL << MIC_EN,
        .mode = GPIO_MODE_OUTPUT,
    };
    return gpio_config(&power) == ESP_OK && gpio_set_level(MIC_EN, 0) == ESP_OK;
}

cJSON *reterminal_e100x_microphone_record(const cJSON *params) {
    const cJSON *duration = cJSON_GetObjectItemCaseSensitive(params, "duration_ms");
    if (!cJSON_IsNumber(duration) || duration->valuedouble != duration->valueint
        || duration->valueint < 100 || duration->valueint > MAX_DURATION_MS) {
        return reterminal_e100x_error("capture_failed", "duration_ms must be an integer from 100 to 500");
    }
    if (!link_power_acquire(LINK_POWER_MICROPHONE))
        return reterminal_e100x_error("busy", "microphone operation unavailable");
    if (!link_power_pm_enter(LINK_PM_MICROPHONE)) {
        link_power_release(LINK_POWER_MICROPHONE);
        return reterminal_e100x_error("capture_failed", "microphone power lock unavailable");
    }
    size_t bytes = (size_t)SAMPLE_RATE * duration->valueint / 1000 * sizeof(int16_t);
    size_t encoded_size = 4 * ((bytes + 2) / 3) + 1;
    uint8_t *pcm = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    unsigned char *encoded = heap_caps_malloc(encoded_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    i2s_chan_handle_t channel = NULL;
    bool enabled = false;
    bool power_ready = false;
    cJSON *result = NULL;
    power_ready = reterminal_e100x_microphone_init();
    if (!pcm || !encoded || !power_ready) {
        result = reterminal_e100x_error("capture_failed", "microphone memory or power GPIO unavailable");
        goto done;
    }
    i2s_chan_config_t config = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    config.dma_desc_num = 4;
    config.dma_frame_num = 256;
    if (i2s_new_channel(&config, NULL, &channel) != ESP_OK) {
        result = reterminal_e100x_error("capture_failed", "I2S0 is unavailable");
        goto done;
    }
    const i2s_pdm_rx_config_t pdm = {
        .clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {.clk = MIC_CLK, .din = MIC_DATA},
    };
    if (i2s_channel_init_pdm_rx_mode(channel, &pdm) != ESP_OK) {
        result = reterminal_e100x_error("capture_failed", "PDM initialization failed");
        goto done;
    }
    gpio_set_level(MIC_EN, 1);
    vTaskDelay(pdMS_TO_TICKS(20));
    if (i2s_channel_enable(channel) != ESP_OK) {
        result = reterminal_e100x_error("capture_failed", "PDM start failed");
        goto done;
    }
    enabled = true;
    // Let the PDM filter settle. Discard its startup samples.
    int16_t discard[256];
    size_t received = 0;
    if (i2s_channel_read(channel, discard, sizeof(discard), &received, 500) != ESP_OK
        || received != sizeof(discard)) {
        result = reterminal_e100x_error("capture_failed", "PDM startup read failed");
        goto done;
    }
    received = 0;
    if (i2s_channel_read(channel, pcm, bytes, &received, 1500) != ESP_OK || received != bytes) {
        result = reterminal_e100x_error("capture_failed", "PDM capture incomplete");
        goto done;
    }
    size_t encoded_len = 0;
    if (mbedtls_base64_encode(encoded, encoded_size, &encoded_len, pcm, bytes) != 0) {
        result = reterminal_e100x_error("capture_failed", "PCM encoding failed");
        goto done;
    }
    encoded[encoded_len] = 0;
    result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "ok", true);
    cJSON *payload = cJSON_AddObjectToObject(result, "payload");
    cJSON_AddStringToObject(payload, "format", "pcm_s16le");
    cJSON_AddNumberToObject(payload, "sample_rate_hz", SAMPLE_RATE);
    cJSON_AddNumberToObject(payload, "channels", 1);
    cJSON_AddNumberToObject(payload, "duration_ms", duration->valueint);
    cJSON_AddNumberToObject(payload, "bytes", bytes);
    if (!cJSON_AddStringToObject(payload, "audio_base64", (const char *)encoded)) {
        cJSON_Delete(result);
        result = reterminal_e100x_error("capture_failed", "cannot allocate audio response");
    }
done:
    if (enabled) i2s_channel_disable(channel);
    if (channel) i2s_del_channel(channel);
    if (power_ready) gpio_set_level(MIC_EN, 0);
    free(pcm);
    free(encoded);
    link_power_pm_leave(LINK_PM_MICROPHONE);
    link_power_release(LINK_POWER_MICROPHONE);
    return result;
}
