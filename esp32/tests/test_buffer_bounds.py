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

"""Two buffer bounds pinned in source.

The S3/S31 ROM tinfl_decompress writes 3 bytes past what it reports for length codes 286/287, so every buffer handed
to it has TINFL_OUT_SPARE (at least 3) bytes beyond the length tinfl is told. config_store's copy_key wipes the
buffer it allocated, not the second NVS read's length (which NVS sets to the value's size at that moment). And the
still patch's inflate (inflate_exact, run on a fake of the ROM's tinfl) tells tinfl the input ends: a stream cut short
is refused, never read past its end, and only DONE with every byte is a patch."""

import os
import re
import shlex
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class BufferBounds(unittest.TestCase):
    def test_every_inflate_output_has_spare(self) -> None:
        src = (ROOT / "components" / "muse" / "muse_ui_fb.c").read_text()
        spare = int(re.search(r"#define TINFL_OUT_SPARE (\d+)", src).group(1))
        self.assertGreaterEqual(spare, 3)
        allocs = re.findall(r"uint8_t \*out = heap_caps_malloc\(([^,]+),", src)
        self.assertEqual(len(allocs), 2, allocs)    # the self-test's and the still patch's output buffers
        for alloc in allocs:
            self.assertIn("TINFL_OUT_SPARE", alloc)
        self.assertEqual(src.count("out && inflate_exact("), 2)    # and those two are the only callers

    def test_a_short_stream_never_inflates(self) -> None:
        src = (ROOT / "components" / "muse" / "muse_ui_fb.c").read_text()
        start = src.index("static bool inflate_exact(")
        fn = src[start:src.index("\n}\n", start) + 3]
        driver = r"""
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define heap_caps_malloc(n, caps) malloc(n)
#define ESP_LOGW(tag, ...) ((void)(tag), fprintf(stderr, __VA_ARGS__))
static const char *TAG = "t";
#define TINFL_SLACK 1024
#define TINFL_FLAG_PARSE_ZLIB_HEADER 1
#define TINFL_FLAG_HAS_MORE_INPUT 2
#define TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF 4
typedef enum { TINFL_STATUS_FAILED = -1, TINFL_STATUS_DONE = 0, TINFL_STATUS_NEEDS_MORE_INPUT = 1,
               TINFL_STATUS_HAS_MORE_OUTPUT = 2 } tinfl_status;
typedef struct { int state; } tinfl_decompressor;
#define tinfl_init(d) ((d)->state = 0)
/* The ROM's tinfl on a stream whose whole length is FULL: with less input and no TINFL_FLAG_HAS_MORE_INPUT it reads
 * zeros past the end and never finishes (here: exit 3); with the flag it asks for more. ANSWER: what a whole stream
 * returns. */
#define FULL 40
static tinfl_status ANSWER = TINFL_STATUS_DONE;
static tinfl_status tinfl_decompress(tinfl_decompressor *d, const uint8_t *in, size_t *in_len, uint8_t *start,
                                     uint8_t *next, size_t *out_len, uint32_t flags) {
    (void)d; (void)in; (void)start;
    if (*in_len < FULL) {
        if (!(flags & TINFL_FLAG_HAS_MORE_INPUT)) {
            printf("read past the input\n");
            exit(3);
        }
        *out_len = *out_len / 2;
        return TINFL_STATUS_NEEDS_MORE_INPUT;
    }
    for (size_t i = 0; i < *out_len; i++) next[i] = (uint8_t)i;
    return ANSWER;
}
%s
int main(void) {
    static uint8_t in[FULL], out[500 + 8];
    int whole = inflate_exact(in, FULL, out, 500);
    int cut = inflate_exact(in, 16, out, 500);
    ANSWER = TINFL_STATUS_HAS_MORE_OUTPUT;   /* every byte, but not DONE */
    int more = inflate_exact(in, FULL, out, 500);
    printf("%%d %%d %%d\n", whole, cut, more);
    return 0;
}
""" % fn
        with tempfile.TemporaryDirectory() as d:
            c = Path(d) / "inflate.c"
            c.write_text(driver)
            exe = Path(d) / "inflate"
            cc = shlex.split(os.environ.get("CC", "cc"))
            r = subprocess.run([*cc, "-std=c11", "-Wall", "-Wextra", "-Werror", str(c), "-o", str(exe)],
                               capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30)
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        self.assertEqual(run.stdout.split(), ["1", "0", "0"])   # whole: a patch; 16 bytes: refused; not DONE: refused

    def test_copy_key_wipes_what_it_allocated(self) -> None:
        src = (ROOT / "main" / "config_store.c").read_text()
        body = src[src.index("value = malloc("):]
        body = body[:body.index("return ok;")]
        self.assertIn("value = malloc(cap)", body)
        self.assertIn("wipe(value, cap)", body)
        self.assertNotIn("wipe(value, len)", body)


if __name__ == "__main__":
    unittest.main()
