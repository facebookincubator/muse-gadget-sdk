/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "boards/waveshare_s3_185b_power.h"

struct fake_i2c_bus { int unused; };
struct fake_i2c_dev { int unused; };
static struct fake_i2c_bus bus;
static struct fake_i2c_dev gauge;
static i2c_master_bus_handle_t s_i2c = &bus;
static i2c_master_dev_handle_t s_gauge;
static const char *TAG = "board";
static esp_err_t add_error, read_error, logged_error;
static int add_calls, read_calls, warnings, infos;
static bool usb;

static bool usb_serial_jtag_is_connected(void) { return usb; }
static const char *error_name(esp_err_t err) { logged_error = err; return "error"; }
#define esp_err_to_name(err) error_name(err)
#define ESP_LOGW(tag, fmt, arg) do { (void)(tag); (void)(fmt); (void)(arg); ++warnings; } while (0)
#define ESP_LOGI(tag, ...) do { (void)(tag); ++infos; } while (0)

esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t handle,
                                   const i2c_device_config_t *cfg,
                                   i2c_master_dev_handle_t *out)
{
    assert(handle == &bus && cfg->device_address == 0x55 && cfg->scl_speed_hz == 100000);
    assert(cfg->dev_addr_length == I2C_ADDR_BIT_LEN_7);
    ++add_calls;
    if (add_error) return add_error;
    *out = &gauge;
    return ESP_OK;
}

esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t handle,
                                    const uint8_t *wbuf, size_t wlen,
                                    uint8_t *rbuf, size_t rlen, int timeout_ms)
{
    assert(handle == &gauge && wlen == 1 && rlen == 2 && timeout_ms == 100);
    ++read_calls;
    if (read_error) return read_error;
    uint16_t value;
    switch (wbuf[0]) {
    case 0x0a: value = 8; break;
    case 0x3a: value = 32; break;
    case 0x08: value = 3805; break;
    case 0x2c: value = 9; break;
    case 0x0c: value = 100; break;
    default: assert(false); return ESP_FAIL;
    }
    rbuf[0] = value & 255;
    rbuf[1] = value >> 8;
    return ESP_OK;
}

typedef struct { int x1, y1, x2, y2; } lv_area_t;
typedef struct { lv_area_t *area; } lv_event_t;
static void *lv_event_get_param(lv_event_t *event) { return event->area; }

/* Exact function definitions extracted from the board source by the test. */
#include "waveshare_185b_board_functions.h"

static void check_area(lv_area_t original)
{
    lv_area_t area = original;
    lv_event_t event = { &area };
    round_area(&event);
    assert((area.x1 & 1) == 0 && (area.y1 & 1) == 0);
    assert((area.x2 & 1) == 1 && (area.y2 & 1) == 1);
    assert(area.x1 >= 0 && area.y1 >= 0 && area.x2 < 360 && area.y2 < 360);
    assert(area.x1 <= original.x1 && area.y1 <= original.y1);
    assert(area.x2 >= original.x2 && area.y2 >= original.y2);
    assert(original.x1 - area.x1 <= 1 && original.y1 - area.y1 <= 1);
    assert(area.x2 - original.x2 <= 1 && area.y2 - original.y2 <= 1);
    lv_area_t once = area;
    round_area(&event);
    assert(memcmp(&once, &area, sizeof(area)) == 0);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    if (strcmp(argv[1], "alignment") == 0) {
        for (int y = 0; y < 360; ++y) {
            for (int x = 0; x < 360; ++x) check_area((lv_area_t){x, y, x, y});
        }
        check_area((lv_area_t){0, 0, 359, 359});
        check_area((lv_area_t){1, 3, 358, 356});
        check_area((lv_area_t){20, 30, 99, 199});
        return 0;
    }
    assert(strcmp(argv[1], "power") == 0);
    muse_power_t out = { .battery_pct = 99, .battery_mv = 4200, .charging = true };
    usb = true;
    add_error = ESP_FAIL;
    assert(read_power(&out) == ESP_OK);
    assert(out.battery_pct == -1 && out.battery_mv == 0 && !out.charging && out.usb);
    assert(warnings == 1 && logged_error == ESP_FAIL && read_calls == 0);
    assert(add_calls == 1 && !s_gauge);
    assert(read_power(&out) == ESP_OK);
    assert(add_calls == 2 && warnings == 1);  /* retries, without log spam */
    add_error = ESP_OK;
    assert(read_power(&out) == ESP_OK);
    assert(add_calls == 3 && read_calls == 5 && infos == 1);
    assert(out.battery_pct == 9 && out.battery_mv == 3805 && out.charging);
    usb = false;
    read_error = ESP_ERR_TIMEOUT;
    assert(read_power(&out) == ESP_OK);
    assert(out.battery_pct == -1 && out.battery_mv == 0 && !out.charging && !out.usb);
    assert(add_calls == 3 && warnings == 2 && logged_error == ESP_ERR_TIMEOUT);
    read_error = ESP_OK;
    assert(read_power(&out) == ESP_OK);
    assert(out.battery_pct == 9 && !out.usb && infos == 2);
    return 0;
}
