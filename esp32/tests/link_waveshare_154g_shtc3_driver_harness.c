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

// Host SHTC3 driver harness: the whole driver against a fake I2C bus and
// FreeRTOS. The runner supplies empty IDF headers; the fakes are below.
// stack_monitor.h is the real one, from main/.
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_STATE 0x103
static const char *esp_err_to_name(esp_err_t err) { return err == ESP_OK ? "ESP_OK" : "error"; }

#define ESP_LOGI(tag, ...) ((void)(tag))
#define ESP_LOGW(tag, ...) ((void)(tag))
#define ESP_LOGE(tag, ...) ((void)(tag))

static int64_t fake_now_us = 1000000;
static int64_t esp_timer_get_time(void) { return fake_now_us; }

typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define taskENTER_CRITICAL(lock) ((void)(lock))
#define taskEXIT_CRITICAL(lock) ((void)(lock))
#define pdMS_TO_TICKS(ms) (ms)
#define pdPASS 1
typedef void (*TaskFunction_t)(void *);
static void vTaskDelay(int ticks) { (void)ticks; }
static TaskFunction_t fake_task;
static int xTaskCreate(TaskFunction_t fn, const char *name, int stack, void *arg, int prio, void *handle) {
    (void)name; (void)stack; (void)arg; (void)prio; (void)handle;
    fake_task = fn;
    return pdPASS;
}

typedef int i2c_port_num_t;
#define I2C_NUM_0 0
#define I2C_CLK_SRC_DEFAULT 0
#define I2C_ADDR_BIT_LEN_7 0
typedef struct {
    i2c_port_num_t i2c_port;
    int sda_io_num, scl_io_num, clk_source, glitch_ignore_cnt;
    struct { unsigned enable_internal_pullup : 1; } flags;
} i2c_master_bus_config_t;
typedef struct { int dev_addr_length; uint16_t device_address; uint32_t scl_speed_hz; } i2c_device_config_t;
typedef struct fake_bus *i2c_master_bus_handle_t;
typedef struct fake_dev *i2c_master_dev_handle_t;
static struct fake_bus { int unused; } fake_bus;
static struct fake_dev { int unused; } fake_dev;

static bool fake_sensor_answers;   // the SHTC3 ACKs and measures
static bool fake_device_added;
static uint16_t fake_commands[8];
static int fake_command_count;
static uint8_t fake_raw[6];

static esp_err_t i2c_master_get_bus_handle(i2c_port_num_t port, i2c_master_bus_handle_t *bus) {
    (void)port; (void)bus;
    return ESP_ERR_INVALID_STATE;   // no bus yet: the driver creates it
}
static esp_err_t i2c_new_master_bus(const i2c_master_bus_config_t *cfg, i2c_master_bus_handle_t *bus) {
    assert(cfg->sda_io_num == 47 && cfg->scl_io_num == 48);
    *bus = &fake_bus;
    return ESP_OK;
}
static esp_err_t i2c_master_probe(i2c_master_bus_handle_t bus, uint16_t addr, int timeout) {
    (void)bus; (void)timeout;
    assert(addr == 0x70);
    return fake_sensor_answers ? ESP_OK : ESP_FAIL;
}
static esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t bus, const i2c_device_config_t *cfg,
                                           i2c_master_dev_handle_t *dev) {
    (void)bus;
    assert(cfg->device_address == 0x70);
    fake_device_added = true;
    *dev = &fake_dev;
    return ESP_OK;
}
static esp_err_t i2c_master_transmit(i2c_master_dev_handle_t dev, const uint8_t *buf, size_t len, int timeout) {
    (void)timeout;
    assert(dev == &fake_dev && len == 2);
    if (fake_command_count < 8) fake_commands[fake_command_count++] = (uint16_t)((buf[0] << 8) | buf[1]);
    return fake_sensor_answers ? ESP_OK : ESP_FAIL;
}
static esp_err_t i2c_master_receive(i2c_master_dev_handle_t dev, uint8_t *buf, size_t len, int timeout) {
    (void)timeout;
    assert(dev == &fake_dev && len == 6);
    if (!fake_sensor_answers) return ESP_FAIL;
    memcpy(buf, fake_raw, 6);
    return ESP_OK;
}

#define CONFIG_HOMEHUB_WAVESHARE_154G_SHTC3_TEMP_OFFSET -40
#include "waveshare_154g_shtc3.c"

void stack_monitor_poll(stack_monitor_t *state) { (void)state; }

int main(void) {
    // The sensor misses the probe at boot: the driver still polls it.
    fake_sensor_answers = false;
    waveshare_154g_shtc3_init();
    assert(fake_device_added);
    assert(fake_task == sensors_task);

    // While it stays silent, a poll fails, still sends sleep, and
    // sensors.read says there are no readings.
    assert(!poll_once());
    assert(fake_commands[fake_command_count - 1] == SHTC3_CMD_SLEEP);
    cJSON *result = waveshare_154g_shtc3_command();
    assert(cJSON_IsFalse(cJSON_GetObjectItem(result, "ok")));
    cJSON_Delete(result);

    // Once it answers, the next poll reads it: wake, measure, sleep.
    fake_sensor_answers = true;
    fake_command_count = 0;
    fake_raw[0] = 0x80; fake_raw[1] = 0x00; fake_raw[2] = shtc3_crc(fake_raw, 2);
    fake_raw[3] = 0x80; fake_raw[4] = 0x00; fake_raw[5] = shtc3_crc(fake_raw + 3, 2);
    assert(poll_once());
    assert(fake_command_count == 3);
    assert(fake_commands[0] == SHTC3_CMD_WAKEUP);
    assert(fake_commands[1] == SHTC3_CMD_MEASURE);
    assert(fake_commands[2] == SHTC3_CMD_SLEEP);

    // 42.5 C less the -4 C offset, and 50 %RH.
    result = waveshare_154g_shtc3_command();
    assert(cJSON_IsTrue(cJSON_GetObjectItem(result, "ok")));
    char *json = cJSON_PrintUnformatted(cJSON_GetObjectItem(result, "payload"));
    assert(strstr(json, "\"temperature\":{\"value\":38.5,\"unit\":\"celsius\",\"age_s\":0}"));
    assert(strstr(json, "\"humidity\":{\"value\":50,\"unit\":\"percent_rh\",\"age_s\":0}"));
    cJSON_free(json);
    cJSON_Delete(result);

    puts("PASS waveshare 1.54G shtc3 driver: polls after a missed boot probe, recovers, wake/measure/sleep");
    return 0;
}
