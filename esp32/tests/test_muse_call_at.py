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

"""The bench's timed call (console call.at=DELAY_S,LEN_S, muse_call_at.h): armed
before a phone connects, it starts DELAY_S after the next Live call client becomes ready, ends LEN_S later,
fires once; a client that goes before the start puts it back to waiting; call.at=0 disarms."""

from __future__ import annotations

import os
import shlex
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MUSE = ROOT / "components/muse"

HARNESS = r'''
#include <stdio.h>
#include "muse_call_at.h"
#define S 1000000LL
static int bad;
#define CHECK(c) do { if (!(c)) { printf("FAIL %d: %s\n", __LINE__, #c); bad++; } } while (0)
int main(void)
{
    muse_call_at_t a;
    /* Armed with a client already there: only a later one counts. */
    muse_call_at_arm(&a, 5, 45, true);
    CHECK(muse_call_at_tick(&a, true, 1 * S) == MUSE_CALL_AT_NONE);
    CHECK(muse_call_at_tick(&a, false, 2 * S) == MUSE_CALL_AT_NONE);   /* it went */
    CHECK(muse_call_at_tick(&a, true, 10 * S) == MUSE_CALL_AT_NONE);   /* the next one: starts at 15 s */
    CHECK(muse_call_at_tick(&a, true, 14 * S) == MUSE_CALL_AT_NONE);
    CHECK(muse_call_at_tick(&a, true, 15 * S) == MUSE_CALL_AT_START);
    CHECK(muse_call_at_tick(&a, true, 30 * S) == MUSE_CALL_AT_NONE);
    CHECK(muse_call_at_tick(&a, false, 40 * S) == MUSE_CALL_AT_NONE);  /* a call in progress ends on time */
    CHECK(muse_call_at_tick(&a, true, 60 * S) == MUSE_CALL_AT_END);
    CHECK(!a.armed);
    CHECK(muse_call_at_tick(&a, false, 61 * S) == MUSE_CALL_AT_NONE);  /* once */
    CHECK(muse_call_at_tick(&a, true, 62 * S) == MUSE_CALL_AT_NONE);
    CHECK(muse_call_at_tick(&a, true, 200 * S) == MUSE_CALL_AT_NONE);
    /* A client that goes before the start: back to waiting for the next. */
    muse_call_at_arm(&a, 60, 30, false);
    CHECK(muse_call_at_tick(&a, true, 1 * S) == MUSE_CALL_AT_NONE);    /* starts at 61 s */
    CHECK(muse_call_at_tick(&a, false, 30 * S) == MUSE_CALL_AT_NONE);
    CHECK(muse_call_at_tick(&a, false, 61 * S) == MUSE_CALL_AT_NONE);
    CHECK(muse_call_at_tick(&a, true, 70 * S) == MUSE_CALL_AT_NONE);   /* starts at 130 s */
    CHECK(muse_call_at_tick(&a, true, 129 * S) == MUSE_CALL_AT_NONE);
    CHECK(muse_call_at_tick(&a, true, 130 * S) == MUSE_CALL_AT_START);
    CHECK(muse_call_at_tick(&a, true, 160 * S) == MUSE_CALL_AT_END);
    /* Disarmed (len 0): nothing, whatever connects. */
    muse_call_at_arm(&a, 1, 0, false);
    CHECK(!a.armed);
    CHECK(muse_call_at_tick(&a, true, 1 * S) == MUSE_CALL_AT_NONE);
    CHECK(muse_call_at_tick(&a, true, 5 * S) == MUSE_CALL_AT_NONE);
    /* Strict (the boot arm): a client that goes before the start gives it up for good. */
    muse_call_at_arm(&a, 120, 50, false);
    a.strict = true;
    CHECK(muse_call_at_tick(&a, true, 1 * S) == MUSE_CALL_AT_NONE);     /* starts at 121 s */
    CHECK(muse_call_at_tick(&a, false, 60 * S) == MUSE_CALL_AT_GIVE_UP);
    CHECK(!a.armed);
    CHECK(muse_call_at_tick(&a, true, 70 * S) == MUSE_CALL_AT_NONE);    /* never later */
    CHECK(muse_call_at_tick(&a, true, 300 * S) == MUSE_CALL_AT_NONE);
    /* Strict and the client stays: starts and ends as any. */
    muse_call_at_arm(&a, 120, 50, false);
    a.strict = true;
    CHECK(muse_call_at_tick(&a, true, 1 * S) == MUSE_CALL_AT_NONE);
    CHECK(muse_call_at_tick(&a, true, 121 * S) == MUSE_CALL_AT_START);
    CHECK(muse_call_at_tick(&a, false, 150 * S) == MUSE_CALL_AT_NONE);  /* in the call: ends on time */
    CHECK(muse_call_at_tick(&a, true, 171 * S) == MUSE_CALL_AT_END);
    /* Delay 0: at once after the client is ready. */
    muse_call_at_arm(&a, 0, 10, false);
    CHECK(muse_call_at_tick(&a, true, 3 * S) == MUSE_CALL_AT_NONE);
    CHECK(muse_call_at_tick(&a, true, 3 * S + 1) == MUSE_CALL_AT_START);
    printf("%s\n", bad ? "bad" : "ok");
    return bad != 0;
}
'''


class MuseCallAtTest(unittest.TestCase):
    def test_timed_call(self) -> None:
        cc = shlex.split(os.environ.get("CC", "cc"))
        with tempfile.TemporaryDirectory() as t:
            src, exe = Path(t, "h.c"), Path(t, "h")
            src.write_text(HARNESS)
            r = subprocess.run([*cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
                                "-I", str(MUSE), str(src), str(MUSE / "muse_call_at.c"), "-o", str(exe)],
                               capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            r = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout)
            self.assertEqual(r.stdout.strip(), "ok")

    def test_no_committed_config_arms_a_call(self) -> None:
        # The boot arm is for one local bench image: no committed sdkconfig, overlay or defaults file sets it.
        files = subprocess.run(["git", "ls-files"], cwd=ROOT, capture_output=True, text=True, check=True).stdout
        hits = []
        for f in files.split():
            if "sdkconfig" not in Path(f).name:
                continue
            for line in (ROOT / f).read_text(errors="replace").splitlines():
                if line.strip().startswith("CONFIG_MUSE_CALL_AT_BOOT=") and line.strip() != 'CONFIG_MUSE_CALL_AT_BOOT=""':
                    hits.append(f"{f}: {line.strip()}")
        self.assertEqual(hits, [])


if __name__ == "__main__":
    unittest.main()
