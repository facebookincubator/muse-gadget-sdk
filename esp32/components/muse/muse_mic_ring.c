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

#include "muse_mic_ring.h"

#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>

static int s_ring_w, s_ring_h, s_ring_px;
static bool s_ring_round;
static volatile int s_ring_corner;     /* a rectangle's visible corner radius (the console can tune it) */
static atomic_bool s_ring_on;
static atomic_uint s_ring_gen;
static void (*volatile s_ring_listener)(bool on);
/* Each row's hole [i0, i1), worked out once per shape (PERFORMANCE.md: no
 * sqrt per row per frame): 2 x height int16, -1 = no hole (all ring). NULL
 * if it couldn't be allocated: computed per row instead. */
static int16_t *s_rows;
static void build_rows(void);

int muse_mic_ring_width(int width, int height)
{
    int s = width < height ? width : height;
    int w = (int)lroundf(s * 0.022f);
    return w < 2 ? 2 : w;
}

void muse_mic_ring_init(int width, int height, bool round, int corner_px)
{
    s_ring_w = width;
    s_ring_h = height;
    s_ring_px = muse_mic_ring_width(width, height);
    s_ring_round = round;
    muse_mic_ring_set_corner(corner_px);
}

void muse_mic_ring_set_corner(int corner_px)
{
    int s = s_ring_w < s_ring_h ? s_ring_w : s_ring_h;
    s_ring_corner = corner_px < 0 ? 0 : corner_px > s / 2 ? s / 2 : corner_px;
    build_rows();
    atomic_fetch_add(&s_ring_gen, 1);   /* drawn again where it shows */
    void (*fn)(bool) = s_ring_listener;
    if (fn && atomic_load(&s_ring_on)) {
        fn(true);
    }
}

int muse_mic_ring_corner(void)
{
    return s_ring_corner;
}

void muse_mic_ring_set(bool on)
{
    if (atomic_exchange(&s_ring_on, on) == on) {
        return;
    }
    atomic_fetch_add(&s_ring_gen, 1);
    void (*fn)(bool) = s_ring_listener;
    if (fn) {
        fn(on);
    }
}

bool muse_mic_ring_on(void)
{
    return atomic_load(&s_ring_on);
}

uint32_t muse_mic_ring_generation(void)
{
    return atomic_load(&s_ring_gen);
}

void muse_mic_ring_set_listener(void (*fn)(bool on))
{
    s_ring_listener = fn;
}

/* Row y's pixels inside a rounded rectangle [x0, x1) x [y0, y1) with corners
 * of radius r, by pixel centers: [*a, *b). False if none. */
static bool ring_span(int y, int x0, int y0, int x1, int y1, int r, int *a, int *b)
{
    float yc = y + 0.5f;
    if (yc < y0 || yc >= y1 || x1 <= x0) {
        return false;
    }
    float inset = 0;
    float dy = yc < y0 + r ? (y0 + r) - yc : yc > y1 - r ? yc - (y1 - r) : 0;
    if (dy > 0) {
        float d = (float)r * r - dy * dy;
        inset = r - (d > 0 ? sqrtf(d) : 0);
    }
    int lo = (int)ceilf(x0 + inset - 0.5f), hi = (int)ceilf(x1 - inset - 0.5f);
    if (hi <= lo) {
        return false;
    }
    *a = lo;
    *b = hi;
    return true;
}

/*
 * The ring's outer edge is the whole screen rectangle: past the panel's
 * visible edge nothing shows, so no curve guess can leave a sliver of the
 * picture between the ring and the edge (the ESP-Mosaico's rounded corners
 * showed white where an 80 px arc missed theirs). Only the inner edge
 * follows the visible shape, inset by the ring's width: a circle on a round
 * panel, a rectangle with the visible corner radius less the width on a
 * rectangular one, so the ring looks as wide all the way round.
 */
static bool row_compute(int y, int *o0, int *o1, int *i0, int *i1);

static void build_rows(void)
{
    if (s_ring_h <= 0) {
        return;
    }
    if (!s_rows) {
        s_rows = malloc(sizeof(int16_t) * 2 * (size_t)s_ring_h);
        if (!s_rows) {
            return;
        }
    }
    for (int y = 0; y < s_ring_h; y++) {
        int o0, o1, i0 = 0, i1 = 0;
        if (!row_compute(y, &o0, &o1, &i0, &i1) || i0 >= i1) {
            s_rows[2 * y] = s_rows[2 * y + 1] = -1;
        } else {
            s_rows[2 * y] = (int16_t)i0;
            s_rows[2 * y + 1] = (int16_t)i1;
        }
    }
}

bool muse_mic_ring_row(int y, int *o0, int *o1, int *i0, int *i1)
{
    if (s_ring_px <= 0 || y < 0 || y >= s_ring_h) {
        return false;
    }
    if (!s_rows) {
        return row_compute(y, o0, o1, i0, i1);
    }
    *o0 = 0;
    *o1 = s_ring_w;
    if (s_rows[2 * y] < 0) {
        *i0 = *i1 = *o1;   /* all ring */
    } else {
        *i0 = s_rows[2 * y];
        *i1 = s_rows[2 * y + 1];
    }
    return true;
}

static bool row_compute(int y, int *o0, int *o1, int *i0, int *i1)
{
    if (s_ring_px <= 0 || y < 0 || y >= s_ring_h) {
        return false;
    }
    *o0 = 0;
    *o1 = s_ring_w;
    int s = s_ring_w < s_ring_h ? s_ring_w : s_ring_h;
    bool hole;
    if (s_ring_round) {
        int x0 = (s_ring_w - s) / 2, y0 = (s_ring_h - s) / 2, r = s / 2 - s_ring_px;
        hole = r > 0 && ring_span(y, x0 + s_ring_px, y0 + s_ring_px, x0 + s - s_ring_px, y0 + s - s_ring_px, r, i0, i1);
    } else {
        int r = s_ring_corner > s_ring_px ? s_ring_corner - s_ring_px : 0;
        hole = ring_span(y, s_ring_px, s_ring_px, s_ring_w - s_ring_px, s_ring_h - s_ring_px, r, i0, i1);
    }
    if (!hole) {
        *i0 = *i1 = *o1;   /* all ring */
    }
    return true;
}

static void ring_paint(uint8_t *row, int x, int w, int a, int b)
{
    if (a < x) {
        a = x;
    }
    if (b > x + w) {
        b = x + w;
    }
    /* The panel's byte order (big-endian), two bytes at a time. */
    const uint8_t be[2] = { MUSE_MIC_RING_RGB565 >> 8, MUSE_MIC_RING_RGB565 & 0xFF };
    uint16_t px;
    __builtin_memcpy(&px, be, 2);
    uint16_t *p = (uint16_t *)(void *)(row + (a - x) * 2), *e = (uint16_t *)(void *)(row + (b - x) * 2);
    while (p < e) *p++ = px;
}

void muse_mic_ring_apply(uint8_t *dst, int stride, int x, int y, int w, int rows)
{
    muse_mic_ring_apply_as(atomic_load(&s_ring_on), dst, stride, x, y, w, rows);
}

void muse_mic_ring_apply_as(bool on, uint8_t *dst, int stride, int x, int y, int w, int rows)
{
    if (!on || s_ring_px <= 0) {
        return;
    }
    /* Each row is at most two short runs (one across the top and bottom). */
    for (int r = 0; r < rows; r++) {
        int o0, o1, i0, i1;
        if (!muse_mic_ring_row(y + r, &o0, &o1, &i0, &i1)) {
            continue;
        }
        uint8_t *row = dst + (size_t)r * stride;
        if (i0 >= i1) {
            ring_paint(row, x, w, o0, o1);
        } else {
            ring_paint(row, x, w, o0, i0);
            ring_paint(row, x, w, i1, o1);
        }
    }
}
