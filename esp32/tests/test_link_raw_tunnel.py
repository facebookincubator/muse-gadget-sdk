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
import re
import shutil
import shlex
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))   # its helpers, also when run from esp32/ as tests.<module>
from host_child import run_child  # noqa: E402 (macOS's pipe deadlock on a driver's input: host_child.py)


ROOT = Path(__file__).resolve().parents[1]


def _cc_command() -> list[str]:
    cmd = shlex.split(os.environ.get("CXX", "c++"))
    if not cmd or shutil.which(cmd[0]) is None:
        raise unittest.SkipTest("C++ compiler not available")
    return cmd


def _sanitizer_flags(cc: list[str], tmp: Path) -> list[str]:
    probe = tmp / "asan_probe"
    flags = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    proc = run_child(
        [
            *cc,
            "-x",
            "c++",
            "-std=c++17",
            *flags,
            "-o",
            str(probe),
            "-",
        ],
        input=b"int main(void) { return 0; }\n",
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    if proc.returncode != 0:
        return []
    run = subprocess.run(
        [str(probe)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        env={**os.environ, "ASAN_OPTIONS": "detect_leaks=1:abort_on_error=1"},
    )
    return flags if run.returncode == 0 else []


class LinkNoiseTransportHarnessTest(unittest.TestCase):
    def test_noise_transport_auth_upgrade_and_reconnect_policy(self) -> None:
        cc = _cc_command()
        with tempfile.TemporaryDirectory() as tmpdir:
            tmp = Path(tmpdir)
            binary = tmp / "link_noise_transport_harness"
            # Test the relocated production policy and upgrade path. The
            # upgrade result block is kept intact so HTTP auth classification
            # cannot silently degrade into a generic rejection.
            source = (ROOT / "main" / "noise_control.cpp").read_text()
            policy = source[source.index("enum session_result_t {") :
                            source.index("static void reconnect_delay(")]
            constants = "\n".join(re.findall(
                r"^#define (?:RECONNECT_\w+|STABLE_SESSION_MS|VM_REFRESH_FAILURES) +[^\n]+",
                source, re.MULTILINE))
            (tmp / "noise_reconnect.inc").write_text(constants + "\n" + policy)
            upgrade = source[source.index("static bool upgrade_status_readable(") :
                             source.index("// ---- Noise handshake")]
            (tmp / "noise_upgrade.inc").write_text(upgrade)
            session_start = source.index("static session_result_t run_session(")
            start = source.index(
                "    noise_upgrade_result_t upgrade = ws_upgrade(tls);",
                session_start,
            )
            end = source.index("\n    }", start) + len("\n    }")
            (tmp / "noise_upgrade_result.inc").write_text(source[start:end])
            sanitizer_flags = _sanitizer_flags(cc, tmp)
            compile_cmd = [
                *cc,
                "-std=c++17",
                "-D_GNU_SOURCE",
                "-Wall",
                "-Wextra",
                "-g",
                "-O1",
                *sanitizer_flags,
                "-I",
                str(ROOT / "tests" / "link_fakes"),
                "-I",
                str(ROOT / "main"),
                str(ROOT / "tests" / "link_noise_transport_harness.cpp"),
                "-I",
                str(tmp),
                "-o",
                str(binary),
            ]
            compile_proc = subprocess.run(
                compile_cmd,
                cwd=ROOT,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )
            self.assertEqual(
                compile_proc.returncode,
                0,
                msg=compile_proc.stdout + compile_proc.stderr,
            )

            env = os.environ.copy()
            if sanitizer_flags:
                env["ASAN_OPTIONS"] = "detect_leaks=1:abort_on_error=1"
                env["UBSAN_OPTIONS"] = "halt_on_error=1"
            run_proc = subprocess.run(
                [str(binary)],
                cwd=ROOT,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                env=env,
            )
            self.assertEqual(
                run_proc.returncode,
                0,
                msg=run_proc.stdout + run_proc.stderr,
            )


if __name__ == "__main__":
    unittest.main()
