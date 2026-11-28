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

"""The firmware version scheme.

version.txt holds the release number; cmake/version.cmake makes every other
build distinct (X.Y.Z+g<commit>[.dirty]) unless HEAD is exactly the board's
release tag <CODE>-vX.Y.Z. The OTA gate takes the new strings, and a device on
the old 999.0.0 placeholder takes any version.
"""

from __future__ import annotations

import os
import shlex
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def function_source(source: str, signature: str) -> str:
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 0
    for end in range(brace, len(source)):
        if source[end] == "{":
            depth += 1
        elif source[end] == "}":
            depth -= 1
            if depth == 0:
                return source[start:end + 1]
    raise AssertionError(f"unclosed {signature}")


class VersionCmakeTest(unittest.TestCase):
    def setUp(self) -> None:
        if shutil.which("cmake") is None or shutil.which("git") is None:
            self.skipTest("cmake or git unavailable")
        self.tmp = tempfile.TemporaryDirectory()
        self.repo = Path(self.tmp.name) / "repo"
        self.repo.mkdir()
        (self.repo / "version.txt").write_text("1.2.3\n")
        (self.repo / "main.c").write_text("int x;\n")
        self.git("init", "-q")
        self.git("add", ".")
        self.git("-c", "user.name=t", "-c", "user.email=t@example.com", "commit", "-qm", "init")

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def git(self, *args: str) -> str:
        return subprocess.run(["git", "-C", str(self.repo), *args], check=True,
                              text=True, stdout=subprocess.PIPE).stdout.strip()

    def cmake(self, body: str) -> str:
        script = Path(self.tmp.name) / "v.cmake"
        script.write_text(f'cmake_minimum_required(VERSION 3.16)\n'  # IN_LIST (CMP0057) before CMake 4
                          f'set(GADGET_SOURCE_DIR "{self.repo}")\n'
                          f'include("{ROOT / "cmake" / "version.cmake"}")\n{body}\n')
        proc = subprocess.run(["cmake", "-P", str(script)], text=True,
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        return proc.stderr.strip()   # message() goes to stderr

    def version(self, code: str) -> str:
        return self.cmake(f'muse_version_compute(v "{self.repo}" "{code}")\nmessage("${{v}}")')

    def test_dev_builds_carry_the_commit(self) -> None:
        sha = self.git("rev-parse", "--short=8", "HEAD")
        self.assertEqual(self.version("M5SW"), f"1.2.3+g{sha}")
        (self.repo / "untracked.txt").write_text("x")   # untracked files don't count
        self.assertEqual(self.version("M5SW"), f"1.2.3+g{sha}")
        (self.repo / "main.c").write_text("int y;\n")
        self.assertEqual(self.version("M5SW"), f"1.2.3+g{sha}.dirty")

    def test_release_tag_is_per_board(self) -> None:
        sha = self.git("rev-parse", "--short=8", "HEAD")
        self.git("tag", "M5SW-v1.2.3")
        self.assertEqual(self.version("M5SW"), "1.2.3")
        self.assertEqual(self.version("EMO1"), f"1.2.3+g{sha}")   # another board's tag
        self.assertEqual(self.version(""), f"1.2.3+g{sha}")
        self.git("tag", "v1.2.3")
        self.assertEqual(self.version(""), "1.2.3")
        (self.repo / "main.c").write_text("int y;\n")   # the tagged commit, changed: not the release
        self.assertEqual(self.version("M5SW"), f"1.2.3+g{sha}.dirty")

    def test_tag_for_another_number_is_not_the_release(self) -> None:
        sha = self.git("rev-parse", "--short=8", "HEAD")
        self.git("tag", "M5SW-v1.2.2")
        self.assertEqual(self.version("M5SW"), f"1.2.3+g{sha}")

    def test_fits_the_app_descriptor(self) -> None:
        (self.repo / "main.c").write_text("int y;\n")
        self.assertLessEqual(len(self.version("EMO1")), 31)   # esp_app_desc_t.version is 32 bytes

    def test_placeholder_and_bad_numbers_refused(self) -> None:
        for bad in ("999.0.0", "1.0", "v1.0.0", "1.0.0-rc1"):
            with self.subTest(bad=bad):
                (self.repo / "version.txt").write_text(bad + "\n")
                script = Path(self.tmp.name) / "bad.cmake"
                script.write_text(f'include("{ROOT / "cmake" / "version.cmake"}")\n'
                                  f'muse_version_compute(v "{self.repo}" "")\n')
                proc = subprocess.run(["cmake", "-P", str(script)], text=True,
                                      stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                self.assertNotEqual(proc.returncode, 0)

    def test_product_code_from_the_overlays(self) -> None:
        defaults = ";".join([
            "sdkconfig.defaults", "devices/sdkconfig.muse",
            "devices/sdkconfig.muse-m5stack-stopwatch", "devices/sdkconfig.muse-video-ble",
        ])
        out = self.cmake(f'set(GADGET_SOURCE_DIR "{ROOT}")\n'
                         f'muse_version_product_code(c "" "{defaults}")\nmessage("${{c}}")')
        self.assertEqual(out, "M5SW")
        gen = Path(self.tmp.name) / "sdkconfig"
        gen.write_text('CONFIG_HOMEHUB_BLE_PRODUCT_CODE="WC6"\n')
        out = self.cmake(f'set(GADGET_SOURCE_DIR "{ROOT}")\n'
                         f'muse_version_product_code(c "{gen}" "{defaults}")\nmessage("${{c}}")')
        self.assertEqual(out, "WC6")   # a build directory's own config wins


class OtaVersionGateTest(unittest.TestCase):
    def test_gate(self) -> None:
        cc = shlex.split(os.environ.get("CC", "cc"))
        if not cc or shutil.which(cc[0]) is None:
            self.skipTest("C compiler unavailable")
        ota = (ROOT / "main" / "ota.c").read_text()
        src = ("#include <stdbool.h>\n#include <stdlib.h>\n#include <string.h>\n#include <stdio.h>\n"
               + function_source(ota, "static void parse_ver(") + "\n"
               + function_source(ota, "static bool version_is_newer(") + "\n" + r'''
int main(int argc, char **argv) {
    for (int i = 1; i + 1 < argc; i += 2) printf("%d\n", version_is_newer(argv[i], argv[i + 1]));
    return 0;
}
''')
        cases = [
            ("1.0.1", "1.0.0", 1),
            ("1.0.0", "1.0.1", 0),
            ("1.0.0", "1.0.0", 0),
            ("1.0.0+g1234abcd", "1.0.0", 1),          # a dev build of the same release
            ("1.0.0", "1.0.0+g1234abcd", 1),          # back to the release
            ("1.0.0+gaaaaaaaa", "1.0.0+gbbbbbbbb", 1),
            ("1.0.0+gaaaaaaaa", "1.0.0+gaaaaaaaa", 0),
            ("1.0.0+gaaaaaaaa", "1.1.0", 0),          # older number: refused without force
            ("1.0.0", "999.0.0", 1),                  # off the placeholder
            ("999.0.0", "999.0.0", 0),
        ]
        with tempfile.TemporaryDirectory() as tmp:
            c = Path(tmp) / "gate.c"
            c.write_text(src)
            exe = Path(tmp) / "gate"
            subprocess.run([*cc, "-std=c11", "-Wall", str(c), "-o", str(exe)], check=True)
            args = [s for cand, cur, _ in cases for s in (cand, cur)]
            out = subprocess.run([str(exe), *args], check=True, text=True,
                                 stdout=subprocess.PIPE).stdout.split()
        for (cand, cur, want), got in zip(cases, out):
            with self.subTest(cand=cand, cur=cur):
                self.assertEqual(int(got), want)


if __name__ == "__main__":
    unittest.main()
