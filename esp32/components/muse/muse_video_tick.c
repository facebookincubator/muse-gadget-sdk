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

#include "muse_video_tick.h"

#include <stdio.h>

#define TICK_US 1000000

void muse_vtick_start(muse_vtick_t *t, int64_t now_us, const muse_vcounts_t *c)
{
    t->start_us = t->last_us = now_us;
    t->last = *c;
    t->on = true;
}

void muse_vtick_stop(muse_vtick_t *t)
{
    t->on = false;
}

bool muse_vtick_due(const muse_vtick_t *t, int64_t now_us)
{
    return t->on && now_us - t->last_us >= TICK_US;
}

int muse_vtick_line(muse_vtick_t *t, int64_t now_us, const muse_vcounts_t *c, unsigned fps,
                         uint32_t internal_free, uint32_t internal_low, char *out, size_t cap)
{
    if (!t->on) {
        return 0;
    }
    unsigned long at = (unsigned long)((now_us - t->start_us) / 1000);
    unsigned long span = (unsigned long)((now_us - t->last_us) / 1000);
    unsigned long rx = (unsigned long)(c->rx - t->last.rx);
    unsigned shown = (uint16_t)(c->shown - t->last.shown);
    unsigned drop = (uint16_t)(c->dropped - t->last.dropped);
    unsigned long skip = (unsigned long)(c->skipped - t->last.skipped);
    unsigned long decerr = (unsigned long)(c->decode_errors - t->last.decode_errors);
    t->last = *c;
    t->last_us = now_us;
    return snprintf(out, cap, "av video: t=%lu span=%lu rx=%lu shown=%u drop=%u skip=%lu decerr=%lu fps=%u int=%lu low=%lu",
                    at, span, rx, shown, drop, skip, decerr, fps, (unsigned long)internal_free,
                    (unsigned long)internal_low);
}
