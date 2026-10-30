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
 * Muse Gadgets Test's VP8 clip loops (ClipStreamer.encodeLoopsVp8 in both
 * apps) through the apps' own encoder glue (ios/musegadgetstest/native/mgvpx.c,
 * the same code as Android's jni/mgvpx.c) and the host's libvpx, for
 * tests/test_musegadgetstest_vp8_loops.py.
 *
 *   musegadgetstest_vp8_loop_harness PARAMS SIZE FRAMES STEADY_LOOPS out.ivf
 *
 * Encodes a synthetic moving picture FRAMES frames long twice through one
 * encoder, as the apps do (the keyed loop, then the steady loop), and writes
 * what a gadget receives: the keyed loop, then STEADY_LOOPS steady loops.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mgcodecs.h"

/* The apps' libvpx has two switches (jni/libvpx_mg.py); stock libvpx ignores them. */
int vp8_mg_lf_max = -1;
int vp8_mg_mvcheap = 0;

static void put16(FILE *f, unsigned v)
{
    fputc((int)(v & 255), f);
    fputc((int)(v >> 8 & 255), f);
}

static void put32(FILE *f, unsigned v)
{
    put16(f, v & 0xffff);
    put16(f, v >> 16);
}

/* A picture with texture, motion and colour, different every frame. */
static void picture(uint8_t *p, int s, int t)
{
    uint8_t *y = p, *u = p + s * s, *v = u + s * s / 4;
    for (int r = 0; r < s; r++) {
        for (int c = 0; c < s; c++) {
            int cx = c - s / 2 - (t * 3) % (s / 2) + s / 4, cy = r - s / 2;
            int ring = ((cx * cx + cy * cy) / 64 + t) & 15;
            y[r * s + c] = (uint8_t)(40 + ring * 9 + ((c * 7 + r * 13 + t * 5) & 31) + ((r + t) & 8) * 4);
        }
    }
    for (int r = 0; r < s / 2; r++) {
        for (int c = 0; c < s / 2; c++) {
            u[r * s / 2 + c] = (uint8_t)(96 + ((c + t * 2) & 63));
            v[r * s / 2 + c] = (uint8_t)(160 - ((r * 2 + t) & 63));
        }
    }
}

int main(int argc, char **argv)
{
    if (argc != 6) {
        fprintf(stderr, "usage: %s PARAMS SIZE FRAMES STEADY_LOOPS out.ivf\n", argv[0]);
        return 2;
    }
    const char *params = argv[1];
    int s = atoi(argv[2]), n = atoi(argv[3]), loops = atoi(argv[4]);
    size_t fs = (size_t)s * s * 3 / 2;
    uint8_t *pic = malloc(fs);
    uint8_t **fr = calloc((size_t)(2 * n), sizeof(*fr));
    int *len = calloc((size_t)(2 * n), sizeof(*len));
    void *e = mgvpx_open(s, s, 24, 400, params);
    if (!pic || !fr || !len || !e) {
        fprintf(stderr, "open failed\n");
        return 1;
    }
    for (int i = 0; i < 2 * n; i++) {
        const uint8_t *out = NULL;
        int info[2];
        picture(pic, s, i % n);
        int k = mgvpx_encode(e, pic, i, i == 0, &out, info);
        if (k <= 0 || !out) {
            fprintf(stderr, "no frame for picture %d\n", i);
            return 1;
        }
        if (i >= n && !(out[0] & 1)) {
            fprintf(stderr, "keyframe in the steady loop\n");
            return 1;
        }
        fr[i] = malloc((size_t)k);
        memcpy(fr[i], out, (size_t)k);
        len[i] = k;
    }
    mgvpx_close(e);
    FILE *f = fopen(argv[5], "wb");
    if (!f) {
        return 1;
    }
    int total = n * (1 + loops);
    fwrite("DKIF", 1, 4, f);
    put16(f, 0);
    put16(f, 32);
    fwrite("VP80", 1, 4, f);
    put16(f, (unsigned)s);
    put16(f, (unsigned)s);
    put32(f, 24);
    put32(f, 1);
    put32(f, (unsigned)total);
    put32(f, 0);
    for (int k = 0; k < total; k++) {
        int i = k < n ? k : n + k % n;
        put32(f, (unsigned)len[i]);
        put32(f, (unsigned)k);
        put32(f, 0);
        fwrite(fr[i], 1, (size_t)len[i], f);
    }
    fclose(f);
    printf("frames %d keyframe %d\n", total, len[0]);
    return 0;
}
