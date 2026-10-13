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

#include "epaper_154g_sensors.h"

#include <math.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

// SHTC3 protocol: https://sensirion.com/file/datasheet_shtc3, sections 5.2-5.6.
// Waveshare pins/address: https://docs.waveshare.com/ESP32-S3-ePaper-1.54G.
static const char *TAG = "link.sensors";
static i2c_master_dev_handle_t s_sensor;
static SemaphoreHandle_t s_lock;

static uint8_t crc8(const uint8_t *bytes) {
    uint8_t crc = 0xff;
    for (int i = 0; i < 2; i++) {
        crc ^= bytes[i];
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc & 0x80) ? (crc << 1) ^ 0x31 : crc << 1;
        }
    }
    return crc;
}

static esp_err_t sensor_write(uint16_t command) {
    const uint8_t bytes[] = {command >> 8, command & 0xff};
    return i2c_master_transmit(s_sensor, bytes, sizeof(bytes), 100);
}

static esp_err_t sensor_read(double *temperature, double *humidity) {
    esp_err_t err = sensor_write(0x3517);
    uint8_t bytes[6];
    if (err == ESP_OK) {
        // Wake takes up to 240 us. Round delays up even with a coarse RTOS tick.
        vTaskDelay(pdMS_TO_TICKS(1) + 1);
        err = sensor_write(0x7866);
    }
    if (err == ESP_OK) {
        // Normal-mode measurement takes up to 12.1 ms, without clock stretching.
        vTaskDelay(pdMS_TO_TICKS(20) + 1);
        err = i2c_master_receive(s_sensor, bytes, sizeof(bytes), 100);
    }
    esp_err_t sleep = sensor_write(0xb098);
    if (err != ESP_OK) return err;
    if (sleep != ESP_OK) return sleep;
    if (crc8(bytes) != bytes[2] || crc8(bytes + 3) != bytes[5]) {
        return ESP_ERR_INVALID_CRC;
    }
    uint16_t raw_t = ((uint16_t)bytes[0] << 8) | bytes[1];
    uint16_t raw_h = ((uint16_t)bytes[3] << 8) | bytes[4];
    *temperature = -45.0 + 175.0 * raw_t / 65536.0;
    *humidity = 100.0 * raw_h / 65536.0;
    return ESP_OK;
}

void epaper_154g_sensors_init(void) {
    i2c_master_bus_handle_t bus;
    esp_err_t err = i2c_master_get_bus_handle(I2C_NUM_0, &bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SHTC3 bus unavailable: %s", esp_err_to_name(err));
        return;
    }
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return;
    const i2c_device_config_t config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = 0x70,
        .scl_speed_hz = 100000,
    };
    err = i2c_master_bus_add_device(bus, &config, &s_sensor);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SHTC3 setup failed: %s", esp_err_to_name(err));
        return;
    }
    err = sensor_write(0xb098);
    if (err != ESP_OK) ESP_LOGW(TAG, "SHTC3 sleep failed: %s", esp_err_to_name(err));
}

cJSON *epaper_154g_sensors_command(void) {
    double temperature = 0, humidity = 0;
    esp_err_t err = ESP_ERR_INVALID_STATE;
    if (s_sensor && s_lock) {
        // The IDF bus serializes transactions with the ES8311; this lock also
        // keeps two sensor callers from interleaving wake, measure and sleep.
        xSemaphoreTake(s_lock, portMAX_DELAY);
        err = sensor_read(&temperature, &humidity);
        xSemaphoreGive(s_lock);
    }
    cJSON *result = cJSON_CreateObject();
    if (!result) return NULL;
    if (err != ESP_OK) {
        cJSON_AddBoolToObject(result, "ok", false);
        cJSON *error = cJSON_AddObjectToObject(result, "error");
        if (!error) { cJSON_Delete(result); return NULL; }
        cJSON_AddStringToObject(error, "code", "no_readings");
        cJSON_AddStringToObject(error, "message", "no valid SHTC3 temperature and humidity reading");
        return result;
    }
    cJSON_AddBoolToObject(result, "ok", true);
    cJSON *payload = cJSON_AddObjectToObject(result, "payload");
    if (!payload) { cJSON_Delete(result); return NULL; }
    const char *names[] = {"temperature", "humidity"};
    const char *units[] = {"celsius", "percent_rh"};
    const double values[] = {temperature, humidity};
    for (int i = 0; i < 2; i++) {
        cJSON *sensor = cJSON_AddObjectToObject(payload, names[i]);
        if (!sensor) { cJSON_Delete(result); return NULL; }
        cJSON_AddNumberToObject(sensor, "value", round(values[i] * 10) / 10);
        cJSON_AddStringToObject(sensor, "unit", units[i]);
        cJSON_AddNumberToObject(sensor, "age_s", 0);
    }
    return result;
}
