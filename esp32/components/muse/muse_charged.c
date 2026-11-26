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

/* "Charged" for Display > Sleep when charged (muse_charged.h). */

#include "muse_charged.h"

static bool charged_now(const muse_charged_t *st, muse_charged_src_t src, const muse_charged_reading_t *r)
{
    if (!r->usb) {
        return false;   /* unplugged: the only thing that lets a latch go */
    }
    switch (src) {
    case MUSE_CHARGED_DONE:
        /* The charger's word, never the percentage: the first done of the
         * plug-in latches whatever the setting (top-ups after termination
         * drop done; they don't release it). */
        return r->charge_done || st->charged;
    case MUSE_CHARGED_PERCENT:
        if (r->battery_pct < 0 || r->battery_pct > 100) {
            return false;   /* unknown */
        }
        return r->battery_pct >= (st->charged ? MUSE_CHARGED_RELEASE_PCT : MUSE_CHARGED_TAKE_PCT);
    default:
        return false;
    }
}

bool muse_charged_read(muse_charged_t *st, muse_charged_src_t src, const muse_charged_reading_t *r)
{
    bool plugged = st->seen && !st->usb && r->usb;
    st->charged = charged_now(st, src, r);
    st->usb = r->usb;
    st->seen = true;
    st->known = true;
    return plugged;
}

void muse_charged_unknown(muse_charged_t *st, muse_charged_src_t src)
{
    if (src != MUSE_CHARGED_DONE) {
        st->charged = false;   /* % fallback: an unknown reading is below 90 % */
    }
    /* A done board's latch stays: a failed read isn't an unplug. USB's
     * state stays too (no plug-in edge when reads come back); only the hold
     * waits for a good reading (known). */
    st->known = false;
}

bool muse_charged_holds(muse_charged_src_t src, bool never_sleep, bool on_battery, const muse_charged_t *st)
{
    return src != MUSE_CHARGED_NONE && never_sleep && !on_battery && st->known && st->usb && st->charged;
}
