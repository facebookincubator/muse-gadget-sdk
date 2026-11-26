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
#include "mg_ui.h"

#include <math.h>
#include <string.h>

#include "mgcommands.h"

/* ---- touch events ------------------------------------------------------- */

bool mg_touch_feed(mg_touch_track_t *t, bool pressed, uint16_t x, uint16_t y, uint32_t now_ms,
                   mg_touch_event_t *ev)
{
    if (!pressed) {
        if (!t->down) {
            return false;
        }
        t->down = false;
        *ev = (mg_touch_event_t){ .action = mg_touch_up, .x = t->x, .y = t->y };
        t->sent_x = t->x;
        t->sent_y = t->y;
        t->sent_ms = now_ms;
        return true;
    }
    t->x = x;
    t->y = y;
    if (!t->down) {
        t->down = true;
        *ev = (mg_touch_event_t){ .action = mg_touch_down, .x = x, .y = y };
    } else if ((x != t->sent_x || y != t->sent_y) && (uint32_t)(now_ms - t->sent_ms) >= MG_TOUCH_MOVE_MS) {
        *ev = (mg_touch_event_t){ .action = mg_touch_move, .x = x, .y = y };
    } else {
        return false;
    }
    t->sent_x = x;
    t->sent_y = y;
    t->sent_ms = now_ms;
    return true;
}

bool mg_touch_feed_held(mg_touch_track_t *t, bool held, bool pressed, uint16_t x, uint16_t y, uint32_t now_ms,
                        mg_touch_event_t *ev)
{
    if (held) {
        t->held = true;
        return mg_touch_feed(t, false, x, y, now_ms, ev);   /* the touch under way ends at its last point before */
    }
    if (t->held) {
        if (pressed) {
            return false;   /* the finger that was on the passcode entry: none of it goes out */
        }
        t->held = false;
    }
    return mg_touch_feed(t, pressed, x, y, now_ms, ev);
}

/* ---- the warp ------------------------------------------------------------- */

void mg_ui_disc_from_square(float x, float y, float *u, float *v)
{
    *u = x * sqrtf(1.0f - y * y / 2.0f);
    *v = y * sqrtf(1.0f - x * x / 2.0f);
}

static float root0(float a)
{
    return a > 0.0f ? sqrtf(a) : 0.0f;
}

void mg_ui_square_from_disc(float u, float v, float *x, float *y)
{
    float r2 = u * u + v * v;
    if (r2 > 1.0f) {
        float r = sqrtf(r2);
        u /= r;
        v /= r;
    }
    const float k = 2.0f * 1.41421356f;
    float d = u * u - v * v;
    *x = root0(2.0f + d + k * u) / 2.0f - root0(2.0f + d - k * u) / 2.0f;
    *y = root0(2.0f - d + k * v) / 2.0f - root0(2.0f - d - k * v) / 2.0f;
    /* Rounding can leave a hair past the edge. */
    *x = *x < -1.0f ? -1.0f : *x > 1.0f ? 1.0f : *x;
    *y = *y < -1.0f ? -1.0f : *y > 1.0f ? 1.0f : *y;
}

float mg_ui_pixel_coord(uint16_t p, uint16_t w)
{
    return w ? (2.0f * p + 1.0f) / w - 1.0f : 0.0f;
}

/* ---- hid_map ---------------------------------------------------------------- */

static uint16_t get16(const uint8_t *d)
{
    return (uint16_t)(d[0] | d[1] << 8);
}

bool mg_ui_in_edge_ring(uint16_t px, uint16_t py, uint16_t w, uint16_t h, bool round)
{
    float u = mg_ui_pixel_coord(px, w), v = mg_ui_pixel_coord(py, h);
    if (!round) {
        u = u < 0 ? -u : u;
        v = v < 0 ? -v : v;
        return (u > v ? u : v) > MG_UI_EDGE_BAND;
    }
    return u * u + v * v > MG_UI_EDGE_RING * MG_UI_EDGE_RING;
}

int mg_edge_gate_feed(mg_edge_gate_t *g, const mg_touch_event_t *ev, bool gated, uint32_t now_ms,
                      mg_touch_event_t out[2], bool *pan)
{
    *pan = false;
    if (!ev) {
        if (g->state == MG_EDGE_PENDING && now_ms - g->down_ms >= MG_UI_EDGE_HOLD_MS) {
            g->state = MG_EDGE_PHONE;   /* held still: a long press for the phone */
            out[0] = g->down;
            return 1;
        }
        return 0;
    }
    switch (ev->action) {
    case mg_touch_down:
        if (gated) {
            g->state = MG_EDGE_PENDING;
            g->down = *ev;
            g->down_ms = now_ms;
            return 0;
        }
        g->state = MG_EDGE_PHONE;
        out[0] = *ev;
        return 1;
    case mg_touch_move:
        if (g->state == MG_EDGE_PENDING) {
            int dx = (int)ev->x - g->down.x, dy = (int)ev->y - g->down.y;
            if (dx * dx + dy * dy <= MG_UI_EDGE_SLOP * MG_UI_EDGE_SLOP) {
                return 0;
            }
            g->state = MG_EDGE_PAN;
            *pan = true;
            out[0] = g->down;
            out[1] = *ev;
            return 2;
        }
        *pan = g->state == MG_EDGE_PAN;
        out[0] = *ev;
        return g->state == MG_EDGE_NONE ? 0 : 1;
    default: {   /* up */
        uint8_t was = g->state;
        g->state = MG_EDGE_NONE;
        if (was == MG_EDGE_PENDING) {
            /* lifted before it moved: a tap where it went down */
            out[0] = g->down;
            out[1] = (mg_touch_event_t){ mg_touch_up, g->down.x, g->down.y };
            return 2;
        }
        *pan = was == MG_EDGE_PAN;
        out[0] = *ev;
        return was == MG_EDGE_NONE ? 0 : 1;
    }
    }
}

uint16_t mg_hid_map_parse(const uint8_t *d, size_t n, mg_hid_map_t *m)
{
    memset(m, 0, sizeof(*m));
    if (n < 1) {
        return mg_error_code_invalid_length;
    }
    m->mode = d[0];
    if (m->mode == mg_hid_mode_off) {
        return 0;
    }
    if (m->mode > mg_hid_mode_mouse_rel) {
        return mg_error_code_invalid_value;
    }
    size_t need = 1 + 8 + 1 + (m->mode == mg_hid_mode_mouse_rel ? 4 : 0);
    if (n < need) {
        return mg_error_code_invalid_length;
    }
    m->x = get16(d + 1);
    m->y = get16(d + 3);
    m->w = get16(d + 5);
    m->h = get16(d + 7);
    m->flags = d[9];
    if (m->mode == mg_hid_mode_mouse_rel) {
        m->rel_w = get16(d + 10);
        m->rel_h = get16(d + 12);
    }
    bool offset = m->flags & mg_hid_map_offset;
    if (!m->w || !m->h || (!offset && ((uint32_t)m->x + m->w > 65535u || (uint32_t)m->y + m->h > 65535u)) ||
        (m->mode == mg_hid_mode_mouse_rel && (!m->rel_w || !m->rel_h))) {
        return mg_error_code_invalid_value;
    }
    if (m->mode == mg_hid_mode_mouse_rel) {
        /* A touch-down's park, its steps across the whole calibration and the press must fit the queue, or
         * the press would be dropped while the pointer's believed position moved on (mg_hid_touch). */
        uint32_t big = m->rel_w > m->rel_h ? m->rel_w : m->rel_h;
        if (big / 127 + 4 + (big + MG_HID_REL_STEP - 1) / MG_HID_REL_STEP + 1 > MG_HID_QUEUE) {
            return mg_error_code_invalid_value;
        }
    }
    return 0;
}

bool mg_hid_screen_point(const mg_hid_map_t *m, uint16_t px, uint16_t py, uint16_t w, uint16_t h, float *sx,
                         float *sy)
{
    float u = mg_ui_pixel_coord(px, w), v = mg_ui_pixel_coord(py, h), x = u, y = v;
    if (m->flags & mg_hid_map_fisheye) {
        mg_ui_square_from_disc(u, v, &x, &y);
    }
    float fx = (x + 1.0f) / 2.0f, fy = (y + 1.0f) / 2.0f;
    float x0 = m->x / 65535.0f, y0 = m->y / 65535.0f;
    if (m->flags & mg_hid_map_offset) {
        x0 = x0 * 2.0f - 0.5f;
        y0 = y0 * 2.0f - 0.5f;
    }
    float ox = x0 + fx * m->w / 65535.0f, oy = y0 + fy * m->h / 65535.0f;
    bool inside = ox >= 0.0f && ox <= 1.0f && oy >= 0.0f && oy <= 1.0f;
    *sx = ox < 0.0f ? 0.0f : ox > 1.0f ? 1.0f : ox;
    *sy = oy < 0.0f ? 0.0f : oy > 1.0f ? 1.0f : oy;
    return inside;
}

/* ---- the report map ------------------------------------------------------------ */

const uint8_t mg_hid_report_map[] = {
    /* 1: a one-finger touchscreen, laid out as hid-multitouch expects. */
    0x05, 0x0D,        /* Usage Page (Digitizers) */
    0x09, 0x04,        /* Usage (Touch Screen) */
    0xA1, 0x01,        /* Collection (Application) */
    0x85, MG_HID_REPORT_DIGITIZER,
    0x09, 0x22,        /*   Usage (Finger) */
    0xA1, 0x02,        /*   Collection (Logical) */
    0x09, 0x42,        /*     Usage (Tip Switch) */
    0x09, 0x32,        /*     Usage (In Range) */
    0x15, 0x00,        /*     Logical Minimum (0) */
    0x25, 0x01,        /*     Logical Maximum (1) */
    0x75, 0x01,        /*     Report Size (1) */
    0x95, 0x02,        /*     Report Count (2) */
    0x81, 0x02,        /*     Input (Data, Variable, Absolute) */
    0x95, 0x06,        /*     Report Count (6) */
    0x81, 0x03,        /*     Input (Constant): padding */
    0x09, 0x51,        /*     Usage (Contact Identifier) */
    0x25, 0x7F,        /*     Logical Maximum (127) */
    0x75, 0x08,        /*     Report Size (8) */
    0x95, 0x01,        /*     Report Count (1) */
    0x81, 0x02,        /*     Input (Data, Variable, Absolute) */
    0x05, 0x01,        /*     Usage Page (Generic Desktop) */
    0x09, 0x30,        /*     Usage (X) */
    0x09, 0x31,        /*     Usage (Y) */
    0x26, 0xFF, 0x7F,  /*     Logical Maximum (32767) */
    0x75, 0x10,        /*     Report Size (16) */
    0x95, 0x02,        /*     Report Count (2) */
    0x81, 0x02,        /*     Input (Data, Variable, Absolute) */
    0xC0,              /*   End Collection */
    0x05, 0x0D,        /*   Usage Page (Digitizers) */
    0x09, 0x54,        /*   Usage (Contact Count) */
    0x25, 0x01,        /*   Logical Maximum (1) */
    0x75, 0x08,        /*   Report Size (8) */
    0x95, 0x01,        /*   Report Count (1) */
    0x81, 0x02,        /*   Input (Data, Variable, Absolute) */
    0x09, 0x55,        /*   Usage (Contact Count Maximum) */
    0xB1, 0x02,        /*   Feature (Data, Variable, Absolute) */
    0xC0,              /* End Collection */

    /* 2: a mouse with absolute X and Y. */
    0x05, 0x01,        /* Usage Page (Generic Desktop) */
    0x09, 0x02,        /* Usage (Mouse) */
    0xA1, 0x01,        /* Collection (Application) */
    0x85, MG_HID_REPORT_MOUSE_ABS,
    0x09, 0x01,        /*   Usage (Pointer) */
    0xA1, 0x00,        /*   Collection (Physical) */
    0x05, 0x09,        /*     Usage Page (Button) */
    0x19, 0x01,        /*     Usage Minimum (1) */
    0x29, 0x03,        /*     Usage Maximum (3) */
    0x15, 0x00,        /*     Logical Minimum (0) */
    0x25, 0x01,        /*     Logical Maximum (1) */
    0x75, 0x01,        /*     Report Size (1) */
    0x95, 0x03,        /*     Report Count (3) */
    0x81, 0x02,        /*     Input (Data, Variable, Absolute) */
    0x75, 0x05,        /*     Report Size (5) */
    0x95, 0x01,        /*     Report Count (1) */
    0x81, 0x03,        /*     Input (Constant): padding */
    0x05, 0x01,        /*     Usage Page (Generic Desktop) */
    0x09, 0x30,        /*     Usage (X) */
    0x09, 0x31,        /*     Usage (Y) */
    0x26, 0xFF, 0x7F,  /*     Logical Maximum (32767) */
    0x75, 0x10,        /*     Report Size (16) */
    0x95, 0x02,        /*     Report Count (2) */
    0x81, 0x02,        /*     Input (Data, Variable, Absolute) */
    0xC0,              /*   End Collection */
    0xC0,              /* End Collection */

    /* 3: a mouse with relative X, Y and a wheel. */
    0x05, 0x01,        /* Usage Page (Generic Desktop) */
    0x09, 0x02,        /* Usage (Mouse) */
    0xA1, 0x01,        /* Collection (Application) */
    0x85, MG_HID_REPORT_MOUSE_REL,
    0x09, 0x01,        /*   Usage (Pointer) */
    0xA1, 0x00,        /*   Collection (Physical) */
    0x05, 0x09,        /*     Usage Page (Button) */
    0x19, 0x01,        /*     Usage Minimum (1) */
    0x29, 0x03,        /*     Usage Maximum (3) */
    0x15, 0x00,        /*     Logical Minimum (0) */
    0x25, 0x01,        /*     Logical Maximum (1) */
    0x75, 0x01,        /*     Report Size (1) */
    0x95, 0x03,        /*     Report Count (3) */
    0x81, 0x02,        /*     Input (Data, Variable, Absolute) */
    0x75, 0x05,        /*     Report Size (5) */
    0x95, 0x01,        /*     Report Count (1) */
    0x81, 0x03,        /*     Input (Constant): padding */
    0x05, 0x01,        /*     Usage Page (Generic Desktop) */
    0x09, 0x30,        /*     Usage (X) */
    0x09, 0x31,        /*     Usage (Y) */
    0x09, 0x38,        /*     Usage (Wheel) */
    0x15, 0x81,        /*     Logical Minimum (-127) */
    0x25, 0x7F,        /*     Logical Maximum (127) */
    0x75, 0x08,        /*     Report Size (8) */
    0x95, 0x03,        /*     Report Count (3) */
    0x81, 0x06,        /*     Input (Data, Variable, Relative) */
    0xC0,              /*   End Collection */
    0xC0,              /* End Collection */

    /* 4: consumer control, one 16-bit usage (AC Back 0x224, AC Home 0x223). */
    0x05, 0x0C,        /* Usage Page (Consumer) */
    0x09, 0x01,        /* Usage (Consumer Control) */
    0xA1, 0x01,        /* Collection (Application) */
    0x85, MG_HID_REPORT_CONSUMER,
    0x15, 0x00,        /*   Logical Minimum (0) */
    0x26, 0xFF, 0x03,  /*   Logical Maximum (0x3FF) */
    0x19, 0x00,        /*   Usage Minimum (0) */
    0x2A, 0xFF, 0x03,  /*   Usage Maximum (0x3FF) */
    0x75, 0x10,        /*   Report Size (16) */
    0x95, 0x01,        /*   Report Count (1) */
    0x81, 0x00,        /*   Input (Data, Array, Absolute) */
    0xC0,              /* End Collection */
};
const size_t mg_hid_report_map_len = sizeof(mg_hid_report_map);

/* ---- reports ---------------------------------------------------------------- */

static uint16_t abs_coord(float f)
{
    f = f < 0.0f ? 0.0f : f > 1.0f ? 1.0f : f;
    return (uint16_t)(f * MG_HID_ABS_MAX + 0.5f);
}

void mg_hid_digitizer_report(mg_hid_report_t *r, bool tip, float sx, float sy)
{
    uint16_t x = abs_coord(sx), y = abs_coord(sy);
    *r = (mg_hid_report_t){ .id = MG_HID_REPORT_DIGITIZER, .len = MG_HID_DIGITIZER_LEN };
    r->d[0] = tip ? 0x03 : 0x00;   /* tip switch, in range */
    r->d[1] = 0;                   /* contact identifier */
    r->d[2] = (uint8_t)x;
    r->d[3] = (uint8_t)(x >> 8);
    r->d[4] = (uint8_t)y;
    r->d[5] = (uint8_t)(y >> 8);
    r->d[6] = 1;                   /* contact count: the one being reported, lifting included */
}

void mg_hid_mouse_abs_report(mg_hid_report_t *r, uint8_t buttons, float sx, float sy)
{
    uint16_t x = abs_coord(sx), y = abs_coord(sy);
    *r = (mg_hid_report_t){ .id = MG_HID_REPORT_MOUSE_ABS, .len = MG_HID_MOUSE_ABS_LEN };
    r->d[0] = buttons & 0x07;
    r->d[1] = (uint8_t)x;
    r->d[2] = (uint8_t)(x >> 8);
    r->d[3] = (uint8_t)y;
    r->d[4] = (uint8_t)(y >> 8);
}

void mg_hid_mouse_rel_report(mg_hid_report_t *r, uint8_t buttons, int8_t dx, int8_t dy)
{
    *r = (mg_hid_report_t){ .id = MG_HID_REPORT_MOUSE_REL, .len = MG_HID_MOUSE_REL_LEN };
    r->d[0] = buttons & 0x07;
    r->d[1] = (uint8_t)dx;
    r->d[2] = (uint8_t)dy;
    r->d[3] = 0;   /* wheel */
}

/* ---- the engine ------------------------------------------------------------- */

void mg_hid_init(mg_hid_t *h, uint16_t panel_w, uint16_t panel_h)
{
    memset(h, 0, sizeof(*h));
    h->panel_w = panel_w;
    h->panel_h = panel_h;
}

void mg_hid_clear(mg_hid_t *h)
{
    h->head = h->count = 0;
}

void mg_hid_set_map(mg_hid_t *h, const mg_hid_map_t *m)
{
    h->map = *m;
    mg_hid_clear(h);
    h->rel_parked = false;
    h->drop = false;
}

static void push(mg_hid_t *h, const mg_hid_report_t *r)
{
    if (h->count >= MG_HID_QUEUE) {
        h->dropped++;
        return;
    }
    h->q[(h->head + h->count) % MG_HID_QUEUE] = *r;
    h->count++;
}

const mg_hid_report_t *mg_hid_peek(const mg_hid_t *h)
{
    return h->count ? &h->q[h->head] : NULL;
}

void mg_hid_pop(mg_hid_t *h)
{
    if (h->count) {
        h->head = (uint16_t)((h->head + 1) % MG_HID_QUEUE);
        h->count--;
    }
}

/* Moves the relative pointer by (dx, dy) counts in steps of at most
 * MG_HID_REL_STEP on either axis that add up to exactly that. */
static void rel_steps(mg_hid_t *h, int32_t dx, int32_t dy, uint8_t buttons)
{
    int32_t ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy, big = ax > ay ? ax : ay;
    int32_t n = (big + MG_HID_REL_STEP - 1) / MG_HID_REL_STEP, px = 0, py = 0;
    for (int32_t i = 1; i <= n; i++) {
        /* rounded to nearest, either sign */
        int32_t tx = (int32_t)((2 * (int64_t)dx * i + (dx < 0 ? -n : n)) / (2 * n));
        int32_t ty = (int32_t)((2 * (int64_t)dy * i + (dy < 0 ? -n : n)) / (2 * n));
        mg_hid_report_t r;
        mg_hid_mouse_rel_report(&r, buttons, (int8_t)(tx - px), (int8_t)(ty - py));
        push(h, &r);
        px = tx;
        py = ty;
    }
    h->rel_x += dx;
    h->rel_y += dy;
}

void mg_hid_key(mg_hid_t *h, uint16_t usage)
{
    mg_hid_report_t r = { MG_HID_REPORT_CONSUMER, MG_HID_CONSUMER_LEN, { (uint8_t)usage, (uint8_t)(usage >> 8) } };
    push(h, &r);
    r.d[0] = r.d[1] = 0;
    push(h, &r);
}

void mg_hid_touch(mg_hid_t *h, const mg_touch_event_t *ev)
{
    const mg_hid_map_t *m = &h->map;
    if (m->mode == mg_hid_mode_off) {
        return;
    }
    float sx, sy;
    bool inside = mg_hid_screen_point(m, ev->x, ev->y, h->panel_w, h->panel_h, &sx, &sy);
    /* A touch that goes down off the screen (the black past a panned
     * window's edge) is dropped, the whole gesture. */
    if (ev->action == mg_touch_down) {
        h->drop = !inside;
    }
    if (h->drop) {
        if (ev->action == mg_touch_up) {
            h->drop = false;
        }
        return;
    }
    bool down = ev->action != mg_touch_up;
    mg_hid_report_t r;
    switch (m->mode) {
    case mg_hid_mode_digitizer: {
        mg_hid_digitizer_report(&r, down, sx, sy);
        uint16_t x = (uint16_t)(r.d[2] | r.d[3] << 8), y = (uint16_t)(r.d[4] | r.d[5] << 8);
        /*
         * The kernel sends a contact's position only where it changes from the
         * last one it sent for that slot (while touching), and Android takes a
         * new contact with no position as (0, 0): the status bar. So the same
         * spot tapped twice opened the notification panel (on the S22,
         * an icon tapped five times opened once). A new contact where the last
         * one was moves one count (1/32767 of the screen) on each axis that
         * repeats. A lift reports where the contact last was.
         */
        if (ev->action == mg_touch_down) {
            if (x == h->dig_x) {
                x = x < MG_HID_ABS_MAX ? x + 1 : x - 1;
            }
            if (y == h->dig_y) {
                y = y < MG_HID_ABS_MAX ? y + 1 : y - 1;
            }
        }
        if (down) {
            h->dig_x = x;
            h->dig_y = y;
        } else {
            x = h->dig_x;
            y = h->dig_y;
        }
        r.d[2] = (uint8_t)x;
        r.d[3] = (uint8_t)(x >> 8);
        r.d[4] = (uint8_t)y;
        r.d[5] = (uint8_t)(y >> 8);
        push(h, &r);
        break;
    }
    case mg_hid_mode_mouse_abs:
        if (ev->action == mg_touch_down) {
            mg_hid_mouse_abs_report(&r, 0, sx, sy);   /* there first, then press */
            push(h, &r);
        }
        mg_hid_mouse_abs_report(&r, down ? 1 : 0, sx, sy);
        push(h, &r);
        break;
    case mg_hid_mode_mouse_rel: {
        int32_t tx = (int32_t)(sx * m->rel_w + 0.5f), ty = (int32_t)(sy * m->rel_h + 0.5f);
        if (ev->action == mg_touch_down) {
            /* Park: enough full-size moves to cross the whole screen and some. */
            uint16_t big = m->rel_w > m->rel_h ? m->rel_w : m->rel_h;
            int park = big / 127 + 4;
            for (int i = 0; i < park; i++) {
                mg_hid_mouse_rel_report(&r, 0, -127, -127);
                push(h, &r);
            }
            h->rel_x = h->rel_y = 0;
            h->rel_parked = true;
            rel_steps(h, tx, ty, 0);
            mg_hid_mouse_rel_report(&r, 1, 0, 0);
            push(h, &r);
        } else if (h->rel_parked) {
            rel_steps(h, tx - h->rel_x, ty - h->rel_y, 1);   /* a drag: the button stays down */
            if (!down) {
                mg_hid_mouse_rel_report(&r, 0, 0, 0);
                push(h, &r);
            }
        }
        break;
    }
    default:
        break;
    }
}
