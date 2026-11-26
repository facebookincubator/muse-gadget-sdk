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
 * One LC3 implementation (whichever lc3.h it's built with: xplat/lc3fx or
 * xplat/liblc3) as a file tool, so tests/test_mg_codecs.py can cross the two:
 *   lc3_interop_tool enc IN.raw OUT.lc3 [RATE]   mono s16le -> 40-byte frames
 *   lc3_interop_tool dec IN.lc3 OUT.raw [RATE]   the reverse
 * 10 ms frames, 40 bytes (32 kb/s), as the musegadgets voice path uses, at
 * RATE Hz (16000, the default, or 24000).
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lc3.h"

#define DT_US 10000
#define MAX_FRAME_SAMPLES 480
#define FRAME_BYTES 40

int main(int argc, char **argv)
{
    if (argc != 4 && argc != 5) {
        fprintf(stderr, "usage: %s enc|dec IN OUT [RATE]\n", argv[0]);
        return 2;
    }
    int SR_HZ = argc == 5 ? atoi(argv[4]) : 16000;
    int FRAME_SAMPLES = lc3_frame_samples(DT_US, SR_HZ);
    if (FRAME_SAMPLES <= 0 || FRAME_SAMPLES > MAX_FRAME_SAMPLES) {
        fprintf(stderr, "rate %d refused\n", SR_HZ);
        return 1;
    }
    FILE *in = fopen(argv[2], "rb"), *out = fopen(argv[3], "wb");
    if (!in || !out) {
        perror("open");
        return 1;
    }
    int frames = 0;
    if (!strcmp(argv[1], "enc")) {
        void *mem = malloc(lc3_encoder_size(DT_US, SR_HZ));
        lc3_encoder_t enc = lc3_setup_encoder(DT_US, SR_HZ, 0, mem);
        int16_t pcm[MAX_FRAME_SAMPLES];
        uint8_t frame[FRAME_BYTES];
        while (fread(pcm, sizeof(pcm[0]), FRAME_SAMPLES, in) == FRAME_SAMPLES) {
            if (lc3_encode(enc, LC3_PCM_FORMAT_S16, pcm, 1, FRAME_BYTES, frame) != 0) {
                fprintf(stderr, "encode failed at frame %d\n", frames);
                return 1;
            }
            fwrite(frame, 1, FRAME_BYTES, out);
            frames++;
        }
        free(mem);
    } else {
        void *mem = malloc(lc3_decoder_size(DT_US, SR_HZ));
        lc3_decoder_t dec = lc3_setup_decoder(DT_US, SR_HZ, 0, mem);
        int16_t pcm[MAX_FRAME_SAMPLES];
        uint8_t frame[FRAME_BYTES];
        while (fread(frame, 1, FRAME_BYTES, in) == FRAME_BYTES) {
            if (lc3_decode(dec, frame, FRAME_BYTES, LC3_PCM_FORMAT_S16, pcm, 1) != 0) {
                fprintf(stderr, "decode failed at frame %d\n", frames);
                return 1;
            }
            fwrite(pcm, sizeof(pcm[0]), FRAME_SAMPLES, out);
            frames++;
        }
        free(mem);
    }
    fclose(in);
    fclose(out);
    printf("%d frames\n", frames);
    return 0;
}
