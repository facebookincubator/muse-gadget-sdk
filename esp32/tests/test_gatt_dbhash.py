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
"""The GATT Database Hash (main/gatt_dbhash*.c).

- tools/gatt_dbhash.py, the independent reference, against RFC 4493's CMAC
  vectors and the spec's serialization rules;
- main/gatt_dbhash_core.c on PSA Crypto (built from ESP-IDF's tf-psa-crypto,
  as test_mg_token_proof does; skipped without IDF_PATH or
  MBEDTLS_SOURCE_DIR) against the reference, byte for byte.
"""
import importlib.util
import os
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("gatt_dbhash", ROOT / "tools" / "gatt_dbhash.py")
ref = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ref)

TABLE = [
    (1, 0x2800, bytes.fromhex("0018")),
    (2, 0x2803, bytes.fromhex("020300002a")),
    (3, 0, b""),
    (4, 0x2800, bytes.fromhex("0118")),
    (5, 0x2803, bytes.fromhex("200600052a")),
    (6, 0, b""),
    (7, 0x2902, b""),
    (8, 0x2800, bytes.fromhex("9ecadc240ee5a9e093f3a3b50100406e")),
    (9, 0x2803, bytes.fromhex("100a009ecadc240ee5a9e093f3a3b50300406e")),
    (10, 0, b""),
    (11, 0x2902, b""),
    (12, 0x2901, b""),
    (13, 0x2900, bytes.fromhex("0100")),
    (14, 0x2908, b""),
]


def have_cryptography():
    try:
        import cryptography  # noqa: F401
        return True
    except ImportError:
        return False


class Reference(unittest.TestCase):
    def test_serialization_rules(self):
        s = ref.serialize(TABLE)
        # declarations with values, CCC/user description by handle and type only, values and others skipped
        self.assertTrue(s.startswith(bytes.fromhex("0100002800180200032802030000" "2a")))
        self.assertIn(bytes.fromhex("07000229"), s)          # CCC: handle 7, type 0x2902, no value
        self.assertIn(bytes.fromhex("0c000129"), s)          # user description
        self.assertIn(bytes.fromhex("0d0000290100"), s)      # extended properties with its value
        self.assertNotIn(bytes.fromhex("0e000829"), s)       # 0x2908 isn't hashed
        self.assertNotIn(bytes.fromhex("03000000"), s)       # a value attribute isn't
        self.assertEqual(ref.serialize(list(reversed(TABLE))), s)   # handle order, whatever the input order

    @unittest.skipUnless(have_cryptography(), "needs cryptography")
    def test_cmac_rfc4493(self):
        key = bytes.fromhex("2b7e151628aed2a6abf7158809cf4f3c")
        self.assertEqual(ref.cmac(key, b"").hex(), "bb1d6929e95937287fa37d129b756746")
        self.assertEqual(ref.cmac(key, bytes.fromhex("6bc1bee22e409f96e93d7e117393172a")).hex(),
                         "070a16b46b4d4144f79bdd9dd04a287c")


class RealTable(unittest.TestCase):
    """The ESP-Mosaico's own walk (tests/vectors/gatt_dbhash_emo1.txt): its
    shape (every handle from 1, the Generic Attribute service with Service
    Changed, Client Supported Features and the hash) and its hash."""

    def setUp(self):
        self.attrs, self.device = ref.parse((ROOT / "tests/vectors/gatt_dbhash_emo1.txt").read_text())

    @unittest.skipUnless(have_cryptography(), "needs cryptography")
    def test_hash_matches_the_reference(self):
        self.assertEqual(self.device, ref.database_hash(self.attrs))


class CoreOnPsa(unittest.TestCase):
    @unittest.skipUnless(have_cryptography(), "needs cryptography")
    def test_core_matches_the_reference(self):
        idf_path = os.environ.get("IDF_PATH")
        crypto_path = os.environ.get("MBEDTLS_SOURCE_DIR")
        if crypto_path:
            mbedtls = Path(crypto_path).resolve()
        elif idf_path:
            mbedtls = Path(idf_path).resolve() / "components/mbedtls/mbedtls"
        else:
            self.skipTest("Set IDF_PATH or MBEDTLS_SOURCE_DIR for the PSA build")
        crypto = mbedtls / "tf-psa-crypto"
        self.assertTrue((crypto / "CMakeLists.txt").is_file(), "No tf-psa-crypto in the Mbed TLS sources")
        cc = shlex.split(os.environ.get("CC", "cc"))
        cmake = shutil.which("cmake")
        self.assertTrue(cmake, "CMake is required")
        with tempfile.TemporaryDirectory(prefix="gatt-dbhash-") as directory:
            temp = Path(directory)
            compat = temp / "compat"
            (compat / "mbedtls").mkdir(parents=True)
            # IDF relocated these public wrappers; map them to the software headers (test_mg_token_proof).
            for name in ("bignum", "ecp"):
                (compat / "mbedtls" / f"{name}.h").write_text(
                    '#pragma once\n#define MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS\n'
                    f'#include "mbedtls/private/{name}.h"\n')
            (compat / "sdkconfig.h").write_text("")
            build = temp / "crypto-build"
            for cmd in (
                [cmake, "-S", str(mbedtls), "-B", str(build), "-DENABLE_TESTING=OFF", "-DENABLE_PROGRAMS=OFF",
                 "-DGEN_FILES=OFF", "-DMBEDTLS_FATAL_WARNINGS=OFF", "-DDISABLE_PACKAGE_CONFIG_AND_INSTALL=ON",
                 "-DCMAKE_BUILD_TYPE=Release", f"-DCMAKE_C_COMPILER={cc[0]}",
                 "-DCMAKE_C_FLAGS=-I" + shlex.quote(str(compat))],
                [cmake, "--build", str(build), "--target", "tfpsacrypto", "-j", "4"],
            ):
                r = subprocess.run(cmd, cwd=temp, text=True, capture_output=True)
                self.assertEqual(r.returncode, 0, r.stdout[-8000:] + r.stderr[-8000:])
            built = build / "tf-psa-crypto"
            libs = [built / "core/libtfpsacrypto.a", built / "drivers/builtin/libbuiltin.a",
                    built / "drivers/everest/libeverest.a", built / "drivers/p256-m/libp256m.a"]
            libs = [str(p) for p in libs if p.exists()]
            if sys.platform.startswith("linux"):
                libs = ["-Wl,--start-group", *libs, "-Wl,--end-group"]
            exe = temp / "harness"
            cmd = [*cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "main"),
                   "-I", str(crypto / "include"), "-I", str(crypto / "drivers/builtin/include"), "-I", str(built / "include"),
                   str(ROOT / "tests/gatt_dbhash_harness.c"), str(ROOT / "main/gatt_dbhash_core.c"), *libs, "-o", str(exe)]
            r = subprocess.run(cmd, text=True, capture_output=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            out = subprocess.run([str(exe)], text=True, capture_output=True, check=True).stdout
        lines = dict(l.split(" ", 1) for l in out.strip().splitlines())
        self.assertEqual(lines["serial"], ref.serialize(TABLE).hex())
        self.assertEqual(lines["small"], "0")
        self.assertEqual(lines["hash"], ref.database_hash(TABLE).hex())
        self.assertEqual(lines["rfc4493"], bytes.fromhex("070a16b46b4d4144f79bdd9dd04a287c")[::-1].hex())
        self.assertIn("PASS gatt_dbhash", out)


if __name__ == "__main__":
    unittest.main()
