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
 * Runs components/muse_gadget_ble/mg_ui.c (UI input: hid_map, the panel's
 * touches as HID reports and the relative pointer's steps) and prints what it
 * computes as JSON lines; test_mg_ui.py checks them against its own HID
 * descriptor parser and the rules.
 */

#include <stdio.h>
#include <string.h>

#include "mg_ui.h"
#include "mgcommands.h"

static void print_reports(const char *name, mg_hid_t *h)
{
    printf("{\"%s\": [", name);
    const mg_hid_report_t *r;
    bool first = true;
    while ((r = mg_hid_peek(h))) {
        printf("%s[%u", first ? "" : ", ", r->id);
        for (int i = 0; i < r->len; i++) {
            printf(", %u", r->d[i]);
        }
        printf("]");
        first = false;
        mg_hid_pop(h);
    }
    printf("], \"dropped\": %u}\n", (unsigned)h->dropped);
}

int main(void)
{
    /* hid_map parsing. */
    static const struct {
        const char *name;
        uint8_t d[16];
        size_t n;
    } maps[] = {
        { "off", { 0 }, 1 },
        { "digitizer", { 1, 0x00, 0x40, 0x00, 0x20, 0x00, 0x80, 0x00, 0x60, 1 }, 10 },
        { "rel", { 3, 0, 0, 0, 0, 0xff, 0xff, 0xff, 0xff, 0, 0xe8, 0x03, 0x70, 0x08 }, 14 },
        { "short", { 1, 0, 0 }, 3 },
        { "empty", { 0 }, 0 },
        { "bad_mode", { 9 }, 1 },
        { "past_edge", { 2, 0x00, 0x80, 0, 0, 0x00, 0x90, 0xff, 0xff, 0 }, 10 },
        { "rel_no_cal", { 3, 0, 0, 0, 0, 0xff, 0xff, 0xff, 0xff, 0 }, 10 },
        { "rel_too_far", { 3, 0, 0, 0, 0, 0xff, 0xff, 0xff, 0xff, 0, 0xff, 0xff, 0x70, 0x08 }, 14 },
    };
    for (size_t i = 0; i < sizeof(maps) / sizeof(maps[0]); i++) {
        mg_hid_map_t m;
        uint16_t err = mg_hid_map_parse(maps[i].d, maps[i].n, &m);
        printf("{\"map\": \"%s\", \"err\": %u, \"mode\": %u, \"x\": %u, \"y\": %u, \"w\": %u, \"h\": %u, "
               "\"flags\": %u, \"rel_w\": %u, \"rel_h\": %u}\n",
               maps[i].name, err, m.mode, m.x, m.y, m.w, m.h, m.flags, m.rel_w, m.rel_h);
    }

    /* The report map. */
    printf("{\"report_map\": [");
    for (size_t i = 0; i < mg_hid_report_map_len; i++) {
        printf("%s%u", i ? ", " : "", mg_hid_report_map[i]);
    }
    printf("]}\n");

    /* The engine in each mode: a tap at the centre, then a drag. */
    static mg_hid_t h;
    mg_hid_init(&h, 466, 466);
    mg_hid_map_t full = { .mode = mg_hid_mode_digitizer, .x = 0, .y = 0, .w = 65535, .h = 65535 };
    const mg_touch_event_t tap[] = { { mg_touch_down, 233, 233 }, { mg_touch_move, 300, 233 }, { mg_touch_up, 300, 233 } };
    for (int mode = 1; mode <= 3; mode++) {
        full.mode = (uint8_t)mode;
        full.rel_w = 1000;
        full.rel_h = 2000;
        mg_hid_set_map(&h, &full);
        for (int i = 0; i < 3; i++) {
            mg_hid_touch(&h, &tap[i]);
        }
        static const char *const names[] = { "", "digitizer", "mouse_abs", "mouse_rel" };
        print_reports(names[mode], &h);
    }
    /* The digitizer: two taps on one spot never repeat the point at the second down. */
    full.mode = mg_hid_mode_digitizer;
    mg_hid_set_map(&h, &full);
    for (int k = 0; k < 2; k++) {
        mg_hid_touch(&h, &(mg_touch_event_t){ mg_touch_down, 100, 200 });
        mg_hid_touch(&h, &(mg_touch_event_t){ mg_touch_up, 100, 200 });
    }
    print_reports("digitizer_twice", &h);
    /* The HID service at connect: bonded peer x saved shown. */
    printf("{\"show_at_connect\": [%d, %d, %d, %d]}\n", mg_hid_show_at_connect(false, false),
           mg_hid_show_at_connect(false, true), mg_hid_show_at_connect(true, false), mg_hid_show_at_connect(true, true));
    /* Off: nothing. */
    full.mode = mg_hid_mode_off;
    mg_hid_set_map(&h, &full);
    mg_hid_touch(&h, &tap[0]);
    print_reports("off", &h);
    /* A big screen in counts: the queue holds the park and the walk. */
    full.mode = mg_hid_mode_mouse_rel;
    full.rel_w = 3000;
    full.rel_h = 3000;
    mg_hid_set_map(&h, &full);
    const mg_touch_event_t far = { mg_touch_down, 465, 465 };
    mg_hid_touch(&h, &far);
    print_reports("mouse_rel_far", &h);
    /* A consumer key: press, then release. */
    {
        static mg_hid_t h;
        mg_hid_init(&h, 466, 466);
        mg_hid_key(&h, 0x224);
        print_reports("consumer", &h);
    }
    /* A window panned past the screen's top (offset map): the square's top
     * quarter is off the screen; a tap there is dropped, one lower lands. */
    {
        static mg_hid_t h;
        mg_hid_init(&h, 466, 466);
        /* x 0 (offset 0.5 -> 0.25 of 65535), y -0.25 (offset 0.25 / 2 -> 0.125), square 1 x 0.5 of the screen */
        mg_hid_map_t m = { mg_hid_mode_digitizer, 16384, 8192, 65535, 32768, mg_hid_map_offset, 0, 0 };
        mg_hid_set_map(&h, &m);
        float sx, sy;
        bool in_top = mg_hid_screen_point(&m, 233, 40, 466, 466, &sx, &sy);
        bool in_mid = mg_hid_screen_point(&m, 233, 350, 466, 466, &sx, &sy);
        printf("{\"offset_pt\": [%d, %d, %.4f, %.4f]}\n", in_top, in_mid, sx, sy);
        mg_hid_touch(&h, &(mg_touch_event_t){ mg_touch_down, 233, 40 });
        mg_hid_touch(&h, &(mg_touch_event_t){ mg_touch_up, 233, 40 });
        mg_hid_touch(&h, &(mg_touch_event_t){ mg_touch_down, 233, 350 });
        mg_hid_touch(&h, &(mg_touch_event_t){ mg_touch_up, 233, 350 });
        print_reports("offset", &h);
        uint8_t raw[10] = { mg_hid_mode_digitizer, 0x00, 0x40, 0x00, 0x20, 0xff, 0xff, 0x00, 0x80, mg_hid_map_offset };
        mg_hid_map_t p;
        printf("{\"offset_parse\": %u}\n", mg_hid_map_parse(raw, sizeof(raw), &p));
    }
    {
        /* A passcode entry on the gadget's screen (held): a touch under way ends at its last point before, nothing
         * while it's up, nothing of a finger still down after it closes, then a new touch goes out. Each line: an
         * event as [action, x, y], or [] for none. */
        static const struct { bool held, pressed; uint16_t x, y; } in[] = {
            { false, true, 10, 20 },   { false, true, 30, 40 },   { true, true, 100, 100 }, { true, true, 110, 110 },
            { true, false, 0, 0 },     { true, true, 120, 120 },  { false, true, 130, 130 }, { false, true, 140, 140 },
            { false, false, 0, 0 },    { false, true, 200, 210 }, { false, false, 0, 0 },
        };
        mg_touch_track_t t;
        memset(&t, 0, sizeof(t));
        printf("{\"touch_held\": [");
        for (unsigned i = 0; i < sizeof(in) / sizeof(in[0]); i++) {
            mg_touch_event_t ev;
            bool got = mg_touch_feed_held(&t, in[i].held, in[i].pressed, in[i].x, in[i].y, 100u * (i + 1), &ev);
            if (got) {
                printf("%s[%u, %u, %u]", i ? ", " : "", ev.action, ev.x, ev.y);
            } else {
                printf("%s[]", i ? ", " : "");
            }
        }
        printf("]}\n");
    }
    return 0;
}
