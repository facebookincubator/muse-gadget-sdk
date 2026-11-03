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

#include "waveshare_154g_shtc3.h"

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

// Sensirion SHTC3 on the 1.54G's I2C bus. It sleeps between
// measurements: wake it with SHTC3_CMD_WAKEUP, measure temperature first in
// normal mode without clock stretching, read back 6 bytes (temperature
// MSB/LSB/CRC, humidity MSB/LSB/CRC), then put it back to sleep.
#define SHTC3_ADDR          0x70
#define SHTC3_CMD_WAKEUP    0x3517
#define SHTC3_CMD_MEASURE   0x7866
#define SHTC3_CMD_SLEEP     0xB098
#define SHTC3_WAKEUP_MS     1
#define SHTC3_MEASURE_MS    13

// CRC-8 of each 2-byte word: polynomial 0x31, initialized to 0xFF.
static uint8_t shtc3_crc(const uint8_t *data, size_t len) {
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
// Returns false when either CRC fails.
static bool shtc3_decode(const uint8_t raw[6], float *temp_c, float *rh_pct) {
    if (shtc3_crc(raw, 2) != raw[2] || shtc3_crc(raw + 3, 2) != raw[5]) {
        return false;
    }
    uint16_t raw_t = (uint16_t)((raw[0] << 8) | raw[1]);
    uint16_t raw_rh = (uint16_t)((raw[3] << 8) | raw[4]);
    *temp_c = -45.0f + 175.0f * (float)raw_t / 65536.0f;
    *rh_pct = 100.0f * (float)raw_rh / 65536.0f;
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
                                "no sensor readings; the SHTC3 may be missing");
        return result;
    }
    cJSON_AddBoolToObject(result, "ok", true);
    cJSON_AddItemToObject(result, "payload", payload);
    return result;
}

// ---- I2C driver --------------------------------------------------------------

// Waveshare's 1.54G pinout: the onboard SHTC3 hangs off GPIO47/48.
#define SHTC3_SDA_GPIO 47
#define SHTC3_SCL_GPIO 48

// Poll period: a measurement takes ~13 ms, so this is cheap.
#define POLL_MS 10000

static const char *TAG = "link.shtc3";

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static reading_t s_readings[SENSOR_COUNT];
static i2c_master_dev_handle_t s_dev = NULL;

// Kconfig offset, tenths of a C. Waveshare's example subtracts 4 C.
#define SHTC3_TEMP_OFFSET_C (CONFIG_HOMEHUB_WAVESHARE_154G_SHTC3_TEMP_OFFSET / 10.0f)

static esp_err_t send_command(uint16_t command) {
    const uint8_t bytes[] = {(uint8_t)(command >> 8), (uint8_t)command};
    return i2c_master_transmit(s_dev, bytes, sizeof(bytes), 100);
}

static bool poll_once(void) {
    uint8_t raw[6];
    esp_err_t err = send_command(SHTC3_CMD_WAKEUP);
    // A delay of n ticks can end just after n - 1 tick periods: add one.
    if (err == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(SHTC3_WAKEUP_MS) + 1);
        err = send_command(SHTC3_CMD_MEASURE);
    }
    if (err == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(SHTC3_MEASURE_MS) + 1);
        err = i2c_master_receive(s_dev, raw, sizeof(raw), 100);
    }
    // Back to sleep after a failure too.
    send_command(SHTC3_CMD_SLEEP);
    if (err != ESP_OK) return false;
    float temp_c, rh_pct;
    if (!shtc3_decode(raw, &temp_c, &rh_pct)) return false;
    temp_c += SHTC3_TEMP_OFFSET_C;
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

void waveshare_154g_shtc3_init(void) {
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = SHTC3_SDA_GPIO,
        .scl_io_num = SHTC3_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    // The ES8311 codec and the RTC share this bus: use it if it exists, and
    // keep it when the sensor is missing.
    i2c_master_bus_handle_t bus = NULL;
    esp_err_t err = i2c_master_get_bus_handle(I2C_NUM_0, &bus);
    if (err != ESP_OK) err = i2c_new_master_bus(&bus_cfg, &bus);
    // A missed probe isn't fatal: poll anyway, so the sensor reports once it
    // answers, as the reTerminal's SHT4x does.
    if (err == ESP_OK && i2c_master_probe(bus, SHTC3_ADDR, 100) != ESP_OK) {
        ESP_LOGW(TAG, "SHTC3 not responding at boot; will retry reads");
    }
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = SHTC3_ADDR,
        .scl_speed_hz = 100000,
    };
    if (err == ESP_OK) err = i2c_master_bus_add_device(bus, &dev_cfg, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "can't add the SHTC3 at 0x%02x: %s", SHTC3_ADDR, esp_err_to_name(err));
        return;
    }
    if (xTaskCreate(sensors_task, "shtc3", 3072, NULL, 2, NULL) != pdPASS) {
        ESP_LOGE(TAG, "failed to start the sensor task");
        return;
    }
    ESP_LOGI(TAG, "polling the SHTC3 on I2C (SDA=%d SCL=%d)",
             SHTC3_SDA_GPIO, SHTC3_SCL_GPIO);
}

cJSON *waveshare_154g_shtc3_command(void) {
    reading_t snapshot[SENSOR_COUNT];
    taskENTER_CRITICAL(&s_lock);
    memcpy(snapshot, s_readings, sizeof(snapshot));
    taskEXIT_CRITICAL(&s_lock);
    return readings_result(snapshot, esp_timer_get_time());
}
