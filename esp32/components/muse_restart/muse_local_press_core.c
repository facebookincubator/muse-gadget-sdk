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

#include "muse_local_press.h"

#include <string.h>

/* mg-update-v2.json's reference (protocols/test-vectors/mg_update_ref.py, Device.smp and Device.event), step for
 * step, but for the slot's hash (the caller's: slot_ok) and the wired arm's next link (an addition). */

void muse_local_press_core_init(muse_local_press_t *w)
{
    memset(w, 0, sizeof(*w));
    w->prompt_ms = -1;
    w->shown_ms = -1;
    w->bound_ms = -1;
    w->covered_ms = -1;
    w->armed_ms = -1;
}

bool muse_local_press_core_prompting(const muse_local_press_t *w, int64_t now_ms)
{
    return w->prompt_ms >= 0 && now_ms >= w->prompt_ms && now_ms - w->prompt_ms < MUSE_UPDATE_PROMPT_MS;
}

static bool armed(const muse_local_press_t *w, int64_t now_ms)
{
    return w->arm_link && w->armed_ms >= 0 && now_ms >= w->armed_ms && now_ms - w->armed_ms < MUSE_LOCAL_PRESS_MS;
}

static bool within(int64_t since_ms, int64_t now_ms)
{
    return since_ms >= 0 && now_ms >= since_ms && now_ms - since_ms < MUSE_UPDATE_BOUND_MS;
}

static bool same(const muse_update_ident_t *a, const muse_update_ident_t *b)
{
    return a->has_sha && b->has_sha && a->len == b->len && !memcmp(a->sha, b->sha, sizeof(a->sha));
}

bool muse_local_press_core_bound(const muse_local_press_t *w, muse_update_ident_t *id)
{
    if (w->bound_ms < 0) {
        return false;
    }
    *id = w->bound;
    return true;
}

static void end_binding(muse_local_press_t *w)
{
    w->bound_ms = -1;
    w->here = false;
    memset(&w->bound, 0, sizeof(w->bound));
    w->ends++;
}

/* The caller's hash of the slot, if it was taken against this binding. */
static bool slot_ok(const muse_local_press_t *w, const muse_update_req_t *req)
{
    return req->slot_ok && same(&req->slot_for, &w->bound);
}

muse_update_answer_t muse_local_press_core_ask(muse_local_press_t *w, int64_t now_ms, bool locked,
                                               const muse_update_req_t *req)
{
    if (armed(w, now_ms)) {
        return MUSE_UPDATE_OPEN;   /* the wired console's arm: no press */
    }
    bool upload = req->kind == MUSE_UPDATE_UPLOAD0 || req->kind == MUSE_UPDATE_CHUNK;
    if (req->kind == MUSE_UPDATE_UPLOAD0 && !req->id.has_sha) {
        return MUSE_UPDATE_REFUSED;   /* no SHA-256 to bind a press to: refused, never asked */
    }
    if (req->kind == MUSE_UPDATE_CHUNK && !w->here) {
        return MUSE_UPDATE_REFUSED;   /* no upload started on this link: nothing to bind a press to */
    }
    bool bound_live = w->bound_ms >= 0 && within(w->bound_ms, now_ms);
    if (!w->here && req->kind == MUSE_UPDATE_UPLOAD0 && bound_live && same(&req->id, &w->bound)) {
        w->here = true;   /* the bound upload, started again on a new link: no new prompt */
    }
    if (w->here) {
        /* Starting the upload again: within the window only. Its later chunks, its state write (the slot checked), an
         * erase or the reset: always (a transfer under way when the window ends finishes on its link). */
        bool fits = req->kind != MUSE_UPDATE_UPLOAD0 || (bound_live && same(&req->id, &w->bound));
        if (fits) {
            if (upload) {
                uint64_t end = (uint64_t)req->off + req->n;
                if (end > w->bound.len) {
                    end_binding(w);
                    return MUSE_UPDATE_REFUSED;   /* past the bound length */
                }
                if (end == w->bound.len && !slot_ok(w, req)) {
                    end_binding(w);
                    return MUSE_UPDATE_REFUSED;   /* the last chunk: not the bytes the press was for */
                }
            } else if (req->kind == MUSE_UPDATE_STATE && !slot_ok(w, req)) {
                end_binding(w);
                return MUSE_UPDATE_REFUSED;   /* the slot isn't the bound image: nothing is staged */
            }
            return MUSE_UPDATE_OPEN;
        }
        w->here = false;   /* another upload: asks again */
    }
    if (req->kind != MUSE_UPDATE_UPLOAD0 && within(w->covered_ms, now_ms)
        && !memcmp(req->key, w->covered_key, sizeof(req->key))) {
        return MUSE_UPDATE_OPEN;   /* the erase, reset or state write a press covered, on this link */
    }
    if (w->cancelled) {
        w->cancelled = false;   /* this one is refused, the next asks again */
        return MUSE_UPDATE_REFUSED;
    }
    (void)locked;   /* a lock changes only the press (note): the prompt's clock runs from the wake to the keypad */
    if (w->prompt_ms >= 0) {
        if (muse_local_press_core_prompting(w, now_ms)) {
            return MUSE_UPDATE_ASKING;
        }
        w->prompt_ms = -1;   /* timed out: this one is refused, the next asks again */
        return MUSE_UPDATE_REFUSED;
    }
    if (w->shown_ms >= 0 && now_ms >= w->shown_ms && now_ms - w->shown_ms < MUSE_UPDATE_REARM_MS) {
        return MUSE_UPDATE_ASKING;   /* the last prompt on this link showed under 10 s ago: wait, show nothing */
    }
    w->prompt_ms = now_ms;   /* ask for the press */
    w->shown_ms = now_ms;
    w->ask_upload = req->kind == MUSE_UPDATE_UPLOAD0;
    if (w->ask_upload) {
        w->ask = req->id;
    } else {
        memcpy(w->ask_key, req->key, sizeof(w->ask_key));
    }
    return MUSE_UPDATE_ASKING;
}

bool muse_local_press_core_note(muse_local_press_t *w, int64_t now_ms, bool locked)
{
    if (locked || !muse_local_press_core_prompting(w, now_ms)) {
        return false;   /* no prompt (or the keypad's): a press at any other time never counts */
    }
    w->prompt_ms = -1;
    if (w->ask_upload) {
        w->bound = w->ask;   /* bound to the upload that asked, on any link for MUSE_UPDATE_BOUND_MS */
        w->bound_ms = now_ms;
        w->here = true;
    } else {
        memcpy(w->covered_key, w->ask_key, sizeof(w->covered_key));   /* only that change, on this link */
        w->covered_ms = now_ms;
    }
    return true;
}

void muse_local_press_core_cancel(muse_local_press_t *w, int64_t now_ms)
{
    if (muse_local_press_core_prompting(w, now_ms)) {
        w->prompt_ms = -1;
        w->cancelled = true;   /* ended: the next change is refused, once; it binds nothing */
    }
}

void muse_local_press_core_arm(muse_local_press_t *w, int64_t now_ms)
{
    w->prompt_ms = -1;
    w->armed_ms = now_ms;
    w->arm_link = true;
    w->arm_next = true;   /* a link that comes next takes it too (the wire is the owner's) */
}

void muse_local_press_core_link_new(muse_local_press_t *w)
{
    w->prompt_ms = -1;
    w->shown_ms = -1;   /* a new link may show the prompt at once */
    w->cancelled = false;
    w->here = false;   /* the press's upload stays bound (bound_ms), across the drop */
    w->covered_ms = -1;   /* what a press covered was that link's */
    w->arm_link = w->arm_next;
    w->arm_next = false;
}

void muse_local_press_core_forget(muse_local_press_t *w, int64_t now_ms)
{
    if (w->prompt_ms >= 0 && !muse_local_press_core_prompting(w, now_ms)) {
        w->cancelled = true;   /* a prompt that timed out still refuses the next change once */
    }
    w->prompt_ms = -1;
    w->bound_ms = -1;
    w->here = false;
    memset(&w->bound, 0, sizeof(w->bound));
    w->covered_ms = -1;   /* the wired arm stays: the wire is the owner's */
}
