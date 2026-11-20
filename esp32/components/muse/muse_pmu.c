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

#include "muse_pmu.h"

#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "muse_pmu";

#define AXP2101_ADDR 0x34

#define REG_STATUS1 0x00        /* bit5 VBUS good, bit3 battery present */
#define REG_STATUS2 0x01        /* bits[6:5] 01 = charging */
#define REG_COMMON_CFG 0x10     /* bit0 = soft power-off, bit2 = shut down after 16 s of PWR */
#define REG_PWROFF_STATUS 0x21  /* what powered it off last */
#define REG_PWROFF_EN 0x22      /* bit1 = power off when PWR is held past the hold-off */
#define REG_IRQ_LEVEL 0x27      /* bits[3:2] power-key hold-to-off time */
#define REG_ADC_ENABLE 0x30     /* bit0 = battery voltage */
#define REG_VBAT_H 0x34         /* bits[4:0]; 1 mV per count with REG_VBAT_L */
#define REG_VBAT_L 0x35
#define REG_INTEN1 0x40        /* three enable registers, then three status */
#define REG_INTEN2 0x41
#define REG_INTSTS1 0x48
#define REG_INTSTS2 0x49
#define REG_DCDC_ONOFF 0x80     /* bits[4:0] = DCDC5..1 */
#define REG_LDO_ONOFF0 0x90     /* ALDO1-4, BLDO1-2, CPUSLDO, DLDO1 from bit0 */
#define REG_LDO_ONOFF1 0x91     /* bit0 = DLDO2 */
#define REG_BAT_PERCENT 0xA4

/* INTEN2 / INTSTS2 power-key bits. */
#define PKEY_POS_EDGE (1u << 0) /* released */
#define PKEY_NEG_EDGE (1u << 1) /* pressed */
#define PKEY_LONG (1u << 2)
#define PKEY_SHORT (1u << 3)
#define PKEY_ALL (PKEY_POS_EDGE | PKEY_NEG_EDGE | PKEY_LONG | PKEY_SHORT)

/* bits[3:2] = 11 -> 10 s, the longest: a hardware fallback if the firmware is
 * wedged. A talk can run longer (MAX_SECS, muse_voice.c), so boards that talk
 * on PWR keep the hold from powering off while PWR is down
 * (muse_pmu_set_key_shutdown). */
#define PKEY_OFF_10S (3u << 2)
#define PWROFF_KEY (1u << 1)
/* REG10: "PWRON 16s to shut the PMIC". The datasheet has it off by default,
 * but a 1.75C's PMU had it on (its own setup or earlier firmware). Bits
 * 1:0 are commands that clear themselves (restart, power off): never write a 1
 * there. */
#define CFG_PWRON_16S (1u << 2)
#define CFG_COMMANDS 0x03u

static i2c_master_dev_handle_t s_dev;
static bool s_16s_cleared;   /* REG10[2] was on, and the hold in progress cleared it */

static esp_err_t rd(uint8_t reg, uint8_t *val)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, val, 1, 50);
}

static esp_err_t wr(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(s_dev, buf, sizeof(buf), 50);
}

esp_err_t muse_pmu_init(i2c_master_bus_handle_t bus, bool key_irqs)
{
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = AXP2101_ADDR,
        .scl_speed_hz = 400000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &cfg, &s_dev), TAG, "add device");

    uint8_t v;
    ESP_RETURN_ON_ERROR(rd(REG_IRQ_LEVEL, &v), TAG, "AXP2101 not responding");
    ESP_RETURN_ON_ERROR(wr(REG_IRQ_LEVEL, (v & ~0x0C) | PKEY_OFF_10S), TAG, "set off time");

    if (key_irqs) {
        ESP_RETURN_ON_ERROR(rd(REG_INTEN2, &v), TAG, "read inten2");
        ESP_RETURN_ON_ERROR(wr(REG_INTEN2, v | PKEY_ALL), TAG, "enable key irqs");
        ESP_RETURN_ON_ERROR(wr(REG_INTSTS2, PKEY_ALL), TAG, "clear key irqs");
    } else {
        /* Nothing reads them, and a pending one holds the IRQ line low
         * against its pull-up. */
        uint8_t en[3] = { 0 }, sts[3] = { 0 };
        for (int i = 0; i < 3; i++) {
            rd(REG_INTEN1 + i, &en[i]);
            rd(REG_INTSTS1 + i, &sts[i]);
            ESP_RETURN_ON_ERROR(wr(REG_INTEN1 + i, 0), TAG, "disable irqs");
            ESP_RETURN_ON_ERROR(wr(REG_INTSTS1 + i, 0xFF), TAG, "clear irqs");
        }
        ESP_LOGI(TAG, "IRQs off (were enabled %02x %02x %02x, pending %02x %02x %02x)", en[0], en[1], en[2],
                 sts[0], sts[1], sts[2]);
    }

    ESP_RETURN_ON_ERROR(rd(REG_ADC_ENABLE, &v), TAG, "read adc enable");
    ESP_RETURN_ON_ERROR(wr(REG_ADC_ENABLE, v | 0x01), TAG, "enable battery voltage");

    uint8_t common = 0, en = 0, off = 0;
    rd(REG_COMMON_CFG, &common);
    rd(REG_PWROFF_EN, &en);
    rd(REG_PWROFF_STATUS, &off);
    ESP_LOGI(TAG, "AXP2101 ready (hardware PWR hold-off 10 s, COMMON_CFG %02x, PWROFF_EN %02x, last off %02x)",
             common, en, off);
    return ESP_OK;
}

esp_err_t muse_pmu_keep_rails(uint8_t dcdc, uint16_t ldo)
{
    uint8_t dc, ldo0, ldo1;
    ESP_RETURN_ON_FALSE(s_dev, ESP_ERR_INVALID_STATE, TAG, "no PMU");
    ESP_RETURN_ON_ERROR(rd(REG_DCDC_ONOFF, &dc), TAG, "read dcdc");
    ESP_RETURN_ON_ERROR(rd(REG_LDO_ONOFF0, &ldo0), TAG, "read ldo0");
    ESP_RETURN_ON_ERROR(rd(REG_LDO_ONOFF1, &ldo1), TAG, "read ldo1");
    /* The DCDC register's top bits aren't enables; leave them. */
    uint8_t dc_new = dc & (0xE0 | (dcdc & 0x1F));
    uint8_t ldo0_new = ldo0 & (ldo & 0xFF);
    uint8_t ldo1_new = ldo & 0x100 ? ldo1 : ldo1 & ~0x01;
    ESP_RETURN_ON_ERROR(wr(REG_DCDC_ONOFF, dc_new), TAG, "dcdc off");
    ESP_RETURN_ON_ERROR(wr(REG_LDO_ONOFF0, ldo0_new), TAG, "ldo0 off");
    ESP_RETURN_ON_ERROR(wr(REG_LDO_ONOFF1, ldo1_new), TAG, "ldo1 off");
    ESP_LOGI(TAG, "rails: DCDC %02x -> %02x, LDO %02x %02x -> %02x %02x", dc & 0x1F, dc_new & 0x1F, ldo0,
             ldo1 & 0x01, ldo0_new, ldo1_new & 0x01);
    return ESP_OK;
}

esp_err_t muse_pmu_set_key_shutdown(bool on)
{
    uint8_t v = 0;
    if (!s_dev) {
        return ESP_ERR_INVALID_STATE;
    }
    /* Both shutdowns count the physical hold, so each is tried even if the
     * other fails. REG22[1]: off past OFFLEVEL. */
    esp_err_t err = rd(REG_PWROFF_EN, &v);
    if (err == ESP_OK) {
        err = wr(REG_PWROFF_EN, on ? v | PWROFF_KEY : v & ~PWROFF_KEY);
    }
    /* REG10[2]: cleared for the hold only if it was on, and put back after;
     * a hold already cleared it if s_16s_cleared. */
    esp_err_t err16 = ESP_OK;
    if (on == s_16s_cleared) {
        err16 = rd(REG_COMMON_CFG, &v);
        v &= ~CFG_COMMANDS;
        if (err16 == ESP_OK && on) {
            err16 = wr(REG_COMMON_CFG, v | CFG_PWRON_16S);
            s_16s_cleared = err16 != ESP_OK;   /* not put back: try again next time */
        } else if (err16 == ESP_OK && (v & CFG_PWRON_16S)) {
            err16 = wr(REG_COMMON_CFG, v & ~CFG_PWRON_16S);
            s_16s_cleared = err16 == ESP_OK;
        }
    }
    if (err != ESP_OK || err16 != ESP_OK) {
        ESP_LOGW(TAG, "key shutdown %s: %s", on ? "on" : "off", esp_err_to_name(err != ESP_OK ? err : err16));
    }
    return err != ESP_OK ? err : err16;
}

unsigned muse_pmu_poll_key(void)
{
    uint8_t sts;
    if (!s_dev || rd(REG_INTSTS2, &sts) != ESP_OK) {
        return 0;
    }
    sts &= PKEY_ALL;
    if (!sts) {
        return 0;
    }
    wr(REG_INTSTS2, sts);   /* write-1-to-clear */

    unsigned ev = 0;
    if (sts & PKEY_NEG_EDGE) {
        ev |= MUSE_PMU_KEY_PRESS;
    }
    if (sts & PKEY_POS_EDGE) {
        ev |= MUSE_PMU_KEY_RELEASE;
    }
    if (sts & PKEY_SHORT) {
        ev |= MUSE_PMU_KEY_CLICK;
    }
    if (sts & PKEY_LONG) {
        ev |= MUSE_PMU_KEY_LONG;
    }
    return ev;
}

esp_err_t muse_pmu_read_power(muse_power_t *out)
{
    uint8_t s1, s2, pct, hi, lo;
    if (!s_dev) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_RETURN_ON_ERROR(rd(REG_STATUS1, &s1), TAG, "status1");
    ESP_RETURN_ON_ERROR(rd(REG_STATUS2, &s2), TAG, "status2");
    bool battery = s1 & (1u << 3);
    out->usb = s1 & (1u << 5);
    out->charging = battery && ((s2 >> 5) & 0x3) == 0x1;
    out->battery_pct = -1;
    out->battery_mv = 0;
    if (battery && rd(REG_BAT_PERCENT, &pct) == ESP_OK && pct <= 100) {
        out->battery_pct = pct;
    }
    if (battery && rd(REG_VBAT_H, &hi) == ESP_OK && rd(REG_VBAT_L, &lo) == ESP_OK) {
        out->battery_mv = (hi & 0x1F) << 8 | lo;
    }
    return ESP_OK;
}

esp_err_t muse_pmu_power_off(void)
{
    uint8_t v;
    if (!s_dev) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_RETURN_ON_ERROR(rd(REG_COMMON_CFG, &v), TAG, "read common cfg");
    ESP_LOGI(TAG, "powering off");
    return wr(REG_COMMON_CFG, v | 0x01);
}
