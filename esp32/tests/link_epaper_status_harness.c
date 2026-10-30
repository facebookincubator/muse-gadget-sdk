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
// and RGB565 to packed 2-bit and 4-bit ink codes.
// The runner extracts the production pixel code into epaper_pixels.inc.
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// Both panels' code.
#define EPD_COLOR 1
#include "epaper_pixels.inc"

#define W 64
#define H 32

static uint8_t gray[W * H];
static uint8_t bits[W / 8 * H];
static int16_t err[2 * (W + 2)];
static uint16_t rgb[W * H];
static uint8_t codes[W / 2 * H];
static int16_t color_err[6 * (W + 2)];

// The ink code of pixel (x, y) in `codes`.
static int code_at(int x, int y) {
    uint8_t byte = codes[y * W / 2 + x / 2];
    return x & 1 ? byte & 0x0F : byte >> 4;
}

static void fill_rgb(uint16_t px) {
    for (int i = 0; i < W * H; i++) rgb[i] = px;
}

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

static void check_gray4(void) {
    uint8_t packed[W / 4 * H];
    memset(gray, 0, sizeof(gray));
    dither_gray4(gray, packed, W, H, err);
    for (size_t i = 0; i < sizeof(packed); i++) assert(packed[i] == 0);
    memset(gray, 255, sizeof(gray));
    dither_gray4(gray, packed, W, H, err);
    for (size_t i = 0; i < sizeof(packed); i++) assert(packed[i] == 0xFF);
    gray[0] = 0;
    gray[1] = 85;
    gray[2] = 170;
    gray[3] = 255;
    dither_gray4(gray, packed, W, H, err);
    assert(packed[0] == 0x1B);
}

static void check_rgb565_be(void) {
    const uint8_t src[] = {0xF8, 0x00, 0x07, 0xE0, 0x00, 0x1F, 0x84, 0x10};
    for (int i = 0; i < 4; i++) rgb[i] = rgb565_from_be(src + 2 * i);
    assert(rgb[0] == 0xF800);
    assert(rgb[1] == 0x07E0);
    assert(rgb[2] == 0x001F);
    assert(rgb[3] == 0x8410);
    assert(luma565(rgb[3]) >= 126 && luma565(rgb[3]) <= 134);
    for (int i = 4; i < W * H; i++) rgb[i] = 0xFFFF;
    dither_color(rgb, codes, W, H, color_err, 4);
    assert(codes[0] == 0x36);
    assert(codes[1] >> 4 == 5);
    assert((codes[1] & 0x0F) <= 1);
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

static void check_color_dither(void) {
    assert(nearest_ink(0, 255, 255, 6) == 4);
    // Each ink's own colour comes through exactly, as its controller code.
    const struct { uint16_t px; int code; } inks[] = {
        {0x0000, 0}, {0xFFFF, 1}, {0xFFE0, 2}, {0xF800, 3}, {0x001F, 5}, {0x07E0, 6},
    };
    for (size_t i = 0; i < sizeof(inks) / sizeof(inks[0]); i++) {
        fill_rgb(inks[i].px);
        dither_color(rgb, codes, W, H, color_err, 4);
        for (int y = 0; y < H; y++) {
            for (int x = 0; x < W; x++) assert(code_at(x, y) == inks[i].code);
        }
    }

    // Left pixel in the high nibble: red, blue, then white.
    fill_rgb(0xFFFF);
    rgb[0] = 0xF800;
    rgb[1] = 0x001F;
    rgb[W] = 0xF800;
    rgb[W + 1] = 0x001F;
    dither_color(rgb, codes, W, H, color_err, 4);
    assert(codes[0] == 0x35);
    assert(codes[1] == 0x11);
    assert(codes[W / 2] == 0x35);
    assert(codes[W / 2 + 1] == 0x11);

    // Grays stay black and white, as that share of white dots.
    const uint16_t grays[] = {0x2104, 0x8410, 0x8411, 0xC618};
    for (size_t g = 0; g < sizeof(grays) / sizeof(grays[0]); g++) {
        fill_rgb(grays[g]);
        dither_color(rgb, codes, W, H, color_err, 4);
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

    fill_rgb(0x07FF);
    dither_color(rgb, codes, W, H, color_err, 4);
    int blue = 0;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) blue += code_at(x, y) == 5;
    }
    assert(blue > W * H / 2);

    // Orange mixes red and yellow, and nothing else.
    fill_rgb(0xFC00);
    dither_color(rgb, codes, W, H, color_err, 4);
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

static void check_four_color_dither(void) {
    uint8_t packed[W * H / 4 + 1];
    const struct { uint16_t px; uint8_t byte; } inks[] = {
        {0x0000, 0x00}, {0xFFFF, 0x55}, {0xFFE0, 0xAA}, {0xF800, 0xFF},
    };
    for (size_t i = 0; i < sizeof(inks) / sizeof(inks[0]); i++) {
        fill_rgb(inks[i].px);
        memset(packed, 0xA5, sizeof(packed));
        dither_color(rgb, packed, W, H, color_err, 2);
        for (size_t j = 0; j < sizeof(packed) - 1; j++) assert(packed[j] == inks[i].byte);
        assert(packed[sizeof(packed) - 1] == 0xA5);
    }

    // Black/white split within a byte: leftmost pixel in the top two bits.
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) rgb[y * W + x] = x < 2 ? 0x0000 : 0xFFFF;
    }
    dither_color(rgb, packed, W, H, color_err, 2);
    for (int y = 0; y < H; y++) {
        assert(packed[y * W / 4] == 0x05);
        for (int x = 1; x < W / 4; x++) assert(packed[y * W / 4 + x] == 0x55);
    }

    // Blue must be dithered to the four inks, not packed as Spectra 6 code 5:
    // nearly all black, the rest white.
    fill_rgb(0x001F);
    dither_color(rgb, packed, W, H, color_err, 2);
    int black = 0;
    for (int i = 0; i < W * H; i++) {
        int c = packed[i / 4] >> (6 - 2 * (i % 4)) & 3;
        assert(c == 0 || c == 1);
        black += c == 0;
    }
    assert(black > W * H * 9 / 10);
}

int main(void) {
    check_luma();
    check_rgb565_be();
    check_dither();
    check_gray4();
    check_color_dither();
    check_four_color_dither();
    puts("PASS epaper pixels: RGB565 luma, exact black and white, bit order, checkerboard, gray levels as dot density; "
         "exact Spectra 6 inks, nibble order, grays in black and white, orange as red and yellow; "
         "four-colour inks, packed byte count and pixel order");
    return 0;
}
