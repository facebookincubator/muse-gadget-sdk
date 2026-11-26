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

/*
 * Who stops LVGL's worker (esp_lv_adapter_pause/resume), counted: it runs
 * again only when nobody holds it. The screen-off path (a board's
 * display_pause, from the input task) and video (muse_video's show task,
 * drawing to the panel past LVGL) both stop it; each calling the adapter on
 * its own let one resume what the other still needed paused (LVGL flushing
 * while video drew to the same panel), and a resume landing between a
 * pause's request and the worker's acknowledgement left that pause waiting
 * for good. Calls are serialized here.
 *
 * Every pause is bounded (MUSE_LVGL_PAUSE_MS, one retry, then given up with
 * LVGL left running and an error on muse_ui), on top of the adapter's own
 * pause handshake fix (cmake/esp_lvgl_adapter_fix.cmake): nothing here ever
 * waits for good, and a lost acknowledgement recovers by itself.
 */
#define MUSE_LVGL_PAUSE_MS 500
typedef enum {
    MUSE_LVGL_HOLD_SCREEN = 1,   /* the screen is off */
    MUSE_LVGL_HOLD_VIDEO = 2,    /* video owns the panel */
} muse_lvgl_holder_t;

/* who holds (true) or lets go (false). Returns true while anyone holds. */
bool muse_lvgl_hold(muse_lvgl_holder_t who, bool hold);

/* Whether LVGL's worker is stopped now (false after a given-up pause). */
bool muse_lvgl_hold_stopped(void);
/* Pauses given up since boot. */
unsigned muse_lvgl_hold_gave_up(void);
