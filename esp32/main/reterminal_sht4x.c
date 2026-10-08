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

#include "reterminal_sht4x.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "stack_monitor.h"

// ---- Protocol (host-tested) -------------------------------------------------

// Sensirion SHT4x on the reTerminal E100x's I2C0 bus. High-precision
// single-shot measurement: send SHT4X_CMD_MEASURE_HPM, wait ~10 ms, read
// back 6 bytes (temperature MSB/LSB/CRC, humidity MSB/LSB/CRC).
#define SHT4X_ADDR            0x44
#define SHT4X_CMD_MEASURE_HPM 0xFD
#define SHT4X_MEASURE_MS      10

// CRC-8 of each 2-byte word: polynomial 0x31, initialized to 0xFF.
static uint8_t sht4x_crc(const uint8_t *data, size_t len) {
    uint8_t crc = 0xFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

// Decode one 6-byte measurement into degrees Celsius and percent RH.
// Returns false when either CRC fails. Humidity is clamped to 0-100%.
static bool sht4x_decode(const uint8_t raw[6], float *temp_c, float *rh_pct) {
    if (sht4x_crc(raw, 2) != raw[2] || sht4x_crc(raw + 3, 2) != raw[5]) {
        return false;
    }
    uint16_t raw_t = (uint16_t)((raw[0] << 8) | raw[1]);
    uint16_t raw_rh = (uint16_t)((raw[3] << 8) | raw[4]);
    *temp_c = -45.0f + 175.0f * (float)raw_t / 65535.0f;
    *rh_pct = -6.0f + 125.0f * (float)raw_rh / 65535.0f;
    if (*rh_pct < 0.0f) *rh_pct = 0.0f;
    if (*rh_pct > 100.0f) *rh_pct = 100.0f;
    return isfinite(*temp_c) && isfinite(*rh_pct);
}

// Readings older than this count as missing, so a dead sensor drops out.
#define STALE_US (30LL * 1000 * 1000)

enum { SENSOR_TEMPERATURE, SENSOR_HUMIDITY, SENSOR_COUNT };

static const struct {
    const char *name;
    const char *unit;
    int decimals;
} SENSORS[SENSOR_COUNT] = {
    [SENSOR_TEMPERATURE] = {"temperature", "celsius", 1},
    [SENSOR_HUMIDITY] = {"humidity", "percent_rh", 1},
};

typedef struct {
    bool valid;
    float value;
    int64_t at_us;
} reading_t;

// sensors.read result: each reading with its value, unit and age in
// seconds; stale readings are null; with no readings at all, an error.
static cJSON *readings_result(const reading_t *readings, int64_t now_us) {
    cJSON *result = cJSON_CreateObject();
    cJSON *payload = cJSON_CreateObject();
    if (!result || !payload) {
        cJSON_Delete(result);
        cJSON_Delete(payload);
        return NULL;
    }
    bool any = false;
    for (int i = 0; i < SENSOR_COUNT; i++) {
        const reading_t *r = &readings[i];
        if (!r->valid || now_us - r->at_us > STALE_US) {
            cJSON_AddNullToObject(payload, SENSORS[i].name);
            continue;
        }
        cJSON *sensor = cJSON_AddObjectToObject(payload, SENSORS[i].name);
        double scale = pow(10, SENSORS[i].decimals);
        cJSON_AddNumberToObject(sensor, "value", round(r->value * scale) / scale);
        cJSON_AddStringToObject(sensor, "unit", SENSORS[i].unit);
        cJSON_AddNumberToObject(sensor, "age_s", (double)((now_us - r->at_us) / 1000000));
        any = true;
    }
    if (!any) {
        cJSON_Delete(payload);
        cJSON_AddBoolToObject(result, "ok", false);
        cJSON *error = cJSON_AddObjectToObject(result, "error");
        cJSON_AddStringToObject(error, "code", "no_readings");
        cJSON_AddStringToObject(error, "message",
                                "no sensor readings; the SHT4x may be missing");
        return result;
    }
    cJSON_AddBoolToObject(result, "ok", true);
    cJSON_AddItemToObject(result, "payload", payload);
    return result;
}

// ---- I2C driver --------------------------------------------------------------

// Seeed's E100x pinout: the onboard SHT4x hangs off I2C0.
#define SHT4X_SDA_GPIO 19
#define SHT4X_SCL_GPIO 20

// Poll period: the sensor answers in ~10 ms, so this is cheap.
#define POLL_MS 10000

static const char *TAG = "link.sht4x";

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static reading_t s_readings[SENSOR_COUNT];
static i2c_master_dev_handle_t s_dev = NULL;

// Board heat warms the onboard sensor; Kconfig offset, tenths of a C.
#define SHT4X_TEMP_OFFSET_C (CONFIG_HOMEHUB_RETERMINAL_SHT4X_TEMP_OFFSET / 10.0f)

static bool poll_once(void) {
    uint8_t cmd = SHT4X_CMD_MEASURE_HPM;
    uint8_t raw[6];
    if (i2c_master_transmit(s_dev, &cmd, 1, 100) != ESP_OK) return false;
    vTaskDelay(pdMS_TO_TICKS(SHT4X_MEASURE_MS));
    if (i2c_master_receive(s_dev, raw, sizeof(raw), 100) != ESP_OK) return false;
    float temp_c, rh_pct;
    if (!sht4x_decode(raw, &temp_c, &rh_pct)) return false;
    temp_c += SHT4X_TEMP_OFFSET_C;
    int64_t now = esp_timer_get_time();
    taskENTER_CRITICAL(&s_lock);
    s_readings[SENSOR_TEMPERATURE] = (reading_t){true, temp_c, now};
    s_readings[SENSOR_HUMIDITY] = (reading_t){true, rh_pct, now};
    taskEXIT_CRITICAL(&s_lock);
    return true;
}

static void sensors_task(void *arg) {
    (void)arg;
    stack_monitor_t stack = STACK_MONITOR_INIT;
    bool logged = false;
    bool failing = false;
    for (;;) {
        if (poll_once()) {
            if (!logged) {
                logged = true;
                ESP_LOGI(TAG, "first readings: %.1f C, %.1f %%RH",
                         s_readings[SENSOR_TEMPERATURE].value,
                         s_readings[SENSOR_HUMIDITY].value);
            } else if (failing) {
                ESP_LOGI(TAG, "sensor reads recovered");
            }
            failing = false;
        } else if (!failing) {
            // Log once per run of failures, not every poll.
            failing = true;
            ESP_LOGW(TAG, "sensor read failed; will retry");
        }
        stack_monitor_poll(&stack);
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

void reterminal_sht4x_init(void) {
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = -1,
        .sda_io_num = SHT4X_SDA_GPIO,
        .scl_io_num = SHT4X_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus = NULL;
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &bus);
    if (err == ESP_OK && i2c_master_probe(bus, SHT4X_ADDR, 100) != ESP_OK) {
        err = ESP_ERR_NOT_FOUND;
    }
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = SHT4X_ADDR,
        .scl_speed_hz = 100000,
    };
    if (err == ESP_OK) err = i2c_master_bus_add_device(bus, &dev_cfg, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "no SHT4x at 0x%02x: %s", SHT4X_ADDR, esp_err_to_name(err));
        if (bus) i2c_del_master_bus(bus);
        return;
    }
    if (xTaskCreate(sensors_task, "sht4x", 3072, NULL, 2, NULL) != pdPASS) {
        ESP_LOGE(TAG, "failed to start the sensor task");
        return;
    }
    ESP_LOGI(TAG, "polling the SHT4x on I2C (SDA=%d SCL=%d)",
             SHT4X_SDA_GPIO, SHT4X_SCL_GPIO);
}

cJSON *reterminal_sht4x_command(void) {
    reading_t snapshot[SENSOR_COUNT];
    taskENTER_CRITICAL(&s_lock);
    memcpy(snapshot, s_readings, sizeof(snapshot));
    taskEXIT_CRITICAL(&s_lock);
    return readings_result(snapshot, esp_timer_get_time());
}
