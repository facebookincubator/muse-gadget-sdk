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

#include "muse_keypad.h"
#include "i18n.h"

#include <ctype.h>
#include <string.h>

#define COLOR_TEXT 0xf2efff
#define COLOR_CARD 0x1a1530
#define COLOR_CARD_PRESSED 0x2e2552
#define COLOR_ACCENT 0xa77dff

#define TAP_MS 1000    /* a second tap on the same key within this steps to its next character */
#define HOLD_MS 600    /* holding a letter key this long types its digit */

enum { LETTERS, DIGITS, SYMBOLS, MODE_COUNT };

/* Keys: twelve in four rows of three, then the bottom row. They're also the
 * button matrix's ids, but for a round screen's bottom row (key_button()). */
enum { KEY_SHIFT = 9, KEY_SPACE = 10, KEY_BACKSPACE = 11, KEY_MODE, KEY_SHOW, KEY_DONE, KEY_COUNT };

/* What each of the twelve keys types, in tap order. NULL for shift and backspace. */
static const char *const CHARS[MODE_COUNT][12] = {
    { ".-_@1", "abc2", "def3", "ghi4", "jkl5", "mno6", "pqrs7", "tuv8", "wxyz9", NULL, " 0", NULL },
    { "1", "2", "3", "4", "5", "6", "7", "8", "9", ".", "0", NULL },
    /* Every other printable ASCII character. */
    { ".,?!", "@#$%", "&*+=", "-_/\\", "()[]", "{}<>", "'\"`", ":;|", "^~", " ", "0", NULL },
};

static const char *const LABELS[MODE_COUNT][12] = {
    { ".-_@", "abc", "def", "ghi", "jkl", "mno", "pqrs", "tuv", "wxyz", LV_SYMBOL_UP, "space", LV_SYMBOL_BACKSPACE },
    { "1", "2", "3", "4", "5", "6", "7", "8", "9", ".", "0", LV_SYMBOL_BACKSPACE },
    { ".,?!", "@#$%", "&*+=", "-_/\\", "()[]", "{}<>", "'\"`", ":;|", "^~", "space", "0", LV_SYMBOL_BACKSPACE },
};

static const char *const UPPER_LABELS[9] = { ".-_@", "ABC", "DEF", "GHI", "JKL", "MNO", "PQRS", "TUV", "WXYZ" };

/* The mode key names the mode it switches to. */
static const char *const NEXT_MODE[MODE_COUNT] = { "123", "#+=", "abc" };

static lv_obj_t *s_kp, *s_ta;
static lv_timer_t *s_tap_timer;
static const char *s_map[KEY_COUNT + 2 + 4 + 1];   /* keys, spacers, row ends, the end */
static bool s_round;         /* a hidden spacer at each end of the bottom row */
static int s_mode;
static bool s_upper;
static int s_pending = -1;   /* the key whose character can still change */
static int s_step;
static uint32_t s_pressed_ms;
static bool s_held;          /* this press already typed its digit */

/* The button matrix's id for a key: past the round screen's first spacer. */
static int key_button(int key)
{
    return s_round && key >= KEY_MODE ? key + 1 : key;
}

/* The key a button is, or -1 for a spacer (or none). */
static int button_key(uint32_t btn)
{
    int key = s_round && btn >= KEY_MODE ? (int)btn - 1 : (int)btn;
    return key >= 0 && key < KEY_COUNT && !(s_round && btn == KEY_MODE) ? key : -1;
}

static void set_ctrl(int key, lv_buttonmatrix_ctrl_t ctrl, bool on)
{
    if (on) {
        lv_buttonmatrix_set_button_ctrl(s_kp, key_button(key), ctrl);
    } else {
        lv_buttonmatrix_clear_button_ctrl(s_kp, key_button(key), ctrl);
    }
}

static void update_map(void)
{
    int i = 0;
    for (int k = 0; k < 12; k++) {
        s_map[i++] = s_mode == LETTERS && s_upper && k < 9 ? UPPER_LABELS[k] : LABELS[s_mode][k];
        if (k % 3 == 2) {
            s_map[i++] = "\n";
        }
    }
    if (s_round) {
        s_map[i++] = " ";   /* a hidden spacer: an empty label would end the map */
    }
    s_map[i++] = NEXT_MODE[s_mode];
    s_map[i++] = lv_textarea_get_password_mode(s_ta) ? tr("Show") : tr("Hide");
    s_map[i++] = LV_SYMBOL_OK;
    if (s_round) {
        s_map[i++] = " ";
    }
    s_map[i] = "";
    lv_buttonmatrix_set_map(s_kp, s_map);
    set_ctrl(KEY_SHIFT, LV_BUTTONMATRIX_CTRL_CHECKED, s_mode == LETTERS && s_upper);
}

/* The pending character is final. */
static void commit(void)
{
    if (s_pending >= 0 && s_kp) {
        set_ctrl(s_pending, LV_BUTTONMATRIX_CTRL_CHECKED, false);
    }
    s_pending = -1;
    if (s_tap_timer) {
        lv_timer_pause(s_tap_timer);
    }
}

static void on_tap_timer(lv_timer_t *t)
{
    (void)t;
    commit();
}

static void add_char(char c)
{
    lv_textarea_add_char(s_ta, (uint32_t)(unsigned char)c);
}

static void type_key(int key)
{
    const char *chars = CHARS[s_mode][key];
    int n = (int)strlen(chars);
    if (n == 1) {
        commit();
        add_char(chars[0]);
        return;
    }
    if (key == s_pending) {
        s_step = (s_step + 1) % n;
        lv_textarea_delete_char(s_ta);
    } else {
        commit();
        s_step = 0;
    }
    char c = chars[s_step];
    size_t before = strlen(lv_textarea_get_text(s_ta));
    add_char(s_upper && s_mode == LETTERS ? (char)toupper((unsigned char)c) : c);
    if (strlen(lv_textarea_get_text(s_ta)) == before) {
        commit();   /* full: there's nothing to replace on the next tap */
        return;
    }
    s_pending = key;
    set_ctrl(key, LV_BUTTONMATRIX_CTRL_CHECKED, true);
    lv_timer_reset(s_tap_timer);
    lv_timer_resume(s_tap_timer);
}

/* The digit a held key types, or 0. */
static char held_digit(int key)
{
    if (key < 0 || key >= 12 || s_mode != LETTERS || !CHARS[LETTERS][key]) {
        return 0;
    }
    const char *chars = CHARS[LETTERS][key];
    char last = chars[strlen(chars) - 1];
    return isdigit((unsigned char)last) ? last : 0;
}

static void on_key(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    int key = button_key(lv_buttonmatrix_get_selected_button(s_kp));   /* -1 while slid off a key */

    if (code == LV_EVENT_PRESSED) {
        s_pressed_ms = lv_tick_get();
        s_held = false;
        return;
    }
    if (code == LV_EVENT_PRESSING) {
        char digit = held_digit(key);
        if (digit && !s_held && lv_tick_elaps(s_pressed_ms) >= HOLD_MS) {
            s_held = true;
            commit();
            add_char(digit);
        }
        return;
    }

    /* LV_EVENT_VALUE_CHANGED: keys fire on release, backspace on press and while held. */
    key = button_key(*(const uint32_t *)lv_event_get_param(e));
    if (key < 0 || s_held) {
        return;
    }
    switch (key) {
    case KEY_BACKSPACE:
        commit();
        lv_textarea_delete_char(s_ta);
        break;
    case KEY_SHIFT:
        if (s_mode == LETTERS) {
            commit();
            s_upper = !s_upper;
            update_map();
        } else {
            type_key(key);
        }
        break;
    case KEY_MODE:
        commit();
        s_mode = (s_mode + 1) % MODE_COUNT;
        update_map();
        break;
    case KEY_SHOW:
        lv_textarea_set_password_mode(s_ta, !lv_textarea_get_password_mode(s_ta));
        update_map();
        break;
    case KEY_DONE:
        commit();
        lv_obj_send_event(s_kp, LV_EVENT_READY, NULL);
        break;
    default:
        if (key < 12) {
            type_key(key);
        }
        break;
    }
}

/* Moving the cursor mustn't let the next tap replace a character somewhere else. */
static void on_ta_pressed(lv_event_t *e)
{
    (void)e;
    commit();
}

static void on_delete(lv_event_t *e)
{
    /* A page being replaced can go after its successor was made. */
    if (lv_event_get_target(e) != s_kp) {
        return;
    }
    if (s_tap_timer) {
        lv_timer_delete(s_tap_timer);
        s_tap_timer = NULL;
    }
    s_kp = NULL;
    s_ta = NULL;
    s_pending = -1;
}

lv_obj_t *muse_keypad_create(lv_obj_t *parent, lv_obj_t *ta, bool round)
{
    s_kp = lv_buttonmatrix_create(parent);
    s_ta = ta;
    s_round = round;
    s_mode = LETTERS;
    s_upper = false;
    s_pending = -1;
    if (!s_tap_timer) {
        s_tap_timer = lv_timer_create(on_tap_timer, TAP_MS, NULL);
    }
    lv_timer_pause(s_tap_timer);
    lv_textarea_set_password_show_time(ta, TAP_MS + 500);   /* long enough to see what the taps chose */

    lv_obj_remove_style_all(s_kp);
    lv_obj_set_style_pad_gap(s_kp, 6, 0);
    lv_obj_set_style_text_font(s_kp, UI_FONT_BIG, LV_PART_ITEMS);
    lv_obj_set_style_text_color(s_kp, lv_color_hex(COLOR_TEXT), LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(s_kp, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(s_kp, lv_color_hex(COLOR_CARD), LV_PART_ITEMS);
    lv_obj_set_style_bg_color(s_kp, lv_color_hex(COLOR_CARD_PRESSED), LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(s_kp, lv_color_hex(COLOR_ACCENT), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_radius(s_kp, 12, LV_PART_ITEMS);
    /* A swipe across the keys is sloppy typing, not "back": it would lose the text. */
    lv_obj_remove_flag(s_kp, LV_OBJ_FLAG_GESTURE_BUBBLE);

    update_map();
    lv_obj_add_event_cb(s_kp, on_key, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_kp, on_key, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(s_kp, on_key, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s_kp, on_delete, LV_EVENT_DELETE, NULL);
    lv_obj_add_event_cb(ta, on_ta_pressed, LV_EVENT_PRESSED, NULL);
    muse_keypad_reset(s_kp, false);
    return s_kp;
}

void muse_keypad_reset(lv_obj_t *kp, bool password)
{
    (void)kp;
    commit();
    s_mode = LETTERS;
    s_upper = false;
    update_map();

    const lv_buttonmatrix_ctrl_t tap = LV_BUTTONMATRIX_CTRL_CLICK_TRIG | LV_BUTTONMATRIX_CTRL_NO_REPEAT;
    for (int k = 0; k < KEY_COUNT; k++) {
        if (k != KEY_BACKSPACE) {
            set_ctrl(k, tap, true);
        }
    }
    set_ctrl(KEY_DONE, LV_BUTTONMATRIX_CTRL_CHECKED, true);
    /* Without a password the gap where Show was keeps Done apart from the mode key. */
    set_ctrl(KEY_SHOW, LV_BUTTONMATRIX_CTRL_HIDDEN, !password);
    /* Round, Done is no wider than the mode key, so both stay in from the edge. */
    int done = s_round ? 0 : 1;
    lv_buttonmatrix_set_button_width(s_kp, key_button(KEY_MODE), password ? 3 : 4);
    lv_buttonmatrix_set_button_width(s_kp, key_button(KEY_SHOW), password ? 3 : 1);
    lv_buttonmatrix_set_button_width(s_kp, key_button(KEY_DONE), (password ? 3 : 4) + done);
    if (s_round) {
        const uint32_t spacers[] = { KEY_MODE, KEY_COUNT + 1 };   /* before key_button(KEY_MODE), after Done */
        for (int i = 0; i < 2; i++) {
            lv_buttonmatrix_set_button_ctrl(s_kp, spacers[i], LV_BUTTONMATRIX_CTRL_HIDDEN);
            lv_buttonmatrix_set_button_width(s_kp, spacers[i], 2);
        }
    }
}
