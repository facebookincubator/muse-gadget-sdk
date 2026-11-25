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
 * Fuzz target for the decoders that read a peer's bytes: LC3 (16 and 24 kHz,
 * 10 ms; built once with lc3fx and once with float liblc3, which share the
 * API) and libsbc (the SDKs' mono, 8-subband build). The first byte picks the
 * decoder and the frame size, the rest is the frame.
 *
 * With libFuzzer (clang -fsanitize=fuzzer) it is a fuzz target. Built with
 * -DFUZZ_STANDALONE it runs by itself for SECONDS (default 10): seeded random
 * frames, and valid frames with random bytes changed.
 */

#include <lc3.h>
#include <sbc.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    static LC3_DECODER_MEM_T(10000, 24000) lc3mem;
    static int16_t pcm[2][SBC_MAX_SAMPLES * 2];
    if (size < 1)
        return 0;
    unsigned sel = data[0];
    data++, size--;

    if (sel & 0x80) {
        /* libsbc: a fresh context, then the frame twice (state from the first)
         * and a concealed frame after it */
        sbc_t sbc;
        struct sbc_frame frame;
        sbc_reset(&sbc);
        for (int i = 0; i < 2; i++)
            sbc_decode(&sbc, data, (unsigned)size, &frame, pcm[0], 1, pcm[1], 1);
        sbc_decode(&sbc, NULL, 0, &frame, pcm[0], 1, pcm[1], 1);
        return 0;
    }

    /* LC3: rate from bit 6, the rest of the input split into frames of
     * 20..400 bytes (size from the low bits), each decoded in turn, with a
     * concealed frame between */
    int sr = sel & 0x40 ? 24000 : 16000;
    int nb = 20 + (int)(sel & 0x3f) * 6;
    lc3_decoder_t d = lc3_setup_decoder(10000, sr, 0, &lc3mem);
    if (!d)
        abort();
    if (size < (size_t)nb)
        nb = (int)size;
    for (size_t off = 0; off + (size_t)nb <= size && nb > 0; off += (size_t)nb) {
        lc3_decode(d, data + off, nb, LC3_PCM_FORMAT_S16, pcm[0], 1);
        lc3_decode(d, NULL, nb, LC3_PCM_FORMAT_S16, pcm[0], 1);
    }
    return 0;
}

#ifdef FUZZ_STANDALONE
#include <time.h>

static uint32_t rng = 1;
static uint32_t rnd(void) { rng = rng * 1664525u + 1013904223u; return rng >> 8; }

int main(int argc, char **argv)
{
    double secs = argc > 1 ? atof(argv[1]) : 10;
    static uint8_t buf[1 + 4 * 400], valid[3][1 + 400];
    size_t vsize[3];

    /* Valid frames to mutate: LC3 at 16 and 24 kHz, and an SBC frame */
    {
        static LC3_ENCODER_MEM_T(10000, 24000) mem;
        int16_t in[240];
        for (int i = 0; i < 240; i++)
            in[i] = (int16_t)((i * 977) % 6000 - 3000);
        for (int k = 0; k < 2; k++) {
            int sr = k ? 24000 : 16000;
            lc3_encoder_t e = lc3_setup_encoder(10000, sr, 0, &mem);
            valid[k][0] = (uint8_t)((k ? 0x40 : 0) | 3);   /* 38 bytes */
            lc3_encode(e, LC3_PCM_FORMAT_S16, in, 1, 38, valid[k] + 1);
            vsize[k] = 1 + 38;
        }
        sbc_t sbc;
        struct sbc_frame f = { .freq = SBC_FREQ_16K, .mode = SBC_MODE_MONO, .bam = SBC_BAM_LOUDNESS,
                               .nblocks = 16, .nsubbands = 8, .bitpool = 26 };
        int16_t pcm[128];
        for (int i = 0; i < 128; i++)
            pcm[i] = (int16_t)((i * 1231) % 8000 - 4000);
        sbc_reset(&sbc);
        valid[2][0] = 0x80;
        sbc_encode(&sbc, pcm, 1, NULL, 0, &f, valid[2] + 1, 400);
        vsize[2] = 1 + sbc_get_frame_size(&f);
    }

    clock_t end = clock() + (clock_t)(secs * CLOCKS_PER_SEC);
    unsigned long n = 0;
    while (clock() < end) {
        for (int k = 0; k < 64; k++, n++) {
            size_t size;
            if (n % 2) {   /* random bytes */
                size = 1 + rnd() % (sizeof buf - 1);
                for (size_t i = 0; i < size; i++)
                    buf[i] = (uint8_t)rnd();
            } else {       /* a valid frame with 1 to 8 bytes changed */
                int v = (int)(n / 2 % 3);
                size = vsize[v];
                memcpy(buf, valid[v], size);
                for (int m = 1 + (int)(rnd() % 8); m > 0; m--)
                    buf[1 + rnd() % (size - 1)] = (uint8_t)rnd();
            }
            LLVMFuzzerTestOneInput(buf, size);
        }
    }
    printf("fuzz: %lu inputs, no fault\n", n);
    return 0;
}
#endif
