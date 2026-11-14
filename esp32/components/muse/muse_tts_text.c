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
#define CJK_CHUNK_MIN_CHARS 20

typedef enum {
    SCRIPT_NONE = 0,
    SCRIPT_LATIN,
    SCRIPT_HAN,
    SCRIPT_JA,
    SCRIPT_KO,
} script_t;

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

static bool is_kana(uint32_t cp)
{
    return (cp >= 0x3040 && cp <= 0x309F) || (cp >= 0x30A0 && cp <= 0x30FF) ||
           (cp >= 0x31F0 && cp <= 0x31FF) || (cp >= 0xFF66 && cp <= 0xFF9F);
}

static bool is_hangul(uint32_t cp)
{
    return (cp >= 0xAC00 && cp <= 0xD7AF) || (cp >= 0x1100 && cp <= 0x11FF) || (cp >= 0x3130 && cp <= 0x318F);
}

static bool is_han(uint32_t cp)
{
    return (cp >= 0x3400 && cp <= 0x4DBF) || (cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0xF900 && cp <= 0xFAFF);
}

static script_t classify_cp(uint32_t cp)
{
    if (is_kana(cp)) {
        return SCRIPT_JA;
    }
    if (is_hangul(cp)) {
        return SCRIPT_KO;
    }
    if (is_han(cp)) {
        return SCRIPT_HAN;
    }
    if ((cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z') || is_letter(cp)) {
        return SCRIPT_LATIN;
    }
    return SCRIPT_NONE;
}

/* Letters that appear in Vietnamese but never in Pinyin romanization. */
static bool is_vi_exclusive(uint32_t cp)
{
    return (cp >= 0x1EA0 && cp <= 0x1EF9) || cp == 0x102 || cp == 0x103 || cp == 0x110 || cp == 0x111 ||
           cp == 0x128 || cp == 0x129 || cp == 0x168 || cp == 0x169 || cp == 0x1A0 || cp == 0x1A1 ||
           cp == 0x1AF || cp == 0x1B0 || cp == 0x00C2 || cp == 0x00E2 || cp == 0x00C3 || cp == 0x00E3 ||
           cp == 0x00CA || cp == 0x00EA || cp == 0x00D4 || cp == 0x00F4 || cp == 0x00D5 || cp == 0x00F5 ||
           cp == 0x00DD || cp == 0x00FD;
}

/* Tone-marked vowels used in Pinyin (macrons, carons, u-diaeresis, acutes/graves). */
static bool is_pinyin_tone(uint32_t cp)
{
    return (cp >= 0x01CD && cp <= 0x01DC) || cp == 0x0100 || cp == 0x0101 || cp == 0x0112 || cp == 0x0113 ||
           cp == 0x011A || cp == 0x011B || cp == 0x012A || cp == 0x012B || cp == 0x014C || cp == 0x014D ||
           cp == 0x016A || cp == 0x016B || cp == 0x00DC || cp == 0x00FC || cp == 0x00C0 || cp == 0x00C1 ||
           cp == 0x00C8 || cp == 0x00C9 || cp == 0x00CC || cp == 0x00CD || cp == 0x00D2 || cp == 0x00D3 ||
           cp == 0x00D9 || cp == 0x00DA || cp == 0x00E0 || cp == 0x00E1 || cp == 0x00E8 || cp == 0x00E9 ||
           cp == 0x00EC || cp == 0x00ED || cp == 0x00F2 || cp == 0x00F3 || cp == 0x00F9 || cp == 0x00FA ||
           cp == 0x0300 || cp == 0x0301 || cp == 0x0304 || cp == 0x030C;
}

/* If p points at '(' or fullwidth '（' holding Pinyin right after Hanzi (such
 * as "你好 (nǐ hǎo)"), returns the byte pointer just past the closing bracket
 * so TTS doesn't read the Pinyin a second time in another language. */
static const unsigned char *skip_pinyin_parens(const unsigned char *p)
{
    size_t open_len;
    uint32_t open_cp = decode(p, &open_len);
    if (open_cp != '(' && open_cp != 0xFF08) {
        return NULL;
    }
    const unsigned char *q = p + open_len;
    bool has_tone = false;
    size_t scanned = 0;
    while (*q && *q != '\n' && scanned < 80) {
        size_t len;
        uint32_t cp = decode(q, &len);
        q += len;
        scanned += len;
        if (cp == ')' || cp == 0xFF09) {
            return has_tone ? q : NULL;
        }
        if (is_vi_exclusive(cp) || is_han(cp) || is_kana(cp) || is_hangul(cp) ||
            cp == ':' || cp == '/' || cp == '-' || cp == ';') {
            return NULL;
        }
        if (is_pinyin_tone(cp)) {
            has_tone = true;
        }
    }
    return NULL;
}

size_t muse_tts_clean(const char *text, char *out, size_t cap)
{
    size_t o = 0;
    bool space = true;   /* no space at the start, none doubled */
    bool last_han = false;
    if (!cap) {
        return 0;
    }
    for (const unsigned char *p = (const unsigned char *)text; *p;) {
        if (last_han) {
            const unsigned char *after = skip_pinyin_parens(p);
            if (after) {
                p = after;
                continue;
            }
        }
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
            } else {
                last_han = false;
            }
        } else {
            uint32_t cp = decode(p, &len);
            put_len = len;
            if (is_han(cp)) {
                last_han = true;
            } else if (cp != 0x3000) {
                last_han = false;
            }
            if (!is_letter(cp) && !is_han(cp) && !is_kana(cp) && !is_hangul(cp)) {
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

muse_tts_announce_t muse_tts_announce_check(const char *text, char *scratch, size_t cap)
{
    if (!text || !scratch || cap < 2) {
        return MUSE_TTS_ANNOUNCE_EMPTY;
    }
    size_t n = muse_tts_clean(text, scratch, cap);
    if (!n) {
        return MUSE_TTS_ANNOUNCE_EMPTY;
    }
    return n > MUSE_TTS_ANNOUNCE_MAX ? MUSE_TTS_ANNOUNCE_TOO_LONG : MUSE_TTS_ANNOUNCE_OK;
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

static bool is_cjk_sentence_end(uint32_t cp)
{
    return cp == 0x3002 || cp == 0xFF01 || cp == 0xFF1F || cp == 0xFF1B || cp == 0xFF1A;
}

static bool is_cjk_pause(uint32_t cp)
{
    return cp == 0xFF0C || cp == 0x3001;
}

static bool is_speakable(uint32_t cp)
{
    return (cp >= '0' && cp <= '9') || classify_cp(cp) != SCRIPT_NONE;
}

/* True if `next` belongs to a different spoken language family than `cur`.
 * Japanese mixes Kanji (SCRIPT_HAN) and Kana (SCRIPT_JA) in the same words,
 * so those two merge into SCRIPT_JA rather than splitting. */
static bool script_conflict(script_t cur, script_t next)
{
    if (cur == SCRIPT_NONE || next == SCRIPT_NONE || cur == next) {
        return false;
    }
    if ((cur == SCRIPT_HAN && next == SCRIPT_JA) || (cur == SCRIPT_JA && next == SCRIPT_HAN)) {
        return false;
    }
    return true;
}

bool muse_tts_next_chunk_lang(const char **text, const char *default_lang, char *out, size_t cap,
                              char *lang_out, size_t lang_cap)
{
    const unsigned char *p = (const unsigned char *)*text;
    while (*p) {
        size_t len;
        uint32_t cp = decode(p, &len);
        if (is_speakable(cp)) {
            break;
        }
        p += len;
    }
    if (!*p) {
        *text = (const char *)p;
        return false;
    }
    const unsigned char *end = p, *sentence = NULL, *word = NULL;
    int chars = 0;
    size_t encoded = 0;
    script_t chunk_script = SCRIPT_NONE;
    bool script_split = false;
    while (*end) {
        size_t len;
        uint32_t cp = decode(end, &len);
        script_t sc = classify_cp(cp);
        if (script_conflict(chunk_script, sc)) {
            script_split = true;
            break;
        }
        size_t enc = encoded_len(end, len);
        if (chars == MUSE_TTS_CHUNK_CHARS || encoded + enc > MUSE_TTS_CHUNK_ENCODED || (size_t)(end - p) + len >= cap) {
            break;
        }
        if (sc != SCRIPT_NONE) {
            if (chunk_script == SCRIPT_NONE) {
                chunk_script = sc;
            } else if (chunk_script == SCRIPT_HAN && sc == SCRIPT_JA) {
                chunk_script = SCRIPT_JA;
            }
        }
        end += len;
        chars++;
        encoded += enc;
        if (cp == ' ' || is_cjk_pause(cp)) {
            word = end;
        } else if (is_cjk_sentence_end(cp) && chars >= CJK_CHUNK_MIN_CHARS) {
            sentence = end;
        } else if (strchr(".!?;:", (int)(cp < 0x80 ? cp : 'x')) && (*end == ' ' || !*end) &&
                   chars >= (chunk_script == SCRIPT_LATIN || chunk_script == SCRIPT_NONE ? CHUNK_MIN_CHARS : CJK_CHUNK_MIN_CHARS)) {
            sentence = end;
        } else if (sc == SCRIPT_HAN || sc == SCRIPT_JA || sc == SCRIPT_KO) {
            word = end;   /* CJK has no spaces between words: any character boundary can wrap */
        }
    }
    if (*end && !script_split) {
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
    if (lang_out && lang_cap) {
        const char *chosen = default_lang && default_lang[0] ? default_lang : "en";
        if (chunk_script == SCRIPT_HAN) {
            chosen = "zh-CN";
        } else if (chunk_script == SCRIPT_JA) {
            chosen = "ja";
        } else if (chunk_script == SCRIPT_KO) {
            chosen = "ko";
        } else if (muse_tts_is_vietnamese(out)) {
            chosen = "vi";
        }
        snprintf(lang_out, lang_cap, "%s", chosen);
    }
    return true;
}

bool muse_tts_next_chunk(const char **text, char *out, size_t cap)
{
    return muse_tts_next_chunk_lang(text, "en", out, cap, NULL, 0);
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

const char *muse_tts_detect_lang(const char *text, const char *fallback)
{
    bool has_han = false, has_ja = false, has_ko = false;
    for (const unsigned char *p = (const unsigned char *)text; *p;) {
        size_t len;
        uint32_t cp = decode(p, &len);
        p += len;
        if (is_kana(cp)) {
            has_ja = true;
        } else if (is_hangul(cp)) {
            has_ko = true;
        } else if (is_han(cp)) {
            has_han = true;
        }
    }
    if (has_ja) {
        return "ja";
    }
    if (has_ko) {
        return "ko";
    }
    if (has_han) {
        return "zh-CN";
    }
    if (muse_tts_is_vietnamese(text)) {
        return "vi";
    }
    return fallback && fallback[0] ? fallback : "en";
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
