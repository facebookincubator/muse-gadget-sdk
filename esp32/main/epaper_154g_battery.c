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

#include "epaper_154g_battery.h"
#include "led_status.h"

#include <stdio.h>

#include "driver/usb_serial_jtag.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

// GPIO4 sees the cell through R21 200k over R38 200k: half of VBAT, at most
// about 2.1 V, within the 12 dB range.
#define BATT_CHANNEL  ADC_CHANNEL_3
#define BATT_ATTEN    ADC_ATTEN_DB_12
#define BATT_DIVIDER  2
#define BATT_SAMPLES  16
#define BATT_LOG_MS   60000

static const char *TAG = "link.battery";
static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cali;
static SemaphoreHandle_t s_lock;
static int s_last_percent = -1; // guarded by s_lock

// ---- Battery maths (host-tested) ----
// The curve the other single-cell boards use: 0 below about 3.5 V, 100 at
// about 4.2 V. Under load the cell reads low, so this is an estimate.
static int battery_percent(int mv) {
    int pct = (-mv * mv + 9016 * mv - 19189000) / 10000;
    return pct < 0 ? 0 : pct > 100 ? 100 : pct;
}

// BATT_SAMPLES calibrated readings averaged, then the divider undone.
static esp_err_t battery_measure(int *mv) {
    int sum = 0;
    for (int i = 0; i < BATT_SAMPLES; i++) {
        int raw, sample;
        esp_err_t err = adc_oneshot_read(s_adc, BATT_CHANNEL, &raw);
        if (err == ESP_OK) err = adc_cali_raw_to_voltage(s_cali, raw, &sample);
        if (err != ESP_OK) return err;
        sum += sample;
    }
    *mv = sum / BATT_SAMPLES * BATT_DIVIDER;
    return ESP_OK;
}

static void battery_line(char *buf, size_t cap, const epaper_154g_battery_t *b) {
    snprintf(buf, cap, "battery %d mV, %d%%, USB host %s", b->millivolts, b->percent,
             b->usb_host ? "connected" : "not seen");
}
// ---- End battery maths ----

esp_err_t epaper_154g_battery_read(epaper_154g_battery_t *out) {
    if (!s_adc || !s_cali || !s_lock) return ESP_ERR_INVALID_STATE;
    int mv = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = battery_measure(&mv);
    if (err == ESP_OK) s_last_percent = battery_percent(mv);
    xSemaphoreGive(s_lock);
    if (err != ESP_OK) return err;
    out->millivolts = mv;
    out->percent = battery_percent(mv);
    out->usb_host = usb_serial_jtag_is_connected();
    return ESP_OK;
}

int epaper_154g_battery_cached_percent(void) {
    if (!s_lock) return -1;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int percent = s_last_percent;
    xSemaphoreGive(s_lock);
    return percent;
}

// One line a minute, so a charge (or its absence) shows in the console.
static void battery_task(void *arg) {
    (void)arg;
    for (;;) {
        epaper_154g_battery_t b;
        char line[64];
        esp_err_t err = epaper_154g_battery_read(&b);
        epaper_154g_status_set_battery(err == ESP_OK ? b.percent : -1);
        if (err == ESP_OK) {
            battery_line(line, sizeof(line), &b);
            ESP_LOGI(TAG, "%s", line);
        } else {
            ESP_LOGW(TAG, "battery read failed: %s", esp_err_to_name(err));
        }
        vTaskDelay(pdMS_TO_TICKS(BATT_LOG_MS));
    }
}

void epaper_154g_battery_init(void) {
    const adc_oneshot_unit_init_cfg_t unit = { .unit_id = ADC_UNIT_1 };
    esp_err_t err = adc_oneshot_new_unit(&unit, &s_adc);
    const adc_oneshot_chan_cfg_t channel = { .atten = BATT_ATTEN, .bitwidth = ADC_BITWIDTH_DEFAULT };
    if (err == ESP_OK) err = adc_oneshot_config_channel(s_adc, BATT_CHANNEL, &channel);
    const adc_cali_curve_fitting_config_t cali = {
        .unit_id = ADC_UNIT_1,
        .chan = BATT_CHANNEL,
        .atten = BATT_ATTEN,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (err == ESP_OK) err = adc_cali_create_scheme_curve_fitting(&cali, &s_cali);
    if (err == ESP_OK && !(s_lock = xSemaphoreCreateMutex())) err = ESP_ERR_NO_MEM;
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "battery reading disabled: %s", esp_err_to_name(err));
        return;
    }
    if (xTaskCreate(battery_task, "battery", 3072, NULL, 1, NULL) != pdPASS) {
        ESP_LOGW(TAG, "no memory for the battery log");
    }
}
