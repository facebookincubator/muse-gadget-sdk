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

"""Muse records a gadget's SDK token only at pairing.

A token set afterwards (in the build, over serial or in Settings) takes effect
only once the gadget pairs again, so the firmware remembers the token pairing
used and says so when it differs. Refreshing the device token does not record
one, so the firmware must not refresh to "report" it.
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
BUILD_TOKEN = "mgst_" + "A" * 43


def _function_body(source: str, signature: str) -> str:
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 0
    for i in range(brace, len(source)):
        if source[i] == "{":
            depth += 1
        elif source[i] == "}":
            depth -= 1
            if depth == 0:
                return source[brace:i + 1]
    raise AssertionError(f"unterminated {signature}")


class LinkIdentityHarnessTest(unittest.TestCase):
    def test_sdk_token_pairing(self) -> None:
        cc = shlex.split(os.environ.get("CC", "cc"))
        if not cc or shutil.which(cc[0]) is None:
            raise unittest.SkipTest("C compiler not available")

        with tempfile.TemporaryDirectory() as tmpdir:
            binary = Path(tmpdir) / "link_identity_harness"
            compile_proc = subprocess.run(
                [
                    *cc,
                    "-std=c11",
                    "-Wall",
                    "-Wextra",
                    f'-DBUILD_TOKEN="{BUILD_TOKEN}"',
                    f'-DCONFIG_GADGET_SDK_TOKEN="{BUILD_TOKEN}"',
                    '-DCONFIG_HOMEHUB_BLE_NAME_PREFIX="MuseGadget"',
                    '-DCONFIG_HOMEHUB_BLE_NAME_SUFFIX=""',
                    "-I",
                    str(ROOT / "tests" / "identity_fakes"),
                    "-I",
                    str(ROOT / "tests" / "link_fakes"),
                    "-I",
                    str(ROOT / "main"),
                    str(ROOT / "tests" / "link_identity_harness.c"),
                    str(ROOT / "main" / "identity.c"),
                    "-o",
                    str(binary),
                ],
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

            run_proc = subprocess.run(
                [str(binary)],
                cwd=ROOT,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )
            self.assertEqual(
                run_proc.returncode,
                0,
                msg=run_proc.stdout + run_proc.stderr,
            )


class LinkSdkTokenPairingContractTest(unittest.TestCase):
    def setUp(self) -> None:
        self.app = (ROOT / "main" / "app.c").read_text()

    def test_pairing_notes_the_token_it_used(self) -> None:
        for signature in (
            "static bool accept_pairing_credentials(",
            "static bool mint_and_store_device_tokens_unlocked(",
        ):
            self.assertIn(
                "identity_note_paired_sdk_token()",
                _function_body(self.app, signature),
                f"{signature} must note the SDK token pairing used",
            )

    def test_refresh_does_not_note_or_report_the_token(self) -> None:
        body = _function_body(self.app, "static bool ensure_access_token_ready_with_gate_held(")
        self.assertNotIn("identity_note_paired_sdk_token", body)
        self.assertNotIn("identity_sdk_token", body)

    def test_unpairing_forgets_the_noted_token(self) -> None:
        source = (ROOT / "main" / "config_store.c").read_text()
        for signature in ("bool config_clear_pairing(", "bool config_clear_setup("):
            self.assertIn('"pair_sdk_hint"', _function_body(source, signature))


if __name__ == "__main__":
    unittest.main()
