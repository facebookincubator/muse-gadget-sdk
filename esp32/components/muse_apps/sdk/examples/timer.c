/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * SPDX-License-Identifier: Apache-2.0
 */

/* One foreground countdown. The three rollers set hours, minutes and seconds;
 * the clock, not callback frequency, determines the remaining time. */
#include "../gadget.h"

GADGET_MANIFEST("{\"api\":\"1.1\",\"name\":\"timer\",\"title\":\"Timer\",\"icon\":\"timer\",\"color\":\"yellow\",\"requires\":[\"touch\"]}");

#define BG 0x0C0E12
#define SURFACE 0x1C2029
#define TEXT 0xF3F1E9
#define DIM 0x8D929F
#define MUTED 0x555B69
#define ACCENT 0xEBC779
#define CLOCK_TIMER 1

enum { SETTING, COUNTING, FINISHED };
static int32_t phase;
static int32_t picker, wheels[3], action, action_text, heading, time_text, detail, close_button;
static uint32_t selected_s, started_ms;
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
}

static void set_picker(void)
{
    phase = SETTING;
    sys_timer_stop(CLOCK_TIMER);
    sys_keep_awake(0);
    ui_set(wheels[0], P_SELECTED, (int32_t)(selected_s / 3600));
    ui_set(wheels[1], P_SELECTED, (int32_t)(selected_s / 60 % 60));
    ui_set(wheels[2], P_SELECTED, (int32_t)(selected_s % 60));
    ui_set(picker, P_HIDDEN, 0);
    ui_set(time_text, P_HIDDEN, 1);
    ui_set(action, P_HIDDEN, 0);
    ui_set(action, P_DISABLED, !selected_s);
    ui_set_text(heading, P_TEXT, "TIMER");
    ui_set_text(action_text, P_TEXT, "Start");
    ui_set_text(detail, P_TEXT, "Scroll to set duration");
}

static void update_countdown(void)
{
    if (phase != COUNTING) return;
    /* Unsigned subtraction also crosses sys_millis()'s signed/unsigned wrap.
     * The longest allowed interval is less than one day. */
    uint32_t elapsed_ms = (uint32_t)sys_millis() - started_ms;
    uint32_t total_ms = selected_s * 1000;
    if (elapsed_ms >= total_ms) {
        phase = FINISHED;
        sys_timer_stop(CLOCK_TIMER);
        show_seconds(0);
        ui_set_text(heading, P_TEXT, "TIME'S UP");
        ui_set_text(detail, P_TEXT, "");
        ui_set_text(action_text, P_TEXT, "Set again");
        ui_set(action, P_DISABLED, 0);
        ui_set(action, P_HIDDEN, 0);
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
    ui_set_text(heading, P_TEXT, "COUNTING DOWN");
    ui_set_text(detail, P_TEXT, "");
    sys_keep_awake(1);
    show_seconds(seconds);
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
    shown_s = -1;
    sys_frame_rate(0);   /* Native roller animations and the clock need no frames. */
    ui_set(UI_ROOT, P_BG_COLOR, BG);
    ui_set(UI_ROOT, P_SCROLLABLE, 0);

    heading = label(UI_ROOT, FONT_20, DIM, -side * 34 / 100);
    time_text = label(UI_ROOT, font, TEXT, -side / 30);
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
    ui_set(action, P_WIDTH, side * 48 / 100);
    ui_set(action, P_HEIGHT, side >= 360 ? 52 : 42);
    ui_set(action, P_BG_COLOR, ACCENT);
    ui_set(action, P_RADIUS, RADIUS_CIRCLE);
    ui_align(action, ALIGN_CENTER, 0, side * 34 / 100);
    action_text = label(action, FONT_20, BG, 0);
    ui_on(action, UI_CLICKED);

    close_button = ui_create(W_BUTTON, UI_ROOT);
    ui_set(close_button, P_WIDTH, 40);
    ui_set(close_button, P_HEIGHT, 40);
    ui_set(close_button, P_BG_COLOR, SURFACE);
    ui_set(close_button, P_RADIUS, RADIUS_CIRCLE);
    ui_align(close_button, ALIGN_CENTER, side * 28 / 100, -side * 34 / 100);
    int32_t close_text = label(close_button, FONT_20, TEXT, 0);
    ui_set_text(close_text, P_TEXT, SYM_CLOSE);
    ui_on(close_button, UI_CLICKED);
    set_picker();
}

GADGET_EXPORT(on_event) void on_event(int32_t type, int32_t a, int32_t b)
{
    if (type == EV_TIMER && a == CLOCK_TIMER) {
        update_countdown();
    } else if (type == EV_UI) {
        if (a == close_button && b == UI_CLICKED) {
            sys_exit();
        } else if (a == action && b == UI_CLICKED) {
            if (phase == SETTING) start_countdown();
            else if (phase == FINISHED) set_picker();
        } else if (phase == SETTING && (b == UI_VALUE_CHANGED || b == UI_SCROLL_END)) {
            for (int32_t i = 0; i < 3; i++) {
                if (a == wheels[i]) ui_set(action, P_DISABLED, !picker_seconds());
            }
        }
    } else if (type == EV_SCREEN && a) {
        update_countdown();
    }
}

GADGET_EXPORT(on_stop) void on_stop(int32_t reason)
{
    (void)reason;
    sys_timer_stop(CLOCK_TIMER);
    sys_keep_awake(0);
}
