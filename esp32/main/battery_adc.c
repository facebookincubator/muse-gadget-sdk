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
#include "sdkconfig.h"
#include "reterminal_e100x_board.h"
#define BATTERY_ENABLE_GPIO E100X_BATTERY_ENABLE_GPIO
#define battery_percent reterminal_e100x_battery_percent

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Sampling policy follows Seeed's common/battery_gauge.cpp.
#define BATTERY_SETTLE_MS 5
#define BATTERY_SAMPLES 10
#define BATTERY_SAMPLE_INTERVAL_MS 25
#define MIN_VALID_ADC_MV 500
#define MAX_ADC_RANGE_MV 400
#define BATTERY_ADC_GPIO E100X_BATTERY_ADC_GPIO

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

    gpio_config_t io_cfg = {
        .pin_bit_mask = (1ULL << BATTERY_ENABLE_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&io_cfg) != ESP_OK) {
        ESP_LOGW(TAG, "enable GPIO config failed");
        return false;
    }
    // E1003's VBAT_EN is held high in Seeed's early board initialization.
#if E100X_BATTERY_KEEP_ENABLED
    gpio_set_level(BATTERY_ENABLE_GPIO, 1);
#else
    gpio_set_level(BATTERY_ENABLE_GPIO, 0);
#endif

    adc_unit_t unit_id;
    if (adc_oneshot_io_to_channel(BATTERY_ADC_GPIO,
                                  &unit_id, &s_channel) != ESP_OK ||
        unit_id != ADC_UNIT_1) {
        ESP_LOGW(TAG, "GPIO%d is not on ADC unit 1",
                 BATTERY_ADC_GPIO);
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
        adc_oneshot_del_unit(s_adc);
        s_adc = NULL;
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
    ESP_LOGI(TAG, "battery ADC ready on GPIO%d", BATTERY_ADC_GPIO);
    s_inited = true;
    return true;
}

bool battery_adc_read(int *mv_out, int *pct_out) {
    if (!mv_out || !pct_out || !battery_adc_init()) {
        return false;
    }
    if (gpio_set_level(BATTERY_ENABLE_GPIO, 1) != ESP_OK) return false;
    vTaskDelay(pdMS_TO_TICKS(BATTERY_SETTLE_MS));
    int sum_mv = 0, min_mv = INT_MAX, max_mv = 0;
    bool ok = true;
    for (int i = 0; i < BATTERY_SAMPLES; i++) {
        int raw = 0, mv = 0;
        if (adc_oneshot_read(s_adc, s_channel, &raw) != ESP_OK) {
            ok = false;
            break;
        }
        if (s_cali_ok) {
            if (adc_cali_raw_to_voltage(s_cali, raw, &mv) != ESP_OK) {
                ok = false;
                break;
            }
        } else {
            // Approximate only when hardware calibration is unavailable.
            mv = raw * 3300 / 4095;
        }
        sum_mv += mv;
        if (mv < min_mv) min_mv = mv;
        if (mv > max_mv) max_mv = mv;
        if (i + 1 < BATTERY_SAMPLES) vTaskDelay(pdMS_TO_TICKS(BATTERY_SAMPLE_INTERVAL_MS));
    }
#if !E100X_BATTERY_KEEP_ENABLED
    if (gpio_set_level(BATTERY_ENABLE_GPIO, 0) != ESP_OK) ok = false;
#endif
    if (!ok) return false;
    int average_mv = sum_mv / BATTERY_SAMPLES;
    if (average_mv < MIN_VALID_ADC_MV || max_mv - min_mv > MAX_ADC_RANGE_MV) return false;
    *mv_out = average_mv * 2; // R19/R21 divide battery voltage by two.
    *pct_out = battery_percent(*mv_out);
    return true;
}
