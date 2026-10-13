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

// Host e-paper pixel harness: RGB565 to gray, gray to packed 1-bit frames,
// and RGB565 to packed ink codes: 4-bit Spectra 6 by default, or the 1.54G's
// 2-bit four inks with -DEPD_INK_BITS=2.
// The runner extracts the production pixel code into epaper_pixels.inc.
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// The black and white panel's code and one colour panel's.
#define EPD_COLOR 1
#ifndef EPD_INK_BITS
#define EPD_INK_BITS 4
#endif
#include "epaper_pixels.inc"

#define W 64
#define H 32

#if EPD_INK_BITS == 4
static uint8_t gray[W * H];
static uint8_t bits[W / 8 * H];
static int16_t err[2 * (W + 2)];
#endif
static uint16_t rgb[W * H];
static uint8_t codes[W * EPD_INK_BITS / 8 * H];
static int16_t color_err[6 * (W + 2)];

// The ink code of pixel (x, y) in `codes`.
static int code_at(int x, int y) {
    const int per_byte = 8 / EPD_INK_BITS;
    uint8_t byte = codes[y * W / per_byte + x / per_byte];
    return byte >> (8 - EPD_INK_BITS * (x % per_byte + 1)) & ((1 << EPD_INK_BITS) - 1);
}

static void fill_rgb(uint16_t px) {
    for (int i = 0; i < W * H; i++) rgb[i] = px;
}

#if EPD_INK_BITS == 4
static int white_count(void) {
    int n = 0;
    for (size_t i = 0; i < sizeof(bits); i++) n += __builtin_popcount(bits[i]);
    return n;
}

static void check_luma(void) {
    assert(luma565(0x0000) == 0);
    assert(luma565(0xFFFF) == 255);
    // Green counts most, blue least.
    assert(luma565(0x07E0) > luma565(0xF800));
    assert(luma565(0xF800) > luma565(0x001F));
    // Mid gray stays mid gray.
    uint8_t mid = luma565(0x8410);
    assert(mid >= 126 && mid <= 134);
}

static void check_dither(void) {
    // Pure black and white come through exactly, 1 for white, leftmost pixel
    // in the top bit.
    memset(gray, 255, sizeof(gray));
    dither_frame(gray, bits, W, H, err);
    for (size_t i = 0; i < sizeof(bits); i++) assert(bits[i] == 0xFF);
    memset(gray, 0, sizeof(gray));
    gray[0] = 255;
    gray[W + 9] = 255;
    dither_frame(gray, bits, W, H, err);
    assert(bits[0] == 0x80);
    assert(bits[W / 8 + 1] == 0x40);
    assert(white_count() == 2);

    // A checkerboard of black and white is left alone.
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) gray[y * W + x] = (x + y) % 2 ? 255 : 0;
    }
    dither_frame(gray, bits, W, H, err);
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W / 8; x++) assert(bits[y * W / 8 + x] == (y % 2 ? 0xAA : 0x55));
    }

    // Grays come out as that share of white dots.
    const int levels[] = {32, 128, 192};
    for (size_t l = 0; l < sizeof(levels) / sizeof(levels[0]); l++) {
        memset(gray, levels[l], sizeof(gray));
        dither_frame(gray, bits, W, H, err);
        int expect = W * H * levels[l] / 255;
        int got = white_count();
        assert(got > expect - W * H / 50 && got < expect + W * H / 50);
    }
}

#endif

#if EPD_INK_BITS == 4
static void check_color_dither(void) {
    // Each ink's own colour comes through exactly, as its controller code.
    const struct { uint16_t px; int code; } inks[] = {
        {0x0000, 0}, {0xFFFF, 1}, {0xFFE0, 2}, {0xF800, 3}, {0x001F, 5}, {0x07E0, 6},
    };
    for (size_t i = 0; i < sizeof(inks) / sizeof(inks[0]); i++) {
        fill_rgb(inks[i].px);
        dither_color(rgb, codes, W, H, color_err);
        for (int y = 0; y < H; y++) {
            for (int x = 0; x < W; x++) assert(code_at(x, y) == inks[i].code);
        }
    }

    // Left pixel in the high nibble: red, blue, then white.
    fill_rgb(0xFFFF);
    rgb[0] = 0xF800;
    rgb[1] = 0x001F;
    dither_color(rgb, codes, W, H, color_err);
    assert(codes[0] == 0x35);
    assert(codes[1] == 0x11);

    // Grays stay black and white, as that share of white dots.
    const uint16_t grays[] = {0x2104, 0x8410, 0xC618};
    for (size_t g = 0; g < sizeof(grays) / sizeof(grays[0]); g++) {
        fill_rgb(grays[g]);
        dither_color(rgb, codes, W, H, color_err);
        int white = 0;
        for (int y = 0; y < H; y++) {
            for (int x = 0; x < W; x++) {
                int c = code_at(x, y);
                assert(c == 0 || c == 1);
                white += c;
            }
        }
        int expect = W * H * luma565(grays[g]) / 255;
        assert(white > expect - W * H / 50 && white < expect + W * H / 50);
    }

    // Orange mixes red and yellow, and nothing else.
    fill_rgb(0xFC00);
    dither_color(rgb, codes, W, H, color_err);
    int red = 0, yellow = 0;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            int c = code_at(x, y);
            assert(c == 2 || c == 3);
            red += c == 3;
            yellow += c == 2;
        }
    }
    assert(red > W * H / 4 && yellow > W * H / 4);
}
#else
static void check_color_dither(void) {
    // Each ink's own colour comes through exactly, as its controller code.
    const struct { uint16_t px; int code; } inks[] = {
        {0x0000, 0}, {0xFFFF, 1}, {0xFFE0, 2}, {0xF800, 3},
    };
    for (size_t i = 0; i < sizeof(inks) / sizeof(inks[0]); i++) {
        fill_rgb(inks[i].px);
        dither_color(rgb, codes, W, H, color_err);
        for (int y = 0; y < H; y++) {
            for (int x = 0; x < W; x++) assert(code_at(x, y) == inks[i].code);
        }
    }

    // Four pixels to a byte, leftmost in the top bits: red, yellow, black,
    // then white.
    fill_rgb(0xFFFF);
    rgb[0] = 0xF800;
    rgb[1] = 0xFFE0;
    rgb[2] = 0x0000;
    dither_color(rgb, codes, W, H, color_err);
    assert(codes[0] == 0xE1);
    assert(codes[1] == 0x55);

    // Flat gray and orange stay a single ink, including after dirty scratch.
    const struct { uint16_t px; int code; } flats[] = {
        {0x2104, 0}, {0x8410, 1}, {0xC618, 1}, {0xFC00, 2},
        // Near the black/white threshold: dropping replicated channel bits flips the ink.
        {0x24DE, 1}, {0x055F, 1},
    };
    for (size_t i = 0; i < sizeof(flats) / sizeof(flats[0]); i++) {
        fill_rgb(flats[i].px);
        memset(color_err, 0x7f, sizeof(color_err));
        dither_color(rgb, codes, W, H, color_err);
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) assert(code_at(x, y) == flats[i].code);
    }
    // A pixel's ink cannot depend on its neighbours or previous rows.
    uint8_t expected[W * H];
    for (int i = 0; i < W * H; i++) {
        uint16_t px = (uint16_t)(i * 7919);
        uint16_t one[4] = {px, px, px, px};
        uint8_t code;
        dither_color(one, &code, 4, 1, color_err);
        expected[i] = code >> 6;
        rgb[i] = px;
    }
    memset(codes, 0xff, sizeof(codes));
    dither_color(rgb, codes, W, H, color_err);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) assert(code_at(x, y) == expected[y * W + x]);

}
#endif

int main(void) {
#if EPD_INK_BITS == 4
    check_luma();
    check_dither();
#endif
    check_color_dither();
#if EPD_INK_BITS == 4
    puts("PASS epaper pixels: RGB565 luma, exact black and white, bit order, checkerboard, gray levels as dot density; "
         "exact Spectra 6 inks, nibble order, grays in black and white, orange as red and yellow");
#else
    puts("PASS epaper pixels: RGB565 luma, exact black and white, bit order, checkerboard, gray levels as dot density; "
         "exact four inks, 2-bit order, flat gray and orange, independent pixels");
#endif
    return 0;
}
