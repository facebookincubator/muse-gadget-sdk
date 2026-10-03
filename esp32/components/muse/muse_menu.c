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

#include "muse_menu.h"

#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "muse_battery.h"
#include "muse_ble.h"
#include "muse_board.h"
#include "muse_chat.h"
#include "muse_input.h"
#include "muse_link.h"
#include "muse_settings.h"
#include "muse_state.h"
#include "muse_text.h"
#include "muse_voice.h"
#include "muse_wifi.h"

static const char *TAG = "muse_menu";

#define COLOR_TEXT 0xf2efff
#define COLOR_DIM 0x8b84a8
#define COLOR_ACCENT 0xa77dff
#define COLOR_SELECTED 0x2e2552
#define COLOR_RULE 0x2a2345
#define COLOR_DANGER 0xff5c5c

/* 128 px screens want a smaller face than the 14 px every board has. */
#if LV_FONT_MONTSERRAT_12
#define FONT_COMPACT (&lv_font_montserrat_12)
#else
#define FONT_COMPACT (&lv_font_montserrat_14)
#endif

#define IDLE_CLOSE_S 30.0f      /* back to the face if left alone */
#define REFRESH_S 0.25f

/*
 * One list, stepped through with Down and acted on with Select. Settings
 * cycle through a few values on each Select (wrapping around), so nothing
 * needs a long press or a third button.
 */
typedef enum {
    ITEM_VOLUME,
    ITEM_SPEAKER,
    ITEM_BRIGHTNESS,
    ITEM_MIC,
    ITEM_SLEEP,
    ITEM_PHONE,
    ITEM_WIFI,
    ITEM_INFO,
    ITEM_BATTERY,
    ITEM_RESET,
    ITEM_SLEEP_NOW,
    ITEM_POWER,
    ITEM_CLOSE,
    ITEM_COUNT,
} item_t;

static const char *const ITEM_NAMES[ITEM_COUNT] = {
    [ITEM_VOLUME] = "Volume",
    [ITEM_SPEAKER] = "Speaker",
    [ITEM_BRIGHTNESS] = "Brightness",
    [ITEM_MIC] = "Mic gain",
    [ITEM_SLEEP] = "Auto-sleep",
    [ITEM_PHONE] = "Phone setup",
    [ITEM_WIFI] = "Wi-Fi",
    [ITEM_INFO] = "Status",
    [ITEM_BATTERY] = "Battery",
    [ITEM_RESET] = "Reset pairing",
    [ITEM_SLEEP_NOW] = "Screen off",
    [ITEM_POWER] = "Power off",
    [ITEM_CLOSE] = "Close menu",
};

/* What the talk button does on each row. */
static const char *const ITEM_ACTIONS[ITEM_COUNT] = {
    [ITEM_VOLUME] = "Change",
    [ITEM_SPEAKER] = "Toggle",
    [ITEM_BRIGHTNESS] = "Change",
    [ITEM_MIC] = "Change",
    [ITEM_SLEEP] = "Change",
    [ITEM_PHONE] = "Toggle",
    [ITEM_WIFI] = "Toggle",
    [ITEM_INFO] = "Open",
    [ITEM_BATTERY] = "Open",
    [ITEM_RESET] = "Select",
    [ITEM_SLEEP_NOW] = "Select",
    [ITEM_POWER] = "Select",
    [ITEM_CLOSE] = "Close",
};

/* Ascending; Select moves to the next one and wraps. */
static const int VOLUME_STEPS[] = { 10, 25, 40, 55, 70, 85, 100 };
static const int BRIGHT_STEPS[] = { 10, 25, 50, 75, 100 };
static const int GAIN_STEPS[] = { 0, 6, 12, 18, 24, 30, 36 };
static const int SLEEP_STEPS[] = { 0, 30, 60, 120, 300, 600 };
static const char *const SLEEP_NAMES[] = { "Never", "30 s", "1 min", "2 min", "5 min", "10 min" };
#define COUNT(a) ((int)(sizeof(a) / sizeof((a)[0])))

typedef enum {
    VIEW_CLOSED,
    VIEW_LIST,
    VIEW_STATUS,
    VIEW_BATTERY,
    VIEW_POWER,
    VIEW_RESET,
} view_t;

static QueueHandle_t s_keys;
static volatile bool s_open;
static view_t s_view;
static int s_sel;
static int s_shown_sel = -1;
static int s_first;             /* top visible row */
static int s_visible_rows;
static int s_row_h;
static float s_last_key;
static float s_next_refresh;
static int64_t s_batt_shown_us;   /* the battery page reads the PM stats, so once a second */

static lv_obj_t *s_root;
static lv_obj_t *s_title;
static lv_obj_t *s_list;
static lv_obj_t *s_rows[ITEM_COUNT];
static lv_obj_t *s_values[ITEM_COUNT];
static lv_obj_t *s_page;        /* status and power-off confirmation text */
static lv_obj_t *s_hint_down;
static lv_obj_t *s_hint_select;
static const char *s_down_text;

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, uint32_t color, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, text);
    return l;
}

static void set_text(lv_obj_t *l, const char *text)
{
    if (strcmp(lv_label_get_text(l), text) != 0) {
        lv_label_set_text(l, text);
    }
}

static void select_hint(const char *action)
{
    char text[40];
    snprintf(text, sizeof(text), "%s%s", muse_board->keyboard ? "Enter " : "", action);
    set_text(s_hint_select, text);
}

static int value_step(const int *steps, int n, int cur, int direction)
{
    if (direction < 0) {
        for (int i = n - 1; i >= 0; --i) if (steps[i] < cur) return steps[i];
        return steps[n - 1];
    }
    for (int i = 0; i < n; ++i) if (steps[i] > cur) return steps[i];
    return steps[0];
}

static const char *sleep_name(int secs)
{
    for (int i = 0; i < COUNT(SLEEP_STEPS); i++) {
        if (SLEEP_STEPS[i] == secs) {
            return SLEEP_NAMES[i];
        }
    }
    return "Custom";
}

static void value_text(int item, char *buf, size_t n)
{
    static const char *const WIFI_VALUES[] = { "Off", "Not set", "Joining", "On", "Failed", "Not found" };
    muse_wifi_status_t w;
    muse_power_t p;

    switch (item) {
    case ITEM_VOLUME:
        snprintf(buf, n, "%d%%", muse_settings_volume());
        break;
    case ITEM_SPEAKER:
        strlcpy(buf, muse_settings_speaker_on() ? "On" : "Off", n);
        break;
    case ITEM_BRIGHTNESS:
        snprintf(buf, n, "%d%%", muse_settings_brightness());
        break;
    case ITEM_MIC:
        snprintf(buf, n, "%d dB", muse_settings_mic_gain());
        break;
    case ITEM_SLEEP:
        strlcpy(buf, sleep_name(muse_settings_sleep_s()), n);
        break;
    case ITEM_PHONE:
        strlcpy(buf, muse_settings_ble_on() ? "On" : "Off", n);
        break;
    case ITEM_WIFI:
        muse_wifi_status(&w);
        strlcpy(buf, WIFI_VALUES[w.state], n);
        break;
    case ITEM_BATTERY:
        p = muse_state_power();
        if (p.usb || p.battery_pct < 0) {
            strlcpy(buf, "USB", n);
        } else {
            snprintf(buf, n, "%d%%", p.battery_pct);
        }
        break;
    default:
        buf[0] = '\0';
        break;
    }
}

static void status_text(char *buf, size_t n)
{
    muse_wifi_status_t w;
    muse_wifi_status(&w);
    muse_hatch_status_t h;
    muse_hatch_status(&h);
    muse_ble_status_t b;
    muse_ble_status(&b);
    muse_power_t p = muse_state_power();

    char batt[16] = "USB";
    if (p.battery_pct >= 0) {
        snprintf(batt, sizeof(batt), "%d%%%s", p.battery_pct, p.charging ? " +" : "");
    }
    const char *phone = b.state == MUSE_BLE_OFF ? "Off" : (b.state == MUSE_BLE_CONNECTED ? "Connected" : b.name);
    snprintf(buf, n, "Wi-Fi %s\nIP    %s\nLink  %s\nMuse  %s\nPhone %s\nPower %s\nVer   %s",
             w.state == MUSE_WIFI_CONNECTED ? w.ssid : (w.state == MUSE_WIFI_OFF ? "off" : "offline"),
             w.state == MUSE_WIFI_CONNECTED ? w.ip : "-", muse_link_state_name(muse_link_state()),
             muse_hatch_state_name(h.state), phone, batt,
             esp_app_get_description()->version);
    muse_text_to_ascii(buf, n);   /* network and phone names can have curly quotes */
}

static void pm_text(char out[24], int pm)
{
    if (pm < 0) {
        strlcpy(out, "-", 24);
    } else {
        snprintf(out, 24, "%d.%d%%", pm / 10, pm % 10);
    }
}

/* The battery meter (muse_battery.h), 15 columns wide. */
static void battery_text(char *buf, size_t n)
{
    muse_battery_t b;
    muse_battery_read(&b);
    if (!b.started) {
        strlcpy(buf, "Unplug USB to\nmeasure how\nlong the\nbattery lasts.", n);
        return;
    }
    char t[24], rate[24] = "-", full[24] = "-", wakes[24] = "-", off[24], slept[24], busy[24];
    int h = (int)(b.secs / 3600), m = (int)(b.secs / 60 % 60), rate10, full_h;
    if (h) {
        snprintf(t, sizeof(t), "%dh%02dm", h, m);
    } else {
        snprintf(t, sizeof(t), "%dm", m);
    }
    if (muse_battery_drain(&b, &rate10, &full_h)) {
        snprintf(rate, sizeof(rate), "%d.%d%%/h", rate10 / 10, rate10 % 10);
        snprintf(full, sizeof(full), "~%d h", full_h);
    }
    if (b.secs && b.slept_pm >= 0) {
        int per10 = (int)(b.sleeps * 10LL / b.secs);
        snprintf(wakes, sizeof(wakes), "%d.%d/s", per10 / 10, per10 % 10);
    }
    pm_text(off, b.screen_off_pm);
    pm_text(slept, b.slept_pm);
    pm_text(busy, b.busy_pm);
    snprintf(buf, n, "%s %s\nBatt  %d>%d%%\nRate  %s\nFull  %s\nOff   %s\nSleep %s\nWakes %s\nBusy  %s",
             b.running ? "On batt" : "Last run", t, b.pct_start, b.pct_now, rate, full, off, slept, wakes, busy);
}

static void refresh(void)
{
    char buf[160];
    if (s_view == VIEW_LIST) {
        for (int i = 0; i < ITEM_COUNT; i++) {
            value_text(i, buf, sizeof(buf));
            set_text(s_values[i], buf);
        }
        if (s_sel != s_shown_sel) {
            for (int i = 0; i < ITEM_COUNT; i++) {
                lv_obj_set_style_bg_opa(s_rows[i], i == s_sel ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
            }
            /* Scroll by whole rows so none is ever cut in half. */
            if (s_sel < s_first) {
                s_first = s_sel;
            } else if (s_sel >= s_first + s_visible_rows) {
                s_first = s_sel - s_visible_rows + 1;
            }
            lv_obj_scroll_to_y(s_list, s_first * s_row_h, LV_ANIM_OFF);
            select_hint(ITEM_ACTIONS[s_sel]);
            s_shown_sel = s_sel;
        }
    } else if (s_view == VIEW_STATUS) {
        status_text(buf, sizeof(buf));
        set_text(s_page, buf);
    } else if (s_view == VIEW_BATTERY) {
        int64_t now = esp_timer_get_time();
        if (!s_batt_shown_us || now - s_batt_shown_us >= 1000000) {
            s_batt_shown_us = now;
            battery_text(buf, sizeof(buf));
            set_text(s_page, buf);
        }
    }
}

static void show(view_t view)
{
    s_view = view;
    s_shown_sel = -1;
    s_batt_shown_us = 0;
    lv_obj_set_flag(s_list, LV_OBJ_FLAG_HIDDEN, view != VIEW_LIST);
    lv_obj_set_flag(s_page, LV_OBJ_FLAG_HIDDEN, view == VIEW_LIST);
    bool danger = view == VIEW_POWER || view == VIEW_RESET;
    lv_obj_set_style_text_color(s_hint_select, lv_color_hex(danger ? COLOR_DANGER : COLOR_TEXT), 0);

    switch (view) {
    case VIEW_STATUS:
    case VIEW_BATTERY:
        set_text(s_title, view == VIEW_STATUS ? "STATUS" : "BATTERY");
        set_text(s_hint_down, muse_board->keyboard ? "Esc Back" : "Back");
        select_hint("Back");
        break;
    case VIEW_POWER: {
        char text[96];
        snprintf(text, sizeof(text), "Turn Muse off?\n\nPress the %s button to turn it back on.",
                 muse_board->keyboard ? "GO" : muse_board->aux_button);
        set_text(s_title, "POWER OFF");
        set_text(s_page, text);
        set_text(s_hint_down, muse_board->keyboard ? "Esc Cancel" : "Cancel");
        select_hint("Power off");
        break;
    }
    case VIEW_RESET:
        set_text(s_title, "RESET PAIRING");
        set_text(s_page, "Forget Wi-Fi and the Muse app pairing, then restart?");
        set_text(s_hint_down, muse_board->keyboard ? "Esc Cancel" : "Cancel");
        select_hint("Reset");
        break;
    default:
        set_text(s_title, muse_board->keyboard ? "MENU  ^v Move  <> Change" : "MENU");
        set_text(s_hint_down, s_down_text);
        break;
    }
    refresh();
}

static void open_menu(void)
{
    ESP_LOGI(TAG, "open");
    s_sel = s_first = 0;
    lv_obj_move_foreground(s_root);
    lv_obj_remove_flag(s_root, LV_OBJ_FLAG_HIDDEN);
    show(VIEW_LIST);
    s_open = true;
}

static void activate(int item, int direction)
{
    ESP_LOGI(TAG, "select %s", ITEM_NAMES[item]);
    switch (item) {
    case ITEM_VOLUME:
        muse_settings_set_volume(value_step(VOLUME_STEPS, COUNT(VOLUME_STEPS), muse_settings_volume(), direction));
        muse_voice_request_chirp();
        break;
    case ITEM_SPEAKER:
        muse_settings_set_speaker_on(!muse_settings_speaker_on());
        break;
    case ITEM_BRIGHTNESS:
        muse_settings_set_brightness(value_step(BRIGHT_STEPS, COUNT(BRIGHT_STEPS), muse_settings_brightness(), direction));
        break;
    case ITEM_MIC:
        muse_settings_set_mic_gain(value_step(GAIN_STEPS, COUNT(GAIN_STEPS), muse_settings_mic_gain(), direction));
        break;
    case ITEM_SLEEP:
        muse_settings_set_sleep_s(value_step(SLEEP_STEPS, COUNT(SLEEP_STEPS), muse_settings_sleep_s(), direction));
        break;
    case ITEM_PHONE:
        muse_settings_set_ble_on(!muse_settings_ble_on());
        break;
    case ITEM_WIFI:
        muse_settings_set_wifi_on(!muse_settings_wifi_on());
        break;
    case ITEM_INFO:
        show(VIEW_STATUS);
        return;
    case ITEM_BATTERY:
        show(VIEW_BATTERY);
        return;
    case ITEM_SLEEP_NOW:
        muse_menu_close();
        muse_state_set_asleep(true);
        return;
    case ITEM_RESET:
        show(VIEW_RESET);
        return;
    case ITEM_POWER:
        show(VIEW_POWER);
        return;
    case ITEM_CLOSE:
    default:
        muse_menu_close();
        return;
    }
    refresh();
}

/* Only Enter/Select may activate a confirmation. Arrow keys never confirm
 * reset/power-off, and left/right only adjust actual settings in the list. */
static void handle(muse_menu_key_t key)
{
    switch (s_view) {
    case VIEW_CLOSED:
        if (key == MUSE_MENU_DOWN || key == MUSE_MENU_UP ||
            key == MUSE_MENU_BACK || key == MUSE_MENU_SELECT) open_menu();
        break;
    case VIEW_LIST:
        if (key == MUSE_MENU_BACK) {
            muse_menu_close();
        } else if (key == MUSE_MENU_DOWN || key == MUSE_MENU_UP) {
            s_sel = (s_sel + (key == MUSE_MENU_UP ? ITEM_COUNT - 1 : 1)) % ITEM_COUNT;
            refresh();
        } else if (key == MUSE_MENU_SELECT) {
            activate(s_sel, 1);
        } else if ((key == MUSE_MENU_LEFT || key == MUSE_MENU_RIGHT) && s_sel <= ITEM_WIFI) {
            activate(s_sel, key == MUSE_MENU_LEFT ? -1 : 1);
        }
        break;
    case VIEW_STATUS:
    case VIEW_BATTERY:
        if (key == MUSE_MENU_BACK || key == MUSE_MENU_LEFT || key == MUSE_MENU_SELECT ||
            key == MUSE_MENU_DOWN || key == MUSE_MENU_UP) show(VIEW_LIST);
        break;
    case VIEW_POWER:
    case VIEW_RESET:
        if (key == MUSE_MENU_BACK || key == MUSE_MENU_LEFT ||
            key == MUSE_MENU_DOWN || key == MUSE_MENU_UP) {
            show(VIEW_LIST);
        } else if (key == MUSE_MENU_SELECT) {
            bool power = s_view == VIEW_POWER;
            muse_menu_close();
            if (power) muse_input_request_power_off();
            else {
                muse_state_set_caption("RESETTING...");
                muse_link_reset_setup();
            }
        }
        break;
    }
}

void muse_menu_key(muse_menu_key_t key)
{
    if (s_keys) {
        uint8_t k = (uint8_t)key;
        xQueueSend(s_keys, &k, 0);
    }
}

bool muse_menu_is_open(void)
{
    return s_open;
}

/* On the bottom bar, on the side of the button's face icon. */
static void align_on_bar(lv_obj_t *l, lv_align_t icon, int pad)
{
    switch (icon) {
    case LV_ALIGN_BOTTOM_LEFT:
    case LV_ALIGN_LEFT_MID:
        lv_obj_align(l, LV_ALIGN_BOTTOM_LEFT, 2 * pad, -pad);
        break;
    case LV_ALIGN_BOTTOM_MID:
        lv_obj_align(l, LV_ALIGN_BOTTOM_MID, 0, -pad);
        break;
    default:
        lv_obj_align(l, LV_ALIGN_BOTTOM_RIGHT, -2 * pad, -pad);
        break;
    }
}

void muse_menu_build(lv_obj_t *parent, int w, int h)
{
    bool small = h < 200 || w < 200;
    const lv_font_t *font = small ? FONT_COMPACT : &lv_font_montserrat_20;
    const lv_font_t *fine = small ? &lv_font_unscii_8 : &lv_font_unscii_16;
    int pad = small ? 2 : 8;
    int title_h = small ? 13 : 40;
    int hint_h = small ? 17 : 44;
    s_row_h = small ? 16 : 36;
    s_visible_rows = (h - title_h - hint_h) / s_row_h;
    /* A button on the right edge (StickS3) gets its hint turned on end in a
     * strip beside it, clear of the list, unless the board can't spare the
     * RAM to draw it turned. */
    const muse_button_hint_t *talk = &muse_board->talk_hint, *aux = &muse_board->aux_hint;
    bool aux_side = aux->align == LV_ALIGN_RIGHT_MID && !muse_board->flat_menu_hints;
    int strip = aux_side ? lv_font_get_line_height(font) + 2 : 0;

    s_keys = xQueueCreate(8, sizeof(uint8_t));

    s_root = lv_obj_create(parent);
    lv_obj_remove_style_all(s_root);
    lv_obj_set_size(s_root, w, h);
    lv_obj_set_style_bg_color(s_root, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_root, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_root, LV_OBJ_FLAG_HIDDEN);

    s_title = label(s_root, fine, COLOR_DIM, "MENU");
    lv_obj_set_style_text_letter_space(s_title, 1, 0);
    lv_obj_align(s_title, LV_ALIGN_TOP_MID, 0, small ? 3 : 12);

    s_list = lv_obj_create(s_root);
    lv_obj_remove_style_all(s_list);
    lv_obj_set_size(s_list, w - strip, s_visible_rows * s_row_h);
    lv_obj_set_pos(s_list, 0, title_h);
    lv_obj_set_style_pad_hor(s_list, pad, 0);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(s_list, LV_OBJ_FLAG_CLICKABLE);
    for (int i = 0; i < ITEM_COUNT; i++) {
        lv_obj_t *r = lv_obj_create(s_list);
        lv_obj_remove_style_all(r);
        lv_obj_set_size(r, lv_pct(100), s_row_h);
        lv_obj_set_style_pad_hor(r, small ? 3 : 10, 0);
        lv_obj_set_style_radius(r, small ? 3 : 8, 0);
        lv_obj_set_style_bg_color(r, lv_color_hex(COLOR_SELECTED), 0);
        lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        uint32_t color = i == ITEM_POWER ? COLOR_DANGER : COLOR_TEXT;
        lv_obj_align(label(r, font, color, ITEM_NAMES[i]), LV_ALIGN_LEFT_MID, 0, 0);
        s_values[i] = label(r, font, COLOR_ACCENT, "");
        lv_obj_align(s_values[i], LV_ALIGN_RIGHT_MID, 0, 0);
        s_rows[i] = r;
    }

    s_page = label(s_root, fine, COLOR_TEXT, "");
    lv_obj_set_width(s_page, w - 4 * pad - strip);
    lv_obj_set_style_text_line_space(s_page, small ? 3 : 8, 0);
    lv_label_set_long_mode(s_page, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_pos(s_page, 2 * pad, title_h + pad);
    lv_obj_add_flag(s_page, LV_OBJ_FLAG_HIDDEN);

    /* Hints sit next to their buttons, where the face shows their icons. */
    lv_obj_t *rule = lv_obj_create(s_root);
    lv_obj_remove_style_all(rule);
    lv_obj_set_size(rule, w, 1);
    lv_obj_set_style_bg_color(rule, lv_color_hex(COLOR_RULE), 0);
    lv_obj_set_style_bg_opa(rule, LV_OPA_COVER, 0);
    lv_obj_align(rule, LV_ALIGN_BOTTOM_MID, 0, -hint_h);
    s_hint_down = label(s_root, font, COLOR_TEXT, "");
    if (aux_side) {
        /* Reads downwards, the arrow pointing down; centred on the icon's
         * spot so it stays put as the text changes. */
        s_down_text = "Down " LV_SYMBOL_RIGHT;
        lv_obj_set_style_transform_rotation(s_hint_down, 900, 0);
        lv_obj_set_style_transform_pivot_x(s_hint_down, lv_pct(50), 0);
        lv_obj_set_style_transform_pivot_y(s_hint_down, lv_pct(50), 0);
        lv_obj_align(s_hint_down, LV_ALIGN_CENTER, (w - strip) / 2, aux->y);
    } else {
        s_down_text = muse_board->keyboard ? "Esc Back" : LV_SYMBOL_DOWN " Down";
        align_on_bar(s_hint_down, aux->align == LV_ALIGN_RIGHT_MID ? LV_ALIGN_BOTTOM_LEFT : aux->align, pad);
    }
    s_hint_select = label(s_root, font, COLOR_TEXT, "");
    align_on_bar(s_hint_select, talk->align, pad);
}

bool muse_menu_tick(float now)
{
    if (!s_root) {
        return false;
    }
    uint8_t k;
    while (xQueueReceive(s_keys, &k, 0) == pdTRUE) {
        s_last_key = now;
        handle((muse_menu_key_t)k);
    }
    if (s_view == VIEW_CLOSED) {
        return false;
    }
    if (now - s_last_key > IDLE_CLOSE_S) {
        muse_menu_close();
        return false;
    }
    if (now >= s_next_refresh) {
        s_next_refresh = now + REFRESH_S;
        refresh();
    }
    return true;
}

void muse_menu_close(void)
{
    if (!s_root || s_view == VIEW_CLOSED) {
        return;
    }
    s_view = VIEW_CLOSED;
    s_open = false;
    lv_obj_add_flag(s_root, LV_OBJ_FLAG_HIDDEN);
}
