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

#include "muse_passcode_ui.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "lvgl.h"
#include "muse_ble.h"
#include "muse_board.h"
#include "muse_home.h"
#include "muse_icons.h"
#include "muse_link.h"
#include "muse_menu.h"
#include "muse_passcode.h"
#include "muse_settings.h"
#include "muse_settings_ui.h"
#include "muse_settings_card.h"
#include "muse_type.h"
#include "muse_ui.h"
#include "sdkconfig.h"
#if CONFIG_MUSE_GADGET_BLE_AUDIO
#include "mg_voice.h"
#endif
#if CONFIG_MUSE_VIDEO
#include "muse_video.h"
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static const char *TAG = "muse_pass";

/* The passcode screens' colors. */
#define COLOR_TEXT 0xf2efff
#define COLOR_DIM 0x8b84a8
#define COLOR_CARD 0x1a1530
#define COLOR_FOCUS 0x2e2552
#define COLOR_ACCENT 0xa77dff
#define COLOR_DANGER 0xff5c5c
#define COLOR_KEY_TEXT_ON 0x14102a   /* the digit on the accent-filled key under the finger */
#define TRAIL_OPA 150                /* the trail, of 255 (59 %) */

/* The keypad's timing. */
#define T_SUBMIT_MS 120     /* the 4th dot shown before the result (at least 100) */
#define T_RIGHT_MS 150      /* right code to the home */
#define T_SHAKE_MS 400      /* wrong: three damped swings */
#define T_DOTS_MS 600       /* wrong: the red dots clear */
#define T_PROMPT_MS 1500    /* wrong / didn't match: the prompt says so this long */
#define T_LINGER_MS 150     /* the key highlight after the finger leaves it */
#define T_TRAIL_MS 200      /* the trail after the lift */
#define T_TOAST_MS 1500
#define T_RESET_MS 3000     /* Reset pairing: two taps within 3 s */
#define T_BUZZ2_MS 170      /* a double tick: the second (the haptics core's double, 50 + 120) */
#define TRAIL_MAX 64

typedef enum { S_NONE, S_KEYPAD, S_WAIT, S_FORGOT } screen_t;
typedef enum { P_UNLOCK, P_VERIFY_CHANGE, P_VERIFY_OFF, P_CHOOSE, P_CONFIRM } purpose_t;
typedef enum { AFTER_NOTHING, AFTER_WAIT, AFTER_CHOOSE } after_t;

static bool s_built;
static muse_pass_geom_t s_g;
static const lv_font_t *s_f_prompt, *s_f_small, *s_f_digit, *s_f_title;
static int s_prompt_caps, s_small_caps, s_digit_caps;
static lv_obj_t *s_catch;     /* the locked face's catcher: any touch opens the keypad */
static lv_obj_t *s_screen;    /* the keypad, the wait or Forgot, on the top layer */
static lv_obj_t *s_toast;
static lv_timer_t *s_timer;
static screen_t s_mode;
static purpose_t s_purpose;
static int s_why;
static int s_pending_why;     /* a press's request waiting for the screen to light */
static void (*s_done)(bool changed);
static bool s_flow;           /* a Settings flow (else: unlocking) */
static bool s_was_locked;
static bool s_face;

/* The keypad. */
static lv_obj_t *s_keys[10], *s_key_lbl[10], *s_trail, *s_prompt, *s_dots_box, *s_dots[MUSE_PASS_DIGITS];
static lv_obj_t *s_del_icon, *s_del_lbl;
static lv_point_precise_t s_trail_pts[TRAIL_MAX];
static int s_trail_n;
static muse_pass_stroke_t s_st;
static bool s_ignore_stroke;
static char s_entry[MUSE_PASS_DIGITS], s_first[MUSE_PASS_DIGITS];
static int s_n;
static bool s_busy;           /* 4 digits in, the result not yet taken */
static bool s_red;            /* the dots show a wrong code */
static uint32_t s_t_touch, s_t_submit, s_t_right, s_t_wrong, s_t_linger, s_t_trail, s_t_toast, s_t_buzz2, s_t_reset;
static bool s_submit_due, s_right_due, s_trail_due, s_toast_up, s_buzz2_due, s_reset_armed;
static int s_wrong_phase;     /* 1: red dots shaking, 2: the prompt only */
static after_t s_after;
static int s_linger = -1;

/* The wait and Forgot. */
static lv_obj_t *s_wait_lbl, *s_forgot_card, *s_reset_card, *s_reset_val;
static int s_wait_shown = -1;

static uint32_t now_ms(void)
{
    return lv_tick_get();
}

static uint32_t since(uint32_t t)
{
    return lv_tick_elaps(t);
}

static int caps_of(const lv_font_t *f, uint32_t letter)
{
    lv_font_glyph_dsc_t g;
    return lv_font_get_glyph_dsc(f, &g, letter, 0) ? g.box_h : lv_font_get_line_height(f) * 7 / 10;
}

static int text_w(const lv_font_t *f, const char *t)
{
    lv_point_t sz;
    lv_text_get_size(&sz, t, f, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return sz.x;
}

static lv_obj_t *plain(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return o;
}

/* A label whose capitals' top is at caps_top, centered on x. */
static lv_obj_t *text_at(lv_obj_t *parent, const lv_font_t *f, uint32_t color, const char *t, double x, int caps_top)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(l, t);
    lv_obj_align(l, LV_ALIGN_TOP_MID, (int)lround(x - s_g.cx), caps_top - muse_type_ink_top(f, 'H'));
    return l;
}

/* An icon centered on (x, y). */
static lv_obj_t *icon_at(lv_obj_t *parent, muse_icon_t icon, int px, uint32_t color, double x, double y)
{
    lv_obj_t *img = muse_icon_create(parent, icon, px, color);
    lv_obj_align(img, LV_ALIGN_CENTER, (int)lround(x - s_g.cx), (int)lround(y - s_g.cy));
    return img;
}

static int inset(void)
{
    return s_g.ring + s_g.m;
}

/* ---- haptics: a tick per digit, a double tick on a wrong code, under
 * the haptics switch and the haptics core's rules where the gadget has one ---- */

static void tick(void)
{
#if CONFIG_MUSE_GADGET_BLE_AUDIO
    if (mg_ble_haptic_talk_cue(50)) {
        return;
    }
#endif
    if (muse_board->buzz && muse_settings_haptics()) {
        muse_board->buzz(50, 60);
    }
}

static void buzz(int n)
{
    tick();
    if (n > 1) {
        s_buzz2_due = true;
        s_t_buzz2 = now_ms();
    }
}

/* ---- the prompt (one line, or two at the same size) --------------------- */

/* Room for a line whose capitals span y .. y + caps. */
static double room(double y, int caps)
{
    if (s_g.layout == MUSE_PASS_RING) {
        double r = s_g.center_vis_r;
        double dy = fmax(fabs(y - s_g.cy), fabs(y + caps - s_g.cy));
        return dy < r ? 2 * sqrt(r * r - dy * dy) - 2 * s_g.edge : 0;
    }
    return 2 * fmin(muse_home_safe_half(y, inset()), muse_home_safe_half(y + caps, inset()));
}

/* Splits text in two at the space that makes the longer line shortest. */
static bool split2(const lv_font_t *f, const char *text, char *out, size_t n)
{
    int best = -1, best_w = 0;
    char a[64];
    size_t len = strlen(text);
    if (len >= sizeof(a)) return false;
    for (size_t i = 1; i < len; i++) {
        if (text[i] != ' ') continue;
        memcpy(a, text, i);
        a[i] = 0;
        int w = text_w(f, a), w2 = text_w(f, text + i + 1);
        int m = w > w2 ? w : w2;
        if (best < 0 || m < best_w) {
            best = (int)i;
            best_w = m;
        }
    }
    if (best < 0) return false;
    snprintf(out, n, "%.*s\n%s", best, text, text + best + 1);
    return true;
}

static int prompt_lines(const char *text, int bottom)
{
    return text_w(s_f_prompt, text) <= room(bottom - s_prompt_caps, s_prompt_caps) ? 1 : 2;
}

/* The prompt above the dots, anchored at its last line's capitals. */
static void set_prompt(const char *text, uint32_t color)
{
    if (!s_prompt) return;
    int bottom = (int)lround(s_g.dots_cy - s_g.dot_d / 2.0 - s_g.gap);
    int pitch = muse_pass_prompt_pitch(s_prompt_caps);
    char two[96];
    int n = prompt_lines(text, bottom);
    if (n == 2 && !split2(s_f_prompt, text, two, sizeof(two))) n = 1;
    lv_label_set_text(s_prompt, n == 2 ? two : text);
    lv_obj_set_style_text_color(s_prompt, lv_color_hex(color), 0);
    lv_obj_set_style_text_line_space(s_prompt, pitch - lv_font_get_line_height(s_f_prompt), 0);
    int first = bottom - s_prompt_caps - (n - 1) * pitch;
    lv_obj_align(s_prompt, LV_ALIGN_TOP_MID, 0, first - muse_type_ink_top(s_f_prompt, 'H'));
}

static const char *purpose_prompt(void)
{
    switch (s_purpose) {
    case P_UNLOCK:
        return s_why == MUSE_PASS_WHY_TALK         ? "Unlock to talk"
               : s_why == MUSE_PASS_WHY_UPDATE     ? "Unlock to update"
               : s_why == MUSE_PASS_WHY_CALL       ? "Unlock to call"
               : s_why == MUSE_PASS_WHY_PHONE_CALL ? "Call from phone"
                                                   : "Enter passcode";
    case P_CHOOSE:
        return "Choose a passcode";
    case P_CONFIRM:
        return "Enter it again";
    default:
        return "Enter passcode";
    }
}

/* ---- the keypad's drawing ------------------------------------------------ */

static void paint(void)
{
    if (s_mode != S_KEYPAD) return;
    int ring_w = (int)lround(0.035 * s_g.pitch);
    ring_w = ring_w < 2 ? 2 : ring_w;
    int under = s_st.down ? s_st.under : -1;
    for (int k = 0; k < 10; k++) {
        bool on = k == under || (k == s_linger && since(s_t_linger) < T_LINGER_MS);
        bool entered = s_st.down && (s_st.stroke_keys & (1u << k));
        lv_obj_set_style_bg_color(s_keys[k], lv_color_hex(on ? COLOR_ACCENT : entered ? COLOR_FOCUS : COLOR_CARD), 0);
        lv_obj_set_style_border_width(s_keys[k], !on && entered ? ring_w : 0, 0);
        lv_obj_set_style_text_color(s_key_lbl[k], lv_color_hex(on ? COLOR_KEY_TEXT_ON : COLOR_TEXT), 0);
    }
    for (int i = 0; i < MUSE_PASS_DIGITS; i++) {
        bool filled = s_red || i < s_n;
        uint32_t c = s_red ? COLOR_DANGER : COLOR_TEXT;
        lv_obj_set_style_bg_opa(s_dots[i], filled ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_bg_color(s_dots[i], lv_color_hex(c), 0);
        lv_obj_set_style_border_color(s_dots[i], lv_color_hex(s_red ? COLOR_DANGER : COLOR_DIM), 0);
        lv_obj_set_style_border_width(s_dots[i], filled ? 0 : s_g.dot_stroke, 0);
    }
    /* The ring hides delete while a finger is down (a swipe crosses the center). */
    bool del_shown = !(s_g.layout == MUSE_PASS_RING && s_st.down);
    lv_obj_set_flag(s_del_icon, LV_OBJ_FLAG_HIDDEN, !del_shown || s_n == 0);
    lv_obj_set_flag(s_del_lbl, LV_OBJ_FLAG_HIDDEN, !del_shown || s_n != 0);
}

static void trail_set(void)
{
    if (!s_trail) return;
    lv_line_set_points(s_trail, s_trail_pts, (uint32_t)s_trail_n);
    lv_obj_set_flag(s_trail, LV_OBJ_FLAG_HIDDEN, s_trail_n < 2);
}

static void trail_add(int x, int y)
{
    if (s_trail_n == TRAIL_MAX) {
        /* Keep the shape, half the points. */
        for (int i = 1; i < TRAIL_MAX / 2; i++) s_trail_pts[i] = s_trail_pts[2 * i];
        s_trail_n = TRAIL_MAX / 2;
    }
    s_trail_pts[s_trail_n].x = x;
    s_trail_pts[s_trail_n].y = y;
    s_trail_n++;
    trail_set();
}

static void trail_clear(void)
{
    s_trail_n = 0;
    s_trail_due = false;
    trail_set();
}

static void shake_cb(void *obj, int32_t v)
{
    double t = v / 1000.0;
    double a = round(0.03 * s_g.s);
    lv_obj_set_style_translate_x(obj, (int)lround(a * sin(2 * M_PI * 3 * t) * (1 - t)), 0);
}

static void shake(void)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_dots_box);
    lv_anim_set_exec_cb(&a, shake_cb);
    lv_anim_set_values(&a, 0, 1000);
    lv_anim_set_duration(&a, T_SHAKE_MS);
    lv_anim_start(&a);
}

/* ---- screens ------------------------------------------------------------- */

static void timer_cb(lv_timer_t *t);

static void ensure_timer(void)
{
    if (!s_timer) {
        s_timer = lv_timer_create(timer_cb, 30, NULL);
    }
}

static void close_screen(void)
{
    if (s_screen) {
        if (s_dots_box) lv_anim_delete(s_dots_box, NULL);
        lv_obj_add_flag(s_screen, LV_OBJ_FLAG_HIDDEN);
        lv_obj_delete_async(s_screen);   /* we may be in one of its own events */
    }
    s_screen = s_prompt = s_dots_box = s_trail = s_del_icon = s_del_lbl = NULL;
    s_wait_lbl = s_forgot_card = s_reset_card = s_reset_val = NULL;
    memset(s_keys, 0, sizeof(s_keys));
    memset(s_key_lbl, 0, sizeof(s_key_lbl));
    memset(s_dots, 0, sizeof(s_dots));
    memset(s_entry, 0, sizeof(s_entry));
    memset(&s_st, 0, sizeof(s_st));
    s_n = 0;
    s_busy = s_red = s_submit_due = s_right_due = s_trail_due = s_reset_armed = false;
    s_wrong_phase = 0;
    s_trail_n = 0;
    s_linger = -1;
    s_wait_shown = -1;
    s_mode = S_NONE;
#if CONFIG_MUSE_VIDEO
    muse_video_set_cover(false);
#endif
}

static void finish(bool changed)
{
    void (*done)(bool) = s_done;
    close_screen();
    memset(s_first, 0, sizeof(s_first));
    s_done = NULL;
    s_flow = false;
    if (done) done(changed);
}

static lv_obj_t *new_screen(void)
{
    close_screen();
    lv_obj_t *p = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(p);
    lv_obj_set_size(p, s_g.w, s_g.h);
    lv_obj_set_pos(p, 0, 0);
    lv_obj_set_style_bg_color(p, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    lv_obj_remove_flag(p, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_flag(p, LV_OBJ_FLAG_CLICKABLE);
    /* Under the Link setup prompt and the sleep cover, which come later on
     * the top layer (muse_ui.c's overlays). */
    lv_obj_move_to_index(p, s_catch ? lv_obj_get_index(s_catch) + 1 : 0);
    s_screen = p;
    s_t_touch = now_ms();
    ensure_timer();
#if CONFIG_MUSE_VIDEO
    muse_video_set_cover(true);   /* a stream stays off the keypad */
#endif
    return p;
}

static void on_pad(lv_event_t *e);
static void open_wait(void);

static void build_keypad(void)
{
    lv_obj_t *p = new_screen();
    s_mode = S_KEYPAD;
    static const lv_event_code_t EV[] = { LV_EVENT_PRESSED, LV_EVENT_PRESSING, LV_EVENT_RELEASED, LV_EVENT_PRESS_LOST };
    for (size_t i = 0; i < sizeof(EV) / sizeof(EV[0]); i++) {
        lv_obj_add_event_cb(p, on_pad, EV[i], NULL);
    }
    for (int k = 0; k < 10; k++) {
        lv_obj_t *key = plain(p);
        if (s_g.layout == MUSE_PASS_RING) {
            int d = (int)lround(s_g.key_d);
            lv_obj_set_size(key, d, d);
            lv_obj_set_style_radius(key, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_pos(key, (int)lround(s_g.key_x[k] - d / 2.0), (int)lround(s_g.key_y[k] - d / 2.0));
        } else {
            int w = (int)lround(s_g.key_w), h = (int)lround(s_g.key_h);
            lv_obj_set_size(key, w, h);
            lv_obj_set_style_radius(key, s_g.key_r, 0);
            lv_obj_set_pos(key, (int)lround(s_g.key_x[k] - w / 2.0), (int)lround(s_g.key_y[k] - h / 2.0));
        }
        lv_obj_set_style_bg_opa(key, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(key, lv_color_hex(COLOR_CARD), 0);
        lv_obj_set_style_border_color(key, lv_color_hex(COLOR_ACCENT), 0);
        lv_obj_set_style_border_width(key, 0, 0);
        s_keys[k] = key;
    }
    /* The trail: over the keys, under the digits and the center's text. */
    s_trail = lv_line_create(p);
    lv_obj_set_pos(s_trail, 0, 0);
    int tw = (int)lround(0.16 * s_g.pitch);
    lv_obj_set_style_line_width(s_trail, tw < 4 ? 4 : tw, 0);
    lv_obj_set_style_line_color(s_trail, lv_color_hex(COLOR_ACCENT), 0);
    lv_obj_set_style_line_opa(s_trail, TRAIL_OPA, 0);
    lv_obj_set_style_line_rounded(s_trail, true, 0);
    lv_obj_remove_flag(s_trail, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_trail, LV_OBJ_FLAG_HIDDEN);
    for (int k = 0; k < 10; k++) {
        char d[2] = { (char)('0' + k), 0 };
        lv_obj_t *l = lv_label_create(p);
        lv_obj_set_style_text_font(l, s_f_digit, 0);
        lv_obj_set_style_text_color(l, lv_color_hex(COLOR_TEXT), 0);
        lv_label_set_text(l, d);
        int w = text_w(s_f_digit, d);
        lv_obj_set_pos(l, (int)lround(s_g.key_x[k] - w / 2.0),
                       (int)lround(s_g.key_y[k] - s_digit_caps / 2.0) - muse_type_ink_top(s_f_digit, '0'));
        s_key_lbl[k] = l;
    }
    s_prompt = lv_label_create(p);
    lv_obj_set_style_text_font(s_prompt, s_f_prompt, 0);
    lv_obj_set_style_text_align(s_prompt, LV_TEXT_ALIGN_CENTER, 0);
    /* The dots, in a box that shakes. */
    s_dots_box = plain(p);
    int d = s_g.dot_d, pitch = s_g.dot_pitch;
    lv_obj_set_size(s_dots_box, 3 * pitch + d, d);
    lv_obj_set_pos(s_dots_box, (int)lround(s_g.cx - 1.5 * pitch - d / 2.0), (int)lround(s_g.dots_cy - d / 2.0));
    for (int i = 0; i < MUSE_PASS_DIGITS; i++) {
        lv_obj_t *dot = plain(s_dots_box);
        lv_obj_set_size(dot, d, d);
        lv_obj_set_pos(dot, i * pitch, 0);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_opa(dot, LV_OPA_COVER, 0);
        s_dots[i] = dot;
    }
    double dx = s_g.layout == MUSE_PASS_RING ? s_g.cx : s_g.key_x[MUSE_PASS_KEY_DEL];
    double dy = s_g.layout == MUSE_PASS_RING ? s_g.del_cy : s_g.key_y[MUSE_PASS_KEY_DEL];
    s_del_icon = icon_at(p, MUSE_ICON_BACKSPACE, s_g.del_box, COLOR_DIM, dx, dy);
    s_del_lbl = text_at(p, s_f_small, COLOR_DIM, "Cancel", dx, (int)lround(dy - s_small_caps / 2.0));
    set_prompt(purpose_prompt(), COLOR_TEXT);
    paint();
}

static void open_keypad(purpose_t purpose, int why)
{
    s_purpose = purpose;
    s_why = why;
    bool checks = purpose == P_UNLOCK || purpose == P_VERIFY_CHANGE || purpose == P_VERIFY_OFF;
    if (checks && muse_passcode_wait_left_ms() > 0) {
        open_wait();
        return;
    }
    build_keypad();
}

/* Any touch on the locked face (not a press on the Link setup prompt over
 * it, which takes its own). */
static void on_catch(lv_event_t *e)
{
    (void)e;
    muse_ble_status_t b;
    muse_ble_status(&b);
    if (muse_link_state() == MUSE_LINK_CONFIRM || b.passkey) {
        return;
    }
    open_keypad(P_UNLOCK, MUSE_PASS_WHY_TOUCH);
}

static void cancel(void)
{
    if (s_flow) {
        finish(false);
    } else {
        close_screen();   /* back to the locked face */
    }
}

static void wrong(const char *text, after_t after)
{
    s_red = true;
    s_n = MUSE_PASS_DIGITS;
    s_wrong_phase = 1;
    s_t_wrong = now_ms();
    s_after = after;
    set_prompt(text, COLOR_DANGER);
    shake();
    buzz(2);
    paint();
}

static void toast(const char *text)
{
    lv_label_set_text(lv_obj_get_child(s_toast, 0), text);
    lv_obj_remove_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_toast);
    s_toast_up = true;
    s_t_toast = now_ms();
    ensure_timer();
}

static void submit(void)
{
    s_submit_due = false;
    trail_clear();   /* gone at submit */
    memset(&s_st, 0, sizeof(s_st));
    if (s_purpose == P_CHOOSE) {
        memcpy(s_first, s_entry, sizeof(s_first));
        memset(s_entry, 0, sizeof(s_entry));
        s_purpose = P_CONFIRM;
        s_n = 0;
        s_busy = false;
        set_prompt(purpose_prompt(), COLOR_TEXT);
        paint();
        return;
    }
    if (s_purpose == P_CONFIRM) {
        bool match = !memcmp(s_first, s_entry, sizeof(s_first));
        if (match) {
            bool setup = !muse_passcode_on();
            bool ok = muse_passcode_set(s_entry);
            memset(s_entry, 0, sizeof(s_entry));
            memset(s_first, 0, sizeof(s_first));
            if (ok && setup) toast("Passcode on");
            finish(ok);
        } else {
            memset(s_first, 0, sizeof(s_first));
            memset(s_entry, 0, sizeof(s_entry));
            wrong("Didn't match", AFTER_CHOOSE);
        }
        return;
    }
    muse_pass_result_t r = muse_passcode_check(s_entry);
    memset(s_entry, 0, sizeof(s_entry));
    switch (r) {
    case MUSE_PASS_RIGHT:
        if (s_purpose == P_UNLOCK) {
            s_right_due = true;   /* the home 150 ms later */
            s_t_right = now_ms();
        } else if (s_purpose == P_VERIFY_CHANGE) {
            s_purpose = P_CHOOSE;
            s_n = 0;
            s_busy = false;
            set_prompt(purpose_prompt(), COLOR_TEXT);
            paint();
        } else {
            muse_passcode_turn_off();
            toast("Passcode off");
            finish(true);
        }
        break;
    case MUSE_PASS_WRONG:
        wrong("Wrong passcode", AFTER_NOTHING);
        break;
    case MUSE_PASS_WAIT:
        wrong("Wrong passcode", AFTER_WAIT);
        break;
    case MUSE_PASS_WAITING:
        open_wait();
        break;
    }
}

static void take(const int *ev, int n)
{
    for (int i = 0; i < n && !s_busy; i++) {
        if (ev[i] == MUSE_PASS_EV_DEL) {
            if (s_n > 0) {
                s_entry[--s_n] = 0;
            } else {
                cancel();   /* "Cancel": back to the locked face (or the Passcode page) */
                return;
            }
        } else if (s_n < MUSE_PASS_DIGITS) {
            s_entry[s_n++] = (char)('0' + ev[i]);
            buzz(1);
            if (s_n == MUSE_PASS_DIGITS) {
                s_busy = true;   /* the 4th digit submits at its registration */
                s_submit_due = true;
                s_t_submit = now_ms();
            }
        }
    }
}

static void on_pad(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *indev = lv_indev_active();
    lv_point_t pt = { 0 };
    if (indev) lv_indev_get_point(indev, &pt);
    s_t_touch = now_ms();
    int ev[MUSE_PASS_EV_MAX];
    int n = 0;
    int was = s_st.under;
    if (code == LV_EVENT_PRESSED) {
        s_ignore_stroke = s_busy || s_wrong_phase == 1;
        if (s_ignore_stroke) return;
        n = muse_pass_stroke_down(&s_st, &s_g, pt.x, pt.y, ev);
        s_trail_n = 0;
        s_trail_due = false;
        trail_add(pt.x, pt.y);
    } else if (code == LV_EVENT_PRESSING) {
        if (s_ignore_stroke || !s_st.down) return;
        n = muse_pass_stroke_move(&s_st, &s_g, pt.x, pt.y, ev);
        trail_add(pt.x, pt.y);
    } else {
        if (s_ignore_stroke || !s_st.down) return;
        n = muse_pass_stroke_up(&s_st, &s_g, pt.x, pt.y, ev);
        s_trail_due = true;   /* the trail goes 200 ms after the lift */
        s_t_trail = now_ms();
    }
    if (was >= 0 && was != s_st.under) {
        s_linger = was;
        s_t_linger = now_ms();
    }
    take(ev, n);
    if (s_mode == S_KEYPAD) paint();
}

/* ---- the wait and Forgot ------------------------------------------------- */

static void open_forgot(void);

static void on_forgot_card(lv_event_t *e)
{
    (void)e;
    open_forgot();
}

static void wait_text(void)
{
    int64_t left = muse_passcode_wait_left_ms();
    int s = (int)((left + 999) / 1000);
    if (s == s_wait_shown || !s_wait_lbl) return;
    s_wait_shown = s;
    char t[32];
    snprintf(t, sizeof(t), "Try again in %d:%02d", s / 60, s % 60);
    lv_label_set_text(s_wait_lbl, t);   /* only the text box redraws */
}

/* A card like a Settings row, colw wide, its top at y. */
static lv_obj_t *card_at(lv_obj_t *p, int y, muse_icon_t icon, uint32_t color, const char *text, lv_obj_t **value,
                         lv_event_cb_t cb)
{
    muse_settings_ui_geom_t G;
    muse_settings_ui_geom(&G);
    int colw = (int)(2 * fmin(muse_home_safe_half(y, inset()), muse_home_safe_half(y + G.row_h, inset())));
    colw = colw > G.list_w ? G.list_w : colw;
    lv_obj_t *box = plain(p);
    lv_obj_set_size(box, colw, G.row_h);
    lv_obj_set_pos(box, (int)lround(s_g.cx - colw / 2.0), y);
    lv_obj_t *c = muse_settings_ui_card(box, icon, color, text, color == COLOR_DANGER ? COLOR_DANGER : COLOR_TEXT, value,
                                        cb, NULL);
    muse_settings_ui_card_fit(c, colw, 0);
    return c;
}

static void open_wait(void)
{
    lv_obj_t *p = new_screen();
    s_mode = S_WAIT;
    int lb = (int)lround(0.11 * s_g.s);
    double ly = round(0.25 * s_g.h);
    icon_at(p, MUSE_ICON_LOCK, lb, COLOR_DIM, s_g.cx, ly);
    int top = (int)lround(ly + lb / 2.0 + s_g.gap);
    s_wait_lbl = text_at(p, s_f_prompt, COLOR_TEXT, "Try again in 0:30", s_g.cx, top);
    int top2 = top + s_prompt_caps + (int)lround(0.7 * s_g.gap);
    text_at(p, s_f_small, COLOR_DIM, "Too many tries", s_g.cx, top2);
    int yc = top2 + s_small_caps + 2 * s_g.gap;
    int y58 = (int)lround(0.58 * s_g.h);
    s_forgot_card = card_at(p, yc > y58 ? yc : y58, MUSE_ICON_INFO, COLOR_ACCENT, "Forgot passcode", NULL, on_forgot_card);
    s_wait_shown = -1;
    wait_text();
}

/* "Tap twice" beside the label where it fits whole (the value goes
 * first); armed, "Tap again" there, or in the label's place. */
static void reset_text(bool armed)
{
    if (!s_reset_card || !s_reset_val) return;
    lv_obj_t *label = lv_obj_get_child(s_reset_card, 1);
    muse_settings_ui_geom_t G;
    muse_settings_ui_geom(&G);
    int colw = lv_obj_get_style_width(lv_obj_get_parent(s_reset_card), 0);   /* set, not yet laid out */
    int vgap = (int)lround(0.6 * G.icon_gap) > 6 ? (int)lround(0.6 * G.icon_gap) : 6;
    int vw = text_w(G.value, "Tap twice") + vgap;
    bool value = text_w(G.label, "Reset pairing") <= colw - 2 * G.pad - G.icon_px - G.icon_gap - vw;
    lv_obj_set_flag(s_reset_val, LV_OBJ_FLAG_HIDDEN, !value);
    lv_label_set_text(s_reset_val, armed ? "Tap again" : "Tap twice");
    lv_label_set_text(label, armed && !value ? "Tap again" : "Reset pairing");
    muse_settings_ui_card_fit(s_reset_card, colw, value ? vw : 0);
}

static void on_reset(lv_event_t *e)
{
    (void)e;
    if (s_reset_armed && since(s_t_reset) < T_RESET_MS) {
        lv_label_set_text(lv_obj_get_child(s_reset_card, 1), "Resetting...");
        lv_obj_add_flag(s_reset_val, LV_OBJ_FLAG_HIDDEN);
        ESP_LOGI(TAG, "Forgot passcode: Reset pairing");
        muse_link_reset_setup();   /* erases the passcode with the pairing (app.c), then restarts */
        s_reset_armed = false;
        return;
    }
    s_reset_armed = true;
    s_t_reset = now_ms();
    reset_text(true);
}

static void on_forgot_gesture(lv_event_t *e)
{
    (void)e;
    if (lv_indev_get_gesture_dir(lv_indev_active()) == LV_DIR_RIGHT) {
        lv_indev_wait_release(lv_indev_active());
        open_keypad(s_purpose, s_why);   /* back: the wait, or the keypad once it's over */
    }
}

static void open_forgot(void)
{
    lv_obj_t *p = new_screen();
    s_mode = S_FORGOT;
    lv_obj_add_event_cb(p, on_forgot_gesture, LV_EVENT_GESTURE, NULL);
    muse_settings_ui_geom_t G;
    muse_settings_ui_geom(&G);
    int tcaps = caps_of(G.title, 'H');
    const char *title = "Forgot passcode";
    if (text_w(G.title, title) > room(G.title_top, tcaps) && s_g.layout == MUSE_PASS_GRID) {
        title = "Forgot it?";
    }
    if (s_g.layout == MUSE_PASS_RING) {
        /* Radially inset, measured on the title row's corners. */
        double r = s_g.s / 2.0 - inset();
        double dy = fmax(fabs(G.title_top - s_g.cy), fabs(G.title_top + tcaps - s_g.cy));
        double room_t = 2 * sqrt(fmax(0, r * r - dy * dy));
        if (text_w(G.title, title) > room_t) title = "Forgot it?";   /* where the long one doesn't fit the title row */
    }
    text_at(p, G.title, COLOR_ACCENT, title, s_g.cx, G.title_top);
    /* The body, wrapped inside the panel from the list's top. */
    int pitch = (int)lround(s_small_caps * 1.75);
    int w = (int)(2 * fmin(muse_home_safe_half(G.list_top, inset()), muse_home_safe_half(G.list_top + 4 * pitch, inset())));
    lv_obj_t *body = lv_label_create(p);
    lv_obj_set_style_text_font(body, s_f_small, 0);
    lv_obj_set_style_text_color(body, lv_color_hex(COLOR_DIM), 0);
    lv_obj_set_style_text_align(body, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_line_space(body, pitch - lv_font_get_line_height(s_f_small), 0);
    lv_obj_set_width(body, w);
    lv_label_set_long_mode(body, LV_LABEL_LONG_MODE_WRAP);
    lv_label_set_text(body, "Reset pairing erases the passcode and this gadget's pairing. Then set it up again in the "
                            "Muse app.");
    lv_obj_align(body, LV_ALIGN_TOP_MID, 0, G.list_top - muse_type_ink_top(s_f_small, 'H'));
    lv_obj_update_layout(body);
    int lines = (lv_obj_get_height(body) + pitch / 2) / pitch;
    int yc = G.list_top + (lines - 1) * pitch + s_small_caps + (int)lround(1.4 * s_g.gap);
    s_reset_card = card_at(p, yc, MUSE_ICON_BLUETOOTH, COLOR_DANGER, "Reset pairing", &s_reset_val, on_reset);
    s_reset_armed = false;
    reset_text(false);
}

/* ---- the clock ----------------------------------------------------------- */

static void timer_cb(lv_timer_t *t)
{
    (void)t;
    if (s_buzz2_due && since(s_t_buzz2) >= T_BUZZ2_MS) {
        s_buzz2_due = false;
        tick();
    }
    if (s_toast_up && since(s_t_toast) >= T_TOAST_MS) {
        s_toast_up = false;
        lv_obj_add_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_mode == S_KEYPAD) {
        if (s_submit_due && since(s_t_submit) >= T_SUBMIT_MS) {
            submit();
            if (s_mode != S_KEYPAD) goto out;
        }
        if (s_right_due && since(s_t_right) >= T_RIGHT_MS) {
            s_right_due = false;
            close_screen();   /* unlocked: the home (muse_passcode_ui_tick sees it) */
            goto out;
        }
        if (s_wrong_phase == 1 && since(s_t_wrong) >= T_DOTS_MS) {
            s_red = false;
            s_n = 0;
            s_busy = false;
            s_wrong_phase = 2;
            if (s_after == AFTER_WAIT) {
                open_wait();
                goto out;
            }
            if (s_after == AFTER_CHOOSE) s_purpose = P_CHOOSE;
            paint();
        }
        if (s_wrong_phase == 2 && since(s_t_wrong) >= T_PROMPT_MS) {
            s_wrong_phase = 0;
            set_prompt(purpose_prompt(), COLOR_TEXT);
        }
        if (s_trail_due && since(s_t_trail) >= T_TRAIL_MS) {
            trail_clear();
        }
        if (s_linger >= 0 && since(s_t_linger) >= T_LINGER_MS) {
            s_linger = -1;
            paint();
        }
        /* 10 s untouched: back to the locked face (the screen timeout still runs). */
        if (!s_busy && !s_st.down && since(s_t_touch) >= MUSE_PASS_KEYPAD_IDLE_MS) {
            cancel();
            goto out;
        }
    } else if (s_mode == S_WAIT) {
        if (muse_passcode_wait_left_ms() <= 0) {
            open_keypad(s_purpose, s_why);   /* over: the keypad */
        } else {
            wait_text();
        }
    } else if (s_mode == S_FORGOT) {
        if (s_reset_armed && since(s_t_reset) >= T_RESET_MS) {
            s_reset_armed = false;
            reset_text(false);
        }
        if (since(s_t_touch) >= MUSE_PASS_KEYPAD_IDLE_MS && lv_display_get_inactive_time(NULL) >= MUSE_PASS_KEYPAD_IDLE_MS) {
            open_keypad(s_purpose, s_why);
        }
    }
out:
    if (s_mode == S_NONE && !s_toast_up && !s_buzz2_due && s_timer) {
        lv_timer_delete(s_timer);
        s_timer = NULL;
    }
}

/* ---- public -------------------------------------------------------------- */

void muse_passcode_ui_build(void)
{
    int w = muse_board->width, h = muse_board->height;
    double ppm;
    if (muse_board->panel_w_mm > 0) {
        ppm = w / muse_board->panel_w_mm;
    } else if (muse_board->diagonal_in > 0) {
        double diag_px = muse_board->round ? (w < h ? w : h) : hypot(w, h);
        ppm = diag_px / (muse_board->diagonal_in * 25.4);
    } else {
        ppm = (w < h ? w : h) / 40.0;
    }
    muse_pass_geom_base(&s_g, w, h, muse_board->round, ppm);
    const muse_type_t *type = muse_type(s_g.s);
    s_f_prompt = type->label;   /* the prompt's capitals: the menu label's on the four boards (+-2 px) */
    s_f_small = type->value;
    s_f_digit = type->digit;
    s_f_title = type->title;
    s_prompt_caps = caps_of(s_f_prompt, 'H');
    s_small_caps = caps_of(s_f_small, 'H');
    s_digit_caps = caps_of(s_f_digit, '0');
    if (s_g.layout == MUSE_PASS_RING) {
        muse_pass_geom_ring(&s_g);
        muse_pass_geom_ring_prompt(&s_g, s_prompt_caps);
    } else {
        /* The grid never moves between prompts: fitted once for the longest. */
        static const char *const PROMPTS[] = { "Choose a passcode", "Enter it again", "Enter passcode",
                                               "Wrong passcode",    "Didn't match",   "Unlock to talk",
                                               "Unlock to update",  "Unlock to call",  "Call from phone" };
        int top = s_g.ring + s_g.m + s_g.gap, lines = 1;
        for (size_t i = 0; i < sizeof(PROMPTS) / sizeof(PROMPTS[0]); i++) {
            if (text_w(s_f_prompt, PROMPTS[i]) > room(top, s_prompt_caps)) lines = 2;
        }
        muse_pass_geom_grid(&s_g, lines, s_prompt_caps);
    }
    s_catch = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_catch);
    lv_obj_set_size(s_catch, w, h);
    lv_obj_remove_flag(s_catch, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_flag(s_catch, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_catch, on_catch, LV_EVENT_PRESSED, NULL);

    s_toast = plain(lv_layer_top());
    lv_obj_set_size(s_toast, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(s_toast, lv_color_hex(COLOR_CARD), 0);
    lv_obj_set_style_bg_opa(s_toast, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_toast, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_hor(s_toast, s_prompt_caps, 0);
    lv_obj_set_style_pad_ver(s_toast, s_prompt_caps / 2, 0);
    lv_obj_t *tl = lv_label_create(s_toast);
    lv_obj_set_style_text_font(tl, s_f_prompt, 0);
    lv_obj_set_style_text_color(tl, lv_color_hex(COLOR_TEXT), 0);
    lv_label_set_text(tl, "");
    lv_obj_align(s_toast, LV_ALIGN_CENTER, 0, (int)lround(0.3 * s_g.s));
    lv_obj_add_flag(s_toast, LV_OBJ_FLAG_HIDDEN);

    s_was_locked = false;
    s_built = true;
    ESP_LOGI(TAG, "keypad: %s, keys %.1f mm, digits %d px, prompt %d px", s_g.layout == MUSE_PASS_RING ? "ring" : "grid",
             s_g.key_mm, s_digit_caps, s_prompt_caps);
}

void muse_passcode_ui_tick(bool face)
{
    if (!s_built) return;
    s_face = face;
    bool locked = muse_passcode_locked();
    if (locked != s_was_locked) {
        s_was_locked = locked;
        if (locked) {
            /* Settings flows end; the menu, Status and Settings close. */
            if (s_flow) finish(false);
            close_screen();
            muse_menu_close();
            muse_settings_ui_go_home();
            muse_ui_show_face();
        } else if (!s_flow && !s_right_due) {
            close_screen();   /* unlocked (the pairing erased; a right code closes itself 150 ms on) */
            s_pending_why = 0;
        }
        muse_home_set_locked(locked);
    }
    int why = muse_passcode_take_keypad_request();
    if (why) s_pending_why = why;
    if (s_pending_why && !locked) s_pending_why = 0;
    if (s_pending_why && locked && face) {
        if (s_mode == S_NONE) {
            open_keypad(P_UNLOCK, s_pending_why);
        } else if (s_mode == S_KEYPAD && s_purpose == P_UNLOCK && s_n == 0 && !s_wrong_phase) {
            s_why = s_pending_why;   /* the press says why */
            set_prompt(purpose_prompt(), COLOR_TEXT);
        }
        s_pending_why = 0;
    }
    bool catcher = locked && s_mode == S_NONE && face;
    if (catcher == lv_obj_has_flag(s_catch, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_set_flag(s_catch, LV_OBJ_FLAG_HIDDEN, !catcher);
    }
}

void muse_passcode_ui_screen_off(void)
{
    if (!s_built) return;
    s_pending_why = 0;
    if (s_flow) {
        finish(false);
    } else if (s_mode != S_NONE && !s_right_due) {
        close_screen();
    }
}

bool muse_passcode_ui_locked_face(void)
{
    return s_built && s_was_locked;
}

bool muse_passcode_ui_covering(void)
{
    return s_mode != S_NONE;
}

void muse_passcode_ui_flow(muse_pass_flow_t flow, void (*done)(bool changed))
{
    if (!s_built) {
        if (done) done(false);
        return;
    }
    s_done = done;
    s_flow = true;
    memset(s_first, 0, sizeof(s_first));
    open_keypad(flow == MUSE_PASS_FLOW_SETUP ? P_CHOOSE : flow == MUSE_PASS_FLOW_CHANGE ? P_VERIFY_CHANGE : P_VERIFY_OFF,
                MUSE_PASS_WHY_TOUCH);
}

const char *muse_passcode_ui_page_name(void)
{
    switch (s_mode) {
    case S_KEYPAD:
        return "keypad";
    case S_WAIT:
        return "wait";
    case S_FORGOT:
        return "forgot";
    default:
        return s_built && s_was_locked && s_face ? "locked" : NULL;
    }
}

int muse_passcode_ui_bench_path(const char *keys, int *xs, int *ys, int max)
{
    if (!s_built || s_mode != S_KEYPAD) return 0;
    return muse_pass_bench_path(&s_g, keys, xs, ys, max);
}

bool muse_passcode_ui_bench_point(const char *what, muse_touch_gesture_t *g)
{
    if (!s_built) return false;
    double x = -1, y = -1;
    if (s_mode == S_KEYPAD && what[0] >= '0' && what[0] <= '9' && !what[1]) {
        x = s_g.key_x[what[0] - '0'];
        y = s_g.key_y[what[0] - '0'];
    } else if (s_mode == S_KEYPAD && !strcmp(what, "del")) {
        x = s_g.layout == MUSE_PASS_RING ? s_g.cx : s_g.key_x[MUSE_PASS_KEY_DEL];
        y = s_g.layout == MUSE_PASS_RING ? s_g.cy : s_g.key_y[MUSE_PASS_KEY_DEL];
    } else if (s_mode == S_WAIT && !strcmp(what, "forgot") && s_forgot_card) {
        return muse_touch_tap_obj(s_forgot_card, MUSE_TOUCH_TAP_MS, g);
    } else if (s_mode == S_FORGOT && !strcmp(what, "reset") && s_reset_card) {
        return muse_touch_tap_obj(s_reset_card, MUSE_TOUCH_TAP_MS, g);
    }
    if (x < 0) return false;
    g->x1 = g->x2 = (int)lround(x);
    g->y1 = g->y2 = (int)lround(y);
    g->ms = MUSE_TOUCH_TAP_MS;
    return true;
}
