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

/* Shared by both chat backends: the voice note's encoding, and reply and
 * transcript text cut to fit the caption. Also the serial console's "@chat"
 * lines for typed turns. */
#include "muse_chat_priv.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "muse_state.h"
#include "muse_text.h"

#define CAPTION_CHARS 32   /* what fits across the round screen */
#define CONSOLE_LINE 400   /* one "@chat" line */
#define CONSOLE_TEXT 240   /* escaped text per line */

static void put_le(uint8_t *p, uint32_t v, int n)
{
    for (int i = 0; i < n; i++) {
        p[i] = (uint8_t)(v >> (8 * i));
    }
}

void muse_hatch_wav_header(uint8_t h[MUSE_HATCH_WAV_HEADER], uint32_t rate)
{
    memcpy(h, "RIFF", 4);
    put_le(h + 4, UINT32_MAX, 4);
    memcpy(h + 8, "WAVEfmt ", 8);
    put_le(h + 16, 16, 4);
    put_le(h + 20, 1, 2);             /* PCM */
    put_le(h + 22, 1, 2);             /* mono */
    put_le(h + 24, rate, 4);
    put_le(h + 28, rate * 2, 4);
    put_le(h + 32, 2, 2);
    put_le(h + 34, 16, 2);
    memcpy(h + 36, "data", 4);
    put_le(h + 40, UINT32_MAX, 4);
}

size_t muse_hatch_base64(const uint8_t *in, size_t n, char *out)
{
    static const char A[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char *o = out;
    size_t i = 0;
    for (; i + 2 < n; i += 3) {
        uint32_t v = in[i] << 16 | in[i + 1] << 8 | in[i + 2];
        *o++ = A[v >> 18];
        *o++ = A[v >> 12 & 63];
        *o++ = A[v >> 6 & 63];
        *o++ = A[v & 63];
    }
    if (i < n) {
        uint32_t v = in[i] << 16 | (i + 1 < n ? in[i + 1] << 8 : 0);
        *o++ = A[v >> 18];
        *o++ = A[v >> 12 & 63];
        *o++ = i + 1 < n ? A[v >> 6 & 63] : '=';
        *o++ = '=';
    }
    return o - out;
}

/* Last CAPTION_CHARS characters of `src`, starting on a word boundary when possible. */
void muse_hatch_tail_words(const char *src, char *out, size_t cap)
{
    size_t len = strlen(src);
    const char *p = src;
    if (len > CAPTION_CHARS) {
        p = src + len - CAPTION_CHARS;
        const char *sp = strchr(p, ' ');
        if (sp && sp[1] && sp - p < 12) {
            p = sp + 1;
        }
        while ((*p & 0xC0) == 0x80) {
            p++;
        }
    }
    while (*p == ' ') {
        p++;
    }
    strlcpy(out, p, cap);
}

/*
 * The next line of `text` wrapped to `cols` characters as the caption shows
 * them (an ellipsis as three dots, muse_text.h), splitting only words longer
 * than a line. CJK has no spaces: it breaks between characters, but never
 * before closing punctuation.
 */
static bool next_line(const char **text, int cols, const char **start, size_t *len)
{
    const char *p = *text;
    while (*p == ' ' || *p == '\n') {
        p++;
    }
    const char *end = p, *brk = NULL, *movable = NULL;
    int n = 0;
    muse_text_cjk_t prev = MUSE_TEXT_NOT_CJK;
    while (*end && *end != '\n') {
        size_t bytes;
        char shown[4];
        int w = muse_text_ascii(end, &bytes, shown);
        w = w < 0 ? 1 : w;
        muse_text_cjk_t cjk = muse_text_cjk(end);
        bool cjk_break = n && cjk != MUSE_TEXT_CJK_CLOSE && (cjk || prev);
        if (n + w > cols && n) {
            if (cjk_break) {
                brk = end;
            } else if (cjk == MUSE_TEXT_CJK_CLOSE && !brk) {
                brk = movable;   /* a full word: its last character goes with the punctuation */
            }
            break;
        }
        if (n && cjk != MUSE_TEXT_CJK_CLOSE) {
            movable = end;
        }
        if (*end == ' ' || cjk_break) {
            brk = end;
        }
        n += w;
        end += bytes;
        prev = cjk;
    }
    if (*end && *end != ' ' && *end != '\n' && brk) {
        end = brk;   /* don't split a word */
    }
    *start = p;
    *len = end - p;
    *text = end;
    return end != p;
}

/*
 * Wraps `text` to the screen's page (muse_state_page) and puts the page holding
 * byte `at` in `out`. Pages overlap by a line: a page's last line starts the
 * next one, so the page turns as that line is reached and nothing is skipped.
 */
bool muse_hatch_caption_at(const char *text, size_t at, char *out, size_t cap)
{
    int cols, lines;
    muse_state_page(muse_text_has_cjk(text), &cols, &lines);
    const char *p = text, *start;
    size_t len;
    int line = -1, n = 0;
    while (next_line(&p, cols, &start, &len)) {
        line = n++;
        if ((size_t)(start + len - text) > at) {
            break;
        }
    }
    if (line < 0) {
        return false;
    }
    int first = lines > 1 ? line / (lines - 1) * (lines - 1) : line;

    size_t o = 0;
    out[0] = '\0';
    p = text;
    for (n = 0; n < first + lines && next_line(&p, cols, &start, &len); n++) {
        if (n < first) {
            continue;
        }
        if (o + 1 + len >= cap) {
            break;   /* whole lines only */
        }
        o += snprintf(out + o, cap - o, "%s%.*s", o ? "\n" : "", (int)len, start);
    }
    return true;
}

/* ---- Typed turns on the serial console ---- */

/* JSON-escapes whole characters of *src into out, up to cap - 1 bytes; moves *src past them. */
static size_t escape_some(const char **src, char *out, size_t cap)
{
    const unsigned char *p = (const unsigned char *)*src;
    size_t o = 0;
    while (*p) {
        char esc[8];
        const char *put = esc;
        size_t n = 1, put_len = 2;
        esc[0] = '\\';
        if (*p == '"' || *p == '\\') {
            esc[1] = (char)*p;
        } else if (*p == '\n') {
            esc[1] = 'n';
        } else if (*p == '\r') {
            esc[1] = 'r';
        } else if (*p == '\t') {
            esc[1] = 't';
        } else if (*p < 0x20) {
            put_len = snprintf(esc, sizeof(esc), "\\u%04x", *p);
        } else {
            while (n < 4 && (p[n] & 0xC0) == 0x80) {
                n++;   /* the rest of a UTF-8 character */
            }
            put = (const char *)p;
            put_len = n;
        }
        if (o + put_len >= cap) {
            break;
        }
        memcpy(out + o, put, put_len);
        o += put_len;
        p += n;
    }
    out[o] = '\0';
    *src = (const char *)p;
    return o;
}

/* Each line goes out in one write, so other tasks' log lines land between lines, not inside them. */
void muse_hatch_console(const char *type, const char *text, const char *fields, ...)
{
    static unsigned seq;
    char line[CONSOLE_LINE];
    do {
        size_t n = snprintf(line, sizeof(line), "@chat {\"seq\":%u,\"type\":\"%s\"",
                            __atomic_add_fetch(&seq, 1, __ATOMIC_RELAXED), type);
        if (fields) {
            va_list ap;
            va_start(ap, fields);
            line[n++] = ',';
            n += vsnprintf(line + n, sizeof(line) - n, fields, ap);
            va_end(ap);
        }
        if (n > sizeof(line) - CONSOLE_TEXT - 16) {
            n = sizeof(line) - CONSOLE_TEXT - 16;   /* never: fields are short */
        }
        if (text) {
            const char *was = text;
            n += snprintf(line + n, sizeof(line) - n, ",\"text\":\"");
            n += escape_some(&text, line + n, CONSOLE_TEXT);
            line[n++] = '"';
            if (text == was) {
                text = NULL;
            }
        }
        line[n++] = '}';
        line[n++] = '\n';
        fwrite(line, 1, n, stdout);
        fflush(stdout);
    } while (text && *text);
}

size_t muse_hatch_unescape(char *s)
{
    char *o = s;
    for (const char *p = s; *p; p++) {
        if (*p == '\\' && p[1]) {
            p++;
            *o++ = *p == 'n' ? '\n' : *p == 'r' ? '\r' : *p == 't' ? '\t' : *p;
        } else {
            *o++ = *p;
        }
    }
    *o = '\0';
    return o - s;
}
