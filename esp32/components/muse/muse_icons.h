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
#include "muse_home_ui.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MUSE_ICON_CALL,
    MUSE_ICON_CALL_END,
    MUSE_ICON_VOLUME_UP,
    MUSE_ICON_VOLUME_OFF,
    MUSE_ICON_MIC,
    MUSE_ICON_BLUETOOTH,
    MUSE_ICON_WIFI,
    MUSE_ICON_POWER,
    MUSE_ICON_INFO,
    MUSE_ICON_VIDEOCAM,
    MUSE_ICON_ADD,
    MUSE_ICON_REMOVE,
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

#if MUSE_HOME_UI
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
#else
/* Without the set (no v1.0 home, muse_home_ui.h): the font's symbol for
 * each, as a label in its color; no masks. */
static inline const char *muse_icon_symbol(muse_icon_t icon)
{
    switch (icon) {
    case MUSE_ICON_VOLUME_UP: return LV_SYMBOL_VOLUME_MAX;
    case MUSE_ICON_VOLUME_OFF: return LV_SYMBOL_MUTE;
    case MUSE_ICON_MIC: return LV_SYMBOL_AUDIO;
    case MUSE_ICON_BLUETOOTH: return LV_SYMBOL_BLUETOOTH;
    case MUSE_ICON_WIFI: return LV_SYMBOL_WIFI;
    case MUSE_ICON_POWER: return LV_SYMBOL_POWER;
    case MUSE_ICON_INFO: return LV_SYMBOL_LIST;
    case MUSE_ICON_VIDEOCAM: return LV_SYMBOL_VIDEO;
    case MUSE_ICON_DISPLAY: return LV_SYMBOL_IMAGE;
    case MUSE_ICON_BATTERY: return LV_SYMBOL_BATTERY_FULL;
    case MUSE_ICON_HOME: return LV_SYMBOL_HOME;
    case MUSE_ICON_DEVELOPER: return LV_SYMBOL_SETTINGS;
    case MUSE_ICON_CHARGING: return LV_SYMBOL_CHARGE;
    case MUSE_ICON_BACKSPACE: return LV_SYMBOL_BACKSPACE;
    default: return "";
    }
}

static inline const lv_image_dsc_t *muse_icon(muse_icon_t icon, int px)
{
    (void)icon;
    (void)px;
    return NULL;
}

static inline const lv_image_dsc_t *muse_icon_ink(muse_icon_t icon, int ink_h)
{
    (void)icon;
    (void)ink_h;
    return NULL;
}

static inline void muse_icon_set(lv_obj_t *label, muse_icon_t icon, int px, uint32_t color)
{
    (void)px;
    lv_label_set_text(label, muse_icon_symbol(icon));
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
}

static inline lv_obj_t *muse_icon_create(lv_obj_t *parent, muse_icon_t icon, int px, uint32_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_20, 0);
    lv_obj_remove_flag(label, LV_OBJ_FLAG_CLICKABLE);
    muse_icon_set(label, icon, px, color);
    lv_obj_center(label);
    return label;
}
#endif

/* The standard button disc's glyph box: 52 % of its diameter. */
static inline int muse_icon_button_glyph(int diameter)
{
    return diameter * 52 / 100;
}

#ifdef __cplusplus
}
#endif
