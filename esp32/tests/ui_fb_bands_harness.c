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
 * A two-band UI picture whose bottom band is narrower
 * than the top. muse_ui_fb_picture reads both bands at the copy's width; with
 * a 464-wide copy over a 464x16 top and a 16x16 bottom (431 bytes, as tinyh264
 * allocates it: 384 of planes, +32+15), it read up to byte 703 of the bottom
 * picture. Run under ASan: `bad` must be refused with nothing read, `good`
 * (two 464x16 bands) draws every macroblock as before.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "muse_ui_fb.h"

void muse_yuv_1x_hq_cols(const uint8_t *y, const uint8_t *u, const uint8_t *v, int ystride, int cstride, int src_w,
                         int src_h, int row, int rows, int x0, int x1, uint8_t *dst, int stride)
{
    (void)y, (void)u, (void)v, (void)ystride, (void)cstride, (void)src_w, (void)src_h, (void)row, (void)rows;
    (void)x0, (void)x1, (void)dst, (void)stride;
}

/* A 4:2:0 picture w x h in its own exact allocation (+extra), like tinyh264's. */
static muse_pic_t pic(int w, int h, int extra, uint8_t fill, void **mem)
{
    size_t ys = (size_t)w * h, cs = (size_t)(w / 2) * (h / 2);
    uint8_t *m = malloc(ys + 2 * cs + (size_t)extra);
    memset(m, fill, ys + 2 * cs + (size_t)extra);
    *mem = m;
    return (muse_pic_t){ .y = m, .u = m + ys, .v = m + ys + cs, .y_stride = w, .c_stride = w / 2, .w = w, .h = h,
                         .seq = fill };
}

int main(int argc, char **argv)
{
    const char *which = argc > 1 ? argv[1] : "good";
    muse_ui_rect_t rects[MUSE_UI_MAX_RECTS];
    if (!muse_ui_fb_begin(464, 32)) {
        printf("FAIL begin\n");
        return 1;
    }
    void *a, *b;
    muse_pic_t top = pic(464, 16, 47, 0x40, &a);
    muse_pic_t bot = !strcmp(which, "bad") ? pic(16, 16, 47, 0x80, &b) : pic(464, 16, 47, 0x80, &b);
    int n = muse_ui_fb_picture(&top, &bot, 16, true, rects);
    int mbs = 0, total = 0, skipped = 0;
    uint32_t us = 0;
    muse_ui_fb_last_stats(&mbs, &total, &skipped, &us);
    printf("%s: %d rects, %d of %d macroblocks\n", which, n, mbs, total);
    /* A one-band picture narrower than the copy is refused too. */
    void *c;
    muse_pic_t narrow = pic(448, 32, 0, 0x20, &c);
    int n1 = muse_ui_fb_picture(&narrow, NULL, 0, true, rects);
    printf("one band 448 under 464: %d\n", n1);
    free(a);
    free(b);
    free(c);
    muse_ui_fb_end();
    return 0;
}
