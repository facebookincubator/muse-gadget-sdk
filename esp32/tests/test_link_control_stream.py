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

"""The control stream's framing: process_inbound_body_chunk(), sliced
from main/noise_control.cpp, run on the host with messages cut across
BodyChunks anywhere."""

import os
import shlex
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CONTROL = ROOT / "main/noise_control.cpp"


class LinkControlStream(unittest.TestCase):
    def test_messages_cut_across_body_chunks(self):
        source = CONTROL.read_text()
        start = source.index("// A control-stream message cut across BodyChunks")
        end = source.index("\n}\n", source.index("static void process_inbound_body_chunk(", start)) + 3
        cc = shlex.split(os.environ.get("CC", "cc"))
        cxx = shlex.split(os.environ.get("CXX", "c++"))
        sanitize = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        env = {**os.environ, "ASAN_OPTIONS": "detect_leaks=0:abort_on_error=1",
               "UBSAN_OPTIONS": "halt_on_error=1:print_stacktrace=1"}
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp)
            (out / "control_stream.inc").write_text(source[start:end])
            cjson = out / "cJSON.o"
            built = subprocess.run(
                [*cc, "-c", "-g", *sanitize, "-I", str(ROOT / "tests/link_fakes"),
                 str(ROOT / "tests/link_fakes/cJSON.c"), "-o", str(cjson)],
                capture_output=True, text=True, env=env)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            binary = out / "control_stream"
            built = subprocess.run(
                [*cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-g", *sanitize,
                 "-I", str(ROOT / "main"), "-I", str(ROOT / "tests/link_fakes"), "-I", tmp,
                 "-I", str(ROOT / "components/noise_core/include"),
                 str(ROOT / "tests/link_control_stream_harness.cpp"), str(cjson), "-o", str(binary)],
                capture_output=True, text=True, env=env)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            ran = subprocess.run([str(binary)], capture_output=True, text=True, env=env)
            self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
            self.assertEqual(ran.stdout.strip(), "ok")


if __name__ == "__main__":
    unittest.main()
