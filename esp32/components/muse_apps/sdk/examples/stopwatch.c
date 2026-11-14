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
 * A stopwatch: the time in a big label inside a ring that fills once a minute.
 * A tap or the talk button starts and stops it; the second button (or, on a
 * touch screen without one, the button on screen) takes a lap while it runs
 * and resets it while it's stopped. Paused, the time blinks; a new lap slides
 * in. Laid out from the screen's size, so it fits any gadget.
 *
 * Widgets, styles and animations draw it; on_frame only updates the numbers,
 * and only while it runs. Build from the SDK folder:
 * python3 build.py examples/stopwatch.c
 */
#include "../gadget.h"

GADGET_MANIFEST("{\"api\":\"1.0\",\"name\":\"stopwatch\",\"title\":\"Stopwatch\",\"icon\":\"timer\",\"color\":\"mint\"}");

#define BG 0x121214
#define RING 0x23232B
#define SAGE 0x5FA87D
#define LAV 0x8E8CA8
#define PALE 0xD8D5CC
#define GOLD 0xC7A252
#define SLATE 0x5E7FA6

#define MAX_LAPS 3

static int32_t ring, time_label, cents_label, state_label, lap_button, lap_button_label;
static int32_t lap_labels[MAX_LAPS], n_lap_labels;
static int32_t touch;
static int32_t running, started_at, banked, last_lap_total, lap_count, laps[MAX_LAPS];

static int32_t elapsed(void)
{
    return banked + (running ? sys_millis() - started_at : 0);
}

/* Numbers as text: no snprintf (libc's stdio doesn't belong in apps). */
static char *put_u(char *p, int32_t v, int32_t width)
{
    char tmp[10];
    int32_t n = 0;
    do {
        tmp[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v && n < 10);
    while (n < width) tmp[n++] = '0';
    while (n) *p++ = tmp[--n];
    return p;
}

static char *put_s(char *p, const char *s)
{
    while (*s) *p++ = *s++;
    return p;
}

/* "MM:SS" */
static char *put_minutes(char *p, int32_t ms)
{
    p = put_u(p, ms / 60000 % 100, 2);
    *p++ = ':';
    return put_u(p, ms / 1000 % 60, 2);
}

/* The numbers and the ring, as they are now. */
static void show_time(void)
{
    int32_t e = elapsed();
    char buf[16], *p;
    *put_minutes(buf, e) = '\0';
    ui_set_text(time_label, P_TEXT, buf);
    p = buf;
    *p++ = '.';
    *put_u(p, e / 10 % 100, 2) = '\0';
    ui_set_text(cents_label, P_TEXT, buf);
    ui_set(ring, P_VALUE, e % 60000 / 100);   /* tenths of a second into the minute */
}

static void show_laps(void)
{
    char buf[32], *p;
    for (int32_t i = 0; i < n_lap_labels; i++) {
        if (i >= lap_count) {
            ui_set_text(lap_labels[i], P_TEXT, "");
            continue;
        }
        p = put_s(buf, "LAP ");
        p = put_u(p, lap_count - i, 1);
        p = put_s(p, "   ");
        p = put_minutes(p, laps[i]);
        *p++ = '.';
        *put_u(p, laps[i] / 10 % 100, 2) = '\0';
        ui_set_text(lap_labels[i], P_TEXT, buf);
    }
}

/* Everything that changes when it starts, stops or resets. */
static void show_state(void)
{
    int32_t e = elapsed();
    int32_t accent = running ? SAGE : e ? GOLD : LAV;
    ui_style(ring, PART_INDICATOR, P_ARC_COLOR, accent);
    ui_set(cents_label, P_TEXT_COLOR, accent);
    ui_set(state_label, P_TEXT_COLOR, accent);
    ui_set_text(state_label, P_TEXT, running ? "RUNNING" : e ? "PAUSED" : touch ? "TAP TO START" : "PRESS TO START");
    if (!running && e) {
        ui_animate(time_label, P_OPA, 70, 700, EASE_IN_OUT | ANIM_REPEAT | ANIM_REVERSE);
    } else {
        ui_animate_stop(time_label, P_OPA);
        ui_set(time_label, P_OPA, OPA_COVER);
    }
    if (lap_button) {
        ui_set_text(lap_button_label, P_TEXT, running ? "LAP" : "RESET");
        ui_set(lap_button, P_DISABLED, !running && !e);
    }
    sys_frame_rate(running ? 30 : 0);   /* on_frame only while the numbers move */
    sys_keep_awake(running);
    show_time();
}

static void start_stop(void)
{
    int32_t now = sys_millis();
    if (running) {
        banked += now - started_at;
    } else {
        started_at = now;
    }
    running = !running;
    show_state();
}

static void lap_reset(void)
{
    if (running) {
        int32_t total = elapsed();
        for (int32_t i = MAX_LAPS - 1; i > 0; i--) laps[i] = laps[i - 1];
        laps[0] = total - last_lap_total;
        last_lap_total = total;
        lap_count++;
        show_laps();
        if (n_lap_labels) {   /* the new lap slides in */
            ui_set(lap_labels[0], P_TRANSLATE_Y, -12);
            ui_set(lap_labels[0], P_OPA, OPA_TRANSP);
            ui_animate(lap_labels[0], P_TRANSLATE_Y, 0, 250, EASE_OUT);
            ui_animate(lap_labels[0], P_OPA, OPA_COVER, 250, EASE_OUT);
        }
    } else {
        banked = last_lap_total = lap_count = 0;
        show_laps();
        show_state();
    }
}

static int32_t label(int32_t font, int32_t color, int32_t dy)
{
    int32_t l = ui_create(W_LABEL, UI_ROOT);
    ui_set(l, P_TEXT_FONT, font);
    ui_set(l, P_TEXT_COLOR, color);
    ui_align(l, ALIGN_CENTER, 0, dy);
    return l;
}

GADGET_EXPORT(on_start) void on_start(void)
{
    int32_t w = sys_info(INFO_SCREEN_WIDTH), h = sys_info(INFO_SCREEN_HEIGHT);
    int32_t side = w < h ? w : h;
    int32_t buttons = sys_info(INFO_BUTTONS);
    touch = sys_info(INFO_TOUCH) == 1;
    ui_set(UI_ROOT, P_BG_COLOR, BG);

    /* A ring around the screen, filling clockwise from 12 o'clock once a minute. */
    ring = ui_create(W_ARC, UI_ROOT);
    ui_set(ring, P_WIDTH, side - 16);
    ui_set(ring, P_HEIGHT, side - 16);
    ui_align(ring, ALIGN_CENTER, 0, 0);
    ui_set(ring, P_CLICKABLE, 0);
    ui_set(ring, P_ROTATION, 270);
    ui_set(ring, P_BG_START_ANGLE, 0);
    ui_set(ring, P_BG_END_ANGLE, 360);
    ui_set(ring, P_MIN_VALUE, 0);
    ui_set(ring, P_MAX_VALUE, 600);
    ui_set(ring, P_ARC_COLOR, RING);
    ui_set(ring, P_ARC_WIDTH, side / 30);
    ui_style(ring, PART_INDICATOR, P_ARC_WIDTH, side / 30);
    ui_style(ring, PART_INDICATOR, P_ARC_ROUNDED, 1);
    ui_style(ring, PART_KNOB, P_BG_OPA, OPA_TRANSP);
    ui_style(ring, PART_KNOB, P_PAD_ALL, 0);

    /* The labels, placed from the middle. */
    int32_t mid = -side / 20;
    time_label = label(FONT_48, PALE, mid);
    cents_label = label(FONT_28, LAV, mid + 44);
    state_label = label(FONT_14, LAV, mid - 50);

    /* A button on screen when there's touch but no second button. */
    int32_t bottom = side / 2 - sys_info(INFO_SAFE_INSET) - 12;
    if (touch && !(buttons & (1 << BUTTON_SECONDARY))) {
        lap_button = ui_create(W_BUTTON, UI_ROOT);
        ui_set(lap_button, P_BG_COLOR, SLATE);
        ui_set(lap_button, P_RADIUS, RADIUS_CIRCLE);
        ui_set(lap_button, P_PAD_LEFT, 18);
        ui_set(lap_button, P_PAD_RIGHT, 18);
        ui_align(lap_button, ALIGN_CENTER, 0, bottom - 18);
        ui_on(lap_button, UI_CLICKED);
        lap_button_label = ui_create(W_LABEL, lap_button);
        ui_set(lap_button_label, P_TEXT_COLOR, PALE);
        bottom -= 44;
    }

    /* As many laps as fit between the numbers and the bottom. */
    for (int32_t y = mid + 84; n_lap_labels < MAX_LAPS && y <= bottom; y += 20) {
        lap_labels[n_lap_labels] = label(FONT_14, n_lap_labels ? LAV : PALE, y);
        n_lap_labels++;
    }

    show_laps();
    show_state();
}

GADGET_EXPORT(on_event) void on_event(int32_t type, int32_t a, int32_t b)
{
    switch (type) {
    case EV_TOUCH_DOWN:
        start_stop();
        break;
    case EV_BUTTON:
        if (b != BTN_DOWN) {
            break;
        }
        if (a == BUTTON_PRIMARY || a == BUTTON_ENTER) {
            start_stop();
        } else if (a == BUTTON_SECONDARY || a == BUTTON_BACK || a == BUTTON_DOWN) {
            lap_reset();
        }
        break;
    case EV_UI:
        if (a == lap_button && b == UI_CLICKED) {
            lap_reset();
        }
        break;
    case EV_SCREEN:
        if (a) {
            show_time();   /* no frames while the screen was off */
        }
        break;
    }
}

GADGET_EXPORT(on_frame) void on_frame(int32_t now_ms)
{
    (void)now_ms;
    show_time();
}
