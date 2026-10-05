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

// UTF-8 captions on the Waveshare 1.54G: decoding, normalising, glyph lookup,
// word wrap, pages and the question-and-answer line, from the header alone.
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "epaper_154g_caption_text.h"

static uint32_t decode(const char *text, size_t *consumed) {
    const char *p = text;
    uint32_t code = epaper_154g_caption_decode(&p);
    *consumed = (size_t)(p - text);
    return code;
}

static void check_decode(void) {
    size_t n;
    assert(decode("A", &n) == 'A' && n == 1);
    assert(decode("\xd0\xaf", &n) == 0x42f && n == 2);
    assert(decode("\xe2\x82\xac", &n) == 0x20ac && n == 3);
    assert(decode("\xf0\x9f\x98\x80", &n) == 0x1f600 && n == 4);
    assert(decode("", &n) == 0 && n == 0);
    // Bad input costs one byte: a stray continuation, a cut sequence, an overlong
    // form and a surrogate.
    assert(decode("\x80x", &n) == '?' && n == 1);
    assert(decode("\xd0", &n) == '?' && n == 1);
    assert(decode("\xd0" "A", &n) == '?' && n == 1);
    assert(decode("\xe0\x80\x80", &n) == '?' && n == 1);
    assert(decode("\xed\xa0\x80", &n) == '?' && n == 1);
}

static void check_glyphs(void) {
    const uint8_t *space = epaper_154g_caption_glyph(' ');
    int ink = 0;
    assert(epaper_154g_caption_glyph('A') && epaper_154g_caption_glyph(0x42f) && epaper_154g_caption_glyph(0x44f));
    assert(!epaper_154g_caption_glyph(0x1f600));
    assert(memcmp(epaper_154g_caption_glyph('A'), epaper_154g_caption_glyph(0x42f), 16) != 0);
    for (int i = 0; i < 16; i++) ink += space[i];
    assert(ink == 0);
}

static void check_normalize(void) {
    char out[64];
    assert(!epaper_154g_caption_normalize("  Hello,\n\n  world\t ", out, sizeof(out)));
    assert(!strcmp(out, "Hello, world"));
    // Typographic quotes and dashes become the font's ASCII, an unknown glyph a question mark.
    assert(!epaper_154g_caption_normalize("\xe2\x80\x9cq\xe2\x80\x9d \xe2\x80\x94 \xf0\x9f\x98\x80", out, sizeof(out)));
    assert(!strcmp(out, "\"q\" - ?"));
    assert(!epaper_154g_caption_normalize("Привет", out, sizeof(out)) && !strcmp(out, "Привет"));
    // A short buffer cuts between characters and reports the cut.
    assert(epaper_154g_caption_normalize("Привет", out, 4) && !strcmp(out, "П"));
    assert(!epaper_154g_caption_normalize(NULL, out, sizeof(out)) && !out[0]);
}

static void check_wrap(void) {
    const char *text = "Поверни экран кнопкой PWR, пожалуйста";
    int start[8], len[8];
    // Words move whole to the next line; the width counts glyphs, not bytes.
    int lines = epaper_154g_caption_wrap(text, EPAPER_154G_CAPTION_COLS, 8, start, len);
    assert(lines == 2);
    assert(!strncmp(text + start[0], "Поверни экран кнопкой", len[0]));
    assert(epaper_154g_caption_cells(text + start[0], len[0]) == 21);
    assert(!strncmp(text + start[1], "PWR, пожалуйста", len[1]));
    assert(epaper_154g_caption_cells(text + start[1], len[1]) == 15);
    assert(epaper_154g_caption_wrap(text, EPAPER_154G_CAPTION_COLS, 0, NULL, NULL) == 2);
    // A word wider than a line breaks at the column.
    lines = epaper_154g_caption_wrap("abcdefghijklmnopqrstuvwxyz0123", EPAPER_154G_CAPTION_COLS, 8, start, len);
    assert(lines == 2 && len[0] == 24 && len[1] == 6);
}

static void check_pages(void) {
    char text[400] = "";
    char page[EPAPER_154G_CAPTION_BYTES];
    // Three six-letter words a line, seven lines a page: 45 words make three pages.
    for (int i = 0; i < 45; i++) {
        snprintf(text + strlen(text), sizeof(text) - strlen(text), "%sword%02d", i ? " " : "", i);
    }
    assert(epaper_154g_caption_page(text, 0, page) == 3);
    assert(!strncmp(page, "word00 ", 7) && !strcmp(page + strlen(page) - 6, "word20"));
    assert(epaper_154g_caption_page(text, 1, page) == 3);
    assert(!strncmp(page, "word21 ", 7) && !strcmp(page + strlen(page) - 6, "word41"));
    assert(epaper_154g_caption_page(text, 2, page) == 3 && !strcmp(page, "word42 word43 word44"));
    assert(epaper_154g_caption_page(text, 3, page) == 3 && !page[0]);
}

static void check_format(void) {
    char out[EPAPER_154G_CAPTION_BYTES];
    char reply[400];
    // Question and answer share the screen when they fit it; otherwise the answer alone.
    epaper_154g_caption_format("Twelve degrees.", "what is the temperature", out);
    assert(!strcmp(out, "You: what is the temperature Muse: Twelve degrees."));
    memset(reply, 'a', 300);
    reply[300] = 0;
    epaper_154g_caption_format(reply, "question", out);
    assert(!strcmp(out, reply));
    epaper_154g_caption_format("", "question", out);
    assert(!out[0]);
}

int main(void) {
    check_decode();
    check_glyphs();
    check_normalize();
    check_wrap();
    check_pages();
    check_format();
    puts("PASS epaper caption: UTF-8, Cyrillic glyphs, normalising, wrap, pages, question line");
    return 0;
}
