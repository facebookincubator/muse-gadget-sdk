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

// Xteink X3 pages: a short list of text pages the agent fills (pages.set,
// pages.clear) and the X3's buttons scroll through. The status screen is the
// first stop of the scroll. Pages are kept across restarts.

// The longest name, title and text a page takes, in characters.
#define X3_PAGE_NAME_MAX  15
#define X3_PAGE_TITLE_MAX 20
#define X3_PAGE_TEXT_MAX  460

// Load the saved pages and start reading the buttons. Call once the display
// is ready.
void x3_pages_start(void);

// Create or replace the page called `name`, and show it. `title` may be NULL
// (the name is used). Returns false when every slot is taken.
bool x3_pages_set(const char *name, const char *title, const char *text);

// Remove the page called `name`. Returns false if there is none.
bool x3_pages_clear(const char *name);
