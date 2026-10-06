/*
 * App Selection Menu / Launcher for M5Stack Core2
 */

#include "app_launcher.h"
#include "../app_manager.h"
#include "../avatar/gfx_primitives.h"
#include "muse_battery.h"
#include "muse_wifi.h"
#include "muse_state.h"

#include <stdio.h>
#include <string.h>

static int s_touched_card = -1;

void app_launcher_init(void)
{
    s_touched_card = -1;
}

void app_launcher_update(uint32_t now_ms)
{
    (void)now_ms;
}

static void draw_bunny_icon(uint16_t *fb, int cx, int cy, uint16_t col)
{
    // Ears
    gfx_fill_round_rect(fb, cx - 12, cy - 22, 8, 20, 3, col);
    gfx_fill_round_rect(fb, cx + 4, cy - 22, 8, 20, 3, col);
    gfx_fill_round_rect(fb, cx - 10, cy - 18, 4, 14, 2, GFX_PINK);
    gfx_fill_round_rect(fb, cx + 6, cy - 18, 4, 14, 2, GFX_PINK);
    // Head
    gfx_fill_round_rect(fb, cx - 18, cy - 6, 36, 26, 8, col);
    // Eyes
    gfx_fill_circle(fb, cx - 7, cy + 4, 3, GFX_BLACK);
    gfx_fill_circle(fb, cx + 7, cy + 4, 3, GFX_BLACK);
    gfx_draw_pixel(fb, cx - 8, cy + 3, GFX_WHITE);
    gfx_draw_pixel(fb, cx + 6, cy + 3, GFX_WHITE);
    // Nose & Cheeks
    gfx_fill_circle(fb, cx, cy + 9, 2, GFX_PINK);
    gfx_fill_circle(fb, cx - 12, cy + 10, 3, 0xFDC8); // Blush
    gfx_fill_circle(fb, cx + 12, cy + 10, 3, 0xFDC8);
}

static void draw_debugger_icon(uint16_t *fb, int cx, int cy, uint16_t col)
{
    // Chip Body
    gfx_fill_round_rect(fb, cx - 16, cy - 14, 32, 28, 4, GFX_BLACK);
    gfx_draw_round_rect(fb, cx - 16, cy - 14, 32, 28, 4, col);
    // Pins
    for (int i = -10; i <= 10; i += 7) {
        gfx_draw_fast_vline(fb, cx + i, cy - 18, 4, col);
        gfx_draw_fast_vline(fb, cx + i, cy + 14, 4, col);
    }
    // Pulse wave inside
    gfx_draw_fast_hline(fb, cx - 11, cy, 5, col);
    gfx_draw_fast_vline(fb, cx - 6, cy - 8, 16, col);
    gfx_draw_fast_vline(fb, cx + 2, cy - 8, 16, col);
    gfx_draw_fast_hline(fb, cx - 6, cy - 8, 8, col);
    gfx_draw_fast_hline(fb, cx + 2, cy + 8, 5, col);
    gfx_draw_fast_vline(fb, cx + 7, cy, 8, col);
}

static void draw_demo_icon(uint16_t *fb, int cx, int cy, uint16_t col)
{
    // Gamepad / Sensor Icon
    gfx_fill_round_rect(fb, cx - 18, cy - 10, 36, 22, 6, GFX_CARD_BG);
    gfx_draw_round_rect(fb, cx - 18, cy - 10, 36, 22, 6, col);
    // D-Pad
    gfx_fill_rect(fb, cx - 13, cy - 4, 3, 9, col);
    gfx_fill_rect(fb, cx - 16, cy - 1, 9, 3, col);
    // Buttons
    gfx_fill_circle(fb, cx + 7, cy - 2, 2, GFX_RED);
    gfx_fill_circle(fb, cx + 13, cy + 2, 2, GFX_YELLOW);
    gfx_fill_circle(fb, cx + 7, cy + 6, 2, GFX_GREEN);
    gfx_fill_circle(fb, cx + 1, cy + 2, 2, GFX_BLUE);
}

void app_launcher_render(uint16_t *fb, uint32_t now_ms)
{
    // 1. Dark Gradient / Sleek Slate Background
    gfx_fill_rect(fb, 0, 0, SCREEN_W, SCREEN_H, GFX_BG_DARK);

    // 2. Top Title Bar (y: 0..28)
    gfx_fill_rect(fb, 0, 0, SCREEN_W, 26, 0x18C3);
    gfx_draw_fast_hline(fb, 0, 26, SCREEN_W, 0x2945);

    gfx_draw_string(fb, 10, 9, "M5STACK CORE2 OS", GFX_ACCENT_CYAN);

    // Battery & Wi-Fi in top bar
    char bat_str[16];
    muse_power_t pwr = muse_state_power();
    int pct = pwr.battery_pct >= 0 ? pwr.battery_pct : 100;
    snprintf(bat_str, sizeof(bat_str), "%d%%", pct);
    gfx_draw_string(fb, 276, 9, bat_str, GFX_TEXT_MAIN);
    // Battery icon
    gfx_draw_rect(fb, 252, 8, 18, 10, GFX_TEXT_MAIN);
    gfx_fill_rect(fb, 270, 11, 2, 4, GFX_TEXT_MAIN);
    int bat_w = (pct * 14) / 100;
    if (bat_w > 0) {
        gfx_fill_rect(fb, 254, 10, bat_w, 6, pct < 20 ? GFX_RED : GFX_GREEN);
    }
    // Wi-Fi dot
    bool wifi_ok = muse_wifi_connected();
    gfx_fill_circle(fb, 238, 13, 3, wifi_ok ? GFX_GREEN : GFX_RED);

    // Subheading
    gfx_draw_string(fb, 10, 31, "CHOOSE AN APPLICATION:", GFX_TEXT_MUTED);

    // 3. Three Application Cards
    // Card Dimensions: w = 94, h = 154, y = 44
    // Card 0: Pet World (x = 10)
    // Card 1: Debugger  (x = 113)
    // Card 2: Demo      (x = 216)
    const struct {
        int x, y, w, h;
        const char *title;
        const char *subtitle;
        const char *desc1;
        const char *desc2;
        uint16_t accent;
    } cards[3] = {
        { 10,  44, 94, 154, "PET WORLD", "AI Assistant", "3-Room Pet", "Voice Chat",  GFX_ACCENT_ROSE },
        { 113, 44, 94, 154, "DEBUGGER",  "Diagnostics",  "Wi-Fi, Noise", "Mic, Spk, AI", GFX_ACCENT_CYAN },
        { 216, 44, 94, 154, "CORE2 DEMO", "Showcase",    "Sensors, AXP", "Touch Canvas", GFX_ACCENT_LIME },
    };

    for (int i = 0; i < 3; i++) {
        bool selected = (s_touched_card == i);
        uint16_t bg = selected ? 0x2965 : GFX_CARD_BG;
        uint16_t border = selected ? GFX_WHITE : cards[i].accent;

        // Card container
        gfx_fill_round_rect(fb, cards[i].x, cards[i].y, cards[i].w, cards[i].h, 8, bg);
        gfx_draw_round_rect(fb, cards[i].x, cards[i].y, cards[i].w, cards[i].h, 8, border);
        if (selected) {
            gfx_draw_round_rect(fb, cards[i].x + 1, cards[i].y + 1, cards[i].w - 2, cards[i].h - 2, 7, border);
        }

        // Top accent pill / badge
        gfx_fill_round_rect(fb, cards[i].x + 8, cards[i].y + 8, cards[i].w - 16, 16, 4, cards[i].accent);
        int badge_x = cards[i].x + (cards[i].w - (int)strlen(cards[i].subtitle) * 6) / 2;
        gfx_draw_string(fb, badge_x, cards[i].y + 13, cards[i].subtitle, GFX_BLACK);

        // Icon area (cy = cards[i].y + 54)
        int cx = cards[i].x + cards[i].w / 2;
        int cy = cards[i].y + 54;
        if (i == 0) {
            draw_bunny_icon(fb, cx, cy, GFX_WHITE);
        } else if (i == 1) {
            draw_debugger_icon(fb, cx, cy, cards[i].accent);
        } else {
            draw_demo_icon(fb, cx, cy, cards[i].accent);
        }

        // Title
        int title_x = cards[i].x + (cards[i].w - (int)strlen(cards[i].title) * 6) / 2;
        gfx_draw_string(fb, title_x, cards[i].y + 82, cards[i].title, GFX_TEXT_MAIN);
        gfx_draw_fast_hline(fb, cards[i].x + 12, cards[i].y + 94, cards[i].w - 24, GFX_CARD_BORDER);

        // Description lines
        int d1_x = cards[i].x + (cards[i].w - (int)strlen(cards[i].desc1) * 6) / 2;
        gfx_draw_string(fb, d1_x, cards[i].y + 104, cards[i].desc1, GFX_TEXT_MUTED);
        int d2_x = cards[i].x + (cards[i].w - (int)strlen(cards[i].desc2) * 6) / 2;
        gfx_draw_string(fb, d2_x, cards[i].y + 118, cards[i].desc2, GFX_TEXT_MUTED);

        // Launch button pill at bottom of card
        uint16_t btn_col = selected ? cards[i].accent : 0x2A69;
        gfx_fill_round_rect(fb, cards[i].x + 12, cards[i].y + 134, cards[i].w - 24, 14, 3, btn_col);
        gfx_draw_string(fb, cards[i].x + 30, cards[i].y + 138, "OPEN", selected ? GFX_BLACK : GFX_TEXT_MAIN);
    }

    // 4. Bottom Navigation Hint (y: 206..240)
    gfx_fill_rect(fb, 0, 206, SCREEN_W, 34, 0x0841);
    gfx_draw_fast_hline(fb, 0, 206, SCREEN_W, 0x2104);
    gfx_draw_string(fb, 16, 214, "[A] APP MENU", GFX_ACCENT_CYAN);
    gfx_draw_string(fb, 114, 214, "[B] HOLD TO TALK", GFX_ACCENT_ROSE);
    gfx_draw_string(fb, 230, 214, "[C] SETTINGS", GFX_ACCENT_GOLD);
    gfx_draw_string(fb, 36, 227, "Touch any card to launch | Button A returns here", GFX_TEXT_DIM);
}

void app_launcher_touch_down(int x, int y)
{
    if (y >= 40 && y <= 200) {
        if (x >= 8 && x < 108) {
            s_touched_card = 0;
        } else if (x >= 108 && x < 212) {
            s_touched_card = 1;
        } else if (x >= 212 && x <= 316) {
            s_touched_card = 2;
        }
    }
}

void app_launcher_touch_move(int x, int y)
{
    (void)x;
    (void)y;
}

void app_launcher_touch_up(void)
{
    if (s_touched_card == 0) {
        app_manager_set_app(APP_PET);
    } else if (s_touched_card == 1) {
        app_manager_set_app(APP_DEBUGGER);
    } else if (s_touched_card == 2) {
        app_manager_set_app(APP_DEMO);
    }
    s_touched_card = -1;
}
