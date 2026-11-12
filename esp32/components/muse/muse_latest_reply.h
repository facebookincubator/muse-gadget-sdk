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

/* LVGL-task-only view of the latest locally received answer. */
typedef const lv_font_t *(*muse_latest_reply_font_t)(const char *text);
void muse_latest_reply_build(lv_obj_t *tileview, muse_latest_reply_font_t font_for);
lv_obj_t *muse_latest_reply_tile(void);
void muse_latest_reply_set_visible(bool visible);
void muse_latest_reply_tick(void);

#if LV_USE_SNAPSHOT
/* Serial bench requests, queued for the LVGL task; false for unknown keys. */
bool muse_latest_reply_bench(const char *command);
#endif
