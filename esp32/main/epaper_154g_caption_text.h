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

#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "fonts/epaper_154g_caption_font.h"

enum { EPAPER_154G_CAPTION_WIDTH = 8, EPAPER_154G_CAPTION_HEIGHT = 16,
       EPAPER_154G_CAPTION_GAP = 1, EPAPER_154G_CAPTION_LINES = 7,
       EPAPER_154G_CAPTION_COLS = 24, EPAPER_154G_CAPTION_BYTES = 1024,
       EPAPER_154G_CAPTION_PAGE_BYTES = 384 };

typedef struct {
    char snapshot[EPAPER_154G_CAPTION_BYTES];
    char formatted[EPAPER_154G_CAPTION_BYTES];
    char page[EPAPER_154G_CAPTION_PAGE_BYTES];
} epaper_154g_caption_buffers_t;

// Invalid UTF-8 consumes one byte. Never inspect a byte beyond the terminating NUL.
static inline uint32_t epaper_154g_caption_decode(const char **cursor) {
    const unsigned char *p = (const unsigned char *)*cursor;
    if (!*p) return 0;
    uint32_t code = *p;
    unsigned count = 1;
    if (code >= 0xc2 && code <= 0xdf) { count = 2; code &= 0x1f; }
    else if (code >= 0xe0 && code <= 0xef) { count = 3; code &= 0x0f; }
    else if (code >= 0xf0 && code <= 0xf4) { count = 4; code &= 7; }
    else if (code >= 0x80) { (*cursor)++; return '?'; }
    for (unsigned i = 1; i < count; i++) {
        if (!p[i] || (p[i] & 0xc0) != 0x80) { (*cursor)++; return '?'; }
        code = code << 6 | (p[i] & 0x3f);
    }
    if ((count == 3 && code < 0x800) || (count == 4 && code < 0x10000)
        || (code >= 0xd800 && code <= 0xdfff) || code > 0x10ffff) {
        (*cursor)++; return '?';
    }
    *cursor += count;
    return code;
}

static inline bool epaper_154g_caption_normalize(const char *src, char *out, size_t cap) {
    const char *p = src ? src : "";
    size_t used = 0;
    if (!cap) return *p != 0;
    while (*p) {
        uint32_t code = epaper_154g_caption_decode(&p);
        if (code <= ' ' || code == 0xa0) {
            if (!used || out[used - 1] == ' ') continue;
            code = ' ';
        } else if (code == 0x2018 || code == 0x2019) code = '\'';
        else if (code == 0x201c || code == 0x201d) code = '"';
        else if (code == 0x2013 || code == 0x2014 || code == 0xad) code = '-';
        else if (!epaper_154g_caption_glyph(code)) code = '?';
        size_t bytes = code < 0x80 ? 1 : code < 0x800 ? 2 : 3;
        if (used + bytes >= cap) { out[used] = 0; return true; }
        if (bytes == 1) {
            out[used++] = (char)code;
        } else if (bytes == 2) {
            out[used++] = (char)(0xc0 | code >> 6);
            out[used++] = (char)(0x80 | (code & 0x3f));
        } else {
            out[used++] = (char)(0xe0 | code >> 12);
            out[used++] = (char)(0x80 | (code >> 6 & 0x3f));
            out[used++] = (char)(0x80 | (code & 0x3f));
        }
    }
    while (used && out[used - 1] == ' ') used--;
    out[used] = 0;
    return false;
}

// Like the status wrapper, but widths count glyphs; offsets and lengths are bytes.
static inline int epaper_154g_caption_wrap(const char *text, int cols, int max,
                                      int *start, int *len) {
    int lines = 0;
    const char *p = text;
    while (*p) {
        while (*p == ' ') p++;
        if (!*p) break;
        const char *begin = p, *space = NULL;
        int cells = 0;
        while (*p && cells < cols) {
            if (*p == ' ') space = p;
            epaper_154g_caption_decode(&p);
            cells++;
        }
        if (*p && *p != ' ' && space) p = space;
        const char *end = p;
        while (end > begin && end[-1] == ' ') end--;
        if (lines < max) { start[lines] = (int)(begin - text); len[lines] = (int)(end - begin); }
        lines++;
    }
    return lines;
}

static inline int epaper_154g_caption_cells(const char *text, int bytes) {
    const char *p = text;
    int count = 0;
    while (p < text + bytes && *p) { epaper_154g_caption_decode(&p); count++; }
    return count;
}

// A page shares the line wrapper, so a word is never split at a page boundary
// unless it is wider than a complete line.
static inline unsigned epaper_154g_caption_page(const char *text, unsigned page, char *out) {
    int lines = epaper_154g_caption_wrap(text, EPAPER_154G_CAPTION_COLS, 0, NULL, NULL);
    unsigned pages = (lines + EPAPER_154G_CAPTION_LINES - 1) / EPAPER_154G_CAPTION_LINES;
    out[0] = 0;
    if (page >= pages) return pages;
    int start[EPAPER_154G_CAPTION_LINES], len[EPAPER_154G_CAPTION_LINES];
    const char *p = text;
    for (unsigned i = 0; i <= page; i++) {
        lines = epaper_154g_caption_wrap(p, EPAPER_154G_CAPTION_COLS, EPAPER_154G_CAPTION_LINES, start, len);
        int last = lines < EPAPER_154G_CAPTION_LINES ? lines - 1 : EPAPER_154G_CAPTION_LINES - 1;
        int end = start[last] + len[last];
        if (i == page) {
            int bytes = end - start[0];
            memcpy(out, p + start[0], bytes);
            out[bytes] = 0;
        }
        p += end;
    }
    return pages;
}

static inline void epaper_154g_caption_format(const char *reply, const char *heard, char *out) {
    epaper_154g_caption_normalize(reply, out, EPAPER_154G_CAPTION_BYTES);
    if (!out[0]) return;
    char question[EPAPER_154G_CAPTION_PAGE_BYTES];
    bool heard_cut = epaper_154g_caption_normalize(heard, question, sizeof(question));
    if (question[0] && !heard_cut && strlen(out) < EPAPER_154G_CAPTION_PAGE_BYTES) {
        size_t question_bytes = strlen(question), answer_bytes = strlen(out);
        char both[EPAPER_154G_CAPTION_PAGE_BYTES];
        if (question_bytes + answer_bytes + 12 < sizeof(both)) {
            memcpy(both, "You: ", 5);
            memcpy(both + 5, question, question_bytes);
            memcpy(both + 5 + question_bytes, " Muse: ", 7);
            memcpy(both + 12 + question_bytes, out, answer_bytes + 1);
            if (epaper_154g_caption_wrap(both, EPAPER_154G_CAPTION_COLS, 0, NULL, NULL) <= EPAPER_154G_CAPTION_LINES)
                memcpy(out, both, strlen(both) + 1);
        }
    }
}
