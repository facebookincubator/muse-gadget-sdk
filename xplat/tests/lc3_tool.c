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
 * LC3 test tool, built once against xplat/liblc3 and once against
 * xplat/lc3fx (they export the same API, so never into one binary):
 *
 *   lc3_tool enc RATE NBYTES IN.raw OUT.lc3     10 ms frames, S16 mono
 *   lc3_tool dec RATE NBYTES IN.lc3 OUT.raw [LOSS]   every LOSS-th frame lost
 *   lc3_tool delay RATE                         algorithmic delay, samples
 *   lc3_tool refuse                             configurations and arguments refused
 *   lc3_tool digest                             hashes of encode + decode (integer signals)
 */

#include <lc3.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "signal.h"

#define DT 10000

static int enc(int sr, int nb, const char *in, const char *out)
{
    size_t n;
    int16_t *pcm = read_file(in, &n);
    int ns = lc3_frame_samples(DT, sr), nf = (int)(n / 2 / ns);
    void *mem = malloc(lc3_encoder_size(DT, sr));
    lc3_encoder_t e = lc3_setup_encoder(DT, sr, 0, mem);
    uint8_t *frames = malloc((size_t)nf * nb + 1);
    if (!e) { fprintf(stderr, "setup %d refused\n", sr); return 1; }
    for (int f = 0; f < nf; f++)
        if (lc3_encode(e, LC3_PCM_FORMAT_S16, pcm + f * ns, 1, nb, frames + f * nb)) {
            fprintf(stderr, "encode failed\n");
            return 1;
        }
    write_file(out, frames, (size_t)nf * nb);
    free(frames), free(mem), free(pcm);
    return 0;
}

static int dec(int sr, int nb, const char *in, const char *out, int loss)
{
    size_t n;
    uint8_t *frames = read_file(in, &n);
    int ns = lc3_frame_samples(DT, sr), nf = (int)(n / nb);
    void *mem = malloc(lc3_decoder_size(DT, sr));
    lc3_decoder_t d = lc3_setup_decoder(DT, sr, 0, mem);
    int16_t *pcm = malloc((size_t)nf * ns * 2 + 2);
    if (!d) { fprintf(stderr, "setup %d refused\n", sr); return 1; }
    for (int f = 0; f < nf; f++) {
        int lost = loss && f % loss == loss - 1;
        /* 0: decoded; 1: concealed. An intact frame must decode: concealing it means it read as corrupt. */
        int r = lc3_decode(d, lost ? NULL : frames + f * nb, nb, LC3_PCM_FORMAT_S16, pcm + f * ns, 1);
        if (r != (lost ? 1 : 0)) {
            fprintf(stderr, "frame %d: decode returned %d\n", f, r);
            return 1;
        }
    }
    write_file(out, pcm, (size_t)nf * ns * 2);
    free(frames), free(mem), free(pcm);
    return 0;
}

static int refuse(void)
{
    static const int rates[] = { 8000, 16000, 24000, 32000, 48000 };
    static uint64_t mem[8192];   /* aligned like LC3_ENCODER_MEM_T */
    for (unsigned i = 0; i < sizeof rates / sizeof *rates; i++)
        printf("setup %d enc %d dec %d\n", rates[i],
               lc3_setup_encoder(DT, rates[i], 0, mem) != NULL,
               lc3_setup_decoder(DT, rates[i], 0, mem) != NULL);
    printf("setup 7500us enc %d dec %d\n", lc3_setup_encoder(7500, 16000, 0, mem) != NULL,
           lc3_setup_decoder(7500, 16000, 0, mem) != NULL);
    for (int hz = 16000; hz <= 48000; hz += 32000)
        printf("hr %d enc %d dec %d samples %d bytes %d\n", hz,
               lc3_hr_setup_encoder(true, DT, hz, 0, mem) != NULL, lc3_hr_setup_decoder(true, DT, hz, 0, mem) != NULL,
               lc3_hr_frame_samples(true, DT, hz), lc3_hr_frame_bytes(true, DT, hz, 500000));

    int16_t pcm[160] = { 0 };
    uint8_t frame[401] = { 0 };
    void *emem = malloc(lc3_encoder_size(DT, 16000)), *dmem = malloc(lc3_decoder_size(DT, 16000));
    lc3_encoder_t e = emem ? lc3_setup_encoder(DT, 16000, 0, emem) : NULL;
    lc3_decoder_t d = dmem ? lc3_setup_decoder(DT, 16000, 0, dmem) : NULL;
    if (!e || !d) { free(emem), free(dmem); return 1; }
    printf("encode bytes 19 %d 401 %d\n", lc3_encode(e, LC3_PCM_FORMAT_S16, pcm, 1, 19, frame),
           lc3_encode(e, LC3_PCM_FORMAT_S16, pcm, 1, 401, frame));
    printf("decode bytes 19 %d 401 %d\n", lc3_decode(d, frame, 19, LC3_PCM_FORMAT_S16, pcm, 1),
           lc3_decode(d, frame, 401, LC3_PCM_FORMAT_S16, pcm, 1));
    printf("format 4 enc %d dec %d\n", lc3_encode(e, (enum lc3_pcm_format)4, pcm, 1, 40, frame),
           lc3_decode(d, frame, 40, (enum lc3_pcm_format)4, pcm, 1));
    free(emem), free(dmem);
    return 0;
}

static int digest(void)
{
    static const int rates[] = { 16000, 24000 }, sizes[] = { 20, 40, 80, 155 };
    enum { NF = 60 };
    static int16_t in[NF * 240], out[NF * 240];
    static uint8_t frames[NF * 155];
    void *em = malloc(lc3_encoder_size(DT, 24000)), *dm = malloc(lc3_decoder_size(DT, 24000));
    for (int r = 0; r < 2; r++)
        for (int b = 0; b < 4; b++) {
            int sr = rates[r], nb = sizes[b], ns = lc3_frame_samples(DT, sr);
            for (int kind = 0; kind < SIG_KINDS; kind++) {
                for (int i = 0; i < NF * ns; i++)
                    in[i] = sig_sample(kind, i, sr);
                lc3_encoder_t e = lc3_setup_encoder(DT, sr, 0, em);
                lc3_decoder_t d = lc3_setup_decoder(DT, sr, 0, dm);
                for (int f = 0; f < NF; f++) {
                    lc3_encode(e, LC3_PCM_FORMAT_S16, in + f * ns, 1, nb, frames + f * nb);
                    lc3_decode(d, f % 7 == 6 ? NULL : frames + f * nb, nb, LC3_PCM_FORMAT_S16, out + f * ns, 1);
                }
                uint64_t h = fnv(fnv(FNV_INIT, frames, (size_t)NF * nb), out, (size_t)NF * ns * 2);
                printf("digest %d %d %d %016llx\n", sr, nb, kind, (unsigned long long)h);
            }
            /* random bytes as frames: the decoder's error and concealment paths */
            lc3_decoder_t d = lc3_setup_decoder(DT, sr, 0, dm);
            sig_rng = 777u + (uint32_t)(sr + nb);
            for (int f = 0; f < NF; f++) {
                for (int i = 0; i < nb; i++)
                    frames[i] = (uint8_t)(sig_rand() >> 24);
                lc3_decode(d, frames, nb, LC3_PCM_FORMAT_S16, out + f * ns, 1);
            }
            printf("digest %d %d random %016llx\n", sr, nb,
                   (unsigned long long)fnv(FNV_INIT, out, (size_t)NF * ns * 2));
        }
    free(em), free(dm);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc >= 6 && !strcmp(argv[1], "enc"))
        return enc(atoi(argv[2]), atoi(argv[3]), argv[4], argv[5]);
    if (argc >= 6 && !strcmp(argv[1], "dec"))
        return dec(atoi(argv[2]), atoi(argv[3]), argv[4], argv[5], argc > 6 ? atoi(argv[6]) : 0);
    if (argc >= 3 && !strcmp(argv[1], "delay"))
        return printf("%d\n", lc3_delay_samples(DT, atoi(argv[2]))) < 0;
    if (argc >= 2 && !strcmp(argv[1], "refuse"))
        return refuse();
    if (argc >= 2 && !strcmp(argv[1], "digest"))
        return digest();
    fprintf(stderr, "usage: lc3_tool enc|dec|delay|refuse|digest ...\n");
    return 2;
}
