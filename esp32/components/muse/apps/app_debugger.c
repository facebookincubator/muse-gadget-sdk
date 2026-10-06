/*
 * Step-by-Step Diagnostics & Debugger App for M5Stack Core2
 */

#include "app_debugger.h"
#include "../app_manager.h"
#include "../avatar/gfx_primitives.h"
#include "muse_audio.h"
#include "muse_battery.h"
#include "muse_voice.h"
#include "muse_chat.h"
#include "muse_settings.h"
#include "muse_state.h"
#include "muse_wifi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_timer.h"

static float s_dbg_level = 0.0f;
static char s_dbg_caption[128] = "Ready for test. Tap Ping or Hold Button B.";
static char s_last_action[48] = "System Idle";
static uint32_t s_action_timer = 0;
static int s_pressed_btn = -1;

void app_debugger_init(void)
{
    s_dbg_level = 0.0f;
    s_pressed_btn = -1;
    strlcpy(s_dbg_caption, "Ready for test. Tap Ping or Hold Button B.", sizeof(s_dbg_caption));
    strlcpy(s_last_action, "Debugger Started", sizeof(s_last_action));
}

void app_debugger_update(uint32_t now_ms, int mode, float audio_level, const char *caption)
{
    (void)mode;
    s_dbg_level += (audio_level - s_dbg_level) * 0.4f;
    if (caption && caption[0]) {
        strlcpy(s_dbg_caption, caption, sizeof(s_dbg_caption));
    }
    if (s_action_timer && now_ms > s_action_timer) {
        s_action_timer = 0;
    }
}

void app_debugger_render(uint16_t *fb, uint32_t now_ms)
{
    // Background
    gfx_fill_rect(fb, 0, 0, SCREEN_W, SCREEN_H, GFX_BG_DARK);

    // 1. Top Header (y: 0..26)
    gfx_fill_rect(fb, 0, 0, SCREEN_W, 26, 0x18C3);
    gfx_draw_fast_hline(fb, 0, 26, SCREEN_W, 0x2945);

    // Return to Apps button [☷ APPS] at (x=6, y=4, w=54, h=18)
    bool apps_btn_down = (s_pressed_btn == 0);
    gfx_fill_round_rect(fb, 6, 4, 54, 18, 3, apps_btn_down ? GFX_ACCENT_CYAN : 0x29A7);
    gfx_draw_round_rect(fb, 6, 4, 54, 18, 3, GFX_ACCENT_CYAN);
    gfx_draw_string(fb, 12, 9, "< APPS", apps_btn_down ? GFX_BLACK : GFX_WHITE);

    gfx_draw_string(fb, 72, 9, "DIAGNOSTICS & DEBUGGER", GFX_ACCENT_CYAN);

    // Wi-Fi dot
    bool wifi_ok = muse_wifi_connected();
    gfx_fill_circle(fb, 308, 13, 4, wifi_ok ? GFX_GREEN : GFX_RED);

    // 2. Step 1: Wi-Fi Status Box (x=6, y=30, w=150, h=48)
    gfx_fill_round_rect(fb, 6, 30, 150, 48, 4, GFX_CARD_BG);
    gfx_draw_round_rect(fb, 6, 30, 150, 48, 4, GFX_CARD_BORDER);
    gfx_draw_string(fb, 12, 34, "STEP 1: WI-FI", GFX_ACCENT_CYAN);

    muse_wifi_status_t wifi_st;
    muse_wifi_status(&wifi_st);
    char buf[96];
    if (wifi_st.state == MUSE_WIFI_CONNECTED) {
        snprintf(buf, sizeof(buf), "SSID: %s", wifi_st.ssid[0] ? wifi_st.ssid : "OK");
        buf[22] = '\0';
        gfx_draw_string(fb, 12, 46, buf, GFX_TEXT_MAIN);
        snprintf(buf, sizeof(buf), "IP:%s %ddBm", wifi_st.ip, wifi_st.rssi);
        gfx_draw_string(fb, 12, 58, buf, GFX_GREEN);
    } else {
        gfx_draw_string(fb, 12, 46, "Status: Disconnected", GFX_RED);
        snprintf(buf, sizeof(buf), "State: %d", wifi_st.state);
        gfx_draw_string(fb, 12, 58, buf, GFX_TEXT_MUTED);
    }

    // 3. Step 2: Hatch Cloud & Noise Status Box (x=164, y=30, w=150, h=48)
    gfx_fill_round_rect(fb, 164, 30, 150, 48, 4, GFX_CARD_BG);
    gfx_draw_round_rect(fb, 164, 30, 150, 48, 4, GFX_CARD_BORDER);
    gfx_draw_string(fb, 170, 34, "STEP 2: CLOUD NOISE", GFX_ACCENT_ROSE);

    muse_hatch_status_t hatch_st;
    muse_hatch_status(&hatch_st);
    if (hatch_st.state == MUSE_HATCH_REACHABLE) {
        gfx_draw_string(fb, 170, 46, "Host: metaaivm.com", GFX_TEXT_MAIN);
        gfx_draw_string(fb, 170, 58, "TLS Noise: OK (200)", GFX_GREEN);
    } else {
        snprintf(buf, sizeof(buf), "State: %s", hatch_st.detail[0] ? hatch_st.detail : "Connecting");
        buf[22] = '\0';
        gfx_draw_string(fb, 170, 46, buf, GFX_YELLOW);
        gfx_draw_string(fb, 170, 58, "Noise: Handshake...", GFX_TEXT_MUTED);
    }

    // 4. Step 3: Microphone Sound Meter Box (x=6, y=82, w=308, h=46)
    gfx_fill_round_rect(fb, 6, 82, 308, 46, 4, GFX_CARD_BG);
    gfx_draw_round_rect(fb, 6, 82, 308, 46, 4, GFX_CARD_BORDER);
    gfx_draw_string(fb, 12, 86, "STEP 3: MIC SPM1423 VU METER", GFX_ACCENT_LIME);

    snprintf(buf, sizeof(buf), "Gain: %ddB", muse_settings_mic_gain());
    gfx_draw_string(fb, 240, 86, buf, GFX_TEXT_MUTED);

    // Audio VU Meter Bar (20 segments)
    int segs = 20;
    int lit_segs = (int)(s_dbg_level * (float)segs * 2.5f);
    if (lit_segs > segs) lit_segs = segs;
    if (lit_segs < 0) lit_segs = 0;

    for (int s = 0; s < segs; s++) {
        int sx = 14 + s * 14;
        uint16_t seg_col = 0x2104; // dark inactive
        if (s < lit_segs) {
            if (s < 12) seg_col = GFX_GREEN;
            else if (s < 16) seg_col = GFX_YELLOW;
            else seg_col = GFX_RED;
        }
        gfx_fill_rect(fb, sx, 102, 10, 18, seg_col);
    }

    // 5. Step 4 & 5 Action Buttons (y=132..158)
    // Button 1: [▶ BEEP SPEAKER] (x=6, y=132, w=150, h=26)
    bool spk_down = (s_pressed_btn == 1);
    gfx_fill_round_rect(fb, 6, 132, 150, 26, 4, spk_down ? GFX_ACCENT_GOLD : 0x2A45);
    gfx_draw_round_rect(fb, 6, 132, 150, 26, 4, GFX_ACCENT_GOLD);
    gfx_draw_string(fb, 16, 140, "▶ BEEP SPEAKER", spk_down ? GFX_BLACK : GFX_WHITE);

    // Button 2: [💬 PING CLOUD] (x=164, y=132, w=150, h=26)
    bool ping_down = (s_pressed_btn == 2);
    gfx_fill_round_rect(fb, 164, 132, 150, 26, 4, ping_down ? GFX_ACCENT_CYAN : 0x2A45);
    gfx_draw_round_rect(fb, 164, 132, 150, 26, 4, GFX_ACCENT_CYAN);
    gfx_draw_string(fb, 178, 140, "💬 PING CLOUD", ping_down ? GFX_BLACK : GFX_WHITE);

    // 6. Live Response Box (y: 162..234)
    gfx_fill_round_rect(fb, 6, 162, 308, 72, 4, 0x0821);
    gfx_draw_round_rect(fb, 6, 162, 308, 72, 4, GFX_CARD_BORDER);

    gfx_draw_string(fb, 12, 166, "LIVE MUSE TRANSCRIPT / RESPONSE:", GFX_ACCENT_GOLD);
    snprintf(buf, sizeof(buf), "[%s]", s_last_action);
    gfx_draw_string(fb, 220, 166, buf, GFX_TEXT_MUTED);

    // Word wrapped display of caption in response box
    int cur_x = 12, cur_y = 180;
    const char *p = s_dbg_caption;
    while (*p && cur_y <= 222) {
        if (cur_x > 298 || *p == '\n') {
            cur_x = 12;
            cur_y += 11;
            if (*p == '\n') { p++; continue; }
        }
        gfx_draw_char(fb, cur_x, cur_y, *p++, GFX_WHITE);
        cur_x += 6;
    }
}

void app_debugger_touch_down(int x, int y)
{
    // [☷ APPS] button
    if (x >= 4 && x <= 64 && y >= 2 && y <= 26) {
        s_pressed_btn = 0;
    }
    // [▶ BEEP SPEAKER] button
    else if (x >= 6 && x <= 156 && y >= 132 && y <= 158) {
        s_pressed_btn = 1;
    }
    // [💬 PING CLOUD] button
    else if (x >= 164 && x <= 314 && y >= 132 && y <= 158) {
        s_pressed_btn = 2;
    }
}

void app_debugger_touch_move(int x, int y)
{
    (void)x;
    (void)y;
}

void app_debugger_touch_up(void)
{
    if (s_pressed_btn == 0) {
        app_manager_open_launcher();
    } else if (s_pressed_btn == 1) {
        strlcpy(s_last_action, "Beeping Speaker...", sizeof(s_last_action));
        /* The voice task owns the mic/speaker I2S channels (shared GPIO 0):
         * play the chirp there, never from the UI task. */
        muse_voice_request_chirp();
    } else if (s_pressed_btn == 2) {
        strlcpy(s_last_action, "Sending Ping...", sizeof(s_last_action));
        strlcpy(s_dbg_caption, "Sending 'Ping' to Muse Spark...", sizeof(s_dbg_caption));
        muse_hatch_text_turn(strdup("Ping"));
    }
    s_pressed_btn = -1;
}
