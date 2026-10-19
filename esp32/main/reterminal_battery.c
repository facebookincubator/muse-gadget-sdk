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

#include "reterminal_battery.h"

#include "driver/gpio.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"

static const char *TAG = "link.battery";

// Seeed's E1001/E1002 battery example specifies GPIO1 (ADC1 channel 0),
// GPIO21 enable, 12 dB attenuation and a 2:1 divider:
// https://wiki.seeedstudio.com/reterminal_e10xx_with_esphome_advanced/
// Keep the divider enabled after init so it settles before health requests.
#define BATTERY_ADC_UNIT ADC_UNIT_1
#define BATTERY_ADC_CHANNEL ADC_CHANNEL_0
#define BATTERY_ENABLE_GPIO GPIO_NUM_21

static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cali;

// The board's voltage-to-charge curve, from Seeed's ESPHome cookbook for
// the reTerminal E series (calibrate_linear): millivolts -> percent.
static const struct {
    int mv;
    int pct;
} kCurve[] = {
    {4150, 100}, {3960, 90}, {3910, 80}, {3850, 70}, {3800, 60},
    {3750, 50},  {3680, 40}, {3580, 30}, {3490, 20}, {3410, 10},
    {3300, 5},   {3270, 0},
};

static int pct_for_mv(int mv)
{
    if (mv >= kCurve[0].mv) return 100;
    for (size_t i = 1; i < sizeof(kCurve) / sizeof(kCurve[0]); i++) {
        if (mv >= kCurve[i].mv) {
            int mv_hi = kCurve[i - 1].mv, mv_lo = kCurve[i].mv;
            int pct_hi = kCurve[i - 1].pct, pct_lo = kCurve[i].pct;
            return pct_lo + (pct_hi - pct_lo) * (mv - mv_lo) / (mv_hi - mv_lo);
        }
    }
    return 0;
}

void reterminal_battery_init(void)
{
    if (s_adc) return;
    gpio_config_t enable = {
        .pin_bit_mask = 1ULL << BATTERY_ENABLE_GPIO,
        .mode = GPIO_MODE_OUTPUT,
    };
    if (gpio_config(&enable) != ESP_OK) {
        ESP_LOGW(TAG, "battery enable GPIO failed");
        return;
    }
    if (gpio_set_level(BATTERY_ENABLE_GPIO, 1) != ESP_OK) {
        ESP_LOGW(TAG, "battery divider enable failed");
        return;
    }

    const adc_oneshot_unit_init_cfg_t unit_cfg = {.unit_id = BATTERY_ADC_UNIT};
    if (adc_oneshot_new_unit(&unit_cfg, &s_adc) != ESP_OK) {
        ESP_LOGW(TAG, "adc unit failed");
        s_adc = NULL;
        gpio_set_level(BATTERY_ENABLE_GPIO, 0);
        return;
    }
    const adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    if (adc_oneshot_config_channel(s_adc, BATTERY_ADC_CHANNEL, &chan_cfg) !=
        ESP_OK) {
        ESP_LOGW(TAG, "adc channel failed");
        adc_oneshot_del_unit(s_adc);
        s_adc = NULL;
        gpio_set_level(BATTERY_ENABLE_GPIO, 0);
        return;
    }
    // A fixed raw-to-voltage scale cannot account for ADC reference variation
    // and nonlinearity. Leave health fields null when calibration is unavailable.
    adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id = BATTERY_ADC_UNIT,
        .chan = BATTERY_ADC_CHANNEL,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    if (adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_cali) != ESP_OK) {
        ESP_LOGW(TAG, "adc calibration unavailable; battery status disabled");
        s_cali = NULL;
        adc_oneshot_del_unit(s_adc);
        s_adc = NULL;
        gpio_set_level(BATTERY_ENABLE_GPIO, 0);
    }
}

bool reterminal_battery_read(int *battery_mv, int *battery_pct)
{
    if (!s_adc || !s_cali || !battery_mv || !battery_pct) return false;
    int sum = 0;
    for (int i = 0; i < 8; i++) {
        int raw;
        if (adc_oneshot_read(s_adc, BATTERY_ADC_CHANNEL, &raw) != ESP_OK) {
            return false;
        }
        sum += raw;
    }
    int raw = sum / 8;
    int pin_mv;
    if (adc_cali_raw_to_voltage(s_cali, raw, &pin_mv) != ESP_OK) {
        return false;
    }
    int mv = pin_mv * 2;  // the board's 2:1 divider
    // Reject readings far below an empty cell. This is not a battery-presence
    // detector: the charger can affect the measured voltage while on USB.
    if (mv < 2500) return false;
    *battery_mv = mv;
    *battery_pct = pct_for_mv(mv);
    return true;
}
