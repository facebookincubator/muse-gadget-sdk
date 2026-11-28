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
 * The Muse home and Status page of the v1.0 UI, on every
 * full-layout touch panel: the reference layout scaled from its 466 px round
 * reference. Battery at the top, the time (12/24 h
 * from the phone, hidden until synced), a date / state line, Muse with the
 * screen's hold region, the Speaker button left and the Call
 * button right, page dots. Status, one swipe right: the
 * connection, battery, ID, volume and firmware. LVGL's task only.
 */

#pragma once

#include <stdbool.h>

#include "lvgl.h"
#include "muse_touch.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Geometry of the home for a panel: every size a share of S, the short side, from
 * the reference home at 466; y from the panel's top unless said otherwise.
 */
typedef struct {
    int short_side;
    int top;            /* 0: the reference lays out the whole panel, top down */
    int muse_px;        /* Muse's canvas, whole cells: the largest in 50-55 % of S */
    int muse_y;         /* its centre, from the screen's centre */
    int button_d;       /* Speaker and Call */
    int button_y;       /* their centres, from the screen's centre (Muse's line) */
    int button_x;       /* ...and from the middle, either side */
    int hold_d;         /* the hold region, a disc over Muse */
    int ring;           /* the mic ring's width */
    int batt_top, batt_w, batt_h, batt_stroke, batt_r, batt_gap, nub_w, nub_h, nub_gap;
    int top_gap;        /* between the top row's icons */
    int time_top;       /* the time's digits start here */
    int time_digits;    /* ...this tall */
    int date_top;       /* the date's capitals start here */
    int date_caps;
    int dot_cy, dot_d, dot_pitch;   /* the page dots */
    int name_top;       /* not set up: the device's name, its capitals' top (the date line; below the rim keys) */
    int state_top;      /* not set up: "Open Muse app", its capitals' top (centered on the time's digits) */
    int state_caps;
} muse_home_geom_t;

/* Computes the layout for w x h (round or with corner_px corners). */
void muse_home_geom(int w, int h, bool round, int corner_px, muse_home_geom_t *g);

/* Builds the home's chrome on the face tile (Muse itself is muse_ui.c's). */
void muse_home_build(lv_obj_t *face, const muse_home_geom_t *g);

/* What the state line says when nothing else does: the link's state in
 * sentence case ("Open Muse app"), or NULL when it's ready (the date shows). */
typedef struct {
    const char *link;      /* NULL: ready */
    bool paired;           /* set up with an app */
    bool token;            /* a device token is stored: else "Open Muse app" and the name */
    bool talks;            /* a press reaches Muse: Speaker and the hold region */
    const char *name;      /* this gadget's BLE name, shown until paired */
} muse_home_state_t;

/* Every quarter second while the face is up. */
void muse_home_tick(const muse_home_state_t *st);

/* The passcode's locked face: no Speaker or Call buttons, no hold
 * region, a lock glyph in the page dots' place, no reply text on the state
 * line. */
void muse_home_set_locked(bool locked);

/* The bench switch is on for the connected central (bench.out=1 from
 * the USB console or, for its own connection, the Nordic UART):
 * "Bench" shows in the page dots' place while true (mg_ble's
 * bench_mark). Any task: sets a flag muse_home_bench_tick shows. */
#define MUSE_HOME_BENCH_TEXT "Bench"
void muse_home_set_bench(bool on);
/* Every frame on the LVGL task (muse_ui.c), whatever the screen shows. */
void muse_home_bench_tick(void);
/* The label shows now (the page dots hide). LVGL task. */
bool muse_home_bench_shown(void);
/* For a video frame (LVGL paused): the mark is on, its text's top left on
 * the screen, its font and color. Any task. */
struct _lv_font_t;   /* lv_font_t */
bool muse_home_bench_mark(int *x, int *y, const struct _lv_font_t **font, uint32_t *rgb);

/* Hidden while a reply's page takes the face (muse_ui.c's answer layouts). */
void muse_home_set_hidden(bool hidden);

/* A hold at screen point x, y would be a talk press: the home's hold region
 * shows there (the face active, not locked), talks (Settings > Talk isn't
 * Button), and no shown call control takes the point (the call's
 * controls wait for a tap's lift there, else they come up at the
 * touch-down). LVGL task. */
bool muse_home_talk_at(int x, int y);

/* A hold on the screen is past the threshold: the face doesn't slide. */
bool muse_home_holding(void);

/* The home shows the time now (synced, set up, the home not hidden by a
 * reply's page): the subtitles' tall band stops below it. LVGL task. */
bool muse_home_clock_shown(void);

/* The state line's y (from the screen's centre), for the call's pill. */
int muse_home_line_y(void);

/* Half the panel's visible width at row y (from the top), inset px in from
 * its edge horizontally: a round panel's chord, a rounded rectangle's corners. */
double muse_home_safe_half(double y, double inset);
/* Bench: where a named home control is (muse_touch.h; LVGL task). */
bool muse_home_bench_point(const char *what, muse_touch_gesture_t *g);

/* A speaker change the gadget didn't make: the
 * volume overlay Speaker's tap opens, with the source line under the level
 * ("Set by phone"), closed 3 s later. False: no home or no Speaker here.
 * LVGL task. */
bool muse_home_volume_by(const char *by);
/* The top row's icon height (the battery's; 0 with no home), for an icon
 * drawn at it elsewhere (the Sound page's muted icon). */
int muse_home_top_h(void);

/* The board's Muse build uses Wi-Fi: the top row shows a Wi-Fi icon. */
bool muse_home_wifi_build(void);

/* The Status page on its tile. */
void muse_status_build(lv_obj_t *tile);
void muse_status_tick(void);

#ifdef __cplusplus
}
#endif
