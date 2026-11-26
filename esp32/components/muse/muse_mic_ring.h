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
 * The mic ring: a solid red ring along the screen's edge for as long as the
 * mic is capturing for someone (a push-to-talk hold once capture starts, a
 * client's start_mic, a realtime voice call's open mic), drawn over everything: the face,
 * menus, camera video and UI streams. Static,
 * #E53935, about 2.2% of the screen's short side wide (8 px at 360, 10 at
 * 466, 11 at 480, 5 at 240). Its outer edge is the screen's rectangle
 * (whatever of it the panel shows); its inner edge follows the visible shape
 * inset by that width: a circle on a round screen, rounded corners on a
 * rectangular one.
 *
 * The voice loops say when capture starts and stops (muse_mic_ring_set).
 * LVGL draws it from muse_mic_ring_row() on its system layer (muse_ui.c); pictures drawn
 * straight to the panel (draw_frame: video, UI streams) get it from
 * muse_mic_ring_apply() on every band on its way to the panel
 * (boards/muse_lcd_bands.c), and the video view repaints when it changes.
 * No board-specific code.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MUSE_MIC_RING_RGB 0xE53935u    /* #E53935 */
#define MUSE_MIC_RING_RGB565 0xE1C7u   /* the same in RGB565 */

/* A rectangular panel's visible corner radius (muse_board_t.corner_px; the
 * bench console's >ring.corner=N tunes it on a board): the ring's inner edge
 * follows it, its outer edge is the screen's rectangle. */
void muse_mic_ring_set_corner(int corner_px);
int muse_mic_ring_corner(void);

/* Width for a screen: 2.2% of the short side, rounded, at least 2 px. */
int muse_mic_ring_width(int width, int height);

/* The screen's shape, once, before anything is drawn: a round panel (a ring
 * at the edge) or a rectangle with corners of corner_px (0: square). */
void muse_mic_ring_init(int width, int height, bool round, int corner_px);

/* Capture started (true) or stopped (false), from any task; cheap and
 * idempotent: a change bumps the generation and calls the listener. */
void muse_mic_ring_set(bool on);
bool muse_mic_ring_on(void);
uint32_t muse_mic_ring_generation(void);   /* changes on every on/off */

/* One listener (the video view), called on the setting task right after a
 * change: it must not block. */
void muse_mic_ring_set_listener(void (*fn)(bool on));

/* The ring's pixels in row y: outer span [*o0, *o1) and the inside to leave
 * alone [*i0, *i1) (empty when i0 >= i1). False when the row has none. */
bool muse_mic_ring_row(int y, int *o0, int *o1, int *i0, int *i1);

/* Paints the ring, if on, over a piece of a picture on its way to the
 * panel: rows y..y+rows-1, columns x..x+w-1 of the screen, at dst (RGB565 in
 * the panel's byte order, big-endian), stride bytes per row. */
void muse_mic_ring_apply(uint8_t *dst, int stride, int x, int y, int w, int rows);
/* The same with the ring's on/off taken once by the caller (muse_mic_ring_on()
 * at a picture's start): every band of one picture gets the same ring, so a
 * capture starting or stopping mid-picture never leaves a partial one. */
void muse_mic_ring_apply_as(bool on, uint8_t *dst, int stride, int x, int y, int w, int rows);

#ifdef __cplusplus
}
#endif
