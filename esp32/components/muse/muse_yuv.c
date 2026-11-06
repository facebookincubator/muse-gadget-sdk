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

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"

#include "esp_attr.h"
#include "esp_log.h"
#include "sdkconfig.h"

/* Offset into the clamp tables, so a sum below 0 or above 255 still indexes them. */
#define OFS 384

/* Only the last pixels of a row use these: in PSRAM, to spare internal RAM. */
static uint16_t *s_tr, *s_tgh, *s_tb;
static int16_t *s_rv, *s_gu, *s_gv, *s_bu, *s_y;
static bool s_init;

/* The SIMD kernel's constants, 8 lanes each: 128, 204, 50, 104, 258, then 16,
 * 149, 255, 0xF8, 0xF8, 0xE0, 0x1C (see muse_yuv_pie.S). */
static int16_t s_k[12][8] __attribute__((aligned(16)));
static int16_t s_scratch[8][8] __attribute__((aligned(16)));

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
    s_tr = t;
    s_tgh = t + 1024;
    s_tb = t + 2048;
    s_y = c5;
    s_rv = c5 + 256;
    s_gu = c5 + 512;
    s_gv = c5 + 768;
    s_bu = c5 + 1024;
    for (int i = 0; i < 1024; i++) {
        int c = i - OFS;
        c = c < 0 ? 0 : c > 255 ? 255 : c;
        s_tr[i] = (uint16_t)(c & 0xF8);
        s_tgh[i] = (uint16_t)(((c & 0xE0) >> 5) | ((c & 0x1C) << 11));
        s_tb[i] = (uint16_t)((c & 0xF8) << 5);
    }
    for (int i = 0; i < 256; i++) {
        s_y[i] = (int16_t)((((i - 16) * 149) >> 7) + OFS);
        s_rv[i] = (int16_t)(((i - 128) * 204) >> 7);
        s_gu[i] = (int16_t)(((i - 128) * 50) >> 7);
        s_gv[i] = (int16_t)(((i - 128) * 104) >> 7);
        s_bu[i] = (int16_t)(((i - 128) * 258) >> 7);
    }
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

void IRAM_ATTR muse_yuv_up2(const uint8_t *y, const uint8_t *u, const uint8_t *v, int ystride, int cstride,
                            int src_w, int src_rows, uint8_t *out, int out_stride)
{
    if (!s_init) {
        init_tables();
    }
    int simd_w = s_pie ? src_w & ~15 : 0;
    for (int r = 0; r < src_rows; r += 2) {
        const uint8_t *y0 = y + r * ystride, *y1 = y0 + ystride;
        const uint8_t *cu = u + (r / 2) * cstride, *cv = v + (r / 2) * cstride;
        uint8_t *o0 = out + 2 * r * out_stride, *o2 = o0 + 2 * out_stride;
#if HAVE_PIE
        if (simd_w) {
            muse_yuv_pie_2rows_up2(y0, y1, cu, cv, o0, o2, simd_w / 16, &s_k[0][0], &s_scratch[0][0],
                                   (uint32_t)out_stride);
        }
#endif
        uint32_t *w0 = (uint32_t *)o0, *w1 = (uint32_t *)(o0 + out_stride);
        uint32_t *w2 = (uint32_t *)o2, *w3 = (uint32_t *)(o2 + out_stride);
        for (int x = simd_w; x < src_w; x += 2) {
            int U = cu[x >> 1], V = cv[x >> 1];
            int rv = s_rv[V], g = s_gu[U] + s_gv[V], bu = s_bu[U];
            uint32_t p;
            p = px2(s_y[y0[x]], rv, g, bu);
            w0[x] = p;
            w1[x] = p;
            p = px2(s_y[y0[x + 1]], rv, g, bu);
            w0[x + 1] = p;
            w1[x + 1] = p;
            p = px2(s_y[y1[x]], rv, g, bu);
            w2[x] = p;
            w3[x] = p;
            p = px2(s_y[y1[x + 1]], rv, g, bu);
            w2[x + 1] = p;
            w3[x + 1] = p;
        }
    }
}

static inline uint16_t px1(int y, int rv, int g, int bu)
{
    return s_tr[y + rv] | s_tgh[y - g] | s_tb[y + bu];
}

void IRAM_ATTR muse_yuv_1x(const uint8_t *y, const uint8_t *u, const uint8_t *v, int ystride, int cstride,
                           int src_w, int src_rows, uint8_t *out, int out_stride)
{
    if (!s_init) {
        init_tables();
    }
    int simd_w = s_pie ? src_w & ~15 : 0;
    for (int r = 0; r < src_rows; r += 2) {
        const uint8_t *y0 = y + r * ystride, *y1 = y0 + ystride;
        const uint8_t *cu = u + (r / 2) * cstride, *cv = v + (r / 2) * cstride;
        uint32_t *w0 = (uint32_t *)(out + r * out_stride), *w1 = (uint32_t *)(out + (r + 1) * out_stride);
#if HAVE_PIE
        if (simd_w) {
            muse_yuv_pie_2rows(y0, y1, cu, cv, (uint8_t *)w0, (uint8_t *)w1, simd_w / 16, &s_k[0][0], &s_scratch[0][0]);
        }
#endif
        for (int x = simd_w; x < src_w; x += 2) {
            int U = cu[x >> 1], V = cv[x >> 1];
            int rv = s_rv[V], g = s_gu[U] + s_gv[V], bu = s_bu[U];
            w0[x >> 1] = px1(s_y[y0[x]], rv, g, bu) | (uint32_t)px1(s_y[y0[x + 1]], rv, g, bu) << 16;
            w1[x >> 1] = px1(s_y[y1[x]], rv, g, bu) | (uint32_t)px1(s_y[y1[x + 1]], rv, g, bu) << 16;
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

void IRAM_ATTR muse_yuv_scaled(const uint8_t *y, const uint8_t *u, const uint8_t *v, int ystride, int cstride,
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

/* 2 -> 3 along a row: a, (a + b + 1) / 2, b. */
static void IRAM_ATTR expand32(const uint8_t *s, int n, uint8_t *d)
{
    for (int i = 0; i < n; i += 2) {
        int a = s[i], b = s[i + 1];
        d[0] = (uint8_t)a;
        d[1] = (uint8_t)((a + b + 1) >> 1);
        d[2] = (uint8_t)b;
        d += 3;
    }
}

static void IRAM_ATTR avg_rows(const uint8_t *a, const uint8_t *b, int n, uint8_t *d)
{
    const uint32_t *wa = (const uint32_t *)a, *wb = (const uint32_t *)b;
    uint32_t *wd = (uint32_t *)d;
    for (int i = 0; i < n / 4; i++) {
        uint32_t x = wa[i], z = wb[i];
        wd[i] = (x | z) - (((x ^ z) & 0xFEFEFEFEu) >> 1);   /* per byte (x + z + 1) / 2 */
    }
}

/* Source row r of a plane, 3/2 expanded, in out (out_n + 15 bytes). */
static const uint8_t *row32(const uint8_t *plane, int stride, int src_w, int r2, uint8_t *tmp_a, uint8_t *tmp_b,
                            uint8_t *out)
{
    /* Output row r2 of this plane: rows 3k, 3k + 1, 3k + 2 come from source
     * rows 2k, (2k + 2k + 1) / 2 and 2k + 1. */
    int k = r2 / 3, ph = r2 % 3;
    const uint8_t *s0 = plane + (size_t)(2 * k) * stride;
    if (ph == 0) {
        expand32(s0, src_w, out);
    } else if (ph == 2) {
        expand32(s0 + stride, src_w, out);
    } else {
        expand32(s0, src_w, tmp_a);
        expand32(s0 + stride, src_w, tmp_b);
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

void IRAM_ATTR muse_yuv_up32(const uint8_t *y, const uint8_t *u, const uint8_t *v, int ystride, int cstride,
                             int src_w, int src_h, int row, int rows, uint8_t *out, int out_stride)
{
    enum { MAXW = 512 };   /* a multiple of 16, as the SIMD kernels want strides */
    /* Rows for the 3/2 scale, taken the first time it's used (only 320 px
     * pictures need it): 2.5 KB of internal RAM other builds keep. */
    static uint8_t *buf;
    if (!buf) {
        buf = heap_caps_aligned_alloc(16, 5 * MAXW, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!buf) {
            return;
        }
    }
    uint8_t *yp[2] = {buf, buf + MAXW}, *cu = buf + 2 * MAXW, *cv = cu + MAXW / 2, *t0 = buf + 3 * MAXW,
            *t1 = buf + 4 * MAXW;
    int out_w = src_w * 3 / 2;
    if (out_w > 480) {
        return;
    }
    (void)src_h;
    for (int r = row; r < row + rows; r += 2) {
        row32(y, ystride, src_w, r, t0, t1, yp[0]);
        row32(y, ystride, src_w, r + 1, t0, t1, yp[1]);
        /* The pair's chroma is row r / 2 of the 3/2 expanded chroma plane. */
        row32(u, cstride, src_w / 2, r / 2, t0, t1, cu);
        row32(v, cstride, src_w / 2, r / 2, t0, t1, cv);
        muse_yuv_1x(yp[0], cu, cv, MAXW, MAXW / 2, out_w, 2, out + (size_t)(r - row) * out_stride, out_stride);
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
                muse_yuv_up2(y, u, v, W, W / 2, W, H, a, OW * 2);
            } else {
                muse_yuv_1x(y, u, v, W, W / 2, W, H, a, W * 2);
            }
            s_pie = true;
            if (up) {
                muse_yuv_up2(y, u, v, W, W / 2, W, H, b, OW * 2);
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
