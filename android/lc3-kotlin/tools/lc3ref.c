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

/* liblc3 reference CLI for the conformance tools (16 kHz mono, s16le raw PCM, concatenated fixed-size frames).
 *   lc3ref enc IN.raw OUT.lc3 [-d dt_us] [-b nbytes] [-l]      -l: encoder LTPF off (what the Kotlin encoder does)
 *   lc3ref dec IN.lc3 OUT.raw [-d dt_us] [-b nbytes] [-P n] [-X seed]
 *      -P n: every n-th frame lost (concealed)   -X seed: corrupt frames (see corrupt() below, mirrored in the tests)
 * Build it scalar and without FMA contraction to get the reference the Kotlin port matches bit for bit:
 *   cc -O2 -ffp-contract=off -U__ARM_NEON -DLC3_PLUS=0 -DLC3_PLUS_HR=0 -I$LIBLC3/include $LIBLC3/src/*.c lc3ref.c */
#include <lc3.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void *slurp(const char *path, long *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(1); }
    if (fseek(f, 0, SEEK_END) || (*len = ftell(f)) < 0 || fseek(f, 0, SEEK_SET)) { perror(path); exit(1); }
    void *p = malloc(*len + 1);
    if (!p) { perror("malloc"); exit(1); }
    if (fread(p, 1, *len, f) != (size_t)*len) { perror("read"); exit(1); }
    fclose(f);
    return p;
}

/* Deterministic corruption: per frame, an LCG decides to leave it (3/4), flip 1-8 bits (3/16) or fill it with
 * noise (1/16). Lc3ConformanceTest.corrupt() is the same function. */
static void corrupt(uint8_t *fr, int nbytes, uint32_t *s)
{
#define NEXT() (*s = *s * 1664525u + 1013904223u, *s >> 8)
    uint32_t r = NEXT() % 16;
    if (r < 12) return;
    if (r < 15) {
        int n = 1 + NEXT() % 8;
        for (int i = 0; i < n; i++) { uint32_t b = NEXT() % (uint32_t)(nbytes * 8); fr[b / 8] ^= 1u << (b % 8); }
    } else {
        for (int i = 0; i < nbytes; i++) fr[i] = NEXT() & 0xff;
    }
#undef NEXT
}

/* The value after option argv[*i], as an int */
static int optval(int argc, char **argv, int *i)
{
    if (*i + 1 >= argc) { fprintf(stderr, "%s needs a value\n", argv[*i]); exit(2); }
    return atoi(argv[++*i]);
}

int main(int argc, char **argv)
{
    if (argc < 4) { fprintf(stderr, "usage: see source\n"); return 2; }
    int dt_us = 10000, nbytes = 40, noltpf = 0, lossp = 0, xseed = -1;
    for (int i = 4; i < argc; i++) {
        if (!strcmp(argv[i], "-d")) dt_us = optval(argc, argv, &i);
        else if (!strcmp(argv[i], "-b")) nbytes = optval(argc, argv, &i);
        else if (!strcmp(argv[i], "-l")) noltpf = 1;
        else if (!strcmp(argv[i], "-P")) lossp = optval(argc, argv, &i);
        else if (!strcmp(argv[i], "-X")) xseed = optval(argc, argv, &i);
    }
    /* The frame buffers below hold 400 bytes and 480 samples */
    if ((dt_us != 7500 && dt_us != 10000) || nbytes < 20 || nbytes > 400 || lossp < 0) {
        fprintf(stderr, "-d takes 7500 or 10000, -b 20 to 400, -P 0 or more\n");
        return 2;
    }
    int ns = lc3_frame_samples(dt_us, 16000);
    long len;
    uint8_t *in = slurp(argv[2], &len);
    FILE *fo = fopen(argv[3], "wb");
    if (!fo) { perror(argv[3]); return 1; }
    static char mem[16384] __attribute__((aligned(16)));
    if (!strcmp(argv[1], "enc")) {
        lc3_encoder_t e = lc3_setup_encoder(dt_us, 16000, 0, mem);
        if (!e) { fprintf(stderr, "encoder setup failed\n"); return 1; }
        if (noltpf) lc3_encoder_disable_ltpf(e);
        uint8_t fr[400];
        for (long f = 0; f < len / 2 / ns; f++) {
            if (lc3_encode(e, LC3_PCM_FORMAT_S16, (int16_t *)in + f * ns, 1, nbytes, fr) < 0) {
                fprintf(stderr, "encode failed\n"); return 1;
            }
            fwrite(fr, 1, nbytes, fo);
        }
    } else {
        lc3_decoder_t d = lc3_setup_decoder(dt_us, 16000, 0, mem);
        if (!d) { fprintf(stderr, "decoder setup failed\n"); return 1; }
        int16_t pcm[480]; uint8_t fr[400]; uint32_t s = (uint32_t)xseed;
        for (long f = 0; f < len / nbytes; f++) {
            memcpy(fr, in + f * nbytes, nbytes);
            if (xseed >= 0) corrupt(fr, nbytes, &s);
            int lost = lossp && (f % lossp) == lossp - 1;
            lc3_decode(d, lost ? NULL : fr, nbytes, LC3_PCM_FORMAT_S16, pcm, 1);
            fwrite(pcm, 2, ns, fo);   /* little-endian hosts */
        }
    }
    fclose(fo);
    return 0;
}
