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
#pragma once
#include <stdint.h>

/* Exact floor(sqrt(n)), without the software floating point on the C6.
 * Ring dimensions are small integers. Restoring square root needs no divide,
 * floating-point conversion or libm call, even for the fallback geometry. */
static inline uint32_t muse_ring_isqrt(uint32_t n)
{
    uint32_t root = 0, bit = UINT32_C(1) << 30;
    while (bit > n) bit >>= 2;
    while (bit) {
        if (n >= root + bit) {
            n -= root + bit;
            root = (root >> 1) + bit;
        } else {
            root >>= 1;
        }
        bit >>= 2;
    }
    return root;
}

#if CONFIG_MUSE_BOARD_WAVESHARE_C6_18
/* Fixed C6 bezel: diameter 360, width 6. These 355 bytes live in flash, not
 * the audio/offline-clip heap. Entries are ceil(sqrt(181^2-y^2)) and
 * floor(sqrt(172^2-y^2)); tests regenerate every entry using integer math.
 * Changed ring geometry takes the integer fallback rather than stale data. */
static const uint8_t s_ring_outer_181[182] = {
    181, 181, 181, 181, 181, 181, 181, 181, 181, 181, 181, 181, 181, 181, 181, 181, 181, 181, 181, 180,
    180, 180, 180, 180, 180, 180, 180, 179, 179, 179, 179, 179, 179, 178, 178, 178, 178, 178, 177, 177,
    177, 177, 177, 176, 176, 176, 176, 175, 175, 175, 174, 174, 174, 174, 173, 173, 173, 172, 172, 172,
    171, 171, 171, 170, 170, 169, 169, 169, 168, 168, 167, 167, 167, 166, 166, 165, 165, 164, 164, 163,
    163, 162, 162, 161, 161, 160, 160, 159, 159, 158, 158, 157, 156, 156, 155, 155, 154, 153, 153, 152,
    151, 151, 150, 149, 149, 148, 147, 146, 146, 145, 144, 143, 143, 142, 141, 140, 139, 139, 138, 137,
    136, 135, 134, 133, 132, 131, 130, 129, 128, 127, 126, 125, 124, 123, 122, 121, 120, 119, 118, 116,
    115, 114, 113, 111, 110, 109, 107, 106, 105, 103, 102, 100, 99, 97, 96, 94, 92, 91, 89, 87,
    85, 83, 81, 79, 77, 75, 73, 70, 68, 65, 63, 60, 57, 54, 50, 47, 43, 38, 33, 27, 19, 0,
};
static const uint8_t s_ring_inner_172[173] = {
    172, 171, 171, 171, 171, 171, 171, 171, 171, 171, 171, 171, 171, 171, 171, 171, 171, 171, 171, 170,
    170, 170, 170, 170, 170, 170, 170, 169, 169, 169, 169, 169, 168, 168, 168, 168, 168, 167, 167, 167,
    167, 167, 166, 166, 166, 166, 165, 165, 165, 164, 164, 164, 163, 163, 163, 162, 162, 162, 161, 161,
    161, 160, 160, 160, 159, 159, 158, 158, 157, 157, 157, 156, 156, 155, 155, 154, 154, 153, 153, 152,
    152, 151, 151, 150, 150, 149, 148, 148, 147, 147, 146, 145, 145, 144, 144, 143, 142, 142, 141, 140,
    139, 139, 138, 137, 136, 136, 135, 134, 133, 133, 132, 131, 130, 129, 128, 127, 126, 126, 125, 124,
    123, 122, 121, 120, 119, 118, 117, 115, 114, 113, 112, 111, 110, 109, 107, 106, 105, 103, 102, 101,
    99, 98, 97, 95, 94, 92, 90, 89, 87, 85, 84, 82, 80, 78, 76, 74, 72, 70, 67, 65,
    63, 60, 57, 54, 51, 48, 45, 41, 36, 31, 26, 18, 0,
};
#endif

static inline int32_t muse_ring_outer_width(int32_t radius, int32_t distance)
{
    if (distance < 0 || distance >= radius) return 0;
#if CONFIG_MUSE_BOARD_WAVESHARE_C6_18
    if (radius == 181) return s_ring_outer_181[distance];
#endif
    uint32_t n = (uint32_t)(radius * radius - distance * distance);
    uint32_t r = muse_ring_isqrt(n);
    return (int32_t)(r + (r * r != n));
}

static inline int32_t muse_ring_inner_width(int32_t radius, int32_t distance)
{
    if (distance < 0 || distance >= radius) return 0;
#if CONFIG_MUSE_BOARD_WAVESHARE_C6_18
    if (radius == 172) return s_ring_inner_172[distance];
#endif
    return (int32_t)muse_ring_isqrt((uint32_t)(radius * radius - distance * distance));
}
