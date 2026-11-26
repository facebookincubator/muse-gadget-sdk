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

#include "muse_home.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#include "muse_battery.h"
#include "muse_ble.h"
#include "muse_board.h"
#include "muse_clock.h"
#include "muse_icons.h"
#include "muse_input.h"
#include "muse_link.h"
#include "muse_settings.h"
#include "muse_state.h"
#include "muse_status_conn.h"
#include "muse_text.h"
#include "muse_touch.h"
#include "muse_type.h"
#include "muse_ui.h"
#include "muse_vol_show.h"
#include "muse_wifi.h"
#if CONFIG_MUSE_GADGET_BLE_AUDIO
#include "mg_ble.h"
#endif

/* Palette. */
#define COLOR_TEXT 0xf2efff
#define COLOR_DIM 0x8b84a8
#define COLOR_CARD 0x1a1530
#define COLOR_OK 0x6ff0bf
#define COLOR_CONNECTED 0x2fbf71
#define COLOR_SPEAKER 0x2e2552
#define COLOR_MUTED 0x8e2426
#define COLOR_BATTERY 0xc8c8c8
#define COLOR_BATTERY_LOW 0xe53935
#define COLOR_ACCENT 0xa77dff

#define TALK_HOLD_MS 250        /* the hold threshold, as muse_voice.c's */
#define CAPTION_SHOW_MS 3000    /* a caption holds the state line this long */
#define SLIDER_SHOW_MS 3000     /* the volume slider closes this long after the last touch */
#define LOW_BATTERY_WARN 15     /* the battery icon turns red */

static muse_home_geom_t s_g;
static int s_w, s_h;
static const char *TAG = "muse_home";

static lv_obj_t *s_root;        /* everything here, to hide under a reply's page */
static lv_obj_t *s_batt, *s_batt_fill, *s_batt_nub;
static lv_obj_t *s_bolt;   /* the charging bolt while external power is connected */
static lv_obj_t *s_time, *s_line, *s_name;
static lv_obj_t *s_hold;
static lv_obj_t *s_spk, *s_spk_icon;
static lv_obj_t *s_slider_box, *s_slider, *s_slider_val;
static lv_obj_t *s_slider_card, *s_slider_by;   /* "Set by phone" under the level, for an outside change */
static uint32_t s_slider_touched;
static int s_line_caps_top;   /* the state line's capitals' top */
static uint32_t s_line_color = UINT32_MAX;   /* its color as last set (a rebuild resets it) */
static lv_obj_t *s_state;     /* not set up: "Open Muse app" */
static bool s_token = true;   /* a device token is stored (the last tick's) */

static uint32_t s_caption_version;
static char s_caption[96];
static uint32_t s_caption_at;   /* lv_tick of the last fresh caption */
static bool s_caption_turn;     /* it came during a turn's thinking or reply: reply text */
static bool s_talks;
static bool s_locked;           /* the passcode's locked face */
static lv_obj_t *s_lock;        /* the lock glyph in the page dots' place */

static struct {
    bool down, talking, cancelled;
    lv_point_t at;
    uint32_t t0;
} s_press;


static int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

/* Python's round(), half to even, as the reference's renderer rounds. */
static int pr(double v)
{
    return (int)nearbyint(v);
}

/*
 * Muse's ink in its 64 x 64 canvas: rows 8 to 60 of the idle frame
 * (from avatar/muse_pixel.c), which the layout centers between the date and the page dots.
 */
#define MUSE_INK_ROW0 8
#define MUSE_INK_ROW1 60

void muse_home_geom(int w, int h, bool round, int corner_px, muse_home_geom_t *g)
{
    (void)corner_px;
    int s = w < h ? w : h;
    double k = s / 466.0;
    memset(g, 0, sizeof(*g));
    g->short_side = s;
    g->top = 0;
    g->ring = clampi(pr(0.022 * s), 2, 1000);
    int rim_margin = g->ring + clampi(pr(0.01 * s), 2, 1000);   /* buttons clear of the mic ring */
    /* The top row: the battery 7.7 % of S wide, half as tall. */
    g->batt_w = pr(36 * k);
    g->batt_h = pr(g->batt_w / 2.0);
    g->batt_stroke = clampi(pr(2 * k), 1, 100);
    g->batt_r = clampi(pr(3 * k), 1, 100);
    g->batt_gap = clampi(pr(2 * k), 1, 100);
    g->nub_w = clampi(pr(4 * k), 2, 100);
    g->nub_h = clampi(pr(8 * k), 3, 100);
    g->nub_gap = clampi(pr(1 * k), 1, 100);
    g->batt_top = clampi(pr(22 * k), g->ring + 3, 1000);
    g->top_gap = clampi(pr(0.015 * s), 3, 100);
    /* The time 9.9 % of S, the date at 1.64 : 1 (the reference home's glyphs). */
    g->time_digits = pr(46 * k);
    g->date_caps = clampi(pr(g->time_digits / (46.0 / 28.0)), 9, 1000);
    g->time_top = g->batt_top + g->batt_h + pr(17 * k);
    g->date_top = g->time_top + g->time_digits + pr(24 * k);
    int date_bot = g->date_top + g->date_caps;
    /* Muse: the largest whole cell in 50-55 % of S, else in 45-55 %. */
    int cell = 0;
    for (int c = 1; c < 12; c++) {
        if (0.50 * s <= 64 * c && 64 * c <= 0.55 * s) cell = c;
    }
    for (int c = 1; !cell && c < 12; c++) {
        if (0.45 * s <= 64 * c && 64 * c <= 0.55 * s) cell = c;
    }
    if (!cell) cell = clampi((int)(0.55 * s / 64), 1, 11);
    g->muse_px = 64 * cell;
    g->hold_d = pr(216.0 / 466.0 * s);
    g->button_d = clampi((int)ceil(92 * k), 44, 1000);
    g->dot_d = clampi(pr(8 * k), 4, 100);
    g->dot_pitch = clampi(pr(20 * k), 10, 100);
    g->dot_cy = h - pr(31.5 * k);
    /* Speaker beside Muse, on its center line, the hold disc 10 px clear of
     * it (the spot mirrored on the right stays clear too, the layout
     * symmetric); Muse's ink centered between the date and the dots, then
     * raised where needed to keep the button inside the mic ring. */
    double d = g->button_d;
    double dx = g->hold_d / 2.0 + 10 + d / 2.0;
    if (dx < 164.5 / 466.0 * s) dx = 164.5 / 466.0 * s;
    double ink_top = MUSE_INK_ROW0 * cell - g->muse_px / 2.0, ink_bot = (MUSE_INK_ROW1 + 1) * cell - g->muse_px / 2.0;
    double dots_top = g->dot_cy - g->dot_d / 2.0;
    double ym = (date_bot + dots_top) / 2.0 - (ink_top + ink_bot) / 2.0;
    if (round) {
        double lim = s / 2.0 - rim_margin - d / 2.0;
        double ym_max = lim > dx ? s / 2.0 + sqrt(lim * lim - dx * dx) : -1;
        if (ym > ym_max) ym = ym_max;
    }
    ym = nearbyint(ym * 2) / 2.0;
    g->muse_y = (int)floor(ym - h / 2.0 + 0.5);
    /* Not set up: "Open Muse app" in the
     * small role centered on the time's digits; the name on the date line. */
    g->state_caps = pr(19.0 / 466.0 * s);
    g->state_top = (int)floor(g->time_top + g->time_digits / 2.0 - g->state_caps / 2.0 + 0.5);
    g->name_top = g->date_top;
    g->button_y = g->muse_y;
    g->button_x = pr(dx);
}

/* ---- the top row: Wi-Fi, Bluetooth, battery ----------------------- */

typedef enum { TOP_OFF, TOP_FLASH, TOP_ON } top_state_t;

static lv_obj_t *s_top_wifi, *s_top_bt;
static top_state_t s_wifi_state = TOP_OFF, s_bt_state = TOP_OFF;
/* A muted speaker (speaker_volume 0) shows in
 * the top row, before Wi-Fi and Bluetooth, for as long as it lasts. */
static lv_obj_t *s_top_mute;
static bool s_muted;
static lv_timer_t *s_flash_timer;
static bool s_flash_on = true;

/* The board's Muse build uses Wi-Fi (the BLE-only builds never show it).
 * Weak: the simulator says per board. */
__attribute__((weak)) bool muse_home_wifi_build(void)
{
#if CONFIG_MUSE_GADGET_BLE_STANDALONE
    return false;
#else
    return true;
#endif
}

static void build_battery(lv_obj_t *p, int x, int y)
{
    const muse_home_geom_t *g = &s_g;
    s_batt = lv_obj_create(p);
    lv_obj_remove_style_all(s_batt);
    lv_obj_set_size(s_batt, g->batt_w, g->batt_h);
    lv_obj_set_style_border_width(s_batt, g->batt_stroke, 0);
    lv_obj_set_style_border_color(s_batt, lv_color_hex(COLOR_BATTERY), 0);
    lv_obj_set_style_radius(s_batt, g->batt_r, 0);
    lv_obj_set_style_pad_all(s_batt, g->batt_gap, 0);
    lv_obj_remove_flag(s_batt, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(s_batt, x, y);
    s_batt_fill = lv_obj_create(s_batt);
    lv_obj_remove_style_all(s_batt_fill);
    lv_obj_set_style_bg_opa(s_batt_fill, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_batt_fill, clampi(g->batt_r - g->batt_stroke - 1, 0, 100), 0);
    lv_obj_set_height(s_batt_fill, lv_pct(100));
    lv_obj_set_width(s_batt_fill, 0);
    lv_obj_remove_flag(s_batt_fill, LV_OBJ_FLAG_CLICKABLE);
    s_batt_nub = lv_obj_create(p);
    lv_obj_remove_style_all(s_batt_nub);
    lv_obj_set_size(s_batt_nub, g->nub_w, g->nub_h);
    lv_obj_set_style_bg_opa(s_batt_nub, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_batt_nub, lv_color_hex(COLOR_BATTERY), 0);
    lv_obj_remove_flag(s_batt_nub, LV_OBJ_FLAG_CLICKABLE);
}

static lv_obj_t *top_icon(lv_obj_t *p, muse_icon_t icon)
{
    const lv_image_dsc_t *d = muse_icon_ink(icon, s_g.batt_h);
    if (!d) {
        return NULL;
    }
    lv_obj_t *img = lv_image_create(p);
    lv_image_set_src(img, d);
    lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_image_recolor(img, lv_color_hex(COLOR_BATTERY), 0);
    lv_obj_set_style_image_recolor_opa(img, LV_OPA_COVER, 0);
    lv_obj_add_flag(img, LV_OBJ_FLAG_HIDDEN);
    return img;
}

/* Centers the icons that are there as one group (an absent one re-centers
 * it; a flashing one keeps its slot, so the battery never moves). */
static void top_layout(void)
{
    const muse_home_geom_t *g = &s_g;
    lv_obj_t *icons[3] = { s_muted ? s_top_mute : NULL, s_wifi_state != TOP_OFF ? s_top_wifi : NULL,
                           s_bt_state != TOP_OFF ? s_top_bt : NULL };
    int total = g->batt_w + g->nub_gap + g->nub_w;
    for (int i = 0; i < 3; i++) {
        if (icons[i]) total += lv_image_get_src_width(icons[i]) + g->top_gap;
    }
    int x = (int)lround(s_w / 2.0 - total / 2.0);
    for (int i = 0; i < 3; i++) {
        if (!icons[i]) continue;
        int iw = lv_image_get_src_width(icons[i]), ih = lv_image_get_src_height(icons[i]);
        lv_obj_set_pos(icons[i], x, g->batt_top + (g->batt_h - ih) / 2);
        x += iw + g->top_gap;
    }
    lv_obj_set_pos(s_batt, x, g->batt_top);
    lv_obj_set_pos(s_batt_nub, x + g->batt_w + g->nub_gap, g->batt_top + (g->batt_h - g->nub_h) / 2);
    if (s_bolt) {
        /* Centered on the battery, 5 px (at 360) after its nub, outside
         * the centered group (it never moves the group); the gap shrinks (to
         * 2 px at least) where the bolt's ink would come within 2 px of the
         * mic ring's band (the Watcher: 2 px). */
        const lv_image_dsc_t *d = lv_image_get_src(s_bolt);
        int box = d->header.w;
        int y = g->batt_top + (g->batt_h - box) / 2;
        int gap = (int)lround(5.0 * g->short_side / 360.0);
        int x0 = x + g->batt_w + g->nub_gap + g->nub_w;
        /* The ink's clearance from the ring band: radial on a round panel,
         * horizontal (safe_half) on a rectangle with rounded corners. */
        for (; gap > 2; gap--) {
            double worst = 1e9;
            for (int r = 0; r < (int)d->header.h; r++) {
                for (int c = 0; c < (int)d->header.w; c++) {
                    if (d->data[r * d->header.stride + c] < 32) continue;
                    double px = x0 + gap + c + 0.5, py = y + r + 0.5, clear;
                    if (muse_board->round) {
                        double R = g->short_side / 2.0 - g->ring;
                        clear = R - hypot(px - s_w / 2.0, py - s_h / 2.0);
                    } else {
                        clear = s_w / 2.0 + muse_home_safe_half(py, g->ring) - px;
                    }
                    if (clear < worst) worst = clear;
                }
            }
            if (worst >= 2) break;
        }
        lv_obj_set_pos(s_bolt, x0 + gap, y);
    }
}

static void top_show(void)
{
    if (s_top_mute) {
        lv_obj_set_flag(s_top_mute, LV_OBJ_FLAG_HIDDEN, !s_muted);   /* steady, never flashing */
    }
    if (s_top_wifi) {
        lv_obj_set_flag(s_top_wifi, LV_OBJ_FLAG_HIDDEN,
                        s_wifi_state == TOP_OFF || (s_wifi_state == TOP_FLASH && !s_flash_on));
    }
    if (s_top_bt) {
        lv_obj_set_flag(s_top_bt, LV_OBJ_FLAG_HIDDEN, s_bt_state == TOP_OFF || (s_bt_state == TOP_FLASH && !s_flash_on));
    }
}

/* 1 Hz, 500 ms on / 500 ms off; only the icon's box redraws, and LVGL's
 * timers stop while the screen sleeps. */
static void on_flash(lv_timer_t *t)
{
    (void)t;
    s_flash_on = !s_flash_on;
    top_show();
}

static void top_tick(void)
{
    top_state_t bt = TOP_OFF, wifi = TOP_OFF;
#if CONFIG_MUSE_GADGET_BLE_AUDIO
    muse_ble_status_t b;
    muse_ble_status(&b);
    if (b.state != MUSE_BLE_OFF) {
        bt = mg_ble_link() == MG_LINK_READY ? TOP_ON : TOP_FLASH;   /* advertising, or the app not ready yet */
    }
#else
    muse_ble_status_t b;
    muse_ble_status(&b);
    bt = b.state == MUSE_BLE_OFF ? TOP_OFF : b.state == MUSE_BLE_CONNECTED ? TOP_ON : TOP_FLASH;
#endif
    if (s_top_wifi && s_token) {   /* no Wi-Fi icon before setup */
        muse_wifi_status_t w;
        muse_wifi_status(&w);
        wifi = w.state == MUSE_WIFI_CONNECTED                                     ? TOP_ON
               : w.state == MUSE_WIFI_OFF || w.state == MUSE_WIFI_NO_NETWORK ? TOP_OFF
                                                                             : TOP_FLASH;
    }
    bool muted = s_top_mute && muse_vol_muted(muse_settings_speaker_on(), muse_settings_volume());
    bool relayout = (bt == TOP_OFF) != (s_bt_state == TOP_OFF) || (wifi == TOP_OFF) != (s_wifi_state == TOP_OFF) ||
                    muted != s_muted;
    if (muted != s_muted) {
        ESP_LOGI(TAG, "top row: muted icon %s", muted ? "on" : "off");
    }
    s_bt_state = bt;
    s_wifi_state = wifi;
    s_muted = muted;
    if (relayout) {
        top_layout();
    }
    bool flashing = bt == TOP_FLASH || wifi == TOP_FLASH;
    if (flashing && !s_flash_timer) {
        s_flash_on = true;
        s_flash_timer = lv_timer_create(on_flash, 500, NULL);
    } else if (!flashing && s_flash_timer) {
        lv_timer_delete(s_flash_timer);
        s_flash_timer = NULL;
        s_flash_on = true;
    }
    top_show();
}

static void build_top_row(lv_obj_t *p)
{
    build_battery(p, 0, s_g.batt_top);
    if (muse_home_wifi_build()) {
        s_top_wifi = top_icon(p, MUSE_ICON_WIFI);
    }
    s_top_bt = top_icon(p, MUSE_ICON_BLUETOOTH);
    s_top_mute = muse_board->audio_init ? top_icon(p, MUSE_ICON_VOLUME_OFF) : NULL;   /* a muted speaker */
    s_muted = false;
    /* The charging bolt: an 18 px box at 360 px (5 % of S), #C8C8C8. */
    s_bolt = NULL;
    const lv_image_dsc_t *bolt = muse_icon(MUSE_ICON_CHARGING, (int)lround(18.0 * s_g.short_side / 360.0));
    if (bolt) {
        s_bolt = lv_image_create(p);
        lv_image_set_src(s_bolt, bolt);
        lv_obj_remove_flag(s_bolt, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_image_recolor(s_bolt, lv_color_hex(COLOR_BATTERY), 0);
        lv_obj_set_style_image_recolor_opa(s_bolt, LV_OPA_COVER, 0);
        lv_obj_add_flag(s_bolt, LV_OBJ_FLAG_HIDDEN);
    }
    top_layout();
}

static void tick_battery(void)
{
    static int shown_pct = -2, shown_state = -1;
    muse_power_t p = muse_state_power();
    bool none = p.battery_pct < 0;
    lv_obj_set_flag(s_batt, LV_OBJ_FLAG_HIDDEN, none);
    lv_obj_set_flag(s_batt_nub, LV_OBJ_FLAG_HIDDEN, none);
    if (s_bolt) {
        /* Keyed on external power (VBUS), charging or full. */
        lv_obj_set_flag(s_bolt, LV_OBJ_FLAG_HIDDEN, none || !p.usb);
    }
    if (none) {
        return;
    }
    /* Never green: #C8C8C8, red at <= 15 % on battery only. */
    bool low = !p.usb && (muse_battery_low_level() >= MUSE_BATTERY_LEVEL_WARN15 || p.battery_pct <= LOW_BATTERY_WARN);
    int state = low ? 2 : 0;
    if (p.battery_pct != shown_pct || state != shown_state) {
        int inner = lv_obj_get_content_width(s_batt);
        lv_obj_set_width(s_batt_fill, inner * clampi(p.battery_pct, 4, 100) / 100);
        uint32_t c = state == 2 ? COLOR_BATTERY_LOW : COLOR_BATTERY;
        lv_obj_set_style_bg_color(s_batt_fill, lv_color_hex(c), 0);
        shown_pct = p.battery_pct;
        shown_state = state;
    }
}

/* The battery policy's warnings (muse_battery.c), from the input
 * task: "Low battery" once at 10 %, and a word before the clean shutdown. */
void muse_ui_notify_low_battery(int pct, muse_battery_level_t level)
{
    (void)pct;
    if (level == MUSE_BATTERY_LEVEL_WARN10) {
        muse_state_set_caption("Low battery");
    } else if (level == MUSE_BATTERY_LEVEL_CRITICAL) {
        muse_state_set_caption("Battery empty, turning off");
    }
}

/* ---- the screen's hold region ------------------------------ */

static bool screen_talks(void)
{
    return s_talks && (!muse_input_talk_choice() || muse_settings_talk_mode() != MUSE_TALK_BUTTON);
}

/*
 * A tap on the idle home, on Muse or anywhere but Speaker, plays the happy
 * bounce (muse_state_make_happy: the avatar's hop and
 * hearts, once, 1.6 s). It sends nothing and opens no mic. During a turn,
 * a realtime voice call or a setup prompt a tap does what that screen does (their own
 * screens take it; a turn on the home ignores it).
 */
static void happy_tap(void)
{
    if (muse_state_mode(NULL) != MUSE_MODE_IDLE) {
        return;
    }
    muse_state_make_happy();
    if (!s_talks && muse_link_token_stored()) {
        muse_state_set_caption("Open Muse app");   /* set up, the app not ready (not set up: the home says so) */
    }
}

static void on_home_tap(lv_event_t *e)
{
    (void)e;
    happy_tap();
}

static void hold_release(void)
{
    if (s_press.talking) {
        ESP_LOGI(TAG, "hold region: the lift ends the turn");
    }
    if (s_press.down && !s_press.cancelled) {
        muse_input_touch_talk(false);
    }
    if (s_press.talking) {
        muse_ui_set_swipe_enabled(true);
    }
    s_press.down = s_press.talking = s_press.cancelled = false;
}

static void on_hold(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *indev = lv_indev_active();
    lv_point_t p = { 0 };
    if (indev) {
        lv_indev_get_point(indev, &p);
    }
    switch (code) {
    case LV_EVENT_SHORT_CLICKED:
        if (!screen_talks()) {
            happy_tap();   /* talking is on the button (Settings > Talk), or nowhere yet */
        }
        return;
    case LV_EVENT_PRESSED:
        if (!screen_talks()) {
            return;   /* no mic: the release is a tap or nothing */
        }
        if (!muse_input_talk_ok(muse_touch_real())) {
            ESP_LOGW(TAG, "hold region: a bench press: real press needed");   /* an injected one, on a release image */
            return;
        }
        s_press.down = true;
        s_press.talking = s_press.cancelled = false;
        s_press.at = p;
        s_press.t0 = lv_tick_get();
        if (muse_touch_real()) {
            muse_input_press(MUSE_PRESS_PANEL);   /* a finger's press (a bench press counts for nothing) */
        }
        muse_input_touch_talk(true);   /* capture from the touch-down: the pre-roll */
        break;
    case LV_EVENT_PRESSING:
        if (!s_press.down || s_press.cancelled || s_press.talking) {
            return;
        }
        if (muse_ui_touch_swiped()) {
            /* A swipe before the threshold (past the slop, muse_swipe.h, or a
             * page scrolling with it): never a hold, nothing is sent
             * (the voice task sees a release under 250 ms). Once talking,
             * drift is fine and swipes are off until release. */
            s_press.cancelled = true;
            muse_input_touch_talk(false);
            ESP_LOGI(TAG, "hold region: a swipe before the hold, no talk");
        } else if (lv_tick_elaps(s_press.t0) >= TALK_HOLD_MS) {
            s_press.talking = true;   /* drift is fine from here, and the face stays put */
            muse_ui_set_swipe_enabled(false);
            ESP_LOGI(TAG, "hold region: talking (held %d ms; drift is fine, swipes off until the lift)", TALK_HOLD_MS);
        }
        break;
    case LV_EVENT_RELEASED:
    case LV_EVENT_PRESS_LOST:
        if (code == LV_EVENT_RELEASED && s_press.down && !s_press.cancelled && !s_press.talking) {
            muse_input_note_touch_tap();   /* before the release goes out: the voice task bounces */
        }
        hold_release();
        break;
    default:
        break;
    }
}

bool muse_home_holding(void)
{
    return s_press.talking;
}

/* ---- Speaker: tap = volume slider, long press = mute --------------- */

static void show_speaker(void)
{
    bool on = muse_settings_speaker_on();
    lv_obj_set_style_bg_color(s_spk, lv_color_hex(on ? COLOR_SPEAKER : COLOR_MUTED), 0);
    muse_icon_set(s_spk_icon, on ? MUSE_ICON_VOLUME_UP : MUSE_ICON_VOLUME_OFF,
                  muse_icon_button_glyph(s_g.button_d), 0xffffff);
}

static void slider_close(void)
{
    if (s_slider_box) {
        lv_obj_add_flag(s_slider_box, LV_OBJ_FLAG_HIDDEN);
    }
}

static void on_slider(lv_event_t *e)
{
    int v = lv_slider_get_value(s_slider);
    char buf[16];
    snprintf(buf, sizeof(buf), "%d%%", v);
    lv_label_set_text(s_slider_val, buf);
    s_slider_touched = lv_tick_get();
    if (lv_event_get_code(e) == LV_EVENT_RELEASED) {
        muse_settings_set_volume(v);
        if (v > 0 && !muse_settings_speaker_on()) muse_settings_set_speaker_on(true);
        if (v == 0 && muse_settings_speaker_on()) muse_settings_set_speaker_on(false);   /* 0 is muted */
    }
}

static void on_slider_backdrop(lv_event_t *e)
{
    if (lv_event_get_target(e) == s_slider_box) {
        slider_close();   /* a tap beside the card closes it */
    }
}

static void build_slider(void)
{
    s_slider_box = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_slider_box);
    lv_obj_set_size(s_slider_box, s_w, s_h);
    lv_obj_add_flag(s_slider_box, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_slider_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_slider_box, on_slider_backdrop, LV_EVENT_CLICKED, NULL);
    float k = s_g.short_side / 466.0f;
    lv_obj_t *card = s_slider_card = lv_obj_create(s_slider_box);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, (int)lroundf(300 * k), (int)lroundf(96 * k));
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(COLOR_CARD), 0);
    lv_obj_set_style_radius(card, (int)lroundf(24 * k), 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(card, LV_ALIGN_CENTER, 0, s_g.button_y);
    s_slider_val = lv_label_create(card);
    lv_obj_set_style_text_font(s_slider_val, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_slider_val, lv_color_hex(COLOR_TEXT), 0);
    lv_obj_align(s_slider_val, LV_ALIGN_TOP_MID, 0, (int)lroundf(10 * k));
    /* Who changed it, when that wasn't the gadget ("Set by phone"),
     * under the level; the card grows by its line while it shows. */
    s_slider_by = lv_label_create(card);
    lv_obj_set_style_text_font(s_slider_by, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_slider_by, lv_color_hex(COLOR_DIM), 0);
    lv_label_set_text(s_slider_by, "");
    lv_obj_align(s_slider_by, LV_ALIGN_TOP_MID, 0,
                 (int)lroundf(10 * k) + lv_font_get_line_height(&lv_font_montserrat_20) + 2);
    lv_obj_add_flag(s_slider_by, LV_OBJ_FLAG_HIDDEN);
    s_slider = lv_slider_create(card);
    lv_obj_set_width(s_slider, lv_pct(80));
    lv_obj_set_height(s_slider, 12);
    lv_obj_align(s_slider, LV_ALIGN_BOTTOM_MID, 0, -(int)lroundf(22 * k));
    lv_slider_set_range(s_slider, 0, 100);
    lv_obj_set_style_bg_color(s_slider, lv_color_hex(0x2a2345), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_slider, lv_color_hex(COLOR_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_slider, lv_color_hex(COLOR_TEXT), LV_PART_KNOB);
    lv_obj_set_ext_click_area(s_slider, 16);
    lv_obj_add_event_cb(s_slider, on_slider, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s_slider, on_slider, LV_EVENT_RELEASED, NULL);
}

/* by: the source line for a change made from outside, NULL for the
 * user's own tap on Speaker. */
static void slider_open_by(const char *by)
{
    if (!s_slider_box) {
        build_slider();
    }
    int v = muse_settings_speaker_on() ? muse_settings_volume() : 0;
    lv_slider_set_value(s_slider, v, LV_ANIM_OFF);
    char buf[16];
    snprintf(buf, sizeof(buf), "%d%%", v);
    lv_label_set_text(s_slider_val, buf);
    float k = s_g.short_side / 466.0f;
    int base_h = (int)lroundf(96 * k);
    if (by) {
        lv_label_set_text(s_slider_by, by);
        lv_obj_remove_flag(s_slider_by, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_height(s_slider_card, base_h + lv_font_get_line_height(&lv_font_montserrat_14) + 4);
    } else {
        lv_obj_add_flag(s_slider_by, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_height(s_slider_card, base_h);
    }
    s_slider_touched = lv_tick_get();
    lv_obj_remove_flag(s_slider_box, LV_OBJ_FLAG_HIDDEN);
}

static void slider_open(void)
{
    ESP_LOGI(TAG, "volume slider open (the Speaker button)");
    slider_open_by(NULL);
}

bool muse_home_volume_by(const char *by)
{
    if (!s_root || !s_spk || s_locked) {
        /* No Speaker here (a board without one), or the passcode's locked
         * face (no Speaker, and any touch opens the keypad): the
         * caller says it on the caption line instead. */
        return false;
    }
    slider_open_by(by);
    show_speaker();
    ESP_LOGI(TAG, "volume overlay: %d%% (%s)", muse_settings_speaker_on() ? muse_settings_volume() : 0, by);
    return true;
}

int muse_home_top_h(void)
{
    return s_root ? s_g.batt_h : 0;
}

static void on_speaker(lv_event_t *e)
{
    switch (lv_event_get_code(e)) {
    case LV_EVENT_SHORT_CLICKED:
        slider_open();
        break;
    case LV_EVENT_LONG_PRESSED: {
        bool on = !muse_settings_speaker_on();
        muse_settings_set_speaker_on(on);   /* reported to the app as speaker_volume */
        show_speaker();
        break;
    }
    default:
        break;
    }
}

/* ---- building ------------------------------------------------------------ */

static lv_obj_t *button_disc(lv_obj_t *p, int x, int y, int d, uint32_t color)
{
    lv_obj_t *b = lv_obj_create(p);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, d, d);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(color), 0);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(b, 8);
    lv_obj_align(b, LV_ALIGN_CENTER, x, y);
    return b;
}

static void bench_build(const muse_home_geom_t *g, int lock_top);

void muse_home_build(lv_obj_t *face, const muse_home_geom_t *g)
{
    s_g = *g;
    s_w = muse_board->width;
    s_h = muse_board->height;
    float k = g->short_side / 466.0f;

    s_root = lv_obj_create(face);
    lv_obj_remove_style_all(s_root);
    lv_obj_set_size(s_root, lv_pct(100), lv_pct(100));
    lv_obj_remove_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);
    /* A tap on the home outside Speaker and the hold region: the happy
     * bounce. Swipes still reach the pager (gesture bubble). */
    lv_obj_add_flag(s_root, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_EVENT_BUBBLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(s_root, on_home_tap, LV_EVENT_SHORT_CLICKED, NULL);

    build_top_row(s_root);

    /* The time and the date placed by their ink: the digits' top at time_top,
     * the capitals' at date_top, in the panel's type set. */
    const muse_type_t *type = muse_type(g->short_side);
    s_time = lv_label_create(s_root);
    lv_obj_set_style_text_font(s_time, type->time, 0);
    lv_obj_set_style_text_color(s_time, lv_color_hex(COLOR_TEXT), 0);
    lv_label_set_text(s_time, "");
    lv_obj_align(s_time, LV_ALIGN_TOP_MID, 0, g->time_top - muse_type_ink_top(type->time, '0'));

    int line_top = g->date_top - muse_type_ink_top(type->date, 'H');
    s_line = lv_label_create(s_root);
    s_line_color = UINT32_MAX;
    lv_obj_set_style_text_font(s_line, type->date, 0);
    lv_obj_set_style_text_color(s_line, lv_color_hex(COLOR_DIM), 0);
    lv_obj_set_style_text_align(s_line, LV_TEXT_ALIGN_CENTER, 0);
    /* Between the StopWatch's rim key icons (+-144 px at 466) where it has them. */
    lv_obj_set_width(s_line, (int)lroundf((muse_board->rim_controls ? 240 : 330) * k));
    lv_obj_set_height(s_line, lv_font_get_line_height(type->date));   /* one line, never wrapping into Muse */
    lv_label_set_long_mode(s_line, LV_LABEL_LONG_MODE_DOTS);
    s_line_caps_top = g->date_top;
    lv_label_set_text(s_line, "");
    lv_obj_align(s_line, LV_ALIGN_TOP_MID, 0, line_top);

    /* Not set up: "Open Muse app" in the
     * small role, dim, in the time's band; the device's name as the phone
     * lists it on the date line, in the info font, white. On the StopWatch
     * the date line runs into the rim key hints: the name sits just below
     * them. */
    s_state = lv_label_create(s_root);
    lv_obj_set_style_text_font(s_state, type->small, 0);
    lv_obj_set_style_text_color(s_state, lv_color_hex(COLOR_DIM), 0);
    lv_label_set_text(s_state, "");
    lv_obj_align(s_state, LV_ALIGN_TOP_MID, 0, g->state_top - muse_type_ink_top(type->small, 'H'));
    int name_top = g->name_top;
    if (muse_board->rim_controls) {
        name_top = s_h / 2 + muse_board->talk_hint.y + 18 + 4;   /* the 36 px hint box's bottom, 4 px clear */
    }
    s_name = lv_label_create(s_root);
    lv_obj_set_style_text_font(s_name, type->date, 0);
    lv_obj_set_style_text_color(s_name, lv_color_hex(COLOR_TEXT), 0);
    lv_obj_set_style_text_align(s_name, LV_TEXT_ALIGN_CENTER, 0);
    {
        double a = muse_home_safe_half(name_top, g->ring);
        double b = muse_home_safe_half(name_top + g->date_caps, g->ring);
        lv_obj_set_width(s_name, (int)(2 * (a < b ? a : b)));
    }
    lv_obj_set_height(s_name, lv_font_get_line_height(type->date));
    lv_label_set_long_mode(s_name, LV_LABEL_LONG_MODE_DOTS);
    lv_label_set_text(s_name, "");
    lv_obj_align(s_name, LV_ALIGN_TOP_MID, 0, name_top - muse_type_ink_top(type->date, 'H'));

    /* The hold region: a disc over Muse, clear of both buttons. */
    s_hold = lv_obj_create(face);
    lv_obj_remove_style_all(s_hold);
    lv_obj_set_size(s_hold, g->hold_d, g->hold_d);
    lv_obj_set_style_radius(s_hold, LV_RADIUS_CIRCLE, 0);
    lv_obj_remove_flag(s_hold, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_hold, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_align(s_hold, LV_ALIGN_CENTER, 0, g->muse_y);
    static const lv_event_code_t HOLD_EVENTS[] = { LV_EVENT_PRESSED, LV_EVENT_PRESSING, LV_EVENT_RELEASED,
                                                   LV_EVENT_PRESS_LOST, LV_EVENT_SHORT_CLICKED };
    for (size_t i = 0; i < sizeof(HOLD_EVENTS) / sizeof(HOLD_EVENTS[0]); i++) {
        lv_obj_add_event_cb(s_hold, on_hold, HOLD_EVENTS[i], NULL);
    }

    if (muse_board->audio_init) {
        s_spk = button_disc(s_root, -g->button_x, g->button_y, g->button_d, COLOR_SPEAKER);
        s_spk_icon = muse_icon_create(s_spk, MUSE_ICON_VOLUME_UP, muse_icon_button_glyph(g->button_d), 0xffffff);
        lv_obj_add_event_cb(s_spk, on_speaker, LV_EVENT_SHORT_CLICKED, NULL);
        lv_obj_add_event_cb(s_spk, on_speaker, LV_EVENT_LONG_PRESSED, NULL);
        show_speaker();
    }
    /* The passcode's locked face: a
     * lock glyph (Material lock, #8B84A8) 6.2 % of S where the page dots
     * were, its bottom on theirs. */
    int box = pr(0.062 * g->short_side);
    box = box < 14 ? 14 : box;
    s_lock = muse_icon_create(s_root, MUSE_ICON_LOCK, box, COLOR_DIM);
    lv_obj_align(s_lock, LV_ALIGN_CENTER, 0, (int)lround(g->dot_cy - box / 2.0 + g->dot_d / 2.0 - s_h / 2.0));
    lv_obj_add_flag(s_lock, LV_OBJ_FLAG_HIDDEN);
    bench_build(g, (int)lround(g->dot_cy + g->dot_d / 2.0 - box));
}

void muse_home_set_locked(bool locked)
{
    s_locked = locked;
    if (!s_root) {
        return;
    }
    if (locked) {
        hold_release();
        slider_close();
    }
    /* No Speaker button (nothing on it starts a turn), no hold region; the
     * lock glyph instead of the page dots (muse_ui.c). */
    lv_obj_t *const objs[] = {
        s_spk,
        s_hold,
    };
    for (size_t i = 0; i < sizeof(objs) / sizeof(objs[0]); i++) {
        if (objs[i]) lv_obj_set_flag(objs[i], LV_OBJ_FLAG_HIDDEN, locked);
    }
    lv_obj_set_flag(s_lock, LV_OBJ_FLAG_HIDDEN, !locked);
}

/* ---- the bench mark (bench.out=1) ----------------------------------------
 *
 * "Bench" in amber, the board's small type, in the page dots' place (the
 * dots hide while it shows), one line above the lock glyph on the locked
 * face: on LVGL's top layer, so it stands over every screen LVGL draws (the
 * home and its talk screen, Settings). Shown while the switch is on for the connected
 * central (mg_ble's bench_mark), so wherever the mic can send to an
 * unproven central it says so.
 */
#define COLOR_BENCH 0xffb43c
static lv_obj_t *s_bench;
static volatile bool s_bench_want;
static bool s_bench_shown, s_bench_raised;
static int s_bench_x, s_bench_y, s_bench_y_locked;   /* the label's top left on the screen */
static const lv_font_t *s_bench_font;

static void bench_build(const muse_home_geom_t *g, int lock_top)
{
    s_bench_font = muse_type(g->short_side)->small;
    lv_point_t sz;
    lv_text_get_size(&sz, MUSE_HOME_BENCH_TEXT, s_bench_font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    s_bench_x = (s_w - sz.x) / 2;
    s_bench_y = g->dot_cy - sz.y / 2;                         /* centered on the dots' row */
    s_bench_y_locked = lock_top - clampi(pr(0.01 * g->short_side), 2, 100) - sz.y;   /* above the lock glyph */
    if (!s_bench) {
        s_bench = lv_label_create(lv_layer_top());
        lv_obj_remove_flag(s_bench, LV_OBJ_FLAG_CLICKABLE);
    }
    lv_label_set_text(s_bench, MUSE_HOME_BENCH_TEXT);
    lv_obj_set_style_text_font(s_bench, s_bench_font, 0);
    lv_obj_set_style_text_color(s_bench, lv_color_hex(COLOR_BENCH), 0);
    lv_obj_set_size(s_bench, sz.x, sz.y);
    lv_obj_set_pos(s_bench, s_bench_x, s_bench_y);
    lv_obj_set_flag(s_bench, LV_OBJ_FLAG_HIDDEN, !s_bench_shown);
}

void muse_home_set_bench(bool on)
{
    s_bench_want = on;   /* muse_home_bench_tick shows it (LVGL task) */
}

void muse_home_bench_tick(void)
{
    if (!s_bench) {
        return;
    }
    bool on = s_bench_want, raised = s_lock && !lv_obj_has_flag(s_lock, LV_OBJ_FLAG_HIDDEN);
    if (on != s_bench_shown) {
        ESP_LOGI(TAG, "Bench mark %s (bench switch %s for the connected central)", on ? "on" : "off",
                 on ? "on" : "off");
        lv_obj_set_flag(s_bench, LV_OBJ_FLAG_HIDDEN, !on);
        s_bench_shown = on;
    }
    if (raised != s_bench_raised) {
        lv_obj_set_y(s_bench, raised ? s_bench_y_locked : s_bench_y);
        s_bench_raised = raised;
    }
}

bool muse_home_bench_shown(void)
{
    return s_bench_shown;
}

bool muse_home_bench_mark(int *x, int *y, const lv_font_t **font, uint32_t *rgb)
{
    if (!s_bench_font || !s_bench_want) {
        return false;
    }
    *x = s_bench_x;
    *y = s_bench_y;   /* over video (never the locked face) */
    *font = s_bench_font;
    *rgb = COLOR_BENCH;
    return true;
}

void muse_home_set_hidden(bool hidden)
{
    if (s_root) {
        lv_obj_set_flag(s_root, LV_OBJ_FLAG_HIDDEN, hidden);
    }
}

bool muse_home_clock_shown(void)
{
    return s_root && s_time && !lv_obj_has_flag(s_root, LV_OBJ_FLAG_HIDDEN) && lv_label_get_text(s_time)[0] != 0;
}

/* ---- every tick ------------------------------------------------------------ */

static void set_label(lv_obj_t *l, const char *text)
{
    if (strcmp(lv_label_get_text(l), text) != 0) {
        lv_label_set_text(l, text);
    }
}

/* The info font, or a step smaller where the text doesn't fit the line's
 * width (between the StopWatch's rim keys, or a round panel's chord: the
 * Balance rule's shrink order), keeping its capitals' top where they were. */
static void fit_label(lv_obj_t *l, const char *text, int caps_top)
{
    const muse_type_t *type = muse_type(s_g.short_side);
    const lv_font_t *fonts[] = { type->date, type->conn, type->row };
    int w = lv_obj_get_style_width(l, 0);
    const lv_font_t *f = fonts[2];
    for (size_t i = 0; i < 3; i++) {
        lv_point_t sz;
        lv_text_get_size(&sz, text, fonts[i], 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        if (sz.x <= w) {
            f = fonts[i];
            break;
        }
    }
    if (lv_obj_get_style_text_font(l, 0) != f) {
        lv_obj_set_style_text_font(l, f, 0);
        lv_obj_set_height(l, lv_font_get_line_height(f));
        lv_obj_align(l, LV_ALIGN_TOP_MID, 0, caps_top - muse_type_ink_top(f, 'H'));
    }
    set_label(l, text);
}


void muse_home_tick(const muse_home_state_t *st)
{
    if (!s_root) {
        return;
    }
    s_talks = st->talks;
    s_token = st->token;
    if (s_press.down && !s_talks) {
        hold_release();
    }
    tick_battery();
    top_tick();

    /* The time, once the phone has set the clock (hidden until then), and
     * never while the gadget isn't set up (the name stands in). */
    struct tm tm;
    char buf[48] = "";
    bool synced = muse_clock_local(&tm);
    if (synced && st->token) {
        if (muse_clock_24h()) {
            strftime(buf, sizeof(buf), "%H:%M", &tm);
        } else {
            int h = tm.tm_hour % 12;
            snprintf(buf, sizeof(buf), "%d:%02d", h ? h : 12, tm.tm_min);
        }
    }
    set_label(s_time, buf);

    /* The state line: a fresh caption, else the link's state, else the date. */
    uint32_t now = lv_tick_get();
    if (muse_state_caption(s_caption, sizeof(s_caption), &s_caption_version)) {
        s_caption_at = now;
        muse_mode_t m = muse_state_mode(NULL);
        s_caption_turn = m == MUSE_MODE_THINKING || m == MUSE_MODE_SPEAKING;
        muse_text_sentence_case(s_caption);   /* captions in sentence case */
    }
    const char *line;
    uint32_t color = COLOR_DIM;
    char date[32] = "";
    /* No SDK token keeps the Muse identity and
     * says so here until one is stored (the app can't finish setup without it). */
    bool no_sdk_token = !muse_link_sdk_token();
    /* Locked, no reply text on the screen; low battery and the
     * rest of the captions still show. */
    if (s_caption[0] && lv_tick_diff(now, s_caption_at) < CAPTION_SHOW_MS && !(s_locked && s_caption_turn)) {
        line = s_caption;
        color = COLOR_TEXT;
    } else if (!st->token) {
        line = "";   /* not set up: the name has the date line */
    } else if (st->link) {
        line = st->link;
    } else if (no_sdk_token) {
        line = "SDK token needed";
        color = COLOR_TEXT;
    } else {
        if (synced) {
            strftime(date, sizeof(date), "%a %b %e", &tm);
            /* "%e" pads single days with a space: "Oct  8" -> "Oct 8". */
            char *dbl = strstr(date, "  ");
            if (dbl) memmove(dbl, dbl + 1, strlen(dbl));
        }
        line = date;
    }
    fit_label(s_line, line, s_line_caps_top);
    /* A style set invalidates the label even with the same value: only on a
     * change, or the date line redraws on every tick (a release candidate's bug). */
    if (color != s_line_color) {
        s_line_color = color;
        lv_obj_set_style_text_color(s_line, lv_color_hex(color), 0);
    }
    /* The name without "MuseGadget-": M5SW-43A0E4, as Connect a device lists it. */
    const char *name = "";
    if (!st->token && st->name) {
        name = st->name;
        if (!strncmp(name, "MuseGadget-", 11)) name += 11;
    }
    set_label(s_name, name);
    set_label(s_state, st->token ? "" : no_sdk_token ? "SDK token needed" : "Open Muse app");   /* keyed on the stored token, not the clock */

    if (s_spk) {
        static int shown = -1;
        int on = muse_settings_speaker_on();
        if (on != shown) {
            shown = on;
            show_speaker();
        }
        /* Speaker stays where the home has it, set up or not:
         * it's the device's own volume. */
    }
    if (s_slider_box && !lv_obj_has_flag(s_slider_box, LV_OBJ_FLAG_HIDDEN) &&
        lv_tick_elaps(s_slider_touched) > SLIDER_SHOW_MS && !lv_obj_has_state(s_slider, LV_STATE_PRESSED)) {
        slider_close();
    }
}

static bool status_bench_point(muse_touch_gesture_t *g);

/* Bench (the console's tap=home.*): "status" (the Status page's connection
 * row), "speaker" (a tap opens the slider),
 * "speaker.hold" (the long press that mutes and unmutes), "slider<N>" (the
 * open slider dragged to N). LVGL task. */
bool muse_home_bench_point(const char *what, muse_touch_gesture_t *g)
{
    if (!strcmp(what, "status")) {
        return status_bench_point(g);   /* the Status page's connection row */
    }
    if (!strcmp(what, "speaker")) {
        return muse_touch_tap_obj(s_spk, MUSE_TOUCH_TAP_MS, g);
    }
    if (!strcmp(what, "speaker.hold")) {
        return muse_touch_tap_obj(s_spk, MUSE_TOUCH_HOLD_MS, g);
    }
    if (!strncmp(what, "slider", 6) && what[6] >= '0' && what[6] <= '9') {
        if (!s_slider_box || lv_obj_has_flag(s_slider_box, LV_OBJ_FLAG_HIDDEN)) {
            return false;
        }
        return muse_touch_slide_obj(s_slider, atoi(what + 6), g);
    }
    return false;
}

/* ---- Status --------------------------------------------------------- */

static lv_obj_t *s_st_conn, *s_st_batt, *s_st_id, *s_st_vol, *s_st_fw;
/* The connection row (muse_status_conn.h): its font for fitting, the
 * tap's toggle, whether a tap has anything to show, the last refresh. */
static const lv_font_t *s_st_conn_font;
/* Its layouts (muse_status_wrap): one line at its row (y1, w1) in the
 * connection font, or two, the first a line higher where the page has room
 * (y2; widths w2a, w2b: a round panel's chord at each line), in that font or,
 * when two lines of it still can't hold the whole text, the rows' smaller
 * one (the shrink order fit_label uses). The text shown and its layout. */
typedef struct {
    const lv_font_t *font;
    int y2, w2a, w2b;
} st_two_t;
static int s_st_y1, s_st_w1, s_st_layout;
static st_two_t s_st_two[2];
static char s_st_shown[MUSE_STATUS_LINE_MAX];
static muse_status_tap_t s_st_tap;
static bool s_st_has_detail, s_st_detail_was;
static int64_t s_st_conn_ms;
static bool s_st_conn_due = true;
static uint32_t s_st_conn_color = 0xFFFFFFFFu;

/* Half the panel's visible width at row y, inset px in from its edge
 * horizontally. */
double muse_home_safe_half(double y, double inset)
{
    int w = muse_board->width, h = muse_board->height;
    if (muse_board->round) {
        double r = (w < h ? w : h) / 2.0, dy = fabs(y - h / 2.0);
        double c = dy < r ? sqrt(r * r - dy * dy) : 0;
        return c > inset ? c - inset : 0;
    }
    double r = muse_board->corner_px;
    if (r > 0 && (y < r || y > h - r)) {
        double yc = y < r ? r : h - r, dy = fabs(y - yc);
        if (dy >= r - inset) return 0;
        return w / 2.0 - r + sqrt((r - inset) * (r - inset) - dy * dy);
    }
    return w / 2.0 - inset;
}

static int64_t st_now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

/* The row's width in the label's font (muse_status_wrap). */
static int st_measure(const char *text, void *ctx)
{
    lv_point_t sz;
    lv_text_get_size(&sz, text, (const lv_font_t *)ctx, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return sz.x;
}

/* The row's text on one line, or wrapped onto two (never
 * "...", the whole name readable): the label moves up a line and
 * widens to the wider of the two lines' chords; each line centered. Only on
 * a change (the wrap measures each character). LVGL task. */
static void status_conn_show(const char *text)
{
    if (s_st_layout && !strcmp(text, s_st_shown)) {
        return;
    }
    strlcpy(s_st_shown, text, sizeof(s_st_shown));
    int layout = 0;   /* 0: one line; 1 + i: two lines in s_st_two[i] */
    if (st_measure(text, (void *)s_st_conn_font) <= s_st_w1) {
        set_label(s_st_conn, text);
    } else {
        char l1[MUSE_STATUS_LINE_MAX], l2[MUSE_STATUS_LINE_MAX], both[2 * MUSE_STATUS_LINE_MAX];
        for (int i = 0; i < 2; i++) {
            const st_two_t *t = &s_st_two[i];
            int r = muse_status_wrap(text, t->w2a, t->w2b, st_measure, (void *)t->font, l1, l2, sizeof(l1));
            layout = 1 + i;
            if (r != MUSE_STATUS_WRAP_CUT) {
                break;   /* whole in this font */
            }
        }
        snprintf(both, sizeof(both), l2[0] ? "%s\n%s" : "%s", l1, l2);
        set_label(s_st_conn, both);
    }
    if (layout + 1 != s_st_layout) {
        s_st_layout = layout + 1;
        const lv_font_t *font = layout ? s_st_two[layout - 1].font : s_st_conn_font;
        const st_two_t *t = layout ? &s_st_two[layout - 1] : NULL;
        lv_obj_set_style_text_font(s_st_conn, font, 0);
        lv_obj_set_width(s_st_conn, !t ? s_st_w1 : t->w2a > t->w2b ? t->w2a : t->w2b);
        lv_obj_align(s_st_conn, LV_ALIGN_TOP_MID, 0, (t ? t->y2 : s_st_y1) - muse_type_ink_top(font, 'H'));
    }
}

/* The connection row's text and color: who is connected, or after a
 * tap the app and its version (Wi-Fi: the address and signal). Refreshed
 * every 250 ms while Status shows, and at once after a tap. LVGL task. */
static void status_conn(void)
{
    int64_t now = st_now_ms();
    bool detail = muse_status_detail_shown(&s_st_tap, now);
    if (!s_st_conn_due && detail == s_st_detail_was && now - s_st_conn_ms < 250) {
        return;
    }
    s_st_conn_due = false;
    s_st_conn_ms = now;
    muse_conn_info_t ci = { .kind = MUSE_CONN_NONE };
    uint32_t color = COLOR_TEXT;
#if CONFIG_MUSE_GADGET_BLE_AUDIO
    mg_link_t link = mg_ble_link();
    mg_ble_peer_t peer;
    if (link == MG_LINK_READY) {
        mg_ble_peer(&peer);
        ci.kind = MUSE_CONN_APP;
        ci.gap_name = peer.gap_name;
        ci.app_name = peer.app_name;
        ci.app_version = peer.app_version;
        color = COLOR_CONNECTED;   /* the reference's "Connected" green: the app is ready */
    }
#endif
    muse_wifi_status_t w;
    if (ci.kind == MUSE_CONN_NONE) {
        muse_wifi_status(&w);
        if (w.state == MUSE_WIFI_CONNECTED) {
            ci.kind = MUSE_CONN_WIFI;   /* a Wi-Fi gadget with no phone app: its network */
            ci.ssid = w.ssid;
            ci.ip = w.ip;
            ci.rssi = w.rssi;
        }
    }
    if (ci.kind == MUSE_CONN_NONE) {
#if CONFIG_MUSE_GADGET_BLE_AUDIO
        if (link == MG_LINK_SESSION || link == MG_LINK_CONNECTING) {
            ci.kind = MUSE_CONN_BT_ONLY;
        }
#else
        muse_ble_status_t b;
        muse_ble_status(&b);
        if (b.state == MUSE_BLE_CONNECTED) {
            ci.kind = MUSE_CONN_BT_ONLY;
        }
#endif
    }
    s_st_has_detail = ci.kind == MUSE_CONN_APP || ci.kind == MUSE_CONN_WIFI;
    if (!s_st_has_detail) {
        s_st_tap.detail = false;   /* nothing to show: the row as it is */
        detail = false;
    }
    s_st_detail_was = detail;
    char text[MUSE_STATUS_LINE_MAX];
    if (detail) {
        muse_status_detail_text(&ci, text, sizeof(text));
    } else {
        muse_status_conn_text(&ci, text, sizeof(text));
    }
    status_conn_show(text);
    /* A style set redraws the label even with the same value: only on a change. */
    if (color != s_st_conn_color) {
        s_st_conn_color = color;
        lv_obj_set_style_text_color(s_st_conn, lv_color_hex(color), 0);
    }
}

/* tap=home.status: the tap that shows the app's version. LVGL task. */
static bool status_bench_point(muse_touch_gesture_t *g)
{
    return s_st_conn && muse_touch_tap_obj(s_st_conn, MUSE_TOUCH_TAP_MS, g);
}

static void st_conn_clicked(lv_event_t *e)
{
    (void)e;
    muse_status_tap(&s_st_tap, st_now_ms(), s_st_has_detail);
    s_st_conn_due = true;
    status_conn();
}

/* Status: the title, the connection line, then
 * Battery / ID / Volume / FW in one column, the block centered between the
 * title and the page dots. */
static lv_obj_t *status_row(lv_obj_t *tile, const char *name, const lv_font_t *font, int x, int w, int y)
{
    lv_obj_t *n = lv_label_create(tile);
    lv_obj_set_style_text_font(n, font, 0);
    lv_obj_set_style_text_color(n, lv_color_hex(COLOR_DIM), 0);
    lv_label_set_text(n, name);
    lv_obj_set_pos(n, x, y);
    lv_obj_t *v = lv_label_create(tile);
    lv_obj_set_style_text_font(v, font, 0);
    lv_obj_set_style_text_color(v, lv_color_hex(COLOR_TEXT), 0);
    lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_width(v, w * 7 / 10);
    lv_label_set_long_mode(v, LV_LABEL_LONG_MODE_DOTS);
    lv_label_set_text(v, "");
    lv_obj_set_pos(v, x + w - w * 7 / 10, y);
    return v;
}

void muse_status_build(lv_obj_t *tile)
{
    int w = muse_board->width, h = muse_board->height;
    int S = w < h ? w : h;
    double s = S / 466.0;
    s = s < 0.5 ? 0.5 : s > 1.03 ? 1.03 : s;
    const muse_type_t *type = muse_type(S);
    muse_home_geom_t g;
    muse_home_geom(w, h, muse_board->round, muse_board->corner_px, &g);

    int title_top = clampi(pr(42 * s), 4, 1000), title_caps = clampi(pr(15 * s), 9, 1000);
    lv_obj_t *title = lv_label_create(tile);
    lv_obj_set_style_text_font(title, type->title, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(COLOR_ACCENT), 0);
    lv_label_set_text(title, "Status");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, title_top - muse_type_ink_top(type->title, 'H'));

    int conn_caps = clampi(pr(20 * s), 11, 1000), row_caps = clampi(pr(15 * s), 10, 1000);
    int pitch = clampi(pr(44 * s), row_caps * 2 + 6, 1000), gap_conn = clampi(pr(26 * s), row_caps, 1000);
    int title_bot = title_top + title_caps;
    int block = conn_caps + gap_conn + 3 * pitch + row_caps;
    int top = pr(title_bot + ((g.dot_cy - g.dot_d / 2.0) - title_bot - block) / 2.0);

    s_st_conn = lv_label_create(tile);
    lv_obj_set_style_text_font(s_st_conn, type->conn, 0);
    /* One line at its row; two (muse_status_wrap) start a line higher, into
     * the room above the block, as far as it goes keeping a gap under the
     * title (the second line takes the first's place, the rows stay). */
    int inset = pr(14 * s);
    s_st_y1 = top;
    s_st_w1 = (int)(2 * muse_home_safe_half(top + conn_caps / 2.0, inset));
    const lv_font_t *two_fonts[2] = { type->conn, type->row };
    for (int i = 0; i < 2; i++) {
        st_two_t *t = &s_st_two[i];
        int line_h = lv_font_get_line_height(two_fonts[i]);
        int caps = i ? row_caps : conn_caps;
        int room = top + conn_caps - caps - title_bot - clampi(pr(10 * s), 4, 1000);
        int up = room >= line_h ? line_h : room > 0 ? room : 0;
        t->font = two_fonts[i];
        t->y2 = top + conn_caps - caps - up;   /* line 2's capitals end where the one line's do */
        t->w2a = (int)(2 * muse_home_safe_half(t->y2 + caps / 2.0, inset));
        t->w2b = (int)(2 * muse_home_safe_half(t->y2 + line_h + caps / 2.0, inset));
    }
    s_st_layout = 0;
    s_st_shown[0] = 0;
    lv_obj_set_width(s_st_conn, s_st_w1);
    lv_obj_set_style_text_align(s_st_conn, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_st_conn, LV_LABEL_LONG_MODE_CLIP);
    lv_label_set_text(s_st_conn, "");
    lv_obj_align(s_st_conn, LV_ALIGN_TOP_MID, 0, top - muse_type_ink_top(type->conn, 'H'));
    /* A tap shows the app and its version (or Wi-Fi's address and signal)
     * for 5 s, or until the next tap. */
    s_st_conn_font = type->conn;
    lv_obj_add_flag(s_st_conn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(s_st_conn, clampi(pr(12 * s), 6, 1000));
    lv_obj_add_event_cb(s_st_conn, st_conn_clicked, LV_EVENT_CLICKED, NULL);
    s_st_conn_due = true;
    s_st_conn_color = 0xFFFFFFFFu;

    int y = top + conn_caps + gap_conn;
    int col = pr(280 * s), m = clampi(pr(14 * s), 8, 1000);
    double colw = col;
    for (int i = 0; i < 4; i++) {
        double yy = y + i * pitch;
        double a = muse_home_safe_half(yy, m), b = muse_home_safe_half(yy + row_caps, m);
        double half = a < b ? a : b;
        if (2 * half < colw) colw = 2 * half;
    }
    int cw = (int)colw, x = (int)lround(w / 2.0 - colw / 2.0);
    int ink = muse_type_ink_top(type->row, 'H');
    s_st_batt = status_row(tile, "Battery", type->row, x, cw, y - ink);
    s_st_id = status_row(tile, "ID", type->row, x, cw, y + pitch - ink);
    s_st_vol = status_row(tile, "Volume", type->row, x, cw, y + 2 * pitch - ink);
    s_st_fw = status_row(tile, "FW", type->row, x, cw, y + 3 * pitch - ink);
    muse_status_tick();
}

void muse_status_tick(void)
{
    if (!s_st_conn) {
        return;
    }
    status_conn();

    char buf[40];
    muse_power_t p = muse_state_power();
    if (p.battery_pct < 0) {
        strlcpy(buf, p.usb ? "USB power" : "None", sizeof(buf));
    } else {
        snprintf(buf, sizeof(buf), "%d%%%s", p.battery_pct, p.charging ? ", charging" : "");
    }
    set_label(s_st_batt, buf);
    muse_ble_status_t bs;
    muse_ble_status(&bs);
    const char *tail = strrchr(bs.name, '-');
    set_label(s_st_id, tail && tail[1] ? tail + 1 : bs.name);
    if (!muse_board->audio_init) {
        set_label(s_st_vol, "No speaker");
    } else if (muse_settings_speaker_on()) {
        snprintf(buf, sizeof(buf), "%d%%", muse_settings_volume());
        set_label(s_st_vol, buf);
    } else {
        set_label(s_st_vol, "Muted");
    }
    set_label(s_st_fw, esp_app_get_description()->version);
}
