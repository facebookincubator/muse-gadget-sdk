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

/*
 * The gadget owns its speaker and shows every change it didn't make itself
 * (the speaker is controlled on the device: the phone never mutes it without
 * the device showing it). The parts with no LVGL in them,
 * for the host tests:
 *
 * - muse_vol_watch(): polled by the UI's frame tick with
 *   muse_settings_volume_changes(); says when a change made from outside
 *   (an app's set_settings speaker_volume, Muse's phone setup, the console's
 *   vol=) has landed since the last poll, and the source line to show beside
 *   the volume overlay. The gadget's own changes show as they always have;
 *   a boot's restore from NVS isn't a change (the count starts at 0 and
 *   counts none of it).
 * - muse_vol_muted(): the muted state the top row and the Sound page show
 *   for as long as it lasts (speaker_volume 0: the speaker off).
 */

#include <stdbool.h>
#include <stdint.h>

#include "muse_settings.h"

/* The overlay's source line: short, sentence case. */
#define MUSE_VOL_BY_PHONE_TEXT "Set by phone"
#define MUSE_VOL_BY_BENCH_TEXT "Set by bench"

static inline const char *muse_vol_by_text(muse_vol_by_t by)
{
    return by == MUSE_VOL_BY_PHONE ? MUSE_VOL_BY_PHONE_TEXT : by == MUSE_VOL_BY_BENCH ? MUSE_VOL_BY_BENCH_TEXT : NULL;
}

typedef struct {
    uint32_t seen;   /* the change count last polled (0 at boot, as the count) */
} muse_vol_watch_t;

/* The source line when changes landed since the last poll and the latest was
 * made from outside, else NULL (none, or the gadget's own). Several changes
 * between two polls (a client's set is the level and then the switch) show
 * once. */
static inline const char *muse_vol_watch(muse_vol_watch_t *w, uint32_t changes, muse_vol_by_t by)
{
    if (changes == w->seen) {
        return NULL;
    }
    w->seen = changes;
    return muse_vol_by_text(by);
}

/* speaker_volume 0: the muted icon shows. */
static inline bool muse_vol_muted(bool speaker_on, int volume)
{
    return !speaker_on || volume <= 0;
}
