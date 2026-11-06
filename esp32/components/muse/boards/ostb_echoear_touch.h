/* Copyright (c) 2026 Mihir Jadhav. SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Register2: single-finger count, X high/low, Y high/low, trailing byte. */
static inline bool ostb_echoear_touch_decode(const uint8_t raw[6], uint16_t *x, uint16_t *y)
{
    if ((raw[0] & 15) != 1) return false;
    uint16_t tx = ((raw[1] & 15) << 8) | raw[2];
    uint16_t ty = ((raw[3] & 15) << 8) | raw[4];
    if (tx >= 360 || ty >= 360) return false;
    *x = tx;
    *y = ty;
    return true;
}
