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
#include "muse_video_wifi_gate.h"

#include <stddef.h>

bool muse_vwg_start_allowed(const muse_vwg_backoff_t *b, int64_t now_us)
{
    return !b->until_us || now_us >= b->until_us;
}

int muse_vwg_refused(muse_vwg_backoff_t *b, int64_t now_us)
{
    if (b->refused < MUSE_VWG_FREE_REFUSALS) {
        b->refused++;
    }
    if (b->refused < MUSE_VWG_FREE_REFUSALS) {
        return 0;
    }
    /* Each refusal after a pause doubles the next one, up to the cap. */
    int p = b->pause_ms ? b->pause_ms * 2 : MUSE_VWG_FIRST_PAUSE_MS;
    b->pause_ms = p < MUSE_VWG_MAX_PAUSE_MS ? p : MUSE_VWG_MAX_PAUSE_MS;
    b->until_us = now_us + (int64_t)b->pause_ms * 1000;
    return b->pause_ms;
}

void muse_vwg_reset(muse_vwg_backoff_t *b)
{
    b->refused = 0;
    b->pause_ms = 0;
    b->until_us = 0;
}

bool muse_vwg_announce(muse_vwg_announce_t *a, bool ready, bool wanted, const muse_vwg_announce_ops_t *ops)
{
    a->ready = ready;
    a->wanted = wanted;
    bool want = ready && wanted;
    if (want && !a->on && ops && ops->add) {
        a->on = ops->add(ops->ctx) == 0;
    } else if (!want && a->on) {
        if (ops && ops->remove) {
            ops->remove(ops->ctx);
        }
        a->on = false;   /* (a failed remove leaves nothing to retry: mDNS drops it with the interface) */
    }
    return a->on;
}
