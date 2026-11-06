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

/* The AXP2101 driver (muse_pmu.c) against a fake register file on a fake I2C
 * bus, for test_muse_pmu.py: one case per run, by its number. */
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "muse_pmu.c"

#define COMMON_CFG 0x10
#define PWROFF_EN 0x22
#define IRQ_LEVEL 0x27

struct fake_i2c_bus { int unused; };
struct fake_i2c_dev { uint16_t addr; };

static struct fake_i2c_bus s_bus;
static struct fake_i2c_dev s_axp;
static uint8_t s_regs[256];
static int s_writes[256];      /* writes per register */
static int s_reads;
static int s_fail_read = -1, s_fail_write = -1;
static esp_err_t s_add_err;

esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t bus, const i2c_device_config_t *cfg,
                                    i2c_master_dev_handle_t *dev)
{
    assert(bus == &s_bus && cfg->device_address == 0x34);
    if (s_add_err) {
        return s_add_err;
    }
    s_axp.addr = cfg->device_address;
    *dev = &s_axp;
    return ESP_OK;
}

esp_err_t i2c_master_transmit(i2c_master_dev_handle_t dev, const uint8_t *buf, size_t len, int timeout_ms)
{
    assert(dev == &s_axp && len == 2 && timeout_ms > 0);
    if (buf[0] == s_fail_write) {
        return ESP_ERR_TIMEOUT;
    }
    s_regs[buf[0]] = buf[1];
    s_writes[buf[0]]++;
    return ESP_OK;
}

esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t dev, const uint8_t *wbuf, size_t wlen, uint8_t *rbuf,
                                      size_t rlen, int timeout_ms)
{
    assert(dev == &s_axp && wlen == 1 && rlen == 1 && timeout_ms > 0);
    s_reads++;
    if (wbuf[0] == s_fail_read) {
        return ESP_ERR_TIMEOUT;
    }
    *rbuf = s_regs[wbuf[0]];
    return ESP_OK;
}

static int total_writes(void)
{
    int n = 0;
    for (int i = 0; i < 256; i++) {
        n += s_writes[i];
    }
    return n;
}

/* Init: 10 s hold-off in REG27[3:2], every other bit kept. It leaves REG22
 * and REG10 as they are: boards that don't talk on PWR keep the PMU's own
 * key shutdowns, and those that do turn them on themselves. */
static void init_sets_hold_off_only(void)
{
    s_regs[IRQ_LEVEL] = 0x31;    /* IRQLEVEL 2.5 s, OFFLEVEL 4 s, ONLEVEL 512 ms */
    s_regs[PWROFF_EN] = 0x05;    /* over-temperature on, restart; key shutdown off */
    s_regs[COMMON_CFG] = 0x34;   /* 16 s shutdown on */
    assert(muse_pmu_init(&s_bus, true) == ESP_OK);
    assert(s_regs[IRQ_LEVEL] == 0x3D);
    assert(s_regs[PWROFF_EN] == 0x05 && s_writes[PWROFF_EN] == 0);
    assert(s_regs[COMMON_CFG] == 0x34 && s_writes[COMMON_CFG] == 0);
    assert(muse_pmu_init(&s_bus, false) == ESP_OK);
    assert(s_writes[PWROFF_EN] == 0 && s_writes[COMMON_CFG] == 0);
}

/* The setter changes REG22[1] and nothing else. */
static void setter_changes_only_bit1(void)
{
    assert(muse_pmu_init(&s_bus, true) == ESP_OK);
    for (int start = 0; start < 8; start++) {
        memset(s_writes, 0, sizeof(s_writes));
        s_regs[PWROFF_EN] = (uint8_t)start;
        assert(muse_pmu_set_key_shutdown(false) == ESP_OK);
        assert(s_regs[PWROFF_EN] == (start & ~0x02));
        assert(muse_pmu_set_key_shutdown(true) == ESP_OK);
        assert(s_regs[PWROFF_EN] == (start | 0x02));
        assert(s_writes[PWROFF_EN] == 2 && total_writes() == 2);
    }
}

/* A failed read writes nothing; a failed read or write is returned. */
static void errors_are_returned(void)
{
    assert(muse_pmu_init(&s_bus, true) == ESP_OK);
    s_regs[PWROFF_EN] = 0x07;
    memset(s_writes, 0, sizeof(s_writes));
    s_fail_read = PWROFF_EN;
    assert(muse_pmu_set_key_shutdown(false) == ESP_ERR_TIMEOUT);
    assert(total_writes() == 0 && s_regs[PWROFF_EN] == 0x07);
    s_fail_read = -1;
    s_fail_write = PWROFF_EN;
    assert(muse_pmu_set_key_shutdown(false) == ESP_ERR_TIMEOUT);
    assert(s_regs[PWROFF_EN] == 0x07);
    /* Init doesn't touch REG22, so a PMU that won't take it still comes up
     * (keys, battery, rails); the board's own turn-on reports the failure. */
    assert(muse_pmu_init(&s_bus, true) == ESP_OK);
    s_fail_write = -1;
    s_fail_read = PWROFF_EN;
    assert(muse_pmu_init(&s_bus, true) == ESP_OK);
}

/* REG10[2], the 16 s shutdown: cleared on PWR's press only if it was on, put
 * back at the release, every other bit kept and the self-clearing command
 * bits (restart, power off) never written as 1. */
static void sixteen_second_shutdown_follows_the_hold(void)
{
    assert(muse_pmu_init(&s_bus, false) == ESP_OK);
    s_regs[PWROFF_EN] = 0x06;
    s_regs[COMMON_CFG] = 0x37;   /* 16 s on; bits 1:0 as if they read back set */
    assert(muse_pmu_set_key_shutdown(false) == ESP_OK);
    assert(s_regs[PWROFF_EN] == 0x04 && s_regs[COMMON_CFG] == 0x30);
    /* A second press (a missed release) keeps what to put back. */
    assert(muse_pmu_set_key_shutdown(false) == ESP_OK);
    assert(s_writes[COMMON_CFG] == 1);
    s_regs[COMMON_CFG] = 0x10;   /* something else changed bit 5 meanwhile: kept */
    assert(muse_pmu_set_key_shutdown(true) == ESP_OK);
    assert(s_regs[PWROFF_EN] == 0x06 && s_regs[COMMON_CFG] == 0x14 && s_writes[COMMON_CFG] == 2);
    /* Released again: REG10 was put back, so it's left alone. */
    assert(muse_pmu_set_key_shutdown(true) == ESP_OK && s_writes[COMMON_CFG] == 2);

    /* Off, as the datasheet has it: never written at all. */
    s_regs[COMMON_CFG] = 0x30;
    assert(muse_pmu_set_key_shutdown(false) == ESP_OK);
    assert(muse_pmu_set_key_shutdown(true) == ESP_OK);
    assert(s_regs[COMMON_CFG] == 0x30 && s_writes[COMMON_CFG] == 2);
}

/* Each shutdown is tried even if the other can't be, and a 16 s shutdown that
 * couldn't be put back is tried again at the next release. */
static void sixteen_second_errors(void)
{
    assert(muse_pmu_init(&s_bus, false) == ESP_OK);
    s_regs[PWROFF_EN] = 0x06;
    s_regs[COMMON_CFG] = 0x34;
    s_fail_write = PWROFF_EN;
    assert(muse_pmu_set_key_shutdown(false) == ESP_ERR_TIMEOUT);
    assert(s_regs[PWROFF_EN] == 0x06 && s_regs[COMMON_CFG] == 0x30);
    s_fail_write = COMMON_CFG;
    assert(muse_pmu_set_key_shutdown(true) == ESP_ERR_TIMEOUT);
    assert(s_regs[PWROFF_EN] == 0x06 && s_regs[COMMON_CFG] == 0x30);
    s_fail_write = -1;
    assert(muse_pmu_set_key_shutdown(true) == ESP_OK);
    assert(s_regs[COMMON_CFG] == 0x34);
    /* An unreadable REG10 is left alone; REG22 still goes off. */
    s_fail_read = COMMON_CFG;
    int before = s_writes[COMMON_CFG];
    assert(muse_pmu_set_key_shutdown(false) == ESP_ERR_TIMEOUT);
    assert(s_regs[PWROFF_EN] == 0x04 && s_writes[COMMON_CFG] == before);
    s_fail_read = -1;
    assert(muse_pmu_set_key_shutdown(true) == ESP_OK && s_regs[COMMON_CFG] == 0x34);
}

/* Without a PMU, nothing goes on the bus. */
static void no_pmu(void)
{
    assert(muse_pmu_set_key_shutdown(false) == ESP_ERR_INVALID_STATE);
    assert(muse_pmu_set_key_shutdown(true) == ESP_ERR_INVALID_STATE);
    assert(s_reads == 0 && total_writes() == 0);
    s_add_err = ESP_FAIL;
    assert(muse_pmu_init(&s_bus, true) == ESP_FAIL);
    assert(muse_pmu_set_key_shutdown(true) == ESP_ERR_INVALID_STATE);
    assert(s_reads == 0 && total_writes() == 0);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    switch (atoi(argv[1])) {
    case 0: init_sets_hold_off_only(); break;
    case 1: setter_changes_only_bit1(); break;
    case 2: errors_are_returned(); break;
    case 3: no_pmu(); break;
    case 4: sixteen_second_shutdown_follows_the_hold(); break;
    case 5: sixteen_second_errors(); break;
    default: return 2;
    }
    return 0;
}
