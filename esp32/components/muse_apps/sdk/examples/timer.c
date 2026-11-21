/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * SPDX-License-Identifier: Apache-2.0
 */

/* One foreground countdown. The three rollers set hours, minutes and seconds;
 * the clock, not callback frequency, determines the remaining time. */
#include "../gadget.h"

GADGET_MANIFEST("{\"api\":\"1.2\",\"name\":\"timer\",\"title\":\"Timer\",\"icon\":\"timer\",\"color\":\"yellow\",\"requires\":[\"touch\"]}");

#define BG 0x0C0E12
#define SURFACE 0x1C2029
#define TEXT 0xF3F1E9
#define DIM 0x8D929F
#define MUTED 0x555B69
#define ACCENT 0xEBC779
#define CLOCK_TIMER 1

enum { SETTING, COUNTING, PAUSED, FINISHED };
static int32_t phase;
static int32_t picker, wheels[3], action, action_text, heading, time_text, detail, close_button;
static int32_t ring, reset_hint;
static uint32_t selected_s, started_ms, remaining_ms, last_tap_ms, finished_ms, input_started_ms;
static int32_t resetting, tap_valid, touch_down, touch_moved, touch_x, touch_y;
#define RESET_TIMER 2
#define DOUBLE_TAP_MS 300

static void ring_state(int32_t state)
{
    ui_animate_stop(ring, P_OPA);
    ui_set(ring, P_HIDDEN, state == SETTING);
    ui_style(ring, PART_INDICATOR, P_ARC_COLOR, state == FINISHED ? 0xFF3030 : 0xFFD43B);
    ui_set(ring, P_OPA, state == PAUSED ? 60 : 255);
    if (state == COUNTING) {
        ui_set(ring, P_OPA, 0);
        ui_animate(ring, P_OPA, 255, 1000, EASE_IN_OUT | ANIM_REPEAT | ANIM_REVERSE);
    }
}

static void finish(void);
static int32_t shown_s;

static char *put_two(char *p, uint32_t n)
{
    *p++ = (char)('0' + n / 10);
    *p++ = (char)('0' + n % 10);
    return p;
}

static void options(char *buf, int32_t count)
{
    char *p = buf;
    for (int32_t i = 0; i < count; i++) {
        p = put_two(p, (uint32_t)i);
        if (i + 1 < count) *p++ = '\n';
    }
    *p = '\0';
}

static uint32_t picker_seconds(void)
{
    int32_t h = ui_get(wheels[0], P_SELECTED);
    int32_t m = ui_get(wheels[1], P_SELECTED);
    int32_t s = ui_get(wheels[2], P_SELECTED);
    if (h < 0 || h > 23 || m < 0 || m > 59 || s < 0 || s > 59) return 0;
    return (uint32_t)(h * 3600 + m * 60 + s);
}

static void show_seconds(uint32_t seconds)
{
    if ((int32_t)seconds == shown_s) return;
    shown_s = (int32_t)seconds;
    char text[9], *p = text;
    if (seconds >= 3600) {
        p = put_two(p, seconds / 3600);
        *p++ = ':';
    }
    p = put_two(p, seconds / 60 % 60);
    *p++ = ':';
    p = put_two(p, seconds % 60);
    *p = '\0';
    ui_set_text(time_text, P_TEXT, text);
    /* Fill the screen width, including the longer hours format, while leaving
     * room for the outer ring and the paused / finished heading. Layout sizes
     * are untransformed, so each update fits without accumulating scale. */
    int32_t w = sys_info(INFO_SCREEN_WIDTH), h = sys_info(INFO_SCREEN_HEIGHT);
    int32_t side = w < h ? w : h;
    int32_t tw = ui_get(time_text, P_WIDTH), th = ui_get(time_text, P_HEIGHT);
    if (tw > 0 && th > 0) {
        int32_t scale = side * 84 / tw;
        int32_t height_scale = side * 32 / th;
        if (scale > height_scale) scale = height_scale;
        ui_set(time_text, P_TRANSFORM_SCALE, scale);
    }
}

static void set_picker(void)
{
    phase = SETTING;
    sys_timer_stop(CLOCK_TIMER);
    sys_keep_awake(0);
    tap_valid = 0;
    ring_state(SETTING);
    ui_set(wheels[0], P_SELECTED, (int32_t)(selected_s / 3600));
    ui_set(wheels[1], P_SELECTED, (int32_t)(selected_s / 60 % 60));
    ui_set(wheels[2], P_SELECTED, (int32_t)(selected_s % 60));
    ui_set(picker, P_HIDDEN, 0);
    ui_set(time_text, P_HIDDEN, 1);
    ui_set(action, P_HIDDEN, 0);
    ui_set(action, P_DISABLED, !selected_s);
    ui_set(heading, P_HIDDEN, 1);
    ui_set(reset_hint, P_HIDDEN, 0);
    ui_set_text(action_text, P_TEXT, "Start");
    ui_set_text(detail, P_TEXT, "");
}

static void update_countdown(void)
{
    if (phase != COUNTING) return;
    /* Unsigned subtraction also crosses sys_millis()'s signed/unsigned wrap.
     * The longest allowed interval is less than one day. */
    uint32_t elapsed_ms = (uint32_t)sys_millis() - started_ms;
    uint32_t total_ms = remaining_ms;
    if (elapsed_ms >= total_ms) {
        finish();
        return;
    }
    /* Round up: don't show zero before the deadline. */
    show_seconds((total_ms - elapsed_ms + 999) / 1000);
}

static void start_countdown(void)
{
    if (phase != SETTING) return;
    uint32_t seconds = picker_seconds();
    if (!seconds) return;
    selected_s = seconds;
    /* Settle the rollers at the values actually used for this countdown. */
    ui_set(wheels[0], P_SELECTED, (int32_t)(seconds / 3600));
    ui_set(wheels[1], P_SELECTED, (int32_t)(seconds / 60 % 60));
    ui_set(wheels[2], P_SELECTED, (int32_t)(seconds % 60));
    remaining_ms = selected_s * 1000;
    input_started_ms = (uint32_t)sys_millis();
    started_ms = (uint32_t)sys_millis();
    if (sys_timer(CLOCK_TIMER, 100, 1) < 0) {
        ui_set_text(detail, P_TEXT, "Couldn't start. Try again.");
        return;
    }
    phase = COUNTING;
    shown_s = -1;
    ui_set(picker, P_HIDDEN, 1);
    ui_set(time_text, P_HIDDEN, 0);
    ui_set(action, P_HIDDEN, 1);
    ui_set(heading, P_HIDDEN, 1);
    ui_set(reset_hint, P_HIDDEN, 1);
    ring_state(COUNTING);
    tap_valid = 0;
    ui_set_text(detail, P_TEXT, "");
    sys_keep_awake(1);
    show_seconds(seconds);
}

static void finish(void)
{
    phase = FINISHED;
    finished_ms = (uint32_t)sys_millis();
    tap_valid = 0;
    sys_timer_stop(CLOCK_TIMER);
    show_seconds(0);
    ui_set(time_text, P_HIDDEN, 1);
    ui_align(heading, ALIGN_CENTER, 0, 0);
    ui_set_text(heading, P_TEXT, "Time's up");
    ui_set(heading, P_TRANSFORM_SCALE, 150);
    ui_set(heading, P_HIDDEN, 0);
    ui_set(action, P_HIDDEN, 1);
    ring_state(FINISHED);
}

static void countdown_tap(void)
{
    uint32_t now = (uint32_t)sys_millis();
    if (phase == FINISHED) {
        if (now - finished_ms >= 400) set_picker();
        return;
    }
    if (phase != COUNTING && phase != PAUSED) return;
    if (now - input_started_ms < 250) return;
    if (tap_valid && now - last_tap_ms <= DOUBLE_TAP_MS) {
        finish();
        return;
    }
    tap_valid = 1;
    last_tap_ms = now;
    if (phase == COUNTING) {
        update_countdown();
        if (phase != COUNTING) return;
        remaining_ms -= now - started_ms;
        phase = PAUSED;
        sys_timer_stop(CLOCK_TIMER);
        int32_t w = sys_info(INFO_SCREEN_WIDTH), h = sys_info(INFO_SCREEN_HEIGHT);
        int32_t side = w < h ? w : h;
        ui_align(heading, ALIGN_CENTER, 0, -side * 21 / 100);
        ui_set_text(heading, P_TEXT, "Paused");
        ui_set(heading, P_TRANSFORM_SCALE, 100);
        ui_set(heading, P_HIDDEN, 0);
        ring_state(PAUSED);
    } else {
        if (sys_timer(CLOCK_TIMER, 100, 1) < 0) return;
        started_ms = now;
        phase = COUNTING;
        ui_set(heading, P_HIDDEN, 1);
        ring_state(COUNTING);
    }
}

static void reset_picker(void)
{
    if (phase != SETTING || resetting) return;
    selected_s = 0;
    ui_set(action, P_DISABLED, 1);
    resetting = 1;
    for (int i = 0; i < 3; i++) {
        ui_set(wheels[i], P_DISABLED, 1);
        ui_set(wheels[i], P_ANIM_DURATION, 500);
        ui_roller_select(wheels[i], 0, 1);
    }
    if (sys_timer(RESET_TIMER, 550, 0) < 0) {
        resetting = 0;
        for (int i = 0; i < 3; i++) ui_set(wheels[i], P_DISABLED, 0);
    }
}

static int32_t label(int32_t parent, int32_t font, int32_t color, int32_t y)
{
    int32_t obj = ui_create(W_LABEL, parent);
    ui_set(obj, P_TEXT_FONT, font);
    ui_set(obj, P_TEXT_COLOR, color);
    ui_align(obj, ALIGN_CENTER, 0, y);
    return obj;
}

GADGET_EXPORT(on_start) void on_start(void)
{
    int32_t w = sys_info(INFO_SCREEN_WIDTH), h = sys_info(INFO_SCREEN_HEIGHT);
    int32_t side = w < h ? w : h;
    int32_t font = side >= 360 ? FONT_48 : FONT_28;
    selected_s = 0;
    resetting = tap_valid = touch_down = touch_moved = 0;
    shown_s = -1;
    sys_frame_rate(0);   /* Native roller animations and the clock need no frames. */
    ui_set(UI_ROOT, P_BG_COLOR, BG);
    ui_set(UI_ROOT, P_SCROLLABLE, 0);

    ring = ui_create(W_ARC, UI_ROOT);
    ui_set(ring, P_WIDTH, side - 10);
    ui_set(ring, P_HEIGHT, side - 10);
    ui_align(ring, ALIGN_CENTER, 0, 0);
    ui_set(ring, P_CLICKABLE, 0);
    ui_set(ring, P_MIN_VALUE, 0);
    ui_set(ring, P_MAX_VALUE, 100);
    ui_set(ring, P_BG_START_ANGLE, 0);
    ui_set(ring, P_BG_END_ANGLE, 360);
    ui_set(ring, P_VALUE, 100);
    ui_set(ring, P_ARC_OPA, 0);
    ui_style(ring, PART_INDICATOR, P_ARC_OPA, 255);
    ui_style(ring, PART_INDICATOR, P_ARC_WIDTH, 6);
    ui_style(ring, PART_KNOB, P_BG_OPA, 0);
    ui_style(ring, PART_KNOB, P_PAD_ALL, 0);
    heading = label(UI_ROOT, FONT_28, 0xFFFFFF, -side * 21 / 100);
    ui_set(heading, P_TRANSFORM_PIVOT_X, PCT(50));
    ui_set(heading, P_TRANSFORM_PIVOT_Y, PCT(50));
    time_text = label(UI_ROOT, font, TEXT, 0);
    ui_set(time_text, P_TRANSFORM_PIVOT_X, PCT(50));
    ui_set(time_text, P_TRANSFORM_PIVOT_Y, PCT(50));
    detail = label(UI_ROOT, FONT_14, DIM, side * 23 / 100);

    picker = ui_create(W_OBJ, UI_ROOT);
    ui_set(picker, P_WIDTH, w);
    ui_set(picker, P_HEIGHT, side * 53 / 100);
    ui_set(picker, P_BG_OPA, OPA_TRANSP);
    ui_set(picker, P_BORDER_WIDTH, 0);
    ui_set(picker, P_PAD_ALL, 0);
    ui_set(picker, P_SCROLLABLE, 0);
    ui_align(picker, ALIGN_CENTER, 0, -side / 30);

    int32_t band = ui_create(W_OBJ, picker);
    ui_set(band, P_WIDTH, side * 76 / 100);
    ui_set(band, P_HEIGHT, side >= 360 ? 64 : 44);
    ui_set(band, P_BG_COLOR, SURFACE);
    ui_set(band, P_BORDER_WIDTH, 0);
    ui_set(band, P_RADIUS, 14);
    ui_set(band, P_CLICKABLE, 0);
    ui_set(band, P_SCROLLABLE, 0);
    ui_align(band, ALIGN_CENTER, 0, 0);

    const char *units[] = { "HOURS", "MIN", "SEC" };
    char values[180];
    for (int32_t i = 0; i < 3; i++) {
        int32_t x = (i - 1) * side * 24 / 100;
        int32_t unit = label(UI_ROOT, FONT_14, DIM, -side * 27 / 100);
        ui_set_text(unit, P_TEXT, units[i]);
        ui_align(unit, ALIGN_CENTER, x, -side * 27 / 100);
        /* Unit headings belong to the picker too: hide them with it. */
        ui_set_parent(unit, picker);
        ui_align(unit, ALIGN_CENTER, x, -side * 235 / 1000);

        int32_t roller = wheels[i] = ui_create(W_ROLLER, picker);
        ui_set(roller, P_WIDTH, side * 22 / 100);
        ui_set(roller, P_BG_OPA, OPA_TRANSP);
        ui_set(roller, P_BORDER_WIDTH, 0);
        ui_set(roller, P_PAD_ALL, 0);
        ui_set(roller, P_TEXT_FONT, font);
        ui_set(roller, P_TEXT_COLOR, MUTED);
        ui_set(roller, P_TEXT_ALIGN, TEXT_ALIGN_CENTER);
        ui_set(roller, P_TEXT_LINE_SPACE, side >= 360 ? 12 : 6);
        ui_set(roller, P_ANIM_DURATION, 160);
        ui_style(roller, PART_SELECTED, P_BG_OPA, OPA_TRANSP);
        ui_style(roller, PART_SELECTED, P_TEXT_COLOR, TEXT);
        ui_style(roller, PART_SELECTED, P_TEXT_FONT, font);
        options(values, i == 0 ? 24 : 60);
        ui_set_text(roller, P_OPTIONS, values);
        ui_set(roller, P_INFINITE, 1);
        ui_set(roller, P_VISIBLE_ROWS, 3);
        ui_align(roller, ALIGN_CENTER, x, 0);
        ui_on(roller, UI_VALUE_CHANGED | UI_SCROLL_END);
    }

    action = ui_create(W_BUTTON, UI_ROOT);
    ui_set(action, P_WIDTH, side * 72 / 100);
    ui_set(action, P_HEIGHT, side >= 360 ? 78 : 60);
    ui_set(action, P_BG_COLOR, ACCENT);
    ui_set(action, P_RADIUS, RADIUS_CIRCLE);
    ui_align(action, ALIGN_CENTER, 0, side * 28 / 100);
    action_text = label(action, FONT_28, BG, 0);
    ui_on(action, UI_CLICKED);

    close_button = ui_create(W_BUTTON, UI_ROOT);
    ui_set(close_button, P_WIDTH, 48);
    ui_set(close_button, P_HEIGHT, 48);
    ui_set(close_button, P_BG_COLOR, SURFACE);
    ui_set(close_button, P_RADIUS, RADIUS_CIRCLE);
    ui_align(close_button, ALIGN_CENTER, -side * 28 / 100, -side * 34 / 100);
    int32_t close_text = label(close_button, FONT_20, TEXT, 0);
    ui_set_text(close_text, P_TEXT, SYM_LEFT);
    ui_on(close_button, UI_CLICKED);
    reset_hint = ui_create(W_CANVAS, UI_ROOT);
    ui_set(reset_hint, P_WIDTH, 32);
    ui_set(reset_hint, P_HEIGHT, 32);
    ui_set(reset_hint, P_CLICKABLE, 0);
    ui_align(reset_hint, ALIGN_CENTER, side * 28 / 100, -side * 34 / 100);
    cv_fill(reset_hint, BG);
    cv_arc(reset_hint, 16, 16, 12, 2, -60, 240, TEXT);
    cv_line(reset_hint, 16, 16, 16, 9, 2, TEXT);
    cv_line(reset_hint, 16, 16, 22, 16, 2, TEXT);
    cv_line(reset_hint, 5, 5, 5, 12, 2, TEXT);
    cv_line(reset_hint, 5, 5, 12, 5, 2, TEXT);
    set_picker();
}

static void request_control(int exit)
{
    if (exit) sys_exit();
    else if (phase == SETTING) reset_picker();
    else set_picker();
}

GADGET_EXPORT(on_event) void on_event(int32_t type, int32_t a, int32_t b)
{
    if (type == EV_TIMER && a == CLOCK_TIMER) update_countdown();
    else if (type == EV_TIMER && a == RESET_TIMER) {
        resetting = 0;
        for (int i = 0; i < 3; i++) {
            ui_set(wheels[i], P_SELECTED, 0);
            ui_set(wheels[i], P_DISABLED, 0);
        }
    } else if (type == EV_BUTTON && b == BTN_DOWN) {
        if (a == BUTTON_SECONDARY || a == BUTTON_BACK) request_control(1);
        else if (a == BUTTON_PRIMARY) request_control(0);
    } else if (type == EV_TOUCH_DOWN) {
        touch_down = 1; touch_moved = 0; touch_x = a; touch_y = b;
    } else if (type == EV_TOUCH_MOVE && touch_down) {
        if (a - touch_x > 12 || touch_x - a > 12 || b - touch_y > 12 || touch_y - b > 12) touch_moved = 1;
    } else if (type == EV_TOUCH_UP) {
        int tap = touch_down && !touch_moved;
        touch_down = 0;
        if (tap) countdown_tap();
    } else if (type == EV_UI) {
        if (a == close_button && b == UI_CLICKED) request_control(1);
        else if (a == action && b == UI_CLICKED) {
            if (phase == SETTING && !resetting) start_countdown();
        } else if (phase == SETTING && !resetting && (b == UI_VALUE_CHANGED || b == UI_SCROLL_END)) {
            for (int i = 0; i < 3; i++) if (a == wheels[i]) ui_set(action, P_DISABLED, !picker_seconds());
        }
    } else if (type == EV_SCREEN && a) update_countdown();
}

GADGET_EXPORT(on_stop) void on_stop(int32_t reason)
{
    (void)reason;
    sys_timer_stop(CLOCK_TIMER);
    sys_timer_stop(RESET_TIMER);
    sys_keep_awake(0);
}
