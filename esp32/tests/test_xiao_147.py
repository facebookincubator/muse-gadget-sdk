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

"""Exercise the XIAO 1.47 touch report decoding."""
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class Xiao147Tests(unittest.TestCase):
    def test_touch_report_bounds_and_release(self):
        cc = shlex.split(os.environ.get("CC", "cc"))
        if not cc or not shutil.which(cc[0]):
            self.skipTest("host C compiler is not installed")
        with tempfile.TemporaryDirectory() as tmp:
            exe = Path(tmp) / "touch.exe"
            subprocess.run([*cc, "-std=c11", "-Wall", "-Wextra", "-Werror",
                            "-I", str(ROOT / "components/muse/boards"),
                            str(ROOT / "tests/xiao_147_touch_harness.c"), "-o", str(exe)], check=True)
            subprocess.run([str(exe)], check=True, timeout=10)
