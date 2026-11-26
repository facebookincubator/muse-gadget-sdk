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


class LinkNoiseSessionTest(unittest.TestCase):
    def compile_and_run_harness(
        self, compiler_env: str, default_compiler: str, standard: str, source: Path
    ) -> None:
        compiler = shlex.split(os.environ.get(compiler_env, default_compiler))
        if not compiler or shutil.which(compiler[0]) is None:
            self.skipTest(f"{default_compiler} compiler not available")
        with tempfile.TemporaryDirectory() as tmpdir:
            binary = Path(tmpdir) / source.stem
            compiled = subprocess.run(
                [
                    *compiler,
                    f"-std={standard}",
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

    def test_generation_helpers(self) -> None:
        self.compile_and_run_harness(
            "CC",
            "cc",
            "c11",
            ROOT / "tests" / "link_noise_session_harness.c",
        )

    def test_existing_async_operations_capture_the_originating_session(self) -> None:
        noise = NOISE_C.read_text()
        app = APP_C.read_text()

        # The control sender's turn, which run_session() and the device
        # session over BLE both run.
        session = _function_body(noise, "static session_result_t run_session(")
        self.assertIn("if (!control_tx_step(tls, session, control_tx, session_generation,", session)
        dispatch = _function_body(noise, "static bool control_tx_step(")
        stale_callback = dispatch[:dispatch.index("char *json = wrap_result_json(")]
        self.assertIn("pr.session_generation, session_generation", stale_callback)
        self.assertIn("free(pr.request_id)", stale_callback)
        self.assertIn("cJSON_Delete(pr.result)", stale_callback)
        self.assertIn("continue;", stale_callback)

        self.assertIn("args->session_generation = session_generation", app)
        self.assertIn("args->session_generation, args->request_id", app)

    def test_upgrade_request_and_classification(self) -> None:
        """Runs tests/link_noise_upgrade_harness.c.

        Covers what goes on the wire for an upgrade — the exact bearer in an
        Authorization header, vm_id escaped into the request target, CR/LF and
        a missing token refused — and how the response is classified, which is
        where an edge rejection of the bearer has to be told apart from a
        network failure so the session refreshes credentials and applies the
        authentication-failure backoff.
        """
        self.compile_and_run_harness(
            "CC",
            "cc",
            "c11",
            ROOT / "tests" / "link_noise_upgrade_harness.c",
        )

    def test_the_session_loop_acts_on_an_auth_rejection(self) -> None:
        """The wiring the harness cannot reach.

        noise_upgrade_classify() returning NOISE_UPGRADE_AUTH_REJECTED is only
        worth anything if the session loop turns it into SESSION_AUTH_FAILED,
        which is what selects the 60s backoff and raises ws_auth_failed for
        app.c to re-fetch on. Running that end to end needs the FreeRTOS task
        and TLS transport, so this checks the three links in the chain are
        connected.
        """
        noise = NOISE_C.read_text()
        app = APP_C.read_text()

        session = _function_body(noise, "static session_result_t run_session(")
        mapping = session[session.index("ws_upgrade(tls);"):]
        self.assertIn(
            "return upgrade == NOISE_UPGRADE_AUTH_REJECTED ? SESSION_AUTH_FAILED",
            mapping,
        )

        policy = _function_body(noise, "static bool update_reconnect_policy(")
        self.assertIn("if (result == SESSION_AUTH_FAILED) {", policy)
        self.assertIn("*backoff_ms = RECONNECT_AUTH_FAILED_MS;", policy)

        task = _function_body(noise, "static void noise_ctrl_task(")
        self.assertIn("if (result == SESSION_AUTH_FAILED) {", task)
        self.assertIn('notify_status("ws_auth_failed");', task)

        ws_status = _function_body(app, "static void on_ws_control_status(")
        self.assertIn('strcmp(status, "ws_auth_failed") == 0', ws_status)
        self.assertIn("schedule_vm_auth_refresh();", ws_status)

    def test_the_end_of_the_control_stream_ends_the_session(self) -> None:
        """When the VM closes the device's control stream (end_body on it),
        the device is no longer registered, and WebSocket pongs alone would
        keep the session up, unreachable. Runs control_stream_ended() from
        noise_control.cpp and checks the session loop acts on it, after
        reading the messages that frame carried (a Response's inline body
        included)."""
        noise = NOISE_C.read_text()
        define = next(line for line in noise.splitlines()
                      if line.startswith("#define CTRL_STREAM_ID "))
        signature = "static bool control_stream_ended(const DecodedServiceFrame &frame) {"
        start = noise.index(signature)
        function = noise[start:noise.index("\n}\n", start) + 3]

        session = _function_body(noise, "static session_result_t run_session(")
        complete = session[session.index(
            "if (inbound.frame_status == InboundFrameStatus::Complete) {"):]
        # The frame's messages go through dispatch_inbound_frame() (the BLE
        # device session shares it), a Response's inline body included.
        dispatch = _function_body(noise, "static bool dispatch_inbound_frame(")
        self.assertIn("process_inbound_body_chunk(frame.body_chunk.data,", dispatch)
        self.assertIn("process_inbound_body_chunk(frame.response.body,", dispatch)
        # A Response that ends the control stream runs its inline messages first (a final node.unpaired among
        # them), then ends the session.
        ended_in_response = dispatch[:dispatch.index('"server ended the control stream in its response"')]
        ended_in_response = ended_in_response[ended_in_response.rindex("frame.response.end_body) {"):]
        self.assertIn("process_inbound_body_chunk(frame.response.body, session_generation);", ended_in_response)
        ended = complete.index("if (control_stream_ended(inbound.frame)) {")
        self.assertLess(complete.index("dispatch_inbound_frame(inbound.frame,"), ended)
        self.assertIn("error = true;", complete[ended:ended + 300])

        compiler = shlex.split(os.environ.get("CXX", "c++"))
        if not compiler or shutil.which(compiler[0]) is None:
            self.skipTest("c++ compiler not available")
        with tempfile.TemporaryDirectory() as tmpdir:
            (Path(tmpdir) / "control_stream_end.inc").write_text(define + "\n" + function)
            binary = Path(tmpdir) / "control_stream_end"
            compiled = subprocess.run(
                [*compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                 "-I", tmpdir, "-I", str(ROOT / "components" / "noise_core" / "include"),
                 str(ROOT / "tests" / "link_control_stream_end_harness.cpp"),
                 "-o", str(binary)],
                text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            self.assertEqual(compiled.returncode, 0, msg=compiled.stdout + compiled.stderr)
            ran = subprocess.run([str(binary)], text=True,
                                 stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            self.assertEqual(ran.returncode, 0, msg=ran.stdout + ran.stderr)
            self.assertEqual(ran.stdout.strip(), "ok")


if __name__ == "__main__":
    unittest.main()
