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

"""The call's connecting backstop (mgcommands.h "Live call") and the unanswered start, on a fake clock: the deadlines
in components/muse/muse_call_backstop.h, driven the way muse_call.c drives
them (a start asked, the client's states as op_live_call hands them over,
muse_call_poll every 100 ms), and what muse_call.c does when one is due.
"""

import os
import shlex
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))   # its helpers, also when run from esp32/ as tests.<module>
from host_child import run_child  # noqa: E402 (macOS's pipe deadlock on a driver's input: host_child.py)

ROOT = Path(__file__).resolve().parents[1]
MUSE = ROOT / "components/muse"

# The call's phases and muse_call.c's handling, around the real deadlines:
#   ask T | client T STATE (connecting, live, ended, failed, idle) | run UNTIL
# run polls every 100 ms up to UNTIL (ms) and prints what the gadget did:
# "T end_sent banner=TEXT" when a deadline ends the call.
DRIVER = r"""
#include <stdio.h>
#include <string.h>
#include "muse_call_backstop.h"

enum { NONE, ASKED, CONNECTING, LIVE };
static int phase;
static muse_call_timers_t tm;
static long long clock_ms;

static void poll_until(long long until)
{
    for (; clock_ms <= until; clock_ms += 100) {
        switch (muse_call_timers_due(&tm, phase == ASKED, phase == CONNECTING, clock_ms * 1000)) {
        case MUSE_CALL_DUE_NO_ANSWER:
            printf("%lld end_sent banner=No answer\n", clock_ms);
            phase = NONE;
            break;
        case MUSE_CALL_DUE_BACKSTOP:
            printf("%lld end_sent banner=Couldn't start the call\n", clock_ms);
            phase = NONE;
            break;
        default:
            break;
        }
    }
}

int main(void)
{
    char line[96], st[32];
    long long t;
    while (fgets(line, sizeof(line), stdin)) {
        if (sscanf(line, "ask %lld", &t) == 1) {
            poll_until(t - 1);
            if (phase == NONE) {
                muse_call_timers_asked(&tm, t * 1000);
                phase = ASKED;
            }
        } else if (sscanf(line, "client %lld %31s", &t, st) == 2) {
            poll_until(t - 1);
            if (!strcmp(st, "connecting")) {
                muse_call_timers_connecting(&tm, t * 1000);
                phase = CONNECTING;
            } else if (!strcmp(st, "live")) {
                phase = LIVE;
            } else {
                phase = NONE;
            }
        } else if (sscanf(line, "run %lld", &t) == 1) {
            poll_until(t);
            printf("phase %d\n", phase);
        }
    }
    return 0;
}
"""


class ConnectingBackstop(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.tmp = tempfile.TemporaryDirectory()
        d = Path(cls.tmp.name)
        (d / "driver.c").write_text(DRIVER)
        cls.exe = d / "backstop"
        cc = shlex.split(os.environ.get("CC", "cc"))
        subprocess.run(cc + ["-std=c11", "-Wall", "-Wextra", "-Werror", "-I", str(MUSE), str(d / "driver.c"), "-o",
                             str(cls.exe)], check=True)
        cls.call = (MUSE / "muse_call.c").read_text()

    @classmethod
    def tearDownClass(cls) -> None:
        cls.tmp.cleanup()

    def run_lines(self, lines: list[str]) -> list[str]:
        out = run_child([str(self.exe)], input="\n".join(lines) + "\n", text=True, capture_output=True,
                        check=True).stdout
        return out.splitlines()

    def test_a_silent_client_is_ended_at_60s(self) -> None:
        # Asked at 0, the client's connecting at 1.2 s, then nothing.
        r = self.run_lines(["ask 0", "client 1200 connecting", "run 200000"])
        self.assertEqual(r, ["61300 end_sent banner=Couldn't start the call", "phase 0"])

    def test_a_resent_connecting_restarts_the_60s(self) -> None:
        r = self.run_lines(["ask 0", "client 1200 connecting", "client 50000 connecting", "run 200000"])
        self.assertEqual(r, ["110100 end_sent banner=Couldn't start the call", "phase 0"])
        # Again and again: never while the client keeps saying it's connecting.
        lines = ["ask 0"] + [f"client {t} connecting" for t in range(1000, 300001, 55000)] + ["run 300000"]
        self.assertEqual(self.run_lines(lines), ["phase 2"])

    def test_a_newer_state_ends_connecting(self) -> None:
        self.assertEqual(self.run_lines(["ask 0", "client 1000 connecting", "client 40000 live", "run 400000"]),
                         ["phase 3"])
        for st in ("ended", "failed", "idle"):
            with self.subTest(state=st):
                self.assertEqual(self.run_lines(["ask 0", "client 1000 connecting", f"client 9000 {st}",
                                                 "run 200000"]), ["phase 0"])

    def test_an_unanswered_start_still_ends_at_30s(self) -> None:
        r = self.run_lines(["ask 0", "run 100000"])
        self.assertEqual(r, ["30100 end_sent banner=No answer", "phase 0"])

if __name__ == "__main__":
    unittest.main()
