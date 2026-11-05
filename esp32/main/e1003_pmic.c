// SPDX-License-Identifier: Apache-2.0
// E1003 V1.0 schematic: SY6974B on GPIO19/20 I2C, address 0x6B.
// Status masks follow Seeed's source/lib/SY6974/SY6974.cpp.

#include "e1003_pmic.h"
#include "reterminal_e100x_board.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define PMIC_ADDR 0x6b

static i2c_master_dev_handle_t s_pmic;
static SemaphoreHandle_t s_op_lock;
static bool s_initializing;
static portMUX_TYPE s_init_lock = portMUX_INITIALIZER_UNLOCKED;

static bool device_ready(void) {
    // Publish the device and its operation lock together. Blocking work is
    // outside the short critical section; a failed attempt remains retryable.
    for (;;) {
        portENTER_CRITICAL(&s_init_lock);
        if (s_pmic) {
            portEXIT_CRITICAL(&s_init_lock);
            return true;
        }
        if (!s_initializing) {
            s_initializing = true;
            portEXIT_CRITICAL(&s_init_lock);
            break;
        }
        portEXIT_CRITICAL(&s_init_lock);
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    i2c_master_bus_handle_t bus = reterminal_e100x_i2c_bus_get();
    bool found = bus && i2c_master_probe(bus, PMIC_ADDR, 100) == ESP_OK;
    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = PMIC_ADDR,
        .scl_speed_hz = 100000,
    };
    i2c_master_dev_handle_t device = NULL;
    if (found) found = i2c_master_bus_add_device(bus, &cfg, &device) == ESP_OK;
    SemaphoreHandle_t lock = found ? xSemaphoreCreateMutex() : NULL;
    if (found && !lock) {
        i2c_master_bus_rm_device(device);
        found = false;
    }
    portENTER_CRITICAL(&s_init_lock);
    if (found) {
        s_pmic = device;
        s_op_lock = lock;
    }
    s_initializing = false;
    portEXIT_CRITICAL(&s_init_lock);
    return found;
}

static bool read_reg(uint8_t reg, uint8_t *value) {
    return i2c_master_transmit_receive(s_pmic, &reg, 1, value, 1, 100) == ESP_OK;
}

bool e1003_pmic_read(e1003_pmic_status_t *out) {
    if (!out || !device_ready()) return false;
    if (xSemaphoreTake(s_op_lock, pdMS_TO_TICKS(100)) != pdTRUE) return false;
    // SY6974B REG09 first read returns the latched previous fault and clears
    // it; the second consecutive read reports the present fault condition.
    uint8_t status, fault_latched, fault, bus;
    bool ok = read_reg(0x08, &status) && read_reg(0x09, &fault_latched)
           && read_reg(0x09, &fault) && read_reg(0x0a, &bus);
    xSemaphoreGive(s_op_lock);
    if (!ok) return false;
    uint8_t input = (status >> 5) & 0x07;
    uint8_t charge = (status >> 3) & 0x03;
    *out = (e1003_pmic_status_t){
        .status = status, .fault_latched = fault_latched,
        .fault = fault, .bus = bus,
        .input_code = input, .charge_code = charge,
        .bus_good = (bus & 0x80) != 0,
        .input_present = (bus & 0x80) != 0 && input >= 1 && input <= 6,
        .charging = charge == 1 || charge == 2,
        .charge_done = charge == 3,
    };
    return true;
}

bool e1003_pmic_read_power(bool *charging, bool *input_present) {
    if (!charging || !input_present || !device_ready()) return false;
    if (xSemaphoreTake(s_op_lock, pdMS_TO_TICKS(100)) != pdTRUE) return false;
    uint8_t status, bus;
    bool ok = read_reg(0x08, &status) && read_reg(0x0a, &bus);
    xSemaphoreGive(s_op_lock);
    if (!ok) return false;
    uint8_t input = (status >> 5) & 0x07;
    uint8_t charge = (status >> 3) & 0x03;
    *charging = charge == 1 || charge == 2;
    *input_present = (bus & 0x80) != 0 && input >= 1 && input <= 6;
    return true;
}

const char *e1003_pmic_input_name(uint8_t code) {
    static const char *const names[] = {
        "none", "usb_sdp", "usb_cdp", "usb_dcp", "high_voltage_dcp",
        "unknown_adapter", "nonstandard_adapter", "otg",
    };
    return code < 8 ? names[code] : "unknown";
}

const char *e1003_pmic_charge_name(uint8_t code) {
    static const char *const names[] = {"not_charging", "precharge", "fast_charging", "charge_done"};
    return code < 4 ? names[code] : "unknown";
}
