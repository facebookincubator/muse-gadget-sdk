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

#include "muse_mirror_core.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

static unsigned read_le(int n)
{
    unsigned v = 0;
    for (int i = 0; i < n; i++) {
        int b = getchar();
        assert(b != EOF);
        v |= (unsigned)b << (8 * i);
    }
    return v;
}

static void emit(muse_mirror_core_t *c, uint16_t *fb, uint8_t *msg, size_t cap, int state)
{
    bool pending = muse_mirror_core_pending(c);
    size_t n = muse_mirror_core_build(c, fb, msg, cap, state);
    assert(n >= 8 && n <= cap);
    assert(!pending || msg[2] || msg[3]);
    for (int i = 0; i < 4; i++) putchar(n >> (8 * i) & 255);
    assert(fwrite(msg, 1, n, stdout) == n);
}

int main(void)
{
    int w = read_le(2), h = read_le(2);
    size_t cap = read_le(4);
    muse_mirror_core_t c;
    assert(!muse_mirror_core_init(&c, 0, h));
    assert(!muse_mirror_core_init(&c, 65536, h));
    assert(muse_mirror_core_init(&c, w, h));
    uint16_t *fb = calloc((size_t)w * h, sizeof(*fb));
    uint8_t *msg = malloc(cap);
    assert(fb && msg);
    for (int cmd; (cmd = getchar()) != EOF;) {
        if (cmd == 'U') {
            int x = read_le(2), y = read_le(2), rw = read_le(2), rh = read_le(2);
            assert(x + rw <= w && y + rh <= h && rw && rh);
            for (int row = y; row < y + rh; row++) {
                for (int col = x; col < x + rw; col++) fb[(size_t)row * w + col] = read_le(2);
            }
            muse_mirror_core_dirty(&c, x, y, x + rw - 1, y + rh - 1);
        } else if (cmd == 'B') {
            emit(&c, fb, msg, cap, read_le(1));
        } else if (cmd == 'D') {
            int state = read_le(1), frames = 0;
            while (muse_mirror_core_pending(&c)) {
                assert(++frames < 100000);
                emit(&c, fb, msg, cap, state);
            }
        } else if (cmd == 'S') {
            muse_mirror_core_t saved = c;
            assert(muse_mirror_core_build(&c, fb, msg, 8, 0) == 0);
            assert(c.ndirty == saved.ndirty && c.tail_on == saved.tail_on);
        } else if (cmd == 'A') {
            muse_mirror_core_dirty(&c, -10, -10, w + 10, h + 10);
        } else {
            assert(0);
        }
    }
    assert(!muse_mirror_core_pending(&c));
    free(msg);
    free(fb);
    return 0;
}
