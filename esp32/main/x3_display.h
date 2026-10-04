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

// What the Xteink X3's e-paper shows besides led_status.h (epaper_x3_status.c).
// Both return at once, and the screen follows; false when it is not ready.

// Show a text message under the agent's name, in place of the status screen
// until led_status_show_animation(). Lines wrap at spaces and newlines.
// Printable ASCII; other bytes show as '?'.
bool x3_display_show_text(const char *text);

// Show a page of text under its own `title`, with "number / count" at the
// bottom, until led_status_show_animation().
bool x3_display_show_page(const char *title, const char *text, int number, int count);
