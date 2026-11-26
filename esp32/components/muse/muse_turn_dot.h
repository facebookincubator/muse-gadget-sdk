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
 * The turn's status dot: on every screen a talk
 * turn can start from that doesn't show the Muse avatar (Status, Settings
 * and its pages, the button menu, a picture Muse drew, the phone camera, UI
 * streams), one small breathing dot at the same spot, top
 * center just inside the red mic ring, clear of the time and the ring:
 *   - thinking (stop_mic until the reply's first audio):
 *     #E07BFF, breathing every 1.6 s;
 *   - responding (reply audio playing): #6FF0BF, every 0.96 s.
 * Nothing at listening (the ring says it), nothing outside a turn,
 * never text, and nothing during a realtime voice call (its screens stay
 * as the call has them). It follows
 * muse_state's mode, which every turn sets (the Wi-Fi voice loops, a
 * musegadgets client's assistant_state), so BLE and Wi-Fi turns show alike.
 *
 * Drawn the way the mic ring is: painted into
 * whatever goes to the panel, never an LVGL object. muse_ui.c paints it at
 * LV_EVENT_FLUSH_START and invalidates only its box when its look changes
 * (at most 12.5 times a second); boards/muse_lcd_bands.c paints it on every
 * band drawn straight to the panel (video, UI streams), and the video view
 * repaints its box when it breathes over a still picture. No board code.
 */

#include <stdbool.h>
#include <stdint.h>

#include "muse_state.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MUSE_TURN_THINKING_RGB 0xE07BFFu     /* the thinking accent */
#define MUSE_TURN_RESPONDING_RGB 0x6FF0BFu   /* the speaking accent */

typedef enum {
    MUSE_TURN_DOT_OFF = 0,
    MUSE_TURN_DOT_THINKING,
    MUSE_TURN_DOT_RESPONDING,
} muse_turn_dot_kind_t;

/* The screen's shape, once, before anything is drawn (as muse_mic_ring_init). */
void muse_turn_dot_init(int width, int height, bool round, int corner_px);

/* Hold still at full size (e-paper: a breathing dot would refresh the panel
 * a dozen times a second). */
void muse_turn_dot_set_static(bool still);

/* What a mode shows: thinking or responding, else off (listening is the
 * ring; idle, error, boot and off show nothing). */
muse_turn_dot_kind_t muse_turn_dot_kind(muse_mode_t mode);

/* Whether an avatar's own thinking / speaking accents are these: the
 * default avatar's are; a custom one (components/muse/avatar/) may not be,
 * and then the dot shows over it too. */
bool muse_turn_dot_avatar_ok(uint32_t thinking_rgb, uint32_t speaking_rgb);

/* A realtime voice call is on (muse_voice_in_call): the dot stays off whatever the mode, on
 * every path, until it's over. The UI sets it every mode watch; any task. */
void muse_turn_dot_set_in_call(bool in_call);
bool muse_turn_dot_in_call(void);

/* What the screen should show now (OFF where the avatar shows the turn),
 * at now_us. True when the dot's look changed (shown, hidden, its color or
 * its breath step): its box needs drawing again. Cheap; any task. */
bool muse_turn_dot_update(muse_turn_dot_kind_t kind, int64_t now_us);

/* What the last update decided. */
muse_turn_dot_kind_t muse_turn_dot_shown(void);
/* Changes on every look change. */
uint32_t muse_turn_dot_generation(void);
/* The dot's color for a kind, 0xRRGGBB (0 for OFF). */
uint32_t muse_turn_dot_rgb(muse_turn_dot_kind_t kind);

/* Its box on the screen, end exclusive: whole groups of 4 columns (the
 * SPD2010's windows) and even rows. All of the dot's pixels are inside it. */
void muse_turn_dot_box(int *x0, int *y0, int *x1, int *y1);
/* Its center and full radius (for tests and the docs). */
void muse_turn_dot_geometry(float *cx, float *cy, float *radius);

/* Paints the dot as it looks now, if shown, over rows y..y+rows-1, columns
 * x..x+w-1 of the screen at dst (RGB565), stride bytes per row, blended
 * into what is there; big_endian: the panel's byte order (else the CPU's,
 * little-endian). Pixels outside its box are never touched. */
void muse_turn_dot_paint(uint8_t *dst, int stride, int x, int y, int w, int rows, bool big_endian);
/* The same in the panel's byte order, for pictures drawn straight to it
 * (as muse_mic_ring_apply). */
void muse_turn_dot_apply(uint8_t *dst, int stride, int x, int y, int w, int rows);

#ifdef __cplusplus
}
#endif
