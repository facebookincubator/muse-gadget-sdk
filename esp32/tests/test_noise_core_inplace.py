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
import shlex
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
COMPONENT = ROOT / "components" / "noise_core"


class NoiseCoreInPlaceTest(unittest.TestCase):
    def test_in_place_encoding_matches_the_copying_path(self) -> None:
        cxx = shlex.split(os.environ.get("CXX", "c++"))
        if not cxx or shutil.which(cxx[0]) is None:
            raise unittest.SkipTest("C++ compiler not available")
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp) / "noise_core_inplace"
            built = subprocess.run(
                [*cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                 "-fno-exceptions", "-fno-rtti",
                 "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                 "-DNOISE_BUILDING_TESTS",
                 "-I", str(COMPONENT / "include"),
                 str(ROOT / "tests" / "noise_core_inplace_harness.cpp"),
                 str(COMPONENT / "src" / "ServiceCodec.cpp"),
                 str(COMPONENT / "src" / "TransportFrameCodec.cpp"),
                 str(COMPONENT / "src" / "Status.cpp"),
                 "-o", str(binary)],
                capture_output=True, text=True)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            ran = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
            self.assertIn("ok", ran.stdout)

    def test_session_in_place_seal_and_inbound(self) -> None:
        cxx = shlex.split(os.environ.get("CXX", "c++"))
        if not cxx or shutil.which(cxx[0]) is None:
            raise unittest.SkipTest("C++ compiler not available")
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp) / "noise_session_inplace"
            built = subprocess.run(
                [*cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                 "-fno-exceptions", "-fno-rtti",
                 "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                 "-DNOISE_BUILDING_TESTS",
                 "-I", str(COMPONENT / "include"),
                 str(ROOT / "tests" / "noise_session_inplace_harness.cpp"),
                 *(str(COMPONENT / "src" / name) for name in (
                     "ClientSession.cpp", "InitiatorHandshake.cpp", "ServiceCodec.cpp",
                     "Status.cpp", "Transport.cpp", "TransportFrameCodec.cpp")),
                 "-o", str(binary)],
                capture_output=True, text=True)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            ran = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
            self.assertIn("session in-place ok", ran.stdout)


if __name__ == "__main__":
    unittest.main()
