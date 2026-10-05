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
#include <stdint.h>

#include "esp_err.h"

/*
 * Bring up the display and build the UI: the avatar on the first tile,
 * settings one swipe to the left. Also owns screen sleep and brightness.
 */
esp_err_t muse_ui_start(void);

/* From any task: the screen has gone dark for sleep (and not yet woken). */
bool muse_ui_dark(void);

/* From any task: the UI is built (and the display lock exists). */
bool muse_ui_ready(void);

/* The functions below run in the LVGL task (or with the display lock held). */

/* Slide back to the face (e.g. when a talk starts). */
void muse_ui_show_face(void);
/* A talk starting in the next second stays on the page it started from (an
 * app's talk button) instead of sliding to the face. */
void muse_ui_keep_page(void);
/* Settings sub-pages turn off the tile swipe so they can use horizontal gestures. */
void muse_ui_set_swipe_enabled(bool enabled);
/* Temporarily applies a brightness while a slider is dragged. */
void muse_ui_preview_brightness(int pct);

/*
 * display.draw_url, from any task. An image covers the face until a tap, a
 * talk, the menu or muse_ui_image_hide(). Pixels are RGB565, high byte first.
 * The size is false without PSRAM for the image or before the UI is up.
 */
bool muse_ui_image_size(int *w, int *h);
bool muse_ui_image_draw(int x, int y, int w, int h, const uint16_t *pixels);
void muse_ui_image_hide(void);
/* Watcher camera mode: shows an on-screen shutter hint over the live image. */
void muse_ui_camera_hint(bool visible);

/*
 * display.show_text, from any task: wrapped, centred text in fg on bg
 * (0xRRGGBB), size 0 small to 2 large. It covers the face the way an image
 * does, replaces one, and goes the same ways (muse_ui_image_hide included).
 */
bool muse_ui_text_show(const char *text, uint32_t fg, uint32_t bg, int size);

/* display.show_ui: a title, text, a progress bar and buttons, covering the
 * face as text does. A tap on the background doesn't close it; a button
 * press goes to muse_hw_button(id). */
#define MUSE_UI_BUTTONS_MAX 6
typedef struct {
    char id[16];
    char label[24];
    uint32_t color;           /* 0xRRGGBB */
} muse_ui_button_t;

typedef struct {
    const char *title;        /* NULL or "": none */
    const char *text;
    int progress;             /* 0..100, or -1 for none */
    uint32_t fg, bg, accent;  /* text, background, progress bar */
    int n_buttons;
    muse_ui_button_t buttons[MUSE_UI_BUTTONS_MAX];
} muse_ui_panel_t;

/* From any task. */
bool muse_ui_panel_show(const muse_ui_panel_t *panel);

/* The pages' tileview and the face's tile, for muse_apps.c (the LVGL task,
 * or with the display lock); NULL on boards without touch. */
struct _lv_obj_t;
struct _lv_obj_t *muse_ui_tileview(void);
struct _lv_obj_t *muse_ui_face_tile(void);
struct _lv_obj_t *muse_ui_settings_tile(void);
struct _lv_obj_t *muse_ui_pet_tile(void);

/* Bench testing, from any task: streams the screen over USB serial. */
void muse_ui_request_snapshot(void);
