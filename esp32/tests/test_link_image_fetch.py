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
import shutil
import shlex
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def _cc_command() -> list[str]:
    cmd = shlex.split(os.environ.get("CC", "cc"))
    if not cmd or shutil.which(cmd[0]) is None:
        raise unittest.SkipTest("C compiler not available")
    return cmd


def _sanitizer_flags(cc: list[str], tmp: Path) -> list[str]:
    probe = tmp / "asan_probe"
    flags = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    proc = subprocess.run(
        [
            *cc,
            "-x",
            "c",
            "-std=c11",
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


class LinkImageFetchHarnessTest(unittest.TestCase):
    def _run_harness(self, idle: int) -> None:
        cc = _cc_command()
        with tempfile.TemporaryDirectory() as tmpdir:
            tmp = Path(tmpdir)
            binary = tmp / "link_image_fetch_harness"
            sanitizer_flags = _sanitizer_flags(cc, tmp)
            compile_cmd = [
                *cc,
                "-std=c11",
                "-D_GNU_SOURCE",
                f"-DCONFIG_HOMEHUB_WIFI_IDLE_MAX_MODEM={idle}",
                "-Wall",
                "-Wextra",
                # The fake ESP_LOG macros drop their arguments.
                "-Wno-unused-parameter",
                "-Wno-unused-variable",
                "-g",
                "-O1",
                *sanitizer_flags,
                "-DLINK_FAKE_CUSTOM_TASKS",
                "-DLINK_FAKE_CUSTOM_HEAP_CAPS",
                "-I",
                str(ROOT / "tests" / "image_fetch_fakes"),
                "-I",
                str(ROOT / "tests" / "link_fakes"),
                "-I",
                str(ROOT / "main"),
                str(ROOT / "tests" / "link_image_fetch_harness.c"),
                str(ROOT / "main" / "image_fetch.c"),
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

    def test_redirect_scheme_lock_and_deadline(self) -> None:
        self._run_harness(0)

    def test_idle_modem_transfer_lifetime(self) -> None:
        self._run_harness(1)


if __name__ == "__main__":
    unittest.main()
