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

"""The token proof on the device's crypto: mg_proto.c with
components/muse_gadget_ble/mg_token_proof.c on PSA Crypto, against
protocols/test-vectors/mg-token-proof-v1.json (test_mg_ble runs the same
harness on the host reference, tests/mg_token_proof_ref.c).

Needs the Mbed TLS sources ESP-IDF ships (TF-PSA-Crypto), like
test_link_pairing_handshake:
  IDF_PATH=/path/to/esp-idf python3 -m unittest tests.test_mg_token_proof
or MBEDTLS_SOURCE_DIR=<Mbed TLS project root containing tf-psa-crypto/>.
"""

from __future__ import annotations

import os
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_mg_ble import MG, PROTOCOLS, ROOT, codec_build, compile_and_run, proof_vectors_header  # noqa: E402


def psa_build(test: unittest.TestCase, temp: Path) -> tuple[list[Path], list[str]]:
    """Builds TF-PSA-Crypto from the Mbed TLS sources ESP-IDF ships into temp:
    the include directories and the libraries to link (skips the test
    without IDF_PATH or MBEDTLS_SOURCE_DIR)."""
    idf_path = os.environ.get("IDF_PATH")
    crypto_path = os.environ.get("MBEDTLS_SOURCE_DIR")
    if crypto_path:
        mbedtls = Path(crypto_path).resolve()
    elif idf_path:
        mbedtls = Path(idf_path).resolve() / "components/mbedtls/mbedtls"
    else:
        test.skipTest("Set IDF_PATH or MBEDTLS_SOURCE_DIR for the PSA Crypto tests")
    crypto = mbedtls / "tf-psa-crypto"
    test.assertTrue((crypto / "CMakeLists.txt").is_file(), "No tf-psa-crypto in the Mbed TLS sources")
    cc = shlex.split(os.environ.get("CC", "cc"))
    cmake = shutil.which("cmake")
    test.assertTrue(cmake, "CMake is required")
    compat = temp / "compat"
    (compat / "mbedtls").mkdir(parents=True)
    # IDF relocated these public wrappers while keeping the upstream
    # APIs; map them to the software headers, as the pairing test does.
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
        test.assertEqual(r.returncode, 0, r.stdout[-8000:] + r.stderr[-8000:])
    built = build / "tf-psa-crypto"
    libs = [built / "core/libtfpsacrypto.a", built / "drivers/builtin/libbuiltin.a",
            built / "drivers/everest/libeverest.a", built / "drivers/p256-m/libp256m.a"]
    libs = [str(p) for p in libs if p.exists()]
    if sys.platform.startswith("linux"):
        libs = ["-Wl,--start-group", *libs, "-Wl,--end-group"]
    return [crypto / "include", crypto / "drivers/builtin/include", built / "include"], libs


class MgTokenProofTest(unittest.TestCase):
    def test_token_proof_on_psa_against_vectors(self) -> None:
        with tempfile.TemporaryDirectory(prefix="mg-proof-") as directory:
            temp = Path(directory)
            psa_incs, libs = psa_build(self, temp)
            gen = temp / "gen"
            gen.mkdir()
            proof_vectors_header(gen)
            srcs, defs, incs = codec_build(False)
            out = compile_and_run(
                self,
                [ROOT / "tests/mg_proto_harness.c", MG / "mg_proto.c", MG / "mg_linkpol.c", MG / "mg_session.c", MG / "mg_ui.c", MG / "mg_queue.c", MG / "mg_route.c",
                 MG / "mg_codec.c", MG / "mg_token_proof.c", MG / "mg_play.c", MG / "mg_resample.c",
                 ROOT / "components/muse_vstream/muse_vstream.c"],
                defs,
                [gen, MG, MG / "include", ROOT / "components/muse_vstream/include", PROTOCOLS, *psa_incs, *incs],
                extra=libs, vendor=srcs)
            self.assertIn("PASS mg_proto", out)


if __name__ == "__main__":
    unittest.main()
