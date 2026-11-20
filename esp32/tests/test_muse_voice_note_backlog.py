# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""The voice note's mic backlog (muse_chat_session.cpp), which a held note
fills in normal use.

ESP-IDF's xStreamBufferCreateWithCaps() builds a static stream buffer, which
holds one byte less than its storage, and a send that doesn't fit writes what
does. The test cuts the buffer's size and its writer out of the source, runs
them against a fake with those semantics, and checks every sample comes out
whole and in order after the backlog has overflowed.
"""

from __future__ import annotations

import os
import re
import shlex
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

FAKE = r"""
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef int TickType_t;
#define pdMS_TO_TICKS(ms) (ms)
typedef struct {
    uint8_t *buf;
    size_t len, head, tail;   /* len is the storage: it holds len - 1 */
} sb_t;
typedef sb_t *StreamBufferHandle_t;
static void reader_step(void);   /* the reader running while the writer waits */
static size_t used(sb_t *b) { return (b->head + b->len - b->tail) % b->len; }
static size_t space(sb_t *b) { return b->len - 1 - used(b); }
static StreamBufferHandle_t create(size_t size)
{
    sb_t *b = calloc(1, sizeof(*b));
    b->buf = malloc(size);
    b->len = size;
    return b;
}
static size_t xStreamBufferSend(sb_t *b, const void *p, size_t n, TickType_t wait)
{
    (void)wait;
    if (space(b) < n) {
        reader_step();
    }
    size_t room = space(b);
    n = n < room ? n : room;   /* FreeRTOS writes what fits */
    for (size_t i = 0; i < n; i++, b->head = (b->head + 1) % b->len) b->buf[b->head] = ((const uint8_t *)p)[i];
    return n;
}
static size_t xStreamBufferReceive(sb_t *b, void *p, size_t n, TickType_t wait)
{
    (void)wait;
    size_t have = used(b);
    n = n < have ? n : have;
    for (size_t i = 0; i < n; i++, b->tail = (b->tail + 1) % b->len) ((uint8_t *)p)[i] = b->buf[b->tail];
    return n;
}
static int16_t sample(size_t i) { return (int16_t)(i * 7919u + 13); }
"""


class PcmStreamBufferTest(unittest.TestCase):
    def setUp(self) -> None:
        cc = shlex.split(os.environ.get("CC", "cc"))
        if not cc or shutil.which(cc[0]) is None:
            self.skipTest("C compiler not available")
        self.cc = cc

    def run_harness(self, code: str) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            c = Path(tmp) / "pcm.c"
            c.write_text(code)
            exe = Path(tmp) / "pcm"
            built = subprocess.run([*self.cc, "-std=gnu11", "-Wall", "-Wextra", "-Werror", str(c), "-o", str(exe)],
                                   capture_output=True, text=True)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            ran = subprocess.run([str(exe)], capture_output=True, text=True)
        self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)

    @staticmethod
    def function(source: str, start: str) -> str:
        at = source.index(start)
        return source[at:source.index("\n}\n", at) + 3]

    def test_voice_note_backlog_keeps_every_sample(self) -> None:
        # A held note fed in with feed_live()'s "what there's room for", faster
        # than the session takes it: the backlog fills, and a send that stopped
        # halfway through a sample came back one short, so the next sent that
        # half again and every byte after it was one off.
        src = (ROOT / "components/muse/muse_chat_session.cpp").read_text()
        size = re.search(r"s_in = xStreamBufferCreateWithCaps\(([^,]+),", src).group(1)
        writer = self.function(src, "extern \"C\" size_t muse_hatch_turn_audio_wait(").replace("extern \"C\" ", "")
        self.run_harness(FAKE + r"""
#define IN_BYTES (16000 * 2 * 8)
static StreamBufferHandle_t s_in;
""" + writer + r"""
#define NOTE (16000 * 20)   /* 20 s, NOTE_MAX_BYTES */
static uint8_t got[NOTE * 2 + 16];
static size_t got_len;
static void reader_step(void)
{
    /* record_note() takes up to a part at a time, an even number of bytes. */
    got_len += xStreamBufferReceive(s_in, got + got_len, 6144, 0);
}
int main(void)
{
    s_in = create(""" + size + r""");
    static int16_t note[NOTE];
    for (size_t i = 0; i < NOTE; i++) note[i] = sample(i);
    for (size_t sent = 0; sent < NOTE;) sent += muse_hatch_turn_audio_wait(note + sent, NOTE - sent, 0);
    got_len += xStreamBufferReceive(s_in, got + got_len, sizeof(got) - got_len, 0);
    if (got_len != sizeof(note)) return printf("%zu bytes for %zu\n", got_len, sizeof(note)), 1;
    for (size_t i = 0; i < NOTE; i++) {
        int16_t v;
        memcpy(&v, got + 2 * i, 2);
        if (v != note[i]) return printf("sample %zu of %d is wrong\n", i, NOTE), 1;
    }
    return 0;
}
""")


if __name__ == "__main__":
    unittest.main()
