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
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class Ghash32Test(unittest.TestCase):
    def test_tables_match_spec_vectors_and_reference(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp) / "ghash32"
            built = subprocess.run(
                [*shlex.split(os.environ.get("CC", "cc")), "-std=c11", "-O2",
                 "-Wall", "-Wextra", "-Werror",
                 "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                 "-I", str(ROOT / "main"),
                 str(ROOT / "tests" / "ghash32_harness.c"),
                 str(ROOT / "main" / "ghash32.c"),
                 "-o", str(binary)],
                capture_output=True, text=True)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            ran = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
            self.assertIn("ok", ran.stdout)

    def test_fast_gcm_wraps_idf_entry_points(self) -> None:
        cmake = (ROOT / "main" / "CMakeLists.txt").read_text()
        self.assertIn("-Wl,--wrap=esp_aes_gcm_crypt_and_tag", cmake)
        self.assertIn("-Wl,--wrap=esp_aes_gcm_auth_decrypt", cmake)
        source = (ROOT / "main" / "fast_gcm.c").read_text()
        self.assertIn("__real_esp_aes_gcm_crypt_and_tag(", source)
        self.assertIn("__real_esp_aes_gcm_auth_decrypt(", source)
        # Only 96-bit IVs take the fast path.
        self.assertIn("iv_len != 12", source)


if __name__ == "__main__":
    unittest.main()
