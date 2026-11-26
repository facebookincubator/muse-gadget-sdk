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

/* Settings' cards for other screens built like the menu (muse_settings_ui.c;
 * the passcode's wait and Forgot). LVGL task. */

#include <stdint.h>

#include "lvgl.h"
#include "muse_icons.h"

/* The menu's geometry, for screens built like it (the passcode's wait and
 * Forgot): a card's height, the column's width, the list's top, the
 * title's capitals' top and the type. After muse_settings_ui_build(). */
typedef struct {
    int row_h, list_w, list_top, title_top, pad, icon_px, icon_gap;
    const lv_font_t *title, *label, *value;
} muse_settings_ui_geom_t;
void muse_settings_ui_geom(muse_settings_ui_geom_t *g);
/* A card like a Settings row in parent (its width): the icon and the label
 * in their colors, a value if value_out, cb on a tap. */
lv_obj_t *muse_settings_ui_card(lv_obj_t *parent, muse_icon_t icon, uint32_t icon_color, const char *text,
                                uint32_t text_color, lv_obj_t **value_out, lv_event_cb_t cb, void *user);
/* The card rule on such a card card_w wide with `trailing` px
 * after the label (a value, a switch): a label that doesn't fit beside the
 * icon drops the icon; one line, never wrapped. */
void muse_settings_ui_card_fit(lv_obj_t *card, int card_w, int trailing);
