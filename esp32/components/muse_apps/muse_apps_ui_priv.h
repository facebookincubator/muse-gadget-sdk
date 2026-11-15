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
 * Between muse_apps_ui.c (the object table) and muse_apps_canvas.c (cv_*).
 * Everything here runs under LVGL's lock.
 */

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"

/* ---- muse_apps_ui.c gives the canvas ---- */

/* A live W_CANVAS object, with the canvas's own data in *ext (NULL until its
 * size is set); NULL for anything else. */
lv_obj_t *muse_apps_ui_canvas(int32_t handle, void **ext);
/* Pixels of images and canvases together: false, and nothing taken, past the
 * screen's own count (limits.image_pixels). */
bool muse_apps_ui_pixels_take(uint32_t n);
void muse_apps_ui_pixels_give(uint32_t n);
/* A FONT_ id's font, or NULL. */
const lv_font_t *muse_apps_ui_font(int32_t id);
/* A shared style owned by this app, or NULL for an invalid handle. */
const lv_style_t *muse_apps_ui_style(int32_t handle);

/* ---- muse_apps_canvas.c gives the object table ---- */

/* P_WIDTH or P_HEIGHT set on a canvas: (re)allocates its RGB565 buffer once
 * both are known. 0, GADGET_ERR_ARG or GADGET_ERR_NO_MEMORY. */
int32_t muse_apps_canvas_size(lv_obj_t *obj, void **ext, bool width, int32_t px);
/* The canvas's size as set so far (0 until set). */
int32_t muse_apps_canvas_get_size(void *ext, bool width);
/* The canvas object is being deleted: frees its buffer. */
void muse_apps_canvas_free(void *ext);
