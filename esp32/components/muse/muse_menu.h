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

#include "sdkconfig.h"
#if !CONFIG_MUSE_UI_FREEINK
#include "lvgl.h"
#endif

/*
 * Menu for boards without touch, in place of the settings tile.
 * On two-button boards, aux opens it and steps down; talk selects.
 * Keyboard boards use arrows, Enter and Esc, leaving Space/GO for talk.
 * Hints above the buttons say what each one does.
 */

typedef enum {
    MUSE_MENU_DOWN,     /* aux button: opens the menu, then moves down */
    MUSE_MENU_SELECT,   /* Enter, or talk button on two-button boards */
    MUSE_MENU_UP,
    MUSE_MENU_LEFT,     /* decrease the selected value */
    MUSE_MENU_RIGHT,    /* increase the selected value */
    MUSE_MENU_BACK,     /* open from the face, otherwise back/cancel */
} muse_menu_key_t;

/* Safe from any task. */
void muse_menu_key(muse_menu_key_t key);
bool muse_menu_is_open(void);

/* LVGL task only. */
#if !CONFIG_MUSE_UI_FREEINK
void muse_menu_build(lv_obj_t *parent, int w, int h);
#endif
/* Handles queued keys; returns true while the menu covers the face. */
bool muse_menu_tick(float now);
void muse_menu_close(void);
