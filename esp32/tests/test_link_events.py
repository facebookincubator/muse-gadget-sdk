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

from __future__ import annotations

import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


class LinkEventsHarnessTest(unittest.TestCase):
    def test_connectivity_history_ring(self) -> None:
        cc = shlex.split(os.environ.get("CC", "cc"))
        if not cc or shutil.which(cc[0]) is None:
            self.skipTest("C compiler not available")
        with tempfile.TemporaryDirectory() as tmpdir:
            binary = Path(tmpdir) / "link_events_harness"
            compiled = subprocess.run(
                [
                    *cc, "-std=c11", "-D_GNU_SOURCE",
                    "-DLINK_FAKE_CUSTOM_HEAP_CAPS=1", "-DLINK_FAKE_CUSTOM_TIMER=1",
                    "-Wall", "-Wextra", "-Werror", "-g",
                    "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                    "-I", str(ROOT / "tests" / "link_fakes"),
                    "-I", str(ROOT / "main"),
                    str(ROOT / "tests" / "link_events_harness.c"),
                    str(ROOT / "main" / "link_events.c"),
                    str(ROOT / "tests" / "link_fakes" / "cJSON.c"),
                    "-o", str(binary),
                ],
                text=True, capture_output=True,
            )
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            env = {**os.environ, "ASAN_OPTIONS": "detect_leaks=0:abort_on_error=1",
                   "UBSAN_OPTIONS": "halt_on_error=1"}
            ran = subprocess.run([str(binary)], text=True, capture_output=True, env=env)
            self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)


if __name__ == "__main__":
    unittest.main()
