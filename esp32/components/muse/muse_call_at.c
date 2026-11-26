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

/* The bench's timed call (muse_call_at.h). */

#include "muse_call_at.h"

#include <string.h>

void muse_call_at_arm(muse_call_at_t *a, uint32_t delay_s, uint32_t len_s, bool available)
{
    memset(a, 0, sizeof(*a));
    a->armed = len_s > 0;
    a->delay_s = delay_s;
    a->len_s = len_s;
    a->was_available = available;
}

muse_call_at_action_t muse_call_at_tick(muse_call_at_t *a, bool available, int64_t now_us)
{
    if (!a->armed) {
        a->was_available = available;
        return MUSE_CALL_AT_NONE;
    }
    muse_call_at_action_t act = MUSE_CALL_AT_NONE;
    if (a->end_us) {
        if (now_us >= a->end_us) {
            a->armed = false;   /* once */
            act = MUSE_CALL_AT_END;
        }
    } else if (a->fire_us) {
        if (!available) {
            a->fire_us = 0;   /* the client went: wait for the next one */
            if (a->strict) {
                a->armed = false;
                act = MUSE_CALL_AT_GIVE_UP;
            }
        } else if (now_us >= a->fire_us) {
            a->fire_us = 0;
            a->end_us = now_us + (int64_t)a->len_s * 1000000;
            act = MUSE_CALL_AT_START;
        }
    } else if (available && !a->was_available) {
        a->fire_us = now_us + (int64_t)a->delay_s * 1000000;
        if (a->fire_us == 0) {
            a->fire_us = 1;
        }
    }
    a->was_available = available;
    return act;
}
