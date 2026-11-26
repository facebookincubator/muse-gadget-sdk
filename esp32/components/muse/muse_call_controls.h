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
 * Live call: the client is the Muse app, which implements the call on its own session.
 */
#pragma once

/*
 * The live call's on-screen controls and volume gestures (muse_call.c), as
 * plain arithmetic so the host tests can check them.
 *
 * Volume: a vertical swipe steps it in sixteenths, up louder, a longer swipe
 * more steps; the − and + buttons and the board's volume keys step one.
 *
 * The controls panel, MUSE_CALL_CTL_W x MUSE_CALL_CTL_H, centered near
 * the bottom of the screen, the same pixels on every board: a hint line at the
 * top (at the call's start), a volume row (− button, level bar, + button) and
 * a red End call button. Shown on a tap over the call's picture or the calling
 * screen, hidden 3 s after the last touch. Coordinates below are relative to
 * the panel's top left.
 *
 * Hit areas: what's drawn plus a margin, and − and + out to the screen's side
 * edges across their row (taps beside + once went nowhere). Where a
 * hit area is under 7 mm tall on a panel (the minimum hit height these boards
 * keep), only its rows
 * grow, − and + upward into the hint line and End call downward into the
 * margin below the panel; nothing drawn changes.
 */

#define MUSE_CALL_VOLUME_LEVELS 16
#define MUSE_CALL_SWIPE_FIRST_PX 30   /* this far up or down is one step... */
#define MUSE_CALL_SWIPE_STEP_PX 60    /* ...and each this much further another */
#define MUSE_CALL_START_VOLUME 50     /* a call that starts with the speaker at 0 gets this */

#define MUSE_CALL_CTL_W 300
#define MUSE_CALL_CTL_H 156
#define MUSE_CALL_CTL_BOTTOM 40       /* panel bottom this far above the screen's */
#define MUSE_CALL_CTL_MIN_SCREEN 320  /* smaller screens get no panel */

#define MUSE_CALL_HINT_H 24           /* the hint line: rows 0 .. this */
#define MUSE_CALL_HINT_X0 10
#define MUSE_CALL_HINT_X1 290
#define MUSE_CALL_BTN 56              /* − and + buttons: circles this wide */
#define MUSE_CALL_BTN_Y 30
#define MUSE_CALL_BAR_X0 72           /* the level bar */
#define MUSE_CALL_BAR_X1 228
#define MUSE_CALL_BAR_Y0 53
#define MUSE_CALL_BAR_Y1 63
#define MUSE_CALL_END_X0 60           /* End call */
#define MUSE_CALL_END_X1 240
#define MUSE_CALL_END_Y0 100
#define MUSE_CALL_END_Y1 152

#define MUSE_CALL_HIT_PX 10           /* the hit margin around what's drawn (half of it above End call) */
#define MUSE_CALL_MIN_MM10 70         /* a hit area at least 7 mm tall */
#define MUSE_CALL_SPARE_PX 2          /* hit areas stay this far inside the panel's real outline */

/* − and +'s hit rows (before growing), and End call's. */
#define MUSE_CALL_BTN_HIT_Y0 (MUSE_CALL_BTN_Y - MUSE_CALL_HIT_PX)
#define MUSE_CALL_BTN_HIT_Y1 (MUSE_CALL_BTN_Y + MUSE_CALL_BTN + MUSE_CALL_HIT_PX / 2)
#define MUSE_CALL_END_HIT_Y0 (MUSE_CALL_END_Y0 - MUSE_CALL_HIT_PX / 2)
#define MUSE_CALL_END_HIT_Y1 (MUSE_CALL_END_Y1 + MUSE_CALL_HIT_PX)

typedef struct {
    short x, y;          /* the panel's top left on the screen */
    short screen_w, screen_h;   /* the screen: − and + reach its side edges */
    short round, corner; /* its real outline: round (w = h), else corners of this radius */
    short min_px;        /* 7 mm on this panel, in pixels */
    short btn_up;        /* rows − and +'s hit area grows upward by (0: already 7 mm) */
    short end_down;      /* rows End call's grows downward by */
} muse_call_panel_t;

/*
 * The panel on a w x h screen (round, or rectangular with corners of radius
 * `corner`) whose pixels per mm times 10 are px_per_mm10 (0: 10.6, a typical
 * 1.75" round panel's). False: the screen is too small for it (the pill,
 * swipes and the volume keys only).
 */
/* Touch boards too small for the panel: a compact End call button, always
 * shown in a call, at the bottom middle: the screen's width less 8 px a side, at most 200 px; at least 7 mm
 * tall (the touch target floor) and 40 px; 6 px above the bottom edge (an eighth of the height on a round panel). Only
 * that rectangle ends the call: a tap anywhere else never does. 0: the panel fits (no button), or the screen
 * is too small for one. */
typedef struct {
    short x, y, w, h;
} muse_call_end_btn_t;

static inline int muse_call_end_button(int w, int h, int round, int px_per_mm10, muse_call_end_btn_t *b)
{
    if ((w >= MUSE_CALL_CTL_MIN_SCREEN && h >= MUSE_CALL_CTL_MIN_SCREEN) || w < 120 || h < 120) {
        return 0;
    }
    int ppm = px_per_mm10 > 0 ? px_per_mm10 : 106;
    int bh = (MUSE_CALL_MIN_MM10 * ppm + 99) / 100;
    bh = bh < 40 ? 40 : bh;
    int bw = w - 16 > 200 ? 200 : w - 16;
    int inset = round ? h / 8 : 6;
    b->x = (short)((w - bw) / 2);
    b->y = (short)(h - inset - bh);
    b->w = (short)bw;
    b->h = (short)bh;
    return 1;
}

static inline int muse_call_end_button_hit(const muse_call_end_btn_t *b, int x, int y)
{
    return x >= b->x && x < b->x + b->w && y >= b->y && y < b->y + b->h;
}

static inline int muse_call_panel(int w, int h, int round, int corner, int px_per_mm10, muse_call_panel_t *P)
{
    if (w < MUSE_CALL_CTL_MIN_SCREEN || h < MUSE_CALL_CTL_MIN_SCREEN) {
        return 0;
    }
    int ppm = px_per_mm10 > 0 ? px_per_mm10 : 106;
    int min_px = (MUSE_CALL_MIN_MM10 * ppm + 99) / 100;   /* rounded up: never under the mm */
    int btn = MUSE_CALL_BTN_HIT_Y1 - MUSE_CALL_BTN_HIT_Y0, end = MUSE_CALL_END_HIT_Y1 - MUSE_CALL_END_HIT_Y0;
    P->x = (short)((w - MUSE_CALL_CTL_W) / 2);
    P->y = (short)(h - MUSE_CALL_CTL_BOTTOM - MUSE_CALL_CTL_H);
    P->screen_w = (short)w;
    P->screen_h = (short)h;
    P->round = (short)(round != 0);
    P->corner = (short)corner;
    P->min_px = (short)min_px;
    P->btn_up = (short)(min_px > btn ? min_px - btn : 0);
    P->end_down = (short)(min_px > end ? min_px - end : 0);
    /* Never past the panel's top, nor the screen's bottom. */
    if (P->btn_up > MUSE_CALL_BTN_HIT_Y0) {
        P->btn_up = MUSE_CALL_BTN_HIT_Y0;
    }
    if (P->end_down > MUSE_CALL_CTL_BOTTOM + MUSE_CALL_CTL_H - MUSE_CALL_END_HIT_Y1) {
        P->end_down = (short)(MUSE_CALL_CTL_BOTTOM + MUSE_CALL_CTL_H - MUSE_CALL_END_HIT_Y1);
    }
    return 1;
}

typedef enum {
    MUSE_CALL_HIT_NONE,
    MUSE_CALL_HIT_MINUS,
    MUSE_CALL_HIT_PLUS,
    MUSE_CALL_HIT_END,
} muse_call_hit_t;

/* Steps for a finger dx, dy pixels from where it went down (screen y grows
 * downwards): positive is louder. 0 until it has gone far enough, and while
 * it moves more sideways than up or down (a page swipe). */
static inline int muse_call_swipe_steps(int dx, int dy)
{
    int ady = dy < 0 ? -dy : dy, adx = dx < 0 ? -dx : dx;
    if (ady < MUSE_CALL_SWIPE_FIRST_PX || ady < 2 * adx) {
        return 0;
    }
    int steps = 1 + (ady - MUSE_CALL_SWIPE_FIRST_PX) / MUSE_CALL_SWIPE_STEP_PX;
    return dy < 0 ? steps : -steps;
}

/* The volume (0-100) steps sixteenths from pct, on the sixteenths' grid, clamped. */
static inline int muse_call_volume_step(int pct, int steps)
{
    int level = (pct * MUSE_CALL_VOLUME_LEVELS + 50) / 100 + steps;
    level = level < 0 ? 0 : level > MUSE_CALL_VOLUME_LEVELS ? MUSE_CALL_VOLUME_LEVELS : level;
    return (level * 100 + MUSE_CALL_VOLUME_LEVELS / 2) / MUSE_CALL_VOLUME_LEVELS;
}

/* Screen pixel sx, sy is inside the panel's real outline with `spare`
 * pixels to spare (its center, against a round panel's edge or a corner's
 * arc; doubled coordinates, no square roots). */
static inline int muse_call_in_outline(const muse_call_panel_t *P, int sx, int sy, int spare)
{
    const long w = P->screen_w, h = P->screen_h;
    if (sx < spare || sy < spare || sx >= w - spare || sy >= h - spare) {
        return 0;
    }
    long px = 2L * sx + 1, py = 2L * sy + 1;   /* the pixel's center, doubled */
    if (P->round) {
        long dx = px - w, dy = py - h, r = w - 2L * spare;
        return dx * dx + dy * dy <= r * r;
    }
    long c = P->corner;
    if (c <= 0) {
        return 1;
    }
    long ax = px < 2 * c ? 2 * c : px > 2 * (w - c) ? 2 * (w - c) : px;
    long ay = py < 2 * c ? 2 * c : py > 2 * (h - c) ? 2 * (h - c) : py;
    long dx = px - ax, dy = py - ay, r = 2 * (c - spare);
    return dx * dx + dy * dy <= r * r;
}

/* What a tap at x, y (relative to the panel) lands on. The buttons take a
 * margin around what's drawn: fingers are bigger than the circles. − and +
 * also reach out to the screen's side edges across their rows. No hit area
 * reaches past the panel's real outline (MUSE_CALL_SPARE_PX inside it). */
static inline muse_call_hit_t muse_call_ctl_hit(const muse_call_panel_t *P, int x, int y)
{
    const int m = MUSE_CALL_HIT_PX;
    if (!muse_call_in_outline(P, P->x + x, P->y + y, MUSE_CALL_SPARE_PX)) {
        return MUSE_CALL_HIT_NONE;
    }
    if (x >= MUSE_CALL_END_X0 - m && x < MUSE_CALL_END_X1 + m && y >= MUSE_CALL_END_HIT_Y0 &&
        y < MUSE_CALL_END_HIT_Y1 + P->end_down) {
        return MUSE_CALL_HIT_END;
    }
    if (y >= MUSE_CALL_BTN_HIT_Y0 - P->btn_up && y < MUSE_CALL_BTN_HIT_Y1) {
        if (x >= -P->x && x < MUSE_CALL_BTN + m) {
            return MUSE_CALL_HIT_MINUS;
        }
        if (x >= MUSE_CALL_CTL_W - MUSE_CALL_BTN - m && x < P->screen_w - P->x) {
            return MUSE_CALL_HIT_PLUS;
        }
    }
    return MUSE_CALL_HIT_NONE;
}

/*
 * What a tap does to the controls (a tap away from the controls dismisses
 * them). up:
 * they were up when this touch went down (the touch that showed them, at its
 * touch-down or as a tap's lift, is never a tap away: its own lift keeps
 * what it showed). Hidden, a tap brings them up, 3 s from it; up, a tap on a
 * control acts and re-arms the 3 s; up, a tap away from the controls
 * (anywhere on the picture that isn't a control) dismisses them at once. A
 * press held MUSE_CALL_TALK_HOLD_US or longer (held) isn't a tap: away from
 * the controls it keeps them, 3 s from its lift. With no touch they hide at
 * 3 s. *until_us: when they hide (now_us: hidden now).
 */
#define MUSE_CALL_CONTROLS_SHOW_US (3 * 1000000LL)

typedef enum {
    MUSE_CALL_TAP_SHOWN,       /* they were hidden: the tap only brings them up */
    MUSE_CALL_TAP_DISMISSED,   /* up, a tap away from the controls: hidden at once */
    MUSE_CALL_TAP_CONTROL,     /* up, a tap on a control: the control acts, they stay */
    MUSE_CALL_TAP_KEPT,        /* up, a hold away from the controls (not a tap): they stay */
} muse_call_tap_t;

static inline muse_call_tap_t muse_call_tap(long long now_us, long long *until_us, int up, muse_call_hit_t hit,
                                            int held)
{
    if (up && hit == MUSE_CALL_HIT_NONE && !held) {
        *until_us = now_us;
        return MUSE_CALL_TAP_DISMISSED;
    }
    *until_us = now_us + MUSE_CALL_CONTROLS_SHOW_US;
    if (!up) {
        return MUSE_CALL_TAP_SHOWN;
    }
    return hit == MUSE_CALL_HIT_NONE ? MUSE_CALL_TAP_KEPT : MUSE_CALL_TAP_CONTROL;
}

/* The controls show at now_us (until_us: from the last tap or change). */
static inline int muse_call_controls_up(long long now_us, long long until_us)
{
    return now_us < until_us;
}

/*
 * When a touch brings the controls up (from filmed calls, a touch-to-controls
 * bar of <= 50 ms p90):
 *
 * - Where the picture isn't a talk surface (a hold there starts no talk
 *   turn: the board talks on its button, Settings > Talk is Button, the call
 *   video or the calling screen, which never talk), they come up at the
 *   TOUCH-DOWN. A drag or a hold keeps them up, their 3 s counting from the
 *   last touch (the lift). The lift is still a tap: up at the touch-down,
 *   it acts on the control under it (End call, - or +) or, away from them,
 *   dismisses them (muse_call_tap); hidden at the touch-down, it only
 *   brought them up, and its own lift never dismisses what it showed.
 * - Where a hold is a talk press (the home's hold region with Settings >
 *   Talk on Screen or Both), they come up at a tap's lift, as before; a
 *   press held MUSE_CALL_TALK_HOLD_US or longer is talk, and shows nothing at
 *   its lift (no controls flash after a turn).
 */
#define MUSE_CALL_TALK_HOLD_US (250 * 1000LL)   /* the hold threshold (muse_home.c's TALK_HOLD_MS) */

typedef struct {
    int down;             /* a finger is down */
    int early;            /* not a talk surface: the controls came up at its touch-down */
    int was_up;           /* the controls were up at the touch-down (a tap acts on them) */
    int talk;             /* a talk surface: a hold here is a talk press */
    long long t0_us;      /* the touch-down */
} muse_call_touch_t;

typedef enum {
    MUSE_CALL_LIFT_NONE,  /* nothing more (a swipe, a talk press, or no press) */
    MUSE_CALL_LIFT_TAP,   /* a tap: tap() with was_up */
} muse_call_lift_t;

/* The touch-down. 1: the controls come up (or stay) now, 3 s from now. */
static inline int muse_call_touch_down(muse_call_touch_t *t, long long now_us, long long *until_us, int up,
                                       int talk_surface)
{
    t->down = 1;
    t->was_up = up;
    t->talk = talk_surface;
    t->t0_us = now_us;
    t->early = !talk_surface;
    if (t->early) {
        *until_us = now_us + MUSE_CALL_CONTROLS_SHOW_US;
    }
    return t->early;
}

/* The finger still down (a drag or a hold): controls that came up at the touch-down stay, 3 s from now. */
static inline void muse_call_touch_held(const muse_call_touch_t *t, long long now_us, long long *until_us)
{
    if (t->down && t->early) {
        *until_us = now_us + MUSE_CALL_CONTROLS_SHOW_US;
    }
}

/* The lift (moved: it went past a tap, a swipe). */
static inline muse_call_lift_t muse_call_touch_up(muse_call_touch_t *t, long long now_us, long long *until_us,
                                                  int moved)
{
    if (!t->down) {
        return MUSE_CALL_LIFT_NONE;
    }
    t->down = 0;
    if (t->early) {
        *until_us = now_us + MUSE_CALL_CONTROLS_SHOW_US;   /* 3 s from the last touch */
    }
    if (moved || (t->talk && now_us - t->t0_us >= MUSE_CALL_TALK_HOLD_US)) {
        return MUSE_CALL_LIFT_NONE;
    }
    return MUSE_CALL_LIFT_TAP;
}

/* The level bar's filled width for a volume. */
static inline int muse_call_bar_fill(int pct)
{
    pct = pct < 0 ? 0 : pct > 100 ? 100 : pct;
    return (MUSE_CALL_BAR_X1 - MUSE_CALL_BAR_X0) * pct / 100;
}

/* One ARGB8888 pixel (LVGL's order: blue, green, red, alpha) over one RGB565
 * pixel in the panel's byte order (big-endian), as video frames hold them. */
static inline void muse_call_blend565be(unsigned char *d, const unsigned char *s)
{
    unsigned a = s[3];
    if (!a) {
        return;
    }
    unsigned r = s[2], g = s[1], b = s[0];
    if (a < 255) {
        unsigned v = (unsigned)d[0] << 8 | d[1];
        unsigned dr = (v >> 11) << 3, dg = ((v >> 5) & 63) << 2, db = (v & 31) << 3;
        r = (r * a + dr * (255 - a) + 127) / 255;
        g = (g * a + dg * (255 - a) + 127) / 255;
        b = (b * a + db * (255 - a) + 127) / 255;
    }
    unsigned v = (r >> 3) << 11 | (g >> 2) << 5 | (b >> 3);
    d[0] = (unsigned char)(v >> 8);
    d[1] = (unsigned char)v;
}

/* ---- the panel drawn shape by shape (no canvas) --------------------------- */

/* rgb (0xRRGGBB) over one RGB565 pixel in the panel's byte order (big-endian)
 * at coverage a (0-255), weights out of 256: no divisions. */
static inline void muse_call_blend_rgb(unsigned char *d, unsigned long rgb, unsigned a)
{
    if (!a) {
        return;
    }
    unsigned v;
    if (a >= 255) {
        v = (unsigned)((rgb >> 19 & 31) << 11 | (rgb >> 10 & 63) << 5 | (rgb >> 3 & 31));
    } else {
        unsigned w = a + (a >> 7), iw = 256 - w;
        v = (unsigned)d[0] << 8 | d[1];
        unsigned r = (unsigned)(((rgb >> 19 & 31) * w + (v >> 11) * iw) >> 8);
        unsigned g = (unsigned)(((rgb >> 10 & 63) * w + (v >> 5 & 63) * iw) >> 8);
        unsigned b = (unsigned)(((rgb >> 3 & 31) * w + (v & 31) * iw) >> 8);
        v = r << 11 | g << 5 | b;
    }
    d[0] = (unsigned char)(v >> 8);
    d[1] = (unsigned char)v;
}

/* Coverage (0-255) of pixel px, py by the rectangle x0..x1, y0..y1 (ends
 * exclusive) with corners of radius rad: a soft one-pixel edge, from twice
 * the distance between pixel centres and the corner circle's centre. */
static inline unsigned muse_call_rr_cover(int px, int py, int x0, int y0, int x1, int y1, int rad)
{
    if (px < x0 || px >= x1 || py < y0 || py >= y1) {
        return 0;
    }
    int dx = 0, dy = 0;
    if (2 * px + 1 < 2 * x0 + 2 * rad) {
        dx = 2 * x0 + 2 * rad - (2 * px + 1);
    } else if (2 * px + 1 > 2 * x1 - 2 * rad) {
        dx = 2 * px + 1 - (2 * x1 - 2 * rad);
    }
    if (2 * py + 1 < 2 * y0 + 2 * rad) {
        dy = 2 * y0 + 2 * rad - (2 * py + 1);
    } else if (2 * py + 1 > 2 * y1 - 2 * rad) {
        dy = 2 * py + 1 - (2 * y1 - 2 * rad);
    }
    if (!dx || !dy) {
        return 255;   /* along a straight edge */
    }
    int d2 = dx * dx + dy * dy, lo = 2 * rad - 1, hi = 2 * rad + 1;
    if (d2 <= lo * lo) {
        return 255;
    }
    if (d2 >= hi * hi) {
        return 0;
    }
    return (unsigned)(255 * (hi * hi - d2) / (hi * hi - lo * lo));
}

static inline int muse_call_isqrt(int v)
{
    int r = 0, bit = 1 << 30;
    if (v <= 0) {
        return 0;
    }
    while (bit > v) {
        bit >>= 2;
    }
    while (bit) {
        if (v >= r + bit) {
            v -= r + bit;
            r = (r >> 1) + bit;
        } else {
            r >>= 1;
        }
        bit >>= 2;
    }
    return r;
}

/*
 * A filled rounded rectangle x0..x1, y0..y1 (radius rad) in rgb into a piece:
 * dst's pixel (0, 0) is (px, py) in the same coordinates, w columns, rows rows,
 * stride bytes apart. Rows clear of the corners are solid; in a corner row the
 * integer square root gives the columns the circle reaches and fully covers,
 * and only the few between are blended (muse_call_rr_cover), so the cost is
 * the solid writes. The same pixels as blending muse_call_rr_cover everywhere.
 */
static inline void muse_call_rrect(unsigned char *dst, int stride, int px, int py, int w, int rows, int x0, int y0,
                                   int x1, int y1, int rad, unsigned long rgb)
{
    int r0 = y0 - py, r1 = y1 - py, c0 = x0 - px, c1 = x1 - px;
    r0 = r0 < 0 ? 0 : r0;
    r1 = r1 > rows ? rows : r1;
    c0 = c0 < 0 ? 0 : c0;
    c1 = c1 > w ? w : c1;
    if (r0 >= r1 || c0 >= c1) {
        return;
    }
    unsigned char solid[2] = { 0, 0 };
    muse_call_blend_rgb(solid, rgb, 255);
    for (int r = r0; r < r1; r++) {
        int y = py + r;
        unsigned char *row = dst + (long)r * stride;
        int cs = c0, ce = c1;   /* the solid run */
        if (rad > 0 && (y < y0 + rad || y >= y1 - rad)) {
            int dy = y < y0 + rad ? 2 * y0 + 2 * rad - (2 * y + 1) : 2 * y + 1 - (2 * y1 - 2 * rad);
            int lo = 2 * rad - 1, hi = 2 * rad + 1;
            int out = dy < hi ? muse_call_isqrt(hi * hi - dy * dy) : -1;
            int in = dy < lo ? muse_call_isqrt(lo * lo - dy * dy) : -1;
            int lcx2 = 2 * x0 + 2 * rad, rcx2 = 2 * x1 - 2 * rad;   /* the corner centres, doubled */
            /* Blend from a column before the circle's reach to the first fully covered one, and mirrored. */
            int lb = out >= 0 ? (lcx2 - out - 1) / 2 - 1 : x0 + rad;
            int ls = in >= 0 ? (lcx2 - in) / 2 + 1 : x0 + rad;
            int rb = out >= 0 ? (rcx2 + out + 1) / 2 + 1 : x1 - rad;
            int rs = in >= 0 ? (rcx2 + in) / 2 - 1 : x1 - rad;
            lb = lb < x0 ? x0 : lb;
            ls = ls > x0 + rad ? x0 + rad : ls;
            rb = rb > x1 ? x1 : rb;
            rs = rs < x1 - rad ? x1 - rad : rs;
            ls = ls < lb ? lb : ls;
            rs = rs > rb ? rb : rs;
            for (int c = lb - px > c0 ? lb - px : c0; c < c1 && c < ls - px; c++) {
                muse_call_blend_rgb(row + c * 2, rgb, muse_call_rr_cover(px + c, y, x0, y0, x1, y1, rad));
            }
            for (int c = rs - px > c0 ? rs - px : c0; c < c1 && c < rb - px; c++) {
                muse_call_blend_rgb(row + c * 2, rgb, muse_call_rr_cover(px + c, y, x0, y0, x1, y1, rad));
            }
            cs = ls - px > c0 ? ls - px : c0;
            ce = rs - px < c1 ? rs - px : c1;
        }
        for (int c = cs; c < ce; c++) {
            row[c * 2] = solid[0];
            row[c * 2 + 1] = solid[1];
        }
    }
}
