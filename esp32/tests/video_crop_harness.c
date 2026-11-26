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
 * H.264 frame cropping through muse_vdec.c on the real tinyh264
 * (tests/test_video_crop.py).
 *
 * Usage:
 *   harness decode CLIP START_W START_H OUT
 *     Decodes an Annex B clip access unit by access unit with a decoder opened
 *     for start_streaming's START_W x START_H, as the video task does. Writes
 *     each picture as muse_vdec hands it over (its w x h window: Y, then U,
 *     then V, packed) to OUT and prints one JSON line per access unit: the
 *     result, the picture's size, the progress callback's checks (rows never
 *     past the window, never going back, the last report the whole window
 *     and the same planes as the picture) and whether a macroblock map came.
 *   harness window CW CH CHROMA FRAME_MBS_ONLY L R T B
 *     Prints muse_h264_crop_window()'s answer as JSON.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "muse_vdec.h"
#include "muse_vstream.h"

typedef struct {
    int calls, last_rows, past, back, bad_planes;
    muse_pic_t last;
} prog_t;

static void on_progress(void *ctx, const muse_pic_t *pic, int rows)
{
    prog_t *p = ctx;
    if (rows > pic->h || rows < 0) {
        p->past++;
    }
    if (p->calls && rows < p->last_rows) {
        p->back++;
    }
    if (p->calls && (pic->y != p->last.y || pic->u != p->last.u || pic->v != p->last.v || pic->w != p->last.w ||
                     pic->h != p->last.h)) {
        p->bad_planes++;
    }
    p->calls++;
    p->last_rows = rows;
    p->last = *pic;
}

/* The next access unit: from a parameter set, delimiter or slice that
 * follows a slice, to the next such one. */
static size_t next_au(const uint8_t *b, size_t n, size_t at)
{
    int seen_slice = 0;
    for (size_t i = at; i + 3 < n; i++) {
        if (b[i] == 0 && b[i + 1] == 0 && b[i + 2] == 1) {
            int t = b[i + 3] & 0x1f;
            if (seen_slice && (t == 1 || t == 5 || t == 7 || t == 8 || t == 9)) {
                size_t s = i;
                while (s > at && b[s - 1] == 0) {
                    s--;   /* a 4-byte start code's leading zero */
                }
                return s;
            }
            if (t == 1 || t == 5) {
                seen_slice = 1;
            }
            i += 2;
        }
    }
    return n;
}

static int decode(const char *clip, int sw, int sh, const char *out_path)
{
    FILE *f = fopen(clip, "rb");
    if (!f) {
        perror(clip);
        return 2;
    }
    static uint8_t file[1 << 20], au[64 * 1024];
    size_t n = fread(file, 1, sizeof(file), f);
    fclose(f);
    FILE *out = fopen(out_path, "wb");
    if (!out) {
        perror(out_path);
        return 2;
    }
    muse_vdec_t *v = muse_vdec_open(MUSE_VIDEO_CODEC_H264, sw, sh);
    if (!v) {
        fprintf(stderr, "no decoder\n");
        return 2;
    }
    size_t at = 0;
    while (at < n) {
        size_t end = next_au(file, n, at);
        size_t len = end - at;
        if (len > sizeof(au)) {
            fprintf(stderr, "access unit too long\n");
            return 2;
        }
        memcpy(au, file + at, len);   /* the decoder edits its input */
        at = end;
        prog_t p = { 0 };
        muse_vdec_set_progress(v, on_progress, &p);
        muse_pic_t pic;
        int rc = muse_vdec_decode(v, au, (uint32_t)len, &pic);
        int w = 0, h = 0, same = 0, last_ok = 0;
        if (rc == MUSE_VDEC_PICTURE) {
            w = pic.w;
            h = pic.h;
            same = pic.same != NULL;
            last_ok = p.calls > 0 && p.last_rows == pic.h && p.last.y == pic.y && p.last.u == pic.u &&
                      p.last.v == pic.v;
            for (int r = 0; r < h; r++) fwrite(pic.y + (size_t)r * pic.y_stride, 1, (size_t)w, out);
            for (int r = 0; r < h / 2; r++) fwrite(pic.u + (size_t)r * pic.c_stride, 1, (size_t)w / 2, out);
            for (int r = 0; r < h / 2; r++) fwrite(pic.v + (size_t)r * pic.c_stride, 1, (size_t)w / 2, out);
        }
        printf("{\"rc\": %d, \"w\": %d, \"h\": %d, \"calls\": %d, \"past\": %d, \"back\": %d, \"bad_planes\": %d, "
               "\"last_ok\": %d, \"same\": %d}\n",
               rc, w, h, p.calls, p.past, p.back, p.bad_planes, last_ok, same);
    }
    fclose(out);
    muse_vdec_close(v);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 6 && !strcmp(argv[1], "decode")) {
        return decode(argv[2], atoi(argv[3]), atoi(argv[4]), argv[5]);
    }
    if (argc == 10 && !strcmp(argv[1], "window")) {
        muse_vcrop_t c;
        bool ok = muse_h264_crop_window(atoi(argv[2]), atoi(argv[3]), (unsigned)atoi(argv[4]), atoi(argv[5]) != 0,
                                        (uint32_t)strtoul(argv[6], NULL, 10), (uint32_t)strtoul(argv[7], NULL, 10),
                                        (uint32_t)strtoul(argv[8], NULL, 10), (uint32_t)strtoul(argv[9], NULL, 10), &c);
        printf("{\"ok\": %d, \"x\": %d, \"y\": %d, \"w\": %d, \"h\": %d}\n", ok, c.x, c.y, c.w, c.h);
        return 0;
    }
    fprintf(stderr, "usage: harness decode CLIP W H OUT | window CW CH CHROMA FMO L R T B\n");
    return 2;
}
