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

"""The sealed device session's seal as the ESP32 builds
it: portable/mg_device_session's mg_session_seal.c, vendored unchanged, on
this component's crypto glue (mg_seal_crypto.c over mg_token_proof.c).

- protocols/test-vectors/mg-session-seal-v1.json byte for byte through the
  vendored copy (its own test_seal.c: keys, sealed bytes, opening, the
  reference checker's rejections, every truncation and bit flip, the
  streaming path), on PSA Crypto (the device's; test_mg_session runs the
  host reference).
- The sealed exchange of test_mg_session's "sealed" harness on PSA Crypto,
  sealed_data and sealed_interleaved frame by frame included.
"""

from __future__ import annotations

import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_mg_ble import MG, PROTOCOLS, ROOT, SEAL_INCS, SEAL_SRCS, compile_and_run  # noqa: E402
from test_mg_session import mg_build, seal_header, vectors_header  # noqa: E402
from test_mg_token_proof import psa_build  # noqa: E402

PORTABLE = ROOT.parent / "portable/mg_device_session"
SEAL_VECTORS = PROTOCOLS / "test-vectors/mg-session-seal-v1.json"


def seal_vectors_header(gen: Path) -> None:
    subprocess.run([sys.executable, str(PORTABLE / "tests/gen_seal_vectors.py"), str(SEAL_VECTORS),
                    str(gen / "seal_vectors.h")], check=True)


class SealVectorsTest(unittest.TestCase):
    def run_seal_vectors(self, crypto: list[Path], incs: list[Path], extra: list[str]) -> str:
        with tempfile.TemporaryDirectory(prefix="mg-seal-") as gen:
            seal_vectors_header(Path(gen))
            out = compile_and_run(self, [*SEAL_SRCS, *crypto], [], [Path(gen), MG / "include", PROTOCOLS,
                                                                    *SEAL_INCS, *incs],
                                  extra=extra, vendor=[PORTABLE / "tests/test_seal.c"])
        n = len(json.loads(SEAL_VECTORS.read_text(encoding="utf-8"))["vectors"])
        self.assertIn(f"mg_session_seal: {n} vectors", out)
        return out


    def test_seal_vectors_on_psa(self) -> None:
        with tempfile.TemporaryDirectory(prefix="mg-seal-psa-") as directory:
            psa_incs, libs = psa_build(self, Path(directory))
            self.run_seal_vectors([MG / "mg_token_proof.c"], psa_incs, libs)

    def test_sealed_exchange_on_psa(self) -> None:
        # The "sealed" harness (test_mg_session.SealedSessionTest) with the
        # device's own crypto in place of the host reference.
        with tempfile.TemporaryDirectory(prefix="mg-seal-psa-") as directory:
            temp = Path(directory)
            psa_incs, libs = psa_build(self, temp)
            gen = temp / "gen"
            gen.mkdir()
            vectors_header(gen)
            seal_header(gen)
            srcs, defs, incs, vendor = mg_build(gen)
            srcs = [MG / "mg_token_proof.c" if s.name == "mg_token_proof_ref.c" else s for s in srcs]
            out = compile_and_run(self, [ROOT / "tests/mg_session_harness.c", *srcs], defs, [*incs, *psa_incs],
                                  extra=libs, vendor=vendor, args=["sealed"])
            self.assertIn("PASS mg_session sealed", out)
            # sealed_data (mgcommands.h, Sealed data) on the device's PSA code too.
            steps = json.loads(SEAL_VECTORS.read_text(encoding="utf-8"))["sealed_data_large"]["steps"]
            self.assertIn(f"SEALED_DATA_LARGE {len(steps)} steps byte for byte", out)
            # sealed_credentials_saved on the device's PSA code too.
            sc = json.loads(SEAL_VECTORS.read_text(encoding="utf-8"))["sealed_credentials_saved"]["steps"]
            self.assertIn(f"SEALED_SAVED {len(sc) - 1} updates (session)", out)
            # sealed_interleaved ("Two in flight") too, frame by frame.
            il = json.loads(SEAL_VECTORS.read_text(encoding="utf-8"))["sealed_interleaved"]["steps"]
            self.assertIn(f"SEALED_INTERLEAVED {len(il)} steps frame by frame (stored at once)", out)
            self.assertIn(f"SEALED_INTERLEAVED {len(il)} steps frame by frame (store_later)", out)


if __name__ == "__main__":
    unittest.main()
