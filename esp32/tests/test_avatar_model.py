# Copyright (c) Meta Platforms, Inc. and affiliates.
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at http://www.apache.org/licenses/LICENSE-2.0
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
"""Run the real metadata/cache adapter under ASan and UBSan."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
class AvatarModel(unittest.TestCase):
    def test_metadata_and_cache_failures(self):
        found = list((ROOT / "managed_components/espressif__cjson").rglob("cJSON.c"))
        if not found:
            self.skipTest("Run an IDF build first to populate pinned cJSON")
        cjson = found[0]
        with tempfile.TemporaryDirectory() as tmp:
            exe = str(Path(tmp) / "model")
            subprocess.run([*shlex.split(os.environ.get("CC", "cc")), "-std=c11",
                            "-Wall", "-Wextra", "-Werror", "-Wno-deprecated-declarations", "-D_POSIX_C_SOURCE=200809L", "-fsanitize=address,undefined",
                            "-fno-omit-frame-pointer", "-I", str(ROOT / "main"),
                            "-I", str(cjson.parent), str(ROOT / "main/avatar_identity.c"),
                            str(ROOT / "main/avatar_cache.c"), str(ROOT / "tests/avatar_model_harness.c"),
                            str(cjson), "-o", exe], check=True)
            subprocess.run([exe,tmp], check=True, env={**os.environ,
                           "ASAN_OPTIONS":"detect_leaks=0:halt_on_error=1"})
