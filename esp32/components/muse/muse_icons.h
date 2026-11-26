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
 * The Muse Gadgets standard icon set: Material Icons glyphs as 8-bit alpha masks, generated at the
 * sizes the UI draws them (muse_icons_gen.h), tinted at draw time. Never a
 * hand-drawn phone or speaker.
 */

#pragma once

#include <stdbool.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MUSE_ICON_VOLUME_UP,
    MUSE_ICON_VOLUME_OFF,
    MUSE_ICON_MIC,
    MUSE_ICON_BLUETOOTH,
    MUSE_ICON_WIFI,
    MUSE_ICON_POWER,
    MUSE_ICON_INFO,
    MUSE_ICON_VIDEOCAM,
    MUSE_ICON_DISPLAY,     /* the Display row (brightness, screen timeout) */
    MUSE_ICON_BATTERY,     /* the Battery row */
    MUSE_ICON_HOME,        /* the Muse row (Material home, redrawn) */
    MUSE_ICON_DEVELOPER,   /* the Developer row */
    MUSE_ICON_CHARGING,    /* the top row's charging bolt */
    MUSE_ICON_LOCK,        /* the passcode lock: the locked face, the wait screen, Settings > Passcode */
    MUSE_ICON_TOUCH,       /* Settings > Passcode's Change passcode row */
    MUSE_ICON_BACKSPACE,   /* the passcode keypad's delete */
    MUSE_ICON_COUNT,
} muse_icon_t;

/* The icon's mask at the largest generated size not over px (the smallest
 * there is otherwise), as an A8 image; NULL if the set has no such glyph. */
const lv_image_dsc_t *muse_icon(muse_icon_t icon, int px);

/* An lv_image showing the icon in color, centered in parent. */
lv_obj_t *muse_icon_create(lv_obj_t *parent, muse_icon_t icon, int px, uint32_t color);

/* The top row's Wi-Fi or Bluetooth, cropped to its ink, ink_h px tall
 * (the battery's height): the largest generated that fits, or NULL. */
const lv_image_dsc_t *muse_icon_ink(muse_icon_t icon, int ink_h);

/* Changes an icon made by muse_icon_create(). */
void muse_icon_set(lv_obj_t *img, muse_icon_t icon, int px, uint32_t color);

/* The standard button disc's glyph box: 52 % of its diameter. */
static inline int muse_icon_button_glyph(int diameter)
{
    return diameter * 52 / 100;
}

#ifdef __cplusplus
}
#endif
