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

#include "battery_adc.h"

#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Single-cell Li-ion charge curve endpoints, in millivolts.
#define BATTERY_MV_EMPTY 3000
#define BATTERY_MV_FULL 4200
// Time for the divider enable MOSFET and RC to settle before sampling.
#define BATTERY_SETTLE_MS 10

static const char *TAG = "battery_adc";

static adc_oneshot_unit_handle_t s_adc = NULL;
static adc_cali_handle_t s_cali = NULL;
static bool s_cali_ok = false;
static adc_channel_t s_channel = ADC_CHANNEL_0;
static bool s_inited = false;

static bool battery_adc_init(void) {
    if (s_inited) {
        return s_adc != NULL;
    }
    s_inited = true;

    gpio_config_t io_cfg = {
        .pin_bit_mask = (1ULL << CONFIG_HOMEHUB_BATTERY_ENABLE_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&io_cfg) != ESP_OK) {
        ESP_LOGW(TAG, "enable GPIO config failed");
        return false;
    }
    gpio_set_level(CONFIG_HOMEHUB_BATTERY_ENABLE_GPIO, 0);

    adc_unit_t unit_id;
    if (adc_oneshot_io_to_channel(CONFIG_HOMEHUB_BATTERY_ADC_GPIO,
                                  &unit_id, &s_channel) != ESP_OK ||
        unit_id != ADC_UNIT_1) {
        ESP_LOGW(TAG, "GPIO%d is not on ADC unit 1",
                 CONFIG_HOMEHUB_BATTERY_ADC_GPIO);
        return false;
    }
    adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id = ADC_UNIT_1,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    if (adc_oneshot_new_unit(&unit_cfg, &s_adc) != ESP_OK) {
        ESP_LOGW(TAG, "ADC unit init failed");
        s_adc = NULL;
        return false;
    }
    adc_oneshot_chan_cfg_t chan_cfg = {
        .bitwidth = ADC_BITWIDTH_DEFAULT,
        .atten = ADC_ATTEN_DB_12,
    };
    if (adc_oneshot_config_channel(s_adc, s_channel, &chan_cfg) != ESP_OK) {
        ESP_LOGW(TAG, "ADC channel config failed");
        return false;
    }
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id = ADC_UNIT_1,
        .chan = s_channel,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_cali) == ESP_OK) {
        s_cali_ok = true;
    } else {
        ESP_LOGW(TAG, "ADC calibration unavailable, using raw estimate");
    }
#endif
    ESP_LOGI(TAG, "battery ADC ready on GPIO%d", CONFIG_HOMEHUB_BATTERY_ADC_GPIO);
    return true;
}

bool battery_adc_read(int *mv_out, int *pct_out) {
    if (!battery_adc_init()) {
        return false;
    }
    gpio_set_level(CONFIG_HOMEHUB_BATTERY_ENABLE_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(BATTERY_SETTLE_MS));
    int raw = 0;
    esp_err_t r = adc_oneshot_read(s_adc, s_channel, &raw);
    gpio_set_level(CONFIG_HOMEHUB_BATTERY_ENABLE_GPIO, 0);
    if (r != ESP_OK) {
        return false;
    }
    int adc_mv = 0;
    if (s_cali_ok) {
        if (adc_cali_raw_to_voltage(s_cali, raw, &adc_mv) != ESP_OK) {
            return false;
        }
    } else {
        // Uncalibrated fallback: 12-bit scale over the ~3.3 V 11 dB attenuation range.
        adc_mv = raw * 3300 / 4095;
    }
    // The divider halves the battery voltage.
    int batt_mv = adc_mv * 2;
    int pct = (batt_mv - BATTERY_MV_EMPTY) * 100 /
              (BATTERY_MV_FULL - BATTERY_MV_EMPTY);
    if (pct < 0) {
        pct = 0;
    }
    if (pct > 100) {
        pct = 100;
    }
    *mv_out = batt_mv;
    *pct_out = pct;
    return true;
}
