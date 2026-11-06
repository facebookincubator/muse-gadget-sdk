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

"""main/ble_client.c against a scripted NimBLE: late callbacks after a
timeout, error codes versus counts, and links dropping under the worker."""

from __future__ import annotations

import os
import shlex
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
TESTS = ROOT / "tests"


def _sanitizer_flags(cc: list[str]) -> list[str]:
    """AddressSanitizer and UBSan when the compiler can link them."""
    flags = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    with tempfile.TemporaryDirectory() as tmp:
        src = Path(tmp) / "t.c"
        src.write_text("int main(void) { return 0; }\n")
        probe = subprocess.run([*cc, *flags, str(src), "-o", str(Path(tmp) / "t")],
                               capture_output=True, text=True)
    return flags if probe.returncode == 0 else []


class BleClientTest(unittest.TestCase):
    def test_client_against_scripted_nimble(self) -> None:
        cc = shlex.split(os.environ.get("CC", "cc"))
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp) / "ble_client"
            built = subprocess.run(
                [*cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-g",
                 *_sanitizer_flags(cc),
                 "-I", str(TESTS / "ble_client_fakes"), "-I", str(ROOT / "main"),
                 str(TESTS / "ble_client_harness.c"), str(ROOT / "main/ble_client.c"),
                 "-o", str(binary)],
                capture_output=True, text=True,
            )
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            ran = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30)
            self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
            self.assertIn("ble_client ok", ran.stdout)


if __name__ == "__main__":
    unittest.main()
