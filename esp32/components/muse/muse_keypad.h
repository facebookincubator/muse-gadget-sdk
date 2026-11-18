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

#include "lvgl.h"

/*
 * A phone-style keypad for small touch screens: twelve big keys with a few
 * characters each. Tapping a key again within a second steps to its next
 * character; holding a letter key types its digit. Other keys switch to
 * digits and to symbols. Only one keypad exists at a time.
 */

/* Types into `ta`. Done sends LV_EVENT_READY to the keypad. Size and place it
 * like any object; the keys fill it in five rows. On a round screen the bottom
 * row keeps in from the sides, where the circle cuts it off. */
lv_obj_t *muse_keypad_create(lv_obj_t *parent, lv_obj_t *ta, bool round);

/* Back to lower-case letters with nothing pending. A password gets a key that
 * shows and hides it. */
void muse_keypad_reset(lv_obj_t *kp, bool password);

/* LVGL context: commit a pending multi-tap before external editing. */
lv_obj_t *muse_keypad_for_textarea(lv_obj_t *ta);
