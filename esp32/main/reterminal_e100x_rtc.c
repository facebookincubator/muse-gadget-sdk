// SPDX-License-Identifier: Apache-2.0

#include "reterminal_e100x_rtc.h"

#include "reterminal_e100x_board.h"

#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define RTC_ADDR 0x51

static i2c_master_dev_handle_t s_rtc;
static SemaphoreHandle_t s_op_lock;
static bool s_initializing;
static portMUX_TYPE s_init_lock = portMUX_INITIALIZER_UNLOCKED;

static bool rtc_device(void) {
    for (;;) {
        portENTER_CRITICAL(&s_init_lock);
        if (s_rtc) {
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
    bool found = bus && i2c_master_probe(bus, RTC_ADDR, 100) == ESP_OK;
    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = RTC_ADDR,
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
        s_rtc = device;
        s_op_lock = lock;
    }
    s_initializing = false;
    portEXIT_CRITICAL(&s_init_lock);
    return found;
}

static int from_bcd(uint8_t value) {
    return (value & 15) + 10 * (value >> 4);
}

static uint8_t to_bcd(int value) {
    return (uint8_t)((value / 10 << 4) | (value % 10));
}

static bool valid_date(const reterminal_e100x_rtc_time_t *t) {
    if (!t || t->year < 2000 || t->year > 2099 || t->month < 1 || t->month > 12
        || t->hour < 0 || t->hour > 23 || t->minute < 0 || t->minute > 59
        || t->second < 0 || t->second > 59) return false;
    static const int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int max_day = days[t->month - 1];
    if (t->month == 2 && t->year % 4 == 0) max_day++;
    return t->day >= 1 && t->day <= max_day;
}

static uint8_t weekday(const reterminal_e100x_rtc_time_t *t) {
    static const int before_month[] = {0,31,59,90,120,151,181,212,243,273,304,334};
    int years = t->year - 2000;
    int days = years * 365 + (years + 3) / 4 + before_month[t->month - 1] + t->day - 1;
    if (t->month > 2 && t->year % 4 == 0) days++;
    return (uint8_t)((days + 6) % 7); // 2000-01-01 was Saturday; Sunday=0.
}

// Match Seeed's RTC initialization: resume a stopped oscillator without
// replacing the user's calendar or clearing the voltage-low indication.
// The caller holds s_op_lock. Verify the STOP bit after the write.
static bool ensure_running(void) {
    uint8_t reg = 0x00, control;
    if (i2c_master_transmit_receive(s_rtc, &reg, 1, &control, 1, 100) != ESP_OK) return false;
    if (!(control & 0x20)) return true;
    uint8_t start[] = {0x00, (uint8_t)(control & ~0x20)};
    if (i2c_master_transmit(s_rtc, start, sizeof(start), 100) != ESP_OK) return false;
    return i2c_master_transmit_receive(s_rtc, &reg, 1, &control, 1, 100) == ESP_OK
        && !(control & 0x20);
}

bool reterminal_e100x_rtc_read(reterminal_e100x_rtc_time_t *out) {
    if (!out || !rtc_device()) return false;
    if (xSemaphoreTake(s_op_lock, pdMS_TO_TICKS(100)) != pdTRUE) return false;
    if (!ensure_running()) {
        xSemaphoreGive(s_op_lock);
        return false;
    }
    uint8_t reg = 0x02, data[7];
    esp_err_t err = i2c_master_transmit_receive(s_rtc, &reg, 1, data, sizeof(data), 100);
    xSemaphoreGive(s_op_lock);
    if (err != ESP_OK) return false;
    out->second = from_bcd(data[0] & 0x7f);
    out->minute = from_bcd(data[1] & 0x7f);
    out->hour = from_bcd(data[2] & 0x3f);
    out->day = from_bcd(data[3] & 0x3f);
    out->month = from_bcd(data[5] & 0x1f);
    out->year = 2000 + from_bcd(data[6]);
    out->valid = !(data[0] & 0x80) && !(data[5] & 0x80) && valid_date(out);
    return true;
}

bool reterminal_e100x_rtc_set(const reterminal_e100x_rtc_time_t *t) {
    if (!valid_date(t) || !rtc_device()) return false;
    if (xSemaphoreTake(s_op_lock, pdMS_TO_TICKS(100)) != pdTRUE) return false;
    uint8_t control_reg = 0x00, control;
    if (i2c_master_transmit_receive(s_rtc, &control_reg, 1, &control, 1, 100) != ESP_OK) {
        xSemaphoreGive(s_op_lock);
        return false;
    }
    uint8_t stop[] = {0x00, (uint8_t)(control | 0x20)};
    if (i2c_master_transmit(s_rtc, stop, sizeof(stop), 100) != ESP_OK) {
        xSemaphoreGive(s_op_lock);
        return false;
    }
    // PCF8563 time registers 0x02..0x08, including the derived weekday.
    uint8_t data[] = {0x02, to_bcd(t->second), to_bcd(t->minute), to_bcd(t->hour),
                      to_bcd(t->day), weekday(t), to_bcd(t->month), to_bcd(t->year - 2000)};
    bool written = i2c_master_transmit(s_rtc, data, sizeof(data), 100) == ESP_OK;
    uint8_t start[] = {0x00, (uint8_t)(control & ~0x20)};
    bool running = i2c_master_transmit(s_rtc, start, sizeof(start), 100) == ESP_OK;
    xSemaphoreGive(s_op_lock);
    return written && running;
}
