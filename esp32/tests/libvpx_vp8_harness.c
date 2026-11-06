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
/* libvpx_vp8_harness IN.ivf OUT.yuv: decodes with components/libvpx_vp8,
 * writes the shown pictures (visible size, I420), and checks the progress
 * hook: rows never go back, and the last report of each shown picture is its
 * full height. Prints "pics N progress ok" or the failure. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "vpx/vp8dx.h"
#include "vpx/vpx_decoder.h"

static int s_last = -1, s_bad, s_calls;
static const unsigned char *s_y;

static void progress(void *ctx, const unsigned char *y, const unsigned char *u, const unsigned char *v, int ys, int cs,
                     int w, int h, int rows)
{
    (void)ctx; (void)u; (void)v; (void)ys; (void)cs; (void)w;
    if (rows < s_last || rows > h || (s_y && y != s_y)) {
        s_bad++;
    }
    s_y = y;
    s_last = rows;
    s_calls++;
}

int main(int argc, char **argv)
{
    FILE *f = fopen(argv[1], "rb");
    if (!f) return 2;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)n);
    if (fread(b, 1, (size_t)n, f) != (size_t)n) return 2;
    fclose(f);
    FILE *o = fopen(argv[2], "wb");
    vpx_codec_ctx_t d;
    vpx_codec_dec_cfg_t cfg = { 1, 0, 0 };
    if (vpx_codec_dec_init(&d, vpx_codec_vp8_dx(), &cfg, 0)) return 3;
    vp8dx_mgx_set_progress(&d, progress, NULL);
    long i = 32;
    int pics = 0;
    while (i + 12 <= n) {
        uint32_t sz = b[i] | b[i + 1] << 8 | b[i + 2] << 16 | (uint32_t)b[i + 3] << 24;
        i += 12;
        s_last = -1;
        s_y = NULL;
        s_calls = 0;
        if (vpx_codec_decode(&d, b + i, sz, NULL, 0)) {
            printf("decode error at %d\n", pics);
            return 4;
        }
        i += sz;
        vpx_codec_iter_t it = NULL;
        vpx_image_t *img = vpx_codec_get_frame(&d, &it);
        if (!img) {
            if (s_calls) s_bad++;   /* a hidden frame reports nothing */
            continue;
        }
        if (s_last != (int)img->d_h || s_y != img->planes[0]) s_bad++;
        for (int p = 0; p < 3; p++) {
            int w = p ? (int)(img->d_w + 1) / 2 : (int)img->d_w, h = p ? (int)(img->d_h + 1) / 2 : (int)img->d_h;
            for (int y = 0; y < h; y++) fwrite(img->planes[p] + y * img->stride[p], 1, (size_t)w, o);
        }
        pics++;
    }
    fclose(o);
    vpx_codec_destroy(&d);
    printf("pics %d progress %s\n", pics, s_bad ? "BAD" : "ok");
    return s_bad ? 5 : 0;
}
