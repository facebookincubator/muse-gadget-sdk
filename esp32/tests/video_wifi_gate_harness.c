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

/*
 * The Wi-Fi video port's gate (components/muse/muse_video_wifi_gate.c) on
 * the host, for tests/test_video_untrusted.py (WifiPortGate): refused starts
 * and the pause they make, and the mDNS announcement through a fake mdns.
 * Prints one line a check; exits 1 if any failed.
 */
#include <stdio.h>
#include <string.h>

#include "muse_video_wifi_gate.h"

static int fails;

static void check(int ok, const char *what)
{
    printf("%s %s\n", ok ? "ok" : "FAIL", what);
    fails += !ok;
}

#define MS(x) ((int64_t)(x) * 1000)

static void backoff(void)
{
    muse_vwg_backoff_t b;
    memset(&b, 0, sizeof(b));
    int64_t t = MS(100000);
    /* The first refusals are free. */
    for (int i = 1; i < MUSE_VWG_FREE_REFUSALS; i++) {
        check(muse_vwg_start_allowed(&b, t), "starts heard before the free refusals are used");
        check(muse_vwg_refused(&b, t) == 0, "a free refusal starts no pause");
    }
    check(muse_vwg_refused(&b, t) == 1000, "the next refusal pauses starts for 1 s");
    check(!muse_vwg_start_allowed(&b, t + MS(999)), "no start heard during the pause");
    check(muse_vwg_start_allowed(&b, t + MS(1000)), "starts heard once the pause is over");
    /* Each refusal after a pause doubles the next, up to 30 s. */
    int want[] = { 2000, 4000, 8000, 16000, 30000, 30000, 30000 };
    for (size_t i = 0; i < sizeof(want) / sizeof(want[0]); i++) {
        t += MS(60000);
        int p = muse_vwg_refused(&b, t);
        char what[64];
        snprintf(what, sizeof(what), "pause %zu is %d ms", i + 2, want[i]);
        check(p == want[i], what);
        check(!muse_vwg_start_allowed(&b, t + MS(want[i]) - 1) && muse_vwg_start_allowed(&b, t + MS(want[i])),
              "the pause lasts exactly that long");
    }
    /* A start taken (or the view opening) ends it all. */
    muse_vwg_reset(&b);
    check(muse_vwg_start_allowed(&b, t), "heard after a reset");
    for (int i = 1; i < MUSE_VWG_FREE_REFUSALS; i++) {
        check(muse_vwg_refused(&b, t) == 0, "free refusals again after a reset");
    }
    check(muse_vwg_refused(&b, t) == 1000, "and the first pause is 1 s again");
    /* Refusals during a pause keep it at the cap, never past it. */
    muse_vwg_reset(&b);
    for (int i = 0; i < 100; i++) {
        muse_vwg_refused(&b, t);
    }
    check(b.pause_ms == MUSE_VWG_MAX_PAUSE_MS && b.until_us == t + MS(MUSE_VWG_MAX_PAUSE_MS), "a hundred refusals: 30 s");
}

typedef struct {
    int adds, removes, fail_add;
} fake_mdns_t;

static int fake_add(void *ctx)
{
    fake_mdns_t *m = ctx;
    m->adds++;
    return m->fail_add ? -1 : 0;
}

static int fake_remove(void *ctx)
{
    ((fake_mdns_t *)ctx)->removes++;
    return 0;
}

static void announce(void)
{
    fake_mdns_t m = { 0, 0, 0 };
    muse_vwg_announce_ops_t ops = { fake_add, fake_remove, &m };
    muse_vwg_announce_t a;
    memset(&a, 0, sizeof(a));
    check(!muse_vwg_announce(&a, false, false, &ops) && m.adds == 0, "not announced at boot");
    check(!muse_vwg_announce(&a, true, false, &ops) && m.adds == 0, "mDNS up, view closed: not announced");
    check(muse_vwg_announce(&a, true, true, &ops) && m.adds == 1, "the view opens: announced");
    check(muse_vwg_announce(&a, true, true, &ops) && m.adds == 1, "asked again: no second add");
    check(!muse_vwg_announce(&a, true, false, &ops) && m.removes == 1, "the view closes: removed");
    check(!muse_vwg_announce(&a, true, false, &ops) && m.removes == 1, "closed again: no second remove");
    /* The view opened before the network was up: announced once mDNS is. */
    muse_vwg_announce_t b;
    memset(&b, 0, sizeof(b));
    fake_mdns_t n = { 0, 0, 0 };
    ops.ctx = &n;
    check(!muse_vwg_announce(&b, false, true, &ops) && n.adds == 0, "view open, no network: waits");
    check(muse_vwg_announce(&b, true, true, &ops) && n.adds == 1, "the network comes up: announced");
    check(!muse_vwg_announce(&b, false, true, &ops) && n.removes == 1, "the network goes: removed");
    /* An add that fails is tried again at the next change, not counted as up. */
    muse_vwg_announce_t c;
    memset(&c, 0, sizeof(c));
    fake_mdns_t f = { 0, 0, 1 };
    ops.ctx = &f;
    check(!muse_vwg_announce(&c, true, true, &ops) && f.adds == 1, "a failed add: not announced");
    check(!muse_vwg_announce(&c, true, false, &ops) && f.removes == 0, "nothing to remove after a failed add");
    f.fail_add = 0;
    check(muse_vwg_announce(&c, true, true, &ops) && f.adds == 2, "the next open announces");
    /* The bench build (starts any time) passes wanted always: announced as soon as mDNS is up. */
    muse_vwg_announce_t d;
    memset(&d, 0, sizeof(d));
    fake_mdns_t g = { 0, 0, 0 };
    ops.ctx = &g;
    check(muse_vwg_announce(&d, true, true, &ops) && g.adds == 1 && g.removes == 0, "starts any time: always announced");
}

int main(void)
{
    backoff();
    announce();
    printf("%s\n", fails ? "FAILED" : "all ok");
    return fails ? 1 : 0;
}
