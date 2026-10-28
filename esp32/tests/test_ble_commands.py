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
JSON = Path(os.environ.get(
    "CJSON_SOURCE_DIR", ROOT / "managed_components/espressif__cjson/cJSON"
))


class BleCommandsTest(unittest.TestCase):
    def test_commands_against_a_fake_client(self) -> None:
        if not (JSON / "cJSON.c").exists():
            raise unittest.SkipTest("cJSON sources not available (run idf.py build once)")
        cc = shlex.split(os.environ.get("CC", "cc"))
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp) / "ble_commands"
            cjson = Path(tmp) / "cjson.o"
            built = subprocess.run(
                [*cc, "-std=c11", "-w", "-DCJSON_NESTING_LIMIT=16", "-c",
                 str(JSON / "cJSON.c"), "-o", str(cjson)],
                capture_output=True, text=True)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            built = subprocess.run(
                [*cc, "-std=c11", "-Wall", "-Wextra", "-Werror",
                 "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                 "-D_DEFAULT_SOURCE", "-DCJSON_NESTING_LIMIT=16",
                 "-I", str(ROOT / "tests" / "ble_commands_fakes"),
                 "-I", str(ROOT / "main"), "-I", str(JSON),
                 str(ROOT / "tests" / "ble_commands_harness.c"),
                 str(ROOT / "main" / "ble_commands.c"),
                 str(cjson), "-lm", "-o", str(binary)],
                capture_output=True, text=True)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            ran = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
            self.assertIn("ok", ran.stdout)

    def test_wired_into_register_and_dispatch(self) -> None:
        noise = (ROOT / "main" / "noise_control.cpp").read_text()
        self.assertIn("ble_commands_register(commands);", noise)
        app = (ROOT / "main" / "app.c").read_text()
        self.assertIn("ble_commands_handle(command, params, request_id, session_generation)", app)
        self.assertIn("ble_commands_init(&s_ble_cmd_ops);", app)


if __name__ == "__main__":
    unittest.main()
