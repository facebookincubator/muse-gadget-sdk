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

/*
 * Pet World & 3-Room House Implementation
 * High-Resolution Native 320x240 Graphics Engine
 */

#include "pet_world.h"
#include "pet_sprites.h"
#include "gfx_primitives.h"
#include "../app_manager.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include <time.h>
#include "muse_chat.h"
#include "muse_state.h"
#include "muse_wifi.h"

static const char *TAG = "pet_world";

// Colors (RGB565)
#define COLOR_BG_TOPBAR   0x2124 // Dark slate
#define COLOR_TEXT_MUTED  0x9CD3
#define COLOR_GOLD        0xFDC0
#define COLOR_HEART_PINK  0xF9B4

#define COLOR_MUSE_BLUE       0x1BDD
#define COLOR_MUSE_BLUE_DARK  0x11B4
#define COLOR_MUSE_BLUE_LIGHT 0x4C5F
#define COLOR_MUSE_FACE       0x445E
#define COLOR_MUSE_LOGO       0xF759
#define COLOR_MUSE_EYE        0x0882
#define COLOR_MUSE_BLUSH      0xEA4D

// Kitchen Colors
#define COLOR_KITCHEN_WALL   0xEF5B
#define COLOR_KITCHEN_TILE1  0xF7BE
#define COLOR_KITCHEN_TILE2  0xE657
#define COLOR_FRIDGE         0xDF7E
#define COLOR_COUNTER        0xC552

// Living Room Colors
#define COLOR_LIVING_WALL    0xFE38
#define COLOR_LIVING_FLOOR   0xCDD0
#define COLOR_LIVING_COUCH   0x4477
#define COLOR_LIVING_CUSHION 0xF52A

// Bedroom Colors
#define COLOR_BED_WALL       0x296E
#define COLOR_BED_FLOOR      0x39AF
#define COLOR_BED_FRAME      0x8A22
#define COLOR_BED_SHEET      0xDE7B
#define COLOR_BED_BLANKET    0x9334

#define AVATAR_W 52
#define AVATAR_H 62

static uint16_t *s_fb = NULL;
static pet_world_t s_world;

// --- Pet-world-only drawing: ellipses, triangles, alpha-blended sprites. ---
// Rectangles, circles and 5x7 text come from gfx_primitives.h.

static void fill_ellipse(uint16_t *fb, int cx, int cy, int rx, int ry, uint16_t col)
{
    if (rx <= 0 || ry <= 0) return;
    float ry2 = (float)(ry * ry);
    for (int dy = -ry; dy <= ry; dy++) {
        int py = cy + dy;
        if (py < 0 || py >= WORLD_SCREEN_H) continue;
        int dx = (int)(rx * sqrtf(fmaxf(0.0f, 1.0f - (float)(dy * dy) / ry2)));
        gfx_draw_fast_hline(fb, cx - dx, py, 2 * dx + 1, col);
    }
}

static void draw_ellipse(uint16_t *fb, int cx, int cy, int rx, int ry, uint16_t col)
{
    if (rx <= 0 || ry <= 0) return;
    int x = 0;
    int y = ry;
    long a2 = (long)rx * rx;
    long b2 = (long)ry * ry;
    long crit1 = -(a2 / 4 + rx % 2 + b2);
    long crit2 = -(b2 / 4 + ry % 2 + a2);
    long crit3 = -(b2 / 4 + ry % 2);
    long t = -a2 * y;
    long dxt = 2 * b2 * x, dyt = -2 * a2 * y;
    long d2xt = 2 * b2, d2yt = 2 * a2;

    while (y >= 0 && x <= rx) {
        gfx_draw_pixel(fb, cx + x, cy + y, col);
        gfx_draw_pixel(fb, cx - x, cy + y, col);
        gfx_draw_pixel(fb, cx + x, cy - y, col);
        gfx_draw_pixel(fb, cx - x, cy - y, col);
        if (t + b2 * x <= crit1 || t + a2 * y <= crit3) {
            x++;
            dxt += d2xt;
            t += dxt;
        } else if (t - a2 * y > crit2) {
            y--;
            dyt += d2yt;
            t += dyt;
        } else {
            x++;
            dxt += d2xt;
            t += dxt;
            y--;
            dyt += d2yt;
            t += dyt;
        }
    }
}

static void fill_triangle(uint16_t *fb, int x0, int y0, int x1, int y1, int x2, int y2, uint16_t col)
{
    // Sort vertices by y: y0 <= y1 <= y2
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; t = x0; x0 = x1; x1 = t; }
    if (y1 > y2) { int t = y1; y1 = y2; y2 = t; t = x1; x1 = x2; x2 = t; }
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; t = x0; x0 = x1; x1 = t; }

    if (y0 == y2) return;

    for (int y = y0; y <= y2; y++) {
        if (y < 0 || y >= WORLD_SCREEN_H) continue;
        int xa = x0 + (x2 - x0) * (y - y0) / (y2 - y0);
        int xb;
        if (y < y1) {
            xb = (y1 == y0) ? x0 : (x0 + (x1 - x0) * (y - y0) / (y1 - y0));
        } else {
            xb = (y2 == y1) ? x1 : (x1 + (x2 - x1) * (y - y1) / (y2 - y1));
        }
        if (xa > xb) { int t = xa; xa = xb; xb = t; }
        gfx_draw_fast_hline(fb, xa, y, xb - xa + 1, col);
    }
}

// Alpha blend pixel helper
static inline void blend_pixel(uint16_t *dst, uint16_t src, uint8_t a)
{
    if (a == 0) return;
    if (a >= 250) {
        *dst = src;
        return;
    }
    /* Per-channel blend. Combining R and B into one lane (the usual 0xF81F
     * trick) underflows when a source channel is darker than the destination,
     * bleeding colour between channels; this keeps each channel independent. */
    uint32_t ia = 256u - a;
    uint16_t d = *dst;
    uint32_t r = ((((uint32_t)src >> 11) & 0x1F) * a + (((uint32_t)d >> 11) & 0x1F) * ia) >> 8;
    uint32_t g = ((((uint32_t)src >> 5) & 0x3F) * a + (((uint32_t)d >> 5) & 0x3F) * ia) >> 8;
    uint32_t b = (((uint32_t)src & 0x1F) * a + ((uint32_t)d & 0x1F) * ia) >> 8;
    *dst = (uint16_t)((r << 11) | (g << 5) | b);
}

// Render 52x62 sprite with full alpha blending
static void draw_sprite_52x62(uint16_t *fb, const uint32_t *sprite, int sx, int sy)
{
    for (int r = 0; r < PET_SPRITE_H; r++) {
        int dy = sy + r;
        if (dy < 0 || dy >= WORLD_SCREEN_H) continue;
        uint16_t *row = &fb[dy * WORLD_SCREEN_W];
        const uint32_t *sp_row = &sprite[r * PET_SPRITE_W];
        for (int c = 0; c < PET_SPRITE_W; c++) {
            int dx = sx + c;
            if (dx < 0 || dx >= WORLD_SCREEN_W) continue;
            uint32_t val = sp_row[c];
            uint8_t a = (uint8_t)(val >> 16);
            if (a == 0) continue;
            uint16_t s_col = (uint16_t)(val & 0xFFFF);
            blend_pixel(&row[dx], s_col, a);
        }
    }
}

// --- House & 3-Room World Drawing ---

static void draw_walls(uint16_t *fb, float cam_x)
{
    int wall_h = 142 - WORLD_STATUS_BAR_H;

    // 1. Kitchen Wall (World X: 0..320)
    int kw_sx = (int)(0 - cam_x);
    if (kw_sx + 320 > 0 && kw_sx < WORLD_SCREEN_W) {
        int c_left = kw_sx < 0 ? 0 : kw_sx;
        int c_right = (kw_sx + 320) > WORLD_SCREEN_W ? WORLD_SCREEN_W : (kw_sx + 320);
        gfx_fill_rect(fb, c_left, WORLD_STATUS_BAR_H, c_right - c_left, wall_h, COLOR_KITCHEN_WALL);
    }

    // 2. Living Room Wall (World X: 320..640)
    int lw_sx = (int)(320 - cam_x);
    if (lw_sx + 320 > 0 && lw_sx < WORLD_SCREEN_W) {
        int c_left = lw_sx < 0 ? 0 : lw_sx;
        int c_right = (lw_sx + 320) > WORLD_SCREEN_W ? WORLD_SCREEN_W : (lw_sx + 320);
        gfx_fill_rect(fb, c_left, WORLD_STATUS_BAR_H, c_right - c_left, wall_h, COLOR_LIVING_WALL);
    }

    // 3. Bedroom Wall (World X: 640..960)
    int bw_sx = (int)(640 - cam_x);
    if (bw_sx + 320 > 0 && bw_sx < WORLD_SCREEN_W) {
        int c_left = bw_sx < 0 ? 0 : bw_sx;
        int c_right = (bw_sx + 320) > WORLD_SCREEN_W ? WORLD_SCREEN_W : (bw_sx + 320);
        gfx_fill_rect(fb, c_left, WORLD_STATUS_BAR_H, c_right - c_left, wall_h, COLOR_BED_WALL);
        // Star wallpaper dots
        for (int wx = 660; wx < 940; wx += 35) {
            for (int wy = WORLD_STATUS_BAR_H + 20; wy < 130; wy += 30) {
                int sx = (int)(wx - cam_x);
                if (sx >= 0 && sx < WORLD_SCREEN_W) {
                    gfx_draw_pixel(fb, sx, wy, 0x73AE);
                }
            }
        }
    }

    // Baseboard
    gfx_fill_rect(fb, 0, 138, WORLD_SCREEN_W, 4, 0x8CD1);

    // Left End Wall (World X: 0)
    int left_sx = (int)(0 - cam_x);
    if (left_sx >= -15 && left_sx < WORLD_SCREEN_W) {
        gfx_fill_rect(fb, left_sx, WORLD_STATUS_BAR_H, 12, WORLD_SCREEN_H - WORLD_STATUS_BAR_H, 0x8410);
        gfx_fill_rect(fb, left_sx + 10, WORLD_STATUS_BAR_H, 2, WORLD_SCREEN_H - WORLD_STATUS_BAR_H, 0x630C);
    }

    // Right End Wall (World X: 948)
    int right_sx = (int)(948 - cam_x);
    if (right_sx < WORLD_SCREEN_W) {
        gfx_fill_rect(fb, right_sx, WORLD_STATUS_BAR_H, 12, WORLD_SCREEN_H - WORLD_STATUS_BAR_H, 0x18C3);
        gfx_fill_rect(fb, right_sx - 2, WORLD_STATUS_BAR_H, 2, WORLD_SCREEN_H - WORLD_STATUS_BAR_H, 0x0841);
    }
}

static void draw_floors(uint16_t *fb, float cam_x)
{
    int floor_y = 142;
    int floor_h = WORLD_SCREEN_H - floor_y;

    // 1. Kitchen Checkered Floor (0..320)
    int kf_sx = (int)(0 - cam_x);
    if (kf_sx + 320 > 0 && kf_sx < WORLD_SCREEN_W) {
        int start_col = kf_sx < 0 ? (-kf_sx / 20) : 0;
        int end_col = (WORLD_SCREEN_W - kf_sx) / 20 + 1;
        if (end_col > 16) end_col = 16;
        for (int col = start_col; col < end_col; col++) {
            int tx = kf_sx + col * 20;
            for (int ty = floor_y; ty < WORLD_SCREEN_H; ty += 20) {
                uint16_t c = ((col + (ty / 20)) % 2 == 0) ? COLOR_KITCHEN_TILE1 : COLOR_KITCHEN_TILE2;
                gfx_fill_rect(fb, tx, ty, 20, 20, c);
            }
        }
    }

    // 2. Living Room Oak Planks (320..640)
    int lf_sx = (int)(320 - cam_x);
    if (lf_sx + 320 > 0 && lf_sx < WORLD_SCREEN_W) {
        int c_left = lf_sx < 0 ? 0 : lf_sx;
        int c_right = (lf_sx + 320) > WORLD_SCREEN_W ? WORLD_SCREEN_W : (lf_sx + 320);
        gfx_fill_rect(fb, c_left, floor_y, c_right - c_left, floor_h, COLOR_LIVING_FLOOR);
        for (int y = floor_y; y < WORLD_SCREEN_H; y += 18) {
            gfx_draw_fast_hline(fb, c_left, y, c_right - c_left, 0xAB11);
        }
    }

    // 3. Bedroom Plush Carpet (640..960)
    int bf_sx = (int)(640 - cam_x);
    if (bf_sx + 320 > 0 && bf_sx < WORLD_SCREEN_W) {
        int c_left = bf_sx < 0 ? 0 : bf_sx;
        int c_right = (bf_sx + 320) > WORLD_SCREEN_W ? WORLD_SCREEN_W : (bf_sx + 320);
        gfx_fill_rect(fb, c_left, floor_y, c_right - c_left, floor_h, COLOR_BED_FLOOR);
    }

    // Archway 1 (Kitchen <-> Living Room at x=320)
    int arch1_sx = (int)(320 - cam_x);
    if (arch1_sx >= -20 && arch1_sx < WORLD_SCREEN_W + 20) {
        gfx_fill_rect(fb, arch1_sx - 4, floor_y, 8, floor_h, 0x8A22);
        gfx_fill_rect(fb, arch1_sx - 5, WORLD_STATUS_BAR_H, 10, floor_y - WORLD_STATUS_BAR_H, 0xB575);
        gfx_draw_rect(fb, arch1_sx - 5, WORLD_STATUS_BAR_H, 10, floor_y - WORLD_STATUS_BAR_H, 0x73AE);
        gfx_fill_rect(fb, arch1_sx - 18, WORLD_STATUS_BAR_H, 36, 8, 0x9492);
    }

    // Archway 2 (Living Room <-> Bedroom at x=640)
    int arch2_sx = (int)(640 - cam_x);
    if (arch2_sx >= -20 && arch2_sx < WORLD_SCREEN_W + 20) {
        gfx_fill_rect(fb, arch2_sx - 4, floor_y, 8, floor_h, 0x6960);
        gfx_fill_rect(fb, arch2_sx - 5, WORLD_STATUS_BAR_H, 10, floor_y - WORLD_STATUS_BAR_H, 0x6185);
        gfx_fill_rect(fb, arch2_sx - 18, WORLD_STATUS_BAR_H, 36, 8, 0x5144);
        gfx_fill_round_rect(fb, arch2_sx - 12, WORLD_STATUS_BAR_H + 8, 7, 60, 3, 0x9334);
        gfx_fill_round_rect(fb, arch2_sx + 5, WORLD_STATUS_BAR_H + 8, 7, 60, 3, 0x9334);
    }
}

static void draw_props(uint16_t *fb, float cam_x, int hour, int food_amount, uint32_t now_ms)
{
    // KITCHEN: Fridge at x=35
    int fx = (int)(35 - cam_x);
    int fy = 68;
    if (fx > -60 && fx < WORLD_SCREEN_W) {
        gfx_fill_round_rect(fb, fx, fy, 48, 112, 5, COLOR_FRIDGE);
        gfx_draw_round_rect(fb, fx, fy, 48, 112, 5, 0x7BEF);
        gfx_draw_fast_hline(fb, fx, fy + 42, 48, 0x7BEF);
        gfx_fill_round_rect(fb, fx + 40, fy + 16, 4, 18, 2, 0x4208);
        gfx_fill_round_rect(fb, fx + 40, fy + 52, 4, 26, 2, 0x4208);
        gfx_fill_rect(fb, fx + 10, fy + 12, 10, 10, GFX_YELLOW);
        gfx_fill_rect(fb, fx + 24, fy + 18, 10, 8, COLOR_HEART_PINK);
    }

    // KITCHEN: Counter & Stove at x=175
    int cx = (int)(175 - cam_x);
    int cy = 110;
    if (cx > -120 && cx < WORLD_SCREEN_W) {
        gfx_fill_round_rect(fb, cx, cy, 105, 65, 4, COLOR_COUNTER);
        gfx_fill_round_rect(fb, cx - 4, cy, 113, 8, 3, 0xE71C);
        fill_ellipse(fb, cx + 25, cy + 5, 12, 4, 0x3186);
        fill_ellipse(fb, cx + 25, cy + 2, 10, 3, 0x18C3);
        int steam_t = (now_ms / 250) % 4;
        gfx_fill_circle(fb, cx + 26, cy - 8 - steam_t * 3, 2 + steam_t, 0xEF7D);
        gfx_draw_fast_hline(fb, cx + 10, cy - 25, 85, 0x8410);
        gfx_fill_circle(fb, cx + 25, cy - 25, 2, 0x528A);
        gfx_fill_circle(fb, cx + 55, cy - 25, 2, 0x528A);
        gfx_fill_circle(fb, cx + 80, cy - 25, 2, 0x528A);
    }

    // KITCHEN: Food Bowl at x=115, y=180
    int bx = (int)(115 - cam_x);
    int by = 180;
    if (bx > -30 && bx < WORLD_SCREEN_W + 30) {
        fill_ellipse(fb, bx, by + 12, 18, 6, 0xB575);
        fill_ellipse(fb, bx, by + 8, 16, 7, 0xFD20);
        fill_ellipse(fb, bx, by + 4, 15, 6, 0xFFE0);
        if (food_amount > 0) {
            int food_h = 2 + food_amount * 4 / 100;
            fill_ellipse(fb, bx, by + 4, 12, food_h, 0x9300);
            if (food_amount > 50) {
                gfx_fill_circle(fb, bx - 4, by + 2, 1, GFX_WHITE);
                gfx_fill_circle(fb, bx + 5, by + 3, 1, GFX_WHITE);
            }
        }
        gfx_fill_circle(fb, bx, by + 8, 2, GFX_WHITE);
    }

    // LIVING ROOM: Sofa at x=365
    int sx = (int)(365 - cam_x);
    int sy = 120;
    if (sx > -90 && sx < WORLD_SCREEN_W + 20) {
        gfx_fill_round_rect(fb, sx, sy, 70, 42, 8, COLOR_LIVING_COUCH);
        gfx_fill_round_rect(fb, sx - 4, sy + 25, 78, 20, 6, 0x5CDA);
        gfx_fill_round_rect(fb, sx - 8, sy + 18, 12, 28, 4, COLOR_LIVING_COUCH);
        gfx_fill_round_rect(fb, sx + 66, sy + 18, 12, 28, 4, COLOR_LIVING_COUCH);
        gfx_fill_round_rect(fb, sx + 10, sy + 20, 16, 16, 4, COLOR_LIVING_CUSHION);
    }

    // LIVING ROOM: Scenic Picture Window at x=480
    int wx = (int)(480 - 45 - cam_x);
    int wy = 44;
    int ww = 90;
    int wh = 74;
    if (wx > -ww && wx < WORLD_SCREEN_W) {
        bool is_night = (hour >= 21 || hour < 6);
        bool is_sunset = (hour >= 18 && hour < 21);
        bool is_morning = (hour >= 6 && hour < 9);
        uint16_t sky_col = is_night ? 0x10C8 : is_sunset ? 0xEAA9 : is_morning ? 0xFCE8 : 0x6E5F;

        gfx_fill_round_rect(fb, wx, wy, ww, wh, 6, sky_col);
        if (is_night) {
            gfx_fill_circle(fb, wx + 22, wy + 20, 8, GFX_YELLOW);
            gfx_fill_circle(fb, wx + 26, wy + 18, 7, sky_col);
            gfx_fill_circle(fb, wx + 52, wy + 16, 1, GFX_WHITE);
            gfx_fill_circle(fb, wx + 72, wy + 26, 1, GFX_WHITE);
            gfx_fill_circle(fb, wx + 38, wy + 42, 1, GFX_WHITE);
        } else {
            gfx_fill_circle(fb, wx + 24, wy + 22, 10, 0xFE40);
            gfx_fill_circle(fb, wx + 60, wy + 28, 9, GFX_WHITE);
            gfx_fill_circle(fb, wx + 70, wy + 26, 11, GFX_WHITE);
            gfx_fill_circle(fb, wx + 78, wy + 30, 8, GFX_WHITE);
        }
        gfx_draw_round_rect(fb, wx, wy, ww, wh, 6, 0xFFFF);
        gfx_draw_fast_vline(fb, wx + ww / 2, wy, wh, 0xFFFF);
        gfx_draw_fast_hline(fb, wx, wy + wh / 2, ww, 0xFFFF);
        gfx_fill_round_rect(fb, wx - 4, wy + wh - 2, ww + 8, 6, 2, 0xD6BA);
    }

    // LIVING ROOM: Oval Rug at x=480, y=190
    int rug_sx = (int)(480 - cam_x);
    if (rug_sx > -80 && rug_sx < WORLD_SCREEN_W + 80) {
        fill_ellipse(fb, rug_sx, 190, 75, 26, 0xDE74);
        fill_ellipse(fb, rug_sx, 190, 68, 22, 0xF717);
        draw_ellipse(fb, rug_sx, 190, 60, 18, 0xCE52);
    }

    // LIVING ROOM: Potted Plant at x=595
    int px = (int)(595 - cam_x);
    int py = 150;
    if (px > -25 && px < WORLD_SCREEN_W + 25) {
        fill_triangle(fb, px - 10, py, px + 10, py, px + 7, py + 18, 0xD3A6);
        fill_triangle(fb, px - 10, py, px + 7, py + 18, px - 7, py + 18, 0xD3A6);
        gfx_fill_round_rect(fb, px - 12, py - 3, 24, 4, 2, 0xE488);
        fill_ellipse(fb, px - 7, py - 12, 7, 12, 0x24C6);
        fill_ellipse(fb, px + 7, py - 10, 8, 11, 0x2DC7);
        fill_ellipse(fb, px, py - 16, 7, 13, 0x3DC8);
    }

    // BEDROOM: Nightstand with Lamp at x=710
    int nx = (int)(710 - cam_x);
    int ny = 125;
    if (nx > -45 && nx < WORLD_SCREEN_W + 45) {
        gfx_fill_round_rect(fb, nx, ny + 15, 36, 40, 3, COLOR_BED_FRAME);
        gfx_fill_rect(fb, nx + 4, ny + 22, 28, 12, 0x6960);
        gfx_fill_circle(fb, nx + 18, ny + 28, 2, 0xDE74);
        gfx_fill_round_rect(fb, nx + 14, ny + 4, 8, 12, 2, 0xDE74);
        fill_triangle(fb, nx + 6, ny + 6, nx + 30, ny + 6, nx + 25, ny - 14, 0xFFE0);
        fill_triangle(fb, nx + 6, ny + 6, nx + 25, ny - 14, nx + 11, ny - 14, 0xFFE0);
        gfx_fill_circle(fb, nx + 18, ny - 5, 22, 0x3A22); // subtle glow
    }

    // BEDROOM: Bed at x=770..890
    int bx2 = (int)(770 - cam_x);
    int by2 = 125;
    if (bx2 > -140 && bx2 < WORLD_SCREEN_W + 30) {
        gfx_fill_round_rect(fb, bx2 + 110, by2 - 20, 16, 75, 4, COLOR_BED_FRAME);
        gfx_fill_round_rect(fb, bx2, by2 + 30, 120, 24, 4, COLOR_BED_FRAME);
        gfx_fill_round_rect(fb, bx2 + 4, by2 + 10, 110, 32, 6, COLOR_BED_SHEET);
        gfx_fill_round_rect(fb, bx2 + 4, by2 + 20, 75, 26, 6, COLOR_BED_BLANKET);
        gfx_fill_round_rect(fb, bx2 + 78, by2 + 12, 28, 18, 6, GFX_WHITE);
        gfx_draw_round_rect(fb, bx2 + 78, by2 + 12, 28, 18, 6, 0xCE79);
    }

    // BEDROOM: Wall Painting at x=835
    int pic_x = (int)(835 - cam_x);
    if (pic_x > -40 && pic_x < WORLD_SCREEN_W + 40) {
        gfx_fill_rect(fb, pic_x, WORLD_STATUS_BAR_H + 20, 30, 24, 0x8A22);
        gfx_fill_rect(fb, pic_x + 3, WORLD_STATUS_BAR_H + 23, 24, 18, 0xFD20);
        gfx_fill_circle(fb, pic_x + 15, WORLD_STATUS_BAR_H + 32, 4, GFX_YELLOW);
    }
}

static void draw_toy_ball(uint16_t *fb, const toy_ball_t *ball, float cam_x)
{
    int sx = (int)(ball->x - cam_x);
    int sy = (int)ball->y;
    if (sx < -20 || sx > WORLD_SCREEN_W + 20) return;

    fill_ellipse(fb, sx, sy + 7, 8, 3, 0x9492);
    gfx_fill_circle(fb, sx, sy, (int)ball->radius, GFX_RED);
    // Yellow & Cyan accents
    gfx_fill_circle(fb, sx - 2, sy - 2, 4, GFX_YELLOW);
    gfx_fill_circle(fb, sx + 2, sy + 2, 4, GFX_CYAN);
    gfx_fill_circle(fb, sx - 3, sy - 3, 2, GFX_WHITE); // highlight
}

// --- Status Bar & Mini-Map Radar ---

static void draw_status_bar(uint16_t *fb, const pet_world_t *w)
{
    gfx_fill_rect(fb, 0, 0, WORLD_SCREEN_W, WORLD_STATUS_BAR_H, COLOR_BG_TOPBAR);
    gfx_draw_fast_hline(fb, 0, WORLD_STATUS_BAR_H - 1, WORLD_SCREEN_W, 0x39E7);

    // 0. App Menu Button (x = 3..21)
    gfx_fill_round_rect(fb, 3, 4, 18, 18, 3, 0x18C3);
    gfx_draw_round_rect(fb, 3, 4, 18, 18, 3, 0x05BF);
    gfx_draw_fast_hline(fb, 6, 8, 12, GFX_WHITE);
    gfx_draw_fast_hline(fb, 6, 12, 12, GFX_WHITE);
    gfx_draw_fast_hline(fb, 6, 16, 12, GFX_WHITE);

    // 1. Clock & Sun/Moon (x = 28)
    bool is_night = (w->cur_hour >= 21 || w->cur_hour < 6);
    int icon_x = 28;
    int icon_y = 12;
    if (is_night) {
        gfx_fill_circle(fb, icon_x, icon_y, 3, GFX_YELLOW);
        gfx_fill_circle(fb, icon_x + 2, icon_y - 2, 2, COLOR_BG_TOPBAR);
    } else {
        gfx_fill_circle(fb, icon_x, icon_y, 2, 0xFE40);
        gfx_draw_pixel(fb, icon_x - 3, icon_y, 0xFE40);
        gfx_draw_pixel(fb, icon_x + 3, icon_y, 0xFE40);
        gfx_draw_pixel(fb, icon_x, icon_y - 3, 0xFE40);
        gfx_draw_pixel(fb, icon_x, icon_y + 3, 0xFE40);
    }
    char time_str[10];
    snprintf(time_str, sizeof(time_str), "%02d:%02d", w->cur_hour, w->cur_min);
    gfx_draw_string(fb, 36, 9, time_str, GFX_WHITE);

    // 2. Mini-Map Radar (x = 62..176, w = 114)
    int map_x = 62;
    int map_y = 6;
    int map_w = 114;
    int map_h = 14;
    gfx_fill_round_rect(fb, map_x, map_y, map_w, map_h, 3, 0x18C3);
    gfx_draw_round_rect(fb, map_x, map_y, map_w, map_h, 3, 0x4A69);

    int sep1 = map_x + map_w / 3;
    int sep2 = map_x + (map_w * 2) / 3;
    gfx_draw_fast_vline(fb, sep1, map_y + 2, map_h - 4, 0x31A6);
    gfx_draw_fast_vline(fb, sep2, map_y + 2, map_h - 4, 0x31A6);

    gfx_draw_char(fb, map_x + map_w / 6 - 2, map_y + 4, 'K', 0x73AE);
    gfx_draw_char(fb, map_x + map_w / 2 - 2, map_y + 4, 'L', 0x73AE);
    gfx_draw_char(fb, map_x + (map_w * 5) / 6 - 2, map_y + 4, 'B', 0x73AE);

    // Sliding Viewport Window Indicator
    int vp_w = map_w / 3;
    int vp_x = map_x + (int)((w->cam_x / 640.0f) * (map_w - vp_w));
    if (vp_x < map_x) vp_x = map_x;
    if (vp_x > map_x + map_w - vp_w) vp_x = map_x + map_w - vp_w;
    gfx_draw_round_rect(fb, vp_x, map_y, vp_w, map_h, 3, 0x5D3F);
    gfx_draw_round_rect(fb, vp_x + 1, map_y + 1, vp_w - 2, map_h - 2, 2, GFX_WHITE);

    // Continuous Pet Marker Dot on Mini-Map
    int pet_dot_x = map_x + (int)((w->pet_x / (float)WORLD_W) * map_w);
    if (pet_dot_x < map_x + 2) pet_dot_x = map_x + 2;
    if (pet_dot_x > map_x + map_w - 2) pet_dot_x = map_x + map_w - 2;
    gfx_fill_circle(fb, pet_dot_x, map_y + map_h / 2, 3, COLOR_MUSE_BLUE_LIGHT);
    gfx_draw_circle(fb, pet_dot_x, map_y + map_h / 2, 3, GFX_WHITE);

    // 3. Three Vital Stat Gauges (x = 184..275)
    // Fullness 🍖
    int fullness = 100 - w->hunger;
    if (fullness < 0) fullness = 0;
    gfx_fill_circle(fb, 187, 11, 2, 0xFD20);
    gfx_draw_fast_hline(fb, 185, 13, 5, 0x9300);
    gfx_draw_rect(fb, 193, 9, 16, 7, 0x4A69);
    int bar1 = fullness * 14 / 100;
    uint16_t c1 = fullness > 50 ? 0x24C6 : (fullness > 25 ? 0xFE40 : GFX_RED);
    if (bar1 > 0) gfx_fill_rect(fb, 194, 10, bar1, 5, c1);

    // Joy 💖
    gfx_fill_circle(fb, 218, 11, 2, COLOR_HEART_PINK);
    gfx_fill_circle(fb, 221, 11, 2, COLOR_HEART_PINK);
    fill_triangle(fb, 216, 12, 223, 12, 219, 15, COLOR_HEART_PINK);
    gfx_draw_rect(fb, 226, 9, 16, 7, 0x4A69);
    int bar2 = w->happiness * 14 / 100;
    if (bar2 > 0) gfx_fill_rect(fb, 227, 10, bar2, 5, COLOR_HEART_PINK);

    // Energy ⚡
    gfx_draw_pixel(fb, 252, 9, GFX_YELLOW);
    gfx_draw_pixel(fb, 251, 10, GFX_YELLOW);
    gfx_draw_pixel(fb, 250, 11, GFX_YELLOW);
    gfx_draw_pixel(fb, 252, 12, GFX_YELLOW);
    gfx_draw_pixel(fb, 251, 13, GFX_YELLOW);
    gfx_draw_pixel(fb, 250, 14, GFX_YELLOW);
    gfx_draw_rect(fb, 258, 9, 16, 7, 0x4A69);
    int bar3 = w->energy * 14 / 100;
    if (bar3 > 0) gfx_fill_rect(fb, 259, 10, bar3, 5, 0x07FF);

    // WiFi & Cloud Dot (x = 280)
    gfx_fill_circle(fb, 280, 12, 3, w->wifi_connected ? 0x07FF : 0x528A);
    if (w->muse_connected) {
        gfx_fill_circle(fb, 280, 7, 2, GFX_YELLOW);
    }

    // Battery (x = 292)
    gfx_draw_rect(fb, 292, 8, 18, 9, 0x9CD3);
    gfx_fill_rect(fb, 310, 10, 2, 5, 0x9CD3);
    /* An unknown level (no battery reading) leaves the icon empty. */
    int b_pct = w->battery_pct < 0 ? 0 : (w->battery_pct > 100 ? 100 : w->battery_pct);
    int b_w = b_pct * 14 / 100;
    if (b_w > 0) gfx_fill_rect(fb, 294, 10, b_w, 5, b_pct > 20 ? 0x24C6 : GFX_RED);
}

// --- Speech & Thought Bubbles ---

static void draw_speech_bubble(uint16_t *fb, int sx, int sy, const char *text)
{
    if (!text || !text[0]) return;

    // Word wrap up to 3 lines (max 36 chars per line)
    char lines[3][40];
    int line_count = 0;
    const char *p = text;
    int max_line_len = 0;

    while (*p && line_count < 3) {
        while (*p == ' ') p++;
        if (!*p) break;

        int line_len = 0;
        int last_space = -1;
        const char *start = p;

        while (*p && line_len < 36) {
            if (*p == ' ') last_space = line_len;
            line_len++;
            p++;
        }

        int copy_len = line_len;
        if (*p && line_len >= 36) {
            if (last_space > 0) {
                copy_len = last_space;
                p = start + last_space + 1;
            }
        }

        if (copy_len > 38) copy_len = 38;
        memcpy(lines[line_count], start, copy_len);
        lines[line_count][copy_len] = '\0';
        if (copy_len > max_line_len) max_line_len = copy_len;
        line_count++;
    }

    if (line_count == 0) return;

    int bw = max_line_len * 6 + 18;
    if (bw < 60) bw = 60;
    if (bw > 260) bw = 260;
    int bh = 14 + line_count * 10;
    int bx = sx - bw / 2;
    if (bx < 8) bx = 8;
    if (bx + bw > WORLD_SCREEN_W - 8) bx = WORLD_SCREEN_W - 8 - bw;
    int by = sy - bh - 8;
    if (by < WORLD_STATUS_BAR_H + 4) by = WORLD_STATUS_BAR_H + 4;

    gfx_fill_round_rect(fb, bx, by, bw, bh, 6, GFX_WHITE);
    gfx_draw_round_rect(fb, bx, by, bw, bh, 6, 0x39E7);

    // Pointer tail
    fill_triangle(fb, sx - 4, by + bh, sx + 4, by + bh, sx, by + bh + 6, GFX_WHITE);
    gfx_draw_pixel(fb, sx - 4, by + bh, 0x39E7);
    gfx_draw_pixel(fb, sx + 4, by + bh, 0x39E7);

    // Render lines
    for (int l = 0; l < line_count; l++) {
        gfx_draw_string(fb, bx + 9, by + 6 + l * 10, lines[l], 0x18C3);
    }
}

// --- Main Render Function ---

void pet_world_init(void)
{
    if (!s_fb) {
        s_fb = (uint16_t *)heap_caps_malloc(WORLD_SCREEN_W * WORLD_SCREEN_H * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_fb) {
            ESP_LOGE(TAG, "Failed to allocate 320x240 frame buffer in PSRAM!");
            return;
        }
        memset(s_fb, 0, WORLD_SCREEN_W * WORLD_SCREEN_H * sizeof(uint16_t));
    }

    memset(&s_world, 0, sizeof(s_world));
    s_world.cam_x = 320.0f; // Start centered on Living Room
    s_world.cam_vx = 0.0f;
    s_world.pet_x = 480.0f; // Center of Living Room
    s_world.pet_y = 180.0f;
    s_world.facing_right = true;
    s_world.state = PET_ST_IDLE;
    s_world.goal = PET_GOAL_NONE;
    s_world.hunger = 35;
    s_world.energy = 85;
    s_world.happiness = 80;
    s_world.food_amount = 80;
    s_world.cur_hour = 14;
    s_world.cur_min = 30;
    s_world.battery_pct = -1;   /* unknown until the first status check */
    s_world.wifi_connected = false;
    s_world.muse_connected = false;

    s_world.ball.x = 540.0f;
    s_world.ball.y = 195.0f;
    s_world.ball.radius = 9.0f;

    ESP_LOGI(TAG, "Pet World initialized successfully (FB in PSRAM: %p)", s_fb);
}

uint16_t *pet_world_get_fb(void)
{
    return s_fb;
}

void pet_world_update(uint32_t now_ms, int muse_mode, float audio_level, const char *caption)
{
    // Update ball physics
    toy_ball_t *b = &s_world.ball;
    b->x += b->vx;
    b->y += b->vy;
    b->vx *= 0.94f;
    if (fabsf(b->vx) < 0.05f) b->vx = 0.0f;
    if (b->x < 350.0f) { b->x = 350.0f; b->vx = -b->vx * 0.6f; }
    if (b->x > 610.0f) { b->x = 610.0f; b->vx = -b->vx * 0.6f; }
    if (b->y < FLOOR_Y_MIN) { b->y = FLOOR_Y_MIN; b->vy = -b->vy * 0.5f; }
    if (b->y > FLOOR_Y_MAX) { b->y = FLOOR_Y_MAX; b->vy = 0.0f; }

    // Inertial camera gliding
    if (!s_world.is_swiping) {
        s_world.cam_x += s_world.cam_vx;
        s_world.cam_vx *= 0.88f;
        if (fabsf(s_world.cam_vx) < 0.1f) s_world.cam_vx = 0.0f;
    }
    if (s_world.cam_x < 0.0f) s_world.cam_x = 0.0f;
    if (s_world.cam_x > 640.0f) s_world.cam_x = 640.0f;

    // Handle Muse Assistant state overrides
    s_world.audio_level = audio_level;
    if (s_world.is_talk_pressed || muse_mode == MUSE_MODE_LISTENING) {
        s_world.state = PET_ST_LISTENING;
        snprintf(s_world.speech_text, sizeof(s_world.speech_text), "Listening...");
        s_world.speech_timer = now_ms + 10000;
    } else if (muse_mode == MUSE_MODE_THINKING) {
        s_world.state = PET_ST_THINKING;
        if (caption && caption[0] && strcmp(caption, "SENDING VOICE NOTE") != 0 && strcmp(caption, "THINKING") != 0) {
            snprintf(s_world.speech_text, sizeof(s_world.speech_text), "%s", caption);
        } else {
            snprintf(s_world.speech_text, sizeof(s_world.speech_text), "Thinking...");
        }
        s_world.speech_timer = now_ms + 10000;
    } else if (muse_mode == MUSE_MODE_SPEAKING) {
        s_world.state = PET_ST_SPEAKING;
        if (caption && caption[0]) {
            snprintf(s_world.speech_text, sizeof(s_world.speech_text), "%s", caption);
        } else {
            snprintf(s_world.speech_text, sizeof(s_world.speech_text), "Speaking...");
        }
        s_world.speech_timer = now_ms + 6000; // Keep visible for 6 seconds after speaking ends
    } else {
        if (s_world.state == PET_ST_LISTENING || s_world.state == PET_ST_THINKING || s_world.state == PET_ST_SPEAKING) {
            s_world.state = PET_ST_IDLE;
            if (caption && caption[0] && strcmp(caption, "SENDING VOICE NOTE") != 0 && strcmp(caption, "THINKING") != 0) {
                snprintf(s_world.speech_text, sizeof(s_world.speech_text), "%s", caption);
                if (s_world.speech_timer < now_ms + 5000) {
                    s_world.speech_timer = now_ms + 5000;
                }
            } else {
                s_world.speech_text[0] = '\0';
                s_world.speech_timer = 0;
            }
        }
        if (now_ms > s_world.speech_timer) {
            s_world.speech_text[0] = '\0';
        }
    }

    // Auto-center camera on pet while interacting with voice assistant
    if (s_world.state == PET_ST_LISTENING || s_world.state == PET_ST_THINKING || s_world.state == PET_ST_SPEAKING) {
        float target_cam = s_world.pet_x - (WORLD_SCREEN_W / 2.0f);
        if (target_cam < 0.0f) target_cam = 0.0f;
        if (target_cam > 640.0f) target_cam = 640.0f;
        s_world.cam_x += (target_cam - s_world.cam_x) * 0.15f;
    }

    // Update live indicators every 500ms
    static uint32_t last_status_check = 0;
    if (now_ms - last_status_check > 500) {
        last_status_check = now_ms;
        muse_wifi_status_t w;
        muse_wifi_status(&w);
        s_world.wifi_connected = (w.state == MUSE_WIFI_CONNECTED);

        muse_hatch_status_t link;
        muse_hatch_status(&link);
        s_world.muse_connected = (link.state == MUSE_HATCH_REACHABLE);

        muse_power_t p = muse_state_power();
        s_world.battery_pct = p.battery_pct;

        time_t now_sec = time(NULL);
        struct tm tm_info;
        localtime_r(&now_sec, &tm_info);
        if (tm_info.tm_year >= 120) {
            s_world.cur_hour = tm_info.tm_hour;
            s_world.cur_min = tm_info.tm_min;
        }
    }

    // Needs drift slowly: hungrier while not eating, more tired while awake.
    static uint32_t last_needs_tick = 0;
    if (now_ms - last_needs_tick > 20000) {
        last_needs_tick = now_ms;
        if (s_world.state != PET_ST_EAT && s_world.hunger < 100) s_world.hunger++;
        if (s_world.state != PET_ST_SLEEP && s_world.energy > 0) s_world.energy--;
    }

    // A pat shows the petted animation for a moment before the pet moves on.
    if (s_world.state == PET_ST_PETTED && s_world.petted_frames > 0) {
        s_world.petted_frames--;
    }

    // Autonomous Pet AI (when not interacting with assistant, dragged or being petted)
    if (s_world.state != PET_ST_LISTENING && s_world.state != PET_ST_THINKING &&
        s_world.state != PET_ST_SPEAKING && s_world.state != PET_ST_DRAGGED &&
        !(s_world.state == PET_ST_PETTED && s_world.petted_frames > 0)) {

        if (now_ms > s_world.next_goal_eval) {
            s_world.next_goal_eval = now_ms + 10000 + (rand() % 8000);
            int roll = rand() % 100;
            if (s_world.hunger > 60 && s_world.food_amount > 0) {
                s_world.goal = PET_GOAL_HUNGER;
            } else if (roll < 30) {
                s_world.goal = PET_GOAL_PLAY_BALL;
            } else if (roll < 55) {
                s_world.goal = PET_GOAL_COUCH_RELAX;
            } else if (roll < 75) {
                s_world.goal = PET_GOAL_WINDOW_GAZE;
            } else if (s_world.energy < 30) {
                s_world.goal = PET_GOAL_BEDTIME;
            } else {
                s_world.goal = PET_GOAL_NONE;
            }
        }

        // Execute current goal
        float target_x = s_world.pet_x;
        float target_y = 180.0f;
        if (s_world.goal == PET_GOAL_HUNGER) {
            target_x = 115.0f; // Food bowl
        } else if (s_world.goal == PET_GOAL_PLAY_BALL) {
            target_x = s_world.ball.x;
        } else if (s_world.goal == PET_GOAL_COUCH_RELAX) {
            target_x = 380.0f; target_y = 168.0f;
        } else if (s_world.goal == PET_GOAL_WINDOW_GAZE) {
            target_x = 480.0f;
        } else if (s_world.goal == PET_GOAL_BEDTIME) {
            target_x = 810.0f; target_y = 152.0f;
        }

        float dx = target_x - s_world.pet_x;
        if (fabsf(dx) > 6.0f) {
            s_world.state = PET_ST_WALK;
            s_world.facing_right = (dx > 0);
            float speed = 1.6f;
            s_world.pet_x += (dx > 0 ? speed : -speed);
            s_world.walk_cycle += 0.25f;
        } else {
            // Reached destination
            if (s_world.goal == PET_GOAL_HUNGER) {
                s_world.state = PET_ST_EAT;
                if (s_world.food_amount > 0) s_world.food_amount--;
                if (s_world.hunger > 10) s_world.hunger--;
            } else if (s_world.goal == PET_GOAL_BEDTIME) {
                s_world.state = PET_ST_SLEEP;
                if (s_world.energy < 100) s_world.energy++;
            } else if (s_world.goal == PET_GOAL_PLAY_BALL) {
                s_world.state = PET_ST_PLAY;
                s_world.ball.vx = s_world.facing_right ? 3.5f : -3.5f;
                s_world.goal = PET_GOAL_NONE;
            } else {
                s_world.state = PET_ST_IDLE;
            }
        }
        s_world.pet_y += (target_y - s_world.pet_y) * 0.1f;
    }

    // Camera auto-follow if pet walks off screen
    float pet_screen_x = s_world.pet_x - s_world.cam_x;
    if (pet_screen_x < 40.0f) {
        s_world.cam_x -= (40.0f - pet_screen_x) * 0.05f;
    } else if (pet_screen_x > WORLD_SCREEN_W - 40.0f) {
        s_world.cam_x += (pet_screen_x - (WORLD_SCREEN_W - 40.0f)) * 0.05f;
    }
}

void pet_world_touch_down(int tx, int ty)
{
    s_world.touch_start_x = tx;
    s_world.touch_start_y = ty;
    s_world.last_touch_x = tx;
    s_world.is_swiping = false;
    s_world.is_touching_pet = false;

    // Check tap on status bar (y < 26)
    if (ty < WORLD_STATUS_BAR_H) {
        if (tx < 28) {
            s_world.menu_pressed = true;   /* opens the launcher on release */
            return;
        }
        if (tx >= 62 && tx <= 176) {
            float target = 320.0f;
            if (tx < 100) target = 0.0f;
            else if (tx < 138) target = 320.0f;
            else target = 640.0f;
            s_world.cam_vx = (target - s_world.cam_x) * 0.25f;
            return;
        }
        return;
    }

    // Check tap on pet
    float pet_sx = s_world.pet_x - s_world.cam_x;
    if (fabsf(tx - pet_sx) < 32.0f && fabsf(ty - s_world.pet_y) < 36.0f) {
        s_world.is_touching_pet = true;
        return;
    }

    // Check tap on food bowl in Kitchen (world x = 115)
    float world_tx = s_world.cam_x + tx;
    if (fabsf(world_tx - 115.0f) < 26.0f && fabsf(ty - 180.0f) < 20.0f) {
        s_world.food_amount = 100;
        return;
    }

    // Check tap on toy ball
    if (fabsf(world_tx - s_world.ball.x) < 25.0f && fabsf(ty - s_world.ball.y) < 25.0f) {
        s_world.ball.vx = (tx < (s_world.ball.x - s_world.cam_x)) ? 4.5f : -4.5f;
        s_world.ball.vy = -3.0f;
        return;
    }
}

void pet_world_touch_move(int tx, int ty)
{
    if (s_world.menu_pressed) {
        if (!(ty < WORLD_STATUS_BAR_H && tx < 28)) {
            s_world.menu_pressed = false;   /* slid off the menu icon */
        }
        return;
    }
    int dx = tx - s_world.last_touch_x;
    int total_dist = abs(tx - s_world.touch_start_x) + abs(ty - s_world.touch_start_y);

    if (s_world.is_touching_pet) {
        if (total_dist > 7) {
            s_world.state = PET_ST_DRAGGED;
            s_world.pet_x = s_world.cam_x + tx;
            if (s_world.pet_x < 36.0f) s_world.pet_x = 36.0f;
            if (s_world.pet_x > WORLD_W - 36.0f) s_world.pet_x = WORLD_W - 36.0f;
            s_world.pet_y = ty;
            if (s_world.pet_y < 80.0f) s_world.pet_y = 80.0f;
            if (s_world.pet_y > FLOOR_Y_MAX) s_world.pet_y = FLOOR_Y_MAX;
            s_world.pet_vx = 0;

            // Auto-scrolling when dragging near edges
            if (tx > WORLD_SCREEN_W - 35 && s_world.cam_x < 640.0f) {
                s_world.cam_x += 3.5f;
            } else if (tx < 35 && s_world.cam_x > 0.0f) {
                s_world.cam_x -= 3.5f;
            }
        }
    } else if (ty >= WORLD_STATUS_BAR_H) {
        if (total_dist > 6) {
            s_world.is_swiping = true;
        }
        if (s_world.is_swiping && dx != 0) {
            s_world.cam_x -= dx;
            s_world.cam_vx = -dx * 0.75f;
            s_world.last_touch_x = tx;
        }
    }
}

void pet_world_touch_up(void)
{
    if (s_world.menu_pressed) {
        s_world.menu_pressed = false;
        app_manager_open_launcher();
        return;
    }
    if (s_world.is_touching_pet) {
        if (s_world.state == PET_ST_DRAGGED) {
            s_world.state = PET_ST_IDLE;
            if (fabsf(s_world.pet_x - 115.0f) < 35.0f) {
                s_world.pet_y = 180.0f;
                s_world.goal = PET_GOAL_HUNGER;
            } else if (fabsf(s_world.pet_x - 380.0f) < 40.0f) {
                s_world.pet_y = 168.0f;
                s_world.goal = PET_GOAL_COUCH_RELAX;
            } else if (fabsf(s_world.pet_x - 810.0f) < 45.0f) {
                s_world.pet_y = 152.0f;
                s_world.goal = PET_GOAL_BEDTIME;
            } else {
                s_world.pet_y = 180.0f;
                s_world.goal = PET_GOAL_NONE;
            }
        } else {
            // Tapped pet -> pet him!
            s_world.state = PET_ST_PETTED;
            s_world.petted_frames = 30;   /* about 1.2 s at 40 ms frames */
            s_world.happiness = (s_world.happiness < 95) ? s_world.happiness + 5 : 100;
        }
        s_world.is_touching_pet = false;
    }

    s_world.is_swiping = false;
}

void pet_world_touch_cancel(void)
{
    /* The press was lost: no tap, no pat. A dragged pet lands where it is. */
    s_world.menu_pressed = false;
    if (s_world.is_touching_pet && s_world.state == PET_ST_DRAGGED) {
        s_world.state = PET_ST_IDLE;
        s_world.pet_y = 180.0f;
        s_world.goal = PET_GOAL_NONE;
    }
    s_world.is_touching_pet = false;
    s_world.is_swiping = false;
}

static void draw_talk_indicator(uint16_t *fb, uint32_t now_ms, const pet_world_t *w)
{
    int pill_x = 95;
    int pill_y = 212;
    int pill_w = 130;
    int pill_h = 24;
    int r = 6;

    if (w->is_talk_pressed || w->state == PET_ST_LISTENING) {
        // Active recording / listening state: pulsing bright crimson
        uint16_t bg = (now_ms % 600 < 300) ? 0x9800 : 0x7800;
        gfx_fill_round_rect(fb, pill_x, pill_y, pill_w, pill_h, r, bg);
        gfx_draw_round_rect(fb, pill_x, pill_y, pill_w, pill_h, r, 0xF800); // Bright red border
        gfx_draw_round_rect(fb, pill_x - 1, pill_y - 1, pill_w + 2, pill_h + 2, r + 1, 0xFD20); // Outer glow

        // Blinking red recording dot
        int dot_r = (now_ms % 400 < 200) ? 4 : 3;
        gfx_fill_circle(fb, pill_x + 12, pill_y + 12, dot_r, 0xF800);
        gfx_fill_circle(fb, pill_x + 12, pill_y + 12, 1, GFX_WHITE);

        // Text
        gfx_draw_string(fb, pill_x + 22, pill_y + 8, "LISTENING...", GFX_WHITE);

        // Dynamic audio waveform bars
        int lvl = (int)(w->audio_level * 14.0f);
        if (lvl > 10) lvl = 10;
        for (int bi = 0; bi < 3; bi++) {
            int bh = 4 + (lvl * (bi + 1)) / 3;
            if (bh > 14) bh = 14;
            int bx = pill_x + 104 + bi * 5;
            gfx_fill_rect(fb, bx, pill_y + 12 - bh / 2, 3, bh, 0x07FF); // Cyan wave bars
        }
    } else if (w->state == PET_ST_THINKING) {
        // Assistant thinking / processing state
        gfx_fill_round_rect(fb, pill_x + 5, pill_y, pill_w - 10, pill_h, r, 0x2104);
        gfx_draw_round_rect(fb, pill_x + 5, pill_y, pill_w - 10, pill_h, r, 0xFFE0); // Yellow border

        // Pulsing yellow dot
        gfx_fill_circle(fb, pill_x + 16, pill_y + 12, 3, GFX_YELLOW);
        gfx_draw_string(fb, pill_x + 28, pill_y + 8, "THINKING...", GFX_WHITE);
    } else if (w->state == PET_ST_SPEAKING) {
        // Assistant speaking state
        gfx_fill_round_rect(fb, pill_x + 5, pill_y, pill_w - 10, pill_h, r, 0x0320);
        gfx_draw_round_rect(fb, pill_x + 5, pill_y, pill_w - 10, pill_h, r, 0x07E0); // Green border

        // Green audio dot
        gfx_fill_circle(fb, pill_x + 16, pill_y + 12, 3, 0x07E0);
        gfx_draw_string(fb, pill_x + 26, pill_y + 8, "SPEAKING...", GFX_WHITE);
    } else {
        // Idle hint pill above Button B
        gfx_fill_round_rect(fb, pill_x + 10, pill_y + 2, pill_w - 20, pill_h - 4, 4, 0x18C3);
        gfx_draw_round_rect(fb, pill_x + 10, pill_y + 2, pill_w - 20, pill_h - 4, 4, 0x4A69);

        // Small cyan mic dot
        gfx_fill_circle(fb, pill_x + 22, pill_y + 12, 2, 0x07FF);
        gfx_draw_string(fb, pill_x + 30, pill_y + 8, "HOLD B: TALK", 0xCE79);
    }
}

void pet_world_render(uint16_t *fb, uint32_t now_ms)
{
    if (!fb) return;

    float cam_x = s_world.cam_x;

    // 1. Draw 3-room walls and floors
    draw_walls(fb, cam_x);
    draw_floors(fb, cam_x);
    draw_props(fb, cam_x, s_world.cur_hour, s_world.food_amount, now_ms);
    draw_toy_ball(fb, &s_world.ball, cam_x);

    // 2. Draw Pet
    int sx = (int)(s_world.pet_x - cam_x);
    int sy = (int)s_world.pet_y;

    if (sx >= -AVATAR_W && sx <= WORLD_SCREEN_W + AVATAR_W) {
        // Soft ground shadow
        if (s_world.state != PET_ST_SLEEP) {
            fill_ellipse(fb, sx, sy + AVATAR_H / 2 - 5, 20, 5, 0x528A);
        }

        // Select sprite
        const uint32_t *sp = sprite_idle;
        float bounce = 0.0f;

        if (s_world.state == PET_ST_WALK) {
            bool step = (sinf(s_world.walk_cycle) > 0);
            bounce = fabsf(sinf(s_world.walk_cycle)) * 3.0f;
            if (s_world.facing_right) {
                sp = step ? sprite_walk1 : sprite_walk2;
            } else {
                sp = step ? sprite_walk1_l : sprite_walk2_l;
            }
        } else if (s_world.state == PET_ST_SLEEP) {
            sp = sprite_sleep;
        } else if (s_world.state == PET_ST_PETTED) {
            sp = sprite_happy;
            bounce = sinf(now_ms / 140.0f) * 3.0f;
        } else if (s_world.state == PET_ST_EAT) {
            bool chew = ((now_ms / 200) % 2 == 0);
            sp = chew ? sprite_eat1 : sprite_eat2;
        } else if (s_world.state == PET_ST_SPEAKING) {
            bool mouth = ((now_ms / 150) % 2 == 0);
            sp = mouth ? sprite_eat1 : sprite_happy;
            bounce = sinf(now_ms / 160.0f) * 1.5f;
        } else if (s_world.state == PET_ST_LISTENING || s_world.state == PET_ST_THINKING) {
            sp = sprite_idle;
            bounce = sinf(now_ms / 220.0f) * 2.0f;
        } else if (s_world.state == PET_ST_DRAGGED) {
            sp = sprite_dragged;
        } else {
            // Idle breathing
            bounce = sinf(now_ms / 520.0f) * 1.2f;
            sp = sprite_idle;
        }

        int draw_x = sx - AVATAR_W / 2;
        int draw_y = sy - (int)bounce - AVATAR_H / 2;

        // Draw the 52x62 High-Resolution Bunny Sprite
        draw_sprite_52x62(fb, sp, draw_x, draw_y);

        // Listening pulse rings
        if (s_world.state == PET_ST_LISTENING) {
            float pulse = (sinf(now_ms / 110.0f) + 1.0f) * 0.5f;
            int r = 10 + (int)(pulse * 6);
            gfx_draw_circle(fb, draw_x + 8, draw_y + 14, r, 0x07FF);
            gfx_draw_circle(fb, draw_x + AVATAR_W - 8, draw_y + 14, r, 0x07FF);
        }

        // Cute Nightcap when sleeping in bed
        if (s_world.state == PET_ST_SLEEP) {
            int cap_x = sx - 6;
            int cap_y = draw_y - 2;
            fill_triangle(fb, cap_x - 12, cap_y + 4, cap_x + 12, cap_y + 4, cap_x - 18, cap_y - 8, 0x535F);
            gfx_fill_circle(fb, cap_x - 18, cap_y - 8, 4, GFX_WHITE);
        }

        // Speech Bubble
        if (s_world.speech_text[0]) {
            draw_speech_bubble(fb, sx, draw_y, s_world.speech_text);
        }
    }

    // 3. Status Bar & Mini-Map Radar on top
    draw_status_bar(fb, &s_world);

    // 4. Voice / Talk Button Indication on bottom
    draw_talk_indicator(fb, now_ms, &s_world);
}

void pet_world_set_talk_pressed(bool pressed)
{
    s_world.is_talk_pressed = pressed;
    if (pressed) {
        s_world.state = PET_ST_LISTENING;
        snprintf(s_world.speech_text, sizeof(s_world.speech_text), "Listening...");
    } else if (s_world.state == PET_ST_LISTENING) {
        s_world.state = PET_ST_THINKING;
        snprintf(s_world.speech_text, sizeof(s_world.speech_text), "Thinking...");
    }
}

bool pet_world_is_talk_pressed(void)
{
    return s_world.is_talk_pressed;
}
