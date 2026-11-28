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

/* Host-only differential/sanitizer harness; not part of the firmware build. */
#include "muse_pixel.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define DECL(prefix) \
    void prefix##_render(const muse_pose_t *); \
    void prefix##_set_size(int); \
    void prefix##_set_background(uint32_t); \
    void prefix##_scale(uint16_t *, int, int, int, int, int); \
    uint16_t prefix##_cell(int, int); \
    const uint8_t *prefix##_fb(void); \
    const uint8_t *prefix##_mask(void); \
    const uint16_t *prefix##_pal(void); \
    const uint16_t *prefix##_dim(void); \
    int prefix##_colors(void); \
    void prefix##_reset(void); \
    unsigned long prefix##_trig(void)
DECL(opt);
DECL(ref);

static unsigned frames;
static void same(const void *a, const void *b, size_t n, const char *what)
{
    if (memcmp(a, b, n)) {
        fprintf(stderr, "%s mismatch at frame %u\n", what, frames);
        abort();
    }
}

static void check_frame(const muse_pose_t *p)
{
    ref_render(p);
    opt_render(p);
    frames++;
    same(ref_fb(), opt_fb(), MUSE_PX_W * MUSE_PX_H, "palette indices");
    same(ref_mask(), opt_mask(), MUSE_PX_W * MUSE_PX_H, "part masks");
    assert(ref_colors() == opt_colors());
    same(ref_pal(), opt_pal(), ref_colors() * sizeof(uint16_t), "RGB565 palette");
    same(ref_dim(), opt_dim(), ref_colors() * sizeof(uint16_t), "dim palette");
    for (int y = 0; y < MUSE_PX_H; y++) {
        for (int x = 0; x < MUSE_PX_W; x++) {
            assert(opt_cell(x, y) == ref_cell(x, y));
        }
    }
    /* Outside the grid: the background (index 0). */
    const int invalid[] = { -1, 64, INT_MIN, INT_MAX };
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        assert(opt_cell(invalid[i], 0) == opt_pal()[0]);
        assert(opt_cell(0, invalid[i]) == opt_pal()[0]);
    }
}

/* Explicit red zones, padded strides and a scalar, independent scale oracle. */
#define STRIDE 521
#define GUARD 16
static uint16_t output[GUARD + STRIDE * 512 + GUARD];
static uint16_t original[GUARD + STRIDE * 512 + GUARD];
static void check_scale(int requested, int x0, int x1, int y0, int y1)
{
    int size = requested < 512 ? requested : 512;
    int w = x1 - x0 + 1, h = y1 - y0 + 1;
    memset(output, 0xa5, sizeof(output));
    memset(original, 0xa5, sizeof(original));
    opt_set_size(requested);
    ref_set_size(requested);
    opt_scale(output + GUARD, STRIDE, x0, x1, y0, y1);
    ref_scale(original + GUARD, STRIDE, x0, x1, y0, y1);
    same(output, original, sizeof(output), "scaled ROI / padding");
    for (int row = 0; row < h; row++) {
        int y = y0 + row, cy = y * MUSE_PX_H / size;
        bool ey = size >= 3 * MUSE_PX_H && (y + 1) * MUSE_PX_H / size != cy;
        for (int col = 0; col < w; col++) {
            int x = x0 + col, cx = x * MUSE_PX_W / size;
            bool ex = size >= 3 * MUSE_PX_W && (x + 1) * MUSE_PX_W / size != cx;
            uint8_t c = ref_fb()[cy * MUSE_PX_W + cx];
            uint16_t expected = (ex || ey) ? ref_dim()[c] : ref_pal()[c];
            assert(output[GUARD + row * STRIDE + col] == expected);
            /* Accessor deliberately ignores grid dimming. */
            assert(opt_cell(cx, cy) == ref_pal()[c]);
        }
        for (int col = w; col < STRIDE; col++) {
            assert(output[GUARD + row * STRIDE + col] == 0xa5a5);
        }
    }
    if (size >= 64 && x0 == 0 && y0 == 0 && x1 == size - 1 && y1 == size - 1) {
        /* UI dirty detection previously read the first screen pixel of each
         * cell. For size>=64 that sample exists and is never grid-dimmed. */
        for (int cy = 0; cy < 64; cy++) {
            int sy = (cy * size + 63) / 64;
            for (int cx = 0; cx < 64; cx++) {
                int sx = (cx * size + 63) / 64;
                assert(output[GUARD + sy * STRIDE + sx] == opt_cell(cx, cy));
            }
        }
    }
    for (int i = 0; i < GUARD; i++) {
        assert(output[i] == 0xa5a5);
        assert(output[GUARD + h * STRIDE + i] == 0xa5a5);
    }
}

static uint32_t rng = 0x12345678;
static uint32_t random_u32(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}
static float random_unit(void) { return (random_u32() & 0xffff) / 65535.0f; }

static void differential(void)
{
    ref_reset();
    opt_reset();
    const float levels[] = { 0, 0.001f, 0.2f, 0.5f, 0.999f, 1 };
    const float happiness[] = { 0, 0.001f, 0.2f, 0.20001f, 0.5f, 1 };
    const float deltas[] = { 0, 0.001f, 0.016f, 0.04f, 0.05f, 0.2f, 1, -0.03f };
    muse_pose_t p = {0};
    /* Long mode dwell exercises blink/gaze RNG, palette convergence, ring cache
     * hits/misses/wraps, boot squash, off fade, and pet/audio extreme poses. */
    for (int cycle = 0; cycle < 3; cycle++) {
        for (int mode = 0; mode < MUSE_MODE_COUNT; mode++) {
            p.mode = (muse_mode_t)mode;
            p.mode_t = 0;
            for (int i = 0; i < 180; i++) {
                float dt = deltas[i % 8];
                p.t += dt;
                p.mode_t += dt > 0 ? dt : 0;
                p.level = levels[(i / 3) % 6];
                p.happy = happiness[(i / 7) % 6];
                check_frame(&p);
                if (i == 90 && cycle == 0) {
                    /* Every supported scale size in every mode, including grid
                     * threshold, undersampling, maximum and clamped maximum. */
                    for (int size = 1; size <= 512; size++) {
                        check_scale(size, 0, size - 1, 0, size - 1);
                        int x = size / 3, y = size / 2;
                        check_scale(size, x, x, y, y);
                    }
                    check_scale(1024, 23, 509, 17, 511);
                }
            }
        }
    }
    /* Abrupt mode/palette changes and irregular/non-monotonic timestamps. */
    for (int i = 0; i < 2200; i++) {
        p.mode = (muse_mode_t)(random_u32() % MUSE_MODE_COUNT);
        p.mode_t = random_unit() * 3;
        p.t = random_unit() * 10000;
        p.level = random_unit();
        p.happy = random_unit();
        check_frame(&p);
        int size = 1 + random_u32() % 512;
        int x0 = random_u32() % size, x1 = x0 + random_u32() % (size - x0);
        int y0 = random_u32() % size, y1 = y0 + random_u32() % (size - y0);
        check_scale(size, x0, x1, y0, y1);
    }
    /* The extremes the outline pass's box must cover (muse_pixel.c draw_avatar):
     * the boot squash (widest body), full happy hops with arms up, full audio
     * level, the off wave, the listening and thinking arms, at any time. */
    for (int i = 0; i < 6000; i++) {
        static const muse_mode_t modes[] = { MUSE_MODE_BOOT, MUSE_MODE_IDLE, MUSE_MODE_LISTENING, MUSE_MODE_THINKING,
                                             MUSE_MODE_SPEAKING, MUSE_MODE_ERROR, MUSE_MODE_OFF };
        p.mode = modes[i % 7];
        p.mode_t = p.mode == MUSE_MODE_BOOT ? random_unit() * 0.7f : random_unit() * 2;
        p.t = random_unit() * 1000;
        p.level = (i & 3) ? 1.0f : random_unit();
        p.happy = (i & 1) ? 1.0f : random_unit() * 0.3f;
        check_frame(&p);
    }
    /* A light theme's background: the palette's background and the aura
     * mixed into it, in every mode. */
    ref_set_background(0xf2efe8);
    opt_set_background(0xf2efe8);
    for (int i = 0; i < 420; i++) {
        p.mode = (muse_mode_t)(i / 60);
        p.mode_t = (i % 60) * 0.05f;
        p.t += 0.05f;
        p.level = levels[i % 6];
        p.happy = happiness[(i / 5) % 6];
        check_frame(&p);
    }
    ref_set_background(0);
    opt_set_background(0);
    printf("exact match: %u frames, indices/masks/palettes/accessor; all sizes 1..512 in all 7 modes, full frames + guarded ROIs\n", frames);
}

static double now(void)
{
    struct timespec ts;
    assert(clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts) == 0);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}
static volatile uint32_t checksum;
static double render_time(bool optimized, int mode, int count)
{
    muse_pose_t p = { .mode = (muse_mode_t)mode, .level = 0.5f, .happy = 0.1f };
    if (optimized) opt_reset(); else ref_reset();
    for (int i = 0; i < 100; i++) {
        p.t += 0.05f;
        p.mode_t += 0.05f;
        if (optimized) opt_render(&p); else ref_render(&p);
    }
    double start = now();
    for (int i = 0; i < count; i++) {
        p.t += 0.05f;
        p.mode_t += 0.05f;
        if (optimized) opt_render(&p); else ref_render(&p);
        checksum += optimized ? opt_cell(32, 32) : ref_cell(32, 32);
    }
    return (now() - start) * 1e6 / count;
}
static double median3(double a, double b, double c)
{
    if (a > b) { double t = a; a = b; b = t; }
    if (b > c) { double t = b; b = c; c = t; }
    return a > b ? a : b;
}
static double probe_time(bool accessor, int count)
{
    uint16_t row[320];
    opt_set_size(320);
    double start = now();
    for (int i = 0; i < count; i++) {
        for (int y = 0; y < 64; y++) {
            if (!accessor) opt_scale(row, 320, 0, 319, y * 5, y * 5);
            for (int x = 0; x < 64; x++) {
                checksum += accessor ? opt_cell(x, y) : row[x * 5];
            }
        }
    }
    return (now() - start) * 1e6 / count;
}
static void benchmark(void)
{
    puts("Native host process-CPU microbenchmark, NOT C6 FPS; -O2, 50ms pose increments, warm LUTs; median of 3 x 1000 frames.");
    for (int mode = 0; mode < MUSE_MODE_COUNT; mode++) {
        double r[3], o[3];
        for (int i = 0; i < 3; i++) {
            /* Alternate measurement order to reduce systematic order bias. */
            if (i & 1) {
                o[i] = render_time(true, mode, 1000);
                r[i] = render_time(false, mode, 1000);
            } else {
                r[i] = render_time(false, mode, 1000);
                o[i] = render_time(true, mode, 1000);
            }
        }
        double rm = median3(r[0], r[1], r[2]), om = median3(o[0], o[1], o[2]);
        printf("mode %d: reference %.2f us [%.2f/%.2f/%.2f], optimized %.2f us [%.2f/%.2f/%.2f], ratio %.2fx\n", mode, rm, r[0], r[1], r[2], om, o[0], o[1], o[2], rm / om);
    }
    muse_pose_t p = { .mode = MUSE_MODE_LISTENING, .t = 100, .mode_t = 1, .level = 0.5f };
    ref_reset(); opt_reset(); ref_render(&p); opt_render(&p);
    unsigned long rc = ref_trig(), oc = opt_trig();
    p.t += 0.001f;
    ref_render(&p); opt_render(&p);
    printf("same-dot-count listening frame sinf/cosf calls: reference %lu, optimized %lu\n", ref_trig() - rc, opt_trig() - oc);
    printf("320px dirty-cell probe: 64 scaled rows %.2f us, 4096 accessor reads %.2f us (native host only)\n", probe_time(false, 1000), probe_time(true, 1000));
    printf("checksum %u\n", checksum);
}
int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--benchmark") == 0) benchmark();
    else differential();
    return 0;
}
