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
 * Frames of very different sizes decoded one after another from one reused
 * buffer of exactly AU_MAX bytes, as the video task's s_au: nothing of an
 * earlier frame may decide what the decoder reads in a later one
 * (tests/test_video_untrusted.py, StaleStreamState).
 *
 *   harness CODEC FILE|reset ...
 * CODEC: h264 or vp8 (muse_vdec_open / muse_vdec_decode), or, built
 * with -DSTALE_DIRECT, "direct" (tinyh264's h264bsdDecode alone, no glue).
 * "reset" closes and reopens the decoder. Prints one "NAME LEN RC W H SUM"
 * line per frame (SUM: FNV-1a of the picture's planes, "- - -" when there's
 * no picture) and "guard ok" when the bytes past AU_MAX are untouched. Under
 * AddressSanitizer the guard and, for each frame, the stale bytes between its
 * end and AU_MAX are poisoned, so a read past the frame fails the run too.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__SANITIZE_ADDRESS__)
#define STALE_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define STALE_ASAN 1
#endif
#endif
#if STALE_ASAN
#include <sanitizer/asan_interface.h>
#endif

#if STALE_DIRECT
#include "h264bsd_decoder.h"
#else
#include "muse_vdec.h"
#include "muse_vstream.h"
#endif

#define AU_MAX (64 * 1024)
#define GUARD 256

typedef struct {
#if STALE_DIRECT
    storage_t *s;
#else
    muse_vdec_t *v;
    uint8_t codec;
#endif
} dec_t;

static int dec_open(dec_t *d, const char *codec)
{
#if STALE_DIRECT
    if (strcmp(codec, "direct")) {
        return 0;
    }
    d->s = h264bsdAlloc();
    return d->s && h264bsdInit(d->s, 1) == 0;
#else
    d->codec = !strcmp(codec, "h264") ? MUSE_VIDEO_CODEC_H264 :
               !strcmp(codec, "vp8") ? MUSE_VIDEO_CODEC_VP8 :
               0;
    d->v = d->codec ? muse_vdec_open(d->codec, 480, 480) : NULL;
    return d->v != NULL;
#endif
}

static void dec_close(dec_t *d)
{
#if STALE_DIRECT
    h264bsdShutdown(d->s);
    h264bsdFree(d->s);
#else
    muse_vdec_close(d->v);
#endif
}

static uint32_t fnv(uint32_t x, const uint8_t *p, int n)
{
    while (n-- > 0) {
        x = (x ^ *p++) * 16777619u;
    }
    return x;
}

static uint32_t plane(uint32_t x, const uint8_t *p, int stride, int w, int h)
{
    for (int r = 0; r < h; r++) {
        x = fnv(x, p + (size_t)r * stride, w);
    }
    return x;
}

/* Decodes one frame and prints its result and picture, if any. */
static void dec_decode(dec_t *d, uint8_t *au, uint32_t len)
{
    int rc, w = 0, h = 0;
    uint32_t sum = 2166136261u;
#if STALE_DIRECT
    u8 *pic = NULL;
    u32 pw = 0, ph = 0;
    rc = (int)h264bsdDecode(d->s, au, len, &pic, &pw, &ph);
    if (rc == H264BSD_PIC_RDY && pic) {
        w = (int)pw;
        h = (int)ph;
        sum = plane(sum, pic, w, w, h * 3 / 2);   /* Y, then U and V */
    }
#else
    muse_pic_t pic = { 0 };
    rc = (int)muse_vdec_decode(d->v, au, len, &pic);
    if (rc == MUSE_VDEC_PICTURE) {
        w = pic.w;
        h = pic.h;
        sum = plane(sum, pic.y, pic.y_stride, w, h);
        sum = plane(sum, pic.u, pic.c_stride, w / 2, h / 2);
        sum = plane(sum, pic.v, pic.c_stride, w / 2, h / 2);
    }
#endif
    if (w) {
        printf(" %d %d %d %08x\n", rc, w, h, (unsigned)sum);
    } else {
        printf(" %d - - -\n", rc);
    }
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        return 2;
    }
    uint8_t *au = malloc(AU_MAX + GUARD);
    if (!au) {
        return 2;
    }
    memset(au + AU_MAX, 0xA5, GUARD);
#if STALE_ASAN
    ASAN_POISON_MEMORY_REGION(au + AU_MAX, GUARD);
#endif
    dec_t d = { 0 };
    if (!dec_open(&d, argv[1])) {
        fprintf(stderr, "can't open a %s decoder\n", argv[1]);
        return 2;
    }
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "reset")) {
            dec_close(&d);
            if (!dec_open(&d, argv[1])) {
                return 2;
            }
            printf("reset\n");
            continue;
        }
        FILE *f = fopen(argv[i], "rb");
        if (!f) {
            perror(argv[i]);
            return 2;
        }
        /* the frame over what's left of the last one, as the ring's pop */
#if STALE_ASAN
        ASAN_UNPOISON_MEMORY_REGION(au, AU_MAX);
#endif
        size_t n = fread(au, 1, AU_MAX, f);
        fclose(f);
#if STALE_ASAN
        ASAN_POISON_MEMORY_REGION(au + n, AU_MAX - n);   /* the last frames' bytes */
#endif
        const char *name = strrchr(argv[i], '/') ? strrchr(argv[i], '/') + 1 : argv[i];
        printf("%s %zu", name, n);
        dec_decode(&d, au, (uint32_t)n);
        fflush(stdout);
    }
    dec_close(&d);
#if STALE_ASAN
    ASAN_UNPOISON_MEMORY_REGION(au + AU_MAX, GUARD);
#endif
    for (int i = 0; i < GUARD; i++) {
        if (au[AU_MAX + i] != 0xA5) {
            printf("guard byte %d changed\n", i);
            return 1;
        }
    }
    printf("guard ok\n");
    free(au);
    return 0;
}
