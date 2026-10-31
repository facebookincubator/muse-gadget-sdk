/* Copyright (c) Meta Platforms, Inc. and affiliates. SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

/* CST816S Register2 (single-finger): point count, X high/low, Y high/low, trailing byte.
 * Source: CST816S datasheet + VIEWE BSP verification. */
static inline bool viewe_smartring_touch_decode(const uint8_t raw[6], uint16_t *x, uint16_t *y)
{
    if ((raw[0] & 15) != 1)
        return false; /* Not single-touch */
    uint16_t tx = ((raw[1] & 15) << 8) | raw[2];
    uint16_t ty = ((raw[3] & 15) << 8) | raw[4];
    if (tx >= 360 || ty >= 360)
        return false;
    *x = tx;
    *y = ty;
    return true;
}
