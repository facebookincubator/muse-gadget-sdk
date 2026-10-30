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
 * Settings on e-paper: muse_settings_ui.c's pages, rows and wording, drawn
 * with FreeInk. Each page is drawn whole from what its tick last read, in the
 * LVGL UI's 330 px column of cards. Two things suit the panel better than a
 * copy would: a page longer than the screen moves a screenful per vertical
 * swipe rather than scrolling under the finger, and a slider takes the value
 * where it's tapped rather than following a drag.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_mac.h"
#include "esp_timer.h"

#include "muse_eink_priv.h"

extern "C" {
#include "muse_audio.h"
#include "muse_battery.h"
#include "muse_ble.h"
#include "muse_board.h"
#include "muse_chat.h"
#include "muse_input.h"
#include "muse_link.h"
#include "muse_settings.h"
#include "muse_state.h"
#include "muse_text.h"
#include "muse_ui.h"
#include "muse_voice.h"
#include "muse_wifi.h"
}

namespace muse_eink {

using freeink::ui::ActionEvent;
using freeink::ui::ActionId;

#define LIST_W 330
/* The header in the screen's top-left corner, the list right under it:
 * muse_settings_ui.c places them for a 466 px round screen instead. */
static const Rect BACK = {8, 8, 56, 48};
#define TITLE_Y (BACK.y + (BACK.height - line_height(FONT_UNSCII16)) / 2)
#define LIST_TOP ((int16_t)(BACK.y + BACK.height + 12))
#define ROW_H 58
#define INFO_H 44
#define GAP 10
#define PAD_BOTTOM 110
#define MAX_APS 12
#define SHOWN_MAX 96
#define TEXT_MAX (MUSE_TOKEN_MAX + 1)

enum Page { HOME, WIFI, HATCH, BLE, SOUND, SLEEP, BATTERY, POWER, TEXT, PAGE_COUNT };

enum : ActionId {
    ACT_NAV = ACTION_SETTINGS_BASE,
    ACT_BACK,
    ACT_WIFI_SW,
    ACT_WIFI_SCAN,
    ACT_WIFI_AP,
    ACT_WIFI_SAVED,
    ACT_WIFI_OTHER,
    ACT_HATCH_RESET,
    ACT_HATCH_HOST,
    ACT_HATCH_VM,
    ACT_HATCH_TOKEN,
    ACT_HATCH_SDK,
    ACT_HATCH_TEST,
    ACT_BLE_SW,
    ACT_BLE_FORGET,
    ACT_SPK_SW,
    ACT_VOLUME,
    ACT_GAIN,
    ACT_SLEEP_CHOICE,
    ACT_SLEEP_NOW,
    ACT_BATT_RESET,
    ACT_POWER_OFF,
    ACT_TEXT_SHOW,
    ACT_KB_KEY,
    ACT_KB_SHIFT,
    ACT_KB_MODE,
    ACT_KB_DEL,
    ACT_KB_OK,
    ACT_KB_LANG,
    ACT_LAST,
};

static Page s_page = HOME;
static int s_scroll;            /* how far the page's list has moved up */
static int s_content_h;         /* the list's height, as last drawn */
static bool s_changed;

/* Text that changed since the page was last drawn makes it draw again. */
static void set_text(char *dst, size_t cap, const char *text)
{
    char shown[SHOWN_MAX];
    text = muse_text_showable(text, shown, sizeof(shown));
    if (strncmp(dst, text, cap) != 0) {
        strlcpy(dst, text, cap);
        s_changed = true;
    }
}
#define SET(dst, text) set_text(dst, sizeof(dst), text)

static void set_flag(bool *dst, bool v)
{
    if (*dst != v) {
        *dst = v;
        s_changed = true;
    }
}

/* ─── Drawing: the LVGL page's column, top down ─────────────────────────── */

struct Column {
    Screen *screen;
    int x, y;           /* the next item's top, on screen */
};

static freeink::ui::DrawTarget &T(Column &c)
{
    return c.screen->target();
}

static bool flashing(Column &c, ActionId action, int16_t value)
{
    return c.screen->frame().stateFor(action, value) & freeink::ui::StateFocused;
}

static bool visible(int y, int h)
{
    return y + h > LIST_TOP && y < g_h;
}

static void hit(Column &c, int y, int h, ActionId action, int16_t value)
{
    if (!visible(y, h)) return;
    int top = y < LIST_TOP ? LIST_TOP : y;
    c.screen->frame().hit(Rect{(int16_t)c.x, (int16_t)top, LIST_W, (int16_t)(y + h - top)}, action, value);
}

/* A card: outlined, and inked in while a tap on it shows. Returns the colour
 * of text on it. */
static Color card(Column &c, int h, bool pressed)
{
    Rect r{(int16_t)c.x, (int16_t)c.y, LIST_W, (int16_t)h};
    if (pressed) {
        T(c).fill(r, Paint::solid(Color::Black), 18);
        return Color::White;
    }
    T(c).stroke(r, Paint::solid(Color::Black), 2, 18);
    return Color::Black;
}

static void advance(Column &c, int h)
{
    c.y += h + GAP;
}

static int note_height(const char *text)
{
    return wrapped_lines(LIST_W, FONT_M16, text) * line_height(FONT_M16);
}

/* Only variable-height notes move the hit regions below them. */
static bool set_note(char *dst, size_t cap, const char *text)
{
    int old_h = note_height(dst);
    set_text(dst, cap, text);
    return old_h != note_height(dst);
}
#define SET_NOTE(dst, text) set_note(dst, sizeof(dst), text)

/* Text cut to w px, ending in dots, as LONG_MODE_DOTS. */
static void fit(char *out, size_t cap, const char *text, FontId font, int w)
{
    if (!cap) return;
    size_t n = utf8_prefix_bytes(text, cap - 1);
    memcpy(out, text, n);
    out[n] = 0;
    sanitize_utf8(out);
    if (text_width(font, out) <= w) return;
    size_t room = cap > 3 ? cap - 4 : 0;
    n = utf8_prefix_bytes(out, room);
    out[n] = 0;
    while (n && text_width(font, out) + text_width(font, "...") > w) {
        n = utf8_pop_bytes(out, n);
        out[n] = 0;
    }
    strlcat(out, "...", cap);
}

static void note(Column &c, const char *text)
{
    if (!text[0]) return;
    int h = note_height(text);
    if (visible(c.y, h)) draw_wrapped(c.x, c.y, LIST_W, FONT_M16, text, 40, 0, true);
    advance(c, h);
}

static void row(Column &c, const char *icon, const char *text, const char *value, ActionId action,
                int16_t value_id = 0, bool selected = false)
{
    bool tappable = action != 0;
    if (visible(c.y, ROW_H)) {
        bool pressed = tappable && flashing(c, action, value_id);
        if (selected && !pressed) {
            /* The current choice, on e-paper (muse_settings_ui.c's tick_sleep). */
            T(c).fill(Rect{(int16_t)c.x, (int16_t)c.y, LIST_W, ROW_H}, Paint::dither(Color::LightGray), 18);
        }
        Color ink = card(c, ROW_H, pressed);
        int x = c.x + 16, ty = c.y + (ROW_H - line_height(FONT_M20)) / 2;
        if (icon) {
            draw_text(x, ty, FONT_M20, icon, ink);
            x += text_width(FONT_M20, icon) + 12;
        }
        char v[SHOWN_MAX] = "";
        int vw = 0;
        if (value && value[0]) {
            fit(v, sizeof(v), value, FONT_M16, 130);
            vw = text_width(FONT_M16, v);
        }
        char t[SHOWN_MAX];
        fit(t, sizeof(t), text, FONT_M20, c.x + LIST_W - 16 - x - (vw ? vw + 12 : 0));
        draw_text(x, ty, FONT_M20, t, ink);
        if (vw) draw_text(c.x + LIST_W - 16 - vw, c.y + (ROW_H - line_height(FONT_M16)) / 2, FONT_M16, v, ink);
    }
    if (tappable) hit(c, c.y, ROW_H, action, value_id);
    advance(c, ROW_H);
}

static void switch_row(Column &c, const char *text, bool on, ActionId action)
{
    if (visible(c.y, ROW_H)) {
        Color ink = card(c, ROW_H, flashing(c, action, 0));
        draw_text(c.x + 16, c.y + (ROW_H - line_height(FONT_M20)) / 2, FONT_M20, text, ink);
        /* LVGL's switch: 60 x 32, its knob at the end it's switched to. */
        Rect sw{(int16_t)(c.x + LIST_W - 16 - 60), (int16_t)(c.y + (ROW_H - 32) / 2), 60, 32};
        Paint p = Paint::solid(ink);
        if (on) T(c).fill(sw, p, 16);
        else T(c).stroke(sw, p, 2, 16);
        int kx = on ? sw.x + 60 - 28 : sw.x + 4;
        T(c).fill(Rect{(int16_t)kx, (int16_t)(sw.y + 4), 24, 24},
                  Paint::solid(on ? (ink == Color::Black ? Color::White : Color::Black) : ink), 12);
    }
    hit(c, c.y, ROW_H, action, 0);
    advance(c, ROW_H);
}

static void button(Column &c, const char *text, ActionId action, int16_t value_id = 0)
{
    if (visible(c.y, ROW_H)) {
        Color ink = card(c, ROW_H, flashing(c, action, value_id));
        draw_text_centered(c.x + LIST_W / 2, c.y + (ROW_H - line_height(FONT_M20)) / 2, FONT_M20, text, ink);
    }
    hit(c, c.y, ROW_H, action, value_id);
    advance(c, ROW_H);
}

static void info_row(Column &c, const char *text, const char *value)
{
    if (visible(c.y, INFO_H)) {
        card(c, INFO_H, false);
        int ty = c.y + (INFO_H - line_height(FONT_M16)) / 2;
        draw_text(c.x + 16, ty, FONT_M16, text);
        char v[SHOWN_MAX];
        fit(v, sizeof(v), value, FONT_M16, 160);
        draw_text(c.x + LIST_W - 16 - text_width(FONT_M16, v), ty, FONT_M16, v);
    }
    advance(c, INFO_H);
}

/* Label and value on a line, the slider under them: a tap sets the value
 * there, one of `steps + 1` stops from lo. */
static void slider(Column &c, const char *text, const char *value, int lo, int hi, int v, ActionId action)
{
    const int h = 76;
    int bar_x = c.x + 8 + 10, bar_w = LIST_W - 16 - 20, bar_y = c.y + 6 + 40;
    if (visible(c.y, h)) {
        draw_text(c.x + 8, c.y + 6, FONT_M20, text);
        draw_text(c.x + LIST_W - 8 - text_width(FONT_M20, value), c.y + 6, FONT_M20, value);
        Rect bar{(int16_t)bar_x, (int16_t)bar_y, (int16_t)bar_w, 12};
        T(c).stroke(bar, Paint::solid(Color::Black), 1, 6);
        int fill = hi > lo ? (v - lo) * bar_w / (hi - lo) : 0;
        if (fill > 0) T(c).fill(Rect{bar.x, bar.y, (int16_t)fill, 12}, Paint::solid(Color::Black), 6);
        int kx = bar_x + fill;
        T(c).fill(Rect{(int16_t)(kx - 12), (int16_t)(bar_y + 6 - 12), 24, 24}, Paint::solid(Color::White), 12);
        T(c).stroke(Rect{(int16_t)(kx - 12), (int16_t)(bar_y + 6 - 12), 24, 24}, Paint::solid(Color::Black), 2, 12);
    }
    /* A stop per value, each as wide as its share of the bar. */
    int n = hi - lo + 1;
    for (int i = 0; i < n && visible(c.y, h); i++) {
        int x0 = bar_x + (2 * i - 1) * bar_w / (2 * (n - 1 > 0 ? n - 1 : 1));
        int x1 = bar_x + (2 * i + 1) * bar_w / (2 * (n - 1 > 0 ? n - 1 : 1));
        if (i == 0) x0 = c.x;
        if (i == n - 1) x1 = c.x + LIST_W;
        int top = bar_y - 22 < LIST_TOP ? LIST_TOP : bar_y - 22;
        c.screen->frame().hit(Rect{(int16_t)x0, (int16_t)top, (int16_t)(x1 - x0), (int16_t)(bar_y + 34 - top)}, action,
                              (int16_t)(lo + i));
    }
    advance(c, h);
}

/* ─── The pages' state, read by their ticks ─────────────────────────────── */

/* Home. */
static char s_home_wifi[48], s_home_hatch[48], s_home_ble[16], s_home_sound[24], s_home_sleep[24], s_home_battery[24],
    s_about[96];

/* Wi-Fi. */
static char s_wifi_status[128];
static bool s_wifi_on, s_wifi_scanning;
static muse_wifi_ap_t s_aps[MAX_APS];
static int s_ap_n;
static char s_ap_vals[MAX_APS][16];
static uint32_t s_shown_scan_gen = UINT32_MAX;
static bool s_scan_empty;
static muse_wifi_saved_t s_saved[MUSE_WIFI_SAVED_MAX];
static char s_saved_vals[MUSE_WIFI_SAVED_MAX][24];
static int s_saved_n = -1;
static int s_forget_armed = -1;
static int64_t s_forget_armed_us;
static char s_join_ssid[MUSE_SSID_MAX + 1];
static char s_mac[18];

/* Muse. */
static char s_link_status[128], s_link_reset_lbl[24] = "Reset pairing", s_hatch_status[128];
static char s_hatch_host[MUSE_HOST_MAX + 1], s_hatch_vm[MUSE_VM_MAX + 1], s_hatch_token[16], s_hatch_sdk[24];
static int64_t s_link_reset_armed_us, s_sdk_token_bad_us;

/* Bluetooth. */
static char s_ble_status[96];
static bool s_ble_on;

/* Sound. */
static int s_volume, s_gain_db;
static bool s_speaker_on;

/* Sleep. */
static const int SLEEP_CHOICES[] = {0, 30, 60, 120, 300, 600};
static const char *const SLEEP_NAMES[] = {"Never", "30 seconds", "1 minute", "2 minutes", "5 minutes", "10 minutes"};
#define SLEEP_COUNT (int)(sizeof(SLEEP_CHOICES) / sizeof(SLEEP_CHOICES[0]))
static int s_sleep_cur = -1;

/* Battery. */
static char s_batt_status[96], s_batt_level[32], s_batt_drain[32], s_batt_full[24], s_batt_off[16], s_batt_slept[16],
    s_batt_wakes[16], s_batt_busy[16], s_batt_awake[96];
static int64_t s_batt_shown_us;

/* Text entry. */
typedef void (*text_done_cb_t)(const char *text);
static char *s_text;            /* the field, TEXT_MAX bytes, wiped after use */
static char s_text_title[SHOWN_MAX], s_text_hint[64];
static size_t s_text_max;
static bool s_text_password, s_text_reveal;
static text_done_cb_t s_text_done;
static Page s_text_back;
static freeink::ui::KeyboardEntry s_entry;
static const freeink::ui::KeyboardLayout *s_keyboard_frame;

static const freeink::ui::KeyboardLayout &keyboard_for_frame(void)
{
    s_keyboard_frame = &freeink::ui::builtinKeyboardLayout(s_entry.layout, s_entry.shifted, s_entry.symbols,
                                                          s_entry.numberRow, /*langKey=*/true);
    return *s_keyboard_frame;
}

bool settings_keyboard_changed(void)
{
    return s_page == TEXT && s_keyboard_frame &&
           s_keyboard_frame != &freeink::ui::builtinKeyboardLayout(s_entry.layout, s_entry.shifted, s_entry.symbols,
                                                                   s_entry.numberRow, /*langKey=*/true);
}

static const char *sleep_name(int secs)
{
    for (int i = 0; i < SLEEP_COUNT; i++) {
        if (SLEEP_CHOICES[i] == secs) return SLEEP_NAMES[i];
    }
    return "Custom";
}

/* ─── Navigation ────────────────────────────────────────────────────────── */

static void show(Page p)
{
    if (p == s_page) return;
    navigation_changed();
    s_page = p;
    s_scroll = 0;
    s_changed = true;
    if (p == WIFI) {
        s_shown_scan_gen = UINT32_MAX;
        s_saved_n = -1;
        s_forget_armed = -1;
    } else if (p == HATCH) {
        s_link_reset_armed_us = 0;
        strlcpy(s_link_reset_lbl, "Reset pairing", sizeof(s_link_reset_lbl));
    } else if (p == BATTERY) {
        s_batt_shown_us = 0;
    }
    settings_tick();   /* fill it in now, not at the next tick */
}

static void close_text(void)
{
    if (s_text) memset(s_text, 0, TEXT_MAX);   /* don't leave secrets around */
    show(s_text_back);
}

static void open_text(const char *title, const char *initial, bool password, size_t max_len, const char *hint,
                      text_done_cb_t done, Page back)
{
    if (!s_text) s_text = (char *)calloc(1, TEXT_MAX);
    if (!s_text) return;
    char shown[SHOWN_MAX];
    strlcpy(s_text_title, muse_text_showable(title, shown, sizeof(shown)), sizeof(s_text_title));
    strlcpy(s_text_hint, hint ? hint : "", sizeof(s_text_hint));
    s_text_max = max_len < TEXT_MAX - 1 ? max_len : TEXT_MAX - 1;
    strlcpy(s_text, initial ? initial : "", s_text_max + 1);
    s_entry.attach(s_text, s_text_max + 1);
    s_text_password = password;
    s_text_reveal = false;
    s_text_done = done;
    s_text_back = back;
    show(TEXT);
}

bool settings_in_subpage(void)
{
    return s_page != HOME;
}

bool settings_back(void)
{
    if (s_page == TEXT) {
        close_text();
        return true;
    }
    if (s_page != HOME) {
        show(HOME);
        return true;
    }
    return false;
}

void settings_home(void)
{
    if (s_page == TEXT && s_text) memset(s_text, 0, TEXT_MAX);
    show(HOME);
}

bool settings_typing(void)
{
    return s_page == TEXT;   /* a sloppy swipe mustn't lose the text */
}

void settings_scroll(int dy)
{
    int view = g_h - LIST_TOP;
    int max = s_content_h + PAD_BOTTOM - view;
    /* Swiping up moves the page up: two thirds of a screen at a time. */
    int to = s_scroll + (dy < 0 ? view * 2 / 3 : -view * 2 / 3);
    to = to > max ? max : to;
    to = to < 0 ? 0 : to;
    if (to != s_scroll) {
        navigation_changed();
        s_scroll = to;
        s_changed = true;
    }
}

/* ─── Handlers ──────────────────────────────────────────────────────────── */

static void on_wifi_pass(const char *pass) { muse_settings_set_wifi(s_join_ssid, pass); }

static void on_other_ssid(const char *ssid)
{
    if (!ssid[0]) return;
    strlcpy(s_join_ssid, ssid, sizeof(s_join_ssid));
    open_text(ssid, "", true, MUSE_PASS_MAX, "Empty if it's open", on_wifi_pass, WIFI);
}

static void on_hatch_host_done(const char *text) { muse_settings_set_hatch_host(text); }
static void on_hatch_vm_done(const char *text) { muse_settings_set_hatch_vm(text); }

static void on_hatch_token_done(const char *text)
{
    if (text[0]) muse_settings_set_hatch_token(text, false);
}

static void on_hatch_sdk_token_done(const char *text)
{
    if (text[0] && !muse_link_sdk_token_set(text)) s_sdk_token_bad_us = esp_timer_get_time();
}

static void text_ready(void)
{
    text_done_cb_t done = s_text_done;
    char *text = s_text ? strdup(s_text) : nullptr;
    close_text();
    if (done && text) done(text);
    if (text) {
        memset(text, 0, strlen(text));
        free(text);
    }
}

static void on_action(const ActionEvent &e, void *)
{
    s_changed = true;
    int64_t now = esp_timer_get_time();
    switch (e.action) {
    case ACT_NAV:
        g_app->clearTapFlash();
        show((Page)e.value);
        break;
    case ACT_BACK:
        g_app->clearTapFlash();
        settings_back();
        break;
    case ACT_WIFI_SW: muse_settings_set_wifi_on(!muse_settings_wifi_on()); break;
    case ACT_WIFI_SCAN: muse_wifi_scan(); break;
    case ACT_WIFI_AP: {
        if (s_page != WIFI || e.value < 0 || e.value >= s_ap_n) break;
        /* A saved network asks again too, in case its password changed. */
        const muse_wifi_ap_t *ap = &s_aps[e.value];
        strlcpy(s_join_ssid, ap->ssid, sizeof(s_join_ssid));
        if (ap->secure) {
            g_app->clearTapFlash();
            open_text(ap->ssid, "", true, MUSE_PASS_MAX, "Password", on_wifi_pass, WIFI);
        } else {
            muse_settings_set_wifi(s_join_ssid, "");
        }
        break;
    }
    case ACT_WIFI_SAVED:
        if (s_page != WIFI || e.value < 0 || e.value >= s_saved_n) break;
        /* Two taps within a few seconds forget a saved network. */
        if (s_forget_armed == e.value && now - s_forget_armed_us < 4000000) {
            muse_wifi_forget(s_saved[e.value].ssid);
            s_forget_armed = -1;
            s_forget_armed_us = 0;
        } else {
            s_forget_armed = e.value;
            s_forget_armed_us = now;
        }
        settings_tick();
        break;
    case ACT_WIFI_OTHER:
        g_app->clearTapFlash();
        open_text("Other network", "", false, MUSE_SSID_MAX, "Network name", on_other_ssid, WIFI);
        break;
    case ACT_HATCH_RESET:
        /* Two taps within a few seconds: this wipes Wi-Fi and the Muse app pairing. */
        if (s_link_reset_armed_us && now - s_link_reset_armed_us < 5000000) {
            SET(s_link_reset_lbl, "Resetting...");
            muse_link_reset_setup();
        } else {
            s_link_reset_armed_us = now;
            SET(s_link_reset_lbl, "Tap again to reset");
        }
        break;
    case ACT_HATCH_HOST: {
        char host[MUSE_HOST_MAX + 1];
        muse_settings_hatch_host(host);
        g_app->clearTapFlash();
        open_text("Muse server", host, false, MUSE_HOST_MAX, "Empty for the default", on_hatch_host_done, HATCH);
        break;
    }
    case ACT_HATCH_VM: {
        char vm[MUSE_VM_MAX + 1];
        muse_settings_hatch_vm(vm);
        g_app->clearTapFlash();
        open_text("VM ID", vm, false, MUSE_VM_MAX, "Optional", on_hatch_vm_done, HATCH);
        break;
    }
    case ACT_HATCH_TOKEN:
        g_app->clearTapFlash();
        open_text("Device token", "", true, MUSE_TOKEN_MAX, "Empty keeps the current one", on_hatch_token_done, HATCH);
        break;
    case ACT_HATCH_SDK:
        g_app->clearTapFlash();
        open_text("SDK token", "", true, 48 /* mgst_ and 43 more */, "Empty keeps the current one; a new one restarts",
                  on_hatch_sdk_token_done, HATCH);
        break;
    case ACT_HATCH_TEST: muse_hatch_test(); break;
    case ACT_BLE_SW: muse_settings_set_ble_on(!muse_settings_ble_on()); break;
    case ACT_BLE_FORGET: muse_ble_forget_all(); break;
    case ACT_SPK_SW: muse_settings_set_speaker_on(!muse_settings_speaker_on()); break;
    case ACT_VOLUME:
        muse_settings_set_volume(e.value);
        muse_voice_request_chirp();
        break;
    case ACT_GAIN: muse_settings_set_mic_gain(e.value * 3); break;
    case ACT_SLEEP_CHOICE: muse_settings_set_sleep_s(SLEEP_CHOICES[e.value]); break;
    case ACT_SLEEP_NOW: muse_state_set_asleep(true); break;
    case ACT_BATT_RESET:
        muse_battery_reset();
        s_batt_shown_us = 0;   /* show it now */
        break;
    case ACT_POWER_OFF:
        g_app->clearTapFlash();
        show(HOME);
        muse_ui_show_face();
        muse_input_request_power_off();
        break;
    case ACT_TEXT_SHOW: s_text_reveal = !s_text_reveal; break;
    case ACT_KB_KEY: s_entry.key(e.value, e.longPress); break;
    case ACT_KB_SHIFT:
        s_entry.shift();
        if (s_entry.symbols) navigation_changed();   /* symbol pages change hit geometry */
        break;
    case ACT_KB_MODE:
        s_entry.mode();
        navigation_changed();
        break;
    case ACT_KB_DEL: s_entry.backspace(); break;
    case ACT_KB_LANG: {
        /* The next of FreeInk's layouts the fonts draw and the field reads
         * left to right: Latin, then Cyrillic. */
        static const freeink::ui::KeyboardLayoutId LAYOUTS[] = {
            freeink::ui::KeyboardLayoutId::QwertyEn,   freeink::ui::KeyboardLayoutId::AzertyFr,
            freeink::ui::KeyboardLayoutId::QwertzDe,   freeink::ui::KeyboardLayoutId::SpanishEs,
            freeink::ui::KeyboardLayoutId::CyrillicRu, freeink::ui::KeyboardLayoutId::CyrillicUk,
            freeink::ui::KeyboardLayoutId::CyrillicBe, freeink::ui::KeyboardLayoutId::CyrillicKk,
        };
        const size_t n = sizeof(LAYOUTS) / sizeof(LAYOUTS[0]);
        size_t i = 0;
        while (i < n && LAYOUTS[i] != s_entry.layout) i++;
        s_entry.layout = LAYOUTS[(i + 1) % n];
        s_entry.shifted = false;
        s_entry.symbols = false;
        navigation_changed();
        break;
    }
    case ACT_KB_OK:
        g_app->clearTapFlash();
        text_ready();
        break;
    default: break;
    }
    settings_tick();
}

/* ─── Ticks: muse_settings_ui.c's, into the strings above ───────────────── */

static void tick_home(void)
{
    muse_wifi_status_t w;
    muse_wifi_status(&w);
    static const char *const WIFI_VALUES[] = {"Off", "Not set", "Joining", "", "Failed", "Not nearby"};
    SET(s_home_wifi, w.state == MUSE_WIFI_CONNECTED ? w.ssid : WIFI_VALUES[w.state]);
    muse_hatch_status_t h;
    muse_hatch_status(&h);
    SET(s_home_hatch, muse_hatch_state_name(h.state));
    muse_ble_status_t b;
    muse_ble_status(&b);
    SET(s_home_ble, b.state == MUSE_BLE_OFF ? "Off" : (b.state == MUSE_BLE_CONNECTED ? "Connected" : "On"));
    char buf[96];
    if (!muse_board->audio_init) {
        SET(s_home_sound, muse_board->mic_init ? "Mic only" : "No audio");
    } else if (muse_settings_speaker_on()) {
        snprintf(buf, sizeof(buf), "Vol %d%%", muse_settings_volume());
        SET(s_home_sound, buf);
    } else {
        SET(s_home_sound, "Muted");
    }
    SET(s_home_sleep, sleep_name(muse_settings_sleep_s()));
    muse_power_t p = muse_state_power();
    if (p.battery_pct < 0) strlcpy(buf, "USB", sizeof(buf));
    else snprintf(buf, sizeof(buf), "%s%d%%", p.charging ? LV_SYMBOL_CHARGE " " : "", p.battery_pct);
    SET(s_home_battery, buf);
    snprintf(buf, sizeof(buf), "Muse %s  -  %s", esp_app_get_description()->version,
             w.state == MUSE_WIFI_CONNECTED ? w.ip : "offline");
    SET(s_about, buf);
}

static bool is_saved(const char *ssid)
{
    for (int i = 0; i < s_saved_n; i++) {
        if (!strcmp(s_saved[i].ssid, ssid)) return true;
    }
    return false;
}

static void tick_wifi(void)
{
    muse_wifi_status_t w;
    muse_wifi_status(&w);
    char buf[128];
    switch (w.state) {
    case MUSE_WIFI_OFF: strlcpy(buf, "Wi-Fi is off", sizeof(buf)); break;
    case MUSE_WIFI_NO_NETWORK: strlcpy(buf, "No saved networks. Scan and pick one.", sizeof(buf)); break;
    case MUSE_WIFI_CONNECTING: snprintf(buf, sizeof(buf), "Joining %s\n%s", w.ssid, w.detail); break;
    case MUSE_WIFI_CONNECTED: snprintf(buf, sizeof(buf), "Connected to %s\n%s", w.ssid, w.ip); break;
    case MUSE_WIFI_NOT_NEARBY: strlcpy(buf, "No saved network nearby\nLooking again within a minute", sizeof(buf)); break;
    case MUSE_WIFI_FAILED:
    default: snprintf(buf, sizeof(buf), "Couldn't join %s\n%s", w.ssid, w.detail); break;
    }
    bool layout_changed = SET_NOTE(s_wifi_status, buf);
    layout_changed |= s_wifi_on != (w.state != MUSE_WIFI_OFF);
    set_flag(&s_wifi_on, w.state != MUSE_WIFI_OFF);
    set_flag(&s_wifi_scanning, muse_wifi_scanning());

    muse_wifi_saved_t saved[MUSE_WIFI_SAVED_MAX];
    int n = muse_wifi_saved(saved, MUSE_WIFI_SAVED_MAX);
    if (n != s_saved_n || memcmp(saved, s_saved, n * sizeof(saved[0]))) {
        bool rows_changed = n != s_saved_n;
        for (int i = 0; !rows_changed && i < n; i++) rows_changed = strcmp(saved[i].ssid, s_saved[i].ssid) != 0;
        layout_changed |= rows_changed;
        memcpy(s_saved, saved, n * sizeof(saved[0]));
        s_saved_n = n;
        s_forget_armed = -1;
        s_shown_scan_gen = UINT32_MAX;   /* re-mark the saved ones in the scan list */
        s_changed = true;
    }
    if (s_forget_armed >= 0 && esp_timer_get_time() - s_forget_armed_us >= 4000000) {
        s_forget_armed = -1;
        s_changed = true;
    }
    uint32_t gen;
    muse_wifi_ap_t aps[MAX_APS];
    n = muse_wifi_scan_results(aps, MAX_APS, &gen);
    bool rows_changed = n != s_ap_n;
    for (int i = 0; !rows_changed && i < n; i++) {
        rows_changed = strcmp(aps[i].ssid, s_aps[i].ssid) != 0 || aps[i].secure != s_aps[i].secure;
    }
    layout_changed |= rows_changed;
    memcpy(s_aps, aps, n * sizeof(aps[0]));
    if (gen != s_shown_scan_gen || rows_changed) {
        bool fresh = s_shown_scan_gen != UINT32_MAX, saved_seen = false;
        s_shown_scan_gen = gen;
        s_ap_n = n;
        for (int i = 0; i < n; i++) {
            bool sv = is_saved(s_aps[i].ssid);
            saved_seen |= sv;
            strlcpy(s_ap_vals[i], sv ? "saved" : s_aps[i].secure ? "secured" : "open", sizeof(s_ap_vals[i]));
        }
        layout_changed |= s_wifi_on && s_scan_empty != (gen && !n);
        s_scan_empty = gen && !n;
        s_changed = true;
        /* The scan found one: join it now rather than at the next look. */
        if (fresh && saved_seen && w.state == MUSE_WIFI_NOT_NEARBY) muse_wifi_apply();
    }
    if (layout_changed) {
        /* FreeInk publishes new hit regions before the panel refresh finishes. */
        navigation_changed();
        s_forget_armed = -1;
        s_forget_armed_us = 0;
    }
    for (int i = 0; i < s_saved_n; i++) {
        const char *text = i == s_forget_armed ? "Tap to forget"
                           : w.state == MUSE_WIFI_CONNECTED && !strcmp(w.ssid, s_saved[i].ssid) ? "Connected"
                           : s_saved[i].hidden ? "Hidden"
                                               : "";
        SET(s_saved_vals[i], text);
    }
}

static void tick_hatch(void)
{
    char buf[128];
    const char *paired = !muse_link_hatch_linked()        ? "not paired"
                         : muse_link_sdk_token_changed() ? "paired with another SDK token, pair again to use this one"
                                                         : "paired";
    snprintf(buf, sizeof(buf), "Muse app: %s\n%s", paired, muse_link_state_name(muse_link_state()));
    bool layout_changed = SET_NOTE(s_link_status, buf);
    if (s_link_reset_armed_us && esp_timer_get_time() - s_link_reset_armed_us >= 5000000) {
        s_link_reset_armed_us = 0;
        SET(s_link_reset_lbl, "Reset pairing");
    }
    muse_hatch_status_t h;
    muse_hatch_status(&h);
    if (h.detail[0]) snprintf(buf, sizeof(buf), "%s\n%s", muse_hatch_state_name(h.state), h.detail);
    else strlcpy(buf, muse_hatch_state_name(h.state), sizeof(buf));
    layout_changed |= SET_NOTE(s_hatch_status, buf);
    if (layout_changed) {
        navigation_changed();
        s_link_reset_armed_us = 0;
        SET(s_link_reset_lbl, "Reset pairing");
    }
    char host[MUSE_HOST_MAX + 1], vm[MUSE_VM_MAX + 1];
    muse_settings_hatch_host(host);
    muse_settings_hatch_vm(vm);
    SET(s_hatch_host, host);
    SET(s_hatch_vm, vm[0] ? vm : h.vm_id[0] ? h.vm_id : "Not set");
    /* The device token is a credential, so none of it shows. */
    SET(s_hatch_token, muse_settings_hatch_token_len() ? "Set here" : muse_link_hatch_linked() ? "Set" : "Not set");
    const char *sdk = muse_link_sdk_token();
    if (sdk) snprintf(buf, sizeof(buf), "%.12s...", sdk);
    if (s_sdk_token_bad_us && esp_timer_get_time() - s_sdk_token_bad_us < 5000000) {
        SET(s_hatch_sdk, "Not a token");
    } else {
        s_sdk_token_bad_us = 0;
        SET(s_hatch_sdk, sdk ? buf : "Not set");
    }
}

static void tick_ble(void)
{
    muse_ble_status_t b;
    muse_ble_status(&b);
    char buf[96];
    switch (b.state) {
    case MUSE_BLE_OFF: strlcpy(buf, "Off", sizeof(buf)); break;
    case MUSE_BLE_ADVERTISING: snprintf(buf, sizeof(buf), "Visible as %s", b.name); break;
    case MUSE_BLE_CONNECTED:
    default: snprintf(buf, sizeof(buf), "Phone connected\n%s", b.secure ? "Paired" : "Waiting for pairing"); break;
    }
    if (SET_NOTE(s_ble_status, buf)) navigation_changed();
    set_flag(&s_ble_on, muse_settings_ble_on());
}

static void tick_sound(void)
{
    set_flag(&s_speaker_on, muse_settings_speaker_on());
    int v = muse_settings_volume(), g = muse_settings_mic_gain() / 3 * 3;
    if (v != s_volume || g != s_gain_db) {
        s_volume = v;
        s_gain_db = g;
        s_changed = true;
    }
}

static void tick_sleep(void)
{
    int cur = muse_settings_sleep_s();
    if (cur != s_sleep_cur) {
        s_sleep_cur = cur;
        s_changed = true;
    }
}

static void set_pm(char *dst, size_t cap, int pm)
{
    char buf[16] = "-";
    if (pm >= 0) snprintf(buf, sizeof(buf), "%d.%d%%", pm / 10, pm % 10);
    set_text(dst, cap, buf);
}

static void tick_battery(void)
{
    int64_t now = esp_timer_get_time();
    if (s_batt_shown_us && now - s_batt_shown_us < 30000000) return;   /* e-paper: every 30 s */
    s_batt_shown_us = now;
    muse_battery_t b;
    muse_battery_read(&b);
    muse_power_t p = muse_state_power();
    char buf[96], t[24];
    int h = (int)(b.secs / 3600), m = (int)(b.secs / 60 % 60);
    if (h) snprintf(t, sizeof(t), "%d h %d min", h, m);
    else snprintf(t, sizeof(t), "%d min", m);
    if (!b.started) strlcpy(buf, p.battery_pct < 0 ? "No battery" : "Unplug USB to start measuring.", sizeof(buf));
    else snprintf(buf, sizeof(buf), b.running ? "On battery for %s" : "Last run: %s on battery", t);
    bool layout_changed = SET_NOTE(s_batt_status, buf);

    if (p.battery_pct < 0) strlcpy(buf, "None", sizeof(buf));
    else if (p.battery_mv)
        snprintf(buf, sizeof(buf), "%s%d%%  %d.%02d V", p.charging ? LV_SYMBOL_CHARGE " " : "", p.battery_pct,
                 p.battery_mv / 1000, p.battery_mv % 1000 / 10);
    else snprintf(buf, sizeof(buf), "%s%d%%", p.charging ? LV_SYMBOL_CHARGE " " : "", p.battery_pct);
    SET(s_batt_level, buf);

    int used = b.pct_start - b.pct_now, rate10, full_h;
    if (!b.started) {
        SET(s_batt_drain, "-");
        SET(s_batt_full, "-");
    } else if (muse_battery_drain(&b, &rate10, &full_h)) {
        snprintf(buf, sizeof(buf), "%d%%, %d.%d%%/h", used, rate10 / 10, rate10 % 10);
        SET(s_batt_drain, buf);
        snprintf(buf, sizeof(buf), "lasts ~%d h", full_h);
        SET(s_batt_full, buf);
    } else {
        snprintf(buf, sizeof(buf), "%d%% so far", used > 0 ? used : 0);
        SET(s_batt_drain, buf);
        SET(s_batt_full, "measuring");
    }
    set_pm(s_batt_off, sizeof(s_batt_off), b.started ? b.screen_off_pm : -1);
    set_pm(s_batt_slept, sizeof(s_batt_slept), b.started ? b.slept_pm : -1);
    set_pm(s_batt_busy, sizeof(s_batt_busy), b.started ? b.busy_pm : -1);
    if (b.started && b.secs && b.slept_pm >= 0) {
        int per10 = (int)(b.sleeps * 10LL / b.secs);
        snprintf(buf, sizeof(buf), "%d.%d/s", per10 / 10, per10 % 10);
        SET(s_batt_wakes, buf);
    } else {
        SET(s_batt_wakes, "-");
    }
    buf[0] = 0;
    if (b.started && b.awake[0]) snprintf(buf, sizeof(buf), "Also kept awake by: %s", b.awake);
    layout_changed |= SET_NOTE(s_batt_awake, buf);
    if (layout_changed) navigation_changed();
}

bool settings_tick(void)
{
    switch (s_page) {
    case HOME: tick_home(); break;
    case WIFI: tick_wifi(); break;
    case HATCH: tick_hatch(); break;
    case BLE: tick_ble(); break;
    case SOUND: tick_sound(); break;
    case SLEEP: tick_sleep(); break;
    case BATTERY: tick_battery(); break;
    default: break;
    }
    bool changed = s_changed;
    s_changed = false;
    return changed;
}

/* ─── Pages ─────────────────────────────────────────────────────────────── */

static void title(Screen &screen, const char *text, bool back)
{
    draw_text_centered(g_w / 2, TITLE_Y, FONT_UNSCII16, text, Color::Black, 2);
    if (back) {
        /* muse_settings_ui.c's back arrow: 56 x 48, left of the title. */
        Rect r = BACK;
        bool pressed = screen.frame().stateFor(ACT_BACK, 0) & freeink::ui::StateFocused;
        if (pressed) screen.target().fill(r, Paint::solid(Color::Black), 12);
        draw_text_centered(r.x + r.width / 2, r.y + (r.height - line_height(FONT_M20)) / 2, FONT_M20, LV_SYMBOL_LEFT,
                           pressed ? Color::White : Color::Black);
        screen.frame().hit(r, ACT_BACK);
    }
}

static void page_home(Column &c)
{
    row(c, LV_SYMBOL_WIFI, "Wi-Fi", s_home_wifi, ACT_NAV, WIFI);
    row(c, LV_SYMBOL_HOME, "Muse", s_home_hatch, ACT_NAV, HATCH);
    row(c, LV_SYMBOL_BLUETOOTH, "Bluetooth", s_home_ble, ACT_NAV, BLE);
    row(c, LV_SYMBOL_VOLUME_MAX, "Sound", s_home_sound, ACT_NAV, SOUND);
    row(c, LV_SYMBOL_EYE_CLOSE, "Sleep", s_home_sleep, ACT_NAV, SLEEP);
    row(c, LV_SYMBOL_BATTERY_FULL, "Battery", s_home_battery, ACT_NAV, BATTERY);
    if (muse_board->power_off) row(c, LV_SYMBOL_POWER, "Power off", nullptr, ACT_NAV, POWER);
    note(c, s_about);
}

static void page_wifi(Column &c)
{
    switch_row(c, "Wi-Fi", s_wifi_on, ACT_WIFI_SW);
    note(c, s_wifi_status);
    if (s_saved_n > 0) note(c, "Saved networks");
    for (int i = 0; i < s_saved_n; i++) row(c, LV_SYMBOL_WIFI, s_saved[i].ssid, s_saved_vals[i], ACT_WIFI_SAVED, (int16_t)i);
    if (s_wifi_on) {
        button(c, s_wifi_scanning ? "Scanning..." : LV_SYMBOL_REFRESH "  Scan for networks", ACT_WIFI_SCAN);
        for (int i = 0; i < s_ap_n; i++) row(c, nullptr, s_aps[i].ssid, s_ap_vals[i], ACT_WIFI_AP, (int16_t)i);
        if (s_scan_empty) note(c, "No networks found");
    }
    row(c, LV_SYMBOL_EDIT, "Other network...", nullptr, ACT_WIFI_OTHER);
    if (!s_mac[0]) {
        uint8_t m[6];
        if (esp_read_mac(m, ESP_MAC_WIFI_STA) == ESP_OK) {
            snprintf(s_mac, sizeof(s_mac), "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
        }
    }
    info_row(c, "MAC address", s_mac);
    note(c, "Muse remembers up to 8 networks and joins the strongest one in range. Tap a saved one twice to forget it.");
}

static void page_hatch(Column &c)
{
    note(c, s_link_status);
    button(c, s_link_reset_lbl, ACT_HATCH_RESET);
    note(c, s_hatch_status);
    row(c, nullptr, "Server", s_hatch_host, ACT_HATCH_HOST);
    row(c, nullptr, "VM ID", s_hatch_vm, ACT_HATCH_VM);
    row(c, nullptr, "Device token", s_hatch_token, ACT_HATCH_TOKEN);
    row(c, nullptr, "SDK token", s_hatch_sdk, ACT_HATCH_SDK);
    button(c, "Test connection", ACT_HATCH_TEST);
    note(c, "Pair with the Muse app to use your account; a device token here overrides it, and a long one is "
            "easier to send over Bluetooth. The VM ID picks one of your VMs. The SDK token is from "
            "gadgets.muse.ai; one set here replaces the firmware's. "
            "Reset pairing forgets Wi-Fi and the app pairing, then restarts.");
}

static void page_ble(Column &c)
{
    switch_row(c, "Phone setup", s_ble_on, ACT_BLE_SW);
    note(c, s_ble_status);
    button(c, "Forget paired phones", ACT_BLE_FORGET);
    note(c, "When on, Muse is visible to phones nearby. Open tools/ble_setup.html in Chrome, "
            "connect, and enter the code Muse shows to pair.");
}

static void page_sound(Column &c)
{
    char v[16];
    if (muse_board->audio_init) {
        switch_row(c, "Speaker", s_speaker_on, ACT_SPK_SW);
        snprintf(v, sizeof(v), "%d%%", s_volume);
        /* Volume in 5% stops: twenty taps' worth along the bar. */
        slider(c, "Volume", v, 0, 20, s_volume / 5, ACT_VOLUME);
    }
    snprintf(v, sizeof(v), "%d dB", s_gain_db);
    slider(c, "Mic gain", v, 0, MUSE_MIC_GAIN_MAX / 3, s_gain_db / 3, ACT_GAIN);
}

static void page_sleep(Column &c)
{
    note(c, "Turn the screen off after Muse has been idle for:");
    for (int i = 0; i < SLEEP_COUNT; i++) {
        bool cur = SLEEP_CHOICES[i] == s_sleep_cur;
        row(c, nullptr, SLEEP_NAMES[i], cur ? LV_SYMBOL_OK : "", ACT_SLEEP_CHOICE, (int16_t)i, cur);
    }
    button(c, LV_SYMBOL_EYE_CLOSE "  Sleep now", ACT_SLEEP_NOW);
    note(c, "Tap the screen or press either button to wake.");
}

static void page_battery(Column &c)
{
    note(c, s_batt_status);
    info_row(c, "Battery", s_batt_level);
    info_row(c, "Used", s_batt_drain);
    info_row(c, "A full charge", s_batt_full);
    info_row(c, "Screen off", s_batt_off);
    info_row(c, "Chip asleep", s_batt_slept);
    info_row(c, "Wakes", s_batt_wakes);
    info_row(c, "CPU busy", s_batt_busy);
    note(c, s_batt_awake);
    button(c, LV_SYMBOL_REFRESH "  Start over", ACT_BATT_RESET);
    note(c, "Measures from unplugging USB until it's plugged back in. The gauge moves in 1% steps, so give it a "
            "few hours. Chip asleep is time in light sleep; CPU busy is time a core was running a task.");
}

static void page_power(Column &c)
{
    note(c, "Power Muse off completely?");
    button(c, LV_SYMBOL_POWER "  Power off", ACT_POWER_OFF);
    button(c, "Cancel", ACT_BACK);
    char text[128];
    const char *power = muse_board->power_button ? muse_board->power_button : muse_board->talk_button;
    const char *sleep = muse_board->power_button ? muse_board->power_button : muse_board->aux_button;
    if (sleep) {
        snprintf(text, sizeof(text),
                 "Press the %s button to turn it back on. To just turn the screen off, press the %s button.", power,
                 sleep);
    } else {
        snprintf(text, sizeof(text), "Press the %s button to turn it back on.", power);
    }
    note(c, text);
}

/* Text entry, laid out as muse_settings_ui.c's for a screen 2" or more:
 * title over a one-line field, a Show button beside it for a password, and
 * the keyboard below. */
static void page_text(Screen &screen)
{
    auto &t = screen.target();
    title(screen, "", true);
    char shown[SHOWN_MAX];
    fit(shown, sizeof(shown), s_text_title, FONT_M20, 150);
    draw_text_centered(g_w / 2, BACK.y + (BACK.height - line_height(FONT_M20)) / 2, FONT_M20, shown);

    /* The field under the header, the keyboard at the bottom. */
    const int field_y = LIST_TOP;
    const int w = 300, show_w = 70, gap = 8, h = 48;
    int fx = g_w / 2 - w / 2, fw = s_text_password ? w - show_w - gap : w;
    Rect field{(int16_t)fx, (int16_t)field_y, (int16_t)fw, (int16_t)h};
    t.stroke(field, Paint::solid(Color::Black), 2, 14);
    const int ty = field_y + (h - line_height(FONT_M20)) / 2;
    if (s_entry.empty()) {
        draw_text(fx + 14, field_y + (h - line_height(FONT_M16)) / 2, FONT_M16, s_text_hint, Color::DarkGray);
    }
    /* The end of the text, if it's longer than the field, then the cursor. */
    char vis[128];
    size_t len = strlen(s_text);
    if (s_text_password && !s_text_reveal) {
        /* A star a character, not a byte. */
        size_t n = 0;
        for (const char *p = s_text; *p && n < sizeof(vis) - 1; p++) {
            if (((unsigned char)*p & 0xC0) != 0x80) vis[n++] = '*';
        }
        vis[n] = 0;
    } else {
        const char *tail = len < sizeof(vis) ? s_text : s_text + len - (sizeof(vis) - 1);
        while (((unsigned char)*tail & 0xC0) == 0x80) tail++;   /* whole characters */
        strlcpy(vis, tail, sizeof(vis));
    }
    /* The end of the text, if it's longer than the field, a whole character at a time. */
    const char *start = vis;
    while (*start && text_width(FONT_M20, start) > fw - 28 - 4) {
        do start++; while (((unsigned char)*start & 0xC0) == 0x80);
    }
    draw_text(fx + 14, ty, FONT_M20, start);
    int cx = fx + 14 + text_width(FONT_M20, start);
    t.fill(Rect{(int16_t)cx, (int16_t)(ty + 2), 2, (int16_t)(line_height(FONT_M20) - 4)}, Paint::solid(Color::Black));
    if (s_text_password) {
        Rect b{(int16_t)(fx + w - show_w), (int16_t)field_y, (int16_t)show_w, (int16_t)h};
        bool pressed = screen.frame().stateFor(ACT_TEXT_SHOW, 0) & freeink::ui::StateFocused;
        if (pressed) t.fill(b, Paint::solid(Color::Black), 14);
        else t.stroke(b, Paint::solid(Color::Black), 2, 14);
        draw_text_centered(b.x + show_w / 2, b.y + (h - line_height(FONT_M16)) / 2, FONT_M16,
                           s_text_reveal ? "Hide" : "Show", pressed ? Color::White : Color::Black);
        screen.frame().hit(b, ACT_TEXT_SHOW);
    }

    /* FreeInk's keyboard in the LVGL keyboard's size and font, anchored to
     * the bottom of the screen, under the thumbs. */
    const int kb_w = g_w - 16, kb_h = kb_w * 206 / 384, kb_y = g_h - kb_h - 8;
    freeink::ui::KeyboardProps kb;
    kb.layout = &keyboard_for_frame();
    kb.langAction = ACT_KB_LANG;
    kb.keyAction = ACT_KB_KEY;
    kb.shiftAction = ACT_KB_SHIFT;
    kb.modeAction = ACT_KB_MODE;
    kb.deleteAction = ACT_KB_DEL;
    kb.okAction = ACT_KB_OK;
    kb.inputMask = freeink::ui::InputTouch | freeink::ui::InputLongPress;
    kb.labelText.font = FONT_M20;
    kb.altText.font = FONT_UNSCII8;
    kb.controlText.font = FONT_M16;
    kb.padding = {2, 2, 2, 2};
    kb.gap = 4;
    kb.rowGap = 4;
    kb.keyRadius = 8;
    kb.bottomHitOverflow = 8;
    freeink::ui::keyboard(screen.frame(), Rect{8, (int16_t)kb_y, (int16_t)kb_w, (int16_t)kb_h}, kb);
}

void settings_screen(Screen &screen)
{
    if (s_page == TEXT) {
        page_text(screen);
        return;
    }
    static const char *const TITLES[PAGE_COUNT] = {"SETTINGS", "WI-FI", "MUSE", "BLUETOOTH", "SOUND",
                                                   "AUTO-SLEEP", "BATTERY", "POWER", ""};
    title(screen, TITLES[s_page], s_page != HOME);
    Column c{&screen, g_w / 2 - LIST_W / 2, LIST_TOP - s_scroll};
    screen.target().setClipRect(Rect{0, LIST_TOP, (int16_t)g_w, (int16_t)(g_h - LIST_TOP)});
    switch (s_page) {
    case HOME: page_home(c); break;
    case WIFI: page_wifi(c); break;
    case HATCH: page_hatch(c); break;
    case BLE: page_ble(c); break;
    case SOUND: page_sound(c); break;
    case SLEEP: page_sleep(c); break;
    case BATTERY: page_battery(c); break;
    case POWER: page_power(c); break;
    default: break;
    }
    screen.target().setClipRect(Rect{0, 0, (int16_t)g_w, (int16_t)g_h});
    s_content_h = c.y + s_scroll - LIST_TOP;
}

void settings_setup(App &app)
{
    for (ActionId a = ACT_NAV; a < ACT_LAST; a++) app.on(a, on_action);
    show(HOME);
    settings_tick();
}

}  // namespace muse_eink
