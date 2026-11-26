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

"""Runs a ThreadSanitizer binary. On Linux kernels with more address-space
randomization than the sanitizer's runtime expects (Ubuntu 24.04's default
vm.mmap_rnd_bits), it stops at start with "unexpected memory mapping": run it
again with randomization off for that process (setarch -R), and skip the test,
with the reason, if that isn't possible or isn't permitted here (a container
refusing personality(2)). Before the first run, a probe binary (two threads, built
with the same compiler) checks that the ThreadSanitizer runtime runs here at all; if
it doesn't (it crashes, or the compiler can't build it), every TSan test skips with
the reason instead of failing on a crash that isn't the code's."""

import os
import platform
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

MAPPING = "ThreadSanitizer: unexpected memory mapping"
PROBE_C = """#include <pthread.h>
static int shared;
static void *body(void *arg) { shared = 1; return arg; }
int main(void) {
    pthread_t t;
    if (pthread_create(&t, 0, body, 0)) return 2;
    pthread_join(t, 0);
    return shared == 1 ? 0 : 3;
}
"""
_probe_reason = None   # None: not probed yet; "": TSan runs here; else why it doesn't


def _first_line(out: str) -> str:
    lines = (out or "").strip().splitlines()
    return lines[0] if lines else "no output"


def _run_maybe_setarch(argv, **kw) -> subprocess.CompletedProcess:
    r = subprocess.run(argv, **kw)
    setarch = shutil.which("setarch")
    if (MAPPING in (r.stderr or "") + (r.stdout or "") and setarch and platform.system() == "Linux"):
        r = subprocess.run([setarch, platform.machine(), "-R", *argv], **kw)
    return r


def probe() -> str:
    """'' when a ThreadSanitizer binary runs here, else the reason it doesn't (computed once)."""
    global _probe_reason
    if _probe_reason is not None:
        return _probe_reason
    cc = os.environ.get("CC", "cc")
    with tempfile.TemporaryDirectory(prefix="tsan-probe-") as d:
        src = Path(d) / "probe.c"
        exe = Path(d) / "probe"
        src.write_text(PROBE_C)
        try:
            c = subprocess.run([cc, "-fsanitize=thread", "-g", "-O1", "-pthread", str(src), "-o", str(exe)],
                               capture_output=True, text=True, timeout=120)
        except (OSError, subprocess.TimeoutExpired) as e:
            _probe_reason = f"can't build a ThreadSanitizer probe with {cc}: {e}"
            return _probe_reason
        if c.returncode != 0:
            _probe_reason = f"{cc} can't build a ThreadSanitizer binary: {_first_line(c.stderr + c.stdout)}"
            return _probe_reason
        try:
            r = _run_maybe_setarch([str(exe)], capture_output=True, text=True, timeout=60)
        except subprocess.TimeoutExpired:
            _probe_reason = "the ThreadSanitizer probe binary hung here"
            return _probe_reason
        if r.returncode != 0:
            how = f"signal {-r.returncode}" if r.returncode < 0 else f"exit {r.returncode}"
            _probe_reason = (f"the ThreadSanitizer runtime doesn't run here (a two-thread probe ended with {how}: "
                             f"{_first_line(r.stderr + r.stdout)})")
            return _probe_reason
    _probe_reason = ""
    return _probe_reason


def run(test: unittest.TestCase, argv, **kw) -> subprocess.CompletedProcess:
    why = probe()
    if why:
        test.skipTest(why)
    kw.setdefault("capture_output", True)
    kw.setdefault("text", True)
    r = subprocess.run(argv, **kw)
    if MAPPING not in (r.stderr or "") + (r.stdout or ""):
        return r
    setarch = shutil.which("setarch")
    if setarch and platform.system() == "Linux":
        r2 = subprocess.run([setarch, platform.machine(), "-R", *argv], **kw)
        out = (r2.stderr or "") + (r2.stdout or "")
        if "failed to set personality" in out:
            # A container that refuses personality(2) (EPERM): TSan already failed without it.
            test.skipTest("ThreadSanitizer can't map its memory on this kernel, and setarch -R isn't "
                          "permitted here: " + out.strip().splitlines()[0])
        if MAPPING not in out:
            return r2
    test.skipTest("ThreadSanitizer can't map its memory on this kernel (setarch -R unavailable or not enough)")
