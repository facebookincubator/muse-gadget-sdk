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

/* The boot guard's decision (muse_bootguard.h), with no IDF in it. */

#include "muse_bootguard.h"

#include <string.h>

#include "muse_restart.h"

static uint32_t rec_check(const muse_bootguard_rec_t *r)
{
    return ~(r->magic ^ r->pending ^ r->failed ^ r->crashes ^ r->lost ^ r->button);
}

static void rec_seal(muse_bootguard_rec_t *r)
{
    r->magic = MUSE_BOOTGUARD_MAGIC;
    r->check = rec_check(r);
}

bool muse_bootguard_rec_valid(const muse_bootguard_rec_t *r)
{
    return r->magic == MUSE_BOOTGUARD_MAGIC && r->check == rec_check(r) && r->failed < 1000 && r->crashes < 1000 &&
           r->lost < 1000 && r->button <= 1;
}

static bool is_crash(int reason)
{
    return reason == MUSE_RST_PANIC || reason == MUSE_RST_INT_WDT || reason == MUSE_RST_TASK_WDT ||
           reason == MUSE_RST_WDT;
}

muse_bootguard_out_t muse_bootguard_decide(muse_bootguard_rec_t *r, const muse_bootguard_in_t *in)
{
    muse_bootguard_out_t out = { .mode = MUSE_BOOT_NORMAL, .why = MUSE_BOOT_WHY_NONE };
    if (!muse_bootguard_rec_valid(r) || in->reason == MUSE_RST_POWERON) {
        /* A power-on (or what a power loss left in RTC memory): a fresh start. */
        memset(r, 0, sizeof(*r));
    } else if (r->pending && !in->intentional && in->reason != MUSE_RST_DEEPSLEEP) {
        r->failed++;   /* the last boot never got healthy (a deep-sleep wake: it was turned off) */
    }
    if (is_crash(in->reason)) {
        r->crashes++;
    }
    r->pending = 1;
    bool button = r->button != 0;
    r->button = 0;   /* this boot only: the next one is the guard's to decide */
    if (button) {
        out.mode = MUSE_BOOT_SAFE;   /* the user's key hold, a planned restart ago */
        out.why = MUSE_BOOT_WHY_BUTTON;
    } else if (r->failed >= MUSE_BOOTGUARD_FAILED_BOOTS) {
        /* Escape: start over counting, so a fixed image (or SAFE mode) gets
         * its own three tries. */
        r->failed = 0;
        r->pending = 0;
        out.why = MUSE_BOOT_WHY_FAILED_BOOTS;
        out.mode = in->usb_host && in->can_download ? MUSE_BOOT_DOWNLOAD : MUSE_BOOT_SAFE;
    } else if (r->crashes >= MUSE_BOOTGUARD_CRASHES) {
        r->crashes = 0;
        out.mode = MUSE_BOOT_SAFE;
        out.why = MUSE_BOOT_WHY_CRASHES;
        out.keep_off = true;
    } else if (r->lost >= MUSE_BOOTGUARD_LOST) {
        /* Kept until a boot stays up 10 minutes (rec_stable), not reset here:
         * each lost boot looked healthy, so a fresh count would loop forever. */
        out.mode = MUSE_BOOT_SAFE;
        out.why = MUSE_BOOT_WHY_LOST;
    } else if (in->kept_off) {
        out.mode = MUSE_BOOT_SAFE;
        out.why = MUSE_BOOT_WHY_KEPT_OFF;
    }
    rec_seal(r);
    return out;
}

void muse_bootguard_rec_healthy(muse_bootguard_rec_t *r)
{
    if (!muse_bootguard_rec_valid(r)) {
        memset(r, 0, sizeof(*r));
    }
    r->pending = 0;
    r->failed = 0;
    rec_seal(r);
}

void muse_bootguard_rec_stable(muse_bootguard_rec_t *r)
{
    muse_bootguard_rec_healthy(r);
    r->crashes = 0;
    r->lost = 0;
    rec_seal(r);
}

void muse_bootguard_rec_button(muse_bootguard_rec_t *r)
{
    if (!muse_bootguard_rec_valid(r)) {
        memset(r, 0, sizeof(*r));
    }
    r->button = 1;
    rec_seal(r);
}

void muse_bootguard_rec_lost(muse_bootguard_rec_t *r)
{
    if (!muse_bootguard_rec_valid(r)) {
        memset(r, 0, sizeof(*r));
    }
    r->lost++;
    rec_seal(r);
}

bool muse_bootguard_hold_step(muse_bootguard_hold_t *h, bool down, uint32_t now_ms)
{
    if (!down) {
        h->seen_up = true;
        h->down = false;
        h->fired = false;
        return false;
    }
    if (!h->seen_up) {
        return false;   /* held since before the watch: not a hold the user made in SAFE */
    }
    if (!h->down) {
        h->down = true;
        h->down_ms = now_ms;
    }
    if (h->fired || (uint32_t)(now_ms - h->down_ms) < MUSE_BOOTGUARD_SAFE_HOLD_MS) {
        return false;
    }
    h->fired = true;
    return true;
}

bool muse_bootguard_entry_step(muse_bootguard_hold_t *h, bool down, uint32_t since_boot_ms)
{
    if (!down) {
        if (h->down && !h->fired) {
            h->released_early = true;   /* begun in the window, let go before the SAFE hold: setup mode */
        }
        h->seen_up = true;
        h->down = false;
        return false;
    }
    if (!h->seen_up || h->fired) {
        return false;   /* held (or stuck) since before boot, or already taken */
    }
    if (!h->down) {
        if (since_boot_ms > MUSE_BOOTGUARD_ENTRY_WINDOW_MS) {
            h->fired = true;   /* a press begun after the window: this boot's watch is over */
            return false;
        }
        h->down = true;
        h->down_ms = since_boot_ms;
    }
    if ((uint32_t)(since_boot_ms - h->down_ms) < MUSE_BOOTGUARD_ENTRY_HOLD_MS) {
        return false;
    }
    h->fired = true;
    return true;
}

int muse_bootguard_keys_step(muse_bootguard_keys_t *k, bool down, uint32_t since_boot_ms)
{
    if (down && !k->was_down) {
        k->window_press = since_boot_ms <= MUSE_BOOTGUARD_ENTRY_WINDOW_MS;
    }
    k->was_down = down;
    bool enter = muse_bootguard_entry_step(&k->entry, down, since_boot_ms);
    bool leave = muse_bootguard_hold_step(&k->leave, down, since_boot_ms);
    if (enter) {
        return MUSE_BOOTGUARD_KEY_ENTER;
    }
    return leave && !k->window_press ? MUSE_BOOTGUARD_KEY_LEAVE : MUSE_BOOTGUARD_KEY_NONE;
}

const char *muse_bootguard_why_name(muse_boot_why_t why)
{
    switch (why) {
    case MUSE_BOOT_WHY_FAILED_BOOTS: return "failed boots in a row";
    case MUSE_BOOT_WHY_CRASHES: return "crash resets: Muse off until re-enabled";
    case MUSE_BOOT_WHY_KEPT_OFF:
        return "Muse off since earlier crashes (safe.off, >safe=0 or a 5 s key hold restarts normally)";
    case MUSE_BOOT_WHY_LOST: return "running boots kept going unreachable";
    case MUSE_BOOT_WHY_BUTTON: return "the user held the SAFE key after boot";
    default: return "none";
    }
}
