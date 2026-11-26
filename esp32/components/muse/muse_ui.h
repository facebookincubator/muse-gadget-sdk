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

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "sdkconfig.h"

/*
 * Bring up the display and build the UI: the avatar on the main tile,
 * settings one swipe left, and optional Latest reply one swipe right. Also owns screen sleep and brightness.
 */
esp_err_t muse_ui_start(void);
/* What's on screen, for the bench: "face", "settings", "settings/sleep",
 * "menu", "image". Under the display lock. */
void muse_ui_page_name(char *buf, size_t n);

/* From any task: the screen has gone dark for sleep (and not yet woken). */
bool muse_ui_dark(void);

/* From any task: the brightness a running app asks for, 1 to 100, or 0 for
 * the person's own setting again. */
void muse_ui_app_brightness(int pct);

/* From any task: the face tile is the currently visible tile. */
bool muse_ui_face_active(void);
/* From any task: a vertical volume drag owns the current touch (no hold PTT). */
bool muse_ui_volume_swiping(void);

/* The face has the v1.0 home's screen hold region:
 * set once the UI is up, then constant. */
bool muse_ui_has_hold_region(void);
/* The touch now down (or the last one, until the next touch-down) is a
 * swipe: it moved past the slop from where it went down, or a page or list
 * started scrolling with it (muse_swipe.h). The control under its start
 * gets no tap or hold: LVGL's clicks and long presses are stopped here, and
 * the controls that time their own holds (the home's Call button and hold
 * region) ask this. LVGL task. */
bool muse_ui_touch_swiped(void);

/* Bench and renderer: shows "face", "status", "settings" or
 * "settings/<page>" ("sound", "display", "talk", "about"...). LVGL task. */
void muse_ui_goto(const char *page);
/* From any task: runs fn(arg) on the LVGL task at its next frame (bench
 * presses that read LVGL's objects). False: one is already waiting, or arg
 * is longer than 47 bytes. */
bool muse_ui_run_in_lvgl(void (*fn)(const char *arg), const char *arg);

/* The functions below run in the LVGL task (or with the display lock held). */

/* Slide back to the face (e.g. when a talk starts). */
void muse_ui_show_face(void);
/* Settings sub-pages turn off the tile swipe so they can use horizontal gestures. */
void muse_ui_set_swipe_enabled(bool enabled);
/* Temporarily applies a brightness while a slider is dragged. */
void muse_ui_preview_brightness(int pct);
/* Whether this screen's replies follow the Text size setting: a bigger
 * screen's reading page, or a small one's full-screen reply. */
bool muse_ui_text_size_applies(void);

/*
 * display.draw_url, from any task. An image covers the face until a tap, a
 * talk, the menu or muse_ui_image_hide(). Pixels are RGB565, high byte first.
 * The size is false without PSRAM for the image. Before the UI is up, the
 * LVGL UI already gives the board's size, so display.* can be listed early;
 * draws fail until the UI starts.
 */
bool muse_ui_image_size(int *w, int *h);
/* The panel is round: the image's corners are cut off. */
bool muse_ui_image_round(void);
/* What the panel shows of those pixels: 16 (colour), or 2 on a four-gray e-paper. */
int muse_ui_image_bits(void);
/* The image is complete: a four-gray e-paper dithers it to its grays. */
void muse_ui_image_done(void);
bool muse_ui_image_draw(int x, int y, int w, int h, const uint16_t *pixels);
void muse_ui_image_hide(void);
/* From any task: hide the image ms from now (display.show_image's showing),
 * from the frame tick; 0 or less cancels a pending hide. A new image's first
 * rectangle or any hide cancels it too. Logged on mg.image. */
void muse_ui_image_hide_after(int ms);
/*
 * The avatar face (main/mg_face.h), from any task: px is a full-panel
 * RGB565 picture (native order, heap_caps_malloc'd, the scrim already baked
 * in) that the home draws under everything else on its face in place of
 * Pixel Muse; muse_ui takes it and frees it when it's replaced. NULL brings
 * Pixel Muse back. Applied on the next frame; turns and the turn dot
 * and every control work as on the plain face.
 */
void muse_ui_face_bg_set(uint16_t *px);
/* The avatar's plain picture (the JPEG decoded at the panel's size: no Size,
 * no scrim), handed over like muse_ui_face_bg_set's (takes px, NULL for none;
 * any task). The calling screen shows it at 40 % behind
 * "Calling…" while the avatar face is picked and stored. */
void muse_ui_face_plain_set(uint16_t *px);
/* That picture for LVGL (an lv_image_dsc_t *), NULL with none; *ver changes
 * whenever it does: set the image's source again then (LVGL task only). */
const void *muse_ui_face_image(uint32_t *ver);
/* Watcher camera mode: shows an on-screen shutter hint over the live image. */
void muse_ui_camera_hint(bool visible);

/* Bench testing, from any task: streams the screen over USB serial. */
void muse_ui_request_snapshot(void);
/* The update card asks for its confirm press and is on show (muse_local_press_set_shown). Any task. */
bool muse_ui_update_prompt_shown(void);
#if CONFIG_LV_USE_SNAPSHOT
/* Bench-only, from any task: select the touch tile without pretending a/s
 * are touch-menu navigation. No effect on non-touch boards. */
void muse_ui_bench_page(bool settings);
/* Display-only demo name for privacy-safe screenshots; no provisioning changes. */
void muse_ui_bench_demo(bool enabled);
#endif

/* Bench (console ui.redraw=N): N timed full redraws of the screen on show (min, average, max microseconds).
 * On the LVGL task only (muse_ui_run_in_lvgl). */
void muse_ui_redraw_cost(int n, uint32_t *min_us, uint32_t *avg_us, uint32_t *max_us);
