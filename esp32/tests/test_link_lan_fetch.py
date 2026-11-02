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

"""main/lan_fetch.c (the lan.fetch command) against a scripted home network
and a fake VM: link_lan_fetch_harness.c has the cases."""

from __future__ import annotations

import os
import shlex
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
JSON_SRC = Path(os.environ.get("CJSON_SOURCE_DIR", ROOT / "managed_components/espressif__cjson/cJSON"))


def _cc_command() -> list[str]:
    cmd = shlex.split(os.environ.get("CC", "cc"))
    if not cmd or shutil.which(cmd[0]) is None:
        raise unittest.SkipTest("C compiler not available")
    return cmd


def _sanitizer_flags(cc: list[str], tmp: Path) -> list[str]:
    probe = tmp / "asan_probe"
    flags = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    proc = subprocess.run(
        [*cc, "-x", "c", "-std=c11", *flags, "-o", str(probe), "-"],
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


class LinkLanFetchHarnessTest(unittest.TestCase):
    def test_lan_fetch(self) -> None:
        cc = _cc_command()
        if not (JSON_SRC / "cJSON.c").exists():
            raise unittest.SkipTest("cJSON source not found; run one idf.py build first")
        with tempfile.TemporaryDirectory() as tmpdir:
            tmp = Path(tmpdir)
            binary = tmp / "link_lan_fetch_harness"
            wrapped_source = tmp / "lan_fetch.c"
            wrapped_source.write_text(
                '#include "lan_fetch_host.h"\n'
                f'#include "{ROOT / "main" / "lan_fetch.c"}"\n'
            )
            sanitizer_flags = _sanitizer_flags(cc, tmp)
            compile_cmd = [
                *cc,
                "-std=c11",
                "-D_GNU_SOURCE",
                "-Wall",
                "-Wextra",
                "-Werror",
                # The fake ESP_LOG macros drop their arguments.
                "-Wno-unused-parameter",
                "-Wno-unused-variable",
                "-Wno-unused-but-set-variable",
                "-g",
                "-O1",
                *sanitizer_flags,
                "-DLINK_FAKE_CUSTOM_TASKS",
                "-DLINK_FAKE_CUSTOM_HEAP_CAPS",
                "-I",
                str(ROOT / "tests" / "lan_fetch_fakes"),
                "-I",
                str(JSON_SRC),
                "-I",
                str(ROOT / "tests" / "link_fakes"),
                "-I",
                str(ROOT / "main"),
                str(ROOT / "tests" / "link_lan_fetch_harness.c"),
                str(wrapped_source),
                str(JSON_SRC / "cJSON.c"),
                "-o",
                str(binary),
            ]
            compile_proc = subprocess.run(
                compile_cmd, cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE
            )
            self.assertEqual(compile_proc.returncode, 0, msg=compile_proc.stdout + compile_proc.stderr)

            env = os.environ.copy()
            if sanitizer_flags:
                env["ASAN_OPTIONS"] = "detect_leaks=1:abort_on_error=1"
                env["UBSAN_OPTIONS"] = "halt_on_error=1"
            run_proc = subprocess.run(
                [str(binary)], cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env
            )
            self.assertEqual(run_proc.returncode, 0, msg=run_proc.stdout + run_proc.stderr)
            self.assertIn("all cases passed", run_proc.stdout)


if __name__ == "__main__":
    unittest.main()
