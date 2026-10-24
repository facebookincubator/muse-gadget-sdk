// SPDX-License-Identifier: Apache-2.0
// E1003 V1.0 schematic: GT911 on SDA19/SCL20, INT2, RST48.
// Register addresses and reset/address sequence follow Seeed's factory
// firmware source/lib/GT911 and src/boards/common/gt911_touch.cpp.

#include "e1003_touch.h"
#include "reterminal_e100x_events.h"
#include "reterminal_e100x_board.h"

#include <string.h>

#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define GT911_ADDR 0x5d
#define GT911_STATUS 0x814e
#define GT911_POINTS 0x8150
#define GT911_RESOLUTION 0x8146
#define PANEL_WIDTH E100X_WIDTH
#define PANEL_HEIGHT E100X_HEIGHT
#define PIN_INT E100X_TOUCH_INT_GPIO
#define PIN_RST E100X_TOUCH_RESET_GPIO

static const char *TAG = "e1003.touch";
static i2c_master_dev_handle_t s_dev;
static e1003_touch_state_t s_state;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t s_touch_task;
static volatile bool s_irq_ready;

static void IRAM_ATTR touch_irq(void *arg) {
    (void)arg;
    if (s_touch_task) vTaskNotifyGiveFromISR(s_touch_task, NULL);
}

static esp_err_t read_reg(uint16_t reg, uint8_t *data, size_t len) {
    uint8_t address[] = {(uint8_t)(reg >> 8), (uint8_t)reg};
    return i2c_master_transmit_receive(s_dev, address, sizeof(address), data, len, 100);
}

static esp_err_t write_reg(uint16_t reg, uint8_t value) {
    uint8_t data[] = {(uint8_t)(reg >> 8), (uint8_t)reg, value};
    return i2c_master_transmit(s_dev, data, sizeof(data), 100);
}

static uint16_t clamp_scale(uint16_t raw, uint16_t sensor, uint16_t panel) {
    if (!sensor) return 0;
    uint32_t mapped = (uint32_t)raw * panel / sensor;
    return (uint16_t)(mapped >= panel ? panel - 1 : mapped);
}

static void touch_task(void *arg) {
    (void)arg;
    bool failing = false;
    for (;;) {
        uint8_t status;
        esp_err_t err = read_reg(GT911_STATUS, &status, 1);
        if (err == ESP_OK && (status & 0x80)) {
            int count = status & 0x0f;
            uint8_t point[8];
            if (count == 1) err = read_reg(GT911_POINTS, point, sizeof(point));
            if (err == ESP_OK) err = write_reg(GT911_STATUS, 0);
            if (err == ESP_OK && (count == 0 || count == 1)) {
                bool pressed = count == 1;
                uint16_t x = 0, y = 0;
                if (pressed) {
                    uint16_t raw_x = point[0] | (uint16_t)point[1] << 8;
                    uint16_t raw_y = point[2] | (uint16_t)point[3] << 8;
                    x = clamp_scale(raw_x, s_state.sensor_width, PANEL_WIDTH);
                    y = clamp_scale(raw_y, s_state.sensor_height, PANEL_HEIGHT);
                }
                portENTER_CRITICAL(&s_lock);
                bool was_pressed = s_state.pressed;
                if (!pressed) { x = s_state.x; y = s_state.y; }
                s_state.pressed = pressed;
                s_state.x = x;
                s_state.y = y;
                portEXIT_CRITICAL(&s_lock);
                if (pressed != was_pressed) {
                    reterminal_e100x_events_touch(pressed, x, y);
                    ESP_LOGI(TAG, "touch %s x=%u y=%u", pressed ? "down" : "up", x, y);
                }
            }
        }
        if (err != ESP_OK && !failing) {
            ESP_LOGW(TAG, "GT911 read failed: %s", esp_err_to_name(err));
            failing = true;
        } else if (err == ESP_OK) {
            failing = false;
        }
        if (s_irq_ready) {
            // INT reports coordinate changes; periodic fallback recovers a
            // missed edge and bounds release latency without 50 I2C reads/s.
            portENTER_CRITICAL(&s_lock);
            bool pressed_now = s_state.pressed;
            portEXIT_CRITICAL(&s_lock);
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(pressed_now ? 50 : 250));
        } else {
            // Preserve the proven polling behavior if the GPIO ISR service
            // is unavailable in a particular build or boot sequence.
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
}

bool e1003_touch_init(void) {
    // Hold INT low while releasing RESET to select GT911's 0x5D address.
    const gpio_config_t outputs = {
        .pin_bit_mask = (1ULL << PIN_INT) | (1ULL << PIN_RST),
        .mode = GPIO_MODE_OUTPUT,
    };
    if (gpio_config(&outputs) != ESP_OK) return false;
    gpio_set_level(PIN_INT, 0);
    gpio_set_level(PIN_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(12));
    gpio_set_level(PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(8));
    const gpio_config_t interrupt_pin = {
        .pin_bit_mask = 1ULL << PIN_INT,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
    };
    if (gpio_config(&interrupt_pin) != ESP_OK) return false;
    vTaskDelay(pdMS_TO_TICKS(50));

    i2c_master_bus_handle_t bus = reterminal_e100x_i2c_bus_get();
    if (!bus) {
        ESP_LOGW(TAG, "GT911 unavailable: shared I2C bus init failed");
        return false;
    }
    esp_err_t err = i2c_master_probe(bus, GT911_ADDR, 100);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "GT911 probe at 0x5D failed: %s", esp_err_to_name(err));
        return false;
    }
    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = GT911_ADDR,
        .scl_speed_hz = 100000,
    };
    err = i2c_master_bus_add_device(bus, &cfg, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "GT911 device registration failed: %s", esp_err_to_name(err));
        return false;
    }
    uint8_t resolution[4];
    err = read_reg(GT911_RESOLUTION, resolution, sizeof(resolution));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "GT911 resolution read failed: %s", esp_err_to_name(err));
        return false;
    }
    uint16_t width = resolution[0] | (uint16_t)resolution[1] << 8;
    uint16_t height = resolution[2] | (uint16_t)resolution[3] << 8;
    if (!width || !height) { width = 2048; height = 2048; }
    portENTER_CRITICAL(&s_lock);
    s_state.available = true;
    s_state.sensor_width = width;
    s_state.sensor_height = height;
    portEXIT_CRITICAL(&s_lock);
    if (xTaskCreate(touch_task, "e1003_touch", 3072, NULL, 2, &s_touch_task) != pdPASS) {
        portENTER_CRITICAL(&s_lock);
        s_state.available = false;
        portEXIT_CRITICAL(&s_lock);
        return false;
    }
    err = gpio_install_isr_service(0);
    if (err == ESP_OK || err == ESP_ERR_INVALID_STATE) {
        err = gpio_set_intr_type(PIN_INT, GPIO_INTR_ANYEDGE);
        if (err == ESP_OK) err = gpio_isr_handler_add(PIN_INT, touch_irq, NULL);
        if (err == ESP_OK) {
            s_irq_ready = true;
            xTaskNotifyGive(s_touch_task); // read any status pending during setup
        } else {
            gpio_set_intr_type(PIN_INT, GPIO_INTR_DISABLE);
        }
    }
    if (!s_irq_ready) ESP_LOGW(TAG, "GT911 INT unavailable; retaining 20 ms polling");
    ESP_LOGI(TAG, "GT911 ready at 0x5D, resolution %ux%u", width, height);
    return true;
}

void e1003_touch_read(e1003_touch_state_t *out) {
    if (!out) return;
    portENTER_CRITICAL(&s_lock);
    *out = s_state;
    portEXIT_CRITICAL(&s_lock);
}
