/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* AXS5106L register 0x01: status, count, then two six-byte points.
 * JD9853A portrait mode uses MADCTL=0x48, so mirror touch X with the panel.
 * See Seeed_GFX2/src/touch/Touch_AXS5106L.cpp. */
static inline bool xiao_147_touch_decode(const uint8_t *report, size_t length,
                                       uint16_t *x, uint16_t *y)
{
    if (!report || length != 14 || !x || !y || report[1] < 1 || report[1] > 2) {
        return false;
    }
    unsigned raw_x = ((report[2] & 0x0f) << 8) | report[3];
    unsigned raw_y = ((report[4] & 0x0f) << 8) | report[5];
    if (raw_x >= 172 || raw_y >= 320) {
        return false;
    }
    *x = 171 - raw_x;
    *y = raw_y;
    return true;
}
