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

/* Exercise the production reader against a little-endian I2C register file. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "boards/waveshare_s3_185b_power.h"

struct fake_i2c_dev { int unused; };
static struct fake_i2c_dev gauge;
static uint16_t words[256];
static int fail_reg, reads;

esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t dev,
                                    const uint8_t *wbuf, size_t wlen,
                                    uint8_t *rbuf, size_t rlen, int timeout_ms)
{
    assert(dev == &gauge && wlen == 1 && rlen == 2 && timeout_ms == 100);
    const uint8_t reg = wbuf[0];
    assert(reg == 0x0a || reg == 0x3a || reg == 0x08 || reg == 0x2c || reg == 0x0c);
    ++reads;
    if (reg == fail_reg) return ESP_ERR_TIMEOUT;
    rbuf[0] = words[reg] & 0xff;
    rbuf[1] = words[reg] >> 8;
    return ESP_OK;
}

/* Gauge programming is deliberately unsupported, including in the test. */
esp_err_t i2c_master_transmit(i2c_master_dev_handle_t dev, const uint8_t *buf,
                             size_t len, int timeout_ms)
{
    (void)dev; (void)buf; (void)len; (void)timeout_ms;
    abort();
}

int main(void)
{
    unsigned status, operation, mv, soc, current;
    int usb, attached;
    muse_power_t out = { .battery_pct = 99, .battery_mv = 4200, .charging = true, .usb = true };
    while (scanf("%u %u %u %u %u %d %d %d", &status, &operation, &mv, &soc,
                 &current, &fail_reg, &usb, &attached) == 8) {
        words[0x0a] = status;
        words[0x3a] = operation;
        words[0x08] = mv;
        words[0x2c] = soc;
        words[0x0c] = current;
        reads = 0;
        esp_err_t err = waveshare_185b_read_power(attached ? &gauge : NULL, usb, &out);
        printf("%d %d %d %d %d %d\n", err, out.battery_pct, out.battery_mv,
               out.charging, out.usb, reads);
    }
    return 0;
}
