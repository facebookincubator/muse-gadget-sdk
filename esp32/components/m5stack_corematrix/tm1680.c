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

// The TM1680 LED driver as an esp_lcd panel, from M5GFX's Panel_TM1680
// (28d47949, src/lgfx/v1/panel/Panel_TM1680.{hpp,inl}): raw command bytes, and
// a frame of RAM address 0 then 32 bytes, two per row. M5's wiring puts
// pixel x of a byte (0 the leftmost) at bit (x + 4) % 8. The chip doesn't
// answer reads.

#include "tm1680.h"

#include <stdlib.h>
#include <string.h>
#include <sys/cdefs.h>

#include "esp_check.h"
#include "esp_lcd_panel_interface.h"

static const char *TAG = "tm1680";

#define TIMEOUT_MS 50
#define SIZE       16

typedef struct {
    esp_lcd_panel_t base;
    i2c_master_dev_handle_t dev;
    uint8_t ram[1 + 2 * SIZE];   // the RAM address (0), then the frame
} tm1680_t;

static esp_err_t send(tm1680_t *tm, const uint8_t *bytes, size_t len) {
    return i2c_master_transmit(tm->dev, bytes, len, TIMEOUT_MS);
}

static esp_err_t tm1680_init(esp_lcd_panel_t *panel) {
    tm1680_t *tm = __containerof(panel, tm1680_t, base);
    // System off, 24 rows x 16 commons NMOS, internal clock, system on,
    // 2/16 brightness (M5's), LEDs on.
    static const uint8_t start[] = {0x80, 0xa4, 0x98, 0x81, 0xb1, 0x83};
    ESP_RETURN_ON_ERROR(send(tm, start, sizeof(start)), TAG, "start");
    // The RAM keeps its last image over a restart.
    memset(tm->ram, 0, sizeof(tm->ram));
    return send(tm, tm->ram, sizeof(tm->ram));
}

static esp_err_t tm1680_draw_bitmap(esp_lcd_panel_t *panel, int x_start, int y_start,
                                    int x_end, int y_end, const void *color_data) {
    tm1680_t *tm = __containerof(panel, tm1680_t, base);
    ESP_RETURN_ON_FALSE(color_data && x_start >= 0 && y_start >= 0 && x_end <= SIZE &&
                        y_end <= SIZE, ESP_ERR_INVALID_ARG, TAG, "outside the matrix");
    const uint8_t *src = color_data;
    int stride = (x_end - x_start + 7) / 8;
    for (int y = y_start; y < y_end; y++, src += stride) {
        for (int x = x_start; x < x_end; x++) {
            int i = x - x_start;
            uint8_t *byte = &tm->ram[1 + 2 * y + x / 8];
            uint8_t bit = (uint8_t)(1u << (x % 8 + 4) % 8);
            if (src[i / 8] & (0x80 >> (i % 8))) {
                *byte |= bit;
            } else {
                *byte &= (uint8_t)~bit;
            }
        }
    }
    return send(tm, tm->ram, sizeof(tm->ram));
}

static esp_err_t tm1680_del(esp_lcd_panel_t *panel) {
    tm1680_t *tm = __containerof(panel, tm1680_t, base);
    i2c_master_bus_rm_device(tm->dev);
    free(tm);
    return ESP_OK;
}

esp_err_t tm1680_new_panel(i2c_master_bus_handle_t bus, uint16_t address,
                           esp_lcd_panel_handle_t *panel) {
    ESP_RETURN_ON_FALSE(bus && panel, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    tm1680_t *tm = calloc(1, sizeof(*tm));
    ESP_RETURN_ON_FALSE(tm, ESP_ERR_NO_MEM, TAG, "no memory");
    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = address,
        .scl_speed_hz = 100000,
    };
    esp_err_t err = i2c_master_bus_add_device(bus, &cfg, &tm->dev);
    if (err != ESP_OK) {
        free(tm);
        return err;
    }
    tm->base.init = tm1680_init;
    tm->base.del = tm1680_del;
    tm->base.draw_bitmap = tm1680_draw_bitmap;
    *panel = &tm->base;
    return ESP_OK;
}
