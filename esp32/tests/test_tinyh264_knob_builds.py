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

"""tinyh264's portable optimization knobs on the host: the decoder built with
each one decodes the repository's H.264 vectors (tests/vectors/h264) to the
same pictures, picture for picture, as the decoder without it
(xplat/tests/h264_corpus.py). TINYH264_PACKED4_MC runs its kernels on the
plain-C model of the packed operations; TINYH264_DSP_MC runs its kernels on the
DSP instructions' plain-C stand-ins, and its first-use check against the C must
pass (a mismatch aborts here instead of falling back to the C). Every build
runs under ASan and UBSan where the compiler's runtime works (the damaged
vectors reach the backends' edge paths); where it doesn't, the pictures are
still compared and the sanitizer subtest skips, saying why."""

from __future__ import annotations

import os
import shlex
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TINY = ROOT.parent / "xplat/tinyh264/src"
CORPUS = ROOT.parent / "xplat/tests/h264_corpus.py"
VECTORS = ROOT / "tests/vectors/h264"

# The first-use check's mismatch message aborts the decode.
CHECKED = ("-include string.h -include stdlib.h "
           "'-DTINYH264_LOG(f, ...)=(strstr(f, \"differs\") ? abort() : (void)0)'")
KNOBS = {
    "packed4": "-DTINYH264_PACKED4_MC=1",
    "dsp_mc": "-DTINYH264_DSP_MC=1 " + CHECKED,
    # Backend switches set to 0 are off (several at 0 are not several backends).
    "backends_0": "-DTINYH264_MVE=0 -DTINYH264_DSP_MC=0 -DTINYH264_PACKED4_MC=0",
}


def sanitizer_problem(tmp: str) -> str | None:
    """None if an ASan and UBSan build runs here, else why not."""
    exe = Path(tmp, "probe")
    cmd = [*shlex.split(os.environ.get("CC", "cc")), "-fsanitize=address,undefined", "-x", "c", "-",
           "-o", str(exe)]
    r = subprocess.run(cmd, input="int main(void) { return 0; }\n", capture_output=True, text=True, timeout=120)
    if r.returncode:
        return f"no ASan/UBSan build here: {r.stderr.strip()[-300:]}"
    r = subprocess.run([str(exe)], capture_output=True, text=True, timeout=60)
    if r.returncode:
        return f"the ASan/UBSan runtime doesn't run here (rc {r.returncode}): {r.stderr.strip()[-300:]}"
    return None


class TinyH264KnobBuildsTest(unittest.TestCase):
    sanitize = False

    def hash(self, tmp: str, name: str, cflags: str) -> Path:
        out = Path(tmp, name + ".txt")
        r = subprocess.run([sys.executable, str(CORPUS), "hash", "--src", str(TINY), f"--cflags={cflags}",
                            *(["--sanitize"] if self.sanitize else []), "-o", str(out), str(VECTORS)],
                           capture_output=True, text=True, timeout=1200)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        return out

    def test_knobs_decode_the_same_pictures(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            problem = sanitizer_problem(tmp)
            self.sanitize = problem is None
            ref = self.hash(tmp, "off", "")
            self.assertIn(" f ", ref.read_text())
            for name, cflags in KNOBS.items():
                with self.subTest(knob=name):
                    out = self.hash(tmp, name, cflags)
                    r = subprocess.run([sys.executable, str(CORPUS), "compare", str(ref), str(out)],
                                       capture_output=True, text=True, timeout=60)
                    self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            with self.subTest("sanitizers"):
                if problem:
                    self.skipTest(problem)


if __name__ == "__main__":
    unittest.main()
