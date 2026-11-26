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

#include "muse_battery_policy.h"

static muse_battery_level_t by_percent(muse_battery_level_t was, int pct)
{
    /* Down at the threshold, up again only past it plus the hysteresis. */
    int toast = MUSE_BATTERY_TOAST_PCT, warn = MUSE_BATTERY_WARN_PCT;
    if (was >= MUSE_BATTERY_LEVEL_WARN10) {
        toast += MUSE_BATTERY_HYSTERESIS_PCT;
    }
    if (was >= MUSE_BATTERY_LEVEL_WARN15) {
        warn += MUSE_BATTERY_HYSTERESIS_PCT;
    }
    if (pct <= toast) {
        return MUSE_BATTERY_LEVEL_WARN10;
    }
    if (pct <= warn) {
        return MUSE_BATTERY_LEVEL_WARN15;
    }
    return MUSE_BATTERY_LEVEL_OK;
}

muse_battery_level_t muse_battery_policy_step(muse_battery_policy_t *p, const muse_battery_thresholds_t *t,
                                              int pct, int mv, bool usb)
{
    if (usb || pct < 0) {
        /* External power, or no battery: nothing is low. A charge re-arms the toast. */
        p->level = MUSE_BATTERY_LEVEL_OK;
        p->low_readings = 0;
        if (usb) {
            p->toasted = false;
        }
        return p->level;
    }
    if (p->level == MUSE_BATTERY_LEVEL_CRITICAL) {
        return p->level;   /* shutting down: stays until power comes back */
    }
    if (t->shutdown_mv > 0 && mv > 0 && mv <= t->shutdown_mv) {
        if (++p->low_readings >= MUSE_BATTERY_SHUTDOWN_READINGS) {
            p->level = MUSE_BATTERY_LEVEL_CRITICAL;
            return p->level;
        }
    } else {
        p->low_readings = 0;
    }
    p->level = by_percent(p->level, pct);
    return p->level;
}
