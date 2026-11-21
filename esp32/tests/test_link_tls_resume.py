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

"""The VM WebSocket's TLS session resumption: the tls_resume_* helpers,
sliced from main/noise_control.cpp, run on the host. A connect that failed
before TLS began (DNS, TCP) keeps the session; a TLS-level failure drops it."""

import os
import shlex
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CONTROL = ROOT / "main/noise_control.cpp"


class LinkTlsResume(unittest.TestCase):
    def test_session_kept_across_network_failures(self):
        source = CONTROL.read_text()
        start = source.index("// TLS session resumption for the VM WebSocket")
        end = source.index("#endif", source.index("static void tls_resume_failed(", start))
        cxx = shlex.split(os.environ.get("CXX", "c++"))
        sanitize = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        env = {**os.environ, "ASAN_OPTIONS": "detect_leaks=0:abort_on_error=1",
               "UBSAN_OPTIONS": "halt_on_error=1:print_stacktrace=1"}
        with tempfile.TemporaryDirectory() as tmp:
            (Path(tmp) / "tls_resume.inc").write_text(source[start:end])
            binary = Path(tmp) / "tls_resume"
            built = subprocess.run(
                [*cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-g", *sanitize, "-I", tmp,
                 str(ROOT / "tests/link_tls_resume_harness.cpp"), "-o", str(binary)],
                capture_output=True, text=True, env=env)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            ran = subprocess.run([str(binary)], capture_output=True, text=True, env=env)
            self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
            self.assertEqual(ran.stdout.strip(), "ok")


if __name__ == "__main__":
    unittest.main()
