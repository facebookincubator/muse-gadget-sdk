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

"""The gadget's own files (components/muse_apps/muse_apps_flash.c) on the host,
in a temporary directory standing in for the "files" partition."""
import os
import shlex
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
JSON = Path(os.environ.get(
    "CJSON_SOURCE_DIR", ROOT / "managed_components/espressif__cjson/cJSON"
))


class AppFilesTest(unittest.TestCase):
    def test_files(self):
        cc = shlex.split(os.environ.get("CC", "cc"))
        if not cc or shutil.which(cc[0]) is None:
            raise unittest.SkipTest("C compiler not available")
        if not (JSON / "cJSON.c").exists():
            raise unittest.SkipTest("cJSON sources not available (run idf.py build once)")
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp) / "app_files"
            store = Path(tmp) / "files"
            store.mkdir()
            sanitize = ["-fsanitize=address,undefined", "-g"]
            cjson = Path(tmp) / "cjson.o"
            built = subprocess.run([*cc, "-std=c11", "-w", *sanitize, "-c", str(JSON / "cJSON.c"), "-o", str(cjson)],
                                   capture_output=True, text=True)
            self.assertEqual(built.returncode, 0, built.stderr)
            build = subprocess.run(
                [*cc, "-std=c11", "-D_DEFAULT_SOURCE", "-D_DARWIN_C_SOURCE", "-Wall", "-Wextra", "-Werror",
                 "-Wno-unused-function", *sanitize,
                 "-I", str(ROOT / "tests/app_store_fakes"), "-I", str(JSON),
                 "-I", str(ROOT / "components/muse_apps"),
                 str(ROOT / "tests/app_files_harness.c"), str(ROOT / "components/muse_apps/muse_apps_flash.c"),
                 str(cjson), "-o", str(binary)],
                capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            run = subprocess.run([str(binary), str(store)], capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stderr)
            self.assertEqual(run.stdout, "ok\n")


if __name__ == "__main__":
    unittest.main()
