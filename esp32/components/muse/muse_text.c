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

#include "muse_text.h"

#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>

#if CONFIG_MUSE_EMOJI
#include "fonts/muse_emoji.h"
#endif

/* Punctuation and symbols, and the letters that take two, by code point. */
static const struct {
    uint16_t cp;
    char ascii[4];
} STAND_INS[] = {
    { 0x00A0, " " },     /* no-break space */
    { 0x00A1, "!" },
    { 0x00A2, "c" },     /* cent */
    { 0x00A3, "GBP" },
    { 0x00A5, "Y" },     /* yen */
    { 0x00A6, "|" },
    { 0x00A7, "S" },     /* section */
    { 0x00A9, "(c)" },
    { 0x00AB, "<<" },
    { 0x00AD, "" },      /* soft hyphen */
    { 0x00AE, "(R)" },
    { 0x00B0, "o" },     /* degree */
    { 0x00B1, "+-" },
    { 0x00B2, "2" },
    { 0x00B3, "3" },
    { 0x00B4, "'" },
    { 0x00B5, "u" },     /* micro */
    { 0x00B7, "." },     /* middle dot */
    { 0x00B9, "1" },
    { 0x00BB, ">>" },
    { 0x00BC, "1/4" },
    { 0x00BD, "1/2" },
    { 0x00BE, "3/4" },
    { 0x00BF, "?" },
    { 0x00C6, "AE" },
    { 0x00DE, "Th" },
    { 0x00DF, "ss" },
    { 0x00E6, "ae" },
    { 0x00FE, "th" },
    { 0x0132, "IJ" },
    { 0x0133, "ij" },
    { 0x0149, "'n" },
    { 0x0152, "OE" },
    { 0x0153, "oe" },
    { 0x01A0, "O" },     /* Vietnamese O and U with horn */
    { 0x01A1, "o" },
    { 0x01AF, "U" },
    { 0x01B0, "u" },
    { 0x02BC, "'" },     /* modifier apostrophe */
    { 0x02C6, "^" },
    { 0x02DC, "~" },
    { 0x115F, "" },      /* Hangul fillers, blank but boxes in the CJK font */
    { 0x1160, "" },
    { 0x2010, "-" },     /* hyphen */
    { 0x2011, "-" },     /* non-breaking hyphen */
    { 0x2012, "-" },     /* figure dash */
    { 0x2013, "-" },     /* en dash */
    { 0x2014, "--" },    /* em dash */
    { 0x2015, "--" },    /* horizontal bar */
    { 0x2016, "||" },
    { 0x2018, "'" },     /* single quotes */
    { 0x2019, "'" },
    { 0x201A, "," },
    { 0x201B, "'" },
    { 0x201C, "\"" },    /* double quotes */
    { 0x201D, "\"" },
    { 0x201E, "\"" },
    { 0x201F, "\"" },
    { 0x2020, "+" },     /* dagger */
    { 0x2022, "*" },     /* bullet */
    { 0x2023, ">" },
    { 0x2024, "." },
    { 0x2025, ".." },
    { 0x2026, "..." },   /* ellipsis */
    { 0x2027, "-" },
    { 0x2028, " " },     /* line and paragraph separators */
    { 0x2029, " " },
    { 0x202F, " " },     /* narrow no-break space */
    { 0x2032, "'" },     /* prime */
    { 0x2033, "\"" },
    { 0x2039, "<" },
    { 0x203A, ">" },
    { 0x203C, "!!" },
    { 0x2044, "/" },
    { 0x2047, "??" },
    { 0x2048, "?!" },
    { 0x2049, "!?" },
    { 0x205F, " " },
    { 0x2060, "" },      /* word joiner */
    { 0x20AC, "EUR" },
    { 0x2122, "TM" },
    { 0x2190, "<-" },    /* arrows */
    { 0x2191, "^" },
    { 0x2192, "->" },
    { 0x2193, "v" },
    { 0x2194, "<->" },
    { 0x21D0, "<=" },
    { 0x21D2, "=>" },
    { 0x21D4, "<=>" },
    { 0x2212, "-" },     /* minus */
    { 0x2215, "/" },
    { 0x2217, "*" },
    { 0x2248, "~" },
    { 0x2260, "!=" },
    { 0x2264, "<=" },
    { 0x2265, ">=" },
    { 0x2713, "v" },     /* check marks and crosses */
    { 0x2714, "v" },
    { 0x2717, "x" },
    { 0x2718, "x" },
    { 0x3164, "" },      /* Hangul filler */
    { 0xFEFF, "" },      /* byte order mark */
    { 0xFFA0, "" },      /* halfwidth Hangul filler */
};

/* U+00C0-017F by their plain letter; '_' where STAND_INS has two. */
static const char LATIN[] =
    "AAAAAA_CEEEEIIIIDNOOOOOxOUUUUY__aaaaaa_ceeeeiiiidnooooo/ouuuuy_y"
    "AaAaAaCcCcCcCcDdDdEeEeEeEeEeGgGgGgGgHhHhIiIiIiIiIi__JjKkkLlLlLlLlLl"
    "NnNnNn_NnOoOoOo__RrRrRrSsSsSsSsTtTtTtUuUuUuUuUuUuWwYyYZzZzZzs";

/* U+1EA0-1EF9, Vietnamese letters with a tone mark (and a hat, breve or
 * horn), by their plain letter. */
static const char VIETNAMESE[] =
    "AaAaAaAaAaAaAaAaAaAaAaAaEeEeEeEeEeEeEeEeIiIiOoOoOoOoOoOoOoOoOoOoOoOo"
    "UuUuUuUuUuUuUuYyYyYyYy";

static bool s_keep_latin;   /* muse_text_keep_latin() */
#if CONFIG_MUSE_EMOJI
static bool s_keep_emoji;   /* muse_text_keep_emoji() */
#endif

/* The code point at s, and its length in *len; -1 if it's broken. */
static int32_t decode(const unsigned char *s, size_t *len)
{
    if (s[0] >= 0x80 && s[0] < 0xC0) {
        *len = 1;   /* a stray continuation byte */
        return -1;
    }
    size_t n = s[0] >= 0xF0 ? 4 : s[0] >= 0xE0 ? 3 : s[0] >= 0xC0 ? 2 : 1;
    int32_t cp = n == 1 ? s[0] : s[0] & (0x7F >> n);
    size_t i = 1;
    for (; i < n && (s[i] & 0xC0) == 0x80; i++) {
        cp = cp << 6 | (s[i] & 0x3F);
    }
    *len = i;
    return i == n ? cp : -1;
}

static const char *stand_in(int32_t cp)
{
    size_t lo = 0, hi = sizeof(STAND_INS) / sizeof(STAND_INS[0]);
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (STAND_INS[mid].cp == cp) {
            return STAND_INS[mid].ascii;
        }
        if (STAND_INS[mid].cp < cp) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    if (cp >= 0x2000 && cp <= 0x200A) {
        return " ";   /* spaces of other widths */
    }
    if ((cp >= 0x0300 && cp <= 0x036F)      /* accents typed after their letter */
        || (cp >= 0x200B && cp <= 0x200F)   /* zero-width spaces and joiners, direction marks */
        || cp == 0x20E3                     /* keycap */
        || (cp >= 0x2600 && cp <= 0x27BF)   /* symbols and dingbats */
        || (cp >= 0x2B00 && cp <= 0x2BFF)   /* more arrows, stars */
        || (cp >= 0xFE00 && cp <= 0xFE0F)   /* emoji or text style */
        || (cp >= 0x1F000 && cp <= 0x1FAFF) /* emoji */
        || (cp >= 0xE0000 && cp <= 0xE007F)) {
        return "";
    }
    return NULL;
}

int muse_text_ascii(const char *s, size_t *len, char out[4])
{
    int32_t cp = decode((const unsigned char *)s, len);
    if (cp < 0x80) {
        return -1;   /* ASCII, or broken */
    }
    if (cp >= 0xC0 && cp <= 0x17F && LATIN[cp - 0xC0] != '_') {
        out[0] = LATIN[cp - 0xC0];
        out[1] = '\0';
        return 1;
    }
    if (cp >= 0x1EA0 && cp <= 0x1EF9) {
        out[0] = VIETNAMESE[cp - 0x1EA0];
        out[1] = '\0';
        return 1;
    }
    const char *a = stand_in(cp);
    if (!a) {
        return -1;
    }
    strlcpy(out, a, 4);
    return (int)strlen(out);
}

muse_text_cjk_t muse_text_cjk(const char *s)
{
    static const uint16_t CLOSE[] = {
        0x3001, 0x3002, 0x3009, 0x300B, 0x300D, 0x300F, 0x3011, 0x3015, 0x3017, 0x3019,
        0x301B, 0x30FB, 0x30FC, 0xFF01, 0xFF09, 0xFF0C, 0xFF0E, 0xFF1A, 0xFF1B, 0xFF1F,
        0xFF3D, 0xFF5D, 0xFF60,
    };
    size_t len;
    int32_t cp = decode((const unsigned char *)s, &len);
    for (size_t i = 0; i < sizeof(CLOSE) / sizeof(CLOSE[0]); i++) {
        if (cp == CLOSE[i]) {
            return MUSE_TEXT_CJK_CLOSE;
        }
    }
    bool cjk = (cp >= 0x1100 && cp <= 0x11FF)      /* Hangul jamo */
               || (cp >= 0x2E80 && cp <= 0x9FFF)   /* radicals, punctuation, kana, ideographs */
               || (cp >= 0xAC00 && cp <= 0xD7AF)   /* Hangul syllables */
               || (cp >= 0xF900 && cp <= 0xFAFF)   /* compatibility ideographs */
               || (cp >= 0xFF00 && cp <= 0xFFEF);  /* fullwidth forms */
    return cjk ? MUSE_TEXT_CJK : MUSE_TEXT_NOT_CJK;
}

bool muse_text_has_cjk(const char *s)
{
    while (*s) {
        if (muse_text_cjk(s) != MUSE_TEXT_NOT_CJK) {
            return true;
        }
        size_t len;
        decode((const unsigned char *)s, &len);
        s += len;
    }
    return false;
}

/* The letters the caption's accented font has (fonts/muse_font_latin_16.c). */
static bool latin_letter(int32_t cp)
{
    return (cp >= 0xC0 && cp <= 0x17F) || (cp >= 0x1A0 && cp <= 0x1B0) || (cp >= 0x1EA0 && cp <= 0x1EF9);
}

void muse_text_keep_latin(void)
{
    s_keep_latin = true;
}

bool muse_text_has_latin(const char *s)
{
    while (s_keep_latin && *s) {
        size_t len;
        if (latin_letter(decode((const unsigned char *)s, &len))) {
            return true;
        }
        s += len;
    }
    return false;
}

#if CONFIG_MUSE_EMOJI
/* The emoji the caption font draws (fonts/muse_font_emoji_16.c). */
static bool emoji(int32_t cp)
{
    static const uint32_t CODES[] = { MUSE_EMOJI_CODE_POINTS };
    size_t lo = 0, hi = sizeof(CODES) / sizeof(CODES[0]);
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (CODES[mid] == (uint32_t)cp) {
            return true;
        }
        if (CODES[mid] < (uint32_t)cp) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return false;
}

void muse_text_keep_emoji(void)
{
    s_keep_emoji = true;
}

bool muse_text_emoji(const char *s, size_t *len)
{
    int32_t cp = decode((const unsigned char *)s, len);
    return s_keep_emoji && cp > 0 && emoji(cp);
}

/* Drops what follows the emoji ending at p as part of it: its style and skin
 * tone, and anything joined to it with U+200D, so a sequence shows as its
 * first emoji. n is the text's length; returns the new one. */
static size_t drop_joined(char *p, size_t n, const char *s)
{
    for (;;) {
        size_t len, next = 0;
        int32_t cp = decode((const unsigned char *)p, &len);
        if (cp == 0x200D && p[len]) {
            decode((const unsigned char *)p + len, &next);   /* and the character it joins */
        } else if (cp != 0xFE0E && cp != 0xFE0F && !(cp >= 0x1F3FB && cp <= 0x1F3FF)) {
            return n;
        }
        size_t cut = len + next;
        memmove(p, p + cut, n - (p - s) - cut + 1);
        n -= cut;
    }
}
#endif

static void fold(char *s, size_t cap, bool keep_latin, bool keep_emoji)
{
    size_t n = strlen(s);
#if CONFIG_MUSE_EMOJI
    keep_emoji = keep_emoji && s_keep_emoji;
#else
    (void)keep_emoji;
#endif
    for (char *p = s; *p;) {
        size_t len;
        char a[4];
#if CONFIG_MUSE_EMOJI
        if (keep_emoji && muse_text_emoji(p, &len)) {
            p += len;
            n = drop_joined(p, n, s);
            continue;
        }
#endif
        int alen = keep_latin && latin_letter(decode((const unsigned char *)p, &len)) ? -1
                                                                                     : muse_text_ascii(p, &len, a);
        if (alen < 0) {
            p += len;
            continue;
        }
        if (n - len + alen >= cap) {
            *p = '\0';
            return;
        }
        memmove(p + alen, p + len, n - (p - s) - len + 1);
        memcpy(p, a, alen);
        n = n - len + alen;
        p += alen;
#if CONFIG_MUSE_EMOJI
        if (keep_emoji && !alen) {
            n = drop_joined(p, n, s);   /* an emoji the font lacks takes its sequence with it */
        }
#endif
    }
}

void muse_text_to_ascii(char *s, size_t cap)
{
    fold(s, cap, false, false);
}

void muse_text_to_caption(char *s, size_t cap)
{
    fold(s, cap, s_keep_latin, true);
}

void muse_text_to_label(char *s, size_t cap)
{
    fold(s, cap, s_keep_latin, false);
}

const char *muse_text_showable(const char *text, char *buf, size_t cap)
{
    const char *p = text;
    while (*p && !(*p & 0x80)) {
        p++;
    }
    if (!*p || strlcpy(buf, text, cap) >= cap) {
        return text;
    }
    muse_text_to_ascii(buf, cap);
    return buf;
}

/* ---- Markdown in replies ---- */

static bool md_word(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c & 0x80);
}

/* A line of only "|", "-", ":" and spaces with at least three dashes: a table
 * divider or a "---" rule. */
static bool md_rule(const char *p, const char *eol)
{
    int dashes = 0;
    for (; p < eol; p++) {
        if (*p == '-') {
            dashes++;
        } else if (*p != '|' && *p != ':' && *p != ' ') {
            return false;
        }
    }
    return dashes >= 3;
}

/* Where the * or _ at p, opening emphasis, closes on its line; NULL if it
 * doesn't. It opens after a non-word character and before a non-space, and
 * closes after a non-space and before a non-word character, so "2*3*4" and
 * "snake_case_name" aren't emphasis. */
static const char *md_closing(const char *line, const char *p, const char *eol)
{
    char mark = *p;
    if ((p > line && md_word(p[-1])) || p + 1 >= eol || p[1] == ' ' || p[1] == mark) {
        return NULL;
    }
    for (const char *q = p + 2; q < eol; q++) {
        if (*q != mark || q[-1] == ' ') {
            continue;
        }
        const char *last = q;   /* in "***" the last one closes, after "**" */
        while (last + 1 < eol && last[1] == mark) {
            last++;
        }
        if ((last - q) % 2 == 0 && (last + 1 >= eol || !md_word(last[1]))) {
            return last;
        }
        q = last;   /* "**" closes bold inside the emphasis */
    }
    return NULL;
}

/* The [text](url) link at p: sets *text, *len and returns the end of the link,
 * or NULL if p doesn't start one. */
static const char *md_link(const char *p, const char *eol, const char **text, size_t *len)
{
    const char *close = memchr(p, ']', eol - p);
    if (!close || close + 1 >= eol || close[1] != '(') {
        return NULL;
    }
    const char *end = memchr(close + 2, ')', eol - (close + 2));
    if (!end) {
        return NULL;
    }
    *text = p + 1;
    *len = close - (p + 1);
    return end + 1;
}

size_t muse_text_strip_markdown(char *s, uint64_t wrapped)
{
    char *w = s;   /* writes never pass reads: every rule keeps or shortens */
    const char *r = s;
    for (int i = 0; *r; i++) {
        const char *eol = strchr(r, '\n');
        if (!eol) {
            eol = r + strlen(r);
        }
        const char *next = *eol ? eol + 1 : eol;
        const char *p = r;
        while (p < eol && *p == ' ') {
            p++;
        }
        bool starts = i >= 64 || !(wrapped >> i & 1);   /* only there are line markers */
        if (starts && (md_rule(p, eol) || (eol - p >= 3 && !strncmp(p, "```", 3)))) {
            r = next;   /* the line goes */
            continue;
        }
        memmove(w, r, p - r);   /* the indent */
        w += p - r;
        if (starts && *p == '>') {
            p += p + 1 < eol && p[1] == ' ' ? 2 : 1;
        }
        int hashes = 0;
        while (p + hashes < eol && p[hashes] == '#') {
            hashes++;
        }
        if (starts && hashes && hashes <= 6 && (p + hashes == eol || p[hashes] == ' ')) {
            p += hashes;
            while (p < eol && *p == ' ') {
                p++;
            }
        } else if (starts && (*p == '*' || *p == '+') && p + 1 < eol && p[1] == ' ') {
            *w++ = '-';
            p++;
        }
        char *line = w;
        const char *closing = NULL;   /* the * or _ that ends the emphasis opened before it */
        if (p < eol && *p == '|') {
            p++;
            while (p < eol && *p == ' ') {
                p++;
            }
        }
        while (p < eol) {
            const char *text;
            size_t len;
            const char *end;
            if (*p == '\\' && p + 1 < eol && strchr("\\`*_~#[]()|>-+", p[1])) {
                *w++ = p[1];   /* an escaped marker is just the character */
                p += 2;
            } else if ((*p == '*' || *p == '_' || *p == '~') && p + 1 < eol && p[1] == *p) {
                p += 2;   /* **bold**, __bold__, ~~struck~~ */
            } else if (*p == '`') {
                const char *close = memchr(p + 1, '`', eol - (p + 1));
                if (close) {   /* code keeps its text as it is */
                    memmove(w, p + 1, close - (p + 1));
                    w += close - (p + 1);
                    p = close;
                }
                p++;
            } else if (p == closing) {
                closing = NULL;
                p++;
            } else if ((*p == '*' || *p == '_') && !closing && (closing = md_closing(r, p, eol))) {
                p++;
            } else if (*p == '[' && (end = md_link(p, eol, &text, &len))) {
                memmove(w, text, len);
                w += len;
                p = end;
            } else if (*p == '|') {
                bool spaced = w > line && w[-1] == ' ';
                while (w > line && w[-1] == ' ') {
                    w--;
                }
                p++;
                while (p < eol && *p == ' ') {
                    p++;
                    spaced = true;
                }
                if (p < eol && w > line) {   /* a bar between cells; one at either end just goes */
                    *w++ = ',';
                    if (spaced) {
                        *w++ = ' ';
                    }
                }
            } else {
                *w++ = *p++;
            }
        }
        while (w > line && w[-1] == ' ') {   /* left by a bar or marker at the end */
            w--;
        }
        if (*eol) {
            *w++ = '\n';
        }
        r = next;
    }
    *w = '\0';
    return w - s;
}

static bool word_is(const char *w, size_t n, const char *name)
{
    return strlen(name) == n && strncasecmp(w, name, n) == 0;
}

/* A UTF-8 letter beyond ASCII: Latin-1 and Latin Extended-A, and Latin
 * Extended Additional (Vietnamese). */
static bool accented(const unsigned char *p)
{
    return (p[0] >= 0xC3 && p[0] <= 0xC7) || (p[0] == 0xE1 && (p[1] == 0xBA || p[1] == 0xBB));
}

void muse_text_sentence_case(char *s)
{
    if (!s) {
        return;
    }
    for (const char *p = s; *p; p++) {
        if (*p >= 'a' && *p <= 'z') {
            return;   /* already mixed case: someone chose it */
        }
        if (accented((const unsigned char *)p)) {
            return;   /* a translation's own letters (muse_lang.c): as it is */
        }
    }
    static const char *const KEEP_UPPER[] = { "BLE", "USB", "SDK", "VM", "OK", "I", "LC3", "SBC", "FW", "ID" };
    bool start = true;
    char *p = s;
    while (*p) {
        if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z'))) {
            if ((*p == '.' || *p == '!' || *p == '?') && p[1] == ' ') {
                start = true;
            }
            p++;
            continue;
        }
        char *w = p;
        while ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || *p == '-' ||
               (*p == '\'' && ((p[1] >= 'A' && p[1] <= 'Z') || (p[1] >= 'a' && p[1] <= 'z')))) {
            p++;
        }
        size_t n = (size_t)(p - w);
        bool upper = false;
        for (size_t i = 0; i < sizeof(KEEP_UPPER) / sizeof(KEEP_UPPER[0]); i++) {
            upper = upper || word_is(w, n, KEEP_UPPER[i]);
        }
        if (upper) {
            /* as it is */
        } else if (word_is(w, n, "MUSE")) {
            memcpy(w, "Muse", 4);
        } else if (word_is(w, n, "WI-FI")) {
            memcpy(w, "Wi-Fi", 5);
        } else {
            for (size_t i = start ? 1 : 0; i < n; i++) {
                if (w[i] >= 'A' && w[i] <= 'Z') w[i] = (char)(w[i] - 'A' + 'a');
            }
        }
        start = false;
    }
}
