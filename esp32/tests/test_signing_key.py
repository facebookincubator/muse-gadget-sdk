#!/usr/bin/env python3
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
"""A production build
(CONFIG_HOMEHUB_DEV_BUILD=n) that signs its images must not use the
repository's dev_signing_key.pem (under any name) and fails without a readable
key; dev builds, the default, are unchanged (cmake/signing_key.cmake, run here
in CMake's script mode on the real function)."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class SigningKey(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.cmake = shutil.which("cmake")
        if not cls.cmake:
            raise unittest.SkipTest("no cmake")
        openssl = shutil.which("openssl")
        if not openssl:
            raise unittest.SkipTest("no openssl")
        cls.tmp = Path(tempfile.mkdtemp(prefix="signing_key_"))
        shutil.copy(ROOT / "dev_signing_key.pem", cls.tmp / "release.pem")   # the dev key renamed
        (cls.tmp / "garbled.pem").write_text("-----BEGIN PRIVATE KEY-----\nnot a key\n-----END PRIVATE KEY-----\n")
        subprocess.run([openssl, "ecparam", "-name", "prime256v1", "-genkey", "-noout", "-out",
                        str(cls.tmp / "other.pem")], check=True, capture_output=True)
        # the dev key in another encoding: a different file, the same public key
        subprocess.run([openssl, "pkey", "-in", str(ROOT / "dev_signing_key.pem"), "-outform", "DER", "-out",
                        str(cls.tmp / "dev.der")], check=True, capture_output=True)

    @classmethod
    def tearDownClass(cls) -> None:
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def check(self, dev: str, signs: str, key: str, project: Path = ROOT):
        script = self.tmp / "case.cmake"
        script.write_text(f'include("{ROOT / "cmake/signing_key.cmake"}")\n'
                          f'muse_check_signing_key("{dev}" "{signs}" "{key}" "{project}")\nmessage(STATUS "ok")\n')
        r = subprocess.run([self.cmake, "-P", str(script)], capture_output=True, text=True, cwd=self.tmp)
        return r.returncode, r.stdout + r.stderr

    def test_production_refuses_the_dev_key(self):
        rc, out = self.check("", "y", "dev_signing_key.pem")
        self.assertNotEqual(rc, 0, out)
        self.assertIn("shared dev signing key ('", " ".join(out.split()))
        rc, out = self.check("", "y", "release.pem", self.tmp)   # the same key under another name
        self.assertNotEqual(rc, 0, out)
        self.assertIn("shared dev signing key ('", " ".join(out.split()))
        rc, out = self.check("", "y", "dev.der", self.tmp)       # ... and in another encoding
        self.assertNotEqual(rc, 0, out)
        self.assertIn("shared dev signing key ('", " ".join(out.split()))

    def test_production_fails_closed_on_a_key_it_cant_read(self):
        rc, out = self.check("", "y", "garbled.pem", self.tmp)
        self.assertNotEqual(rc, 0, out)
        self.assertIn("can't read", " ".join(out.split()))

    def test_production_needs_a_key(self):
        for key in ("", "missing.pem", "."):
            with self.subTest(key=key):
                rc, out = self.check("", "y", key, self.tmp)
                self.assertNotEqual(rc, 0, out)
                self.assertIn("no signing key", " ".join(out.split()))

    def test_production_with_another_key_builds(self):
        rc, out = self.check("", "y", "other.pem", self.tmp)
        self.assertEqual(rc, 0, out)

    def test_dev_builds_unchanged(self):
        self.assertEqual(self.check("y", "y", "dev_signing_key.pem")[0], 0)
        self.assertEqual(self.check("y", "y", "missing.pem", self.tmp)[0], 0)
        self.assertEqual(self.check("", "", "dev_signing_key.pem")[0], 0)   # not signing here: nothing to check

    def test_wired_and_dev_by_default(self):
        cfg = (ROOT / "cmake/validate_config.cmake").read_text()
        self.assertIn('muse_check_signing_key("${CONFIG_HOMEHUB_DEV_BUILD}" "${CONFIG_SECURE_BOOT_BUILD_SIGNED_BINARIES}"', cfg)
        kc = (ROOT / "main/Kconfig.projbuild").read_text()
        i = kc.index("config HOMEHUB_DEV_BUILD")
        self.assertIn("default y", kc[i:i + 200])
        self.assertIn('CONFIG_SECURE_BOOT_SIGNING_KEY="dev_signing_key.pem"', (ROOT / "sdkconfig.defaults").read_text())


if __name__ == "__main__":
    unittest.main()
