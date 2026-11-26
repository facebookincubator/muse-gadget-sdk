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

/*
 * The avatar face's picture: a baseline JPEG in memory decoded by the ROM's
 * TJpgDec into a panel-sized RGB565 buffer (native byte order, LVGL's
 * LV_COLOR_FORMAT_RGB565), shrunk 1/2..1/8 and centered as face_place()
 * says, the overhang cut. No ESP-IDF beyond rom/tjpgd.h, so the host tests
 * run it against a decoder of their own (tests/test_mg_face.py).
 */
#include "mg_face_jpeg.h"

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#include "sdkconfig.h"
/* The decoder's work area in PSRAM: the face holds no internal RAM (mg_face.c). */
#define FACE_JPEG_POOL_ALLOC(n) heap_caps_malloc((n), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#else
#define FACE_JPEG_POOL_ALLOC(n) malloc(n)
#endif

#if !defined(ESP_PLATFORM) || CONFIG_MUSE_AVATAR_FACE

#include <stdlib.h>
#include <string.h>

#include "mg_face_core.h"
#include "rom/tjpgd.h"

/* The work pool TJpgDec's documentation asks for (image_fetch.c's). */
#define FACE_JPEG_POOL 3100

typedef struct {
    const uint8_t *src;
    size_t len, pos;
    uint16_t *out;
    int w, h, x0, y0;
} face_jdev_t;

static UINT face_in(JDEC *jd, BYTE *buf, UINT len)
{
    face_jdev_t *d = jd->device;
    size_t left = d->len - d->pos;
    UINT n = len < left ? len : (UINT)left;
    if (buf && n) {
        memcpy(buf, d->src + d->pos, n);
    }
    d->pos += n;
    return n;
}

static UINT face_out(JDEC *jd, void *bitmap, JRECT *rect)
{
    face_jdev_t *d = jd->device;
    int rw = rect->right - rect->left + 1;
    const uint8_t *rgb = bitmap;
    for (int y = rect->top; y <= rect->bottom; y++) {
        int py = d->y0 + y;
        const uint8_t *row = rgb + (size_t)(y - rect->top) * rw * 3;
        if (py < 0 || py >= d->h) {
            continue;
        }
        for (int x = 0; x < rw; x++) {
            int px = d->x0 + rect->left + x;
            if (px < 0 || px >= d->w) {
                continue;
            }
            const uint8_t *p = row + x * 3;
            d->out[(size_t)py * d->w + px] =
                (uint16_t)(((p[0] & 0xF8) << 8) | ((p[1] & 0xFC) << 3) | (p[2] >> 3));
        }
    }
    return 1;
}

const char *mg_face_jpeg_decode(const uint8_t *jpeg, size_t len, uint16_t *out, int w, int h,
                                int *img_w, int *img_h, int *scale_out)
{
    int fw = 0, fh = 0;
    bool progressive = false;
    face_fmt_t fmt = face_sniff(jpeg, len);
    if (fmt != FACE_FMT_JPEG) {
        return fmt == FACE_FMT_PNG ? "PNG refused: the face takes a baseline JPEG"
             : fmt == FACE_FMT_WEBP ? "WebP refused: the face takes a baseline JPEG"
             : "not a JPEG";
    }
    if (!face_jpeg_frame(jpeg, len, &fw, &fh, &progressive)) {
        return progressive ? "progressive JPEG refused: use baseline" : "not a baseline JPEG";
    }
    int scale, x0, y0;
    if (!face_place(fw, fh, w, h, &scale, &x0, &y0)) {
        return "the JPEG's size doesn't suit the screen";
    }
    void *pool = FACE_JPEG_POOL_ALLOC(FACE_JPEG_POOL);
    if (!pool) {
        return "out of memory";
    }
    face_jdev_t d = { .src = jpeg, .len = len, .out = out, .w = w, .h = h, .x0 = x0, .y0 = y0 };
    memset(out, 0, (size_t)w * h * sizeof(uint16_t));
    JDEC jd;
    const char *err = NULL;
    JRESULT rc = jd_prepare(&jd, face_in, pool, FACE_JPEG_POOL, &d);
    if (rc == JDR_OK && ((int)jd.width != fw || (int)jd.height != fh)) {
        err = "not a valid JPEG";
    } else if (rc == JDR_OK) {
        BYTE s = (BYTE)(scale == 8 ? 3 : scale == 4 ? 2 : scale == 2 ? 1 : 0);
        rc = jd_decomp(&jd, face_out, s);
    }
    if (!err && rc != JDR_OK) {
        err = rc == JDR_FMT3 ? "progressive JPEG refused: use baseline"
            : rc == JDR_INP ? "the JPEG ends early"
            : "not a valid JPEG";
    }
    free(pool);
    if (!err) {
        if (img_w) *img_w = fw;
        if (img_h) *img_h = fh;
        if (scale_out) *scale_out = scale;
    }
    return err;
}

#endif  // !ESP_PLATFORM || CONFIG_MUSE_AVATAR_FACE
