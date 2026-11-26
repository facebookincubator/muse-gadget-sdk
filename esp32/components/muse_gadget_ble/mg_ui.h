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
#pragma once

/*
 * UI input (mgcommands.h, UI input; protocols/UI.md), with no BLE stack in
 * it so the host tests build it: touch samples turned into down / move / up
 * events, the square-to-disc warp and its inverse, the client's hid_map, and
 * the HID reports the gadget sends as a touchscreen or a pointer (the report
 * map, and the relative pointer's park-and-step moves). mg_ble.c feeds it the
 * panel's touches and sends what it queues.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- touch events ---- */

#define MG_TOUCH_MOVE_MS 16   /* moves at most this often */

typedef struct {
    uint8_t action;           /* mg_touch_action_t */
    uint16_t x, y;            /* panel pixels */
} mg_touch_event_t;

typedef struct {
    bool down;
    bool held;                /* a passcode entry took the touches: nothing goes out until the finger lifts */
    uint16_t x, y;            /* the latest point while down */
    uint16_t sent_x, sent_y;  /* the last point an event carried */
    uint32_t sent_ms;
} mg_touch_track_t;

/*
 * One touch sample (pressed, and the point if so) at now_ms. Returns the
 * event it makes, if any: down on the first sample pressed, up on the first
 * released one (at the last point seen), and move while pressed when the
 * point differs from the last event's and MG_TOUCH_MOVE_MS have passed since
 * it (a move held back by that is sent with a later sample, so the last
 * point always goes out). Samples ~every 10 ms keep moves current.
 */
bool mg_touch_feed(mg_touch_track_t *t, bool pressed, uint16_t x, uint16_t y, uint32_t now_ms,
                   mg_touch_event_t *ev);
/*
 * mg_touch_feed() for a panel whose own screen may take the touches back:
 * held while a passcode keypad or any other passcode entry is on it
 * (mgcommands.h, UI input). Nothing a finger does then goes out: a touch
 * under way ends at once (up at its last point before), and a finger still
 * down when the entry closes stays out until it lifts, so the next event is
 * a new touch's down.
 */
bool mg_touch_feed_held(mg_touch_track_t *t, bool held, bool pressed, uint16_t x, uint16_t y, uint32_t now_ms,
                        mg_touch_event_t *ev);

/* ---- the warp (protocols/UI.md, Square to disc) ---- */

/* u, v in the disc from x, y in the square, all in [-1, 1]. */
void mg_ui_disc_from_square(float x, float y, float *u, float *v);
/* x, y in the square from u, v in the disc (moved onto the unit circle first if outside). */
void mg_ui_square_from_disc(float u, float v, float *x, float *y);
/* Pixel p of a panel w pixels across, as a coordinate in [-1, 1] (the inscribed circle's). */
float mg_ui_pixel_coord(uint16_t p, uint16_t w);
/* A panel pixel in the outer edge ring (protocols/mgcommands.h UI input
 * controls: edge_pan): on a round panel beyond MG_UI_EDGE_RING of the radius,
 * on a rectangular one in the band along its edges, the outer 10% of its
 * width or height (beyond MG_UI_EDGE_BAND of the half width or height). */
#define MG_UI_EDGE_RING 0.88f
#define MG_UI_EDGE_BAND 0.80f
bool mg_ui_in_edge_ring(uint16_t px, uint16_t py, uint16_t w, uint16_t h, bool round);

/*
 * Tap or pan (protocols/mgcommands.h, UI input controls): a touch that goes
 * down in the edge ring (or anywhere in pan mode) pans the screen mirror
 * only once it moves more than MG_UI_EDGE_SLOP pixels; lifted before that,
 * or held still for MG_UI_EDGE_HOLD_MS, it's an ordinary touch for the phone
 * (a tap, a long press). Until it's decided nothing of it goes anywhere.
 */
#define MG_UI_EDGE_SLOP 12
#define MG_UI_EDGE_HOLD_MS 350
enum { MG_EDGE_NONE, MG_EDGE_PENDING, MG_EDGE_PAN, MG_EDGE_PHONE };
typedef struct {
    uint8_t state;
    mg_touch_event_t down;
    uint32_t down_ms;
} mg_edge_gate_t;
/*
 * One touch event (or NULL: time passing, every sample) through the gate.
 * gated: this gesture may pan (decided at its down: edge ring or pan mode).
 * Writes the events to pass on (at most 2: a held-back down, then the event)
 * and sets *pan when they belong to a pan (the phone's, never the HID's).
 * Returns how many.
 */
int mg_edge_gate_feed(mg_edge_gate_t *g, const mg_touch_event_t *ev, bool gated, uint32_t now_ms,
                      mg_touch_event_t out[2], bool *pan);

/* ---- HID ---- */

/* The client's hid_map (mg_ui_input_hid_map). */
typedef struct {
    uint8_t mode;             /* mg_hid_mode_t */
    uint16_t x, y, w, h;      /* the square on the screen, 1/65535 of its width and height */
    uint8_t flags;            /* mg_hid_map_flags_t */
    uint16_t rel_w, rel_h;    /* mouse_rel: pointer counts across the screen's width and down its height */
} mg_hid_map_t;

/*
 * Parses hid_map's payload (after the sub-command). Returns 0, or the
 * mg_error_code_t: invalid_length, invalid_value (an unknown mode, a square
 * past the screen, no calibration).
 */
uint16_t mg_hid_map_parse(const uint8_t *d, size_t n, mg_hid_map_t *m);

/*
 * Where panel pixel (px, py) of a w x h panel lands on the phone's screen,
 * as fractions of its width and height in [0, 1]: through the inverse warp
 * when the map says the warp is on, into the square, onto the screen.
 */
bool mg_hid_screen_point(const mg_hid_map_t *m, uint16_t px, uint16_t py, uint16_t w, uint16_t h, float *sx,
                         float *sy);   /* false: off the screen (clamped onto its edge) */

/* Report IDs (protocols/UI.md, HID) and their input reports' lengths (the ID not included). */
#define MG_HID_REPORT_DIGITIZER 1
#define MG_HID_REPORT_MOUSE_ABS 2
#define MG_HID_REPORT_MOUSE_REL 3
#define MG_HID_REPORT_CONSUMER 4   /* a consumer-control key: AC Back, AC Home (ui_input hid_key) */
#define MG_HID_CONSUMER_LEN 2
#define MG_HID_DIGITIZER_LEN 7
#define MG_HID_MOUSE_ABS_LEN 5
#define MG_HID_MOUSE_REL_LEN 4
#define MG_HID_REPORT_MAX 7
#define MG_HID_ABS_MAX 32767
#define MG_HID_REL_STEP 8      /* the relative pointer's largest step, counts */

/* The HID Report Map (all three collections, with their report IDs). */
extern const uint8_t mg_hid_report_map[];
extern const size_t mg_hid_report_map_len;

/* A report waiting to go to the HID host. */
typedef struct {
    uint8_t id;
    uint8_t len;
    uint8_t d[MG_HID_REPORT_MAX];
} mg_hid_report_t;

#define MG_HID_QUEUE 512       /* parking plus a move across a big screen in 8-count steps */

/*
 * The touch-to-HID engine: takes the panel's touch events and queues the
 * reports for the map's mode. Relative pointer: on each down, park in the
 * top-left corner (a burst of -127 moves the host clamps at the edge), step
 * to the touch's point in steps of at most MG_HID_REL_STEP counts (the
 * calibration converts the screen point to counts), then press button 1;
 * moves while down drag in the same steps; up releases. Absolute pointer and
 * digitizer: one report per event.
 */
typedef struct {
    mg_hid_map_t map;
    uint16_t panel_w, panel_h;
    mg_hid_report_t q[MG_HID_QUEUE];
    uint16_t head, count;
    int32_t rel_x, rel_y;      /* where the queued moves leave the relative pointer, in counts from the corner */
    bool rel_parked;
    bool drop;                 /* the gesture went down off the screen: none of it is sent */
    uint16_t dig_x, dig_y;     /* the digitizer's last reported point */
    uint32_t dropped;          /* reports that didn't fit */
} mg_hid_t;

void mg_hid_init(mg_hid_t *h, uint16_t panel_w, uint16_t panel_h);
/* A new map: the queue is emptied (a mode change starts afresh). */
void mg_hid_set_map(mg_hid_t *h, const mg_hid_map_t *m);

/*
 * Whether the HID service shows to a new connection, decided at connect before
 * any discovery (mg_ble.c): only to a peer that holds a bond with this gadget,
 * and only while a HID host's service is saved as shown (a bonded HID host
 * loses its input device if the service goes away under it). To anyone else
 * it's hidden whatever an earlier client left saved: a phone that sees a HID
 * service asks to pair, an iPhone on every connection (holding the app's
 * writes until that's answered). It shows to them once a hid_map with HID on
 * arrives on that connection, which is when pairing is wanted.
 */
static inline bool mg_hid_show_at_connect(bool peer_bonded, bool shown_for_host)
{
    return peer_bonded && shown_for_host;
}
/* One touch event (mg_touch_event_t): queues its reports. */
void mg_hid_touch(mg_hid_t *h, const mg_touch_event_t *ev);
/* A consumer-control key (usage on page 0x0C): press and release, queued. */
void mg_hid_key(mg_hid_t *h, uint16_t usage);
/* The next report to send, or NULL; pop it once it's sent. */
const mg_hid_report_t *mg_hid_peek(const mg_hid_t *h);
void mg_hid_pop(mg_hid_t *h);
void mg_hid_clear(mg_hid_t *h);

/* Report builders (exposed for the tests). sx, sy in [0, 1]. */
void mg_hid_digitizer_report(mg_hid_report_t *r, bool tip, float sx, float sy);
void mg_hid_mouse_abs_report(mg_hid_report_t *r, uint8_t buttons, float sx, float sy);
void mg_hid_mouse_rel_report(mg_hid_report_t *r, uint8_t buttons, int8_t dx, int8_t dy);

#ifdef __cplusplus
}
#endif
