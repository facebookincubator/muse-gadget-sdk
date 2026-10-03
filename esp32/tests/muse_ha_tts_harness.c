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

/* Drives muse_ha_tts_text.c for test_muse_ha_tts.py.
 *   clean                 stdin is a reply: prints it readied for speech
 *   id3                   stdin is the start of an MP3: prints its ID3 tag's size
 *   base BASE CAP         prints BASE without trailing slashes, in CAP bytes
 *   url BASE PATH URL CAP prints the audio URL ("-" for a missing PATH or URL),
 *                         or exits 1 when there's none
 *   speech MAX GROWS PIECE...
 *                         appends each PIECE to a speech buffer of at most MAX
 *                         bytes, whose allocations fail after GROWS of them
 *                         (-1: never); prints each append's result, then
 *                         "cut=<0|1> len=<n>" and the text on its own line */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "muse_ha_tts_text.h"

static char *read_all(size_t *len)
{
    size_t cap = 1 << 16, n = 0;
    char *buf = malloc(cap + 1);
    size_t got;
    while (buf && (got = fread(buf + n, 1, cap - n, stdin)) > 0) {
        n += got;
        if (n == cap) {
            cap *= 2;
            buf = realloc(buf, cap + 1);
        }
    }
    if (!buf) {
        exit(2);
    }
    buf[n] = '\0';
    *len = n;
    return buf;
}

static long s_grows_left;

static void *limited_realloc(void *p, size_t n)
{
    if (s_grows_left == 0) {
        return NULL;
    }
    if (s_grows_left > 0) {
        s_grows_left--;
    }
    return realloc(p, n);
}

static const char *arg(const char *s)
{
    return strcmp(s, "-") ? s : NULL;
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "clean")) {
        size_t len;
        char *in = read_all(&len);
        muse_ha_tts_clean(in);
        fputs(in, stdout);
        free(in);
        return 0;
    }
    if (argc > 1 && !strcmp(argv[1], "id3")) {
        size_t len;
        char *in = read_all(&len);
        printf("%zu\n", muse_ha_tts_id3_size((const uint8_t *)in, len));
        free(in);
        return 0;
    }
    if (argc == 4 && !strcmp(argv[1], "base")) {
        size_t cap = strtoul(argv[3], NULL, 10);
        char *out = malloc(cap + 1);
        muse_ha_tts_base(out, cap, argv[2]);
        if (cap) {
            puts(out);
        }
        free(out);
        return 0;
    }
    if (argc == 6 && !strcmp(argv[1], "url")) {
        size_t cap = strtoul(argv[5], NULL, 10);
        char *out = malloc(cap);
        int ok = muse_ha_tts_audio_url(out, cap, argv[2], arg(argv[3]), arg(argv[4]));
        if (ok) {
            puts(out);
        }
        free(out);
        return ok ? 0 : 1;
    }
    if (argc >= 4 && !strcmp(argv[1], "speech")) {
        size_t max = strtoul(argv[2], NULL, 10);
        s_grows_left = strtol(argv[3], NULL, 10);
        muse_ha_tts_speech_t sp = { 0 };
        for (int i = 4; i < argc; i++) {
            printf("%d", muse_ha_tts_speech_append(&sp, argv[i], max, limited_realloc));
            if (sp.buf && (strlen(sp.buf) != sp.len || sp.len >= sp.cap || sp.cap > max)) {
                fprintf(stderr, "bad buffer: len %zu cap %zu max %zu\n", sp.len, sp.cap, max);
                return 3;
            }
        }
        printf("\ncut=%d len=%zu\n%s\n", sp.cut, sp.len, sp.buf ? sp.buf : "");
        muse_ha_tts_speech_free(&sp, free);
        if (sp.buf || sp.len || sp.cap || sp.cut) {
            return 3;
        }
        return 0;
    }
    fprintf(stderr, "usage: clean | id3 | base BASE CAP | url BASE PATH URL CAP | speech MAX GROWS PIECE...\n");
    return 2;
}
