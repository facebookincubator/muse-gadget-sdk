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
 * muse_video_fit.c on the host (tests/test_muse_video_fit.py): where each
 * video board's picture goes, the round panels' cover (no black column
 * or row inside the circle, the crop centered, the window aligned), the
 * suggested sizes, and every board's picture covering its whole screen.
 */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE 1   /* posix_memalign: glibc declares it only with this (macOS and newlib do anyway) */
#endif
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "muse_video_fit.h"

static int fails;
#define CHECK(c)                                                    \
    do {                                                            \
        if (!(c)) {                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);     \
            fails++;                                                \
        }                                                           \
    } while (0)

/* The video boards (components/muse/boards, those with draw_frame; the
 * Python side checks these flags against the board files). */
typedef struct {
    const char *name;
    int w, h;
    int round, align, max_src;
    int sug;   /* the suggestion the board should list */
} board_t;

static const board_t BOARDS[] = {
    { "SenseCAP Watcher", 412, 412, 1, 4, 240, 208 },
    { "M5Stack StopWatch", 466, 466, 1, 2, 240, 240 },
    { "Waveshare S3 1.75C", 466, 466, 1, 2, 240, 240 },
    { "ESP-Mosaico", 480, 480, 0, 2, 480, 480 },
    { "Waveshare C6 1.8", 368, 448, 0, 2, 160, 160 },   /* the C6's panel, 160 px pictures 2x */
};

/* ---- a reference up2: each output pixel says which source pixel it came from */

static const uint8_t *g_ybase;
static int g_up2_calls, g_up2_bad;

static void ref_up2(const uint8_t *y, const uint8_t *u, const uint8_t *v, int ys, int cs, int src_w, int out_rows,
                    uint8_t *out, int os)
{
    (void)u, (void)v, (void)cs;
    g_up2_calls++;
    /* The SIMD kernel's constraints (muse_yuv.h): y 16-byte aligned, ys a
     * multiple of 16, out and its stride multiples of 8; whole row pairs
     * (four output rows) into the cover's block. */
    if (((uintptr_t)y & 15) || (ys & 15) || ((uintptr_t)out & 7) || (os & 7) || out_rows != 4) {
        g_up2_bad++;
    }
    int row0 = (int)((y - g_ybase) / ys);
    for (int k = 0; k < out_rows; k++) {   /* exactly out_rows rows, output row k from source row k / 2 */
        uint16_t *o = (uint16_t *)(out + (size_t)k * os);
        for (int x = 0; x < src_w; x++) {
            /* Never 0 (black): source row, column, plus one. */
            uint16_t p = (uint16_t)(((row0 + k / 2) << 8 | x) + 1);
            o[2 * x] = p;
            o[2 * x + 1] = p;
        }
    }
}

static void *al16(size_t n)
{
    void *p = NULL;
    return posix_memalign(&p, 16, n ? n : 16) ? NULL : p;
}

/* ---- the cover drawn onto a whole panel, in pieces as the panel takes them */

static uint16_t *render(const board_t *b, int pic_w, int pic_h, muse_video_fit_t *f, int piece_rows, int *calls)
{
    muse_video_fit_in_t in = { b->w, b->h, b->round, b->align, b->max_src, 0, 1, pic_w, pic_h, 0, 0, 0 };
    if (!muse_video_fit(&in, f) || !f->cover) {
        return NULL;
    }
    /* Planes as the decoders lay them out: 16-byte aligned rows. */
    int ys = (pic_w + 15) & ~15, cs = ((pic_w / 2) + 7) & ~7;
    uint8_t *y = al16((size_t)ys * pic_h), *u = al16((size_t)cs * pic_h / 2 + 16);
    uint8_t *v = al16((size_t)cs * pic_h / 2 + 16);
    uint16_t *panel = calloc((size_t)b->w * b->h, 2);   /* black */
    muse_video_cover_t c = { al16(MUSE_VIDEO_COVER_BUF(b->max_src)), -1 };
    uint8_t *piece = al16((size_t)f->out_w * 2 * piece_rows);
    g_ybase = y;
    g_up2_calls = 0;
    for (int r = 0; r < f->out_h; r += piece_rows) {
        int n = f->out_h - r < piece_rows ? f->out_h - r : piece_rows;
        CHECK(muse_video_cover_src_rows(f, r, n) <= f->vis_h);
        muse_video_cover_fill(&c, f, ref_up2, y, u, v, ys, cs, r, n, piece, f->out_w * 2);
        for (int k = 0; k < n; k++) {
            memcpy(panel + (size_t)(f->y + r + k) * b->w + f->x, piece + (size_t)k * f->out_w * 2, (size_t)f->out_w * 2);
        }
    }
    *calls = g_up2_calls;
    free(y), free(u), free(v), free(c.buf), free(piece);
    return panel;
}

static uint16_t expect_px(const muse_video_fit_t *f, int px, int py)
{
    int fx = px - f->x + f->crop_x, fy = py - f->y + f->crop_y;
    fx = fx < 0 ? 0 : fx >= 2 * f->vis_w ? 2 * f->vis_w - 1 : fx;
    fy = fy < 0 ? 0 : fy >= 2 * f->vis_h ? 2 * f->vis_h - 1 : fy;
    return (uint16_t)(((fy / 2) << 8 | fx / 2) + 1);
}

static void check_cover(const board_t *b, int pic)
{
    muse_video_fit_t f;
    int calls = 0;
    uint16_t *panel = render(b, pic, pic, &f, 8, &calls);
    CHECK(panel != NULL);
    if (!panel) {
        return;
    }
    /* The window: the whole panel, on whole x_align columns. */
    CHECK(f.x == 0 && f.y == 0 && f.out_w == b->w && f.out_h == b->h);
    CHECK(f.x % b->align == 0 && f.out_w % b->align == 0);
    /* Centered: the two sides' crops (or extensions) differ by at most a pixel. */
    int right = 2 * f.vis_w - f.out_w - f.crop_x, bottom = 2 * f.vis_h - f.out_h - f.crop_y;
    CHECK(abs(right - f.crop_x) <= 1 && abs(bottom - f.crop_y) <= 1);
    /* Every pixel inside the circle is the picture's (none black), and the
     * one the centered 2x mapping names. */
    double cx = (b->w - 1) / 2.0, cy = (b->h - 1) / 2.0, R = b->w / 2.0;
    int inside = 0, black = 0, wrong = 0;
    for (int py = 0; py < b->h; py++) {
        for (int px = 0; px < b->w; px++) {
            if (hypot(px - cx, py - cy) > R) {
                continue;
            }
            inside++;
            uint16_t p = panel[(size_t)py * b->w + px];
            black += p == 0;
            wrong += p != expect_px(&f, px, py);
        }
    }
    CHECK(inside > 0 && black == 0 && wrong == 0);
    /* The pixels just inside the circle's edge on the four sides, the
     * flat edges the old 2x fit left black. */
    int mid = b->w / 2;
    CHECK(panel[(size_t)mid * b->w + 0] != 0 && panel[(size_t)mid * b->w + b->w - 1] != 0);
    CHECK(panel[mid] != 0 && panel[(size_t)(b->h - 1) * b->w + mid] != 0);
    CHECK(panel[(size_t)mid * b->w + 1] != 0 && panel[(size_t)1 * b->w + mid] != 0);
    /* Each source row pair converted once a picture, the kernel's constraints kept. */
    int first = muse_video_cover_src_rows(&f, 0, 1) / 2 - 1, last = muse_video_cover_src_rows(&f, f.out_h - 1, 1) / 2;
    CHECK(calls == last - first);
    CHECK(g_up2_bad == 0);
    printf("%s, %d px: 2x %d, crop %d,%d, window %d,%d %dx%d, %d row pairs\n", b->name, pic, 2 * pic, f.crop_x,
           f.crop_y, f.x, f.y, f.out_w, f.out_h, calls);
    free(panel);
}

int main(void)
{
    for (size_t i = 0; i < sizeof(BOARDS) / sizeof(BOARDS[0]); i++) {
        const board_t *b = &BOARDS[i];
        int sug = muse_video_suggest(b->w, b->h, b->round, b->max_src);
        printf("%s: suggests %d\n", b->name, sug);
        CHECK(sug == b->sug);
        CHECK(sug <= b->max_src && sug % (b->round ? 16 : 8) == 0);
        if (!b->round) {
            continue;   /* square panels: no cover */
        }
        CHECK(2 * sug >= b->w && 2 * (sug - 16) < b->w);   /* the smallest that covers */
        /* The two times cover where it reaches the panel both ways (the
         * suggestion, the largest, sizes between); a smaller picture (an
         * app that kept the old suggestion) is cover-scaled by the fill
         * instead of edge extended (no black bars). */
        check_cover(b, sug);
        check_cover(b, b->max_src);
        for (int pic = 160; pic <= b->max_src; pic += 8) {
            if (2 * pic >= b->w) {
                check_cover(b, pic);
            } else {
                muse_video_fit_in_t in = { b->w, b->h, 1, b->align, b->max_src, 0, 1, pic, pic, 0, 0, 0 };
                muse_video_fit_t f;
                CHECK(muse_video_fit(&in, &f) && f.scale == MUSE_VIDEO_FIT_FILL && !f.cover);
            }
        }
        /* Uneven pieces reuse the block too, and draw the same picture. */
        muse_video_fit_t f8, f6;
        int c8, c6;
        uint16_t *p8 = render(b, sug, sug, &f8, 8, &c8), *p6 = render(b, sug, sug, &f6, 6, &c6);
        CHECK(p8 && p6 && memcmp(p8, p6, (size_t)b->w * b->h * 2) == 0 && c8 == c6);
        free(p8), free(p6);
        /* Past the decoder's size (never from a real decoder): no cover. */
        muse_video_fit_in_t big = { b->w, b->h, 1, b->align, b->max_src, 0, 1, 480, 480, 0, 0, 0 };
        muse_video_fit_t fb;
        CHECK(muse_video_fit(&big, &fb) && !fb.cover);
    }
    /* Nothing to show. */
    muse_video_fit_in_t none = { 412, 412, 1, 4, 240, 0, 1, 2, 200, 0, 0, 0 };
    muse_video_fit_t fn;
    CHECK(!muse_video_fit(&none, &fn));
    /* Fill (muse_board_t.video_fill, the C6 1.8's 368 x 448 rectangle): the panel covered whole, the scale whichever
     * side covers it, the overflow cropped evenly, every output pixel taking a source pixel inside the picture. */
    int fills = 0;
    for (int pw = 32; pw <= 256; pw += 2) {
        for (int ph = 32; ph <= 256; ph += 2) {
            muse_video_fit_in_t in = { 368, 448, 0, 2, 160, 0, 1, pw, ph, 1, 0, 0 };
            muse_video_fit_t f;
            CHECK(muse_video_fit(&in, &f));
            CHECK(f.scale == MUSE_VIDEO_FIT_FILL && f.x == 0 && f.y == 0 && f.out_w == 368 && f.out_h == 448);
            CHECK(f.crop_x >= 0 && f.crop_y >= 0 && (f.crop_x == 0 || f.crop_y == 0));
            CHECK((long)f.vis_w * f.fill_num / f.fill_den >= f.out_w && (long)f.vis_h * f.fill_num / f.fill_den >= f.out_h);
            int x0 = muse_video_fill_src(0, f.crop_x, f.fill_num, f.fill_den, f.vis_w);
            int x1 = muse_video_fill_src(f.out_w - 1, f.crop_x, f.fill_num, f.fill_den, f.vis_w);
            int y0 = muse_video_fill_src(0, f.crop_y, f.fill_num, f.fill_den, f.vis_h);
            int y1 = muse_video_fill_src(f.out_h - 1, f.crop_y, f.fill_num, f.fill_den, f.vis_h);
            CHECK(x0 >= 0 && x1 <= f.vis_w - 1 && y0 >= 0 && y1 <= f.vis_h - 1);
            CHECK(abs(x0 - (f.vis_w - 1 - x1)) <= 1 && abs(y0 - (f.vis_h - 1 - y1)) <= 1);   /* centered */
            fills++;
        }
    }
    {
        muse_video_fit_in_t in = { 368, 448, 0, 2, 160, 0, 1, 128, 158, 1, 0, 0 };
        muse_video_fit_t f;
        CHECK(muse_video_fit(&in, &f));
        printf("Waveshare C6 1.8 fill, 128x158: %dx%d x %d/%d, crop %d,%d, window %d,%d %dx%d (%d fills checked)\n",
               f.vis_w, f.vis_h, f.fill_num, f.fill_den, f.crop_x, f.crop_y, f.x, f.y, f.out_w, f.out_h, fills);
        /* A UI stream, a round panel or two bands never fill. */
        muse_video_fit_in_t ui = { 368, 448, 0, 2, 160, 1, 1, 128, 158, 1, 0, 0 };
        CHECK(muse_video_fit(&ui, &f) && f.scale != MUSE_VIDEO_FIT_FILL);
        muse_video_fit_in_t two = { 368, 448, 0, 2, 160, 0, 2, 128, 158, 1, 0, 0 };
        CHECK(muse_video_fit(&two, &f) && f.scale != MUSE_VIDEO_FIT_FILL);
    }
    /* No black bars: on every video board, every size and aspect an app may send
     * (up to the board's advertised max: larger is refused at the stream's start) covers the whole screen: the
     * window is the whole panel and every output pixel takes a source pixel of the picture, no edge extension. */
    static const int sizes[][2] = { { 128, 158 }, { 158, 128 }, { 240, 240 }, { 320, 240 }, { 240, 320 }, { 480, 480 },
                                    { 176, 144 }, { 144, 176 }, { 160, 120 }, { 120, 160 }, { 208, 208 }, { 232, 232 },
                                    { 480, 360 }, { 360, 480 }, { 112, 96 } };
    int swept = 0;
    for (size_t i = 0; i < sizeof(BOARDS) / sizeof(BOARDS[0]); i++) {
        const board_t *b = &BOARDS[i];
        bool c6 = b->max_src == 160;   /* no PSRAM: its fill takes 256 */
        for (size_t k = 0; k < sizeof(sizes) / sizeof(sizes[0]); k++) {
            int pw = sizes[k][0] > b->max_src ? b->max_src : sizes[k][0];
            int ph = sizes[k][1] > b->max_src ? b->max_src : sizes[k][1];
            muse_video_fit_in_t in = { b->w, b->h, b->round, b->align, b->max_src, 0, 1, pw, ph, c6, 0,
                                       c6 ? 256 : 512 };
            for (int kind = 0; kind < 3; kind++) {
                int rgb = kind == 1;
                if (kind == 2 && b->max_src <= 240) {
                    continue;   /* two bands: the S31's (MAX_BANDS) */
                }
                in.rgb = rgb;
                in.bands = kind == 2 ? 2 : 1;
                muse_video_fit_t f;
                CHECK(muse_video_fit(&in, &f));
                bool whole = f.x == 0 && f.y == 0 && f.out_w == b->w && f.out_h == b->h;
                bool inside;
                if (f.extend) {
                    /* Edges extended: the picture inside the window, the rest repeats its edges. */
                    inside = f.ix >= 0 && f.iy >= 0 && f.ix + f.iw <= f.out_w && f.iy + f.ih <= f.out_h && f.iw > 0 &&
                             f.ih > 0;
                } else if (f.scale == MUSE_VIDEO_FIT_FILL) {
                    inside = (long)f.vis_w * f.fill_num / f.fill_den >= f.out_w &&
                             (long)f.vis_h * f.fill_num / f.fill_den >= f.out_h && f.crop_x >= 0 && f.crop_y >= 0;
                } else if (f.cover) {
                    inside = !rgb && f.crop_x >= 0 && f.crop_y >= 0;   /* 2x reaches the panel: cropped, not extended */
                } else {
                    inside = f.out_w >= b->w && f.out_h >= b->h;   /* the plain path, covering */
                }
                if (!whole || !inside) {
                    printf("FAIL %s %dx%d%s: window %d,%d %dx%d scale %d cover %d crop %d,%d\n", b->name, pw, ph,
                           kind == 1 ? " (RGB rows)" : kind == 2 ? " (two bands)" : "", f.x, f.y, f.out_w, f.out_h,
                           f.scale, f.cover, f.crop_x, f.crop_y);
                    fails++;
                }
                swept++;
            }
        }
    }
    printf("no black bars: %d board x size x kind placements cover the screen\n", swept);
    if (fails) {
        printf("%d failures\n", fails);
        return 1;
    }
    printf("ok\n");
    return 0;
}
