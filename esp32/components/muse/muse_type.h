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
 * The v1.0 UI's type: every
 * text sized by its capitals (the time by its digits) as a share of the
 * panel's short side, from the reference layout. The four ESP32 catalog
 * boards get fonts converted at their panel's sizes (tools/muse/gen_ui_assets.py,
 * fonts/type/); every other board gets the nearest of LVGL's built-in
 * Montserrat sizes. A board compiles only its own set (CMakeLists.txt).
 */

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const lv_font_t *time;    /* the home's time (digits, ':' and '-' only) */
    const lv_font_t *date;    /* the home's date / state line */
    const lv_font_t *title;   /* page titles ("Status", "Settings"), SemiBold */
    const lv_font_t *conn;    /* the Status page's connection line */
    const lv_font_t *row;     /* the Status page's rows */
    const lv_font_t *label;   /* Settings row labels */
    const lv_font_t *value;   /* Settings row values */
    const lv_font_t *small;   /* the not-set-up home's "Open Muse app" */
    const lv_font_t *digit;   /* the passcode keypad's digits (digits only) */
} muse_type_t;

/* The set for a panel whose short side is `s` px. */
const muse_type_t *muse_type(int s);

/* Where a glyph's ink starts below the top of a label set in `font` (the
 * reference places text by its ink: the time by its digits' top, the rest by
 * their capitals'). */
int muse_type_ink_top(const lv_font_t *font, uint32_t letter);

#ifdef __cplusplus
}
#endif
