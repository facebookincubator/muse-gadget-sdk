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

/* The charge-dark decision, pure so the host tests run it. */

#include "muse_charge_dark.h"

#include <stddef.h>

static muse_cd_action_t done(muse_cd_state_t *st, muse_cd_action_t a, muse_cd_why_t w, muse_cd_why_t *why)
{
    if (why) {
        *why = w;
    }
    if (a != MUSE_CD_WAIT) {
        st->dark = false;
    }
    return a;
}

muse_cd_action_t muse_cd_decide(muse_cd_state_t *st, bool ok, const muse_cd_reading_t *r, int healthy_mv,
                                int64_t now_ms, muse_cd_why_t *why)
{
    if (!ok || r == NULL) {
        return done(st, MUSE_CD_BOOT, MUSE_CD_WHY_UNREADABLE, why);   /* fail open: USB powers the board */
    }
    if (!st->dark) {
        /* The first reading: hold only a USB power-up of a low cell nobody pressed a key for. */
        if (healthy_mv <= 0 || !r->usb || !r->usb_on) {
            return done(st, MUSE_CD_BOOT, MUSE_CD_WHY_NOT_USB, why);
        }
        if (r->by_key) {
            return done(st, MUSE_CD_BOOT, MUSE_CD_WHY_KEY, why);
        }
        if (r->battery_mv <= 0) {
            return done(st, MUSE_CD_BOOT, MUSE_CD_WHY_NO_BATTERY, why);
        }
        if (r->battery_mv >= healthy_mv) {
            return done(st, MUSE_CD_BOOT, MUSE_CD_WHY_HEALTHY, why);
        }
        st->dark = true;
        st->healthy_n = 0;
        st->unplugged_n = 0;
        st->since_ms = now_ms;
        return done(st, MUSE_CD_WAIT, MUSE_CD_WHY_LOW, why);
    }
    if (now_ms - st->since_ms >= MUSE_CD_MAX_MS) {
        return done(st, MUSE_CD_BOOT, MUSE_CD_WHY_LIMIT, why);
    }
    if (r->by_key) {
        return done(st, MUSE_CD_BOOT, MUSE_CD_WHY_KEY, why);
    }
    bool healthy = r->battery_mv >= healthy_mv;
    if (!r->usb) {
        st->healthy_n = 0;
        if (healthy) {
            /* Charged far enough by the time USB went: it may run on battery. */
            return done(st, MUSE_CD_BOOT, MUSE_CD_WHY_HEALTHY, why);
        }
        if (++st->unplugged_n >= MUSE_CD_CONFIRM) {
            return done(st, MUSE_CD_OFF, MUSE_CD_WHY_UNPLUGGED, why);
        }
        return done(st, MUSE_CD_WAIT, MUSE_CD_WHY_LOW, why);
    }
    st->unplugged_n = 0;
    if (r->battery_mv <= 0) {
        return done(st, MUSE_CD_BOOT, MUSE_CD_WHY_NO_BATTERY, why);
    }
    st->healthy_n = healthy ? st->healthy_n + 1 : 0;
    if (st->healthy_n >= MUSE_CD_CONFIRM) {
        return done(st, MUSE_CD_BOOT, MUSE_CD_WHY_HEALTHY, why);
    }
    return done(st, MUSE_CD_WAIT, MUSE_CD_WHY_LOW, why);
}

int muse_cd_healthy_mv(int battery_shutdown_mv)
{
    return battery_shutdown_mv > 0 ? battery_shutdown_mv + MUSE_CD_CHARGE_MARGIN_MV : 0;
}

const char *muse_cd_why_name(muse_cd_why_t why)
{
    switch (why) {
    case MUSE_CD_WHY_NOT_USB: return "not a USB power-up";
    case MUSE_CD_WHY_UNREADABLE: return "power unreadable";
    case MUSE_CD_WHY_NO_BATTERY: return "no battery reading";
    case MUSE_CD_WHY_HEALTHY: return "cell healthy";
    case MUSE_CD_WHY_LOW: return "low cell on USB";
    case MUSE_CD_WHY_KEY: return "key";
    case MUSE_CD_WHY_UNPLUGGED: return "unplugged";
    case MUSE_CD_WHY_LIMIT: return "time limit";
    }
    return "?";
}
