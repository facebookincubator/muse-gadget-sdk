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
 * SBC test tool for xplat/libsbc, 16 kHz mono, 8 subbands, loudness:
 *
 *   sbc_tool roundtrip BLOCKS BITPOOL IN.raw OUT.raw   encode, decode back
 *   sbc_tool msbc IN.raw OUT.raw                               the same with mSBC
 *   sbc_tool refuse                                            frames and arguments refused
 *   sbc_tool digest                                            hashes of encode + decode
 *   sbc_tool stereo                                            dual channel and its concealment
 */

#include <sbc.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "signal.h"

static struct sbc_frame frame_of(int msbc, int nblocks, int bitpool)
{
    struct sbc_frame f = { .msbc = msbc, .freq = SBC_FREQ_16K, .mode = SBC_MODE_MONO,
                           .bam = SBC_BAM_LOUDNESS, .nblocks = msbc ? 15 : nblocks,
                           .nsubbands = 8, .bitpool = msbc ? 26 : bitpool };
    return f;
}

/* Encode `nf` frames of `in`, decode them back into `out`; the stream into
 * `stream` when not NULL. Returns the frame size, -1 on error. */
static int run(struct sbc_frame fr, const int16_t *in, int nf, int16_t *out, uint8_t *stream)
{
    sbc_t enc, dec;
    uint8_t data[512];
    int ns = fr.nblocks * fr.nsubbands, size = (int)sbc_get_frame_size(&fr);
    sbc_reset(&enc), sbc_reset(&dec);
    for (int f = 0; f < nf; f++) {
        struct sbc_frame got;
        if (sbc_encode(&enc, in + f * ns, 1, NULL, 0, &fr, data, sizeof data))
            return -1;
        if (stream)
            memcpy(stream + f * size, data, (size_t)size);
        if (sbc_decode(&dec, data, (unsigned)size, &got, out + f * ns, 1, NULL, 0))
            return -1;
    }
    return size;
}

static int roundtrip(struct sbc_frame fr, const char *in, const char *out)
{
    size_t n;
    int16_t *pcm = read_file(in, &n);
    int ns = fr.nblocks * fr.nsubbands, nf = (int)(n / 2 / ns);
    int16_t *res = malloc((size_t)nf * ns * 2 + 2);
    if (run(fr, pcm, nf, res, NULL) < 0) {
        fprintf(stderr, "sbc failed\n");
        return 1;
    }
    write_file(out, res, (size_t)nf * ns * 2);
    free(pcm), free(res);
    return 0;
}

static int refuse(void)
{
    sbc_t sbc;
    struct sbc_frame f = frame_of(0, 16, 26), got;
    int16_t pcm[2][SBC_MAX_SAMPLES];
    uint8_t data[512];

    sbc_reset(&sbc);
    printf("conceal before a good frame %d\n", sbc_decode(&sbc, NULL, 0, &got, pcm[0], 1, NULL, 0));

    struct sbc_frame bad = f;
    bad.freq = SBC_NUM_FREQ;
    printf("frequency out of range size %u encode %d\n", sbc_get_frame_size(&bad),
           sbc_encode(&sbc, pcm[0], 1, NULL, 0, &bad, data, sizeof data));
    bad = f, bad.mode = SBC_NUM_MODE;
    printf("mode out of range size %u\n", sbc_get_frame_size(&bad));
    bad = f, bad.bam = SBC_NUM_BAM;
    printf("allocation out of range size %u\n", sbc_get_frame_size(&bad));

    /* A joint stereo frame and a 4-subband frame, all-zero with valid CRCs: a
     * full build decodes both, a mono 8-subband build must refuse both before
     * writing anything */
    static const uint8_t joint[] = { 0x9c, 0x3d, 0x35, 0x9f }, sb4[] = { 0x9c, 0x30, 0x1a, 0x10 };
    memset(data, 0, sizeof data);
    memcpy(data, joint, sizeof joint);
    printf("joint stereo frame %d\n", sbc_decode(&sbc, data, sizeof data, &got, pcm[0], 1, pcm[1], 1));
    memcpy(data, sb4, sizeof sb4);
    printf("4-subband frame %d\n", sbc_decode(&sbc, data, sizeof data, &got, pcm[0], 1, pcm[1], 1));
    printf("short frame %d\n", sbc_decode(&sbc, data, 3, &got, pcm[0], 1, NULL, 0));
    return 0;
}

/* The two libsbc fixes, in a build with two channels: a dual-channel encode
 * (whose scale factors ASan watches under --sanitize) decodes back, and
 * concealment follows the last good frame's channel count: a stereo frame's
 * needs the right-channel buffer and writes it, a mono frame's after it
 * doesn't. */
static int stereo(void)
{
#if SBC_MAX_CHANNELS > 1
    sbc_t enc, dec;
    struct sbc_frame f = frame_of(0, 16, 32), m = frame_of(0, 16, 26), got;
    int16_t l[SBC_MAX_SAMPLES], r[SBC_MAX_SAMPLES], ol[SBC_MAX_SAMPLES], orr[SBC_MAX_SAMPLES];
    uint8_t data[512];
    int ns = f.nblocks * f.nsubbands;

    f.mode = SBC_MODE_DUAL_CHANNEL;
    for (int i = 0; i < ns; i++)
        l[i] = sig_sample(0, i, 16000), r[i] = sig_sample(1, i, 16000);
    sbc_reset(&enc), sbc_reset(&dec);
    int e = sbc_encode(&enc, l, 1, r, 1, &f, data, sizeof data);
    int d = sbc_decode(&dec, data, sbc_get_frame_size(&f), &got, ol, 1, orr, 1);
    printf("dual channel encode %d decode %d channels %d\n", e, d, got.mode == SBC_MODE_DUAL_CHANNEL ? 2 : 1);
    printf("stereo conceal without a right buffer %d\n", sbc_decode(&dec, NULL, 0, &got, ol, 1, NULL, 0));
    for (int i = 0; i < ns; i++)
        orr[i] = 0x7777;
    d = sbc_decode(&dec, NULL, 0, &got, ol, 1, orr, 1);
    int written = 1;
    for (int i = 0; i < ns; i++)
        written &= orr[i] != 0x7777;
    printf("stereo conceal %d right written %d\n", d, written);

    sbc_reset(&enc);
    e = sbc_encode(&enc, l, 1, NULL, 0, &m, data, sizeof data);
    d = sbc_decode(&dec, data, sbc_get_frame_size(&m), &got, ol, 1, NULL, 0);
    printf("mono after stereo %d %d conceal %d\n", e, d, sbc_decode(&dec, NULL, 0, &got, ol, 1, NULL, 0));
#endif
    return 0;
}

static int digest(void)
{
    enum { NF = 100 };
    static int16_t in[NF * 128], out[NF * 128];
    static uint8_t stream[NF * 512];
    static const struct { int msbc, nblocks, bitpool; } cfg[] = {
        { 0, 16, 26 }, { 1, 15, 26 }, { 0, 16, 2 }, { 0, 4, 60 }, { 0, 16, 124 },
    };
    for (unsigned c = 0; c < sizeof cfg / sizeof *cfg; c++) {
        struct sbc_frame fr = frame_of(cfg[c].msbc, cfg[c].nblocks, cfg[c].bitpool);
        int ns = fr.nblocks * fr.nsubbands;
        for (int kind = 0; kind < SIG_KINDS; kind++) {
            for (int i = 0; i < NF * ns; i++)
                in[i] = sig_sample(kind, i, 16000);
            int size = run(fr, in, NF, out, stream);
            if (size < 0) {
                printf("digest %d %d %d %d failed\n", fr.msbc, fr.nblocks, fr.bitpool, kind);
                continue;
            }
            uint64_t h = fnv(fnv(FNV_INIT, stream, (size_t)NF * size), out, (size_t)NF * ns * 2);
            printf("digest %d %d %d %d %016llx\n", fr.msbc, fr.nblocks, fr.bitpool, kind, (unsigned long long)h);
        }
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc >= 6 && !strcmp(argv[1], "roundtrip"))
        return roundtrip(frame_of(0, atoi(argv[2]), atoi(argv[3])), argv[4], argv[5]);
    if (argc >= 4 && !strcmp(argv[1], "msbc"))
        return roundtrip(frame_of(1, 15, 26), argv[2], argv[3]);
    if (argc >= 2 && !strcmp(argv[1], "refuse"))
        return refuse();
    if (argc >= 2 && !strcmp(argv[1], "digest"))
        return digest();
    if (argc >= 2 && !strcmp(argv[1], "stereo"))
        return stereo();
    fprintf(stderr, "usage: sbc_tool roundtrip|msbc|refuse|digest|stereo ...\n");
    return 2;
}
