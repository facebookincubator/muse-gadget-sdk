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

typedef void (*button_cb)(void);

bool button_init(button_cb on_short_press, button_cb on_double_press,
                 button_cb on_long_press);

// For a setup button that isn't a GPIO (one read over I2C, say): set before
// button_init(), which then reads it with `read` instead of
// CONFIG_HOMEHUB_BUTTON_GPIO. `read` stores whether the button is down and
// returns true, or returns false, leaving it alone, if it couldn't read it: a
// failed read is neither a press nor a release.
typedef bool (*button_read_cb)(bool *pressed);

void button_set_reader(button_read_cb read);

// Called from the button task on each press and release. Returning true from
// a press claims it: it then counts as neither a tap nor a hold, and its
// release is reported too. Must not block.
typedef bool (*button_press_cb)(bool pressed);

void button_set_press_cb(button_press_cb cb);
