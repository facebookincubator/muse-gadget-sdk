/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * Copyright (c) 2026 SHENZHEN VIEWE TECHNOLOGY CO.,LTD.
 * SPDX-License-Identifier: Apache-2.0
 *
 * VIEWE SmartRing-Plus battery arithmetic, kept free of ESP-IDF so the host
 * tests compile it. Values follow VIEWE's smartring_plus BSP 1.1.2: a
 * 3.7 V / 600 mAh cell, read through GPIO1 on V1 boards and an AXP2101 on V2
 * (charged as set here, read through muse_pmu.c).
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

/* AXP2101 (V2), I2C 0x34. Charging at 0.5C for the 600 mAh cell. */
#define SMARTRING_AXP_ADDR       0x34
#define SMARTRING_AXP_CHIP_ID    0x4A    /* REG03 */
#define SMARTRING_AXP_ALDO3_CODE 28      /* REG94: (3300 - 500) / 100 mV, the V2 peripheral rail */
#define SMARTRING_AXP_CV_CODE    0x03    /* REG64: 4.2 V */
#define SMARTRING_AXP_IPRE_CODE  3       /* REG61: 25 mA steps, 75 mA */
#define SMARTRING_AXP_ICC_CODE   9       /* REG62: 200 + 100 * (9 - 8) = 300 mA */
#define SMARTRING_AXP_ITERM_CODE 0x11    /* REG63: enabled, 25 mA */

/* V1: GPIO1 reads the VCC_OUT rail through a 1:2 divider. With USB in, the
 * rail rises above the cell; above 4.25 V it's charging, below 4.15 V not,
 * each confirmed by two readings in a row. */
#define SMARTRING_V1_DIVIDER     2
#define SMARTRING_V1_CHG_ENTER   4250
#define SMARTRING_V1_CHG_EXIT    4150
#define SMARTRING_V1_CHG_DROP    300     /* rail above the cell while charging */
#define SMARTRING_V1_VALID_MIN   2800

/* Open-circuit voltage to charge for the 3.7 V / 600 mAh cell, light load. */
static inline int smartring_ocv_pct(int mv)
{
    static const int16_t pts[][2] = {
        {3300, 0},  {3400, 5},  {3500, 10}, {3550, 15}, {3600, 20}, {3650, 28},
        {3700, 40}, {3750, 48}, {3800, 55}, {3850, 62}, {3900, 70}, {3950, 76},
        {4000, 82}, {4050, 88}, {4100, 93}, {4150, 97}, {4200, 100},
    };
    const int n = sizeof(pts) / sizeof(pts[0]);
    if (mv <= pts[0][0]) {
        return 0;
    }
    for (int i = 1; i < n; i++) {
        if (mv <= pts[i][0]) {
            int dv = pts[i][0] - pts[i - 1][0], dp = pts[i][1] - pts[i - 1][1];
            return pts[i - 1][1] + ((mv - pts[i - 1][0]) * dp + dv / 2) / dv;
        }
    }
    return 100;
}

typedef struct {
    bool charging;
    int streak;
} smartring_v1_t;

/* One V1 reading of the rail (after the divider): the cell's estimated mV and
 * percent, and whether USB is charging it. False while the ADC isn't valid
 * yet; an invalid reading leaves the charging confirmation where it was. */
static inline bool smartring_v1_update(smartring_v1_t *s, int rail_mv, int *mv, int *pct, bool *charging)
{
    if (rail_mv < SMARTRING_V1_VALID_MIN) {
        return false;
    }
    bool want = s->charging ? rail_mv >= SMARTRING_V1_CHG_EXIT : rail_mv > SMARTRING_V1_CHG_ENTER;
    if (want != s->charging) {
        if (++s->streak >= 2) {
            s->charging = want;
            s->streak = 0;
        }
    } else {
        s->streak = 0;
    }
    int cell = s->charging || rail_mv > SMARTRING_V1_CHG_ENTER ? rail_mv - SMARTRING_V1_CHG_DROP : rail_mv;
    *mv = cell;
    *pct = smartring_ocv_pct(cell);
    *charging = s->charging;
    return true;
}
