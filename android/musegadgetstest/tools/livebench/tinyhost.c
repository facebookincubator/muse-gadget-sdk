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

// Host driver for tinyh264: decode an .au file (u32 len + Annex-B AU) and
// write I420 frames (cropped to the SPS crop window if -c W H given).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "h264bsd_decoder.h"

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s in.au out.yuv [reps]\n", argv[0]); return 2; }
    FILE *f = fopen(argv[1], "rb");
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc(n); fread(buf, 1, n, f); fclose(f);
    FILE *o = strcmp(argv[2], "-") ? fopen(argv[2], "wb") : NULL;
    int reps = argc > 3 ? atoi(argv[3]) : 1;
    u32 cw = argc > 5 ? (u32)atoi(argv[4]) : 0, ch = argc > 5 ? (u32)atoi(argv[5]) : 0;
    int frames = 0;
    for (int r = 0; r < reps; r++) {
        storage_t *s = h264bsdAlloc();
        h264bsdInit(s, 1);
        long p = 0;
        while (p + 4 <= n) {
            uint32_t len; memcpy(&len, buf + p, 4); p += 4;
            uint8_t *pic = NULL; u32 w = 0, h = 0;
            u32 rc = h264bsdDecode(s, buf + p, len, &pic, &w, &h);
            if (rc == H264BSD_PIC_RDY) {
                frames++;
                if (!cw) { cw = w; ch = h; }
                if (o && r == 0) {
                    /* the top-left cw x ch (the SPS crop window: right/bottom only) */
                    for (u32 y = 0; y < ch; y++) fwrite(pic + y * w, 1, cw, o);
                    for (int k = 0; k < 2; k++) {
                        uint8_t *pl = pic + w * h + k * (w / 2) * (h / 2);
                        for (u32 y = 0; y < ch / 2; y++) fwrite(pl + y * (w / 2), 1, cw / 2, o);
                    }
                }
            } else if (rc != H264BSD_RDY) {
                fprintf(stderr, "decode error rc=%u at %ld\n", rc, p);
            }
            p += len;
        }
        h264bsdShutdown(s);
        h264bsdFree(s);
    }
    if (o) fclose(o);
    fprintf(stderr, "%d frames\n", frames);
    return 0;
}
