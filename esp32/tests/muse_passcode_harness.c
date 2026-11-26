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
 * Drives muse_passcode_core.c for tests/test_muse_passcode.py: one command a
 * line on stdin, one answer a line on stdout. SHA-256 is portable/
 * mg_link_setup's builtin backend (the gadget uses PSA's). The "store" is the
 * record the last command left: `reboot` starts again from it, as NVS would.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mgls_crypto.h"
#include "muse_passcode_core.h"

static bool sha(const uint8_t *data, size_t len, uint8_t out[32])
{
    return mglsc_sha256(data, len, out) == 0;
}

static bool s_sha_fails;
static bool sha_maybe(const uint8_t *data, size_t len, uint8_t out[32])
{
    return !s_sha_fails && sha(data, len, out);
}

static const char *RESULT[] = { "right", "wrong", "wait", "waiting" };

static void state(const muse_pass_t *p, long long now)
{
    printf("on=%d locked=%d tries=%u level=%u lock=%u left=%lld\n", p->rec.on, p->locked, (unsigned)p->rec.tries,
           (unsigned)p->rec.wait_level, (unsigned)p->rec.lock, (long long)muse_pass_wait_left_ms(p, now));
}

int main(void)
{
    muse_pass_t p;
    muse_pass_rec_t store;
    bool stored = false;
    muse_pass_init(&p, NULL, 0);
    char line[256];
    while (fgets(line, sizeof(line), stdin)) {
        char cmd[32] = "", a[64] = "";
        long long now = 0;
        int n1 = 0, n2 = 0, n3 = 0;
        if (sscanf(line, "%31s", cmd) != 1) continue;
        if (!strcmp(cmd, "fresh")) {
            sscanf(line, "%*s %lld", &now);
            muse_pass_init(&p, NULL, now);
            stored = false;
            state(&p, now);
        } else if (!strcmp(cmd, "reboot")) {
            sscanf(line, "%*s %lld", &now);
            muse_pass_init(&p, stored ? &store : NULL, now);
            state(&p, now);
        } else if (!strcmp(cmd, "set")) {
            sscanf(line, "%*s %63s %lld", a, &now);
            uint8_t salt[MUSE_PASS_SALT_LEN];
            for (int i = 0; i < MUSE_PASS_SALT_LEN; i++) salt[i] = (uint8_t)(0x10 + i);
            printf("set=%d ", muse_pass_set(&p, a, salt, sha_maybe));
            state(&p, now);
        } else if (!strcmp(cmd, "shafail")) {
            sscanf(line, "%*s %d", &n1);
            s_sha_fails = n1 != 0;
            printf("ok\n");
        } else if (!strcmp(cmd, "check")) {
            sscanf(line, "%*s %63s %lld", a, &now);
            muse_pass_result_t r = muse_pass_check(&p, a, now, sha_maybe);
            printf("%s ", RESULT[r]);
            state(&p, now);
        } else if (!strcmp(cmd, "off")) {
            sscanf(line, "%*s %lld %d", &now, &n1);
            printf("now=%d ", muse_pass_screen_off(&p, now, n1));
            state(&p, now);
        } else if (!strcmp(cmd, "on")) {
            sscanf(line, "%*s %lld", &now);
            printf("now=%d ", muse_pass_screen_on(&p, now));
            state(&p, now);
        } else if (!strcmp(cmd, "tick")) {
            sscanf(line, "%*s %lld %d %d %d", &now, &n1, &n2, &n3);
            printf("now=%d ", muse_pass_tick(&p, now, (uint32_t)n1, n2, n3));
            state(&p, now);
        } else if (!strcmp(cmd, "locknow")) {
            muse_pass_lock_now(&p);
            state(&p, 0);
        } else if (!strcmp(cmd, "turnoff")) {
            muse_pass_turn_off(&p);
            state(&p, 0);
        } else if (!strcmp(cmd, "forget")) {
            muse_pass_forget(&p);
            state(&p, 0);
        } else if (!strcmp(cmd, "setlock")) {
            sscanf(line, "%*s %d", &n1);
            muse_pass_set_lock(&p, (muse_pass_lock_t)n1);
            state(&p, 0);
        } else if (!strcmp(cmd, "status")) {
            sscanf(line, "%*s %lld", &now);
            char buf[64];
            muse_pass_status(&p, now, buf, sizeof(buf));
            printf("%s\n", buf);
        } else if (!strcmp(cmd, "load")) {
            /* load HEX NOW: a stored record (format 1's 53 bytes or format 2's), then a boot. */
            char hexs[200] = "";
            sscanf(line, "%*s %199s %lld", hexs, &now);
            muse_pass_rec_t r;
            memset(&r, 0, sizeof(r));
            uint8_t *b = (uint8_t *)&r;
            for (size_t i = 0; i < sizeof(r) && hexs[2 * i] && hexs[2 * i + 1]; i++) {
                unsigned v;
                sscanf(hexs + 2 * i, "%2x", &v);
                b[i] = (uint8_t)v;
            }
            muse_pass_init(&p, &r, now);
            printf("dirty=%d ", p.dirty);
            state(&p, now);
        } else if (!strcmp(cmd, "rec")) {
            const uint8_t *b = (const uint8_t *)&p.rec;
            for (size_t i = 0; i < sizeof(p.rec); i++) printf("%02x", b[i]);
            printf("\n");
        } else {
            printf("? %s\n", cmd);
        }
        if (p.dirty) {
            store = p.rec;   /* the caller stores whenever dirty */
            stored = true;
            p.dirty = false;
        }
        fflush(stdout);
    }
    return 0;
}
