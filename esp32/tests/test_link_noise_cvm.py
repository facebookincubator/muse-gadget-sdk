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
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
NOISE_C = ROOT / "main" / "noise_control.cpp"
APP_C = ROOT / "main" / "app.c"


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
                return source[brace + 1 : i]
    raise AssertionError(f"could not find end of {signature}")


class LinkNoiseCvmTest(unittest.TestCase):
    def compile_and_run_harness(self) -> None:
        compiler = shlex.split(os.environ.get("CC", "cc"))
        if not compiler or shutil.which(compiler[0]) is None:
            self.skipTest("cc compiler not available")
        source = ROOT / "tests" / "link_noise_cvm_harness.c"
        with tempfile.TemporaryDirectory() as tmpdir:
            binary = Path(tmpdir) / source.stem
            compiled = subprocess.run(
                [
                    *compiler,
                    "-std=c11",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-I",
                    str(ROOT / "main"),
                    str(source),
                    "-o",
                    str(binary),
                ],
                cwd=ROOT,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )
            self.assertEqual(
                compiled.returncode, 0, msg=compiled.stdout + compiled.stderr
            )
            ran = subprocess.run(
                [str(binary)],
                cwd=ROOT,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )
            self.assertEqual(ran.returncode, 0, msg=ran.stdout + ran.stderr)

    def test_msg2_classification(self) -> None:
        """Runs tests/link_noise_cvm_harness.c.

        Covers the protobuf walk that tells a CVM RV-auth msg2 (state +
        32-byte vm_nonce) apart from the legacy raw-attestation payload,
        including truncated and malformed buffers falling back to
        not-a-CVM rather than a false locked verdict.
        """
        self.compile_and_run_harness()

    def test_session_loop_surfaces_cvm_locked(self) -> None:
        """The wiring the harness cannot reach.

        noise_cvm_classify_msg2() spotting a locked CVM only helps if the
        handshake returns it distinctly, the session loop maps it to
        SESSION_CVM_LOCKED with the long backoff and the ws_cvm_locked
        status, and app.c shows the unlock instruction instead of
        re-fetching the (valid) token.
        """
        noise = NOISE_C.read_text()
        app = APP_C.read_text()

        handshake = _function_body(
            noise, "static noise_handshake_result_t noise_handshake(")
        self.assertIn("noise_cvm_unlock_required(msg2_kind)", handshake)
        self.assertIn("Unlock your CVM", handshake)
        self.assertIn("NOISE_HANDSHAKE_CVM_LOCKED", handshake)

        session = _function_body(noise, "static session_result_t run_session(")
        self.assertIn("handshake == NOISE_HANDSHAKE_CVM_LOCKED", session)
        self.assertIn("result = SESSION_CVM_LOCKED;", session)

        policy = _function_body(noise, "static bool update_reconnect_policy(")
        self.assertIn("if (result == SESSION_CVM_LOCKED) {", policy)
        self.assertIn("*backoff_ms = RECONNECT_AUTH_FAILED_MS;", policy)

        task = _function_body(noise, "static void noise_ctrl_task(")
        self.assertIn("if (result == SESSION_CVM_LOCKED) {", task)
        self.assertIn('notify_status("ws_cvm_locked");', task)

        ws_status = _function_body(app, "static void on_ws_control_status(")
        self.assertIn('strcmp(status, "ws_cvm_locked") == 0', ws_status)
        self.assertIn("Unlock your CVM", ws_status)
        cvm_branch = ws_status[ws_status.index('"ws_cvm_locked"'):]
        self.assertNotIn("schedule_vm_auth_refresh", cvm_branch)


if __name__ == "__main__":
    unittest.main()
