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
 * A dashboard written against API 1.0, to try its shape: widgets and styles,
 * devices (the battery, the accelerometer, the vibration motor, and a soil
 * probe that isn't a well-known device and may not be there), Muse's latest
 * reply, a button that messages Muse, and a count kept between runs.
 * Build from the SDK folder: python3 build.py examples/dashboard.c.
 */

#include "../gadget.h"

GADGET_MANIFEST("{\"api\":\"1.0\",\"name\":\"dashboard\",\"uses\":[\"chat.read\",\"chat.send\",\"battery\",\"accel\",\"vibration\",\"soil_moisture\"]}");

#define ACCENT 0x00C8FF

static int32_t battery_arc, battery_label, motion_chart, soil_label, reply_label, ask_button;
static int32_t battery, accel, buzzer, soil, soil_field, series, load_op;
static int32_t presses;            /* kept on the VM as state "presses" */
static char text[256];
static gadget_reading_t readings[16];

static void show_battery(void)
{
    gadget_reading_t r;
    if (battery <= 0 || dev_read(battery, &r, sizeof r) < 0) {
        ui_set_text(battery_label, P_TEXT, SYM_CHARGE);
        return;
    }
    ui_animate(battery_arc, P_VALUE, (int32_t)r.v[DEV_BATTERY_PERCENT], 400, EASE_OUT);
    ui_set_text(battery_label, P_TEXT, r.v[DEV_BATTERY_CHARGING] > 0 ? SYM_CHARGE : SYM_BATTERY_FULL);
}

/* "Soil: 42%" without libc. */
static void soil_text(int32_t pct)
{
    static const char prefix[] = "Soil: ";
    char *p = text;
    for (const char *s = prefix; *s; s++)
        *p++ = *s;
    if (pct >= 100)
        *p++ = '1';
    if (pct >= 10)
        *p++ = (char)('0' + pct / 10 % 10);
    *p++ = (char)('0' + pct % 10);
    *p++ = '%';
    *p = 0;
}

/* A device only some gadgets have, found by name; its description told Muse
 * the field and its unit. */
static void show_soil(void)
{
    gadget_reading_t r;
    if (soil > 0 && dev_read(soil, &r, sizeof r) > soil_field) {
        soil_text((int32_t)r.v[soil_field]);
        ui_set_text(soil_label, P_TEXT, text);
    }
}

static void show_reply(void)
{
    int32_t id = chat_latest(0);
    if (id && chat_info(id, CHAT_INFO_ROLE) == ROLE_MUSE) {
        chat_text(id, 0, text, sizeof text);
        ui_set_text(reply_label, P_TEXT, text);
    }
}

GADGET_EXPORT(on_start) void on_start(void)
{
    int32_t inset = sys_info(INFO_SAFE_INSET);

    /* One column of rows, inside the part of a round screen that shows. */
    int32_t col = ui_create(W_OBJ, UI_ROOT);
    ui_set(col, P_WIDTH, PCT(100));
    ui_set(col, P_HEIGHT, PCT(100));
    ui_set(col, P_PAD_ALL, inset);
    ui_set(col, P_BG_OPA, OPA_TRANSP);
    ui_set(col, P_BORDER_WIDTH, 0);
    ui_set(col, P_LAYOUT, LAYOUT_FLEX);
    ui_set(col, P_FLEX_FLOW, FLEX_COLUMN);
    ui_set(col, P_FLEX_CROSS_PLACE, PLACE_CENTER);
    ui_set(col, P_PAD_ROW, 6);

    battery_arc = ui_create(W_ARC, col);
    ui_set(battery_arc, P_WIDTH, 72);
    ui_set(battery_arc, P_HEIGHT, 72);
    ui_set(battery_arc, P_CLICKABLE, 0);
    ui_style(battery_arc, PART_INDICATOR, P_ARC_COLOR, ACCENT);
    ui_style(battery_arc, PART_KNOB, P_BG_OPA, OPA_TRANSP);
    battery_label = ui_create(W_LABEL, battery_arc);
    ui_align(battery_label, ALIGN_CENTER, 0, 0);

    motion_chart = ui_create(W_CHART, col);
    ui_set(motion_chart, P_WIDTH, PCT(100));
    ui_set(motion_chart, P_HEIGHT, 50);
    ui_set(motion_chart, P_POINT_COUNT, 40);
    ui_set(motion_chart, P_MIN_VALUE, 0);
    ui_set(motion_chart, P_MAX_VALUE, 2000);   /* milli-g */
    series = ui_chart_series(motion_chart, ACCENT);

    soil_label = ui_create(W_LABEL, col);
    ui_set(soil_label, P_HIDDEN, 1);

    reply_label = ui_create(W_LABEL, col);
    ui_set(reply_label, P_WIDTH, PCT(100));
    ui_set(reply_label, P_FLEX_GROW, 1);
    ui_set(reply_label, P_LONG_MODE, LONG_DOTS);
    ui_set(reply_label, P_TEXT_FONT, FONT_14);
    ui_set_text(reply_label, P_TEXT, "Ask Muse something");

    ask_button = ui_create(W_BUTTON, col);
    ui_on(ask_button, UI_CLICKED);
    int32_t ask_label = ui_create(W_LABEL, ask_button);
    ui_set_text(ask_label, P_TEXT, SYM_ENVELOPE " Say hi");

    /* Devices: every one is optional here; dev_find returns an error when
     * the gadget doesn't have it. */
    battery = dev_find(DEV_BATTERY);
    accel = dev_find(DEV_ACCEL);
    buzzer = dev_find(DEV_VIBRATION);
    soil = dev_find("soil_moisture");
    if (accel > 0)
        dev_open(accel, 20);
    if (soil > 0 && (soil_field = dev_field(soil, "moisture")) >= 0) {
        ui_set(soil_label, P_HIDDEN, 0);
        show_soil();
    }
    show_battery();
    sys_timer(1, 30000, 1);
    chat_open();
    load_op = state_load("presses", &presses, sizeof presses);
}

GADGET_EXPORT(on_event) void on_event(int32_t type, int32_t a, int32_t b)
{
    switch (type) {
    case EV_TIMER:
        show_battery();
        show_soil();
        break;
    case EV_DEV_DATA: {
        int32_t n = dev_take(a, readings, sizeof readings);
        for (int32_t i = 0; i < n; i++) {
            float x = readings[i].v[DEV_ACCEL_X], y = readings[i].v[DEV_ACCEL_Y], z = readings[i].v[DEV_ACCEL_Z];
            float m = (x < 0 ? -x : x) + (y < 0 ? -y : y) + (z < 0 ? -z : z);
            ui_chart_push(motion_chart, series, (int32_t)(m * 1000));
        }
        break;
    }
    case EV_DEV_EVENT:
        if (a == accel && b == DEV_ACCEL_SHAKE)
            ui_set_text(reply_label, P_TEXT, "Shaken!");
        break;
    case EV_CHAT:
        if (a == CHAT_TEXT || a == CHAT_DONE)
            show_reply();
        break;
    case EV_UI:
        if (a == ask_button && b == UI_CLICKED && chat_send("Hi from the dashboard") > 0) {
            static const float buzz_ms[] = {30};
            presses++;
            if (buzzer > 0)
                dev_call(buzzer, DEV_VIBRATION_BUZZ, buzz_ms, sizeof buzz_ms, "");
        }
        break;
    case EV_DONE:
        if (a == load_op && b < 0)
            presses = 0;
        break;
    }
}

GADGET_EXPORT(on_stop) void on_stop(int32_t reason)
{
    (void)reason;
    if (accel > 0)
        dev_close(accel);
    state_save("presses", &presses, sizeof presses);
}
