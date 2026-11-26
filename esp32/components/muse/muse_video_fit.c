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
#include "muse_video_fit.h"

#include <string.h>

/* The plain path: two times upscaled when that fits the panel, one to one
 * when the picture is the panel's size or more, between the two (on the
 * S31) 3/2 or scaled up to the panel's width; centered. UI streams one to
 * one. */
static void fit_plain(const muse_video_fit_in_t *in, int vis_w, int vis_h, muse_video_fit_t *f)
{
    const int W = in->panel_w, H = in->panel_h;
    const int a = in->x_align > 2 ? in->x_align : 2;
    int scale, ow, oh;
    scale = 2 * vis_w <= W && 2 * vis_h <= H ? 2 : 1;
    if (in->ui) {
        /* One to one, centered: the picture is the panel's (or smaller). */
        scale = 1;
        vis_w = (in->pic_w < W ? in->pic_w : W) & ~(a - 1);
        vis_h = (in->pic_h < H ? in->pic_h : H) & ~1;
        ow = vis_w;
        oh = vis_h;
    } else if (scale == 1 && in->max_src > 240 && vis_w * 3 / 2 == W && vis_h * 3 / 2 <= H && vis_w % 4 == 0 &&
               vis_h % 4 == 0) {
        scale = 3;   /* 320 on the 480 panel: the cheap 3/2 path */
        ow = vis_w * 3 / 2;
        oh = vis_h * 3 / 2 & ~3;
    } else if (scale == 1 && in->max_src > 240 && vis_w < W && vis_h < H) {
        scale = 0;
        ow = W & ~3;
        oh = (vis_h * ow / vis_w) & ~3;
        if (oh > H) {
            oh = H & ~3;
            ow = (vis_w * oh / vis_h) & ~3;
        }
    } else {
        if (scale * vis_w > W) {
            vis_w = W / scale & ~(a - 1);
        }
        if (scale * vis_h > H) {
            vis_h = H / scale & ~3;
        }
        ow = scale * vis_w;
        oh = scale * vis_h;
    }
    f->scale = scale;
    f->vis_w = vis_w;
    f->vis_h = vis_h;
    f->out_w = ow;
    f->out_h = oh;
    f->x = (W - ow) / 2 & ~(a - 1);
    f->y = (H - oh) / 2 & ~1;
}

bool muse_video_fit(const muse_video_fit_in_t *in, muse_video_fit_t *f)
{
    const int W = in->panel_w, H = in->panel_h;
    const int a = in->x_align > 2 ? in->x_align : 2;
    memset(f, 0, sizeof(*f));
    int vis_w = in->pic_w & ~(a - 1);
    int vis_h = in->pic_h & ~3;
    if (vis_w <= 0 || vis_h <= 0) {
        return false;   /* nothing to show, and nothing to divide by below */
    }
    const int fmax = in->fill_max_src > 0 ? in->fill_max_src : 256;
    const bool fill_ok = !in->ui && in->bands <= 1 && vis_w <= fmax && W <= 512;
    const int pw = W & ~(a - 1), ph = H & ~1;   /* the whole panel, on whole x_align columns */
    /* No black bars: a streamed
     * camera picture covers the whole panel whatever size or aspect it
     * comes in. The two times cover (round panels, SIMD) when its 2x reaches
     * the panel both ways, else the fill (any scale, nearest pixel), else
     * (two bands, wider than the fill takes) edge pixels extended. */
    bool cover2 = in->round && !in->ui && !in->rgb && in->bands <= 1 && in->max_src <= 240 &&
                  vis_w <= in->max_src && vis_h <= in->max_src && 2 * vis_w >= pw && 2 * vis_h >= ph;
    muse_video_fit_t plain;
    fit_plain(in, vis_w, vis_h, &plain);
    bool plain_covers = plain.out_w >= pw && plain.out_h >= ph;
    if (fill_ok && !cover2 && (in->fill || in->round || !plain_covers)) {
        /* Fill: the whole panel (whole x_align columns), the picture scaled by
         * whichever of out_w / vis_w and out_h / vis_h covers it, the overflow
         * cropped evenly from both sides. */
        f->scale = MUSE_VIDEO_FIT_FILL;
        f->vis_w = vis_w;
        f->vis_h = vis_h;
        f->out_w = pw;
        f->out_h = ph;
        f->x = (W - f->out_w) / 2 & ~(a - 1);
        f->y = (H - f->out_h) / 2 & ~1;
        if ((long)f->out_w * vis_h >= (long)f->out_h * vis_w) {
            f->fill_num = f->out_w;   /* the width decides; the height overflows */
            f->fill_den = vis_w;
        } else {
            f->fill_num = f->out_h;
            f->fill_den = vis_h;
        }
        f->crop_x = (int)(((long)vis_w * f->fill_num / f->fill_den - f->out_w) / 2);
        f->crop_y = (int)(((long)vis_h * f->fill_num / f->fill_den - f->out_h) / 2);
        return true;
    }
    /* (Never past max_src: the cover's block holds a row pair that wide.) */
    if (cover2) {
        /* Cover: two times upscaled, the window the whole panel (whole
         * x_align columns), the 2x picture centered on it and cropped. */
        f->cover = true;
        f->scale = 2;
        f->vis_w = vis_w;
        f->vis_h = vis_h;
        f->out_w = pw;
        f->out_h = ph;
        f->x = (W - f->out_w) / 2 & ~(a - 1);
        f->y = (H - f->out_h) / 2 & ~1;
        f->crop_x = (2 * vis_w - f->out_w) / 2;
        f->crop_y = (2 * vis_h - f->out_h) / 2;
        return true;
    }
    fit_plain(in, vis_w, vis_h, f);
    if (!in->ui && !plain_covers) {
        /* Two bands, or past the fill: the plain placement with its edges extended to the whole panel. */
        int x0 = (W - pw) / 2 & ~(a - 1), y0 = (H - ph) / 2 & ~1;
        f->extend = true;
        f->ix = f->x > x0 ? f->x - x0 : 0;
        f->iy = f->y > y0 ? f->y - y0 : 0;
        f->iw = f->out_w;
        f->ih = f->out_h;
        f->x = x0;
        f->y = y0;
        f->out_w = pw;
        f->out_h = ph;
    }
    return true;
}

int muse_video_fill_src(int o, int crop, int num, int den, int src_n)
{
    long long s = (((long long)(o + crop) * 2 + 1) * den) / (2LL * num);
    return s < 0 ? 0 : s >= src_n ? src_n - 1 : (int)s;
}

int muse_video_suggest(int panel_w, int panel_h, bool round, int max_src)
{
    int side = panel_w < panel_h ? panel_w : panel_h;
    int src;
    if (max_src > 240) {
        src = side & ~7;   /* the S31: the panel's own size, decoded in two bands */
    } else if (round) {
        src = ((side + 1) / 2 + 15) & ~15;   /* 2x covers the circle */
    } else {
        src = side / 2 & ~7;
    }
    return src > max_src ? max_src : src;
}

int muse_video_band_seam(int scale, int vis_h, int out_h, int h0, int *vis0)
{
    int v = h0 < vis_h ? h0 : vis_h, out;
    v = v > 0 ? v : 0;
    if (scale == 3) {
        v &= ~3;
        out = v * 3 / 2;
    } else if (scale == 1 || scale == 2) {
        v &= ~1;
        out = scale * v;
    } else {
        v &= ~1;
        out = vis_h > 0 ? (int)((int64_t)out_h * v / vis_h) & ~1 : 0;
    }
    out = out < out_h ? out : (out_h > 0 ? out_h : 0);
    *vis0 = v;
    return out;
}

bool muse_video_bands_ok(int h0, int h1, int height)
{
    if (h0 <= 0 || h1 <= 0 || height <= 0 || h0 % 16 || h0 >= height) {
        return false;
    }
    return h0 + h1 == height || (h1 % 16 == 0 && h0 + h1 == ((height + 15) & ~15));
}

static int clamp(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

int muse_video_cover_src_rows(const muse_video_fit_t *f, int row, int rows)
{
    int fr = clamp(row + rows - 1 + f->crop_y, 0, 2 * f->vis_h - 1);
    return 2 * (fr / 4) + 2;
}

void muse_video_cover_fill(muse_video_cover_t *c, const muse_video_fit_t *f, muse_video_up2_fn up2,
                           const uint8_t *y, const uint8_t *u, const uint8_t *v, int ystride, int cstride, int row,
                           int rows, uint8_t *dst, int stride)
{
    const int bw = 2 * f->vis_w;            /* the 2x picture's width, in pixels */
    const int bstride = bw * 2;             /* bytes: a multiple of 16 (vis_w of whole 4s) */
    const int out_w = f->out_w;
    int left = f->crop_x < 0 ? -f->crop_x : 0;   /* edge-extended columns on the left */
    left = left < out_w ? left : out_w;
    const int sx0 = f->crop_x > 0 ? f->crop_x : 0;
    int n = bw - sx0 < out_w - left ? bw - sx0 : out_w - left;
    n = n > 0 ? n : 0;
    for (int r = 0; r < rows; r++) {
        int fr = clamp(row + r + f->crop_y, 0, 2 * f->vis_h - 1);
        int b = fr >> 2;
        if (c->blk != b) {
            /* Source rows 2b and 2b + 1 (chroma row b): four 2x rows. */
            up2(y + (size_t)(2 * b) * ystride, u + (size_t)b * cstride, v + (size_t)b * cstride, ystride, cstride,
                f->vis_w, 4, c->buf, bstride);
            c->blk = b;
        }
        const uint16_t *s = (const uint16_t *)(c->buf + (size_t)(fr & 3) * bstride);
        uint16_t *d = (uint16_t *)(dst + (size_t)r * stride);
        for (int x = 0; x < left; x++) {
            d[x] = s[0];
        }
        memcpy(d + left, s + sx0, (size_t)n * 2);
        for (int x = left + n; x < out_w; x++) {
            d[x] = s[bw - 1];
        }
    }
}

void muse_video_cover_fill_win(muse_video_cover_t *c, const muse_video_fit_t *f, muse_video_up2_fn up2,
                               const uint8_t *y, const uint8_t *u, const uint8_t *v, int ystride, int cstride, int x,
                               int w, int row, int rows, bool first, uint8_t *dst, int stride)
{
    const int bw = 2 * f->vis_w;   /* the 2x picture's width */
    if (w <= 0 || rows <= 0 || bw <= 0) {
        return;
    }
    /* The 2x columns the window shows (edge extended past either side), and the source columns, in whole pairs. */
    int c0 = clamp(x + f->crop_x, 0, bw - 1), c1 = clamp(x + w - 1 + f->crop_x, 0, bw - 1);
    int s0 = (c0 / 2) & ~1, s1 = (c1 / 2 + 2) & ~1;
    s1 = s1 > f->vis_w ? f->vis_w : s1;
    const int n = s1 - s0, bstride = 2 * n * 2;   /* the block: four 2x rows of 2n pixels */
    if (first) {
        c->blk = -1;   /* another window's (or the whole picture's) columns */
    }
    for (int r = 0; r < rows; r++) {
        int fr = clamp(row + r + f->crop_y, 0, 2 * f->vis_h - 1);
        int b = fr >> 2;
        if (c->blk != b) {
            up2(y + (size_t)(2 * b) * ystride + s0, u + (size_t)b * cstride + s0 / 2, v + (size_t)b * cstride + s0 / 2,
                ystride, cstride, n, 4, c->buf, bstride);
            c->blk = b;
        }
        const uint16_t *s = (const uint16_t *)(c->buf + (size_t)(fr & 3) * bstride);
        uint16_t *d = (uint16_t *)(dst + (size_t)r * stride);
        for (int k = 0; k < w; k++) {
            int col = clamp(x + k + f->crop_x, 0, bw - 1);
            d[k] = s[col - 2 * s0];
        }
    }
}
