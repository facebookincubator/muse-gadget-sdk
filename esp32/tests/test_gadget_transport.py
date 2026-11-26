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

"""Gadget routes on the real session/codec; general daemon requests unchanged."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

from test_link_ota import function_source

ROOT = Path(__file__).resolve().parents[1]


class GadgetTransportTest(unittest.TestCase):
    def test_routes_frames_and_identity(self):
        source = (ROOT / "main/noise_control.cpp").read_text()
        constants = source[source.index("static constexpr ServiceType GADGET_SERVICE"):
                           source.index("enum class json_send_status")]
        start = source.index("struct tunnel_emit_ctx {")
        tunnel = source[start:source.index("// ---- Extra daemon requests", start)]
        start = source.index("struct identity_fetch {")
        end = source.index("// ----", start)
        identity = source[start:end]
        start = source.index("    // 4. Open POST")
        open_control = source[start:source.index("    // Drain any stale results", start)]
        harness = (ROOT / "tests/gadget_transport_harness.cpp").read_text()
        harness = harness.replace("@CONSTANTS@", constants)
        harness = harness.replace("@FLUSH@", function_source(source, "static bool flush_outbound("))
        harness = harness.replace("@TUNNEL@", tunnel).replace("@IDENTITY@", identity)
        harness = harness.replace("@OPEN_CONTROL@", open_control)
        core = ROOT / "components/noise_core"
        with tempfile.TemporaryDirectory() as name:
            tmp = Path(name)
            (tmp / "harness.cpp").write_text(harness)
            flags = ["-Wall", "-Wextra", "-Werror", "-g", "-I", str(ROOT / "tests"),
                     "-I", str(ROOT / "tests/link_fakes"), "-I", str(core / "include")]
            sanitizer = os.environ.get("HOST_SANITIZERS", "address,undefined")
            if sanitizer:
                flags += [f"-fsanitize={sanitizer}", "-fno-omit-frame-pointer"]
            compiled = subprocess.run([*shlex.split(os.environ.get("CC", "cc")), "-std=c11", *flags,
                                       "-c", str(ROOT / "tests/link_fakes/cJSON.c"), "-o", str(tmp / "json.o")],
                                      capture_output=True, text=True)
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            sources = [core / "src" / (n + ".cpp") for n in (
                "ClientSession", "InitiatorHandshake", "ServiceCodec", "Status", "Transport", "TransportFrameCodec")]
            binary = tmp / "routes"
            compiled = subprocess.run([*shlex.split(os.environ.get("CXX", "c++")), "-std=c++17", *flags,
                                       str(tmp / "harness.cpp"), str(tmp / "json.o"), *map(str, sources),
                                       "-o", str(binary)], capture_output=True, text=True)
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            ran = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)

    def test_daemon_requests_and_control_rejection_policy_are_preserved(self):
        source = (ROOT / "main/noise_control.cpp").read_text()
        for name in ("static bool req_open(", "static bool req_pump_tx("):
            self.assertIn("ServiceType::Daemon, op.id", function_source(source, name))
        session = function_source(source, "static session_result_t run_session(")
        # Only Wi-Fi's session left the daemon routes: the device session over
        # BLE (ble_session_begin) still opens POST /link-control on the daemon,
        # the route the phone's relay carries, and its control chunks go there
        # (control_service<BleSessionOut>).
        self.assertNotIn('"/link-control"', session)
        self.assertIn("req.path = StringView(GADGET_CONTROL_PATH);", session)
        ble = function_source(source, "static bool ble_session_begin(")
        self.assertIn('req.path = StringView("/link-control");', ble)
        self.assertIn("ServiceType::Daemon, CTRL_STREAM_ID, req,", ble)
        self.assertNotIn('"/link-tunnel"', source)
        self.assertNotIn('"/identity"', source)
        # A refused control stream ends the session: dispatch_inbound_frame()
        # (shared with the BLE session) says so, and run_session() ends.
        dispatch = function_source(source, "static bool dispatch_inbound_frame(")
        reject = dispatch[dispatch.index('ESP_LOGE(TAG, "server rejected the control stream') :]
        self.assertIn("return false;", reject[:260])
        loop = session[session.index("if (!dispatch_inbound_frame(inbound.frame, identity, session_generation, &end_reason)) {") :]
        self.assertIn("error = true;", loop[:160])
        self.assertIn("break;", loop[:200])
        # The route move keeps the baseline Noise session handshake unchanged.
        self.assertIn("noise_handshake(tls, session, ws_buf, ws_buf_size)", session)


if __name__ == "__main__":
    unittest.main()
