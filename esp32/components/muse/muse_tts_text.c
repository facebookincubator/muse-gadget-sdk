/*
 * Copyright (c) 2026 ledienbien-ai
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

/* A reply's text made ready for the text-to-speech service: cleaned, cut into
 * pieces it accepts, and written into a request. Nothing here touches the
 * hardware, so the host tests run it. */
#include "muse_tts.h"

#include <stdio.h>
#include <string.h>

#include "muse_text.h"

#define TTS_URL "https://translate.google.com/translate_tts?ie=UTF-8&client=tw-ob&tl="
/* A sentence end this early isn't worth a request of its own. */
#define CHUNK_MIN_CHARS 60

/* The code point at s and its length in bytes; a broken byte counts as itself. */
static uint32_t decode(const unsigned char *s, size_t *len)
{
    size_t n = s[0] >= 0xF0 ? 4 : s[0] >= 0xE0 ? 3 : s[0] >= 0xC0 ? 2 : 1;
    uint32_t cp = n == 1 ? s[0] : s[0] & (0x7Fu >> n);
    size_t i = 1;
    for (; i < n && (s[i] & 0xC0) == 0x80; i++) {
        cp = cp << 6 | (s[i] & 0x3F);
    }
    *len = i;
    return cp;
}

/* Letters with their accents: Latin-1 and the extended Latin blocks, the
 * Vietnamese block, and accents typed after their letter. */
static bool is_letter(uint32_t cp)
{
    return (cp >= 0xC0 && cp <= 0x24F) || (cp >= 0x1E00 && cp <= 0x1EFF) || (cp >= 0x300 && cp <= 0x36F);
}

size_t muse_tts_clean(const char *text, char *out, size_t cap)
{
    size_t o = 0;
    bool space = true;   /* no space at the start, none doubled */
    if (!cap) {
        return 0;
    }
    for (const unsigned char *p = (const unsigned char *)text; *p;) {
        size_t len = 1;
        char stand[4];
        const char *put = (const char *)p;
        size_t put_len = 1;
        if (*p < 0x80) {
            if (*p == ']' && p[1] == '(') {
                /* A Markdown link's address: its words came before it. */
                const unsigned char *end = (const unsigned char *)strchr((const char *)p, ')');
                if (end) {
                    p = end + 1;
                    continue;
                }
            }
            if (strchr("*`#~[]|<>\\", *p)) {
                p++;
                continue;
            }
            if (*p <= ' ' || *p == 0x7F) {
                put = " ";
            }
        } else {
            uint32_t cp = decode(p, &len);
            put_len = len;
            if (!is_letter(cp)) {
                int n = muse_text_ascii((const char *)p, &len, stand);
                if (n >= 0) {
                    put = stand;
                    put_len = (size_t)n;
                }
            }
        }
        p += len;
        if (put_len == 1 && *put == ' ') {
            if (space) {
                continue;
            }
            space = true;
        } else if (put_len) {
            space = false;
        }
        if (o + put_len >= cap) {
            break;
        }
        memcpy(out + o, put, put_len);
        o += put_len;
    }
    while (o && out[o - 1] == ' ') {
        o--;
    }
    out[o] = '\0';
    return o;
}

/* Bytes of `c` (one character, `len` bytes) once percent-encoded. */
static size_t encoded_len(const unsigned char *c, size_t len)
{
    if (len == 1 && ((*c >= '0' && *c <= '9') || (*c >= 'A' && *c <= 'Z') || (*c >= 'a' && *c <= 'z') ||
                     strchr("-_.~", *c))) {
        return 1;
    }
    return 3 * len;
}

bool muse_tts_next_chunk(const char **text, char *out, size_t cap)
{
    const unsigned char *p = (const unsigned char *)*text;
    while (*p == ' ') {
        p++;
    }
    const unsigned char *end = p, *sentence = NULL, *word = NULL;
    int chars = 0;
    size_t encoded = 0;
    while (*end) {
        size_t len;
        uint32_t cp = decode(end, &len);
        size_t enc = encoded_len(end, len);
        if (chars == MUSE_TTS_CHUNK_CHARS || encoded + enc > MUSE_TTS_CHUNK_ENCODED || (size_t)(end - p) + len >= cap) {
            break;
        }
        end += len;
        chars++;
        encoded += enc;
        if (cp == ' ') {
            word = end;
        } else if (strchr(".!?;:", (int)(cp < 0x80 ? cp : 'x')) && (*end == ' ' || !*end) && chars >= CHUNK_MIN_CHARS) {
            sentence = end;
        }
    }
    if (*end) {
        end = sentence ? sentence : word ? word : end;   /* a word longer than a piece is split */
    }
    size_t n = (size_t)(end - p);
    *text = (const char *)end;
    while (n && p[n - 1] == ' ') {
        n--;
    }
    if (!n || n >= cap) {
        return false;
    }
    memcpy(out, p, n);
    out[n] = '\0';
    return true;
}

bool muse_tts_is_vietnamese(const char *text)
{
    /* Vowels with one accent are in Latin-1 and French and Portuguese write
     * them too, but between Vietnamese and English they mean Vietnamese. */
    static const char LATIN1[] = "\xC0\xC1\xC2\xC3\xC8\xC9\xCA\xCC\xCD\xD2\xD3\xD4\xD5\xD9\xDA\xDD"
                                 "\xE0\xE1\xE2\xE3\xE8\xE9\xEA\xEC\xED\xF2\xF3\xF4\xF5\xF9\xFA\xFD";
    for (const unsigned char *p = (const unsigned char *)text; *p;) {
        size_t len;
        uint32_t cp = decode(p, &len);
        p += len;
        if ((cp >= 0x1EA0 && cp <= 0x1EF9) || cp == 0x102 || cp == 0x103 || cp == 0x110 || cp == 0x111 ||
            cp == 0x128 || cp == 0x129 || cp == 0x168 || cp == 0x169 || cp == 0x1A0 || cp == 0x1A1 || cp == 0x1AF ||
            cp == 0x1B0 || (cp >= 0xC0 && cp <= 0xFF && memchr(LATIN1, (int)cp, sizeof(LATIN1) - 1))) {
            return true;
        }
    }
    return false;
}

size_t muse_tts_url(const char *chunk, const char *lang, char *out, size_t cap)
{
    int o = snprintf(out, cap, TTS_URL "%s&q=", lang);
    if (o < 0 || (size_t)o >= cap) {
        return 0;
    }
    for (const unsigned char *p = (const unsigned char *)chunk; *p; p++) {
        size_t n = encoded_len(p, 1);
        if ((size_t)o + n >= cap) {
            return 0;
        }
        if (n == 1) {
            out[o++] = (char)*p;
        } else {
            o += snprintf(out + o, cap - (size_t)o, "%%%02X", *p);
        }
    }
    out[o] = '\0';
    return (size_t)o;
}
