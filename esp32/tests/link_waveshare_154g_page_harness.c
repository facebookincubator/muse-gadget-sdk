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

// Host harness for the 1.54G's reply page: page text and count in, the
// 200 x 200 picture out. The runner extracts the production rendering code
// into page_render.inc. With PAGE_PGM set, it also writes the picture there.
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fonts/epaper_154g_caption_font.h"
#include "pixel_font.h"
#include "page_render.inc"

static uint8_t s_ink[PAGE_W][PAGE_W];  // 1 where black
static int s_rows;                      // rows covered by bands

static void collect(int y, int height, const uint8_t *rgb565) {
    assert(y == s_rows && height > 0 && height <= CELL_H && y + height <= PAGE_W);
    for (int r = 0; r < height; r++) {
        for (int x = 0; x < PAGE_W; x++) {
            const uint8_t *px = rgb565 + ((size_t)r * PAGE_W + x) * 2;
            assert((px[0] == 0 && px[1] == 0) || (px[0] == 0xFF && px[1] == 0xFF));
            s_ink[y + r][x] = px[0] == 0;
        }
    }
    s_rows += height;
}

static void render(const char *page, const char *count) {
    static uint8_t strip[PAGE_W * CELL_H * 2];
    memset(s_ink, 0, sizeof(s_ink));
    s_rows = 0;
    render_page(page, count, strip, collect);
    assert(s_rows == PAGE_W);
}

// Black pixels inside [x0, x1) x [y0, y1).
static int ink(int x0, int y0, int x1, int y1) {
    int n = 0;
    for (int y = y0; y < y1; y++) {
        for (int x = x0; x < x1; x++) n += s_ink[y][x];
    }
    return n;
}

int main(void) {
    const char *sample =
        "Конечно! Завтра выезд\nв 8:30, потом забрать\nпосылку и обед с Анной\nв 12. Дождь закончится\n"
        "к полудню.\nTomorrow: leave at 8:30,\npick up the parcel, then\nlunch with Anna at 12.\n"
        "The rain should stop by\nnoon, so take a light\njacket.";
    render(sample, "1/3");

    // Margins: nothing in the top 10 rows or the 4 columns at each side.
    assert(ink(0, 0, PAGE_W, PAGE_TOP) == 0);
    assert(ink(0, 0, PAGE_LEFT, PAGE_W) == 0);
    assert(ink(PAGE_W - PAGE_LEFT, 0, PAGE_W, PAGE_W) == 0);
    // Each of the 11 lines inside its own 16-row band.
    for (int line = 0; line < PAGE_LINES; line++) {
        int y = PAGE_TOP + line * CELL_H;
        assert(ink(0, y, PAGE_W, y + CELL_H) > 0);
    }
    // The count: right-aligned in the bottom margin, in the 5x8 font.
    int bottom = PAGE_TOP + PAGE_LINES * CELL_H;
    int count_x = PAGE_W - PAGE_LEFT - 3 * (PIXEL_FONT_WIDTH + 1);
    assert(ink(count_x, bottom + 3, PAGE_W - PAGE_LEFT, bottom + 3 + PIXEL_FONT_HEIGHT) > 0);
    assert(ink(0, bottom, count_x, PAGE_W) == 0);

    const char *pgm = getenv("PAGE_PGM");
    if (pgm) {
        FILE *f = fopen(pgm, "wb");
        assert(f);
        fprintf(f, "P5\n%d %d\n255\n", PAGE_W, PAGE_W);
        for (int y = 0; y < PAGE_W; y++) {
            for (int x = 0; x < PAGE_W; x++) fputc(s_ink[y][x] ? 0 : 255, f);
        }
        fclose(f);
    }

    // No count: the bottom margin stays white.
    render("one page only", "");
    assert(ink(0, bottom, PAGE_W, PAGE_W) == 0);

    // A line longer than the page clips at 24 columns.
    render("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123", "");
    assert(ink(PAGE_LEFT + PAGE_COLS * CELL_W, 0, PAGE_W, PAGE_W) == 0);
    assert(ink(PAGE_LEFT + (PAGE_COLS - 1) * CELL_W, PAGE_TOP, PAGE_LEFT + PAGE_COLS * CELL_W,
               PAGE_TOP + CELL_H) > 0);

    // Cyrillic has its own glyphs; a letter the font lacks (U+4E00) and an
    // invalid byte draw as '?', one cell each.
    render("?", "");
    static uint8_t question[PAGE_W][PAGE_W];
    memcpy(question, s_ink, sizeof(question));
    render("\xd0\x96", "");  // Ж
    assert(ink(0, 0, PAGE_W, PAGE_W) > 0);
    assert(memcmp(s_ink, question, sizeof(question)) != 0);
    render("\xe4\xb8\x80", "");
    assert(memcmp(s_ink, question, sizeof(question)) == 0);
    render("\xff", "");
    assert(memcmp(s_ink, question, sizeof(question)) == 0);

    // Exact pixels: the first cell is the font's bitmap, leftmost pixel in bit 7.
    render("\xd0\x96R", "");
    const uint8_t *zhe = epaper_154g_caption_glyph(0x416);
    for (int r = 0; r < CELL_H; r++) {
        for (int c = 0; c < CELL_W; c++) {
            assert(s_ink[PAGE_TOP + r][PAGE_LEFT + c] == (zhe[r] >> (7 - c) & 1));
        }
    }

    puts("PASS waveshare 1.54G page: margins, 11 line bands, count placement, clipping, Cyrillic and fallback, exact glyph pixels");
    return 0;
}
