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

#include "muse_theme_lvgl.h"
#include "muse_settings_ui.h"
#include "muse_settings_card.h"
#include "muse_restart.h"

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#if LV_USE_SNAPSHOT && CONFIG_MUSE_AVATAR_SYNC && CONFIG_MUSE_UI_THEME
#include <stdatomic.h>
#endif

#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "sdkconfig.h"
#if CONFIG_ESP_HOSTED_ENABLED
#include "esp_wifi.h"
#endif

#include "muse_audio.h"
#include "muse_battery.h"
#include "muse_ble.h"
#include "muse_board.h"
#include "muse_touch.h"
#include "muse_chat.h"
#include "muse_icons.h"
#include "muse_input.h"
#include "muse_lang.h"
#include "muse_keypad.h"
#include "muse_link.h"
#include "muse_passcode.h"
#include "muse_passcode_ui.h"
#include "muse_settings.h"
#if CONFIG_MUSE_AVATAR_SYNC
#include "muse_avatar.h"
#endif
#include "muse_state.h"
#include "muse_text.h"
#include "muse_type.h"
#include "muse_ui.h"
#include "muse_home.h"
#include "muse_vol_show.h"
#include "muse_voice.h"
#include "muse_wakeword_glue.h"
#include "muse_wifi.h"
#include "network_proxy.h"
#if CONFIG_MUSE_APP_VIEW
#include "muse_app_view.h"
#endif
#if CONFIG_MUSE_VIDEO
#include "muse_video.h"
#endif
/* Developer > Tests > Vibration: through the
 * production haptics engine, which runs with musegadgets BLE (mg_ble.c); the
 * simulator's stands in for it (sim_haptics.c, the same core). */
#if CONFIG_MUSE_GADGET_BLE_AUDIO || CONFIG_MUSE_BOARD_SIMULATOR
#define MUSE_VIB_TESTS 1
#include "mg_haptic_test.h"
#else
#define MUSE_VIB_TESTS 0
#endif

/*
 * The settings card list, scaled from its 466 px design
 * by s = short_side / 466 (clamped 0.5-1.03): column min(330 s, the chord at
 * the list's top less the margins), cards max(44, 58 s) high, radius 18 s,
 * gap 10 s, top 84 s; labels ~20 s, values ~16 s. A board that gives its
 * panel's size (panel_w_mm: the four ESP32 catalog boards) grows the list to
 * the millimeter minimums below (scale_geometry()). Compact panels (240 and
 * under, or narrower than 300) keep their tighter rows.
 */
#define LIST_W_REF 330
#define LIST_TOP_REF 84
#define ROW_H_REF 58
#define MAX_APS 12

static int s_list_w = LIST_W_REF, s_list_top = LIST_TOP_REF, s_row_h = ROW_H_REF, s_radius = 18, s_gap = 10;
static int s_pad = 16, s_icon_px = 24, s_icon_gap = 12, s_value_w = 130, s_title_top = 42;
static const lv_font_t *s_font_label = &lv_font_montserrat_20, *s_font_value = &lv_font_montserrat_16;
static const lv_font_t *s_font_title = &lv_font_montserrat_20;

/* Python's round(), half to even, as the reference's renderer rounds. */
static int pr(double v)
{
    return (int)nearbyint(v);
}

/* The menu's physical minimums: a
 * card at least 5.75 mm tall, label capitals 1.4 mm, value capitals 1.15 mm. */
#define MM_CARD 5.75
#define MM_LABEL 1.4
#define MM_VALUE 1.15

static double s_menu_factor = 1.0;   /* the millimeter growth; 1: the pixel layout */
static bool s_bt_onoff;              /* the Bluetooth row says On / Off (no room for "Not connected") */

/* A whole pixel at or above v (the reference's ceil(v - 1e-9)). */
static int up(double v)
{
    return (int)ceil(v - 1e-9);
}

/* The menu's layout, from the top of the
 * panel: title capitals 15 s at 42 s, cards from 84 s, the column the chord
 * at the first card less 14 s each side, labels 14 s and values 11 s tall in
 * the panel's type set (muse_type.h). A board that knows its panel's size
 * (panel_w_mm) and whose list falls below the millimeter minimums grows it by
 * the factor that reaches all three:
 * cards, gap, text, icons and radius x factor, rounded up to whole pixels
 * (then at least the minimum in pixels), padding and the label gap
 * x (1 + factor) / 2, the title at the label size, the list moved down by
 * what the title grew. The type set carries the grown capitals
 * (tools/muse/gen_ui_assets.py). */
static void scale_geometry(void)
{
    int w = muse_board->width, h = muse_board->height;
    int sh = w < h ? w : h;
    double k = sh / 466.0;
    k = k < 0.5 ? 0.5 : k > 1.03 ? 1.03 : k;
    const muse_type_t *type = muse_type(sh);
    s_row_h = pr(ROW_H_REF * k) > 44 ? pr(ROW_H_REF * k) : 44;
    s_radius = pr(18 * k);
    s_gap = pr(10 * k) > 4 ? pr(10 * k) : 4;
    s_list_top = pr(LIST_TOP_REF * k);
    s_pad = pr(16 * k) > 8 ? pr(16 * k) : 8;
    s_icon_px = pr(24 * k) > 14 ? pr(24 * k) : 14;
    s_icon_gap = pr(10 * k) + 2;
    s_value_w = pr(130 * k);
    s_title_top = pr(42 * k) > 4 ? pr(42 * k) : 4;
    s_menu_factor = 1.0;
    if (muse_board->panel_w_mm > 0) {
        double ppm = w / muse_board->panel_w_mm;
        int label = pr(14 * k) > 10 ? pr(14 * k) : 10, value = pr(11 * k) > 9 ? pr(11 * k) : 9;
        int title = pr(15 * k) > 9 ? pr(15 * k) : 9;
        int need_card = up(MM_CARD * ppm), need_label = up(MM_LABEL * ppm), need_value = up(MM_VALUE * ppm);
        double f = (double)need_card / s_row_h;
        f = (double)need_label / label > f ? (double)need_label / label : f;
        f = (double)need_value / value > f ? (double)need_value / value : f;
        if (f > 1.0) {
            int title_gap = s_list_top - (s_title_top + title);
            s_row_h = up(s_row_h * f) > need_card ? up(s_row_h * f) : need_card;
            s_gap = up(s_gap * f);
            s_icon_px = up(s_icon_px * f);
            s_radius = up(s_radius * f);
            s_pad = up(s_pad * (1 + f) / 2);
            s_icon_gap = up(s_icon_gap * (1 + f) / 2);
            s_value_w = pr(130 * k * f);
            label = up(label * f) > need_label ? up(label * f) : need_label;
            int title_caps = label > title ? label : title;
            int top = s_title_top + title_caps + title_gap;
            s_list_top = top > s_list_top ? top : s_list_top;
            s_menu_factor = f;
        }
    }
    int m = pr(14 * k) > 6 ? pr(14 * k) : 6;
    double a = muse_home_safe_half(s_list_top, m), b = muse_home_safe_half(s_list_top + s_row_h, m);
    int chord = (int)(2 * (a < b ? a : b));
    int list_w = pr(LIST_W_REF * k);
    s_list_w = chord < list_w ? chord : list_w;
    /* The generated fonts have text only: the symbols in titles, rows and
     * values (back arrow, check marks, Wi-Fi, refresh, charge) come from
     * LVGL's built-in Montserrat, which carries them. */
    static lv_font_t title_fb, label_fb, value_fb;
    const lv_font_t *sym = sh >= 400 ? &lv_font_montserrat_20 : &lv_font_montserrat_16;
    title_fb = *type->title;
    label_fb = *type->label;
    value_fb = *type->value;
    title_fb.fallback = label_fb.fallback = value_fb.fallback = sym;
    s_font_title = &title_fb;
    s_font_label = &label_fb;
    s_font_value = &value_fb;
    /* A grown list's Bluetooth row says On / Off
     * where its longest state, "Not connected", can't sit whole beside the
     * label (Status keeps the full words). */
    int vgap = pr(0.6 * s_icon_gap) > 6 ? pr(0.6 * s_icon_gap) : 6;
    lv_point_t bt, nc;
    lv_text_get_size(&bt, "Bluetooth", s_font_label, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    lv_text_get_size(&nc, "Not connected", s_font_value, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    int room = s_list_w - 2 * s_pad - s_icon_px - s_icon_gap - vgap - bt.x;
    s_bt_onoff = s_menu_factor > 1.0 && (nc.x > room || nc.x > s_value_w);
}

#define COLOR_TEXT 0xf2efff
#define COLOR_DIM 0x8b84a8
#define COLOR_CARD 0x1a1530
#define COLOR_CARD_PRESSED 0x2e2552
#define COLOR_ACCENT 0xa77dff
#define COLOR_OK 0x6ff0bf
#define COLOR_WARN 0xffb45c
#define COLOR_DANGER 0xff5c5c

typedef void (*text_done_cb_t)(const char *text);

/* The screen size the text page is scaled to (see text_px). */
static int s_text_scale = 466;
static lv_obj_t *s_tile;
static lv_obj_t *s_current;
static lv_obj_t *s_proxy;
static char s_proxy_ssid[MUSE_SSID_MAX + 1];
static network_proxy_config_t s_proxy_edit;
static lv_obj_t *s_proxy_mode, *s_proxy_host, *s_proxy_port, *s_proxy_user, *s_proxy_pass, *s_proxy_status;
static bool s_proxy_saving;
#if CONFIG_MUSE_UI_THEME
static lv_obj_t *s_appearance,*s_appearance_mode,*s_home_appearance,*s_appearance_status;
#endif
static lv_obj_t *s_home, *s_wifi, *s_hatch, *s_ble, *s_sound, *s_sleep, *s_battery, *s_power, *s_text;
#if CONFIG_MUSE_AVATAR_SYNC
static lv_obj_t *s_avatar, *s_avatar_source, *s_avatar_status, *s_home_avatar, *s_avatar_refresh;
#endif
static lv_obj_t *s_talk, *s_aboutp, *s_dev, *s_tests, *s_vib;
static lv_obj_t *s_passp, *s_pass_lockp;                   /* Passcode and its Lock */
static lv_obj_t *s_facep;                                   /* Watch face */
static lv_obj_t *s_about_reset;
/* Developer mode (muse_settings_developer()): five taps on About's FW
 * card within 1.5 s of each other turn it on. */
#define DEV_TAPS 5
static int s_fw_taps;
static int64_t s_fw_tap_us;

/*
 * Only home is kept. A sub-page is built when it opens and deleted on the way
 * back, so only the one on screen holds RAM: all of them at once cost ~50 KB,
 * which a board without PSRAM needs for Link's session.
 */
typedef struct page_s {
    lv_obj_t **obj;
    void (*build)(lv_obj_t *tile);
    const struct page_s *parent;   /* back goes there (Developer > Tests > Vibration); NULL: Settings */
} page_t;
static const page_t *s_page;       /* the sub-page on show (NULL: Settings) */
static bool s_going_back;

/* Home values. */
static lv_obj_t *s_home_wifi, *s_home_hatch, *s_home_ble, *s_home_sound, *s_home_sleep, *s_home_battery, *s_about;
static lv_obj_t *s_home_talk, *s_home_about, *s_home_power, *s_home_dev, *s_home_pass, *s_home_pass_val;
static lv_obj_t *s_home_restart;
static lv_obj_t *s_home_face;   /* Watch face */
static int64_t s_power_armed_us, s_restart_armed_us;
#if CONFIG_MUSE_VIDEO
static lv_obj_t *s_home_camera;
#endif

/* Wi-Fi page. */
static lv_obj_t *s_wifi_sw, *s_wifi_status, *s_wifi_saved, *s_wifi_scan_btn, *s_wifi_scan_lbl, *s_wifi_list;
static uint32_t s_shown_scan_gen = UINT32_MAX;
static muse_wifi_ap_t s_aps[MAX_APS];
static muse_wifi_saved_t s_saved[MUSE_WIFI_SAVED_MAX];
static lv_obj_t *s_saved_vals[MUSE_WIFI_SAVED_MAX];
static int s_saved_n = -1;   /* as listed; -1 before the first fill */
static int s_forget_armed = -1;
static int64_t s_forget_armed_us;
static char s_join_ssid[MUSE_SSID_MAX + 1];

/* Muse page. */
static lv_obj_t *s_hatch_status, *s_hatch_host, *s_hatch_vm, *s_hatch_token, *s_hatch_sdk_token;
static lv_obj_t *s_link_status, *s_link_reset_lbl;
static int64_t s_link_reset_armed_us;
static int64_t s_sdk_token_bad_us;   /* when an invalid SDK token was entered */
static int64_t s_host_bad_us;        /* when a server this build refuses was entered */

/* Bluetooth page. */
static lv_obj_t *s_ble_sw, *s_ble_status;

/* Sound page. */
#if CONFIG_MUSE_BOARD_M5STACK_STOPWATCH_FAMILY && CONFIG_MUSE_REALTIME_VOICE
static lv_obj_t *s_notes_sw;
#endif
static lv_obj_t *s_spk_mute;   /* the muted-speaker icon on the Speaker row while speaker_volume is 0 */
static lv_obj_t *s_spk_sw, *s_vol_val, *s_vol_sl, *s_gain_val, *s_gain_sl, *s_bright_val, *s_bright_sl, *s_mic_bar, *s_mic_val;
static lv_obj_t *s_text_val, *s_text_sl;
#if CONFIG_MUSE_WAKEWORD_ENABLE
static lv_obj_t *s_wake_sw;
#endif

/* Sleep page. */
/* The timeout choices; 2 min is the desk boards' default (muse_settings.c). */
static const int SLEEP_CHOICES[] = { 0, 10, 30, 60, 120, 300 };
static const char *const SLEEP_NAMES[] = { "Never", "10 seconds", "30 seconds", "1 minute", "2 minutes", "5 minutes" };
/* The Display row's value ("2 min"). */
static const char *const SLEEP_SHORT[] = { "Never", "10 s", "30 s", "1 min", "2 min", "5 min" };
#define SLEEP_COUNT (int)(sizeof(SLEEP_CHOICES) / sizeof(SLEEP_CHOICES[0]))
static lv_obj_t *s_sleep_checks[SLEEP_COUNT], *s_sleep_rows[SLEEP_COUNT];
static int s_sleep_shown = -1;
/* Sleep when charged: Always, Never; only where the board knows when
 * it's charged (muse_board_t.charged). */
static const char *const CHARGED_NAMES[] = { "Always", "Never" };
static lv_obj_t *s_chg_checks[2], *s_chg_rows[2];
static int s_chg_shown = -1;

/* Battery page. */
static lv_obj_t *s_batt_status, *s_batt_level, *s_batt_drain, *s_batt_full, *s_batt_off, *s_batt_slept, *s_batt_wakes,
    *s_batt_busy, *s_batt_awake;
static int64_t s_batt_shown_us;

/* Text entry page. */
static lv_obj_t *s_text_title, *s_text_ta;
static lv_obj_t *s_text_kp;                 /* a screen under 2" */
static lv_obj_t *s_text_kb, *s_text_show;   /* a bigger one */
static text_done_cb_t s_text_done;
static lv_obj_t *s_text_back;

/* ---------- building blocks ---------- */

/* Screen text in Brazilian Portuguese (muse_lang.c); the other languages
 * keep these pages in English. */
#if CONFIG_MUSE_UI_LANG_PT_BR
#define T(en) muse_lang_menu(en)
#else
#define T(en) (en)
#endif

/* Network and phone names can have characters the fonts lack (muse_text.h). */
#define SHOWN_MAX 96

/* Large panels (800x480): the text entry page's bigger keys and type. The
 * card list keeps the layout (scale_geometry()) on every panel. */
static bool wide_layout(void)
{
    return muse_board->width >= 640 && muse_board->height >= 400;
}

static const lv_font_t *settings_font(const lv_font_t *font)
{
#if CONFIG_MUSE_UI_LANG_PT_BR
    /* unscii has no accented letters: Montserrat 16 with the Portuguese ones. */
    if (font == &lv_font_unscii_16) {
        font = &lv_font_montserrat_16;
    }
#endif
    if (!wide_layout()) {
        return muse_lang_montserrat(font);
    }
    if (font == &lv_font_unscii_16 || font == &lv_font_montserrat_16) {
        return muse_lang_montserrat(&lv_font_montserrat_20);
    }
    if (font == &lv_font_montserrat_20) {
        return muse_lang_montserrat(&lv_font_montserrat_28);
    }
    return muse_lang_montserrat(font);
}

/* A label's font with the screen language's letters, at the size given. */
static const lv_font_t *lang_font(const lv_font_t *font)
{
#if CONFIG_MUSE_UI_LANG_PT_BR
    if (font == &lv_font_unscii_16) {
        font = &lv_font_montserrat_16;   /* unscii has no accented letters */
    }
#endif
    return muse_lang_montserrat(font);
}

/* A 172 px portrait panel: the text entry page's keypad and its header. */
static bool narrow_panel(void)
{
    return !muse_board->round && muse_board->width <= 200;
}

/* No speaker and no microphone (the LCD-1.28 and LCD-7): no Sound row (the
 * brightness is on Display) and no mic meter. */
static bool screen_only(void)
{
    return !muse_board->audio_init && !muse_board->mic_init && !muse_board->epaper;
}

static void single_line(lv_obj_t *l, int width)
{
    lv_obj_set_width(l, width);
    /* DOTS still wraps into an auto-height label unless both axes are bounded. */
    lv_obj_set_height(l, lv_font_get_line_height(lv_obj_get_style_text_font(l, 0)));
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
}

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, uint32_t color, const char *text)
{
    char shown[SHOWN_MAX];
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, lang_font(font), 0);
    muse_theme_set_style_text_color(l, lv_color_hex(color), 0);
    text = T(text);
    lv_label_set_text(l, muse_lang_showable(text) ? text : muse_text_showable(text, shown, sizeof(shown)));
    return l;
}

static void set_text(lv_obj_t *l, const char *text)
{
    char shown[SHOWN_MAX];
    text = T(text);
    if (!muse_lang_showable(text)) {
        text = muse_text_showable(text, shown, sizeof(shown));
    }
    if (strcmp(lv_label_get_text(l), text) != 0) {
        lv_label_set_text(l, text);
    }
}

static lv_obj_t *note(lv_obj_t *list, const char *text)
{
    lv_obj_t *l = label(list, &lv_font_montserrat_16, COLOR_DIM, text);
    lv_obj_set_width(l, lv_pct(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    return l;
}

static void go_back(void);

static void on_gesture(lv_event_t *e)
{
    (void)e;
    /* Dragging a slider right isn't a "back" swipe. */
    lv_obj_t *pressed = lv_indev_get_active_obj();
    if (pressed && lv_obj_check_type(pressed, &lv_slider_class)) {
        return;
    }
    if (lv_indev_get_gesture_dir(lv_indev_active()) == LV_DIR_RIGHT) {
        lv_indev_wait_release(lv_indev_active());
        go_back();
    }
}

/* Swipe right on a page to go back. By default a swipe bubbles up past the
 * page to the screen, so the page has to stop it. */
static void catch_swipes(lv_obj_t *p)
{
    lv_obj_remove_flag(p, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(p, on_gesture, LV_EVENT_GESTURE, NULL);
}

static void on_back(lv_event_t *e)
{
    (void)e;
    go_back();
}

/*
 * A page: its title at the top, and on a sub-page the back
 * chevron in the title ("< SOUND", the whole title a >= 44 px hit
 * area), then a vertically scrolling column of cards.
 */
static lv_obj_t *page(lv_obj_t *tile, const char *title, bool back, lv_obj_t **list_out)
{
    /* Flat short panels, and ones too narrow for the column, get tighter rows. */
    const bool compact = !muse_board->round && (muse_board->height <= 240 || muse_board->width < 300);
    const int list_top = compact ? (back ? 48 : 36) : s_list_top;
    lv_obj_t *p = lv_obj_create(tile);
    lv_obj_remove_style_all(p);
    lv_obj_set_size(p, lv_pct(100), lv_pct(100));
    lv_obj_remove_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(p, LV_OBJ_FLAG_HIDDEN);
    catch_swipes(p);

    char text[48];
    snprintf(text, sizeof(text), "%s%s", back ? LV_SYMBOL_LEFT " " : "", title);
    lv_obj_t *hit = lv_obj_create(p);
    lv_obj_remove_style_all(hit);
    lv_obj_set_size(hit, LV_SIZE_CONTENT, 44);
    lv_obj_set_style_pad_hor(hit, 12, 0);
    lv_obj_remove_flag(hit, LV_OBJ_FLAG_SCROLLABLE);
    const lv_font_t *tf = compact ? &lv_font_montserrat_16 : s_font_title;
    lv_obj_t *t = label(hit, tf, COLOR_ACCENT, text);
    lv_obj_center(t);
    /* The title's capitals start at title_top, the hit area
     * centered on them. */
    int title_y = compact ? (back ? 2 : 0)
                          : s_title_top - muse_type_ink_top(tf, 'H') + lv_font_get_line_height(tf) / 2 - 22;
    lv_obj_align(hit, LV_ALIGN_TOP_MID, 0, title_y > 0 ? title_y : 0);
    if (back) {
        lv_obj_add_flag(hit, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(hit, on_back, LV_EVENT_CLICKED, NULL);
        lv_obj_set_user_data(p, hit);   /* the bench's "back" (named()) */
    }

    lv_obj_t *list = lv_obj_create(p);
    lv_obj_remove_style_all(list);
    lv_obj_set_size(list, compact ? muse_board->width - 16 : s_list_w, muse_board->height - list_top);
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, list_top);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(list, compact ? 10 : s_gap, 0);
    /* Clear the page dots on flat panels, or the bottom curve on round ones. */
    lv_obj_set_style_pad_bottom(list, compact ? 32 : muse_board->round ? muse_board->height / 4 : 48, 0);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
    *list_out = list;
    return p;
}

static lv_obj_t *card(lv_obj_t *list, bool clickable)
{
    lv_obj_t *c = clickable ? lv_button_create(list) : lv_obj_create(list);
    lv_obj_remove_style_all(c);
    lv_obj_set_size(c, lv_pct(100), s_row_h);
    lv_obj_set_style_radius(c, s_radius, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    muse_theme_set_style_bg_color(c, lv_color_hex(COLOR_CARD), 0);
    muse_theme_set_style_bg_color(c, lv_color_hex(COLOR_CARD_PRESSED), LV_STATE_PRESSED);
    lv_obj_set_style_pad_hor(c, s_pad, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(c, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(c, s_icon_gap, 0);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    return c;
}

/* On a grown list (the millimeter minimums) a row's label and value sit
 * with their capitals, not their line boxes, centered on the card. The card's flex row puts the track (the tallest
 * item, `track` px) at (card - track) / 2 and each item centered in it,
 * rounding the track up to even (lv_flex.c); this moves the label from there
 * to the capitals' place. */
static void center_caps(lv_obj_t *l, int track)
{
    const lv_font_t *font = lv_obj_get_style_text_font(l, 0);
    lv_font_glyph_dsc_t g;
    if (s_menu_factor <= 1.0 || !lv_font_get_glyph_dsc(font, &g, 'H', 0)) {
        return;
    }
    int lh = lv_font_get_line_height(font);
    int top = (s_row_h - track) / 2 + (((track + 1) & ~1) - lh) / 2 + muse_type_ink_top(font, 'H');
    int want = (int)floor((s_row_h - g.box_h) / 2.0 + 0.5);
    lv_obj_set_style_translate_y(l, want - top, 0);
}

static void center_row_caps(lv_obj_t *t, lv_obj_t *v, int icon_h)
{
    int track = lv_font_get_line_height(s_font_label);
    if (v && lv_font_get_line_height(s_font_value) > track) {
        track = lv_font_get_line_height(s_font_value);
    }
    track = icon_h > track ? icon_h : track;
    center_caps(t, track);
    if (v) {
        center_caps(v, track);
    }
}

/* Tappable row: icon, text, right-aligned value. */
/* Without cb, the row only shows its value. `img` says an icon_row() will
 * put an icon image in front. */
static lv_obj_t *row_with(lv_obj_t *list, const char *icon, bool img, const char *text, lv_obj_t **value_out,
                          lv_event_cb_t cb, void *user)
{
    lv_obj_t *c = card(list, cb != NULL);
    lv_obj_t *sym = icon ? label(c, s_font_label, COLOR_ACCENT, icon) : NULL;
    lv_obj_t *t = label(c, s_font_label, COLOR_TEXT, text);
    lv_obj_set_flex_grow(t, 1);
    lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_DOTS);
    if (value_out) {
        lv_obj_t *v = label(c, s_font_value, COLOR_DIM, "");
        int max_w = s_value_w;
        if (s_menu_factor > 1.0) {
            /* A grown list keeps the label whole: the value gets what's left
             * of the card beside it, at most 130 s x factor, and ends in "…"
             * past that. */
            lv_point_t lw, sw = { 0, 0 };
            lv_text_get_size(&lw, text, s_font_label, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
            if (sym) {
                lv_text_get_size(&sw, icon, s_font_label, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
            }
            int lead = img ? s_icon_px + s_icon_gap : sym ? sw.x + s_icon_gap : 0;
            int vgap = pr(0.6 * s_icon_gap) > 6 ? pr(0.6 * s_icon_gap) : 6;   /* label to value */
            int room = s_list_w - 2 * s_pad - lead - lw.x - vgap;
            max_w = room < max_w ? (room > 0 ? room : 0) : max_w;
            lv_obj_set_style_margin_left(v, vgap - s_icon_gap, 0);
        }
        lv_obj_set_style_max_width(v, max_w, 0);
        lv_obj_set_height(v, lv_font_get_line_height(s_font_value));   /* one line, "…" past it */
        lv_label_set_long_mode(v, LV_LABEL_LONG_MODE_DOTS);
        *value_out = v;
    }
    center_row_caps(t, value_out ? *value_out : NULL, 0);
    if (cb) {
        lv_obj_add_event_cb(c, cb, LV_EVENT_CLICKED, user);
    }
    return c;
}

static lv_obj_t *row(lv_obj_t *list, const char *icon, const char *text, lv_obj_t **value_out,
                     lv_event_cb_t cb, void *user)
{
    return row_with(list, icon, false, text, value_out, cb, user);
}

/* A row led by one of the standard icons rather than a symbol. */
static lv_obj_t *icon_row(lv_obj_t *list, muse_icon_t icon, const char *text, lv_obj_t **value_out,
                          lv_event_cb_t cb, void *user)
{
    lv_obj_t *c = row_with(list, NULL, true, text, value_out, cb, user);
    lv_obj_t *img = muse_icon_create(c, icon, s_icon_px, COLOR_ACCENT);
    lv_obj_move_to_index(img, 0);
    center_row_caps(lv_obj_get_child(c, 1), value_out ? *value_out : NULL, s_icon_px);
    return c;
}

static lv_obj_t *switch_row(lv_obj_t *list, const char *text, bool on, lv_event_cb_t cb)
{
    lv_obj_t *c = card(list, false);
    lv_obj_t *t = label(c, s_font_label, COLOR_TEXT, text);
    lv_obj_set_flex_grow(t, 1);
    lv_obj_t *sw = lv_switch_create(c);
    lv_obj_set_size(sw, 60, 32);
    muse_theme_set_style_bg_color(sw, lv_color_hex(0x3a3358), LV_PART_MAIN);
    muse_theme_set_style_bg_color(sw, lv_color_hex(COLOR_ACCENT), LV_PART_INDICATOR | LV_STATE_CHECKED);
    if (on) {
        lv_obj_add_state(sw, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(sw, cb, LV_EVENT_VALUE_CHANGED, NULL);
    return sw;
}

static lv_obj_t *button(lv_obj_t *list, const char *text, uint32_t color, lv_event_cb_t cb, lv_obj_t **label_out)
{
    lv_obj_t *b = card(list, true);
    lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    /* A grown list (the millimeter minimums) sets its buttons at the label size. */
    lv_obj_t *l = label(b, s_menu_factor > 1.0 ? s_font_label : &lv_font_montserrat_20, color, text);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    if (label_out) {
        *label_out = l;
    }
    return b;
}

/* Label + value on one line, slider below. */
static lv_obj_t *slider(lv_obj_t *list, const char *text, int lo, int hi, int value, lv_obj_t **value_out,
                        lv_event_cb_t cb)
{
    lv_obj_t *c = lv_obj_create(list);
    lv_obj_remove_style_all(c);
    lv_obj_set_size(c, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(c, 8, 0);
    lv_obj_set_style_pad_ver(c, 6, 0);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);

    label(c, &lv_font_montserrat_20, COLOR_TEXT, text);
    lv_obj_t *v = label(c, &lv_font_montserrat_20, COLOR_ACCENT, "");
    lv_obj_align(v, LV_ALIGN_TOP_RIGHT, 0, 0);
    *value_out = v;

    lv_obj_t *s = lv_slider_create(c);
    lv_obj_set_width(s, lv_pct(94));
    lv_obj_set_height(s, 12);
    lv_obj_align(s, LV_ALIGN_TOP_MID, 0, 40);
    lv_slider_set_range(s, lo, hi);
    lv_slider_set_value(s, value, LV_ANIM_OFF);
    muse_theme_set_style_bg_color(s, lv_color_hex(0x2a2345), LV_PART_MAIN);
    muse_theme_set_style_bg_color(s, lv_color_hex(COLOR_ACCENT), LV_PART_INDICATOR);
    muse_theme_set_style_bg_color(s, lv_color_hex(COLOR_TEXT), LV_PART_KNOB);
    lv_obj_set_style_pad_all(s, 6, LV_PART_KNOB);
    lv_obj_set_ext_click_area(s, 16);
    lv_obj_add_event_cb(s, cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s, cb, LV_EVENT_RELEASED, NULL);

    lv_obj_t *spacer = lv_obj_create(c);   /* room below the slider */
    lv_obj_remove_style_all(spacer);
    lv_obj_set_size(spacer, 1, 1);
    lv_obj_align(spacer, LV_ALIGN_TOP_LEFT, 0, 64);
    return s;
}

/* A column of rows inside the page's list, filled in later. */
static lv_obj_t *column(lv_obj_t *list)
{
    lv_obj_t *l = lv_obj_create(list);
    lv_obj_remove_style_all(l);
    lv_obj_set_size(l, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(l, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(l, 10, 0);
    lv_obj_remove_flag(l, LV_OBJ_FLAG_SCROLLABLE);
    return l;
}

/* Label and value, not tappable. */
static lv_obj_t *info_row(lv_obj_t *list, const char *text)
{
    lv_obj_t *c = card(list, false);
    lv_obj_set_height(c, 44);
    label(c, &lv_font_montserrat_16, COLOR_TEXT, text);
    /* The value takes what's left, right-aligned, and ends in dots when long. */
    lv_obj_t *v = label(c, &lv_font_montserrat_16, COLOR_ACCENT, "");
    lv_obj_set_flex_grow(v, 1);
    lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(v, LV_LABEL_LONG_MODE_DOTS);
    return v;
}

/* The Developer page's label and value: on a grown list (the millimeter
 * minimums) a card of the list's size with the label and value at the menu's
 * sizes, the value wrapping onto its own line under the label when the two
 * don't fit side by side (diagnostics are never cut); else an
 * info_row. */
static lv_obj_t *dev_row(lv_obj_t *list, const char *text)
{
    if (s_menu_factor <= 1.0) {
        return info_row(list, text);
    }
    lv_obj_t *c = card(list, false);
    lv_obj_set_height(c, LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(c, s_row_h, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(c, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
    int pad_v = (s_row_h - lv_font_get_line_height(s_font_label)) / 2;
    lv_obj_set_style_pad_ver(c, pad_v > 4 ? pad_v : 4, 0);
    lv_obj_set_style_pad_row(c, 4, 0);
    label(c, s_font_label, COLOR_TEXT, text);
    return label(c, s_font_value, COLOR_ACCENT, "");
}

/* Why the chip last reset, for About. */
static const char *reset_name(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON: return "Power on";
    case ESP_RST_SW: return "Restart";
    case ESP_RST_PANIC: return "Crash";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT: return "Watchdog";
    case ESP_RST_DEEPSLEEP: return "Wake from sleep";
    case ESP_RST_BROWNOUT: return "Low voltage";
    case ESP_RST_USB: return "USB";
    default: return "Other";
    }
}

/* The firmware's own record (muse_restart) may replace this with more detail. */
__attribute__((weak)) const char *muse_settings_ui_last_reset(void)
{
    return reset_name(esp_reset_reason());
}

__attribute__((weak)) void muse_settings_ui_restart(void)
{
    muse_restart(MUSE_RESTART_CONSOLE);   /* a planned restart, marked as one */
}

/* ---------- navigation ---------- */

static void forget_controls(lv_obj_t *p);

static void drop(lv_obj_t *p)
{
    forget_controls(p);   /* the bench's named controls never point into a freed page */
    lv_obj_t **const pages[] = { &s_wifi, &s_hatch, &s_ble, &s_sound, &s_sleep, &s_battery, &s_power, &s_text,
                                 &s_talk, &s_aboutp, &s_dev, &s_tests, &s_vib, &s_passp, &s_pass_lockp,
                                 &s_facep, &s_proxy,
#if CONFIG_MUSE_UI_THEME
                                 &s_appearance,
#endif
#if CONFIG_MUSE_AVATAR_SYNC
                                 &s_avatar,
#endif
    };
    for (size_t i = 0; i < sizeof(pages) / sizeof(pages[0]); i++) {
        if (*pages[i] == p) {
            *pages[i] = NULL;
        }
    }
    lv_obj_delete_async(p);   /* we may be in one of its own events */
}

static void vib_leave(void);

/* Every sub-page built and kept under the one on show (Developer and Tests
 * under Vibration), freed on the way back to Settings. */
static void drop_all_but(lv_obj_t *keep)
{
    lv_obj_t **const pages[] = { &s_wifi, &s_hatch, &s_ble, &s_sound, &s_sleep, &s_battery, &s_power, &s_text,
                                 &s_talk, &s_aboutp, &s_dev, &s_tests, &s_vib, &s_passp, &s_pass_lockp,
                                 &s_facep, &s_proxy,
#if CONFIG_MUSE_UI_THEME
                                 &s_appearance,
#endif
#if CONFIG_MUSE_AVATAR_SYNC
                                 &s_avatar,
#endif
    };
    for (size_t i = 0; i < sizeof(pages) / sizeof(pages[0]); i++) {
        if (*pages[i] && *pages[i] != keep) {
            drop(*pages[i]);
        }
    }
}

static void show(lv_obj_t *p)
{
    lv_obj_t *prev = s_current;
    if (prev == p) {
        return;
    }
    if (prev) {
        lv_obj_add_flag(prev, LV_OBJ_FLAG_HIDDEN);
    }
    if (prev == s_sound) {
        muse_voice_set_monitor(false);
    }
    if (prev && prev == s_vib) {
        vib_leave();   /* built on open, freed on leave: a test playing stops with it */
    }
    lv_obj_remove_flag(p, LV_OBJ_FLAG_HIDDEN);
    s_current = p;
    if (p == s_sound && !muse_board->epaper && !screen_only()) {   /* as in the tick: no meter */
        muse_voice_set_monitor(true);
    }
    muse_ui_set_swipe_enabled(p == s_home);
    /* Home keeps nothing open; the text page returns to the page that opened
     * it; back from Vibration or Tests frees the page left. */
    if (p == s_home) {
        s_page = NULL;
        drop_all_but(s_home);
    } else if (prev && prev != s_home && (prev == s_text || s_going_back)) {
        drop(prev);
    }
}

static void open_page(const page_t *page)
{
    if (!*page->obj) {
        page->build(s_tile);
    }
    show(*page->obj);
    s_page = page;
    muse_settings_ui_tick(true);   /* fill it in now, not at the next tick */
}

static void close_text(void);

static void go_back(void)
{
    if (s_current == s_text) {
        close_text();
    } else if (s_current == s_proxy) {
        lv_obj_t *prev = s_proxy;
        network_proxy_wipe(&s_proxy_edit, sizeof(s_proxy_edit));
        show(s_wifi);
        drop(prev);
    } else if (s_page && s_page->parent && s_current == *s_page->obj) {
        s_going_back = true;
        open_page(s_page->parent);
        s_going_back = false;
    } else if (s_current != s_home) {
        show(s_home);
    }
}

static void on_nav(lv_event_t *e)
{
    open_page(lv_event_get_user_data(e));
}

/* ---------- text entry ---------- */

/* Laid out for a 466 px screen; a smaller round one scales it about its centre. */
static int text_px(int v)
{
    return v * s_text_scale / 466;
}

static int text_y(int y)
{
    return muse_board->height / 2 + text_px(y - 233);
}

/* Flat screens the scaled layout doesn't fit (320x240, 240x320): the back
 * arrow and title in one row, the field below, the keyboard under it. */
#define TEXT_COMPACT_FIELD_Y 44
#define TEXT_COMPACT_FIELD_H 40

static bool text_compact(void)
{
    /* Otherwise the full keyboard's field would run under the back arrow (it
     * ends at y 76) or past the screen's sides. Keypad screens (under 2")
     * place their own field. */
    return !muse_board->round && muse_board->diagonal_in >= 2.0f &&
           (text_y(76) < 80 || text_px(300) > muse_board->width - 16);
}

static int text_field_h(void)
{
    return text_compact() ? TEXT_COMPACT_FIELD_H : text_px(48);
}

static int text_field_y(void)
{
    return text_compact() ? TEXT_COMPACT_FIELD_Y : text_y(76);
}

static int text_field_w(void)
{
    return text_compact() ? muse_board->width - 16 : text_px(300);
}

static void close_text(void)
{
    lv_textarea_set_text(s_text_ta, "");   /* don't leave secrets in the widget */
    show(s_text_back);
}

static void on_text_ready(lv_event_t *e)
{
    (void)e;
    text_done_cb_t done = s_text_done;
    char *text = strdup(lv_textarea_get_text(s_text_ta));
    close_text();
    if (done && text) {
        done(text);
    }
    if (text) {
        memset(text, 0, strlen(text));
        free(text);
    }
}

static void on_text_cancel(lv_event_t *e)
{
    (void)e;
    close_text();
}

static void on_text_show(lv_event_t *e)
{
    (void)e;
    bool pw = !lv_textarea_get_password_mode(s_text_ta);
    lv_textarea_set_password_mode(s_text_ta, pw);
    lv_label_set_text(lv_obj_get_child(s_text_show, 0), T(pw ? "Show" : "Hide"));
}

/* Beside the keyboard's field, a button to show a password. */
static void fit_show_button(bool password)
{
    int w = text_field_w(), y = text_field_y(), show_w = 70, gap = 8;
    lv_obj_set_width(s_text_ta, password ? w - show_w - gap : w);
    lv_obj_align(s_text_ta, LV_ALIGN_TOP_MID, password ? -(show_w + gap) / 2 : 0, y);
    lv_obj_align(s_text_show, LV_ALIGN_TOP_MID, (w - show_w) / 2, y);
    lv_label_set_text(lv_obj_get_child(s_text_show, 0), T("Show"));
    if (password) {
        lv_obj_remove_flag(s_text_show, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_text_show, LV_OBJ_FLAG_HIDDEN);
    }
}

static void build_keyboard(void)
{
    s_text_show = lv_button_create(s_text);
    lv_obj_remove_style_all(s_text_show);
    lv_obj_set_size(s_text_show, 70, text_field_h());
    lv_obj_set_style_radius(s_text_show, 14, 0);
    lv_obj_set_style_bg_opa(s_text_show, LV_OPA_COVER, 0);
    muse_theme_set_style_bg_color(s_text_show, lv_color_hex(COLOR_CARD), 0);
    lv_obj_add_event_cb(s_text_show, on_text_show, LV_EVENT_CLICKED, NULL);
    lv_obj_center(label(s_text_show, settings_font(&lv_font_montserrat_16), COLOR_TEXT, "Show"));

    /* Inside the circle, or across the rest of the screen. */
    s_text_kb = lv_keyboard_create(s_text);
    int y = text_compact() ? TEXT_COMPACT_FIELD_Y + TEXT_COMPACT_FIELD_H + 6 : text_y(136);
    if (muse_board->round) {
        lv_obj_set_size(s_text_kb, text_px(384), text_px(206));
    } else {
        int w = muse_board->width - 16;
        int h = muse_board->height - y - 8;
        lv_obj_set_size(s_text_kb, w, LV_MIN(h, w * 206 / 384));
    }
    lv_obj_align(s_text_kb, LV_ALIGN_TOP_MID, 0, y);
    lv_obj_set_style_bg_opa(s_text_kb, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(s_text_kb, 2, 0);
    lv_obj_set_style_pad_gap(s_text_kb, 4, 0);
    lv_obj_set_style_text_font(s_text_kb, settings_font(&lv_font_montserrat_20), LV_PART_ITEMS);
    muse_theme_set_style_bg_color(s_text_kb, lv_color_hex(COLOR_CARD), LV_PART_ITEMS);
    muse_theme_set_style_text_color(s_text_kb, lv_color_hex(COLOR_TEXT), LV_PART_ITEMS);
    lv_obj_set_style_radius(s_text_kb, 8, LV_PART_ITEMS);
    lv_obj_set_style_border_width(s_text_kb, 0, LV_PART_ITEMS);
    lv_obj_remove_flag(s_text_kb, LV_OBJ_FLAG_GESTURE_BUBBLE);   /* a sloppy swipe mustn't lose the text */
    lv_keyboard_set_textarea(s_text_kb, s_text_ta);
    lv_obj_add_event_cb(s_text_kb, on_text_ready, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(s_text_kb, on_text_cancel, LV_EVENT_CANCEL, NULL);
}

/* The text page's back arrow (it has a field, not a title, at the top). */
static lv_obj_t *back_button(lv_obj_t *p)
{
    lv_obj_t *b = lv_button_create(p);
    lv_obj_remove_style_all(b);
    if (wide_layout()) {
        lv_obj_set_size(b, 72, 64);
        lv_obj_align(b, LV_ALIGN_TOP_LEFT, 24, 12);
    } else {
        lv_obj_set_size(b, 56, 48);
        lv_obj_align(b, LV_ALIGN_TOP_MID, -112, 28);
    }
    lv_obj_add_event_cb(b, on_back, LV_EVENT_CLICKED, NULL);
    lv_obj_t *arrow = label(b, wide_layout() ? &lv_font_montserrat_28 : &lv_font_montserrat_20,
                            COLOR_ACCENT, LV_SYMBOL_LEFT);
    lv_obj_center(arrow);
    return b;
}

static void build_text_page(lv_obj_t *tile)
{
    s_text = lv_obj_create(tile);
    lv_obj_remove_style_all(s_text);
    lv_obj_set_size(s_text, lv_pct(100), lv_pct(100));
    lv_obj_remove_flag(s_text, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_text, LV_OBJ_FLAG_HIDDEN);
    catch_swipes(s_text);
    lv_obj_t *back = back_button(s_text);

    /* Between the back arrow and its mirror image. */
    s_text_title = label(s_text, settings_font(&lv_font_montserrat_20), COLOR_ACCENT, "");
    lv_obj_set_width(s_text_title, 150);
    lv_obj_set_style_text_align(s_text_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_text_title, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_align(s_text_title, LV_ALIGN_TOP_MID, 0, 40);

    const lv_font_t *font = &lv_font_montserrat_20;
    const lv_font_t *display_font = settings_font(font);
    int h = text_field_h(), border = 2;
    int pad = (h - 2 * border - lv_font_get_line_height(display_font)) / 2;
    s_text_ta = lv_textarea_create(s_text);
    lv_textarea_set_one_line(s_text_ta, true);
    lv_obj_set_size(s_text_ta, text_field_w(), h);
    lv_obj_set_style_text_font(s_text_ta, display_font, 0);
    lv_obj_set_style_pad_ver(s_text_ta, pad > 0 ? pad : 0, 0);
    lv_obj_set_style_pad_hor(s_text_ta, 14, 0);
    muse_theme_set_style_bg_color(s_text_ta, lv_color_hex(COLOR_CARD), 0);
    muse_theme_set_style_text_color(s_text_ta, lv_color_hex(COLOR_TEXT), 0);
    muse_theme_set_style_border_color(s_text_ta, lv_color_hex(COLOR_ACCENT), 0);
    lv_obj_set_style_border_width(s_text_ta, border, 0);
    lv_obj_set_style_radius(s_text_ta, 14, 0);
    if (wide_layout()) {
        /* Large panels keep a steady cursor. */
        lv_obj_set_style_anim_duration(s_text_ta, 0, LV_PART_CURSOR | LV_STATE_FOCUSED);
    }
    lv_obj_set_style_text_font(s_text_ta, settings_font(&lv_font_montserrat_16), LV_PART_TEXTAREA_PLACEHOLDER);
    muse_theme_set_style_text_color(s_text_ta, lv_color_hex(COLOR_DIM), LV_PART_TEXTAREA_PLACEHOLDER);
    lv_obj_add_state(s_text_ta, LV_STATE_FOCUSED);
    if (muse_board->epaper) {
        /* The theme blinks the cursor every 400 ms, and an e-paper panel
         * refreshes only once the screen has stopped changing. */
        lv_obj_set_style_anim_duration(s_text_ta, 0, LV_PART_CURSOR);
    }
    lv_obj_align(s_text_ta, LV_ALIGN_TOP_MID, 0, text_field_y());
    if (text_compact()) {
        lv_obj_set_size(back, 48, TEXT_COMPACT_FIELD_H);
        lv_obj_align(back, LV_ALIGN_TOP_LEFT, 4, 0);
        single_line(s_text_title, muse_board->width - 120);   /* a long SSID mustn't wrap onto the field */
        lv_obj_align(s_text_title, LV_ALIGN_TOP_MID, 0, 10);
    }

    /* A full keyboard's keys are too small to hit on a screen under 2", or a narrow one. */
    if (muse_board->diagonal_in >= 2.0f && !narrow_panel()) {
        build_keyboard();
        return;
    }

    /* The keys out to the screen's edges, and the rest squeezed in above them:
     * the back arrow beside the field, the title over both. */
    int top = text_y(98);
    if (narrow_panel()) {
        /* Portrait panels need a header inside the screen, not the round layout. */
        lv_obj_set_style_text_font(s_text_title, settings_font(&lv_font_montserrat_16), 0);
        single_line(s_text_title, muse_board->width - 16);
        lv_obj_align(s_text_title, LV_ALIGN_TOP_MID, 0, 8);
        lv_obj_set_width(s_text_ta, muse_board->width - 60);
        lv_obj_align(s_text_ta, LV_ALIGN_TOP_RIGHT, -4, 36);
        lv_obj_set_size(back, 48, h);
        lv_obj_align(back, LV_ALIGN_TOP_LEFT, 4, 36);
        top = 92;
    } else {
        lv_obj_align(s_text_title, LV_ALIGN_TOP_MID, 0, text_y(16));
        lv_obj_set_width(s_text_title, text_px(180));
        lv_obj_set_width(s_text_ta, text_px(224));
        lv_obj_align(s_text_ta, LV_ALIGN_TOP_MID, 0, text_y(44));
        lv_obj_set_size(back, text_px(48), h);
        lv_obj_align(back, LV_ALIGN_TOP_MID, -text_px(140), text_y(44));
    }
    s_text_kp = muse_keypad_create(s_text, s_text_ta, muse_board->round);
    lv_obj_set_size(s_text_kp, muse_board->width, muse_board->height - top);
    lv_obj_align(s_text_kp, LV_ALIGN_TOP_MID, 0, top);
    if (muse_board->round) {
        /* Lifts the bottom row's labels inside the circle. The button matrix
         * stretches edge keys' taps over padding up to LV_DPI_DEF / 10, and not
         * at all over more, so this much keeps them reaching the edge. */
        lv_obj_set_style_pad_bottom(s_text_kp, LV_DPI_DEF / 10, 0);
    }
    lv_obj_add_event_cb(s_text_kp, on_text_ready, LV_EVENT_READY, NULL);
}

/* The hint shows in the empty field, so keep it short. */
static void open_text(const char *title, const char *initial, bool password, int max_len, const char *hint,
                      text_done_cb_t done, lv_obj_t *back)
{
    if (!s_text) {
        build_text_page(s_tile);
    }
    set_text(s_text_title, title);
    lv_textarea_set_max_length(s_text_ta, max_len);
    lv_textarea_set_password_mode(s_text_ta, password);
    lv_textarea_set_text(s_text_ta, initial ? initial : "");
    lv_textarea_set_placeholder_text(s_text_ta, hint ? T(hint) : "");
    if (s_text_kp) {
        muse_keypad_reset(s_text_kp, password);
    } else {
        lv_keyboard_set_mode(s_text_kb, LV_KEYBOARD_MODE_TEXT_LOWER);
        fit_show_button(password);
    }
    s_text_done = done;
    s_text_back = back;
    show(s_text);
}

/* ---------- Wi-Fi ---------- */

static void on_wifi_sw(lv_event_t *e)
{
    muse_settings_set_wifi_on(lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED));
}

static void on_wifi_scan(lv_event_t *e)
{
    (void)e;
    muse_wifi_scan();
}

static void on_wifi_pass(const char *pass)
{
    muse_settings_set_wifi(s_join_ssid, pass);
}

static bool is_saved(const char *ssid)
{
    for (int i = 0; i < s_saved_n; i++) {
        if (!strcmp(s_saved[i].ssid, ssid)) {
            return true;
        }
    }
    return false;
}

/* A saved network asks again too, in case its password changed. */
static void on_wifi_ap(lv_event_t *e)
{
    const muse_wifi_ap_t *ap = &s_aps[(int)(intptr_t)lv_event_get_user_data(e)];
    strlcpy(s_join_ssid, ap->ssid, sizeof(s_join_ssid));
    if (ap->secure) {
        open_text(ap->ssid, "", true, MUSE_PASS_MAX, "Password", on_wifi_pass, s_wifi);
    } else {
        muse_settings_set_wifi(s_join_ssid, "");
    }
}

static void on_other_ssid(const char *ssid)
{
    if (!ssid[0]) {
        return;
    }
    strlcpy(s_join_ssid, ssid, sizeof(s_join_ssid));
    open_text(ssid, "", true, MUSE_PASS_MAX, "Empty if it's open", on_wifi_pass, s_wifi);
}

static void on_wifi_other(lv_event_t *e)
{
    (void)e;
    open_text("Other network", "", false, MUSE_SSID_MAX, "Network name", on_other_ssid, s_wifi);
}

/* Two taps within a few seconds forget a saved network. */
static void on_wifi_saved(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    int64_t now = esp_timer_get_time();
    if (s_forget_armed == i && now - s_forget_armed_us < 4000000) {
        muse_wifi_forget(s_saved[i].ssid);
        s_forget_armed = -1;
        return;
    }
    s_forget_armed = i;
    s_forget_armed_us = now;
}

static bool same_saved(const muse_wifi_saved_t *a, int n)
{
    if (n != s_saved_n) {
        return false;
    }
    for (int i = 0; i < n; i++) {
        if (strcmp(a[i].ssid, s_saved[i].ssid) || a[i].hidden != s_saved[i].hidden) {
            return false;
        }
    }
    return true;
}

/* A proxy belongs to the selected remembered network. Saving uses Link's
 * internal-stack keeper, just as the Wi-Fi credential editor does. */
static void proxy_host_done(const char *text) { strlcpy(s_proxy_edit.host, text, sizeof(s_proxy_edit.host)); }
static void proxy_user_done(const char *text) { strlcpy(s_proxy_edit.username, text, sizeof(s_proxy_edit.username)); }
static void proxy_pass_done(const char *text) { strlcpy(s_proxy_edit.password, text, sizeof(s_proxy_edit.password)); }
static void proxy_port_done(const char *text) {
    char *end;
    long port = strtol(text, &end, 10);
    s_proxy_edit.port = *text && !*end && port > 0 && port <= 65535 ? port : 0;
}

static void on_proxy_mode(lv_event_t *e) {
    (void)e;
    s_proxy_edit.type = s_proxy_edit.type == NETWORK_PROXY_OFF ? NETWORK_PROXY_HTTP
                       : s_proxy_edit.type == NETWORK_PROXY_HTTP ? NETWORK_PROXY_SOCKS5 : NETWORK_PROXY_OFF;
    if (s_proxy_edit.type == NETWORK_PROXY_OFF) network_proxy_wipe(&s_proxy_edit, sizeof(s_proxy_edit));
}

static void on_proxy_field(lv_event_t *e) {
    int field = (int)(intptr_t)lv_event_get_user_data(e);
    char port[6];
    snprintf(port, sizeof(port), "%u", s_proxy_edit.port);
    if (field == 0) open_text("Proxy server", s_proxy_edit.host, false, NETWORK_PROXY_HOST_MAX, "Host name or IP", proxy_host_done, s_proxy);
    if (field == 1) open_text("Proxy port", port, false, 5, "1-65535", proxy_port_done, s_proxy);
    if (field == 2) open_text("Proxy username", s_proxy_edit.username, false, NETWORK_PROXY_AUTH_MAX, "Optional", proxy_user_done, s_proxy);
    if (field == 3) open_text("Proxy password", s_proxy_edit.password, true, NETWORK_PROXY_AUTH_MAX, "Optional", proxy_pass_done, s_proxy);
}

static void on_proxy_save(lv_event_t *e) {
    (void)e;
    if (!network_proxy_valid(&s_proxy_edit)) {
        set_text(s_proxy_status, "Check server, port and authentication");
        return;
    }
    s_proxy_saving = muse_link_wifi_proxy_set(s_proxy_ssid, &s_proxy_edit);
    set_text(s_proxy_status, s_proxy_saving ? "Saving..." : "Busy or network forgotten; try again");
}

static void on_wifi_proxy(lv_event_t *e) {
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    strlcpy(s_proxy_ssid, s_saved[i].ssid, sizeof(s_proxy_ssid));
    muse_link_wifi_proxy_get(s_proxy_ssid, &s_proxy_edit);
    s_proxy_saving = false;
    lv_obj_t *list;
    s_proxy = page(s_tile, "PROXY", true, &list);
    note(list, s_proxy_ssid);
    row(list, NULL, "Configure Proxy", &s_proxy_mode, on_proxy_mode, NULL);
    row(list, NULL, "Server", &s_proxy_host, on_proxy_field, (void *)0);
    row(list, NULL, "Port", &s_proxy_port, on_proxy_field, (void *)1);
    row(list, NULL, "Username", &s_proxy_user, on_proxy_field, (void *)2);
    row(list, NULL, "Password", &s_proxy_pass, on_proxy_field, (void *)3);
    button(list, "Save", COLOR_ACCENT, on_proxy_save, NULL);
    s_proxy_status = note(list, "Cloud connections use this network's proxy. Local traffic stays direct.");
    show(s_proxy);
}

static void tick_proxy(void) {
    const char *mode = s_proxy_edit.type == NETWORK_PROXY_OFF ? "Off"
        : s_proxy_edit.type == NETWORK_PROXY_HTTP ? "Manual: HTTP CONNECT"
        : s_proxy_edit.type == NETWORK_PROXY_SOCKS5 ? "Manual: SOCKS5" : "Invalid; select mode";
    char port[6];
    snprintf(port, sizeof(port), "%u", s_proxy_edit.port);
    set_text(s_proxy_mode, mode);
    set_text(s_proxy_host, s_proxy_edit.host);
    set_text(s_proxy_port, s_proxy_edit.port ? port : "");
    set_text(s_proxy_user, s_proxy_edit.username);
    set_text(s_proxy_pass, s_proxy_edit.password[0] ? "********" : "");
    if (s_proxy_saving) {
        network_proxy_config_t check;
        int state = muse_link_wifi_proxy_get(s_proxy_ssid, &check);
        network_proxy_wipe(&check, sizeof(check));
        if (state != 1) {
            s_proxy_saving = false;
            set_text(s_proxy_status, state == 0 ? "Saved" : "Save failed; try again");
        }
    }
}

static void rebuild_saved_list(void)
{
    muse_wifi_saved_t saved[MUSE_WIFI_SAVED_MAX];
    int n = muse_wifi_saved(saved, MUSE_WIFI_SAVED_MAX);
    if (same_saved(saved, n)) {
        return;
    }
    memcpy(s_saved, saved, n * sizeof(saved[0]));
    s_saved_n = n;
    s_forget_armed = -1;
    s_shown_scan_gen = UINT32_MAX;   /* re-mark the saved ones in the scan list */
    lv_obj_clean(s_wifi_saved);
    if (n) {
        note(s_wifi_saved, "Saved networks");
    }
    for (int i = 0; i < n; i++) {
        row(s_wifi_saved, LV_SYMBOL_WIFI, saved[i].ssid, &s_saved_vals[i], on_wifi_saved, (void *)(intptr_t)i);
        row(s_wifi_saved, NULL, "Configure Proxy", NULL, on_wifi_proxy, (void *)(intptr_t)i);
    }
}

static void tick_saved_list(const muse_wifi_status_t *w)
{
    if (s_forget_armed >= 0 && esp_timer_get_time() - s_forget_armed_us >= 4000000) {
        s_forget_armed = -1;
    }
    for (int i = 0; i < s_saved_n; i++) {
        const char *text = "";
        uint32_t color = COLOR_DIM;
        if (i == s_forget_armed) {
            text = "Tap to forget";
            color = COLOR_DANGER;
        } else if (w->state == MUSE_WIFI_CONNECTED && !strcmp(w->ssid, s_saved[i].ssid)) {
            text = "Connected";
            color = COLOR_OK;
        } else if (s_saved[i].hidden) {
            text = "Hidden";
        }
        set_text(s_saved_vals[i], text);
        muse_theme_set_style_text_color(s_saved_vals[i], lv_color_hex(color), 0);
    }
}

/* Returns true when fresh results include a saved network. */
static bool rebuild_scan_list(void)
{
    uint32_t gen;
    int n = muse_wifi_scan_results(s_aps, MAX_APS, &gen);
    if (gen == s_shown_scan_gen) {
        return false;
    }
    bool fresh = s_shown_scan_gen != UINT32_MAX, saved_seen = false;
    s_shown_scan_gen = gen;
    lv_obj_clean(s_wifi_list);
    for (int i = 0; i < n; i++) {
        lv_obj_t *v;
        row(s_wifi_list, NULL, s_aps[i].ssid, &v, on_wifi_ap, (void *)(intptr_t)i);
        bool saved = is_saved(s_aps[i].ssid);
        saved_seen |= saved;
        char buf[24];
        if (muse_board->epaper) {
            snprintf(buf, sizeof(buf), "%s", T(saved ? "saved" : s_aps[i].secure ? "secured" : "open"));
        } else {
            snprintf(buf, sizeof(buf), "%s%d dBm", T(saved ? "saved  " : (s_aps[i].secure ? "" : "open  ")), s_aps[i].rssi);
        }
        lv_label_set_text(v, buf);
    }
    if (gen && !n) {
        note(s_wifi_list, "No networks found");
    }
    return fresh && saved_seen;
}

static bool sta_mac(uint8_t m[6])
{
#if CONFIG_ESP_HOSTED_ENABLED
    /* The Wi-Fi coprocessor's, asked once: the call queues behind its other RPCs. */
    static uint8_t s_mac[6];
    static bool s_have;
    if (!s_have) {
        s_have = esp_wifi_get_mac(WIFI_IF_STA, s_mac) == ESP_OK;
    }
    memcpy(m, s_mac, sizeof(s_mac));
    return s_have;
#else
    return esp_read_mac(m, ESP_MAC_WIFI_STA) == ESP_OK;
#endif
}

static void build_wifi_page(lv_obj_t *tile)
{
    lv_obj_t *list;
    s_wifi = page(tile, "Wi-Fi", true, &list);
    s_shown_scan_gen = UINT32_MAX;   /* the lists start empty */
    s_saved_n = -1;
    s_forget_armed = -1;
    s_wifi_sw = switch_row(list, "Wi-Fi", muse_settings_wifi_on(), on_wifi_sw);
    s_wifi_status = note(list, "");

    s_wifi_saved = column(list);
    s_wifi_scan_btn = button(list, LV_SYMBOL_REFRESH "  Scan for networks", COLOR_ACCENT, on_wifi_scan, &s_wifi_scan_lbl);
    s_wifi_list = column(list);

    row(list, LV_SYMBOL_EDIT, "Other network...", NULL, on_wifi_other, NULL);
    lv_obj_t *mac = info_row(list, "MAC address");
    uint8_t m[6];
    if (sta_mac(m)) {
        char buf[18];
        snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
        lv_label_set_text(mac, buf);
    }
    note(list, "Muse remembers up to 8 networks and joins the strongest one in range. Tap a saved one twice to forget it.");
}

static void tick_wifi(void)
{
    muse_wifi_status_t w;
    muse_wifi_status(&w);
    char buf[128];
    switch (w.state) {
    case MUSE_WIFI_OFF:
        strlcpy(buf, T("Wi-Fi is off"), sizeof(buf));
        break;
    case MUSE_WIFI_NO_NETWORK:
        strlcpy(buf, T("No saved networks. Scan and pick one."), sizeof(buf));
        break;
    case MUSE_WIFI_CONNECTING:
        snprintf(buf, sizeof(buf), T("Joining %s\n%s"), w.ssid, w.detail);
        break;
    case MUSE_WIFI_CONNECTED:
        if (muse_board->epaper) {
            snprintf(buf, sizeof(buf), T("Connected to %s\n%s"), w.ssid, w.ip);
        } else {
            snprintf(buf, sizeof(buf), T("Connected to %s\n%s  -  %d dBm"), w.ssid, w.ip, w.rssi);
        }
        break;
    case MUSE_WIFI_NOT_NEARBY:
        strlcpy(buf, T("No saved network nearby\nLooking again within a minute"), sizeof(buf));
        break;
    case MUSE_WIFI_FAILED:
    default:
        snprintf(buf, sizeof(buf), T("Couldn't join %s\n%s"), w.ssid, w.detail);
        break;
    }
    set_text(s_wifi_status, buf);
    muse_theme_set_style_text_color(s_wifi_status, lv_color_hex(w.state == MUSE_WIFI_CONNECTED ? COLOR_OK :
                                                            w.state == MUSE_WIFI_FAILED ? COLOR_WARN : COLOR_DIM), 0);

    bool on = w.state != MUSE_WIFI_OFF;
    if (on != lv_obj_has_state(s_wifi_sw, LV_STATE_CHECKED)) {
        lv_obj_set_state(s_wifi_sw, LV_STATE_CHECKED, on);
    }
    lv_obj_set_flag(s_wifi_scan_btn, LV_OBJ_FLAG_HIDDEN, !on);
    lv_obj_set_flag(s_wifi_list, LV_OBJ_FLAG_HIDDEN, !on);
    set_text(s_wifi_scan_lbl, muse_wifi_scanning() ? "Scanning..." : LV_SYMBOL_REFRESH "  Scan for networks");
    rebuild_saved_list();
    tick_saved_list(&w);
    /* The scan found one: join it now rather than at the next look. */
    if (rebuild_scan_list() && w.state == MUSE_WIFI_NOT_NEARBY) {
        muse_wifi_apply();
    }
}

/* ---------- Muse ---------- */

static void on_hatch_host_done(const char *text)
{
    s_host_bad_us = muse_settings_set_hatch_host(text) ? 0 : esp_timer_get_time();
}
static void on_hatch_vm_done(const char *text) { muse_settings_set_hatch_vm(text); }

static void on_hatch_token_done(const char *text)
{
    if (text[0]) {
        muse_settings_set_hatch_token(text, false);
    }
}

/* A valid token is stored and the device restarts to use it. */
static void on_hatch_sdk_token_done(const char *text)
{
    if (text[0] && !muse_link_sdk_token_set(text)) {
        s_sdk_token_bad_us = esp_timer_get_time();
    }
}

static void on_hatch_sdk_token(lv_event_t *e)
{
    (void)e;
    open_text("SDK token", "", true, 48 /* mgst_ and 43 more */, "Empty keeps the current one; a new one restarts",
              on_hatch_sdk_token_done, s_hatch);
}

static void on_hatch_host(lv_event_t *e)
{
    (void)e;
    char host[MUSE_HOST_MAX + 1];
    muse_settings_hatch_host(host);
    open_text("Muse server", host, false, MUSE_HOST_MAX, "Empty for the default", on_hatch_host_done, s_hatch);
}

static void on_hatch_vm(lv_event_t *e)
{
    (void)e;
    char vm[MUSE_VM_MAX + 1];
    muse_settings_hatch_vm(vm);
    open_text("VM ID", vm, false, MUSE_VM_MAX, "Optional", on_hatch_vm_done, s_hatch);
}

static void on_hatch_token(lv_event_t *e)
{
    (void)e;
    open_text("Device token", "", true, MUSE_TOKEN_MAX, "Empty keeps the current one", on_hatch_token_done, s_hatch);
}

static void on_hatch_test(lv_event_t *e)
{
    (void)e;
    muse_hatch_test();
}

/* Two taps within a few seconds: this wipes Wi-Fi and the Muse app pairing. */
static void on_link_reset(lv_event_t *e)
{
    (void)e;
    int64_t now = esp_timer_get_time();
    if (s_link_reset_armed_us && now - s_link_reset_armed_us < 3000000) {   /* two taps within 3 s */
        set_text(s_link_reset_lbl, "Resetting...");
        muse_link_reset_setup();
        return;
    }
    s_link_reset_armed_us = now;
    set_text(s_link_reset_lbl, "Tap again to reset");
}

static void build_hatch_page(lv_obj_t *tile)
{
    lv_obj_t *list;
    s_hatch = page(tile, "Muse", true, &list);
    s_link_reset_armed_us = 0;
    s_link_status = note(list, "");
    s_hatch_status = note(list, "");
    row(list, NULL, "Server", &s_hatch_host, on_hatch_host, NULL);
    row(list, NULL, "VM ID", &s_hatch_vm, on_hatch_vm, NULL);
    row(list, NULL, "Device token", &s_hatch_token, on_hatch_token, NULL);
    row(list, NULL, "SDK token", &s_hatch_sdk_token, on_hatch_sdk_token, NULL);
    button(list, "Test connection", COLOR_ACCENT, on_hatch_test, NULL);
    note(list, "Pair with the Muse app to use your account; a device token here overrides it, and a long one is easier to send over Bluetooth. The VM ID picks one of your VMs. The SDK token is from gadgets.muse.ai; one set here replaces the firmware's. It shows as its length and the start of its SHA-256, never any of it. Reset pairing is on the Bluetooth page.");
}

static void tick_hatch(void)
{
    char link[96];
    const char *paired = !muse_link_hatch_linked()    ? "not paired"
                         : muse_link_sdk_token_changed() ? "paired with another SDK token, pair again to use this one"
                                                         : "paired";
    snprintf(link, sizeof(link), T("Muse app: %s\n%s"), T(paired), T(muse_link_state_name(muse_link_state())));
    set_text(s_link_status, link);
    muse_hatch_status_t h;
    muse_hatch_status(&h);
    char buf[96];
    if (h.detail[0]) {
        snprintf(buf, sizeof(buf), "%s\n%s", T(muse_hatch_state_name(h.state)), h.detail);
    } else {
        strlcpy(buf, muse_hatch_state_name(h.state), sizeof(buf));
    }
    set_text(s_hatch_status, buf);
    muse_theme_set_style_text_color(s_hatch_status, lv_color_hex(h.state == MUSE_HATCH_REACHABLE ? COLOR_OK :
                                                             h.state == MUSE_HATCH_UNREACHABLE ? COLOR_WARN : COLOR_DIM), 0);

    char host[MUSE_HOST_MAX + 1], vm[MUSE_VM_MAX + 1];
    muse_settings_hatch_host(host);
    muse_settings_hatch_vm(vm);
    if (s_host_bad_us && esp_timer_get_time() - s_host_bad_us < 5000000) {
        set_text(s_hatch_host, "Not allowed");
    } else {
        s_host_bad_us = 0;
        set_text(s_hatch_host, host);
    }
    /* One set here, else the one it connected to. */
    set_text(s_hatch_vm, vm[0] ? vm : h.vm_id[0] ? h.vm_id : "Not set");
    /* The device token is a credential, so none of it shows. Pairing stores one
     * (muse_link_hatch_linked); one typed in here overrides it. */
    set_text(s_hatch_token, muse_settings_hatch_token_len() ? "Set here" :
                            muse_link_hatch_linked() ? "Set" : "Not set");
    /* None of the SDK token shows (a screenshot ends up in a transcript):
     * whether one is set, its length and the start of its SHA-256 on a second
     * line, as the firmware logs it (main/token_fp.h). Two lines: the value
     * column is 130 px. */
    unsigned sdk_len;
    char sdk_fp[MUSE_LINK_FP_HEX];
    bool sdk = muse_link_sdk_token_fp(&sdk_len, sdk_fp);
    if (sdk) {
        snprintf(buf, sizeof(buf), sdk_fp[0] ? "Set, %u chars\n%s" : "Set, %u chars", sdk_len, sdk_fp);
    }
    if (s_sdk_token_bad_us && esp_timer_get_time() - s_sdk_token_bad_us < 5000000) {
        set_text(s_hatch_sdk_token, "Not a token");
    } else {
        s_sdk_token_bad_us = 0;
        set_text(s_hatch_sdk_token, sdk ? buf : "Not set");
    }
}

/* ---------- Bluetooth ---------- */

static void on_ble_sw(lv_event_t *e)
{
    muse_ble_phone_setup(lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED));
}

static void on_ble_forget(lv_event_t *e)
{
    (void)e;
    muse_ble_forget_all();
}

static void build_ble_page(lv_obj_t *tile)
{
    lv_obj_t *list;
    s_ble = page(tile, "Bluetooth", true, &list);
    s_link_reset_armed_us = 0;
    s_ble_sw = switch_row(list, "Phone setup", muse_settings_ble_on(), on_ble_sw);
    s_ble_status = note(list, "");
    /* Reset the Muse app pairing on the device, two taps within 3 s. */
    button(list, "Reset pairing", COLOR_DANGER, on_link_reset, &s_link_reset_lbl);
    button(list, "Forget paired phones", COLOR_DANGER, on_ble_forget, NULL);
#if CONFIG_MUSE_GADGET_TEST_BUILD
    note(list, "When on, Muse is visible to phones nearby. Open tools/muse/ble_setup.html in Chrome, connect, and enter the code Muse shows to pair. Once set up, it takes Wi-Fi changes for 10 min after you turn it on.");
#else
    note(list, "When on, Muse is visible to phones nearby. Set up Wi-Fi and your Muse account in the Muse app.");
#endif
}

static void tick_ble(void)
{
    muse_ble_status_t b;
    muse_ble_status(&b);
    char buf[96];
    switch (b.state) {
    case MUSE_BLE_OFF:
        strlcpy(buf, T("Off"), sizeof(buf));
        break;
    case MUSE_BLE_ADVERTISING:
        snprintf(buf, sizeof(buf), T("Visible as %s"), b.name);
        break;
    case MUSE_BLE_CONNECTED:
    default:
        snprintf(buf, sizeof(buf), T("Phone connected\n%s"), T(b.secure ? "Paired" : "Waiting for pairing"));
        break;
    }
    set_text(s_ble_status, buf);
    if (s_link_reset_armed_us && esp_timer_get_time() - s_link_reset_armed_us >= 3000000) {
        s_link_reset_armed_us = 0;
        set_text(s_link_reset_lbl, "Reset pairing");
    }
    muse_theme_set_style_text_color(s_ble_status, lv_color_hex(b.state == MUSE_BLE_CONNECTED && b.secure ? COLOR_OK : COLOR_DIM), 0);
    bool on = muse_settings_ble_on();
    if (on != lv_obj_has_state(s_ble_sw, LV_STATE_CHECKED)) {
        lv_obj_set_state(s_ble_sw, LV_STATE_CHECKED, on);
    }
}

/* ---------- Sound ---------- */

static void set_val(lv_obj_t *l, const char *fmt, int v)
{
    char buf[16];
    snprintf(buf, sizeof(buf), fmt, v);
    set_text(l, buf);
}

static void on_speaker_sw(lv_event_t *e)
{
    muse_settings_set_speaker_on(lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED));
}

#if CONFIG_MUSE_BOARD_M5STACK_STOPWATCH_FAMILY && CONFIG_MUSE_REALTIME_VOICE
static void on_notes_sw(lv_event_t *e)
{
    muse_settings_set_voice_notes(lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED));
}
#endif

#if CONFIG_MUSE_WAKEWORD_ENABLE
/* The listening opt-in, shared with wake.on and the quick menu, and persisted
 * by muse_wakeword_set_enabled. */
static void on_wake_sw(lv_event_t *e)
{
    muse_wakeword_set_enabled(lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED));
}
#endif

#if CONFIG_MUSE_PHOTO_NOTES
static void on_photo_sw(lv_event_t *e)
{
    muse_settings_set_photo_notes(lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED));
}
#endif

static void on_volume(lv_event_t *e)
{
    int v = lv_slider_get_value(s_vol_sl);
    set_val(s_vol_val, "%d%%", v);
    if (lv_event_get_code(e) == LV_EVENT_RELEASED) {
        muse_settings_set_volume(v);
        muse_voice_request_chirp();
    } else {
        muse_audio_set_volume(v);
    }
}

static void on_gain(lv_event_t *e)
{
    int db = lv_slider_get_value(s_gain_sl) * 3;
    set_val(s_gain_val, "%d dB", db);
    if (lv_event_get_code(e) == LV_EVENT_RELEASED) {
        muse_settings_set_mic_gain(db);
    } else {
        muse_audio_set_mic_gain(db);
    }
}

static void on_bright(lv_event_t *e)
{
    int v = lv_slider_get_value(s_bright_sl);
    set_val(s_bright_val, "%d%%", v);
    if (lv_event_get_code(e) == LV_EVENT_RELEASED) {
        muse_settings_set_brightness(v);
    } else {
        muse_ui_preview_brightness(v);
    }
}

static const char *const TEXT_SIZE_NAMES[MUSE_TEXT_SIZES] = {
    "Extra small", "Small", "Medium", "Large", "Extra large",
};

static void on_text_size(lv_event_t *e)
{
    int v = lv_slider_get_value(s_text_sl);
    set_text(s_text_val, TEXT_SIZE_NAMES[v]);
    if (lv_event_get_code(e) == LV_EVENT_RELEASED) {
        muse_settings_set_text_size(v);   /* the face picks it up on its next frame */
    }
}

/* Reply text size, under Brightness, on screens whose replies follow it. */
static void text_size_row(lv_obj_t *list)
{
    if (!muse_ui_text_size_applies()) {
        return;
    }
    s_text_sl = slider(list, "Text size", MUSE_TEXT_XSMALL, MUSE_TEXT_XLARGE, muse_settings_text_size(),
                       &s_text_val, on_text_size);
    set_text(s_text_val, TEXT_SIZE_NAMES[muse_settings_text_size()]);
}

static void build_sound_page(lv_obj_t *tile)
{
    lv_obj_t *list;
    s_sound = page(tile, "Sound", true, &list);
#if CONFIG_MUSE_BOARD_M5STACK_STOPWATCH_FAMILY && CONFIG_MUSE_REALTIME_VOICE
    s_notes_sw = switch_row(list, "Voice notes", muse_settings_voice_notes(), on_notes_sw);
    note(list, "Off: hold yellow to talk live, release to listen; blue ends the call. On: hold yellow to send a voice note. Applies to the next call.");
#endif
    if (muse_board->audio_init) {
        s_spk_sw = switch_row(list, "Speaker", muse_settings_speaker_on(), on_speaker_sw);
        /* Muted shows here too, before the
         * switch, for as long as it lasts: the top row's glyph at its size
         * on the home layout, else the font's mute symbol. */
        lv_obj_t *row = lv_obj_get_parent(s_spk_sw);
        int ink_h = muse_home_top_h();
        const lv_image_dsc_t *ink = ink_h ? muse_icon_ink(MUSE_ICON_VOLUME_OFF, ink_h) : NULL;
        if (ink) {
            s_spk_mute = lv_image_create(row);
            lv_image_set_src(s_spk_mute, ink);
            lv_obj_set_style_image_recolor(s_spk_mute, lv_color_hex(COLOR_TEXT), 0);
            lv_obj_set_style_image_recolor_opa(s_spk_mute, LV_OPA_COVER, 0);
        } else {
            s_spk_mute = label(row, &lv_font_montserrat_20, COLOR_TEXT, LV_SYMBOL_MUTE);
        }
        lv_obj_remove_flag(s_spk_mute, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_move_to_index(s_spk_mute, lv_obj_get_index(s_spk_sw));
        lv_obj_set_flag(s_spk_mute, LV_OBJ_FLAG_HIDDEN,
                        !muse_vol_muted(muse_settings_speaker_on(), muse_settings_volume()));
        s_vol_sl = slider(list, "Volume", 0, 100, muse_settings_volume(), &s_vol_val, on_volume);
    }
    s_gain_sl = slider(list, "Mic gain", 0, MUSE_MIC_GAIN_MAX / 3, muse_settings_mic_gain() / 3, &s_gain_val, on_gain);

    lv_obj_t *meter = lv_obj_create(list);
    lv_obj_remove_style_all(meter);
    lv_obj_set_size(meter, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(meter, 8, 0);
    lv_obj_remove_flag(meter, LV_OBJ_FLAG_SCROLLABLE);
    label(meter, &lv_font_montserrat_16, COLOR_DIM, "Mic level");
    s_mic_val = label(meter, &lv_font_montserrat_16, COLOR_DIM, "");
    lv_obj_align(s_mic_val, LV_ALIGN_TOP_RIGHT, 0, 0);
    s_mic_bar = lv_bar_create(meter);
    lv_obj_set_size(s_mic_bar, lv_pct(94), 10);
    lv_obj_align(s_mic_bar, LV_ALIGN_TOP_MID, 0, 26);
    lv_bar_set_range(s_mic_bar, 0, 60);   /* -70..-10 dBFS */
    muse_theme_set_style_bg_color(s_mic_bar, lv_color_hex(0x2a2345), LV_PART_MAIN);
    lv_obj_set_style_anim_duration(s_mic_bar, 80, 0);
    if (muse_board->epaper) {
        lv_obj_add_flag(meter, LV_OBJ_FLAG_HIDDEN);
    } else {
        note(list, "Talk at arm's length: the bar should reach green (-30 to -15 dBFS) without going orange.");
    }

#if CONFIG_MUSE_WAKEWORD_ENABLE
    s_wake_sw = switch_row(list, "Wake word", muse_wakeword_enabled(), on_wake_sw);
    note(list, "Listens for \"Hey Muse\" so you can start a voice note without holding the talk button.");
#endif
#if CONFIG_MUSE_PHOTO_NOTES
    switch_row(list, "Photo with voice notes", muse_settings_photo_notes(), on_photo_sw);
    note(list, "Holding the talk button also takes a photo, which goes to Muse with your voice note.");
#endif
    if (s_vol_val) set_val(s_vol_val, "%d%%", muse_settings_volume());
    set_val(s_gain_val, "%d dB", muse_settings_mic_gain() / 3 * 3);
}

static void tick_sound(void)
{
#if CONFIG_MUSE_BOARD_M5STACK_STOPWATCH_FAMILY && CONFIG_MUSE_REALTIME_VOICE
    lv_obj_set_state(s_notes_sw, LV_STATE_CHECKED, muse_settings_voice_notes());
#endif
    bool on = muse_settings_speaker_on();   /* also toggled from the face, the phone and the console */
    if (s_spk_sw && on != lv_obj_has_state(s_spk_sw, LV_STATE_CHECKED)) {
        lv_obj_set_state(s_spk_sw, LV_STATE_CHECKED, on);
    }
#if CONFIG_MUSE_WAKEWORD_ENABLE
    bool wake_on = muse_wakeword_enabled();   /* also toggled by the menu, serial, wakeword.disable */
    if (s_wake_sw && wake_on != lv_obj_has_state(s_wake_sw, LV_STATE_CHECKED)) {
        lv_obj_set_state(s_wake_sw, LV_STATE_CHECKED, wake_on);
    }
#endif
    if (s_spk_mute) {   /* muted for as long as it lasts */
        bool muted = muse_vol_muted(on, muse_settings_volume());
        if (muted == lv_obj_has_flag(s_spk_mute, LV_OBJ_FLAG_HIDDEN)) {
            lv_obj_set_flag(s_spk_mute, LV_OBJ_FLAG_HIDDEN, !muted);
            ESP_LOGI("muse_settings_ui", "Sound page: muted icon %s", muted ? "on" : "off");
        }
    }
    /* The level follows a change made elsewhere (the phone, the console, a
     * key), except while a finger is on the slider. */
    if (s_vol_sl && !lv_obj_has_state(s_vol_sl, LV_STATE_PRESSED) &&
        lv_slider_get_value(s_vol_sl) != muse_settings_volume()) {
        lv_slider_set_value(s_vol_sl, muse_settings_volume(), LV_ANIM_OFF);
        set_val(s_vol_val, "%d%%", muse_settings_volume());
    }
    if (muse_board->epaper) return;
    float db = muse_voice_monitor_db();
    int v = (int)(db + 70.0f);
    v = v < 0 ? 0 : (v > 60 ? 60 : v);
    lv_bar_set_value(s_mic_bar, v, LV_ANIM_ON);
    uint32_t color = db > -12.0f ? COLOR_WARN : (db > -30.0f ? COLOR_OK : COLOR_ACCENT);
    muse_theme_set_style_bg_color(s_mic_bar, lv_color_hex(color), LV_PART_INDICATOR);
    set_val(s_mic_val, "%d dBFS", (int)db);
}

/* ---------- Display (brightness and the screen timeout) ---------- */

static void tick_sleep(void);

static void on_sleep_choice(lv_event_t *e)
{
    int secs = SLEEP_CHOICES[(int)(intptr_t)lv_event_get_user_data(e)];
    muse_settings_set_sleep_s(secs);
    tick_sleep();
}

static void on_charged_choice(lv_event_t *e)
{
    muse_settings_set_sleep_when_charged((intptr_t)lv_event_get_user_data(e) == 0);
    tick_sleep();
}

static void on_sleep_now(lv_event_t *e)
{
    (void)e;
    muse_state_set_asleep(true);
}

#if CONFIG_MUSE_MOTION_GESTURES
static void on_lift_sw(lv_event_t *e)
{
    muse_settings_set_lift_wake(lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED));
}

static void on_face_quiet_sw(lv_event_t *e)
{
    muse_settings_set_face_down_quiet(lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED));
}
#endif

static void build_sleep_page(lv_obj_t *tile)
{
    lv_obj_t *list;
    s_sleep = page(tile, "Display", true, &list);
    s_sleep_shown = -1;
    if (!muse_board->epaper) {
        s_bright_sl = slider(list, "Brightness", 10, 100, muse_settings_brightness(), &s_bright_val, on_bright);
        set_val(s_bright_val, "%d%%", muse_settings_brightness());
        text_size_row(list);
    }
    note(list, "Screen timeout");
    for (int i = 0; i < SLEEP_COUNT; i++) {
        s_sleep_rows[i] = row(list, NULL, SLEEP_NAMES[i], &s_sleep_checks[i], on_sleep_choice,
                              (void *)(intptr_t)i);
        muse_theme_set_style_text_color(s_sleep_checks[i], lv_color_hex(COLOR_ACCENT), 0);
    }
    button(list, LV_SYMBOL_EYE_CLOSE "  Sleep now", COLOR_ACCENT, on_sleep_now, NULL);
    s_chg_shown = -1;
    if (muse_board->charged != MUSE_CHARGED_NONE) {
        note(list, "Sleep when charged");
        for (int i = 0; i < 2; i++) {
            s_chg_rows[i] = row(list, NULL, CHARGED_NAMES[i], &s_chg_checks[i], on_charged_choice, (void *)(intptr_t)i);
            muse_theme_set_style_text_color(s_chg_checks[i], lv_color_hex(COLOR_ACCENT), 0);
        }
        note(list, muse_board->charged == MUSE_CHARGED_DONE
                       ? "Never keeps a lit screen on from when the charger reports full until USB is unplugged."
                       : "Never keeps a lit screen on while USB is in and the battery is at 90% or more.");
    }
    note(list, "On USB the screen times out as on battery. Tap the screen, press a button or plug in USB to wake.");
#if CONFIG_MUSE_MOTION_GESTURES
    switch_row(list, "Lift to wake", muse_settings_lift_wake(), on_lift_sw);
    switch_row(list, "Face-down quiet", muse_settings_face_down_quiet(), on_face_quiet_sw);
    note(list, "Lift to wake: lift and tilt the screen toward you. "
               "Face-down quiet silences playback until you turn it back. "
               "Motion controls use extra battery and cannot wake a powered-off device.");
#endif
}

static const char *sleep_name(int secs)
{
    for (int i = 0; i < SLEEP_COUNT; i++) {
        if (SLEEP_CHOICES[i] == secs) {
            return SLEEP_SHORT[i];
        }
    }
    static char custom[16];
    snprintf(custom, sizeof(custom), "%d s", secs);
    return custom;
}

static void tick_sleep(void)
{
    int cur = muse_settings_sleep_s();
    for (int i = 0; i < SLEEP_COUNT; i++) {
        set_text(s_sleep_checks[i], SLEEP_CHOICES[i] == cur ? LV_SYMBOL_OK : "");
        if (muse_board->epaper && cur != s_sleep_shown) {
            muse_theme_set_style_bg_color(s_sleep_rows[i],
                                      lv_color_hex(SLEEP_CHOICES[i] == cur ? COLOR_CARD_PRESSED : COLOR_CARD), 0);
        }
    }
    s_sleep_shown = cur;
    if (muse_board->charged == MUSE_CHARGED_NONE || !s_chg_checks[0]) {
        return;
    }
    int chg = muse_settings_sleep_when_charged() ? 0 : 1;
    for (int i = 0; i < 2; i++) {
        set_text(s_chg_checks[i], i == chg ? LV_SYMBOL_OK : "");
        if (muse_board->epaper && chg != s_chg_shown) {
            lv_obj_set_style_bg_color(s_chg_rows[i], lv_color_hex(i == chg ? COLOR_CARD_PRESSED : COLOR_CARD), 0);
        }
    }
    s_chg_shown = chg;
}

/* ---------- Watch face (the face picker's own page, never under Display) ---------- */

/* The face picker first: Pixel Muse, or the avatar face
 * named after the Muse ("My Muse" until its name is known). Then, only while
 * the avatar face is picked, its Size (the Watch face setting):
 * Large or Small, a tap switching between them. On boards with the avatar
 * face (muse_link_face_available). */
static lv_obj_t *s_face_rows[2], *s_face_checks[2], *s_face_avatar_lbl, *s_face_size, *s_face_size_val;

static void tick_face(void);

static void on_face_choice(lv_event_t *e)
{
    muse_link_face_pick((intptr_t)lv_event_get_user_data(e) != 0);
    tick_face();
}

static void on_face_size(lv_event_t *e)
{
    (void)e;
    muse_link_face_size_set(!muse_link_face_size_small());
    tick_face();
}

static void build_face_page(lv_obj_t *tile)
{
    lv_obj_t *list;
    s_facep = page(tile, "Watch face", true, &list);
    s_face_rows[0] = row(list, NULL, "Pixel Muse", &s_face_checks[0], on_face_choice, (void *)(intptr_t)0);
    char name[48];
    muse_link_face_label(name, sizeof(name));
    s_face_rows[1] = row(list, NULL, name, &s_face_checks[1], on_face_choice, (void *)(intptr_t)1);
    s_face_avatar_lbl = lv_obj_get_child(s_face_rows[1], 0);
    for (int i = 0; i < 2; i++) {
        lv_obj_set_style_text_color(s_face_checks[i], lv_color_hex(COLOR_ACCENT), 0);
    }
    s_face_size = row(list, NULL, "Size", &s_face_size_val, on_face_size, NULL);
}

static void tick_face(void)
{
    if (!s_facep) {
        return;
    }
    bool avatar = muse_link_face_avatar_picked();
    set_text(s_face_checks[0], avatar ? "" : LV_SYMBOL_OK);
    set_text(s_face_checks[1], avatar ? LV_SYMBOL_OK : "");
    char name[48];
    muse_link_face_label(name, sizeof(name));
    set_text(s_face_avatar_lbl, name);
    lv_obj_set_flag(s_face_size, LV_OBJ_FLAG_HIDDEN, !avatar);   /* the avatar face's own option */
    set_text(s_face_size_val, muse_link_face_size_small() ? "Small" : "Large");
}

/* ---------- Talk (screen, button or both) ---------- */

static const char *const TALK_NAMES[] = { "Screen and button", "Screen only", "Button only" };
static lv_obj_t *s_talk_checks[3], *s_haptics_sw;

static void tick_talk(void)
{
    muse_talk_mode_t mode = muse_settings_talk_mode();
    for (int i = 0; i < 3; i++) {
        set_text(s_talk_checks[i], (int)mode == i ? LV_SYMBOL_OK : "");
    }
    if (s_haptics_sw && muse_settings_haptics() != lv_obj_has_state(s_haptics_sw, LV_STATE_CHECKED)) {
        lv_obj_set_state(s_haptics_sw, LV_STATE_CHECKED, muse_settings_haptics());
    }
}

static void on_talk_choice(lv_event_t *e)
{
    muse_settings_set_talk_mode((muse_talk_mode_t)(intptr_t)lv_event_get_user_data(e));
    tick_talk();
}

static void on_haptics_sw(lv_event_t *e)
{
    muse_settings_set_haptics(lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED));
}

static void build_talk_page(lv_obj_t *tile)
{
    lv_obj_t *list;
    s_talk = page(tile, "Talk", true, &list);
    for (int i = 0; i < 3; i++) {
        row(list, NULL, TALK_NAMES[i], &s_talk_checks[i], on_talk_choice, (void *)(intptr_t)i);
        lv_obj_set_style_text_color(s_talk_checks[i], lv_color_hex(COLOR_ACCENT), 0);
    }
    char text[128];
    snprintf(text, sizeof(text), "Hold Muse on the screen, or the %s button, to talk.", muse_board->talk_button);
    note(list, text);
    if (muse_board->buzz) {
        s_haptics_sw = switch_row(list, "Vibrate to talk", muse_settings_haptics(), on_haptics_sw);
        note(list, "A short buzz when Muse starts listening.");
    }
    tick_talk();
}

/* ---------- Passcode ---------- */

/* Settings > Passcode: off, one card, "Turn passcode on"; on, the switch,
 * Change passcode and Lock (Screen off / After 5 min). Turning it on,
 * changing it and turning it off run on the passcode's keypad (muse_passcode_ui.c); Change and Turn off
 * ask for the current code first. */
static lv_obj_t *s_pass_on, *s_pass_sw, *s_pass_change, *s_pass_lock, *s_pass_lock_val;
static lv_obj_t *s_pass_lock_rows[2], *s_pass_lock_checks[2];
static void build_pass_page(lv_obj_t *tile);
static void build_pass_lock_page(lv_obj_t *tile);
static const page_t PASS = { &s_passp, build_pass_page, NULL };
static const page_t PASS_LOCK = { &s_pass_lockp, build_pass_lock_page, &PASS };

/* A label that fits beside the icon on a card, else the short one. */
static const char *fit_label(const char *full, const char *shorter)
{
    lv_point_t sz;
    lv_text_get_size(&sz, full, s_font_label, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return sz.x <= s_list_w - 2 * s_pad - s_icon_px - s_icon_gap ? full : shorter;
}

/* A value that fits beside its label (at most 130 s), else the short one. */
static const char *fit_value(const char *label_text, const char *full, const char *shorter)
{
    lv_point_t lw, vw;
    lv_text_get_size(&lw, label_text, s_font_label, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    lv_text_get_size(&vw, full, s_font_value, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    int vgap = pr(0.6 * s_icon_gap) > 6 ? pr(0.6 * s_icon_gap) : 6;
    int room = s_list_w - 2 * s_pad - s_icon_px - s_icon_gap - lw.x - vgap;
    return vw.x <= room && vw.x <= s_value_w ? full : shorter;
}

static const char *pass_lock_name(muse_pass_lock_t m)
{
    return m == MUSE_PASS_LOCK_AFTER_5_MIN ? fit_value("Lock", "After 5 min", "5 min") : fit_value("Lock", "Screen off", "Sleep");
}

/* The page again, as the passcode now is (on, off). */
static void pass_rebuild(bool changed)
{
    if (!changed || !s_passp || s_current != s_passp) {
        if (s_pass_sw) lv_obj_set_state(s_pass_sw, LV_STATE_CHECKED, muse_passcode_on());
        return;
    }
    lv_obj_t *old = s_passp;
    s_passp = NULL;
    build_pass_page(s_tile);
    show(s_passp);
    drop(old);
}

static void on_pass_on(lv_event_t *e)
{
    (void)e;
    muse_passcode_ui_flow(MUSE_PASS_FLOW_SETUP, pass_rebuild);
}

static void on_pass_change(lv_event_t *e)
{
    (void)e;
    muse_passcode_ui_flow(MUSE_PASS_FLOW_CHANGE, pass_rebuild);
}

static void on_pass_sw(lv_event_t *e)
{
    if (!lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED)) {
        lv_obj_add_state(s_pass_sw, LV_STATE_CHECKED);   /* off only once the current code is entered */
        muse_passcode_ui_flow(MUSE_PASS_FLOW_OFF, pass_rebuild);
    }
}

static void build_pass_page(lv_obj_t *tile)
{
    lv_obj_t *list;
    s_passp = page(tile, "Passcode", true, &list);
    s_pass_on = s_pass_sw = s_pass_change = s_pass_lock = s_pass_lock_val = NULL;
    if (!muse_passcode_on()) {
        s_pass_on = icon_row(list, MUSE_ICON_LOCK, "Turn passcode on", NULL, on_pass_on, NULL);
        muse_settings_ui_card_fit(s_pass_on, s_list_w, 0);
        return;
    }
    lv_obj_t *c = card(list, false);
    muse_icon_create(c, MUSE_ICON_LOCK, s_icon_px, COLOR_ACCENT);
    lv_obj_t *t = label(c, s_font_label, COLOR_TEXT, "Passcode");
    lv_obj_set_flex_grow(t, 1);
    center_row_caps(t, NULL, s_icon_px);
    s_pass_sw = lv_switch_create(c);
    lv_obj_set_size(s_pass_sw, 60, 32);
    lv_obj_set_style_bg_color(s_pass_sw, lv_color_hex(0x3a3358), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_pass_sw, lv_color_hex(COLOR_ACCENT), LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_add_state(s_pass_sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(s_pass_sw, on_pass_sw, LV_EVENT_VALUE_CHANGED, NULL);
    muse_settings_ui_card_fit(c, s_list_w, 60 + s_icon_gap);
    s_pass_lock = icon_row(list, MUSE_ICON_DISPLAY, "Lock", &s_pass_lock_val, on_nav, (void *)&PASS_LOCK);
    set_text(s_pass_lock_val, pass_lock_name(muse_passcode_lock_mode()));
    s_pass_change = icon_row(list, MUSE_ICON_TOUCH, fit_label("Change passcode", "Change code"), NULL, on_pass_change,
                             NULL);
    muse_settings_ui_card_fit(s_pass_change, s_list_w, 0);
}

static void tick_pass(void)
{
    if (s_pass_lock_val) {
        set_text(s_pass_lock_val, pass_lock_name(muse_passcode_lock_mode()));
    }
}

static void tick_pass_lock(void)
{
    muse_pass_lock_t m = muse_passcode_lock_mode();
    for (int i = 0; i < 2; i++) {
        set_text(s_pass_lock_checks[i], (int)m == i ? LV_SYMBOL_OK : "");
    }
}

static void on_pass_lock_choice(lv_event_t *e)
{
    muse_passcode_set_lock_mode((muse_pass_lock_t)(intptr_t)lv_event_get_user_data(e));
    tick_pass_lock();
}

static void build_pass_lock_page(lv_obj_t *tile)
{
    lv_obj_t *list;
    s_pass_lockp = page(tile, "Lock", true, &list);
    static const char *const NAMES[] = { "Screen off", "After 5 min" };
    for (int i = 0; i < 2; i++) {
        s_pass_lock_rows[i] = row(list, NULL, NAMES[i], &s_pass_lock_checks[i], on_pass_lock_choice, (void *)(intptr_t)i);
        lv_obj_set_style_text_color(s_pass_lock_checks[i], lv_color_hex(COLOR_ACCENT), 0);
    }
    tick_pass_lock();
}

/* ---------- About ---------- */

static lv_obj_t *s_about_fw, *s_about_id, *s_about_model, *s_about_note;
static const char *about_hint(void)
{
    return muse_settings_developer() ? "Muse Gadgets. Developer mode is on." : "Muse Gadgets. Tap FW five times for developer mode.";
}

static void on_about_fw(lv_event_t *e);

static void build_about_page(lv_obj_t *tile)
{
    lv_obj_t *list;
    s_aboutp = page(tile, "About", true, &list);
    s_about_model = info_row(list, "Model");
    s_about_fw = info_row(list, "FW");
    lv_obj_add_flag(lv_obj_get_parent(s_about_fw), LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(lv_obj_get_parent(s_about_fw), on_about_fw, LV_EVENT_CLICKED, NULL);
    s_about_id = info_row(list, "ID");
    s_about_reset = info_row(list, "Last reset");
    s_about_note = note(list, about_hint());
    s_fw_taps = 0;
}

static void tick_about(void)
{
    set_text(s_about_model, muse_board->name);
    set_text(s_about_fw, esp_app_get_description()->version);
    muse_ble_status_t b;
    muse_ble_status(&b);
    set_text(s_about_id, b.name);
    set_text(s_about_reset, muse_settings_ui_last_reset());
    /* The tap counter goes back to the hint once the taps stop. */
    if (s_fw_taps && esp_timer_get_time() - s_fw_tap_us >= 3000000) {
        s_fw_taps = 0;
        set_text(s_about_note, about_hint());
    }
}

/* ---------- Battery ---------- */

static void on_battery_reset(lv_event_t *e)
{
    (void)e;
    muse_battery_reset();
    s_batt_shown_us = 0;   /* show it now */
}

static void build_battery_page(lv_obj_t *tile)
{
    lv_obj_t *list;
    s_battery = page(tile, "Battery", true, &list);
    s_batt_shown_us = 0;
    s_batt_status = note(list, "");
    s_batt_level = info_row(list, "Battery");
    s_batt_drain = info_row(list, "Used");
    s_batt_full = info_row(list, "A full charge");
    s_batt_off = info_row(list, "Screen off");
    s_batt_slept = info_row(list, "Chip asleep");
    s_batt_wakes = info_row(list, "Wakes");
    s_batt_busy = info_row(list, "CPU busy");
    s_batt_awake = note(list, "");
    button(list, LV_SYMBOL_REFRESH "  Start over", COLOR_ACCENT, on_battery_reset, NULL);
    note(list, "Measures from unplugging USB until it's plugged back in. The gauge moves in 1% steps, so give it a few hours. Chip asleep is time in light sleep; CPU busy is time a core was running a task.");
}

/* A per-mille figure as a percentage. */
static void set_pm(lv_obj_t *l, int pm)
{
    char buf[16] = "-";
    if (pm >= 0) {
        snprintf(buf, sizeof(buf), "%d.%d%%", pm / 10, pm % 10);
    }
    set_text(l, buf);
}

static void tick_battery(void)
{
    int64_t now = esp_timer_get_time();
    if (s_batt_shown_us && now - s_batt_shown_us < (muse_board->epaper ? 30000000 : 1000000)) {
        return;
    }
    s_batt_shown_us = now;
    muse_battery_t b;
    muse_battery_read(&b);
    muse_power_t p = muse_state_power();
    char buf[96], t[24];

    int h = (int)(b.secs / 3600), m = (int)(b.secs / 60 % 60);
    if (h) {
        snprintf(t, sizeof(t), T("%d h %d min"), h, m);
    } else {
        snprintf(t, sizeof(t), T("%d min"), m);
    }
    if (!b.started) {
        strlcpy(buf, T(p.battery_pct < 0 ? "No battery" : "Unplug USB to start measuring."), sizeof(buf));
    } else {
        snprintf(buf, sizeof(buf), T(b.running ? "On battery for %s" : "Last run: %s on battery"), t);
    }
    set_text(s_batt_status, buf);

    if (p.battery_pct < 0) {
        strlcpy(buf, T("None"), sizeof(buf));
    } else if (p.battery_mv) {
        snprintf(buf, sizeof(buf), "%s%d%%  %d.%02d V", p.charging ? LV_SYMBOL_CHARGE " " : "", p.battery_pct,
                 p.battery_mv / 1000, p.battery_mv % 1000 / 10);
    } else {
        snprintf(buf, sizeof(buf), "%s%d%%", p.charging ? LV_SYMBOL_CHARGE " " : "", p.battery_pct);
    }
    set_text(s_batt_level, buf);

    int used = b.pct_start - b.pct_now, rate10, full_h;
    if (!b.started) {
        set_text(s_batt_drain, "-");
        set_text(s_batt_full, "-");
    } else if (muse_battery_drain(&b, &rate10, &full_h)) {
        snprintf(buf, sizeof(buf), "%d%%, %d.%d%%/h", used, rate10 / 10, rate10 % 10);
        set_text(s_batt_drain, buf);
        snprintf(buf, sizeof(buf), T("lasts ~%d h"), full_h);
        set_text(s_batt_full, buf);
    } else {
        snprintf(buf, sizeof(buf), T("%d%% so far"), used > 0 ? used : 0);
        set_text(s_batt_drain, buf);
        set_text(s_batt_full, "measuring");
    }

    set_pm(s_batt_off, b.started ? b.screen_off_pm : -1);
    set_pm(s_batt_slept, b.started ? b.slept_pm : -1);
    set_pm(s_batt_busy, b.started ? b.busy_pm : -1);
    if (b.started && b.secs && b.slept_pm >= 0) {
        int per10 = (int)(b.sleeps * 10LL / b.secs);
        snprintf(buf, sizeof(buf), "%d.%d/s", per10 / 10, per10 % 10);
        set_text(s_batt_wakes, buf);
    } else {
        set_text(s_batt_wakes, "-");
    }
    buf[0] = '\0';
    if (b.started && b.awake[0]) {
        snprintf(buf, sizeof(buf), T("Also kept awake by: %s"), b.awake);
    }
    set_text(s_batt_awake, buf);
}

/* ---------- Power off (a destructive row: two taps within 3 s) ---------- */

static void on_power_row(lv_event_t *e)
{
    (void)e;
    int64_t now = esp_timer_get_time();
    /* A board whose latch USB holds on (the ESP-Mosaico) can't power off on
     * USB: say so at once rather than a goodbye that ends in "Couldn't power
     * off" (a power-off shows what happens the moment it's asked). */
    if (muse_board->power_off_battery_only && !muse_state_on_battery_real()) {   /* never a bench nap's as-if battery */
        s_power_armed_us = 0;
        set_text(s_home_power, "Unplug first");
        return;
    }
    if (s_power_armed_us && now - s_power_armed_us < 3000000) {
        s_power_armed_us = 0;
        show(s_home);
        muse_ui_show_face();
        muse_input_request_power_off();
        return;
    }
    s_power_armed_us = now;
    set_text(s_home_power, "Tap again");
}

/* Restart, the same way: the input task ends a call and restarts (muse_input.c). */
static void on_restart_row(lv_event_t *e)
{
    (void)e;
    int64_t now = esp_timer_get_time();
    if (s_restart_armed_us && now - s_restart_armed_us < 3000000) {
        s_restart_armed_us = 0;
        show(s_home);
        muse_ui_show_face();
        muse_input_request_restart();
        return;
    }
    s_restart_armed_us = now;
    set_text(s_home_restart, "Tap again");
}

/* ---------- Developer (only in developer mode) ---------- */

static lv_obj_t *s_dev_heap, *s_dev_up, *s_dev_restart_lbl, *s_dev_sw, *s_dev_tests, *s_tests_vib;
/* Developer > Tests > Ask Muse for an image (muse_link_image_*): its button
 * and the line under it. */
static lv_obj_t *s_tests_img, *s_tests_img_lbl, *s_tests_img_note;
static int64_t s_dev_restart_us;
static void build_dev_page(lv_obj_t *tile);
static void build_tests_page(lv_obj_t *tile);
static const page_t DEV = { &s_dev, build_dev_page, NULL };
static const page_t TESTS = { &s_tests, build_tests_page, &DEV };

/* The board's vibration actuator, where the haptics engine runs one
 * (the page only on gadgets with one). */
static bool vib_caps(void *out)
{
#if MUSE_VIB_TESTS
    mght_caps_t c;
    bool ok = mg_ble_haptic_test_caps(&c);
    if (out) {
        *(mght_caps_t *)out = c;
    }
    return ok;
#else
    (void)out;
    return false;
#endif
}

/* Developer mode's row in Settings follows the setting (a console or a
 * factory reset may change it too). */
static void sync_dev_row(void)
{
    if (s_home_dev && lv_obj_has_flag(s_home_dev, LV_OBJ_FLAG_HIDDEN) == muse_settings_developer()) {
        lv_obj_set_flag(s_home_dev, LV_OBJ_FLAG_HIDDEN, !muse_settings_developer());
    }
}

static void on_dev_tone(lv_event_t *e)
{
    (void)e;
    muse_voice_request_chirp();
}

static void on_dev_restart(lv_event_t *e)
{
    (void)e;
    int64_t now = esp_timer_get_time();
    if (s_dev_restart_us && now - s_dev_restart_us < 3000000) {
        set_text(s_dev_restart_lbl, "Restarting...");
        muse_settings_ui_restart();
        return;
    }
    s_dev_restart_us = now;
    set_text(s_dev_restart_lbl, "Tap again to restart");
}

/* The switch at the top: off leaves for Settings, where the row is gone. */
static void on_dev_sw(lv_event_t *e)
{
    bool on = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    muse_settings_set_developer(on);
    sync_dev_row();
    if (!on) {
        show(s_home);
    }
}

static void build_dev_page(lv_obj_t *tile)
{
    lv_obj_t *list;
    s_dev = page(tile, "Developer", true, &list);
    s_dev_restart_us = 0;
    s_dev_tests = NULL;
    s_dev_sw = switch_row(list, "Developer mode", muse_settings_developer(), on_dev_sw);
    s_dev_heap = dev_row(list, "Free memory");
    s_dev_up = dev_row(list, "Up");
    /* Tests: only when the gadget has one (Vibration with an actuator, Ask
     * Muse for an image with the device session's picture test). */
    bool vib = vib_caps(NULL), img = muse_link_image_test();
    if (vib || img) {
        lv_obj_t *v;
        s_dev_tests = row(list, NULL, "Tests", &v, on_nav, (void *)&TESTS);
        set_text(v, vib && img ? "2 tests" : vib ? "Vibration" : "Muse image");
    }
    if (muse_board->audio_init) {
        button(list, "Test tone", COLOR_ACCENT, on_dev_tone, NULL);
    }
    button(list, "Restart", COLOR_DANGER, on_dev_restart, &s_dev_restart_lbl);
    note(list, "Bench tools: the USB console (>status, >face=, >call, >vib=) and tools/muse/snap.py.");
}

static void tick_dev(void)
{
    char buf[48];
    snprintf(buf, sizeof(buf), "%u KB", (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
    set_text(s_dev_heap, buf);
    int64_t up = esp_timer_get_time() / 1000000;
    snprintf(buf, sizeof(buf), "%d h %02d min", (int)(up / 3600), (int)(up / 60 % 60));
    set_text(s_dev_up, buf);
    if (s_dev_restart_us && esp_timer_get_time() - s_dev_restart_us >= 3000000) {
        s_dev_restart_us = 0;
        set_text(s_dev_restart_lbl, "Restart");
    }
}

/* ---------- Developer > Tests > Vibration ---------- */

#if MUSE_VIB_TESTS
static void build_vib_page(lv_obj_t *tile);
static const page_t VIB = { &s_vib, build_vib_page, &TESTS };

static const uint16_t VIB_PULSES[MGHT_PULSE_COUNT] = MGHT_PULSES;
static mght_caps_t s_vib_caps;
static int s_vib_step;   /* the strength step: a test's only, never saved ("a test never changes settings") */
static lv_obj_t *s_vib_status, *s_vib_budget, *s_vib_str_val, *s_vib_minus, *s_vib_plus, *s_vib_stop;
static lv_obj_t *s_vib_pulse[MGHT_PULSE_COUNT], *s_vib_preset[MGHT_PRESET_LAST + 1], *s_vib_ring_val;

static uint8_t vib_strength(void)
{
    return (s_vib_caps.flags & MGHT_FLAG_STRENGTH) ? mght_strength_pct(&s_vib_caps, s_vib_step) : 0;
}

static void tick_vib(void);

static void vib_run(const mght_cmd_t *c)
{
    mg_ble_haptic_test(c);   /* the result shows in the status line */
    tick_vib();
}

static void on_vib_pulse(lv_event_t *e)
{
    mght_cmd_t c = { .op = MGHT_OP_PULSE, .strength = vib_strength() };
    c.ms = VIB_PULSES[(intptr_t)lv_event_get_user_data(e)];
    vib_run(&c);
}

static void on_vib_preset(lv_event_t *e)
{
    mght_cmd_t c = { .op = MGHT_OP_PRESET, .strength = vib_strength() };
    c.preset = (uint8_t)(intptr_t)lv_event_get_user_data(e);   /* Ring again stops it (mght_run) */
    vib_run(&c);
}

static void on_vib_stop(lv_event_t *e)
{
    (void)e;
    mght_cmd_t c = { .op = MGHT_OP_STOP };
    vib_run(&c);
}

static void show_strength(void)
{
    char buf[16];
    snprintf(buf, sizeof(buf), "%u%%", (unsigned)vib_strength());
    set_text(s_vib_str_val, buf);
}

static void on_vib_strength(lv_event_t *e)
{
    int step = s_vib_step + (int)(intptr_t)lv_event_get_user_data(e);
    s_vib_step = step < 1 ? 1 : step > s_vib_caps.strength_steps ? s_vib_caps.strength_steps : step;
    show_strength();
}

static void vib_leave(void)
{
    mght_status_t st;
    mg_ble_haptic_test_status(&st);
    if (st.playing) {
        mght_cmd_t c = { .op = MGHT_OP_STOP };
        mg_ble_haptic_test(&c);
    }
}

/* A small square button for the strength card's - and +. */
static lv_obj_t *step_button(lv_obj_t *parent, const char *sym, intptr_t d)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, s_row_h - 12, s_row_h - 12);
    lv_obj_set_style_radius(b, s_radius / 2 + 2, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x3a3358), 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(COLOR_CARD_PRESSED), LV_STATE_PRESSED);
    lv_obj_t *l = label(b, s_font_label, COLOR_TEXT, sym);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, on_vib_strength, LV_EVENT_CLICKED, (void *)d);
    return b;
}

static void build_vib_page(lv_obj_t *tile)
{
    lv_obj_t *list;
    char buf[48];
    s_vib = page(tile, "Vibration", true, &list);
    vib_caps(&s_vib_caps);

    /* The status line, pinned under the title so a tap anywhere down the
     * list shows its result: the last result or refusal, the minute's on
     * time against its 20 s budget, patterns this boot. The list starts
     * below it. */
    lv_obj_t *band = lv_obj_create(s_vib);
    lv_obj_remove_style_all(band);
    lv_obj_set_size(band, lv_obj_get_style_width(list, 0), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(band, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(band, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(band, LV_OBJ_FLAG_SCROLLABLE);
    int32_t list_top = lv_obj_get_style_y(list, 0);
    lv_obj_align(band, LV_ALIGN_TOP_MID, 0, list_top);
    s_vib_status = label(band, s_font_value, COLOR_TEXT, "Ready");
    s_vib_budget = label(band, &lv_font_montserrat_16, COLOR_DIM, "");
    lv_obj_t *const lines[] = { s_vib_status, s_vib_budget };
    for (int i = 0; i < 2; i++) {
        lv_obj_set_width(lines[i], lv_pct(100));
        lv_label_set_long_mode(lines[i], LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_style_text_align(lines[i], LV_TEXT_ALIGN_CENTER, 0);
    }
    /* Room for two lines of each (a result and the budget may wrap). */
    int32_t band_h = 2 * lv_font_get_line_height(s_font_value) + 2 * lv_font_get_line_height(&lv_font_montserrat_16);
    lv_obj_set_height(band, band_h);
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, list_top + band_h + s_gap);
    lv_obj_set_height(list, muse_board->height - (list_top + band_h + s_gap));
    s_vib_step = mght_strength_step(&s_vib_caps, s_vib_caps.default_strength ? s_vib_caps.default_strength : 100);

    /* Header (read-only): the actuator and its shortest felt pulse. */
    lv_obj_t *h = card(list, false);
    lv_obj_set_height(h, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(h, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(h, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_ver(h, s_pad / 2 + 4, 0);
    lv_obj_set_style_pad_row(h, 2, 0);
    mght_actuator_text(&s_vib_caps, buf, sizeof(buf));
    label(h, s_font_label, COLOR_TEXT, buf);
    snprintf(buf, sizeof(buf), "Shortest pulse %u ms", (unsigned)s_vib_caps.min_on_ms);
    label(h, s_font_value, COLOR_DIM, buf);

    note(list, "Pulses");
    for (int i = 0; i < MGHT_PULSE_COUNT; i++) {
        lv_obj_t *v;
        snprintf(buf, sizeof(buf), "%u ms", (unsigned)VIB_PULSES[i]);
        s_vib_pulse[i] = row(list, NULL, buf, &v, on_vib_pulse, (void *)(intptr_t)i);
        if (VIB_PULSES[i] < s_vib_caps.min_on_ms) {
            /* Shorter than the actuator renders: drawn dim, played lengthened. */
            lv_obj_set_style_text_color(lv_obj_get_child(s_vib_pulse[i], 0), lv_color_hex(COLOR_DIM), 0);
            lv_obj_set_style_max_width(v, s_list_w * 2 / 3, 0);
            snprintf(buf, sizeof(buf), "lengthened to %u ms", (unsigned)mght_pulse_played_ms(&s_vib_caps, VIB_PULSES[i]));
            set_text(v, buf);
        }
    }

    note(list, "Presets");
    for (int p = MGHT_PRESET_FIRST; p <= MGHT_PRESET_LAST; p++) {
        lv_obj_t *v;
        s_vib_preset[p] = row(list, NULL, mght_preset_name((uint8_t)p, true), &v, on_vib_preset, (void *)(intptr_t)p);
        uint32_t ms = mght_preset_ms(&s_vib_caps, (uint8_t)p);
        if (p == MGHT_PRESET_RING) {
            s_vib_ring_val = v;
            snprintf(buf, sizeof(buf), "up to %u s", (unsigned)(ms / 1000));
        } else {
            snprintf(buf, sizeof(buf), "%u ms", (unsigned)ms);
        }
        set_text(v, buf);
    }

    s_vib_str_val = s_vib_minus = s_vib_plus = NULL;
    if (s_vib_caps.flags & MGHT_FLAG_STRENGTH) {
        note(list, "Strength (this test's only; never saved)");
        lv_obj_t *c = card(list, false);
        lv_obj_set_flex_align(c, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        s_vib_minus = step_button(c, LV_SYMBOL_MINUS, -1);
        s_vib_str_val = label(c, s_font_label, COLOR_ACCENT, "");
        s_vib_plus = step_button(c, LV_SYMBOL_PLUS, 1);
        show_strength();
    }

    lv_obj_t *stop_lbl;
    s_vib_stop = button(list, "Stop", COLOR_DANGER, on_vib_stop, &stop_lbl);
    note(list, "Any key stops it too. Tests skip the Vibration switch, never the safety limits.");
}

static void tick_vib(void)
{
    if (!s_vib) {
        return;
    }
    mght_status_t st;
    mg_ble_haptic_test_status(&st);
    set_text(s_vib_status, st.line);
    char two[2 * MGHT_LINE_MAX];
    snprintf(two, sizeof(two), "%s\n%s", st.budget, st.boot);
    set_text(s_vib_budget, two);
    bool ringing = st.playing && st.preset == MGHT_PRESET_RING;
    lv_obj_t *ring = s_vib_preset[MGHT_PRESET_RING];
    /* Ringing: the card lit and outlined until the second tap, a key, Stop or 10 s. */
    if ((lv_obj_get_style_border_width(ring, 0) > 0) != ringing) {
        lv_obj_set_style_bg_color(ring, lv_color_hex(ringing ? COLOR_CARD_PRESSED : COLOR_CARD), 0);
        lv_obj_set_style_border_color(ring, lv_color_hex(COLOR_ACCENT), 0);
        lv_obj_set_style_border_width(ring, ringing ? 2 : 0, 0);
    }
    char buf[24];
    snprintf(buf, sizeof(buf), "up to %u s", (unsigned)(mght_preset_ms(&s_vib_caps, MGHT_PRESET_RING) / 1000));
    set_text(s_vib_ring_val, ringing ? "tap to stop" : buf);
}
#else
static void vib_leave(void)
{
}
#endif

/* A page is going: its named controls with it (named(), the bench). */
static void forget_controls(lv_obj_t *p)
{
    if (p == s_aboutp) {
        s_about_fw = s_about_note = NULL;
        s_fw_taps = 0;
    } else if (p == s_dev) {
        s_dev_sw = s_dev_tests = NULL;
    } else if (p == s_passp) {
        s_pass_on = s_pass_sw = s_pass_change = s_pass_lock = s_pass_lock_val = NULL;
    } else if (p == s_pass_lockp) {
        memset(s_pass_lock_rows, 0, sizeof(s_pass_lock_rows));
        memset(s_pass_lock_checks, 0, sizeof(s_pass_lock_checks));
    } else if (p == s_facep) {
        memset(s_face_rows, 0, sizeof(s_face_rows));
        memset(s_face_checks, 0, sizeof(s_face_checks));
        s_face_avatar_lbl = s_face_size = s_face_size_val = NULL;
    } else if (p == s_tests) {
        s_tests_vib = s_tests_img = s_tests_img_lbl = s_tests_img_note = NULL;
#if MUSE_VIB_TESTS
    } else if (p == s_vib) {
        s_vib_status = s_vib_budget = s_vib_str_val = s_vib_minus = s_vib_plus = s_vib_stop = s_vib_ring_val = NULL;
        memset(s_vib_pulse, 0, sizeof(s_vib_pulse));
        memset(s_vib_preset, 0, sizeof(s_vib_preset));
#endif
    }
}

/* ---------- Developer > Tests ---------- */

/* Ask Muse for an image: the request goes from the gadget's own session;
 * the line under the button follows the run (tick_tests). */
static void on_tests_img(lv_event_t *e)
{
    (void)e;
    char why[96];
    if (!muse_link_image_ask(why, sizeof(why)) && s_tests_img_note) {
        char line[128];
        snprintf(line, sizeof(line), "Can't ask: %s.", why);
        set_text(s_tests_img_note, line);
    }
}

#if CONFIG_MUSE_UI_THEME
static const char *appearance_name(void) {
    static const char *const names[]={"Automatic","Dark","Light"};
    return names[muse_theme_mode()];
}
static void on_appearance(lv_event_t *e) {
    (void)e;muse_theme_request_mode((muse_theme_mode_t)((muse_theme_mode()+1)%3));
}
static void tick_appearance(void) {
    set_text(s_appearance_mode,appearance_name());
    set_text(s_appearance_status,muse_theme_save_error()?"Could not save appearance":"");
}
static void build_appearance(lv_obj_t *tile) {
    lv_obj_t *list;s_appearance=page(tile,"APPEARANCE",true,&list);
    row(list,NULL,"Theme",&s_appearance_mode,on_appearance,NULL);
    note(list,"Automatic follows the content background when available.");
    s_appearance_status=note(list,"");tick_appearance();
}
static const page_t APPEARANCE={&s_appearance,build_appearance,NULL};
#endif
#if CONFIG_MUSE_AVATAR_SYNC
static void on_avatar_source(lv_event_t *e) {
    (void)e;
    muse_avatar_select(muse_avatar_source() == MUSE_AVATAR_PERSONAL ? MUSE_AVATAR_BUILTIN : MUSE_AVATAR_PERSONAL);
}
static void on_avatar_refresh(lv_event_t *e) { (void)e; muse_avatar_refresh(); }
static void on_avatar_clear(lv_event_t *e) { (void)e; muse_avatar_clear(); }
static void tick_avatar(void) {
    set_text(s_avatar_source, muse_avatar_source() == MUSE_AVATAR_PERSONAL ? "My Muse" : "Built-in");
    set_text(s_avatar_status, muse_avatar_status());
    if (muse_avatar_source() == MUSE_AVATAR_PERSONAL) lv_obj_remove_state(s_avatar_refresh, LV_STATE_DISABLED);
    else lv_obj_add_state(s_avatar_refresh, LV_STATE_DISABLED);
}
static void build_avatar_page(lv_obj_t *tile) {
    lv_obj_t *list;
    s_avatar = page(tile, "AVATAR", true, &list);
    row(list, NULL, "Source", &s_avatar_source, on_avatar_source, NULL);
    s_avatar_status = info_row(list, "Status");
    s_avatar_refresh = row(list, LV_SYMBOL_REFRESH, "Refresh now", NULL, on_avatar_refresh, NULL);
    row(list, LV_SYMBOL_TRASH, "Clear saved avatar", NULL, on_avatar_clear, NULL);
    tick_avatar();
}
static const page_t AVATAR = { &s_avatar, build_avatar_page, NULL };
#endif

static void tick_tests(void)
{
    if (!s_tests_img_note) {
        return;
    }
    char line[128] = "";
    bool running = muse_link_image_status(line, sizeof(line));
    if (line[0]) {
        set_text(s_tests_img_note, line);
    }
    if (running) {
        /* A run under way (Muse making the picture, then showing it): the
         * page stays open past the menu's 30 s idle close, so its
         * line, and the result after, are there to read. */
        lv_display_trigger_activity(NULL);
    }
}

static void build_tests_page(lv_obj_t *tile)
{
    lv_obj_t *list;
    s_tests = page(tile, "Tests", true, &list);
    s_tests_vib = s_tests_img = s_tests_img_lbl = s_tests_img_note = NULL;
    bool any = false;
    if (muse_link_image_test()) {
        s_tests_img = button(list, "Ask Muse for an image", COLOR_ACCENT, on_tests_img, &s_tests_img_lbl);
        s_tests_img_note = note(list, "");
        tick_tests();
        any = true;
    }
#if MUSE_VIB_TESTS
    mght_caps_t c;
    if (vib_caps(&c)) {
        lv_obj_t *v;
        char buf[24];
        s_tests_vib = row(list, NULL, "Vibration", &v, on_nav, (void *)&VIB);
        mght_actuator_text(&c, buf, sizeof(buf));
        set_text(v, buf);
        note(list, "Test pulses and presets through the haptics engine, with its safety limits.");
        any = true;
    }
#endif
    if (!any) {
        note(list, "No tests on this board.");
    }
}

static void on_about_fw(lv_event_t *e)
{
    (void)e;
    int64_t now = esp_timer_get_time();
    s_fw_taps = s_fw_tap_us && now - s_fw_tap_us < 1500000 ? s_fw_taps + 1 : 1;
    s_fw_tap_us = now;
    if (muse_settings_developer()) {
        set_text(s_about_note, about_hint());
        return;
    }
    if (s_fw_taps >= DEV_TAPS) {
        muse_settings_set_developer(true);
        sync_dev_row();
        s_fw_taps = 0;
        set_text(s_about_note, "Developer mode on");
        return;
    }
    char buf[48];
    int left = DEV_TAPS - s_fw_taps;
    snprintf(buf, sizeof(buf), "%d more tap%s for developer mode", left, left == 1 ? "" : "s");
    set_text(s_about_note, buf);
}

/* ---------- Home: the v1.0 menu ---------- */

static const page_t WIFI = { &s_wifi, build_wifi_page, NULL };
static const page_t HATCH = { &s_hatch, build_hatch_page, NULL };
static const page_t BLE = { &s_ble, build_ble_page, NULL };
static const page_t SOUND = { &s_sound, build_sound_page, NULL };
static const page_t SLEEP = { &s_sleep, build_sleep_page, NULL };
static const page_t FACE = { &s_facep, build_face_page, NULL };
static const page_t BATTERY = { &s_battery, build_battery_page, NULL };
static const page_t TALK = { &s_talk, build_talk_page, NULL };
static const page_t ABOUT = { &s_aboutp, build_about_page, NULL };

#if CONFIG_MUSE_VIDEO
/* Phone camera: the video view, which asks the phone for a stream. */
static void on_camera(lv_event_t *e)
{
    (void)e;
    muse_video_open();
}
#endif

#if CONFIG_MUSE_APP_VIEW
/* Apps: the launcher of the apps installed on the gadget (muse_app_view.h). */
static void on_apps(lv_event_t *e)
{
    (void)e;
    muse_app_view_home();
}

static void on_apps_sw(lv_event_t *e)
{
    muse_settings_set_apps_on(lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED));   /* restarts */
}
#endif

/* The rows in this order, each left out when the board lacks it:
 * Sound, Display, Talk, Battery, Bluetooth, Wi-Fi, Muse, About,
 * Power off, Developer. Sound is left out with no speaker and no mic
 * (screen_only(): the brightness is on Display); with CONFIG_MUSE_APP_VIEW,
 * Run apps and Apps follow Muse. */
static void build_home(lv_obj_t *tile)
{
    lv_obj_t *list;
    s_home = page(tile, "Settings", false, &list);
    s_home_sound = NULL;
    if (!screen_only()) {
        icon_row(list, MUSE_ICON_VOLUME_UP, "Sound", &s_home_sound, on_nav, (void *)&SOUND);
    }
    /* The face picker's own row, before Display (never under it),
     * with the Display icon (the Watch face row); only where the gadget
     * has more than one face (shown by tick_home once the avatar face is).
     * No value: a face's name doesn't fit beside the label on a grown list. */
    s_home_face = icon_row(list, MUSE_ICON_DISPLAY, "Watch face", NULL, on_nav, (void *)&FACE);
    lv_obj_set_flag(s_home_face, LV_OBJ_FLAG_HIDDEN, !muse_link_face_available());
    icon_row(list, MUSE_ICON_DISPLAY, "Display", &s_home_sleep, on_nav, (void *)&SLEEP);
#if CONFIG_MUSE_UI_THEME
    row(list, LV_SYMBOL_IMAGE, "Appearance", &s_home_appearance, on_nav, (void *)&APPEARANCE);
#endif
#if CONFIG_MUSE_AVATAR_SYNC
    row(list, LV_SYMBOL_IMAGE, "Avatar", &s_home_avatar, on_nav, (void *)&AVATAR);
#endif
    s_home_pass = NULL;
    if (muse_passcode_supported()) {
        /* After Display, On / Off. */
        s_home_pass = icon_row(list, MUSE_ICON_LOCK, "Passcode", &s_home_pass_val, on_nav, (void *)&PASS);
    }
    if (muse_input_talk_choice()) {
        icon_row(list, MUSE_ICON_MIC, "Talk", &s_home_talk, on_nav, (void *)&TALK);
    }
    icon_row(list, MUSE_ICON_BATTERY, "Battery", &s_home_battery, on_nav, (void *)&BATTERY);
    icon_row(list, MUSE_ICON_BLUETOOTH, "Bluetooth", &s_home_ble, on_nav, (void *)&BLE);
#if !CONFIG_MUSE_GADGET_BLE_STANDALONE
    if (muse_home_wifi_build()) {
        icon_row(list, MUSE_ICON_WIFI, "Wi-Fi", &s_home_wifi, on_nav, (void *)&WIFI);
    }
#endif
    icon_row(list, MUSE_ICON_HOME, "Muse", &s_home_hatch, on_nav, (void *)&HATCH);
#if CONFIG_MUSE_APP_VIEW
    switch_row(list, "Run apps", muse_settings_apps_on(), on_apps_sw);
    if (muse_settings_apps_on()) {
        row(list, LV_SYMBOL_LIST, "Apps", NULL, on_apps, NULL);
    }
#endif
#if CONFIG_MUSE_VIDEO
    if (muse_video_supported()) {
        icon_row(list, MUSE_ICON_VIDEOCAM, "Phone camera", &s_home_camera, on_camera, NULL);
    }
#endif
    icon_row(list, MUSE_ICON_INFO, "About", &s_home_about, on_nav, (void *)&ABOUT);
    if (muse_board->power_off) {
        s_power_armed_us = 0;
        icon_row(list, MUSE_ICON_POWER, "Power off", &s_home_power, on_power_row, NULL);
        s_restart_armed_us = 0;
        row(list, LV_SYMBOL_REFRESH, "Restart", &s_home_restart, on_restart_row, NULL);
    }
    /* Developer: only while Developer mode is on. */
    lv_obj_t *dev = icon_row(list, MUSE_ICON_DEVELOPER, "Developer", NULL, on_nav, (void *)&DEV);
    s_home_dev = dev;
    lv_obj_set_flag(dev, LV_OBJ_FLAG_HIDDEN, !muse_settings_developer());
    s_about = note(list, "");
}

static void tick_home(void)
{
#if CONFIG_MUSE_UI_THEME
    set_text(s_home_appearance,appearance_name());
#endif
#if CONFIG_MUSE_AVATAR_SYNC
    set_text(s_home_avatar, muse_avatar_source() == MUSE_AVATAR_PERSONAL ? "My Muse" : "Built-in");
#endif
    sync_dev_row();
    muse_wifi_status_t w;
    muse_wifi_status(&w);
    static const char *const WIFI_VALUES[] = { "Off", "Not set", "Joining", "", "Failed", "Not nearby" };
    if (s_home_wifi) {
        set_text(s_home_wifi, w.state == MUSE_WIFI_CONNECTED ? w.ssid : WIFI_VALUES[w.state]);
    }

    muse_hatch_status_t h;
    muse_hatch_status(&h);
    set_text(s_home_hatch, muse_hatch_state_name(h.state));

    muse_ble_status_t b;
    muse_ble_status(&b);
    set_text(s_home_ble, b.state == MUSE_BLE_OFF ? "Off"
                         : (b.state == MUSE_BLE_CONNECTED && !s_bt_onoff ? "Connected" : "On"));

    if (!s_home_sound) {
        /* screen_only(): no Sound row */
    } else if (!muse_board->audio_init) {
        set_text(s_home_sound, muse_board->mic_init ? "Mic only" : "No audio");
    } else if (muse_settings_speaker_on()) {
        set_val(s_home_sound, "%d%%", muse_settings_volume());
    } else {
        set_text(s_home_sound, "Muted");
    }
    set_text(s_home_sleep, sleep_name(muse_settings_sleep_s()));
    bool faces = muse_link_face_available();
    if (faces == lv_obj_has_flag(s_home_face, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_set_flag(s_home_face, LV_OBJ_FLAG_HIDDEN, !faces);
    }
    if (s_home_pass_val) {
        set_text(s_home_pass_val, muse_passcode_on() ? "On" : "Off");
    }
    if (s_home_talk) {
        static const char *const SHORT[] = { "Both", "Screen", "Button" };
        set_text(s_home_talk, SHORT[muse_settings_talk_mode() % 3]);
    }

    muse_power_t p = muse_state_power();
    char buf[96];
    if (p.battery_pct < 0) {
        strlcpy(buf, "USB", sizeof(buf));
    } else {
        snprintf(buf, sizeof(buf), "%s%d%%", p.charging ? LV_SYMBOL_CHARGE " " : "", p.battery_pct);
    }
    set_text(s_home_battery, buf);
    if (s_home_power && s_power_armed_us && esp_timer_get_time() - s_power_armed_us >= 3000000) {
        s_power_armed_us = 0;
    }
    if (s_home_power) {
        set_text(s_home_power, s_power_armed_us ? "Tap again" : "");
    }
    if (s_restart_armed_us && esp_timer_get_time() - s_restart_armed_us >= 3000000) {
        s_restart_armed_us = 0;
    }
    if (s_home_restart) {
        set_text(s_home_restart, s_restart_armed_us ? "Tap again" : "");
    }
#if CONFIG_MUSE_VIDEO
    if (s_home_camera) {
        set_text(s_home_camera, muse_video_has_client() ? "Ready" : "No phone");
    }
#endif
    set_text(s_home_about, esp_app_get_description()->version);
    if (s_home_wifi && w.state == MUSE_WIFI_CONNECTED) {
        set_text(s_about, w.ip);
    } else {
        set_text(s_about, "");
    }
}

/* ---------- public ---------- */

void muse_settings_ui_build(lv_obj_t *tile)
{
    scale_geometry();
    if (muse_board->round && muse_board->height < 466) {
        s_text_scale = muse_board->height;
    }
    s_tile = tile;
    build_home(tile);
    show(s_home);
}

#if LV_USE_SNAPSHOT
/* Bench screenshots (the console's ui.settings=NAME, muse_input.c): set from
 * the console's task, opened by the LVGL task at the next tick. The names of
 * muse_settings_ui_open(), "home", and "ble" and "sleep" for Bluetooth and
 * Display. */
static const char *const BENCH_PAGES[][2] = {
    { "home", NULL },          { "wifi", "wifi" },         { "muse", "muse" },   { "ble", "bluetooth" },
    { "bluetooth", "bluetooth" }, { "sound", "sound" },    { "sleep", "display" }, { "display", "display" },
    { "battery", "battery" },  { "talk", "talk" },         { "about", "about" }, { "developer", "developer" },
    { "passcode", "passcode" }, { "watchface", "watchface" }, { "tests", "tests" }, { "vibration", "vibration" },
#if CONFIG_MUSE_UI_THEME
    { "appearance", "appearance" },
#endif
#if CONFIG_MUSE_AVATAR_SYNC
    { "avatar", "avatar" },
#endif
};
static volatile int s_bench_open = -1;

bool muse_settings_ui_bench_open(const char *name)
{
    for (int i = 0; i < (int)(sizeof(BENCH_PAGES) / sizeof(BENCH_PAGES[0])); i++) {
        if (!strcmp(name, BENCH_PAGES[i][0])) {
            s_bench_open = i;
            return true;
        }
    }
    return false;
}

static void bench_open_pending(void)
{
    int i = s_bench_open;
    if (i < 0) {
        return;
    }
    s_bench_open = -1;
    muse_settings_ui_go_home();   /* closes the text page and drops the open sub-page */
    if (BENCH_PAGES[i][1]) {
        muse_settings_ui_open(BENCH_PAGES[i][1]);   /* one the board lacks stays on Settings */
    }
}

#if CONFIG_MUSE_AVATAR_SYNC && CONFIG_MUSE_UI_THEME
static atomic_uint s_bench_action;
bool muse_settings_ui_bench_action(const char *action)
{
    unsigned wanted = !strcmp(action,"primary") ? 1 : !strcmp(action,"back") ? 2 : 0;
    unsigned empty = 0;
    return wanted && atomic_compare_exchange_strong(&s_bench_action,&empty,wanted);
}
static void bench_action_pending(void)
{
    unsigned action = atomic_exchange(&s_bench_action,0);
    if (!action) return;
    lv_obj_t *target = NULL;
    if (s_current == s_appearance || s_current == s_avatar) {
        if (action == 2) target = lv_obj_get_child(s_current,1);
        else target = lv_obj_get_parent(s_current == s_appearance ? s_appearance_mode : s_avatar_source);
    }
    if (target) lv_obj_send_event(target,LV_EVENT_CLICKED,NULL);
    printf("@ui_settings_action {\"action\":\"%s\",\"ok\":%s,\"page\":\"%s\"}\n",
           action == 1 ? "primary" : "back",target ? "true" : "false",
           s_current == s_appearance ? "appearance" : s_current == s_avatar ? "avatar" : s_current == s_home ? "home" : "other");
    fflush(stdout);
}
#endif
#endif

void muse_settings_ui_tick(bool visible)
{
    static bool was_visible;
#if LV_USE_SNAPSHOT
    if (visible) {
        bench_open_pending();
#if CONFIG_MUSE_AVATAR_SYNC && CONFIG_MUSE_UI_THEME
        bench_action_pending();
#endif
    }
#endif
    if (visible != was_visible) {
        was_visible = visible;
        /* Only listen to the mic while the Sound page is actually on screen. */
        muse_voice_set_monitor(visible && s_current == s_sound && !muse_board->epaper && !screen_only());
    }
    if (!visible) {
        return;
    }
    if (s_current == s_home) {
        tick_home();
#if CONFIG_MUSE_UI_THEME
    } else if (s_current == s_appearance) {
        tick_appearance();
#endif
#if CONFIG_MUSE_AVATAR_SYNC
    } else if (s_current == s_avatar) {
        tick_avatar();
#endif
    } else if (s_current == s_proxy) {
        tick_proxy();
    } else if (s_current == s_wifi) {
        tick_wifi();
    } else if (s_current == s_hatch) {
        tick_hatch();
    } else if (s_current == s_ble) {
        tick_ble();
    } else if (s_current == s_sound) {
        tick_sound();
    } else if (s_current == s_sleep) {
        tick_sleep();
    } else if (s_current == s_facep) {
        tick_face();
    } else if (s_current == s_battery) {
        tick_battery();
    } else if (s_current == s_talk) {
        tick_talk();
    } else if (s_current == s_aboutp) {
        tick_about();
    } else if (s_current == s_dev) {
        tick_dev();
    } else if (s_current == s_tests) {
        tick_tests();
    } else if (s_current == s_passp) {
        tick_pass();
    } else if (s_current == s_pass_lockp) {
        tick_pass_lock();
#if MUSE_VIB_TESTS
    } else if (s_current == s_vib) {
        tick_vib();
#endif
    }
}

void muse_settings_ui_open(const char *name)
{
    const struct {
        const char *name;
        const page_t *page;
    } pages[] = {
        { "sound", &SOUND }, { "display", &SLEEP }, { "talk", &TALK }, { "battery", &BATTERY },
        { "bluetooth", &BLE }, { "wifi", &WIFI }, { "muse", &HATCH }, { "about", &ABOUT }, { "developer", &DEV },
        { "passcode", &PASS }, { "watchface", &FACE },
#if CONFIG_MUSE_UI_THEME
        { "appearance", &APPEARANCE },
#endif
#if CONFIG_MUSE_AVATAR_SYNC
        { "avatar", &AVATAR },
#endif
    };
    for (size_t i = 0; i < sizeof(pages) / sizeof(pages[0]); i++) {
        if (!strcmp(name, pages[i].name)) {
            if (pages[i].page == &PASS && !muse_passcode_supported()) return;
            if (pages[i].page == &SOUND && screen_only()) return;
            if (pages[i].page == &FACE && !muse_link_face_available()) return;
            open_page(pages[i].page);
            return;
        }
    }
    if (!strcmp(name, "passcode_lock") && muse_passcode_supported() && muse_passcode_on()) {
        open_page(&PASS);
        open_page(&PASS_LOCK);   /* back goes to Passcode */
        return;
    }
    /* Developer > Tests (> Vibration, only with an actuator), their pages
     * under them built so back goes up the way it would. */
    if (!strcmp(name, "tests") || (!strcmp(name, "vibration") && vib_caps(NULL))) {
        open_page(&DEV);
        open_page(&TESTS);
#if MUSE_VIB_TESTS
        if (name[0] == 'v') {
            open_page(&VIB);
        }
#endif
    }
}

void muse_settings_ui_go_home(void)
{
    if (s_current == s_text) {
        close_text();
    }
    if (s_home && s_current != s_home) {
        show(s_home);
    }
}

/* Bench (the console's tap=set.*), on the Sound page: "speaker" (its
 * switch) and "volume<N>" (its slider dragged to N), scrolled into view
 * first. LVGL task. */
/* The named controls of About, Settings, Developer, Tests and Vibration
 * (the bench's tap=set.<name>, the simulator's press=<name>). */
static lv_obj_t *named(const char *what)
{
    if (!strcmp(what, "back")) return s_current ? lv_obj_get_user_data(s_current) : NULL;   /* the title's chevron */
    if (!strcmp(what, "about.fw")) return s_about_fw ? lv_obj_get_parent(s_about_fw) : NULL;
    if (!strcmp(what, "developer")) return s_home_dev;
    if (!strcmp(what, "dev.switch")) return s_dev_sw;
    if (!strcmp(what, "dev.tests")) return s_dev_tests;
    if (!strcmp(what, "tests.vibration")) return s_tests_vib;
    if (!strcmp(what, "tests.image")) return s_tests_img;
    /* Settings > Passcode: the row, Turn on, the switch, Change, Lock and its two choices. */
    if (!strcmp(what, "passcode")) return s_home_pass;
    /* Settings > Watch face: the row, Pixel Muse, the avatar face and its Size. */
    if (!strcmp(what, "watchface")) return s_home_face;
    if (!strcmp(what, "face.pixel")) return s_facep ? s_face_rows[0] : NULL;
    if (!strcmp(what, "face.avatar")) return s_facep ? s_face_rows[1] : NULL;
    if (!strcmp(what, "face.size")) return s_facep ? s_face_size : NULL;
    if (!strcmp(what, "pass.on")) return s_pass_on;
    if (!strcmp(what, "pass.switch")) return s_pass_sw;
    if (!strcmp(what, "pass.change")) return s_pass_change;
    if (!strcmp(what, "pass.lock")) return s_pass_lock;
    if (!strcmp(what, "pass.lock0")) return s_pass_lockp ? s_pass_lock_rows[0] : NULL;
    if (!strcmp(what, "pass.lock1")) return s_pass_lockp ? s_pass_lock_rows[1] : NULL;
#if MUSE_VIB_TESTS
    if (!strncmp(what, "vib.", 4)) {
        const char *v = what + 4;
        if (!strcmp(v, "stop")) return s_vib_stop;
        if (!strcmp(v, "minus")) return s_vib_minus;
        if (!strcmp(v, "plus")) return s_vib_plus;
        if (!strncmp(v, "pulse", 5)) {
            int ms = atoi(v + 5);
            for (int i = 0; i < MGHT_PULSE_COUNT; i++) {
                if (VIB_PULSES[i] == ms) return s_vib_pulse[i];
            }
            return NULL;
        }
        for (int p = MGHT_PRESET_FIRST; p <= MGHT_PRESET_LAST; p++) {
            if (!strcmp(v, mght_preset_name((uint8_t)p, false))) return s_vib_preset[p];
        }
    }
#endif
    return NULL;
}

lv_obj_t *muse_settings_ui_find(const char *what)
{
    lv_obj_t *obj = named(what);
    if (!obj || !s_current || lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) {
        return NULL;
    }
    lv_obj_t *p = obj;
    while (p && p != s_current) {
        p = lv_obj_get_parent(p);
    }
    if (!p) {
        return NULL;   /* not on the page on show */
    }
    lv_obj_scroll_to_view_recursive(obj, LV_ANIM_OFF);
    lv_obj_update_layout(obj);
    return obj;
}

bool muse_settings_ui_bench_point(const char *what, muse_touch_gesture_t *g)
{
    lv_obj_t *found = muse_settings_ui_find(what);
    if (found) {
        return muse_touch_tap_obj(found, MUSE_TOUCH_TAP_MS, g);
    }
    lv_obj_t *obj = !strcmp(what, "speaker") ? s_spk_sw : !strncmp(what, "volume", 6) ? s_vol_sl : NULL;
    if (!obj || !s_sound || s_current != s_sound) {
        return false;
    }
    lv_obj_scroll_to_view_recursive(obj, LV_ANIM_OFF);
    lv_obj_update_layout(obj);
    if (obj == s_spk_sw) {
        return muse_touch_tap_obj(obj, MUSE_TOUCH_TAP_MS, g);
    }
    return muse_touch_slide_obj(obj, atoi(what + 6), g);
}

bool muse_settings_ui_in_subpage(void)
{
    return s_current != s_home;
}

const char *muse_settings_ui_page_name(void)
{
    const struct {
        lv_obj_t *page;
        const char *name;
    } names[] = {
        { s_home, "home" }, { s_wifi, "wifi" }, { s_hatch, "muse" }, { s_ble, "bluetooth" },
        { s_sound, "sound" }, { s_sleep, "sleep" }, { s_battery, "battery" }, { s_power, "power" },
        { s_text, "text" }, { s_talk, "talk" }, { s_aboutp, "about" }, { s_dev, "developer" },
        { s_tests, "tests" }, { s_vib, "vibration" }, { s_passp, "passcode" }, { s_pass_lockp, "passcode_lock" },
        { s_facep, "watchface" }, { s_proxy, "proxy" },
#if CONFIG_MUSE_UI_THEME
        { s_appearance, "appearance" },
#endif
#if CONFIG_MUSE_AVATAR_SYNC
        { s_avatar, "avatar" },
#endif
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (names[i].page && names[i].page == s_current) {
            return names[i].name;
        }
    }
    return "?";
}

void muse_settings_ui_geom(muse_settings_ui_geom_t *g)
{
    g->row_h = s_row_h;
    g->list_w = s_list_w;
    g->list_top = s_list_top;
    g->title_top = s_title_top;
    g->pad = s_pad;
    g->icon_px = s_icon_px;
    g->icon_gap = s_icon_gap;
    g->title = s_font_title;
    g->label = s_font_label;
    g->value = s_font_value;
}

lv_obj_t *muse_settings_ui_card(lv_obj_t *parent, muse_icon_t icon, uint32_t icon_color, const char *text,
                                uint32_t text_color, lv_obj_t **value_out, lv_event_cb_t cb, void *user)
{
    lv_obj_t *c = icon_row(parent, icon, text, value_out, cb, user);
    muse_icon_set(lv_obj_get_child(c, 0), icon, s_icon_px, icon_color);
    lv_obj_set_style_text_color(lv_obj_get_child(c, 1), lv_color_hex(text_color), 0);
    return c;
}

void muse_settings_ui_card_fit(lv_obj_t *c, int card_w, int trailing)
{
    lv_obj_t *img = lv_obj_get_child(c, 0), *t = lv_obj_get_child(c, 1);
    lv_point_t sz;
    lv_text_get_size(&sz, lv_label_get_text(t), s_font_label, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    int room = card_w - 2 * s_pad - trailing;
    lv_obj_set_flag(img, LV_OBJ_FLAG_HIDDEN, sz.x > room - s_icon_px - s_icon_gap);
    if (sz.x > room) {
        lv_obj_set_style_text_font(t, s_font_value, 0);   /* still too long: the smaller type */
    }
    lv_obj_set_height(t, lv_font_get_line_height(lv_obj_get_style_text_font(t, 0)));
    lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_DOTS);
}
