/*
 * Hardware & M5Stack Core2 Showcase Demo App
 * Showcases capacitive touch, physics particle engine, AXP192 power telemetry, and audio visualizer.
 */

#include "app_demo.h"
#include "../app_manager.h"
#include "../avatar/gfx_primitives.h"
#include "muse_battery.h"
#include "muse_state.h"

#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#define MAX_BALLS 8

typedef struct {
    float x, y;
    float vx, vy;
    float r;
    uint16_t color;
    bool active;
} demo_ball_t;

static demo_ball_t s_balls[MAX_BALLS];
static float s_demo_audio = 0.0f;
static int s_demo_touch_x = -1;
static int s_demo_touch_y = -1;
static bool s_demo_touching = false;
static bool s_back_pressed = false;

static const uint16_t BALL_COLORS[] = {
    GFX_CYAN, GFX_YELLOW, GFX_PINK, GFX_GREEN, GFX_ORANGE, GFX_ACCENT_BLUE, GFX_WHITE
};

void app_demo_init(void)
{
    s_demo_touching = false;
    s_back_pressed = false;
    for (int i = 0; i < MAX_BALLS; i++) {
        s_balls[i].x = 40.0f + (float)(i * 35);
        s_balls[i].y = 80.0f + (float)((i % 3) * 20);
        s_balls[i].vx = ((float)(rand() % 40) - 20.0f) * 0.1f;
        s_balls[i].vy = ((float)(rand() % 40) - 20.0f) * 0.1f;
        s_balls[i].r = 8.0f + (float)(rand() % 6);
        s_balls[i].color = BALL_COLORS[i % 7];
        s_balls[i].active = true;
    }
}

void app_demo_update(uint32_t now_ms, float audio_level)
{
    (void)now_ms;
    s_demo_audio += (audio_level - s_demo_audio) * 0.35f;

    // Physics step for balls
    for (int i = 0; i < MAX_BALLS; i++) {
        if (!s_balls[i].active) continue;

        // Gravity
        s_balls[i].vy += 0.25f;

        // Position update
        s_balls[i].x += s_balls[i].vx;
        s_balls[i].y += s_balls[i].vy;

        // Wall collisions
        float min_x = s_balls[i].r + 4.0f;
        float max_x = (float)SCREEN_W - s_balls[i].r - 4.0f;
        float min_y = 68.0f + s_balls[i].r;
        float max_y = 190.0f - s_balls[i].r;

        if (s_balls[i].x < min_x) {
            s_balls[i].x = min_x;
            s_balls[i].vx = -s_balls[i].vx * 0.85f;
        } else if (s_balls[i].x > max_x) {
            s_balls[i].x = max_x;
            s_balls[i].vx = -s_balls[i].vx * 0.85f;
        }

        if (s_balls[i].y < min_y) {
            s_balls[i].y = min_y;
            s_balls[i].vy = -s_balls[i].vy * 0.85f;
        } else if (s_balls[i].y > max_y) {
            s_balls[i].y = max_y;
            s_balls[i].vy = -s_balls[i].vy * 0.82f;
            // Floor friction
            s_balls[i].vx *= 0.96f;
        }
    }
}

void app_demo_render(uint16_t *fb, uint32_t now_ms)
{
    // Background
    gfx_fill_rect(fb, 0, 0, SCREEN_W, SCREEN_H, GFX_BG_DARK);

    // 1. Header (y: 0..26)
    gfx_fill_rect(fb, 0, 0, SCREEN_W, 26, 0x18C3);
    gfx_draw_fast_hline(fb, 0, 26, SCREEN_W, 0x2945);

    // Return button [☷ APPS] at (x=6, y=4, w=54, h=18)
    gfx_fill_round_rect(fb, 6, 4, 54, 18, 3, s_back_pressed ? GFX_ACCENT_LIME : 0x29A7);
    gfx_draw_round_rect(fb, 6, 4, 54, 18, 3, GFX_ACCENT_LIME);
    gfx_draw_string(fb, 12, 9, "< APPS", s_back_pressed ? GFX_BLACK : GFX_WHITE);

    gfx_draw_string(fb, 72, 9, "HARDWARE SHOWCASE DEMO", GFX_ACCENT_LIME);

    // 2. Telemetry Bar (y: 30..62)
    // Card 1: AXP192 Battery & Power
    gfx_fill_round_rect(fb, 6, 30, 150, 32, 4, GFX_CARD_BG);
    gfx_draw_round_rect(fb, 6, 30, 150, 32, 4, GFX_CARD_BORDER);

    char buf[32];
    muse_power_t pwr = muse_state_power();
    int pct = pwr.battery_pct >= 0 ? pwr.battery_pct : 100;
    snprintf(buf, sizeof(buf), "BATTERY: %d%%", pct);
    gfx_draw_string(fb, 12, 34, buf, GFX_ACCENT_LIME);
    snprintf(buf, sizeof(buf), "STATUS: %s", muse_state_on_battery() ? "BATTERY" : "CHARGING");
    gfx_draw_string(fb, 12, 46, buf, GFX_TEXT_MAIN);

    // Card 2: Memory & Platform Specs
    gfx_fill_round_rect(fb, 164, 30, 150, 32, 4, GFX_CARD_BG);
    gfx_draw_round_rect(fb, 164, 30, 150, 32, 4, GFX_CARD_BORDER);
    gfx_draw_string(fb, 170, 34, "CORE: ESP32-D0WD", GFX_ACCENT_CYAN);
    gfx_draw_string(fb, 170, 46, "PSRAM: 8MB | FLASH: 16MB", GFX_TEXT_MAIN);

    // 3. Physics Sandbox Canvas (y: 66..192)
    gfx_fill_round_rect(fb, 6, 66, 308, 126, 6, 0x0821);
    gfx_draw_round_rect(fb, 6, 66, 308, 126, 6, 0x2125);
    gfx_draw_string(fb, 14, 72, "TOUCH SCREEN TO INTERACT WITH PARTICLES", GFX_TEXT_DIM);

    // Draw Physics Balls
    for (int i = 0; i < MAX_BALLS; i++) {
        if (!s_balls[i].active) continue;
        int bx = (int)s_balls[i].x;
        int by = (int)s_balls[i].y;
        int br = (int)s_balls[i].r;
        gfx_fill_circle(fb, bx, by, br, s_balls[i].color);
        // Highlight reflection
        gfx_fill_circle(fb, bx - br / 3, by - br / 3, br / 3, GFX_WHITE);
    }

    // Touch Indicator
    if (s_demo_touching && s_demo_touch_y >= 66 && s_demo_touch_y <= 192) {
        gfx_draw_circle(fb, s_demo_touch_x, s_demo_touch_y, 16, GFX_ACCENT_ROSE);
        gfx_draw_circle(fb, s_demo_touch_x, s_demo_touch_y, 12, GFX_WHITE);
        snprintf(buf, sizeof(buf), "Touch: (%d, %d)", s_demo_touch_x, s_demo_touch_y);
        gfx_draw_string(fb, 12, 176, buf, GFX_YELLOW);
    }

    // 4. Audio Spectrum Visualizer (y: 196..236)
    gfx_fill_round_rect(fb, 6, 196, 308, 40, 4, GFX_CARD_BG);
    gfx_draw_round_rect(fb, 6, 196, 308, 40, 4, GFX_CARD_BORDER);
    gfx_draw_string(fb, 12, 199, "REAL-TIME AUDIO SPECTRUM (SPM1423 MIC)", GFX_ACCENT_GOLD);

    // 16 animated equalizer bars
    int num_bars = 16;
    for (int b = 0; b < num_bars; b++) {
        int bx = 14 + b * 18;
        // Pseudo-spectrum variance based on live audio level and time
        float wave = sinf((float)(now_ms) * 0.008f + (float)b * 0.7f) * 0.3f + 0.7f;
        int bar_h = (int)(s_demo_audio * 28.0f * wave);
        if (bar_h > 24) bar_h = 24;
        if (bar_h < 2) bar_h = 2;

        uint16_t b_col = (b < 6) ? GFX_GREEN : (b < 11) ? GFX_YELLOW : GFX_RED;
        gfx_fill_rect(fb, bx, 234 - bar_h, 12, bar_h, b_col);
    }
}

void app_demo_touch_down(int x, int y)
{
    if (x >= 4 && x <= 64 && y >= 2 && y <= 26) {
        s_back_pressed = true;
        return;
    }
    s_demo_touching = true;
    s_demo_touch_x = x;
    s_demo_touch_y = y;

    // Fling nearest ball towards touch
    for (int i = 0; i < MAX_BALLS; i++) {
        float dx = (float)x - s_balls[i].x;
        float dy = (float)y - s_balls[i].y;
        float dist = sqrtf(dx * dx + dy * dy);
        if (dist < 40.0f) {
            s_balls[i].vx = (dx / (dist + 1.0f)) * 8.0f;
            s_balls[i].vy = -6.0f;
        }
    }
}

void app_demo_touch_move(int x, int y)
{
    s_demo_touch_x = x;
    s_demo_touch_y = y;
}

void app_demo_touch_up(void)
{
    if (s_back_pressed) {
        app_manager_open_launcher();
        s_back_pressed = false;
        return;
    }
    s_demo_touching = false;
}
