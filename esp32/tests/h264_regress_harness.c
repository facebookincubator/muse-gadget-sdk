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
 * tinyh264's regression corpus (tests/vectors/h264/regress, from fuzzing
 * tinyh264) through muse_vdec.c as the video task drives it:
 * one decoder, every access unit copied into one reused buffer (s_au).
 * tests/test_video_route.py runs it under ASan and UBSan.
 *
 * A file: byte 0 a mode (the fuzzing harness's decoder mode; ignored here), then records of
 * [u16 LE length][access unit]. Prints one JSON line per file: each access
 * unit's result (1 picture, 0 none, -1 error) and its picture's FNV-1a hash.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "muse_vdec.h"
#include "muse_vstream.h"

static uint32_t fnv(const uint8_t *p, size_t n, uint32_t h)
{
    for (size_t i = 0; i < n; i++) {
        h = (h ^ p[i]) * 16777619u;
    }
    return h;
}

static uint32_t pic_hash(const muse_pic_t *p)
{
    uint32_t h = 2166136261u;
    for (int y = 0; y < p->h; y++) h = fnv(p->y + (size_t)y * p->y_stride, (size_t)p->w, h);
    for (int y = 0; y < p->h / 2; y++) h = fnv(p->u + (size_t)y * p->c_stride, (size_t)p->w / 2, h);
    for (int y = 0; y < p->h / 2; y++) h = fnv(p->v + (size_t)y * p->c_stride, (size_t)p->w / 2, h);
    return h;
}

int main(int argc, char **argv)
{
    static uint8_t file[256 * 1024], au[64 * 1024];
    for (int a = 1; a < argc; a++) {
        FILE *f = fopen(argv[a], "rb");
        if (!f) {
            perror(argv[a]);
            return 2;
        }
        size_t n = fread(file, 1, sizeof(file), f);
        fclose(f);
        const char *name = strrchr(argv[a], '/') ? strrchr(argv[a], '/') + 1 : argv[a];
        muse_vdec_t *v = muse_vdec_open(MUSE_VIDEO_CODEC_H264, 480, 480);
        printf("{\"file\": \"%s\", \"aus\": [", name);
        size_t at = 1;
        int k = 0;
        while (at + 2 <= n) {
            size_t len = file[at] | (size_t)file[at + 1] << 8;
            at += 2;
            if (len > n - at) {
                len = n - at;   /* a cut record: what's there */
            }
            /* one buffer for every access unit, as the video task's s_au;
             * what's past this one holds the last one's bytes */
            memcpy(au, file + at, len);
            at += len;
            muse_pic_t p;
            int rc = len ? muse_vdec_decode(v, au, (uint32_t)len, &p) : -1;
            printf("%s[%d, %u]", k++ ? ", " : "", rc, rc == MUSE_VDEC_PICTURE ? pic_hash(&p) : 0u);
        }
        printf("]}\n");
        fflush(stdout);
        muse_vdec_close(v);
    }
    return 0;
}
