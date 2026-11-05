/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include "driver/i2c_master.h"
#include "muse_state.h"

/* BQ27220 standard, read-only commands, little-endian words. See TI SLUUBD4A,
 * sections 2.6-2.8, 2.21 and 2.27, and Waveshare's bsp_battery_get_data():
 * https://www.ti.com/lit/ug/sluubd4a/sluubd4a.pdf
 * No control commands, unsealing, calibration or capacity writes here.
 */
static inline esp_err_t waveshare_185b_gauge_word(i2c_master_dev_handle_t gauge,
                                               uint8_t reg, uint16_t *value)
{
    uint8_t data[2];
    esp_err_t err = i2c_master_transmit_receive(gauge, &reg, 1, data, sizeof(data), 100);
    if (err == ESP_OK) {
        *value = (uint16_t)data[0] | (uint16_t)data[1] << 8;
    }
    return err;
}

static inline esp_err_t waveshare_185b_read_power(i2c_master_dev_handle_t gauge,
                                               bool usb, muse_power_t *out)
{
    /* A failed/partial read must clear the previous sample, never become 0%.
     * Keep USB detection independent of the gauge so unplugging still works.
     */
    *out = (muse_power_t){ .battery_pct = -1, .usb = usb };
    if (!gauge) return ESP_ERR_INVALID_STATE;
    uint16_t status, operation, mv, soc, current;
    esp_err_t err = waveshare_185b_gauge_word(gauge, 0x0a, &status);
    if (err != ESP_OK) return err;
    if (!(status & (1u << 3))) return ESP_OK;  /* BATTPRES */
    err = waveshare_185b_gauge_word(gauge, 0x3a, &operation);
    if (err != ESP_OK) return err;
    if (!(operation & (1u << 5)) || (operation & (1u << 10))) {
        return ESP_ERR_INVALID_STATE;  /* not INITCOMP, or CFGUPDATE */
    }
    if ((err = waveshare_185b_gauge_word(gauge, 0x08, &mv)) != ESP_OK ||
        (err = waveshare_185b_gauge_word(gauge, 0x2c, &soc)) != ESP_OK ||
        (err = waveshare_185b_gauge_word(gauge, 0x0c, &current)) != ESP_OK) {
        return err;
    }
    if (soc > 100 || mv < 2000 || mv > 5000) return ESP_ERR_INVALID_RESPONSE;
    const int ma = current < 0x8000 ? (int)current : (int)current - 65536;
    out->battery_pct = soc;
    out->battery_mv = mv;
    /* DSG clear alone also means relaxation; it does not prove charging. */
    out->charging = ma > 0 && !(status & 1u);
    return ESP_OK;
}
