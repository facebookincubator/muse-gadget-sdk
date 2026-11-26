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

/* muse_vdirty_digest against what tinyh264 actually decodes (tests/test_muse_vdirty.py): a macroblock whose
 * digest equals the last picture's must equal it byte for byte (deblocking, skips with a motion vector, the
 * VideoToolbox captures' coded-but-unchanged blocks included), and every changed one must be caught. Also how
 * many the decoder's conservative map (muse_vdec_same) finds, and the digests with it as their skip list. One
 * JSON line per stream. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "h264bsd_decoder.h"
#include "h264bsd_storage.h"
#include "muse_vdec_same.h"
#include "muse_vdirty.h"

static int nal_type(const unsigned char *p) { return p[0] & 0x1f; }

int main(int argc, char **argv)
{
    for (int a = 1; a < argc; a++) {
        FILE *f = fopen(argv[a], "rb");
        if (!f) {
            return 2;
        }
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        unsigned char *buf = malloc((size_t)n);
        if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
            return 2;
        }
        fclose(f);
        storage_t *s = h264bsdAlloc();
        h264bsdInit(s, 1);
        unsigned char *prev_copy = NULL, *prev_out = NULL;
        muse_vdirty_hash_t *dg = NULL, *dg2 = NULL;
        uint8_t *chg = NULL, *chg2 = NULL, *same = NULL;
        long pics = 0, mbs = 0, truly_same = 0, dg_same = 0, dg_false_same = 0, dg_false_changed = 0, map_same = 0,
             with_map_wrong = 0, hashed_with_map = 0;
        long au_start = -1, i = 0;
        int pw = 0, ph = 0;
        while (i + 3 < n) {
            long sc = -1;
            for (long k = i; k + 3 < n; k++) {
                if (buf[k] == 0 && buf[k + 1] == 0 && buf[k + 2] == 1) {
                    sc = k;
                    break;
                }
            }
            if (sc < 0) {
                break;
            }
            long start = sc > 0 && buf[sc - 1] == 0 ? sc - 1 : sc;
            if (au_start < 0) {
                au_start = start;
            }
            long next = n;
            for (long k = sc + 3; k + 3 < n; k++) {
                if (buf[k] == 0 && buf[k + 1] == 0 && (buf[k + 2] == 1 || (buf[k + 2] == 0 && buf[k + 3] == 1))) {
                    next = k;
                    break;
                }
            }
            int t = nal_type(buf + sc + 3);
            i = next;
            if (t != 1 && t != 5) {
                continue;
            }
            u8 *pic = NULL;
            u32 w = 0, h = 0;
            u32 rc = h264bsdDecode(s, buf + au_start, (u32)(next - au_start), &pic, &w, &h);
            au_start = -1;
            if (rc != H264BSD_PIC_RDY || !pic) {
                continue;
            }
            size_t ysz = (size_t)w * h, csz = ysz / 4, sz = ysz + 2 * csz;
            int mw = (int)w / 16, mh = (int)h / 16;
            bool prev_ok = prev_copy && (int)w == pw && (int)h == ph;
            if ((int)w != pw || (int)h != ph) {
                free(dg); free(dg2); free(chg); free(chg2); free(same);
                dg = calloc((size_t)mw * mh, sizeof(*dg));
                dg2 = calloc((size_t)mw * mh, sizeof(*dg2));
                chg = calloc((size_t)mw * mh, 1);
                chg2 = calloc((size_t)mw * mh, 1);
                same = calloc((size_t)mw * mh, 1);
                pw = (int)w;
                ph = (int)h;
            }
            int got = prev_ok && prev_out ? muse_h264_same_map(s, prev_out, same, (size_t)mw * mh) : -1;
            muse_vdirty_digest(pic, pic + ysz, pic + ysz + csz, (int)w, (int)w / 2, (int)w, (int)h, dg, prev_ok, NULL, 0,
                               chg);
            muse_vdirty_digest(pic, pic + ysz, pic + ysz + csz, (int)w, (int)w / 2, (int)w, (int)h, dg2, prev_ok,
                               got >= 0 ? same : NULL, mw, chg2);
            for (int my = 0; my < mh && prev_ok; my++) {
                for (int mx = 0; mx < mw; mx++) {
                    int eq = 1;
                    for (int r = 0; r < 16 && eq; r++) {
                        eq = !memcmp(pic + (size_t)(my * 16 + r) * w + mx * 16,
                                     prev_copy + (size_t)(my * 16 + r) * w + mx * 16, 16);
                    }
                    for (int pl = 0; pl < 2 && eq; pl++) {
                        size_t base = ysz + (size_t)pl * csz;
                        for (int r = 0; r < 8 && eq; r++) {
                            size_t o = base + (size_t)(my * 8 + r) * (w / 2) + mx * 8;
                            eq = !memcmp(pic + o, prev_copy + o, 8);
                        }
                    }
                    int k = my * mw + mx;
                    truly_same += eq;
                    dg_same += !chg[k];
                    dg_false_same += !chg[k] && !eq;
                    dg_false_changed += chg[k] && eq;
                    map_same += got >= 0 && same[k];
                    with_map_wrong += chg2[k] != !eq;
                    hashed_with_map += !(got >= 0 && same[k]);
                    mbs++;
                }
            }
            prev_copy = realloc(prev_copy, sz);
            memcpy(prev_copy, pic, sz);
            prev_out = pic;
            pics++;
        }
        printf("{\"file\": \"%s\", \"pictures\": %ld, \"mbs\": %ld, \"truly_same\": %ld, \"digest_same\": %ld, "
               "\"digest_false_same\": %ld, \"digest_false_changed\": %ld, \"map_same\": %ld, "
               "\"with_map_wrong\": %ld, \"hashed_with_map\": %ld}\n",
               strrchr(argv[a], '/') ? strrchr(argv[a], '/') + 1 : argv[a], pics, mbs, truly_same, dg_same,
               dg_false_same, dg_false_changed, map_same, with_map_wrong, hashed_with_map);
        h264bsdShutdown(s);
        h264bsdFree(s);
        free(buf);
        free(prev_copy);
    }
    return 0;
}
