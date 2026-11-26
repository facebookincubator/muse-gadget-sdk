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
#include "muse_ui_fb.h"

#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"

#include "muse_yuv.h"

/* Patches inflate with the ROM's tinfl (esp_rom's miniz.h): the ESP32-S3's and
 * the ESP32-S31's ROMs both have it. */
#if CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32S31
#include "miniz.h"
#define HAVE_TINFL 1
#else
#define HAVE_TINFL 0
#endif
/* Room past the decompressor, in case a ROM's tinfl_decompressor is larger
 * than esp_rom's header says: the self-test below would then fail instead
 * of writing past the block. */
#define TINFL_SLACK 1024
/* Room past the output: the S3 and S31 ROMs' tinfl_decompress decode length codes 286/287
 * as a zero-length copy that still writes 3 bytes past the bytes it reports, so every output buffer gets this much
 * spare beyond what tinfl is told it may fill. */
#define TINFL_OUT_SPARE 8

static const char *TAG = "muse_ui_fb";

static uint16_t *s_fb;               /* RGB565, panel byte order, w x h */
static uint8_t *s_y, *s_u, *s_v;     /* the last decoded picture (crop), for the comparison */
static int s_w, s_h;
/* Per macroblock: MB_CHANGED this picture (scratch), MB_PATCHED holds a still
 * patch's exact pixels (never converted over until its own picture changes). */
#define MB_CHANGED 1
#define MB_PATCHED 2
static uint8_t *s_mb;
static SemaphoreHandle_t s_lock;
static int s_last_mbs, s_last_total, s_skipped, s_last_skipped;
/* The picture each band last brought here: the copy holds it, all of it. */
static uint32_t s_seq[2];
static uint32_t s_last_us;

bool muse_ui_fb_begin(int w, int h)
{
    muse_ui_fb_end();
    w &= ~1;
    h &= ~1;
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
    }
    const uint32_t caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    s_fb = heap_caps_calloc((size_t)w * h, 2, caps);
    s_y = heap_caps_malloc((size_t)w * h, caps);
    s_u = heap_caps_malloc((size_t)w * h / 4, caps);
    s_v = heap_caps_malloc((size_t)w * h / 4, caps);
    s_mb = heap_caps_calloc((size_t)((w + 15) / 16) * ((h + 15) / 16), 1, caps);
    if (!s_lock || !s_fb || !s_y || !s_u || !s_v || !s_mb) {
        ESP_LOGE(TAG, "no memory for a %dx%d frame buffer", w, h);
        muse_ui_fb_end();
        return false;
    }
    s_w = w;
    s_h = h;
    return true;
}

void muse_ui_fb_end(void)
{
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    free(s_fb);
    free(s_y);
    free(s_u);
    free(s_v);
    free(s_mb);
    s_fb = NULL;
    s_mb = NULL;
    s_seq[0] = s_seq[1] = 0;
    s_y = s_u = s_v = NULL;
    s_w = s_h = 0;
    if (s_lock) {
        xSemaphoreGive(s_lock);
    }
}

bool muse_ui_fb_active(void)
{
    return s_fb != NULL;
}

/* Row r of a plane, both bands stacked (r in the plane's own rows). */
static inline const uint8_t *prow(const muse_pic_t *p0, const muse_pic_t *p1, int h0, int plane, int r)
{
    int div = plane ? 2 : 1;
    const muse_pic_t *p = r * div < h0 || !p1 ? p0 : p1;
    int br = p == p0 ? r : r - h0 / div;
    const uint8_t *base = plane == 0 ? p->y : plane == 1 ? p->u : p->v;
    return base + (size_t)br * (plane ? p->c_stride : p->y_stride);
}

/* Known unchanged from the copy without comparing: the decoder says the
 * macroblock is its previous picture's exactly, and that picture is the one
 * the copy holds (muse_vdec_same.h). */
static inline bool mb_same(const muse_pic_t *p0, const muse_pic_t *p1, int h0, int mx, int my)
{
    int band = p1 && my * 16 >= h0;
    const muse_pic_t *p = band ? p1 : p0;
    int row = band ? my - h0 / 16 : my;
    return p->same && p->same_as && p->same_as == s_seq[band] && p->same[row * p->same_cols + mx];
}

/* Does macroblock (mx, my) differ from the copy? Copies it when it does (or when all). */
static bool mb_changed(const muse_pic_t *p0, const muse_pic_t *p1, int h0, int mx, int my, bool all)
{
    if (!all && mb_same(p0, p1, h0, mx, my)) {
        s_skipped++;
        return false;
    }
    int x = mx * 16, y = my * 16;
    int w = s_w - x < 16 ? s_w - x : 16, h = s_h - y < 16 ? s_h - y : 16;
    bool diff = all;
    for (int r = 0; r < h && !diff; r++) {
        diff = memcmp(prow(p0, p1, h0, 0, y + r) + x, s_y + (size_t)(y + r) * s_w + x, w) != 0;
    }
    for (int r = 0; r < h / 2 && !diff; r++) {
        size_t o = (size_t)(y / 2 + r) * (s_w / 2) + x / 2;
        diff = memcmp(prow(p0, p1, h0, 1, y / 2 + r) + x / 2, s_u + o, w / 2) != 0 ||
               memcmp(prow(p0, p1, h0, 2, y / 2 + r) + x / 2, s_v + o, w / 2) != 0;
    }
    if (!diff) {
        return false;
    }
    for (int r = 0; r < h; r++) {
        memcpy(s_y + (size_t)(y + r) * s_w + x, prow(p0, p1, h0, 0, y + r) + x, w);
    }
    for (int r = 0; r < h / 2; r++) {
        size_t o = (size_t)(y / 2 + r) * (s_w / 2) + x / 2;
        memcpy(s_u + o, prow(p0, p1, h0, 1, y / 2 + r) + x / 2, w / 2);
        memcpy(s_v + o, prow(p0, p1, h0, 2, y / 2 + r) + x / 2, w / 2);
    }
    return true;
}

int muse_ui_fb_picture(const muse_pic_t *p0, const muse_pic_t *p1, int h0, bool all, muse_ui_rect_t *rects)
{
    if (!s_fb) {
        return 0;
    }
    /* Each band read at the copy's width, the bottom from h0 down: both must cover it. */
    if (!muse_pic_covers(p0, s_w, p1 ? h0 : s_h) || (p1 && (h0 <= 0 || h0 >= s_h || h0 % 16 ||
                                                            !muse_pic_covers(p1, s_w, s_h - h0)))) {
        return -1;
    }
    int64_t t0 = esp_timer_get_time();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int mw = (s_w + 15) / 16, mh = (s_h + 15) / 16, n = 0, changed = 0;
    if (!p1) {
        h0 = s_h;
    }
    /* First which macroblocks changed (the copy takes the new picture's). */
    s_skipped = 0;
    for (int my = 0; my < mh; my++) {
        for (int mx = 0; mx < mw; mx++) {
            uint8_t *f = &s_mb[my * mw + mx];
            /* all (a keyframe, a woken screen) converts everything, except a lossless still patch whose
             * macroblock the picture didn't change: that keeps the patch (drawn below as the copy holds it). */
            if (mb_changed(p0, p1, h0, mx, my, all && !(*f & MB_PATCHED))) {
                *f = MB_CHANGED;   /* converted below: no longer a patch's */
                changed++;
            } else {
                *f &= (uint8_t)~MB_CHANGED;
            }
        }
    }
    /* Per macroblock row: the span of macroblocks to convert from the copy,
     * merged into rectangles. That's the changed ones, and their unchanged
     * neighbours unless a still patch holds them: the chroma blend reaches
     * one pixel into the next macroblock, so a neighbour's edge pixels
     * depend on what changed. Its other pixels convert to what it had. */
    int cur = -1;   /* the rectangle the previous row's span went into */
    for (int my = 0; my < mh; my++) {
        int lo = mw, hi = -1;
        int y0 = my * 16, rows = s_h - y0 < 16 ? s_h - y0 : 16;
        for (int mx = 0; mx < mw; mx++) {
            uint8_t f = s_mb[my * mw + mx];
            bool conv = f & MB_CHANGED;
            for (int dy = -1; !conv && !(f & MB_PATCHED) && dy <= 1; dy++) {
                for (int dx = -1; dx <= 1 && !conv; dx++) {
                    int nx = mx + dx, ny = my + dy;
                    conv = nx >= 0 && ny >= 0 && nx < mw && ny < mh && (s_mb[ny * mw + nx] & MB_CHANGED);
                }
            }
            if (!conv && all && (f & MB_PATCHED)) {
                lo = mx < lo ? mx : lo;   /* a kept patch: drawn (all redraws everything), not converted */
                hi = mx;
            }
            if (conv) {
                lo = mx < lo ? mx : lo;
                hi = mx;
                /* Convert only these: any other macroblock keeps what the
                 * frame buffer has (a still patch, maybe), even inside the
                 * rectangle drawn. */
                int cx1 = (mx + 1) * 16 < s_w ? (mx + 1) * 16 : s_w;
                muse_yuv_1x_hq_cols(s_y, s_u, s_v, s_w, s_w / 2, s_w, s_h, y0, rows, mx * 16, cx1,
                                    (uint8_t *)(s_fb + (size_t)y0 * s_w), s_w * 2);
            }
        }
        if (hi < 0) {
            cur = -1;
            continue;
        }
        int x0 = lo * 16, x1 = (hi + 1) * 16 < s_w ? (hi + 1) * 16 : s_w;
        /* Extend the previous rectangle when the spans line up roughly. */
        if (cur >= 0) {
            muse_ui_rect_t *r = &rects[cur];
            int ux0 = r->x < x0 ? r->x : x0, ux1 = r->x + r->w > x1 ? r->x + r->w : x1;
            int area_union = (ux1 - ux0) * (r->h + rows), area_sep = r->w * r->h + (x1 - x0) * rows;
            if (area_union <= area_sep + area_sep / 4) {
                r->x = (uint16_t)ux0;
                r->w = (uint16_t)(ux1 - ux0);
                r->h = (uint16_t)(r->h + rows);
                continue;
            }
        }
        if (n == MUSE_UI_MAX_RECTS) {
            /* Too many: one rectangle around all of them. */
            int bx0 = s_w, by0 = s_h, bx1 = 0, by1 = 0;
            for (int i = 0; i < n; i++) {
                bx0 = rects[i].x < bx0 ? rects[i].x : bx0;
                by0 = rects[i].y < by0 ? rects[i].y : by0;
                bx1 = rects[i].x + rects[i].w > bx1 ? rects[i].x + rects[i].w : bx1;
                by1 = rects[i].y + rects[i].h > by1 ? rects[i].y + rects[i].h : by1;
            }
            bx0 = x0 < bx0 ? x0 : bx0;
            bx1 = x1 > bx1 ? x1 : bx1;
            by1 = y0 + rows > by1 ? y0 + rows : by1;
            rects[0] = (muse_ui_rect_t){ (uint16_t)bx0, (uint16_t)by0, (uint16_t)(bx1 - bx0), (uint16_t)(by1 - by0) };
            n = 1;
            cur = 0;
            continue;
        }
        rects[n] = (muse_ui_rect_t){ (uint16_t)x0, (uint16_t)y0, (uint16_t)(x1 - x0), (uint16_t)rows };
        cur = n++;
    }
    s_seq[0] = p0->seq;
    s_seq[1] = p1 ? p1->seq : 0;
    xSemaphoreGive(s_lock);
    s_last_mbs = changed;
    s_last_total = mw * mh;
    s_last_skipped = s_skipped;
    s_last_us = (uint32_t)(esp_timer_get_time() - t0);
    return n;
}

#if HAVE_TINFL
/* One zlib stream through the ROM's inflate into out (exactly out_len bytes). out must hold out_len +
 * TINFL_OUT_SPARE bytes: tinfl is told out_len, and the spare takes the ROM's stray 3-byte write. */
static bool inflate_exact(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_len)
{
    tinfl_decompressor *dec = heap_caps_malloc(sizeof(*dec) + TINFL_SLACK, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!dec) {
        return false;
    }
    tinfl_init(dec);
    size_t got = out_len;
    /* TINFL_FLAG_HAS_MORE_INPUT: a stream cut short returns NEEDS_MORE_INPUT. Without it the ROM's tinfl reads
     * zeros past the input's end, and a short stream (16 bytes) never finishes. Only DONE, with every byte, is
     * a patch. */
    tinfl_status st = tinfl_decompress(dec, in, &in_len, out, out, &got,
                                       TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF |
                                           TINFL_FLAG_HAS_MORE_INPUT);
    free(dec);
    if (st != TINFL_STATUS_DONE || got != out_len) {
        ESP_LOGW(TAG, "inflate status %d, %u of %u bytes", (int)st, (unsigned)got, (unsigned)out_len);
        return false;
    }
    return true;
}
#endif

bool muse_ui_fb_can_inflate(void)
{
#if HAVE_TINFL
    /* zlib.compress(bytes(i * 7 & 255 for i in range(300)) + bytes(200), 9):
     * literals, a match and stored lengths, through the same flags as a patch. */
    static const uint8_t z[] = {
        0x78, 0xda, 0x63, 0x60, 0xe7, 0x13, 0x95, 0x51, 0xd6, 0x32, 0xb4, 0xb0, 0x77, 0xf3, 0x0d, 0x89,
        0x4e, 0xca, 0x2c, 0x28, 0xaf, 0x6b, 0xed, 0x99, 0x3c, 0x6b, 0xe1, 0x8a, 0xf5, 0xdb, 0xf6, 0x1e,
        0x39, 0x7d, 0xe9, 0xe6, 0x83, 0xe7, 0xef, 0xbe, 0xfe, 0x61, 0xe6, 0x12, 0x94, 0x90, 0x57, 0xd3,
        0x35, 0xb1, 0x76, 0xf2, 0x0c, 0x08, 0x8f, 0x4b, 0xcd, 0x29, 0xae, 0x6a, 0xec, 0xe8, 0x9f, 0x36,
        0x77, 0xc9, 0xea, 0x4d, 0x3b, 0x0f, 0x1c, 0x3f, 0x77, 0xf5, 0xce, 0xe3, 0x57, 0x1f, 0x7f, 0xfc,
        0x67, 0xe3, 0x15, 0x91, 0x56, 0xd2, 0x34, 0x30, 0xb7, 0x73, 0xf5, 0x09, 0x8e, 0x4a, 0xcc, 0xc8,
        0x2f, 0xab, 0x6d, 0xe9, 0x9e, 0x34, 0x73, 0xc1, 0xf2, 0x75, 0x5b, 0xf7, 0x1c, 0x3e, 0x75, 0xf1,
        0xc6, 0xfd, 0x67, 0x6f, 0xbf, 0xfc, 0x66, 0xe2, 0x14, 0x10, 0x97, 0x53, 0xd5, 0x31, 0xb6, 0x72,
        0xf4, 0xf0, 0x0f, 0x8b, 0x4d, 0xc9, 0x2e, 0xaa, 0x6c, 0x68, 0xef, 0x9b, 0x3a, 0x67, 0xf1, 0xaa,
        0x8d, 0x3b, 0xf6, 0x1f, 0x3b, 0x7b, 0xe5, 0xf6, 0xa3, 0x97, 0x1f, 0xbe, 0xff, 0x63, 0xe5, 0x11,
        0x96, 0x52, 0xd4, 0xd0, 0x37, 0xb3, 0x75, 0xf1, 0x0e, 0x8a, 0x4c, 0x48, 0xcf, 0x2b, 0xad, 0x69,
        0xee, 0x9a, 0x38, 0x63, 0xfe, 0xb2, 0xb5, 0x5b, 0x76, 0x1f, 0x3a, 0x79, 0xe1, 0xfa, 0xbd, 0xa7,
        0x6f, 0x3e, 0xff, 0x62, 0xe4, 0xe0, 0x17, 0x93, 0x55, 0xd1, 0x36, 0xb2, 0x74, 0x70, 0xf7, 0x0b,
        0x8d, 0x49, 0xce, 0x2a, 0xac, 0xa8, 0x6f, 0xeb, 0x9d, 0x32, 0x7b, 0xd1, 0xca, 0x0d, 0xdb, 0xf7,
        0x1d, 0x3d, 0x73, 0xf9, 0xd6, 0xc3, 0x17, 0xef, 0xbf, 0xfd, 0x65, 0xe1, 0x16, 0x92, 0x54, 0x50,
        0xd7, 0x33, 0xb5, 0x71, 0xf6, 0x0a, 0x8c, 0x88, 0x4f, 0xcb, 0x2d, 0xa9, 0x6e, 0xea, 0x9c, 0x30,
        0x7d, 0xde, 0xd2, 0x35, 0x9b, 0x77, 0x1d, 0x3c, 0x71, 0xfe, 0xda, 0xdd, 0x27, 0xaf, 0x3f, 0xfd,
        0x64, 0x20, 0xc1, 0xff, 0x0c, 0xc3, 0x04, 0x00, 0x00, 0xbe, 0x80, 0x92, 0x5f,
    };
    static int s_ok = -1;
    if (s_ok < 0) {
        uint8_t *out = heap_caps_malloc(500 + TINFL_OUT_SPARE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        bool ok = out && inflate_exact(z, sizeof(z), out, 500);
        for (int i = 0; ok && i < 500; i++) {
            ok = out[i] == (i < 300 ? (uint8_t)(i * 7) : 0);
        }
        free(out);
        s_ok = ok;
        ESP_LOGI(TAG, "ROM inflate self-test: %s", ok ? "ok, still patches on" : "FAILED, no still patches");
    }
    return s_ok;
#else
    return false;
#endif
}

bool muse_ui_fb_still(const uint8_t *d, uint32_t n, muse_ui_rect_t *r)
{
#if HAVE_TINFL
    if (!s_fb || n < 9) {
        return false;
    }
    int x = d[0] | d[1] << 8, y = d[2] | d[3] << 8, w = d[4] | d[5] << 8, h = d[6] | d[7] << 8, fmt = d[8];
    if (fmt != 1 || w <= 0 || h <= 0 || (x | y | w | h) & 1 || x + w > s_w || y + h > s_h) {
        ESP_LOGW(TAG, "still patch refused: %dx%d at %d,%d format %d (picture %dx%d)", w, h, x, y, fmt, s_w, s_h);
        return false;
    }
    size_t out_len = (size_t)w * h * 2;
    uint8_t *out = heap_caps_malloc(out_len + TINFL_OUT_SPARE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    bool ok = out && inflate_exact(d + 9, n - 9, out, out_len);
    if (ok) {
        /* Big-endian RGB565 is the panel's byte order already. */
        xSemaphoreTake(s_lock, portMAX_DELAY);
        for (int row = 0; row < h; row++) {
            memcpy(s_fb + (size_t)(y + row) * s_w + x, out + (size_t)row * w * 2, (size_t)w * 2);
        }
        int mw = (s_w + 15) / 16;
        for (int my = y / 16; my <= (y + h - 1) / 16; my++) {
            for (int mx = x / 16; mx <= (x + w - 1) / 16; mx++) {
                s_mb[my * mw + mx] |= MB_PATCHED;
            }
        }
        xSemaphoreGive(s_lock);
        *r = (muse_ui_rect_t){ (uint16_t)x, (uint16_t)y, (uint16_t)w, (uint16_t)h };
    }
    free(out);
    return ok;
#else
    (void)d;
    (void)n;
    (void)r;
    return false;
#endif
}

void muse_ui_fb_fill(uint8_t *dst, int stride, int row, int rows, void *arg)
{
    const muse_ui_rect_t *r = arg;
    if (!s_fb) {
        memset(dst, 0, (size_t)rows * stride);
        return;
    }
    for (int i = 0; i < rows; i++) {
        memcpy(dst + (size_t)i * stride, s_fb + (size_t)(r->y + row + i) * s_w + r->x, (size_t)r->w * 2);
    }
}

const uint16_t *muse_ui_fb_pixels(int *w, int *h)
{
    *w = s_w;
    *h = s_h;
    return s_fb;
}

void muse_ui_fb_last_stats(int *mbs, int *total, int *skipped, uint32_t *convert_us)
{
    *mbs = s_last_mbs;
    *skipped = s_last_skipped;
    *total = s_last_total;
    *convert_us = s_last_us;
}
