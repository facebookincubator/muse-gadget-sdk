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
#include "muse_yuv.h"
#include "muse_video_fit.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "muse_mem.h"   /* MUSE_SIMD_CAPS */

#include "esp_attr.h"
#if defined(ESP_PLATFORM)
#include "esp_cpu.h"
#endif

/* The converters in IRAM: they run for every pixel of every picture. */
#define YUV_HOT IRAM_ATTR
#include "esp_log.h"
#include "sdkconfig.h"

/* Offset into the clamp tables, so a sum below 0 or above 255 still indexes them. */
#define OFS 384

/* Only the last pixels of a row use these: in PSRAM, to spare internal RAM. */
static const uint16_t *s_tr, *s_tgh, *s_tb;
static const int16_t *s_rv, *s_gu, *s_gv, *s_bu, *s_y;
static bool s_init;

/* The SIMD kernel's constants, 8 lanes each: 128, 204, 50, 104, 258, then 16,
 * 149, 255, 0xF8, 0xF8, 0xE0, 0x1C (see muse_yuv_pie.S). */
static int16_t s_k[12][8] __attribute__((aligned(16)));
static int16_t s_scratch[8][8] __attribute__((aligned(16), unused));   /* the SIMD kernels' (unused without them: the C6) */

#if CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32S31
#define HAVE_PIE 1
extern void muse_yuv_pie_2rows_up2(const uint8_t *y0, const uint8_t *y1, const uint8_t *u, const uint8_t *v,
                                   uint8_t *o0, uint8_t *o1, uint32_t nchunks, const int16_t *k, int16_t *scratch,
                                   uint32_t ostride);
extern void muse_yuv_pie_2rows(const uint8_t *y0, const uint8_t *y1, const uint8_t *u, const uint8_t *v,
                               uint8_t *o0, uint8_t *o1, uint32_t nchunks, const int16_t *k, int16_t *scratch);
#else
#define HAVE_PIE 0
#endif
/* The SIMD kernels in use: on the S3 they're proven; the S31's (a port no
 * emulator runs) must first give the C path's pixels (selftest()). */
#if CONFIG_IDF_TARGET_ESP32S31
static bool s_pie;
#else
static bool s_pie = HAVE_PIE;
#endif
static void selftest(void);

static void init_tables(void)
{
    uint16_t *t = heap_caps_malloc(3 * 1024 * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    int16_t *c5 = heap_caps_malloc(5 * 256 * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!t || !c5) {
        t = t ? t : malloc(3 * 1024 * sizeof(uint16_t));
        c5 = c5 ? c5 : malloc(5 * 256 * sizeof(int16_t));
    }
    uint16_t *tr = t, *tgh = t + 1024, *tb = t + 2048;
    int16_t *ty = c5, *rv = c5 + 256, *gu = c5 + 512, *gv = c5 + 768, *bu = c5 + 1024;
    for (int i = 0; i < 1024; i++) {
        int c = i - OFS;
        c = c < 0 ? 0 : c > 255 ? 255 : c;
        tr[i] = (uint16_t)(c & 0xF8);
        tgh[i] = (uint16_t)(((c & 0xE0) >> 5) | ((c & 0x1C) << 11));
        tb[i] = (uint16_t)((c & 0xF8) << 5);
    }
    for (int i = 0; i < 256; i++) {
        ty[i] = (int16_t)((((i - 16) * 149) >> 7) + OFS);
        rv[i] = (int16_t)(((i - 128) * 204) >> 7);
        gu[i] = (int16_t)(((i - 128) * 50) >> 7);
        gv[i] = (int16_t)(((i - 128) * 104) >> 7);
        bu[i] = (int16_t)(((i - 128) * 258) >> 7);
    }
    s_tr = tr, s_tgh = tgh, s_tb = tb;
    s_y = ty, s_rv = rv, s_gu = gu, s_gv = gv, s_bu = bu;
    static const int16_t kv[12] = { 128, 204, 50, 104, 258, 16, 149, 255, 0xF8, 0xF8, 0xE0, 0x1C };
    for (int j = 0; j < 12; j++) {
        for (int l = 0; l < 8; l++) {
            s_k[j][l] = kv[j];
        }
    }
    s_init = true;
    selftest();
}

static inline uint32_t px2(int y, int rv, int g, int bu)
{
    uint32_t p = s_tr[y + rv] | s_tgh[y - g] | s_tb[y + bu];
    return p | p << 16;
}

void YUV_HOT muse_yuv_up2(const uint8_t *y, const uint8_t *u, const uint8_t *v, int ystride, int cstride,
                          int src_w, int out_rows, uint8_t *out, int out_stride)
{
    if (!s_init) {
        init_tables();
    }
    int simd_w = s_pie ? src_w & ~15 : 0;
    /* Each source row pair makes four output rows: a pair whose four don't
     * all fit the slice (out_rows not a multiple of 4) writes only those that
     * do, on the C path, and reads its second row only if it shows. */
    for (int r = 0; 2 * r < out_rows; r += 2) {
        int n = out_rows - 2 * r < 4 ? out_rows - 2 * r : 4;   /* output rows of this pair */
        const uint8_t *y0 = y + r * ystride, *y1 = n > 2 ? y0 + ystride : y0;
        const uint8_t *cu = u + (r / 2) * cstride, *cv = v + (r / 2) * cstride;
        uint8_t *o0 = out + 2 * r * out_stride;
        int sw = n == 4 ? simd_w : 0;   /* the kernel writes all four rows */
#if HAVE_PIE
        /* Aligned rows only (muse_yuv_1x). */
        if (sw && ((((uintptr_t)y0 | (uintptr_t)y1) & 15) ||
                   (((uintptr_t)o0 | (uintptr_t)out_stride | (uintptr_t)cu | (uintptr_t)cv) & 7))) {
            sw = 0;
        }
        if (sw) {
            muse_yuv_pie_2rows_up2(y0, y1, cu, cv, o0, o0 + 2 * out_stride, sw / 16, &s_k[0][0], &s_scratch[0][0],
                                   (uint32_t)out_stride);
        }
#endif
        uint32_t *w[4];
        for (int k = 0; k < 4; k++) {
            w[k] = (uint32_t *)(o0 + (size_t)(k < n ? k : 0) * out_stride);   /* rows past the slice: never written */
        }
        for (int x = sw; x < src_w; x += 2) {
            int U = cu[x >> 1], V = cv[x >> 1];
            int rv = s_rv[V], g = s_gu[U] + s_gv[V], bu = s_bu[U];
            uint32_t p0 = px2(s_y[y0[x]], rv, g, bu), p1 = px2(s_y[y0[x + 1]], rv, g, bu);
            w[0][x] = p0;
            w[0][x + 1] = p1;
            if (n > 1) {
                w[1][x] = p0;
                w[1][x + 1] = p1;
            }
            if (n > 2) {
                uint32_t q0 = px2(s_y[y1[x]], rv, g, bu), q1 = px2(s_y[y1[x + 1]], rv, g, bu);
                w[2][x] = q0;
                w[2][x + 1] = q1;
                if (n > 3) {
                    w[3][x] = q0;
                    w[3][x + 1] = q1;
                }
            }
        }
    }
}

static inline uint16_t px1(int y, int rv, int g, int bu)
{
    return s_tr[y + rv] | s_tgh[y - g] | s_tb[y + bu];
}

void YUV_HOT muse_yuv_1x(const uint8_t *y, const uint8_t *u, const uint8_t *v, int ystride, int cstride,
                           int src_w, int src_rows, uint8_t *out, int out_stride)
{
    if (!s_init) {
        init_tables();
    }
    int simd_w = s_pie ? src_w & ~15 : 0;
    for (int r = 0; r < src_rows; r += 2) {
        /* An odd slice's last row is a pair's first alone (C path; its second row is neither read nor written). */
        bool two = r + 1 < src_rows;
        const uint8_t *y0 = y + r * ystride, *y1 = two ? y0 + ystride : y0;
        const uint8_t *cu = u + (r / 2) * cstride, *cv = v + (r / 2) * cstride;
        uint32_t *w0 = (uint32_t *)(out + r * out_stride), *w1 = (uint32_t *)(out + (r + (two ? 1 : 0)) * out_stride);
        int sw = two ? simd_w : 0;
#if HAVE_PIE
        /* The vector loads and stores ignore the low bits of an address:
         * luma loads are 16 bytes (muse_yuv_pie.S: ee.vld.128), chroma
         * loads and the stores 8 (ee.vld.l.64, ee.vst.l/h.64). A row that
         * doesn't start on that (a 466 px UI picture's 932 B output rows:
         * every odd row 4 bytes off) would land up to 2 pixels early, so
         * such rows go the scalar way. A 240 px video's 120 B chroma rows
         * are 8-aligned and stay on the vector path. */
        if (sw && ((((uintptr_t)y0 | (uintptr_t)y1) & 15) ||
                   (((uintptr_t)w0 | (uintptr_t)w1 | (uintptr_t)cu | (uintptr_t)cv) & 7))) {
            sw = 0;
        }
        if (sw) {
            muse_yuv_pie_2rows(y0, y1, cu, cv, (uint8_t *)w0, (uint8_t *)w1, sw / 16, &s_k[0][0], &s_scratch[0][0]);
        }
#endif
        for (int x = sw; x < src_w; x += 2) {
            int U = cu[x >> 1], V = cv[x >> 1];
            int rv = s_rv[V], g = s_gu[U] + s_gv[V], bu = s_bu[U];
            w0[x >> 1] = px1(s_y[y0[x]], rv, g, bu) | (uint32_t)px1(s_y[y0[x + 1]], rv, g, bu) << 16;
            if (two) {
                w1[x >> 1] = px1(s_y[y1[x]], rv, g, bu) | (uint32_t)px1(s_y[y1[x + 1]], rv, g, bu) << 16;
            }
        }
    }
}

/*
 * BT.601 limited range in Q12, rounded: R = 1.164 (Y-16) + 1.596 (V-128),
 * G = 1.164 (Y-16) - 0.391 (U-128) - 0.813 (V-128), B = 1.164 (Y-16) + 2.018 (U-128).
 */
#define HQ_Y 4769
#define HQ_RV 6537
#define HQ_GU 1604
#define HQ_GV 3330
#define HQ_BU 8263
/* 4x4 Bayer thresholds in 1/16 of a step, centred (-7.5..7.5 as 2x-15..15). */
static const int8_t s_bayer[4][4] = { { -15, 1, -11, 5 }, { 9, -7, 13, -3 }, { -9, 7, -13, 3 }, { 15, -1, 11, -5 } };

static inline int clamp255(int x)
{
    return x < 0 ? 0 : x > 255 ? 255 : x;
}

/* One RGB565 pixel (the panel's byte order, as px1) from Q12 RGB and a dither offset. */
static inline uint16_t hq_px(int y12, int u, int v, int d)
{
    /* u, v: chroma - 128 in Q4 (bilinear sums of 16). */
    int r = y12 + (HQ_RV * v >> 4);
    int g = y12 - ((HQ_GU * u + HQ_GV * v) >> 4);
    int b = y12 + (HQ_BU * u >> 4);
    /* Q12 -> 8 bits with rounding, then the dither at the 5/6-bit step. */
    /* The bits RGB565 drops are truncated, so the dither spans one whole
     * step (d + 16 = 1..31 thirty-seconds of it): on average unbiased. R and
     * B steps are 8 levels of 8 bits (8 << 12 in Q12, so a 32nd is 1 << 10),
     * G's 4 (1 << 9). */
    int r8 = clamp255((r + (d + 16) * 1024) >> 12);
    int g8 = clamp255((g + (d + 16) * 512) >> 12);
    int b8 = clamp255((b + (d + 16) * 1024) >> 12);
    return (uint16_t)((r8 & 0xF8) | (((g8 & 0xE0) >> 5) | ((g8 & 0x1C) << 11)) | ((b8 & 0xF8) << 5));
}

void muse_yuv_1x_hq(const uint8_t *y, const uint8_t *u, const uint8_t *v, int ystride, int cstride,
                              int src_w, int src_h, int row, int rows, uint8_t *out, int out_stride)
{
    muse_yuv_1x_hq_cols(y, u, v, ystride, cstride, src_w, src_h, row, rows, 0, src_w, out, out_stride);
}

void muse_yuv_1x_hq_cols(const uint8_t *y, const uint8_t *u, const uint8_t *v, int ystride, int cstride,
                         int src_w, int src_h, int row, int rows, int x0, int x1, uint8_t *out, int out_stride)
{
    int cw = src_w / 2, ch = (src_h + 1) / 2;
    for (int r = 0; r < rows; r++) {
        int sy = row + r;                      /* source row */
        int cy = sy >> 1;                      /* its chroma row */
        int cy2 = sy & 1 ? cy + 1 : cy - 1;    /* the neighbour row it leans towards */
        cy2 = cy2 < 0 ? 0 : cy2 >= ch ? ch - 1 : cy2;
        const uint8_t *yr = y + (size_t)sy * ystride;
        const uint8_t *u0 = u + (size_t)cy * cstride, *u1 = u + (size_t)cy2 * cstride;
        const uint8_t *v0 = v + (size_t)cy * cstride, *v1 = v + (size_t)cy2 * cstride;
        uint16_t *o = (uint16_t *)(out + (size_t)r * out_stride);
        const int8_t *dith = s_bayer[sy & 3];
        for (int cx = x0 / 2; cx < x1 / 2; cx++) {
            int cl = cx > 0 ? cx - 1 : 0, cr = cx + 1 < cw ? cx + 1 : cw - 1;
            /* vertical blend (3:1) at this column and its neighbours, in Q2 */
            int uc = 3 * u0[cx] + u1[cx], ul = 3 * u0[cl] + u1[cl], ur = 3 * u0[cr] + u1[cr];
            int vc = 3 * v0[cx] + v1[cx], vl = 3 * v0[cl] + v1[cl], vr = 3 * v0[cr] + v1[cr];
            /* horizontal blend (3:1) for the left and right pixel: Q4, minus 128 */
            int ua = 3 * uc + ul - 2048, ub = 3 * uc + ur - 2048;
            int va = 3 * vc + vl - 2048, vb = 3 * vc + vr - 2048;
            int x = 2 * cx;
            o[x] = hq_px(HQ_Y * (yr[x] - 16), ua, va, dith[x & 3]);
            o[x + 1] = hq_px(HQ_Y * (yr[x + 1] - 16), ub, vb, dith[(x + 1) & 3]);
        }
    }
}

/* Q8 source position of each output column, for muse_yuv_scaled(). */
static uint32_t *s_xmap;
static int s_xmap_src, s_xmap_out;

int muse_yuv_src_rows(int src_h, int out_h, int out_rows)
{
    int last = (out_rows * src_h + out_h - 1) / out_h + 1;
    return last > src_h ? src_h : last;
}

void YUV_HOT muse_yuv_scaled(const uint8_t *y, const uint8_t *u, const uint8_t *v, int ystride, int cstride,
                               int src_w, int src_h, int out_w, int out_h, int row, int rows, uint8_t *out,
                               int out_stride)
{
    if (!s_init) {
        init_tables();
    }
    if (s_xmap_src != src_w || s_xmap_out != out_w) {
        free(s_xmap);
        s_xmap = malloc(out_w * sizeof(uint32_t));
        if (!s_xmap) {
            s_xmap_src = 0;
            return;
        }
        for (int x = 0; x < out_w; x++) {
            /* Pixel centres line up: (x + 0.5) * src / out - 0.5, clamped. */
            int p = ((2 * x + 1) * src_w * 128) / out_w - 128;
            p = p < 0 ? 0 : p > (src_w - 1) * 256 ? (src_w - 1) * 256 : p;
            s_xmap[x] = (uint32_t)p;
        }
        s_xmap_src = src_w;
        s_xmap_out = out_w;
    }
    for (int r = 0; r < rows; r++) {
        int oy = row + r;
        int py = ((2 * oy + 1) * src_h * 128) / out_h - 128;
        py = py < 0 ? 0 : py > (src_h - 1) * 256 ? (src_h - 1) * 256 : py;
        int sy = py >> 8, fy = py & 255;
        const uint8_t *y0 = y + sy * ystride, *y1 = sy + 1 < src_h ? y0 + ystride : y0;
        int cy = (py + 128) >> 9;
        const uint8_t *cu = u + cy * cstride, *cv = v + cy * cstride;
        uint16_t *o = (uint16_t *)(out + r * out_stride);
        for (int x = 0; x < out_w; x++) {
            int px = s_xmap[x], sx = px >> 8, fx = px & 255;
            int sx1 = sx + 1 < src_w ? sx + 1 : sx;
            int top = y0[sx] * 256 + (y0[sx1] - y0[sx]) * fx;
            int bot = y1[sx] * 256 + (y1[sx1] - y1[sx]) * fx;
            int Y = (top * 256 + (bot - top) * fy + 32768) >> 16;
            int ci = (px + 128) >> 9;
            int U = cu[ci], V = cv[ci];
            o[x] = px1(s_y[Y], s_rv[V], s_gu[U] + s_gv[V], s_bu[U]);
        }
    }
}

/* muse_yuv_fill(): one source row converted, and which source column each
 * output column takes (its key: what it was built for). The show task's only. */
#define FILL_MAX_SRC MUSE_YUV_FILL_MAX_SRC
#define FILL_MAX_OUT 512
static uint16_t s_fline[FILL_MAX_SRC];
static int s_fkey[4] = { -1, -1, -1, -1 };

int muse_yuv_fill_src_rows(int src_h, int num, int den, int crop_y, int out_rows)
{
    return out_rows > 0 ? muse_video_fill_src(out_rows - 1, crop_y, num, den, src_h) + 1 : 0;
}

/* Each source column's run of output columns (its repeat count), from the first source column used. */
static uint8_t s_frun[FILL_MAX_SRC];
static int s_fs0, s_fs1, s_frun_min, s_frun_max;   /* the runs' shortest and longest (the ends' included) */


static void fill_xmap(int src_w, int num, int den, int crop_x, int out_w)
{
    if (s_fkey[0] != src_w || s_fkey[1] != (num << 12 | den) || s_fkey[2] != crop_x || s_fkey[3] != out_w) {
        memset(s_frun, 0, sizeof(s_frun));
        for (int x = 0; x < out_w; x++) {
            int sx = muse_video_fill_src(x, crop_x, num, den, src_w);
            if (s_frun[sx] < 255) {
                s_frun[sx]++;
            }
        }
        s_fs0 = muse_video_fill_src(0, crop_x, num, den, src_w);
        s_fs1 = muse_video_fill_src(out_w - 1, crop_x, num, den, src_w);
        s_frun_min = 255;
        s_frun_max = 0;
        for (int i = s_fs0; i <= s_fs1; i++) {
            s_frun_min = s_frun[i] < s_frun_min ? s_frun[i] : s_frun_min;
            s_frun_max = s_frun[i] > s_frun_max ? s_frun[i] : s_frun_max;
        }
        s_fkey[0] = src_w;
        s_fkey[1] = num << 12 | den;
        s_fkey[2] = crop_x;
        s_fkey[3] = out_w;
    }
}

void muse_yuv_fill_row565(const uint16_t *src, int src_w, int num, int den, int crop_x, int out_w,
                                    uint16_t *out)
{
    if (src_w > FILL_MAX_SRC || out_w > FILL_MAX_OUT || src_w < 1 || num <= 0 || den <= 0) {
        memset(out, 0, (size_t)out_w * 2);
        return;
    }
    fill_xmap(src_w, num, den, crop_x, out_w);
    uint16_t *d = out;
    for (int x = s_fs0; x <= s_fs1; x++) {
        for (int k = 0; k < s_frun[x]; k++) {
            *d++ = src[x];
        }
    }
}

/* The last output row made, kept for the next piece's first row (its source row,
 * -1: none; a new frame starts at output row 0 and forgets it). */
static uint16_t s_frow[FILL_MAX_OUT + 3];
static int s_frow_sy = -1;
static uint32_t s_fill_cycles, s_fill_cconv, s_fill_cexp, s_fill_ccopy;   /* the last three: a breakdown (bench) */

uint32_t muse_yuv_fill_cycles(void)
{
    uint32_t c = s_fill_cycles;
    s_fill_cycles = 0;
    return c;
}

void muse_yuv_fill_breakdown(uint32_t *conv, uint32_t *exp, uint32_t *copy)
{
    *conv = s_fill_cconv;
    *exp = s_fill_cexp;
    *copy = s_fill_ccopy;
    s_fill_cconv = s_fill_cexp = s_fill_ccopy = 0;
}

static inline uint32_t cycles_now(void)
{
#if defined(ESP_PLATFORM)
    return (uint32_t)esp_cpu_get_cycle_count();
#else
    return 0;
#endif
}

void IRAM_ATTR muse_yuv_fill(const uint8_t *y, const uint8_t *u, const uint8_t *v, int ystride, int cstride,
                             int src_w, int src_h, int num, int den, int crop_x, int crop_y, int out_w, int row,
                             int rows, uint8_t *out, int out_stride)
{
    if (!s_init) {
        init_tables();
    }
    if (src_w > FILL_MAX_SRC || out_w > FILL_MAX_OUT || src_w < 2 || num <= 0 || den <= 0) {
        for (int r = 0; r < rows; r++) {
            memset(out + (size_t)r * out_stride, 0, (size_t)out_w * 2);
        }
        return;
    }
    uint32_t c0 = cycles_now();
    fill_xmap(src_w, num, den, crop_x, out_w);
    if (row == 0) {
        s_frow_sy = -1;   /* a new frame */
    }
    const size_t n = (size_t)out_w * 2;
    int last = s_frow_sy;   /* the source row the row above holds (the piece before's last: s_frow) */
    for (int r = 0; r < rows; r++) {
        int sy = muse_video_fill_src(row + r, crop_y, num, den, src_h);
        uint16_t *o = (uint16_t *)(out + (size_t)r * out_stride);
        if (sy == last) {
            /* The same source row again: a copy of the row above (the piece before's, kept, for a piece's first). */
            uint32_t t = cycles_now();
            memcpy(o, r ? (const uint8_t *)o - out_stride : (const uint8_t *)s_frow, n);
            s_fill_ccopy += cycles_now() - t;
            continue;
        }
        uint32_t t0 = cycles_now();
        const uint8_t *yr = y + (size_t)sy * ystride;
        const uint8_t *cu = u + (size_t)(sy / 2) * cstride, *cv = v + (size_t)(sy / 2) * cstride;
        const int x0 = s_fs0 & ~1, x1 = s_fs1 + 1;
        for (int x = x0; x < x1; x += 2) {
            int U = cu[x >> 1], V = cv[x >> 1];
            int rv = s_rv[V], g = s_gu[U] + s_gv[V], bu = s_bu[U];
            s_fline[x] = px1(s_y[yr[x]], rv, g, bu);
            s_fline[x + 1] = px1(s_y[yr[x + 1]], rv, g, bu);
        }
        uint32_t t1 = cycles_now();
        s_fill_cconv += t1 - t0;
        /* Each source pixel written its run of times: two or three at this scale. */
        uint16_t *d = o;
        if (s_frun_min >= 2 && s_frun_max <= 3) {
            /* Two or three each (scales 2-3): two always, a third that the next pixel overwrites when the run is
             * two; the last pixel on its own, so nothing is written past the row. */
            for (int x = s_fs0; x < s_fs1; x++) {
                uint16_t px = s_fline[x];
                d[0] = px;
                d[1] = px;
                d[2] = px;
                d += s_frun[x];
            }
            for (int k = 0; k < s_frun[s_fs1]; k++) {
                d[k] = s_fline[s_fs1];
            }
            s_fill_cexp += cycles_now() - t1;
            last = sy;
            continue;
        }
        for (int x = s_fs0; x <= s_fs1; x++) {
            uint16_t px = s_fline[x];
            switch (s_frun[x]) {
            case 3:
                d[2] = px;
                /* fall through */
            case 2:
                d[1] = px;
                /* fall through */
            case 1:
                d[0] = px;
                /* fall through */
            case 0:
                break;
            default:
                for (int k = 0; k < s_frun[x]; k++) {
                    d[k] = px;
                }
                break;
            }
            d += s_frun[x];
        }
        s_fill_cexp += cycles_now() - t1;
        last = sy;
    }
    /* The piece's last row, for the next piece's first if it's the same source row. */
    if (rows > 0) {
        memcpy(s_frow, out + (size_t)(rows - 1) * out_stride, n);
        s_frow_sy = last;
    }
    s_fill_cycles += cycles_now() - c0;
}

void YUV_HOT muse_yuv_fill_win(const uint8_t *y, const uint8_t *u, const uint8_t *v, int ystride, int cstride,
                               int src_w, int src_h, int num, int den, int crop_x, int crop_y, int out_x, int w,
                               int row, int rows, bool first, uint8_t *out, int out_stride)
{
    if (first) {
        s_frow_sy = -1;   /* the kept row is another window's (or the whole picture's) */
    }
    /* Output column o of the window is the picture's out_x + o: the same source column with the crop moved. */
    muse_yuv_fill(y, u, v, ystride, cstride, src_w, src_h, num, den, crop_x + out_x, crop_y, w, row, rows, out,
                  out_stride);
}

/* 2 -> 3 along a row: a, (a + b + 1) / 2, b. */
static void YUV_HOT expand32(const uint8_t *s, int n, uint8_t *d)
{
    for (int i = 0; i < n; i += 2) {
        int a = s[i], b = s[i + 1];
        d[0] = (uint8_t)a;
        d[1] = (uint8_t)((a + b + 1) >> 1);
        d[2] = (uint8_t)b;
        d += 3;
    }
}

static void YUV_HOT avg_rows(const uint8_t *a, const uint8_t *b, int n, uint8_t *d)
{
    const uint32_t *wa = (const uint32_t *)a, *wb = (const uint32_t *)b;
    uint32_t *wd = (uint32_t *)d;
    for (int i = 0; i < n / 4; i++) {
        uint32_t x = wa[i], z = wb[i];
        wd[i] = (x | z) - (((x ^ z) & 0xFEFEFEFEu) >> 1);   /* per byte (x + z + 1) / 2 */
    }
}

/* Source row r of a plane, 3/2 expanded, in out (out_n + 15 bytes). Source
 * rows at or past src_h (the plane's rows) are never read: the last one
 * stands in for them. */
static const uint8_t *row32(const uint8_t *plane, int stride, int src_w, int src_h, int r2, uint8_t *tmp_a,
                            uint8_t *tmp_b, uint8_t *out)
{
    /* Output row r2 of this plane: rows 3k, 3k + 1, 3k + 2 come from source
     * rows 2k, (2k + 2k + 1) / 2 and 2k + 1. */
    int k = r2 / 3, ph = r2 % 3;
    int r0 = 2 * k < src_h ? 2 * k : src_h - 1, r1 = 2 * k + 1 < src_h ? 2 * k + 1 : src_h - 1;
    const uint8_t *s0 = plane + (size_t)r0 * stride, *s1 = plane + (size_t)r1 * stride;
    if (ph == 0) {
        expand32(s0, src_w, out);
    } else if (ph == 2) {
        expand32(s1, src_w, out);
    } else {
        expand32(s0, src_w, tmp_a);
        expand32(s1, src_w, tmp_b);
        avg_rows(tmp_a, tmp_b, src_w * 3 / 2, out);
    }
    return out;
}

int muse_yuv_up32_src_rows(int src_h, int out_rows)
{
    /* Output rows < out_rows read source rows up to 2 * ceil(out_rows / 3) + 1,
     * chroma included (its rows pair the luma's). */
    int need = 2 * ((out_rows + 2) / 3) + 2;
    return need > src_h ? src_h : need;
}

void YUV_HOT muse_yuv_up32(const uint8_t *y, const uint8_t *u, const uint8_t *v, int ystride, int cstride,
                             int src_w, int src_h, int row, int rows, uint8_t *out, int out_stride)
{
    enum { MAXW = 512 };   /* a multiple of 16, as the SIMD kernels want strides */
    /* Rows for the 3/2 scale, taken the first time it's used (only 320 px
     * pictures need it): 2.5 KB of internal RAM other builds keep. */
    static uint8_t *buf;
    if (!buf) {
        buf = heap_caps_aligned_alloc(16, 5 * MAXW, MUSE_SIMD_CAPS);   /* main SRAM: SIMD rows (muse_mem.h) */
        if (!buf) {
            return;
        }
    }
    uint8_t *yp[2] = {buf, buf + MAXW}, *cu = buf + 2 * MAXW, *cv = cu + MAXW / 2, *t0 = buf + 3 * MAXW,
            *t1 = buf + 4 * MAXW;
    int out_w = src_w * 3 / 2;
    if (out_w > 480 || src_h < 2) {
        return;
    }
    for (int r = row; r < row + rows; r += 2) {
        /* The slice's rows only: an odd slice ends on a pair's first row. */
        int n = row + rows - r < 2 ? 1 : 2;
        row32(y, ystride, src_w, src_h, r, t0, t1, yp[0]);
        if (n == 2) {
            row32(y, ystride, src_w, src_h, r + 1, t0, t1, yp[1]);
        }
        /* The pair's chroma is row r / 2 of the 3/2 expanded chroma plane. */
        row32(u, cstride, src_w / 2, src_h / 2, r / 2, t0, t1, cu);
        row32(v, cstride, src_w / 2, src_h / 2, r / 2, t0, t1, cv);
        muse_yuv_1x(yp[0], cu, cv, MAXW, MAXW / 2, out_w, n, out + (size_t)(r - row) * out_stride, out_stride);
    }
}

/*
 * The SIMD kernels against the C path on a test picture (every Y, U and V
 * value turns up), at the S31's first conversion. Any difference keeps the C
 * path, which gives the same picture at about seven times the cost.
 */
static void selftest(void)
{
#if CONFIG_IDF_TARGET_ESP32S31
    enum { W = 64, H = 4, OW = 2 * W };
    static uint8_t y[W * H] __attribute__((aligned(16)));
    static uint8_t u[W / 2 * H / 2] __attribute__((aligned(16))), v[W / 2 * H / 2] __attribute__((aligned(16)));
    static uint8_t a[OW * 2 * H * 2] __attribute__((aligned(16))), b[OW * 2 * H * 2] __attribute__((aligned(16)));
    uint32_t seed = 12345;
    bool same = true;
    for (int round = 0; round < 32 && same; round++) {
        for (int i = 0; i < W * H; i++) {
            seed = seed * 1103515245u + 12345u;
            y[i] = round < 2 ? (uint8_t)(i * 4 + round) : (uint8_t)(seed >> 24);
        }
        for (int i = 0; i < W / 2 * H / 2; i++) {
            seed = seed * 1103515245u + 12345u;
            u[i] = round < 2 ? (uint8_t)(i * 8 + 3 * round) : (uint8_t)(seed >> 24);
            v[i] = round < 2 ? (uint8_t)(255 - i * 8) : (uint8_t)(seed >> 16);
        }
        for (int up = 0; up < 2 && same; up++) {
            memset(a, 0, sizeof(a));
            memset(b, 0xA5, sizeof(b));
            s_pie = false;
            if (up) {
                muse_yuv_up2(y, u, v, W, W / 2, W, 2 * H, a, OW * 2);
            } else {
                muse_yuv_1x(y, u, v, W, W / 2, W, H, a, W * 2);
            }
            s_pie = true;
            if (up) {
                muse_yuv_up2(y, u, v, W, W / 2, W, 2 * H, b, OW * 2);
            } else {
                muse_yuv_1x(y, u, v, W, W / 2, W, H, b, W * 2);
            }
            size_t n = up ? (size_t)OW * 2 * H * 2 : (size_t)W * 2 * H;
            same = memcmp(a, b, n) == 0;
        }
    }
    s_pie = same;
    if (same) {
        ESP_LOGI("muse_yuv", "S31 SIMD colour conversion matches C: in use");
    } else {
        ESP_LOGW("muse_yuv", "S31 SIMD colour conversion differs from C: using C");
    }
#endif
}
