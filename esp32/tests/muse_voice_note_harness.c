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

/* Drives the voice note's encoding helpers (muse_chat_text.c) for
 * test_muse_voice_note.py, which checks the output against CPython.
 *   header R   prints the 44-byte WAV header for rate R
 *   base64     stdin is records of <u32le n><n bytes>; prints <u32le ret> and
 *              the output buffer for each, including 16 guard bytes after the
 *              4*ceil(n/3) the encoding should fill
 *   body       stdin is 16 kHz PCM: prints the POST body a voice note sends */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "muse_chat.h"
#include "muse_chat_priv.h"
#include "muse_state.h"

#define GUARD 16
#define GUARD_BYTE 0xa5

/* muse_chat_text.c's captions page to the screen; nothing here pages. */
void muse_state_page(bool cjk, int *cols, int *lines)
{
    (void)cjk;
    *cols = 16;
    *lines = 2;
}

static uint8_t *read_all(size_t *len)
{
    size_t cap = 1 << 16, n = 0, got;
    uint8_t *buf = malloc(cap);
    while (buf && (got = fread(buf + n, 1, cap - n, stdin)) > 0) {
        n += got;
        if (n == cap) {
            cap *= 2;
            buf = realloc(buf, cap);
        }
    }
    if (!buf) {
        exit(2);
    }
    *len = n;
    return buf;
}

static uint32_t get_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void put_le32(uint32_t v)
{
    uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
    fwrite(b, 1, 4, stdout);
}

static int base64_records(const uint8_t *in, size_t len)
{
    size_t at = 0;
    while (at < len) {
        if (len - at < 4 || len - at - 4 < get_le32(in + at)) {
            return 2;
        }
        size_t n = get_le32(in + at);
        at += 4;
        /* Exact-size copies, so ASan sees a read past either end. */
        uint8_t *src = malloc(n ? n : 1);
        size_t want = (n + 2) / 3 * 4;
        char *out = malloc(want + GUARD);
        if (!src || !out) {
            return 2;
        }
        memcpy(src, in + at, n);
        memset(out, GUARD_BYTE, want + GUARD);
        size_t ret = muse_hatch_base64(src, n, out);
        put_le32((uint32_t)ret);
        fwrite(out, 1, want + GUARD, stdout);
        free(src);
        free(out);
        at += n;
    }
    return 0;
}

static int body(const uint8_t *pcm, size_t n)
{
    size_t wav_len = MUSE_HATCH_WAV_HEADER + n;
    uint8_t *wav = malloc(wav_len);
    char *b64 = malloc((wav_len + 2) / 3 * 4);
    if (!wav || !b64) {
        return 2;
    }
    muse_hatch_wav_header(wav, 16000);
    memcpy(wav + MUSE_HATCH_WAV_HEADER, pcm, n);
    size_t b64_len = muse_hatch_base64(wav, wav_len, b64);
    fputs(MUSE_HATCH_NOTE_HEAD, stdout);
    fwrite(b64, 1, b64_len, stdout);
    fputs(MUSE_HATCH_NOTE_TAIL, stdout);
    free(wav);
    free(b64);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 3 && !strcmp(argv[1], "header")) {
        uint8_t h[MUSE_HATCH_WAV_HEADER];
        muse_hatch_wav_header(h, (uint32_t)strtoul(argv[2], NULL, 10));
        fwrite(h, 1, sizeof(h), stdout);
        return 0;
    }
    if (argc != 2) {
        return 2;
    }
    size_t len;
    uint8_t *in = read_all(&len);
    int rc = !strcmp(argv[1], "base64") ? base64_records(in, len) : !strcmp(argv[1], "body") ? body(in, len) : 2;
    free(in);
    return rc;
}
