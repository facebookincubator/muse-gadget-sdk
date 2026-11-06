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

"""tinyh264's ESP32-S31 SIMD interpolation on the host: a model of the
assembly's lane operations against the C, and h264bsd_reconstruct.c's hooks
and first-use check with the model in place of the assembly
(tests/tinyh264_pie_harness.c)."""

from __future__ import annotations

import os
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TINY = ROOT / "components/tinyh264/src"


class TinyH264PieTest(unittest.TestCase):
    def test_model_and_glue(self) -> None:
        cc = os.environ.get("CC", "cc")
        srcs = sorted(str(p) for p in TINY.glob("*.c"))
        with tempfile.TemporaryDirectory() as tmp:
            Path(tmp, "esp_log.h").write_text(
                "#include <stdio.h>\n"
                "#define ESP_LOGI(tag, fmt, ...) printf(\"I %s: \" fmt \"\\n\", tag, ##__VA_ARGS__)\n"
                "#define ESP_LOGW(tag, fmt, ...) printf(\"W %s: \" fmt \"\\n\", tag, ##__VA_ARGS__)\n")
            exe = Path(tmp, "pie")
            build = subprocess.run(
                [cc, "-O1", "-w", "-DTINYH264_PIE_S31=1", f"-I{tmp}", f"-I{TINY}", "-o", str(exe),
                 str(ROOT / "tests/tinyh264_pie_harness.c"), *srcs, "-lm"],
                capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=300)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertIn("PASS tinyh264_pie", run.stdout)
            self.assertIn("matches C", run.stdout)


if __name__ == "__main__":
    unittest.main()
