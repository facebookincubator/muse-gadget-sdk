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
 * The passcode lock's rules, pure C with no
 * IDF and no LVGL so the host tests run them (tests/test_muse_passcode.py):
 *
 *   - the stored record: SHA-256 over a random 16-byte salt, a format tag
 *     and the 4 digits, with the wrong count and the wait level;
 *   - when it locks: at screen-off, or 5 minutes after it, never during a
 *     Live call or a talk turn, and after 5 minutes without input where the
 *     screen stays on; a reboot starts locked;
 *   - wrong codes: 5 free, then 30 s, 1, 2, 4, 5 min (the cap), the count and
 *     level kept across reboots, a right code resetting them; never a wipe;
 *   - the keypad's geometry per panel (the digit ring on round
 *     touch panels, the 3 x 4 grid on rectangles) and its hit rules
 *     (touch-down enters a key at once, a swipe registers a key
 *     within 0.35 x the pitch of its center once the finger has moved 1.5 mm,
 *     a key repeats only after the finger left its cell, delete is a tap).
 *
 * The caller gives the clock (ms) and the SHA-256; it stores the record
 * whenever `dirty` is set.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MUSE_PASS_DIGITS 4
#define MUSE_PASS_SALT_LEN 16
#define MUSE_PASS_HASH_LEN 32
#define MUSE_PASS_FORMAT 2               /* the record's layout (1: an earlier one); any other reads as no passcode */
#define MUSE_PASS_FORMAT_1_SIZE 53       /* format 1's blob: one byte shorter (no reserved byte) */
#define MUSE_PASS_TAG "mg-passcode-v1"   /* hashed between the salt and the digits (the format tag) */
#define MUSE_PASS_FREE_TRIES 5           /* wrong codes in a row before the first wait */
#define MUSE_PASS_WAIT_LEVELS 5          /* 30 s, 1, 2, 4, 5 min */
#define MUSE_PASS_AFTER_MS (5 * 60 * 1000)      /* "After 5 min": the lock this long after screen-off */
#define MUSE_PASS_IDLE_MS (5 * 60 * 1000)       /* a screen that stays on: the lock after this long idle */
#define MUSE_PASS_KEYPAD_IDLE_MS 10000          /* the keypad untouched: back to the locked face */

typedef enum {
    MUSE_PASS_LOCK_SCREEN_OFF = 0,   /* default */
    MUSE_PASS_LOCK_AFTER_5_MIN = 1,
} muse_pass_lock_t;

/* The stored record (the settings store's "passcode" blob). Never sent,
 * printed or logged. */
typedef struct {
    uint8_t format;       /* MUSE_PASS_FORMAT */
    uint8_t on;
    uint8_t lock;         /* muse_pass_lock_t */
    uint8_t tries;        /* wrong codes in a row (saturates) */
    uint8_t wait_level;   /* the wait level in force once tries >= 5: 0..4 */
    uint8_t salt[MUSE_PASS_SALT_LEN];
    uint8_t hash[MUSE_PASS_HASH_LEN];
    /* Format 2's last byte: reserved, 0. */
    uint8_t reserved;
} muse_pass_rec_t;

/* False if the hash failed: then nothing is set, and no code is right. */
typedef bool (*muse_pass_sha256_t)(const uint8_t *data, size_t len, uint8_t out[32]);

typedef struct {
    muse_pass_rec_t rec;
    bool locked;
    bool dark;               /* the screen is off */
    bool after_due;          /* "After 5 min": a screen-off that will lock at dark_since + 5 min */
    int64_t dark_since_ms;
    int64_t wait_until_ms;   /* a wrong-code wait runs until then; 0: none */
    bool dirty;              /* rec changed: store it */
} muse_pass_t;

typedef enum {
    MUSE_PASS_RIGHT,
    MUSE_PASS_WRONG,        /* wrong, still free tries */
    MUSE_PASS_WAIT,         /* wrong, and a wait starts (the wait screen) */
    MUSE_PASS_WAITING,      /* refused unchecked: a wait is running */
} muse_pass_result_t;

/* rec: as loaded (NULL or another format: no passcode). A passcode that's on
 * starts locked; a stored count of 5 or more starts its level's wait again. */
void muse_pass_init(muse_pass_t *p, const muse_pass_rec_t *rec, int64_t now_ms);

/* The wait at a level: 30, 60, 120, 240, then 300 s. */
int muse_pass_wait_s(int level);

/* The screen turned off. busy: a Live call or a talk turn is on (no lock;
 * the next screen-off after it ends locks). True if this locked it. */
bool muse_pass_screen_off(muse_pass_t *p, int64_t now_ms, bool busy);
/* The screen woke: "After 5 min" locks here once 5 minutes passed. True if
 * this locked it. */
bool muse_pass_screen_on(muse_pass_t *p, int64_t now_ms);
/* Every frame or so. stays_on: the screen doesn't time out (USB power, or the
 * timeout Never): 5 minutes without input lock it. True if this locked it. */
bool muse_pass_tick(muse_pass_t *p, int64_t now_ms, uint32_t idle_ms, bool stays_on, bool busy);
/* A talk press: "After 5 min" locks here once 5 minutes passed in the dark (the
 * press can come before the tick or the screen-on applies it). True if this
 * locked it. */
bool muse_pass_press(muse_pass_t *p, int64_t now_ms);
/* The console's `pass lock` (a bench). No-op without a passcode. */
void muse_pass_lock_now(muse_pass_t *p);

/* Checks 4 digits ('0'-'9'). A right code unlocks and clears the count; a
 * wrong one counts and from the 5th starts a wait. */
muse_pass_result_t muse_pass_check(muse_pass_t *p, const char digits[MUSE_PASS_DIGITS], int64_t now_ms,
                                   muse_pass_sha256_t sha);
/* Milliseconds left of a running wait, else 0. */
int64_t muse_pass_wait_left_ms(const muse_pass_t *p, int64_t now_ms);

/* Settings > Passcode: a new code (the confirm step matched), on and
 * unlocked; salt from the secure RNG. False (nothing changed) if the hash
 * failed. */
bool muse_pass_set(muse_pass_t *p, const char digits[MUSE_PASS_DIGITS], const uint8_t salt[MUSE_PASS_SALT_LEN],
                   muse_pass_sha256_t sha);
/* Turn off (the current code was entered): the code is wiped. */
void muse_pass_turn_off(muse_pass_t *p);
void muse_pass_set_lock(muse_pass_t *p, muse_pass_lock_t lock);
/* The pairing's erase (Reset pairing, the app's Forget, a setup reset, a
 * completed Link setup, a factory reset): everything back to the default. */
void muse_pass_forget(muse_pass_t *p);

/* The console's `pass`: "on locked tries=0 wait=0", "off" (never the code,
 * the salt or the hash). */
void muse_pass_status(const muse_pass_t *p, int64_t now_ms, char *out, size_t n);

/* ---- the keypad's geometry and hit rules ---------------------------------- */

typedef enum {
    MUSE_PASS_RING,   /* round touch panels */
    MUSE_PASS_GRID,   /* rectangles and squares: 1 2 3 / 4 5 6 / 7 8 9 / blank 0 del */
} muse_pass_layout_t;

#define MUSE_PASS_KEY_DEL 10      /* the grid's delete cell */
#define MUSE_PASS_KEY_CENTER -2   /* the ring's center zone, or the grid's blank cell */
#define MUSE_PASS_KEY_NONE -1     /* above the grid (the prompt and dots) */

typedef struct {
    muse_pass_layout_t layout;
    int w, h, s;              /* panel px; s the short side */
    double ppm;               /* px per mm on the glass */
    int ring, edge, m;        /* the mic ring band, 1 % of S, 2 % of S */
    int prompt_caps, small_caps, dot_d, dot_pitch, dot_stroke, gap, slop_px;
    int digit_caps, del_box;
    double cx, cy;
    double pitch;             /* the ring's key pitch; the grid's smaller cell side */
    double key_mm;            /* a key cell's smaller side in mm */
    double dots_cy, del_cy;   /* the entry dots' line; the ring's delete / Cancel */
    /* ring */
    double rc, key_d, center_hit_r, center_vis_r, inner_r;
    /* grid */
    double kp_left, kp_top, kp_bot, cell_w, cell_h, inner_rx, inner_ry, key_w, key_h;
    int key_gap, key_r;
    double key_x[11], key_y[11];   /* digits 0-9, then the grid's delete */
} muse_pass_geom_t;

/* The sizes every layout shares, for a w x h
 * panel at ppm px/mm. round picks the ring. */
void muse_pass_geom_base(muse_pass_geom_t *g, int w, int h, bool round, double ppm);
/* The ring's keys; then the UI fits the prompt in center_vis_r and sets
 * del_box / del_cy with muse_pass_geom_ring_prompt(). */
void muse_pass_geom_ring(muse_pass_geom_t *g);
void muse_pass_geom_ring_prompt(muse_pass_geom_t *g, int prompt_caps_used);
/* The grid under the prompt: its lines (1 or 2) at prompt_caps_used. */
void muse_pass_geom_grid(muse_pass_geom_t *g, int prompt_lines, int prompt_caps_used);
/* The prompt's line pitch: 1.65 x its capitals. */
int muse_pass_prompt_pitch(int caps);

/* The cell under a point: a digit 0-9, MUSE_PASS_KEY_DEL, _CENTER or _NONE. */
int muse_pass_cell(const muse_pass_geom_t *g, double x, double y);
/* The point is inside the key's swipe area (0.35 x the pitch from its center). */
bool muse_pass_inner(const muse_pass_geom_t *g, int key, double x, double y);

/* One stroke (touch-down to lift). */
typedef struct {
    bool down;
    double x0, y0;        /* the landing */
    double lx, ly;        /* the last sample */
    bool slop_passed;     /* moved 1.5 mm from the landing */
    int land_cell;
    int blocked;          /* the key that registered last until the finger leaves its cell; -1 none */
    int under;            /* the key whose cell holds the finger (for the highlight); -1 none */
    bool entered;         /* a key registered in this stroke */
    uint16_t stroke_keys; /* the keys registered in this stroke (bit per digit) */
} muse_pass_stroke_t;

#define MUSE_PASS_EV_DEL 10   /* an event: delete (or Cancel with no digits) */
#define MUSE_PASS_EV_MAX 8

/* tap=pass.swipe.<k1>,<k2>[,...]'s stroke (the bench's three-key swipe):
 * the keys' centres as corners, and on the ring the centre
 * between two keys (the way the reference's trail runs, a straight chord
 * otherwise crossing the ring); 0 for a key that isn't a single digit, fewer
 * than two keys, or more corners than max. */
int muse_pass_bench_path(const muse_pass_geom_t *g, const char *keys, int *xs, int *ys, int max);

/* Touch-down / a move / the lift at (x, y): the keys it registered (0-9) and
 * MUSE_PASS_EV_DEL, in order, into out; returns how many. A move is followed
 * along the line from the last sample, so a fast swipe can't skip a key. */
int muse_pass_stroke_down(muse_pass_stroke_t *st, const muse_pass_geom_t *g, double x, double y,
                          int out[MUSE_PASS_EV_MAX]);
int muse_pass_stroke_move(muse_pass_stroke_t *st, const muse_pass_geom_t *g, double x, double y,
                          int out[MUSE_PASS_EV_MAX]);
int muse_pass_stroke_up(muse_pass_stroke_t *st, const muse_pass_geom_t *g, double x, double y,
                        int out[MUSE_PASS_EV_MAX]);

#ifdef __cplusplus
}
#endif
