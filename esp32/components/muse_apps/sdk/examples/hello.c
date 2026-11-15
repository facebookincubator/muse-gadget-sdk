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
 * The smallest useful gadget app: a greeting, a spinner and the seconds it has
 * run. A tap or a button press changes the colour. Widgets do the drawing, a
 * timer does the counting: no on_frame. Build from the SDK folder:
 * python3 build.py examples/hello.c
 */
#include "../gadget.h"

GADGET_MANIFEST("{\"api\":\"1.0\",\"name\":\"hello\",\"title\":\"Hello\",\"icon\":\"smile\",\"color\":\"green\"}");

static const int32_t colors[] = {0x5FA87D, 0x5E7FA6, 0xC7A252, 0xC05A50};
static int32_t spinner, count, color, seconds;

/* The count as text, without snprintf (libc's stdio doesn't belong in apps). */
static void show_seconds(void)
{
    char buf[12], *p = buf + sizeof buf - 1;
    int32_t s = seconds;
    *p = '\0';
    do *--p = (char)('0' + s % 10); while ((s /= 10) && p > buf);
    ui_set_text(count, P_TEXT, p);
}

static void show_color(void)
{
    ui_style(spinner, PART_INDICATOR, P_ARC_COLOR, colors[color]);
    ui_set(count, P_TEXT_COLOR, colors[color]);
}

GADGET_EXPORT(on_start) void on_start(void)
{
    int32_t w = sys_info(INFO_SCREEN_WIDTH), h = sys_info(INFO_SCREEN_HEIGHT);
    int32_t size = (w < h ? w : h) * 3 / 4;
    ui_set(UI_ROOT, P_BG_COLOR, 0x121214);

    spinner = ui_create(W_SPINNER, UI_ROOT);
    ui_set(spinner, P_WIDTH, size);
    ui_set(spinner, P_HEIGHT, size);
    ui_set(spinner, P_ARC_COLOR, 0x23232B);     /* the track: style properties set this way are the main part's */
    ui_align(spinner, ALIGN_CENTER, 0, 0);

    int32_t title = ui_create(W_LABEL, UI_ROOT);
    ui_set_text(title, P_TEXT, "Hello from Muse");
    ui_set(title, P_TEXT_FONT, FONT_20);
    ui_set(title, P_TEXT_COLOR, 0xD8D5CC);
    ui_align(title, ALIGN_CENTER, 0, -36);

    count = ui_create(W_LABEL, UI_ROOT);
    ui_set(count, P_TEXT_FONT, FONT_48);
    ui_align(count, ALIGN_CENTER, 0, 20);

    show_color();
    show_seconds();
    sys_timer(1, 1000, 1);
    sys_log("hello: started");
}

GADGET_EXPORT(on_event) void on_event(int32_t type, int32_t a, int32_t b)
{
    (void)a;
    if (type == EV_TIMER) {
        seconds++;
        show_seconds();
    } else if (type == EV_TOUCH_DOWN || (type == EV_BUTTON && b == BTN_DOWN)) {
        color = (color + 1) % 4;
        show_color();
    }
}
