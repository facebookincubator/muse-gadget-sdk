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

#include "muse_turn_dot.h"

#include <math.h>
#include <stdatomic.h>

#include "muse_mic_ring.h"

/*
 * Geometry, as shares of S, the screen's short side: the dot's full radius
 * 1.7 % (8 px at 466, 7 at 412, 6 at 368), its center on the vertical
 * middle, its top edge 1 % of S (at least 2 px) inside the mic ring's inner
 * edge at the top. At the top center the ring's inner edge is ring-width
 * px down on every shape (a circle, a rectangle, a rounded rectangle), so
 * one rule gives the same spot on every face.
 */
#define DOT_R_SHARE 0.017f
#define DOT_GAP_SHARE 0.010f
#define DOT_MIN_SCALE 0.72f    /* the breath: radius 72-100 % of the full one */
#define DOT_MIN_ALPHA 0.50f    /* ...and opacity 50-100 % */

/* The breath: steps of 80 ms (12.5 redraws of the dot's box a second at the
 * most), 20 of them for thinking (1.6 s), 12 for responding (0.96 s). It
 * starts at its fullest, so the first frame of a turn's state is the most
 * visible one. */
#define STEP_US 80000
#define THINKING_STEPS 20
#define RESPONDING_STEPS 12

static float s_dot_cx, s_dot_cy, s_dot_r;
static int s_dot_bx0, s_dot_by0, s_dot_bx1, s_dot_by1;
static atomic_bool s_dot_still;
static atomic_bool s_dot_in_call;
/* The look now, one word so any task reads it whole: kind in bits 0-1, the
 * breath step in bits 2-7. */
static atomic_uint s_dot_look;
static atomic_uint s_dot_gen;
static atomic_uint s_dot_kind_since;      /* the kind the breath started for */
static atomic_uint s_dot_since_ms;        /* ...and when (ms, wrapping: only differences count) */

void muse_turn_dot_init(int width, int height, bool round, int corner_px)
{
    (void)round;
    (void)corner_px;
    int s = width < height ? width : height;
    int ring = muse_mic_ring_width(width, height);
    float r = roundf(s * DOT_R_SHARE);
    s_dot_r = r < 3 ? 3 : r;
    float gap = roundf(s * DOT_GAP_SHARE);
    gap = gap < 2 ? 2 : gap;
    s_dot_cx = width / 2.0f;
    s_dot_cy = ring + gap + s_dot_r;
    /* One px of anti-aliasing past the radius, then whole groups of 4
     * columns and pairs of rows. */
    int x0 = (int)floorf(s_dot_cx - s_dot_r - 1), x1 = (int)ceilf(s_dot_cx + s_dot_r + 1);
    int y0 = (int)floorf(s_dot_cy - s_dot_r - 1), y1 = (int)ceilf(s_dot_cy + s_dot_r + 1);
    x0 &= ~3;
    x1 = (x1 + 3) & ~3;
    y0 &= ~1;
    y1 = (y1 + 1) & ~1;
    s_dot_bx0 = x0 < 0 ? 0 : x0;
    s_dot_bx1 = x1 > width ? width : x1;
    s_dot_by0 = y0 < 0 ? 0 : y0;
    s_dot_by1 = y1 > height ? height : y1;
    atomic_fetch_add(&s_dot_gen, 1);
}

void muse_turn_dot_set_static(bool still)
{
    atomic_store(&s_dot_still, still);
}

void muse_turn_dot_set_in_call(bool in_call)
{
    atomic_store(&s_dot_in_call, in_call);
}

bool muse_turn_dot_in_call(void)
{
    return atomic_load(&s_dot_in_call);
}

muse_turn_dot_kind_t muse_turn_dot_kind(muse_mode_t mode)
{
    return mode == MUSE_MODE_THINKING ? MUSE_TURN_DOT_THINKING
         : mode == MUSE_MODE_SPEAKING ? MUSE_TURN_DOT_RESPONDING
                                      : MUSE_TURN_DOT_OFF;
}

bool muse_turn_dot_avatar_ok(uint32_t thinking_rgb, uint32_t speaking_rgb)
{
    return (thinking_rgb & 0xFFFFFFu) == MUSE_TURN_THINKING_RGB && (speaking_rgb & 0xFFFFFFu) == MUSE_TURN_RESPONDING_RGB;
}

uint32_t muse_turn_dot_rgb(muse_turn_dot_kind_t kind)
{
    return kind == MUSE_TURN_DOT_THINKING ? MUSE_TURN_THINKING_RGB
         : kind == MUSE_TURN_DOT_RESPONDING ? MUSE_TURN_RESPONDING_RGB
                                            : 0;
}

static int dot_steps_of(unsigned kind)
{
    return kind == MUSE_TURN_DOT_THINKING ? THINKING_STEPS : RESPONDING_STEPS;
}

bool muse_turn_dot_update(muse_turn_dot_kind_t kind, int64_t now_us)
{
    unsigned k = atomic_load(&s_dot_in_call) ? MUSE_TURN_DOT_OFF : (unsigned)kind & 3u;   /* no dot in a call */
    unsigned step = 0;
    if (k != MUSE_TURN_DOT_OFF) {
        if (atomic_exchange(&s_dot_kind_since, k) != k) {
            atomic_store(&s_dot_since_ms, (unsigned)(now_us / 1000));
        }
        unsigned t_ms = (unsigned)(now_us / 1000) - atomic_load(&s_dot_since_ms);
        if (!atomic_load(&s_dot_still) && t_ms < 0x80000000u) {
            step = (t_ms / (STEP_US / 1000)) % (unsigned)dot_steps_of(k);
        }
    } else {
        atomic_store(&s_dot_kind_since, MUSE_TURN_DOT_OFF);
    }
    unsigned look = k | step << 2;
    if (atomic_exchange(&s_dot_look, look) == look) {
        return false;
    }
    atomic_fetch_add(&s_dot_gen, 1);
    return true;
}

muse_turn_dot_kind_t muse_turn_dot_shown(void)
{
    return (muse_turn_dot_kind_t)(atomic_load(&s_dot_look) & 3u);
}

uint32_t muse_turn_dot_generation(void)
{
    return atomic_load(&s_dot_gen);
}

void muse_turn_dot_box(int *x0, int *y0, int *x1, int *y1)
{
    *x0 = s_dot_bx0;
    *y0 = s_dot_by0;
    *x1 = s_dot_bx1;
    *y1 = s_dot_by1;
}

void muse_turn_dot_geometry(float *cx, float *cy, float *radius)
{
    *cx = s_dot_cx;
    *cy = s_dot_cy;
    *radius = s_dot_r;
}

static inline uint16_t dot_blend565(uint16_t bg, uint16_t fr, uint16_t fg, uint16_t fb, unsigned a)
{
    int r = bg >> 11, g = (bg >> 5) & 63, b = bg & 31;
    r += ((int)fr - r) * (int)a >> 8;
    g += ((int)fg - g) * (int)a >> 8;
    b += ((int)fb - b) * (int)a >> 8;
    return (uint16_t)(r << 11 | g << 5 | b);
}

void muse_turn_dot_paint(uint8_t *dst, int stride, int x, int y, int w, int rows, bool big_endian)
{
    unsigned look = atomic_load(&s_dot_look);
    unsigned kind = look & 3u;
    if (kind == MUSE_TURN_DOT_OFF || s_dot_r <= 0) {
        return;
    }
    int x0 = x > s_dot_bx0 ? x : s_dot_bx0, x1 = x + w < s_dot_bx1 ? x + w : s_dot_bx1;
    int y0 = y > s_dot_by0 ? y : s_dot_by0, y1 = y + rows < s_dot_by1 ? y + rows : s_dot_by1;
    if (x0 >= x1 || y0 >= y1) {
        return;
    }
    /* The breath: 1 at step 0, 0 half a period later. */
    unsigned step = look >> 2;
    float lv = 0.5f + 0.5f * cosf(6.2831853f * (float)step / (float)dot_steps_of(kind));
    float r = s_dot_r * (DOT_MIN_SCALE + (1.0f - DOT_MIN_SCALE) * lv);
    float alpha = DOT_MIN_ALPHA + (1.0f - DOT_MIN_ALPHA) * lv;
    uint32_t rgb = muse_turn_dot_rgb((muse_turn_dot_kind_t)kind);
    uint16_t fr = (uint16_t)(rgb >> 19 & 31), fg = (uint16_t)(rgb >> 10 & 63), fb = (uint16_t)(rgb >> 3 & 31);
    float in2 = (r - 0.5f) * (r - 0.5f), out2 = (r + 0.5f) * (r + 0.5f);
    unsigned full = (unsigned)lroundf(alpha * 256.0f);
    for (int py = y0; py < y1; py++) {
        float dy = (float)py + 0.5f - s_dot_cy;
        uint8_t *row = dst + (size_t)(py - y) * (size_t)stride;
        for (int px = x0; px < x1; px++) {
            float dx = (float)px + 0.5f - s_dot_cx;
            float d2 = dx * dx + dy * dy;
            if (d2 >= out2) {
                continue;
            }
            unsigned a = full;
            if (d2 > in2) {
                float cov = r + 0.5f - sqrtf(d2);   /* the edge pixel's share inside */
                a = (unsigned)lroundf(alpha * cov * 256.0f);
                if (!a) {
                    continue;
                }
            }
            uint8_t *p = row + (size_t)(px - x) * 2;
            uint16_t bg = big_endian ? (uint16_t)(p[0] << 8 | p[1]) : (uint16_t)(p[0] | p[1] << 8);
            uint16_t v = a >= 256 ? (uint16_t)(fr << 11 | fg << 5 | fb) : dot_blend565(bg, fr, fg, fb, a);
            if (big_endian) {
                p[0] = (uint8_t)(v >> 8);
                p[1] = (uint8_t)v;
            } else {
                p[0] = (uint8_t)v;
                p[1] = (uint8_t)(v >> 8);
            }
        }
    }
}

void muse_turn_dot_apply(uint8_t *dst, int stride, int x, int y, int w, int rows)
{
    muse_turn_dot_paint(dst, stride, x, y, w, rows, true);
}
