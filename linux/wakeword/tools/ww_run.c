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
 * ww-run: the Linux Hey Muse runner on a file, for checks.
 *
 *   ww-run [--sensitivity low|default|high] [--scores OUT.f32] [--chunk N] FILE
 *
 * FILE is a 16 kHz mono 16-bit WAV, or raw signed 16-bit little-endian PCM
 * (any other name). Feeds it in N-sample chunks (default 320, as the
 * firmware does) and prints one line per wake, "wake <seconds>", then
 * "inferences <count>". --scores writes every inference's probability as
 * float32.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ww_runner.h"

static FILE *g_scores;
static unsigned long g_inferences;

static void on_score(void *ctx, float p)
{
    (void)ctx;
    g_inferences++;
    if (g_scores != NULL) {
        fwrite(&p, sizeof(p), 1, g_scores);
    }
}

static uint32_t le32(const unsigned char *b) { return b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24; }
static uint16_t le16(const unsigned char *b) { return (uint16_t)(b[0] | b[1] << 8); }

/* Positions f at the samples and sets *bytes to their length; returns 0 if
 * FILE is not a usable WAV. */
static int wav_data(FILE *f, unsigned long long *bytes)
{
    unsigned char h[12];
    if (fread(h, 1, 12, f) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) {
        return 0;
    }
    int fmt_ok = 0;
    for (;;) {
        unsigned char c[8];
        if (fread(c, 1, 8, f) != 8) {
            return 0;
        }
        uint32_t len = le32(c + 4);
        if (!memcmp(c, "fmt ", 4)) {
            unsigned char fm[16];
            if (len < 16 || fread(fm, 1, 16, f) != 16) {
                return 0;
            }
            fmt_ok = le16(fm) == 1 && le16(fm + 2) == 1 && le32(fm + 4) == 16000 && le16(fm + 14) == 16;
            if (fseek(f, (long)(len - 16 + (len & 1)), SEEK_CUR)) {
                return 0;
            }
        } else if (!memcmp(c, "data", 4)) {
            *bytes = len;
            return fmt_ok;
        } else if (fseek(f, (long)(len + (len & 1)), SEEK_CUR)) {
            return 0;
        }
    }
}

int main(int argc, char **argv)
{
    ww_sensitivity_t sensitivity = WW_SENSITIVITY_DEFAULT;
    const char *scores = NULL, *path = NULL;
    size_t chunk = 320;
    bool bad = false;
    for (int i = 1; i < argc && !bad; i++) {
        const bool option = argv[i][0] == '-' && argv[i][1] == '-';
        if (option && i + 1 >= argc) {
            bad = true;   /* an option without its value */
        } else if (!strcmp(argv[i], "--sensitivity")) {
            const char *s = argv[++i];
            if (!strcmp(s, "low")) {
                sensitivity = WW_SENSITIVITY_LOW;
            } else if (!strcmp(s, "high")) {
                sensitivity = WW_SENSITIVITY_HIGH;
            } else if (!strcmp(s, "default")) {
                sensitivity = WW_SENSITIVITY_DEFAULT;
            } else {
                bad = true;
            }
        } else if (!strcmp(argv[i], "--scores")) {
            scores = argv[++i];
        } else if (!strcmp(argv[i], "--chunk")) {
            chunk = (size_t)strtoul(argv[++i], NULL, 10);
        } else if (option || path != NULL) {
            bad = true;
        } else {
            path = argv[i];
        }
    }
    if (bad || path == NULL || chunk == 0 || chunk > 16000) {
        fprintf(stderr, "usage: ww-run [--sensitivity low|default|high] [--scores OUT.f32] [--chunk N] FILE\n");
        return 2;
    }
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        perror(path);
        return 1;
    }
    size_t n = strlen(path);
    unsigned long long left = ~0ULL;   /* raw PCM: to the end of the file */
    if (n > 4 && !strcmp(path + n - 4, ".wav") && !wav_data(f, &left)) {
        fprintf(stderr, "%s: need a 16 kHz mono 16-bit PCM WAV\n", path);
        return 1;
    }
    if (scores != NULL && (g_scores = fopen(scores, "wb")) == NULL) {
        perror(scores);
        return 1;
    }
    ww_config_t cfg = { .sample_rate_hz = 16000, .sensitivity = sensitivity, .vad = WW_VAD_ENERGY };
    ww_runner_t *runner = ww_runner_create(&cfg);
    if (runner == NULL) {
        fprintf(stderr, "the runner didn't start\n");
        return 1;
    }
    ww_runner_set_score_hook(runner, on_score, NULL);
    int16_t *pcm = malloc(chunk * sizeof(int16_t));
    if (pcm == NULL) {
        fprintf(stderr, "out of memory\n");
        return 1;
    }
    unsigned long long fed = 0;
    size_t got;
    /* A WAV's samples end where its data chunk says, not at the end of
     * the file (metadata chunks can follow). */
    size_t want;
    while ((want = left / sizeof(int16_t) < chunk ? (size_t)(left / sizeof(int16_t)) : chunk) > 0 &&
           (got = fread(pcm, sizeof(int16_t), want, f)) > 0) {
        left -= got * sizeof(int16_t);
        ww_event_t ev = ww_runner_feed(runner, pcm, got);
        fed += got;
        if (ev == WW_EVENT_WAKE) {
            printf("wake %.2f\n", (double)fed / 16000.0);
        }
        if (!ww_runner_ok(runner)) {
            fprintf(stderr, "inference failed\n");
            return 1;
        }
    }
    printf("inferences %lu\n", g_inferences);
    ww_runner_destroy(runner);
    free(pcm);
    fclose(f);
    if (g_scores != NULL) {
        fclose(g_scores);
    }
    return 0;
}
