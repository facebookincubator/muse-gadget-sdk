/*
 * Fast 2D Drawing Primitives & Font Engine for 320x240 RGB565 Framebuffers
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>

#define SCREEN_W 320
#define SCREEN_H 240

// 16-bit RGB565 Color Palette
#define GFX_BLACK       0x0000
#define GFX_NAVY        0x000F
#define GFX_DARKGREEN   0x03E0
#define GFX_DARKCYAN    0x03EF
#define GFX_MAROON      0x7800
#define GFX_PURPLE      0x780F
#define GFX_OLIVE       0x7BE0
#define GFX_LIGHTGREY   0xC618
#define GFX_DARKGREY    0x39E7
#define GFX_BLUE        0x001F
#define GFX_GREEN       0x07E0
#define GFX_CYAN        0x07FF
#define GFX_RED         0xF800
#define GFX_MAGENTA     0xF81F
#define GFX_YELLOW      0xFFE0
#define GFX_WHITE       0xFFFF
#define GFX_ORANGE      0xFD20
#define GFX_GREENYELLOW 0xAFE5
#define GFX_PINK        0xF81F

// Modern Dark Theme UI Colors
#define GFX_BG_DARK     0x1082  // Deep slate background
#define GFX_CARD_BG     0x2124  // Card container background
#define GFX_CARD_BORDER 0x4228  // Subtle border
#define GFX_TEXT_MAIN   0xFFFF  // Crisp white text
#define GFX_TEXT_MUTED  0x9CD3  // Muted secondary text
#define GFX_TEXT_DIM    0x632C  // Dim text
#define GFX_ACCENT_BLUE 0x3CFE  // Modern bright blue
#define GFX_ACCENT_CYAN 0x05BF  // Neon cyan
#define GFX_ACCENT_ROSE 0xFB8E  // Warm coral rose
#define GFX_ACCENT_LIME 0x4FE6  // Electric lime green
#define GFX_ACCENT_GOLD 0xFDE0  // Gold amber

// 5x7 ASCII Font (ASCII 32..126)
static const uint8_t g_font5x7[95 * 5] = {
    0x00, 0x00, 0x00, 0x00, 0x00, // (space)
    0x00, 0x00, 0x5F, 0x00, 0x00, // !
    0x00, 0x07, 0x00, 0x07, 0x00, // "
    0x14, 0x7F, 0x14, 0x7F, 0x14, // #
    0x24, 0x2A, 0x7F, 0x2A, 0x12, // $
    0x23, 0x13, 0x08, 0x64, 0x62, // %
    0x36, 0x49, 0x55, 0x22, 0x50, // &
    0x00, 0x05, 0x03, 0x00, 0x00, // '
    0x00, 0x1C, 0x22, 0x41, 0x00, // (
    0x00, 0x41, 0x22, 0x1C, 0x00, // )
    0x08, 0x2A, 0x1C, 0x2A, 0x08, // *
    0x08, 0x08, 0x3E, 0x08, 0x08, // +
    0x00, 0x50, 0x30, 0x00, 0x00, // ,
    0x08, 0x08, 0x08, 0x08, 0x08, // -
    0x00, 0x60, 0x60, 0x00, 0x00, // .
    0x20, 0x10, 0x08, 0x04, 0x02, // /
    0x3E, 0x51, 0x49, 0x45, 0x3E, // 0
    0x00, 0x42, 0x7F, 0x40, 0x00, // 1
    0x42, 0x61, 0x51, 0x49, 0x46, // 2
    0x21, 0x41, 0x45, 0x4B, 0x31, // 3
    0x18, 0x14, 0x12, 0x7F, 0x10, // 4
    0x27, 0x45, 0x45, 0x45, 0x39, // 5
    0x3C, 0x4A, 0x49, 0x49, 0x30, // 6
    0x01, 0x71, 0x09, 0x05, 0x03, // 7
    0x36, 0x49, 0x49, 0x49, 0x36, // 8
    0x06, 0x49, 0x49, 0x29, 0x1E, // 9
    0x00, 0x36, 0x36, 0x00, 0x00, // :
    0x00, 0x56, 0x36, 0x00, 0x00, // ;
    0x00, 0x08, 0x14, 0x22, 0x41, // <
    0x14, 0x14, 0x14, 0x14, 0x14, // =
    0x41, 0x22, 0x14, 0x08, 0x00, // >
    0x02, 0x01, 0x51, 0x09, 0x06, // ?
    0x32, 0x49, 0x79, 0x41, 0x3E, // @
    0x7E, 0x11, 0x11, 0x11, 0x7E, // A
    0x7F, 0x49, 0x49, 0x49, 0x36, // B
    0x3E, 0x41, 0x41, 0x41, 0x22, // C
    0x7F, 0x41, 0x41, 0x22, 0x1C, // D
    0x7F, 0x49, 0x49, 0x49, 0x41, // E
    0x7F, 0x09, 0x09, 0x01, 0x01, // F
    0x3E, 0x41, 0x41, 0x51, 0x32, // G
    0x7F, 0x08, 0x08, 0x08, 0x7F, // H
    0x00, 0x41, 0x7F, 0x41, 0x00, // I
    0x20, 0x40, 0x41, 0x3F, 0x01, // J
    0x7F, 0x08, 0x14, 0x22, 0x41, // K
    0x7F, 0x40, 0x40, 0x40, 0x40, // L
    0x7F, 0x02, 0x04, 0x02, 0x7F, // M
    0x7F, 0x04, 0x08, 0x10, 0x7F, // N
    0x3E, 0x41, 0x41, 0x41, 0x3E, // O
    0x7F, 0x09, 0x09, 0x09, 0x06, // P
    0x3E, 0x41, 0x51, 0x21, 0x5E, // Q
    0x7F, 0x09, 0x19, 0x29, 0x46, // R
    0x46, 0x49, 0x49, 0x49, 0x31, // S
    0x01, 0x01, 0x7F, 0x01, 0x01, // T
    0x3F, 0x40, 0x40, 0x40, 0x3F, // U
    0x1F, 0x20, 0x40, 0x20, 0x1F, // V
    0x7F, 0x20, 0x18, 0x20, 0x7F, // W
    0x63, 0x14, 0x08, 0x14, 0x63, // X
    0x03, 0x04, 0x78, 0x04, 0x03, // Y
    0x61, 0x51, 0x49, 0x45, 0x43, // Z
    0x00, 0x7F, 0x41, 0x41, 0x00, // [
    0x02, 0x04, 0x08, 0x10, 0x20, // backslash
    0x00, 0x41, 0x41, 0x7F, 0x00, // ]
    0x04, 0x02, 0x01, 0x02, 0x04, // ^
    0x40, 0x40, 0x40, 0x40, 0x40, // _
    0x00, 0x01, 0x02, 0x04, 0x00, // `
    0x20, 0x54, 0x54, 0x54, 0x78, // a
    0x7F, 0x48, 0x44, 0x44, 0x38, // b
    0x38, 0x44, 0x44, 0x44, 0x20, // c
    0x38, 0x44, 0x44, 0x48, 0x7F, // d
    0x38, 0x54, 0x54, 0x54, 0x18, // e
    0x08, 0x7E, 0x09, 0x01, 0x02, // f
    0x08, 0x14, 0x54, 0x54, 0x3C, // g
    0x7F, 0x08, 0x04, 0x04, 0x78, // h
    0x00, 0x44, 0x7D, 0x40, 0x00, // i
    0x20, 0x40, 0x44, 0x3D, 0x00, // j
    0x7F, 0x10, 0x28, 0x44, 0x00, // k
    0x00, 0x41, 0x7F, 0x40, 0x00, // l
    0x7C, 0x04, 0x18, 0x04, 0x78, // m
    0x7C, 0x08, 0x04, 0x04, 0x78, // n
    0x38, 0x44, 0x44, 0x44, 0x38, // o
    0x7C, 0x14, 0x14, 0x14, 0x08, // p
    0x08, 0x14, 0x14, 0x18, 0x7C, // q
    0x7C, 0x08, 0x04, 0x04, 0x08, // r
    0x48, 0x54, 0x54, 0x54, 0x20, // s
    0x04, 0x3F, 0x44, 0x40, 0x20, // t
    0x3C, 0x40, 0x40, 0x20, 0x7C, // u
    0x1C, 0x20, 0x40, 0x20, 0x1C, // v
    0x3C, 0x40, 0x30, 0x40, 0x3C, // w
    0x44, 0x28, 0x10, 0x28, 0x44, // x
    0x0C, 0x50, 0x50, 0x50, 0x3C, // y
    0x44, 0x64, 0x54, 0x4C, 0x44, // z
    0x08, 0x36, 0x41, 0x00, 0x00, // {
    0x00, 0x00, 0x7F, 0x00, 0x00, // |
    0x00, 0x41, 0x36, 0x08, 0x00, // }
    0x08, 0x10, 0x08, 0x04, 0x08  // ~
};

static inline void gfx_draw_pixel(uint16_t *fb, int x, int y, uint16_t col)
{
    if (x >= 0 && x < SCREEN_W && y >= 0 && y < SCREEN_H) {
        fb[y * SCREEN_W + x] = col;
    }
}

static inline void gfx_draw_fast_hline(uint16_t *fb, int x, int y, int w, uint16_t col)
{
    if (y < 0 || y >= SCREEN_H || w <= 0) return;
    int x1 = x < 0 ? 0 : x;
    int x2 = (x + w) > SCREEN_W ? SCREEN_W : (x + w);
    if (x1 >= x2) return;
    uint16_t *p = &fb[y * SCREEN_W + x1];
    int count = x2 - x1;
    while (count--) *p++ = col;
}

static inline void gfx_draw_fast_vline(uint16_t *fb, int x, int y, int h, uint16_t col)
{
    if (x < 0 || x >= SCREEN_W || h <= 0) return;
    int y1 = y < 0 ? 0 : y;
    int y2 = (y + h) > SCREEN_H ? SCREEN_H : (y + h);
    if (y1 >= y2) return;
    uint16_t *p = &fb[y1 * SCREEN_W + x];
    for (int cy = y1; cy < y2; cy++) {
        *p = col;
        p += SCREEN_W;
    }
}

static inline void gfx_fill_rect(uint16_t *fb, int x, int y, int w, int h, uint16_t col)
{
    if (w <= 0 || h <= 0) return;
    int x1 = x < 0 ? 0 : x;
    int y1 = y < 0 ? 0 : y;
    int x2 = (x + w) > SCREEN_W ? SCREEN_W : (x + w);
    int y2 = (y + h) > SCREEN_H ? SCREEN_H : (y + h);
    if (x1 >= x2 || y1 >= y2) return;
    for (int cy = y1; cy < y2; cy++) {
        uint16_t *p = &fb[cy * SCREEN_W + x1];
        int count = x2 - x1;
        while (count--) *p++ = col;
    }
}

static inline void gfx_draw_rect(uint16_t *fb, int x, int y, int w, int h, uint16_t col)
{
    gfx_draw_fast_hline(fb, x, y, w, col);
    gfx_draw_fast_hline(fb, x, y + h - 1, w, col);
    gfx_draw_fast_vline(fb, x, y, h, col);
    gfx_draw_fast_vline(fb, x + w - 1, y, h, col);
}

static inline void gfx_fill_round_rect(uint16_t *fb, int x, int y, int w, int h, int r, uint16_t col)
{
    if (r <= 0) {
        gfx_fill_rect(fb, x, y, w, h, col);
        return;
    }
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    gfx_fill_rect(fb, x + r, y, w - 2 * r, h, col);
    gfx_fill_rect(fb, x, y + r, r, h - 2 * r, col);
    gfx_fill_rect(fb, x + w - r, y + r, r, h - 2 * r, col);
    for (int dy = 0; dy < r; dy++) {
        int dx = (int)sqrtf((float)(r * r - (r - dy) * (r - dy)));
        gfx_draw_fast_hline(fb, x + r - dx, y + dy, dx, col);
        gfx_draw_fast_hline(fb, x + w - r, y + dy, dx, col);
        gfx_draw_fast_hline(fb, x + r - dx, y + h - 1 - dy, dx, col);
        gfx_draw_fast_hline(fb, x + w - r, y + h - 1 - dy, dx, col);
    }
}

static inline void gfx_draw_round_rect(uint16_t *fb, int x, int y, int w, int h, int r, uint16_t col)
{
    gfx_draw_fast_hline(fb, x + r, y, w - 2 * r, col);
    gfx_draw_fast_hline(fb, x + r, y + h - 1, w - 2 * r, col);
    gfx_draw_fast_vline(fb, x, y + r, h - 2 * r, col);
    gfx_draw_fast_vline(fb, x + w - 1, y + r, h - 2 * r, col);
    for (int dy = 0; dy <= r; dy++) {
        int dx = (int)sqrtf((float)(r * r - dy * dy));
        gfx_draw_pixel(fb, x + r - dy, y + r - dx, col);
        gfx_draw_pixel(fb, x + w - 1 - r + dy, y + r - dx, col);
        gfx_draw_pixel(fb, x + r - dy, y + h - 1 - r + dx, col);
        gfx_draw_pixel(fb, x + w - 1 - r + dy, y + h - 1 - r + dx, col);
    }
}

static inline void gfx_fill_circle(uint16_t *fb, int cx, int cy, int r, uint16_t col)
{
    if (r <= 0) return;
    for (int dy = -r; dy <= r; dy++) {
        int py = cy + dy;
        if (py < 0 || py >= SCREEN_H) continue;
        int dx = (int)sqrtf((float)(r * r - dy * dy));
        gfx_draw_fast_hline(fb, cx - dx, py, 2 * dx + 1, col);
    }
}

static inline void gfx_draw_circle(uint16_t *fb, int cx, int cy, int r, uint16_t col)
{
    int x = 0;
    int y = r;
    int d = 3 - 2 * r;
    while (y >= x) {
        gfx_draw_pixel(fb, cx + x, cy + y, col);
        gfx_draw_pixel(fb, cx - x, cy + y, col);
        gfx_draw_pixel(fb, cx + x, cy - y, col);
        gfx_draw_pixel(fb, cx - x, cy - y, col);
        gfx_draw_pixel(fb, cx + y, cy + x, col);
        gfx_draw_pixel(fb, cx - y, cy + x, col);
        gfx_draw_pixel(fb, cx + y, cy - x, col);
        gfx_draw_pixel(fb, cx - y, cy - x, col);
        if (d < 0) {
            d += 4 * x + 6;
        } else {
            d += 4 * (x - y) + 10;
            y--;
        }
        x++;
    }
}

static inline void gfx_draw_char(uint16_t *fb, int x, int y, char c, uint16_t col)
{
    if (c < 32 || c > 126) c = ' ';
    const uint8_t *glyph = &g_font5x7[(c - 32) * 5];
    for (int col_i = 0; col_i < 5; col_i++) {
        uint8_t bits = glyph[col_i];
        for (int row_i = 0; row_i < 7; row_i++) {
            if (bits & (1 << row_i)) {
                gfx_draw_pixel(fb, x + col_i, y + row_i, col);
            }
        }
    }
}

static inline void gfx_draw_string(uint16_t *fb, int x, int y, const char *str, uint16_t col)
{
    while (*str) {
        if (*str == '\n') {
            y += 9;
            str++;
            continue;
        }
        gfx_draw_char(fb, x, y, *str++, col);
        x += 6;
    }
}

static inline void gfx_draw_char_scaled(uint16_t *fb, int x, int y, char c, uint16_t col, int scale)
{
    if (scale <= 1) {
        gfx_draw_char(fb, x, y, c, col);
        return;
    }
    if (c < 32 || c > 126) c = ' ';
    const uint8_t *glyph = &g_font5x7[(c - 32) * 5];
    for (int col_i = 0; col_i < 5; col_i++) {
        uint8_t bits = glyph[col_i];
        for (int row_i = 0; row_i < 7; row_i++) {
            if (bits & (1 << row_i)) {
                gfx_fill_rect(fb, x + col_i * scale, y + row_i * scale, scale, scale, col);
            }
        }
    }
}

static inline void gfx_draw_string_scaled(uint16_t *fb, int x, int y, const char *str, uint16_t col, int scale)
{
    while (*str) {
        if (*str == '\n') {
            y += 9 * scale;
            str++;
            continue;
        }
        gfx_draw_char_scaled(fb, x, y, *str++, col, scale);
        x += 6 * scale;
    }
}
