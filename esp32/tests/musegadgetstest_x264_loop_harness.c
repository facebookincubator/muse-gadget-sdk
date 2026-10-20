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
 * Muse Gadgets Test's H.264 clip loops (ClipStreamer.encodeLoopsX264): the
 * apps' x264 glue (ios/musegadgetstest/native/mgx264.c, the same as the Android
 * JNI's) and the host's libx264 encode a synthetic clip twice in a row, the
 * keyed loop then the steady loop, as the apps do. The clip has a hard cut in
 * the middle, and its end differs from its start, so its wrap-around is a
 * cut too. Prints the frames x264 made IDRs.
 *
 *   musegadgetstest_x264_loop_harness PARAMS SIZE FRAMES
 *
 * Exit 0 when only the keyed loop's first frame is an IDR, 1 when the steady
 * loop has one (the apps refuse such a clip), 2 on errors.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mgcodecs.h"

/* A moving gradient, and its negative for the second half: a hard cut. */
static void frame(uint8_t *f, int size, int i, int frames)
{
    int cut = i >= frames / 2;
    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            int v = (x * 3 + y * 2 + i * 4) & 255;
            f[y * size + x] = (uint8_t)(cut ? 255 - v : v);
        }
    }
    memset(f + size * size, cut ? 60 : 190, (size_t)size * size / 4);
    memset(f + size * size * 5 / 4, cut ? 200 : 70, (size_t)size * size / 4);
}

static int is_idr(const uint8_t *p, int n)
{
    for (int i = 0; i + 3 < n; i++) {
        if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1 && (p[i + 3] & 0x1f) == 5) {
            return 1;
        }
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 4) {
        fprintf(stderr, "usage: %s PARAMS SIZE FRAMES\n", argv[0]);
        return 2;
    }
    int size = atoi(argv[2]), frames = atoi(argv[3]);
    void *e = mgx264_open(size, size, 24, 600, "fast", argv[1]);
    if (!e) {
        fprintf(stderr, "x264 open failed\n");
        return 2;
    }
    uint8_t *f = malloc((size_t)size * size * 3 / 2);
    int got = 0, steady_idr = 0, info[2];
    for (int i = 0; i < 4 * frames && got < 2 * frames; i++) {
        const uint8_t *au = NULL;
        int n;
        if (i < 2 * frames) {
            frame(f, size, i % frames, frames);
            n = mgx264_encode(e, f, i, i == 0, &au, info);
        } else {
            n = mgx264_encode(e, NULL, 0, 0, &au, info);   /* flush the lookahead */
        }
        if (n > 0) {
            if (is_idr(au, n)) {
                printf("IDR at %s loop frame %d\n", got < frames ? "keyed" : "steady", got % frames);
                steady_idr += got >= frames;
            }
            got++;
        }
    }
    mgx264_close(e);
    free(f);
    if (got != 2 * frames) {
        fprintf(stderr, "x264 gave %d frames for %d\n", got, 2 * frames);
        return 2;
    }
    return steady_idr ? 1 : 0;
}
