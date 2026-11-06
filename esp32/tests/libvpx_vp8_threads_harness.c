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
 * Several libvpx_vp8 decoders at once, one thread each, as the gadget runs
 * two bands of one picture on two cores (muse_video): any state the decoder
 * instances share shows up as pictures that differ from a decoder running
 * alone. tests/test_libvpx_vp8.py.
 *
 *   libvpx_vp8_threads_harness ROUNDS a.ivf [b.ivf ...]
 *
 * Decodes each stream alone first (its reference hash of every picture), then
 * all of them together, ROUNDS times, one thread per stream, and prints
 * "threads ok" when every concurrent decode matched its reference.
 */

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vpx/vp8dx.h"
#include "vpx/vpx_decoder.h"

#define MAXS 8

typedef struct {
    unsigned char *buf;
    long len;
    uint64_t hash;
    int pics;
    int fail;
} stream_t;

/* A start line for each round (macOS has no pthread_barrier_t). */
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static int nthreads, waiting, generation;

static void barrier(void)
{
    pthread_mutex_lock(&mu);
    int g = generation;
    if (++waiting == nthreads) {
        waiting = 0;
        generation++;
        pthread_cond_broadcast(&cv);
    } else {
        while (g == generation) {
            pthread_cond_wait(&cv, &mu);
        }
    }
    pthread_mutex_unlock(&mu);
}

static uint64_t fnv(uint64_t h, const unsigned char *p, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        h = (h ^ p[i]) * 0x100000001b3ull;
    }
    return h;
}

/* Every picture of the stream, hashed row by row (pictures have borders). */
static uint64_t decode(const stream_t *s, int *pics)
{
    vpx_codec_ctx_t d;
    vpx_codec_dec_cfg_t cfg = {1, 0, 0};
    uint64_t h = 0xcbf29ce484222325ull;
    if (vpx_codec_dec_init(&d, vpx_codec_vp8_dx(), &cfg, 0)) {
        return 0;
    }
    *pics = 0;
    for (long i = 32; i + 12 <= s->len;) {
        unsigned sz = s->buf[i] | s->buf[i + 1] << 8 | s->buf[i + 2] << 16 | (unsigned)s->buf[i + 3] << 24;
        i += 12;
        if (sz > (unsigned long)(s->len - i)) {
            break;
        }
        vpx_codec_decode(&d, s->buf + i, sz, NULL, 0);
        i += sz;
        vpx_codec_iter_t it = NULL;
        vpx_image_t *img;
        while ((img = vpx_codec_get_frame(&d, &it)) != NULL) {
            for (int p = 0; p < 3; p++) {
                int w = p ? (int)(img->d_w + 1) / 2 : (int)img->d_w, ph = p ? (int)(img->d_h + 1) / 2 : (int)img->d_h;
                for (int y = 0; y < ph; y++) {
                    h = fnv(h, img->planes[p] + y * img->stride[p], (size_t)w);
                }
            }
            (*pics)++;
        }
    }
    vpx_codec_destroy(&d);
    return h;
}

static int rounds;

static void *worker(void *arg)
{
    stream_t *s = arg;
    for (int r = 0; r < rounds; r++) {
        int pics;
        barrier();
        if (decode(s, &pics) != s->hash || pics != s->pics) {
            s->fail++;
        }
    }
    return NULL;
}

int main(int argc, char **argv)
{
    if (argc < 3 || argc - 2 > MAXS) {
        fprintf(stderr, "usage: %s ROUNDS a.ivf [b.ivf ...]\n", argv[0]);
        return 2;
    }
    rounds = atoi(argv[1]);
    int n = argc - 2;
    stream_t s[MAXS];
    memset(s, 0, sizeof(s));
    for (int k = 0; k < n; k++) {
        FILE *f = fopen(argv[2 + k], "rb");
        if (!f) {
            perror(argv[2 + k]);
            return 1;
        }
        fseek(f, 0, SEEK_END);
        s[k].len = ftell(f);
        fseek(f, 0, SEEK_SET);
        s[k].buf = malloc((size_t)s[k].len);
        if (!s[k].buf || fread(s[k].buf, 1, (size_t)s[k].len, f) != (size_t)s[k].len) {
            return 1;
        }
        fclose(f);
        s[k].hash = decode(&s[k], &s[k].pics);
        if (!s[k].pics) {
            fprintf(stderr, "%s: no pictures\n", argv[2 + k]);
            return 1;
        }
    }
    nthreads = n;
    pthread_t t[MAXS];
    for (int k = 0; k < n; k++) {
        pthread_create(&t[k], NULL, worker, &s[k]);
    }
    int fails = 0;
    for (int k = 0; k < n; k++) {
        pthread_join(t[k], NULL);
        if (s[k].fail) {
            printf("%s: %d of %d concurrent decodes differ from decoding it alone\n", argv[2 + k], s[k].fail, rounds);
        }
        fails += s[k].fail;
    }
    if (!fails) {
        printf("threads ok: %d streams, %d rounds\n", n, rounds);
    }
    return fails ? 1 : 0;
}
