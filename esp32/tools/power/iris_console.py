#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy at http://www.apache.org/licenses/LICENSE-2.0
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""PTEST byte-stream adapter over Mosaico's managed ESP-Iris Gateway."""
import base64
import json
from pathlib import Path
import re
import struct
import subprocess
import time

SERVICE = "0x5054"


def validate_iris_args(device_id, workspace):
    """Validate local arguments without touching the Gateway or any device."""
    if not re.fullmatch(r"[0-9a-fA-F]{32}", device_id):
        raise ValueError("Iris requires an explicit 32-digit device ID")
    workspace = Path(workspace).expanduser().resolve()
    if not (workspace / "mosaico.py").is_file():
        raise ValueError("--mosaico-workspace must contain mosaico.py")
    return device_id.lower(), workspace


class IrisConsole:
    poll_after_reply = True
    arm_timeout_s = 120
    results_timeout_s = 120

    def __init__(self, device_id, workspace, python="python3", *, runner=subprocess.run):
        self.device_id, self.workspace = validate_iris_args(device_id, workspace)
        self.python, self.runner = python, runner
        self.boot, self.offset = 0, 0
        self.buffer = b""
        self.command_started = None
        self.line_time_s, self.clock_uncertainty_s = None, 0.0
        self._first_received = None
        self._pending = True
        self._expected = "ready"
        self._read()

    def _rpc(self, method, payload):
        result = self.runner(
            [self.python, "mosaico.py", "iris", "rpc", SERVICE, str(method),
             "--device-id", self.device_id, "--payload-hex", payload.hex(),
             "--deadline-ms", "3000", "--json"],
            cwd=self.workspace, capture_output=True, text=True, timeout=15, check=True,
        )
        reply = json.loads(result.stdout)
        if reply.get("status") != "succeeded" or reply.get("device_id") != self.device_id:
            raise ValueError("Iris RPC returned the wrong device or failed")
        return base64.b64decode(reply["response"]["payload_base64"], validate=True)

    def _read(self):
        raw = self._rpc(2, struct.pack("<II", self.boot, self.offset))
        received = time.monotonic()
        if len(raw) < 16:
            raise ValueError("truncated PTEST stream header")
        version, boot, offset, end = struct.unpack_from("<IIII", raw)
        chunk = raw[16:]
        if (version != 1 or not boot or (self.boot and boot != self.boot)
                or (self.boot and offset != self.offset) or offset + len(chunk) > end):
            raise ValueError("PTEST stream changed boot, lost bytes, or has an invalid range")
        if chunk and self._first_received is None:
            self._first_received = received
        self.boot, self.offset = boot, offset + len(chunk)
        self.buffer += chunk
        if len(self.buffer) > 128 * 1024:
            raise ValueError("PTEST line exceeds stream capacity")
        return bool(chunk)

    @property
    def status_pending(self):
        return self._pending and self._expected == "status"

    def send(self, line, wake=True):
        del wake
        if self.status_pending and line != "ptest.abort":
            raise ValueError("previous Iris status reply is still pending")
        encoded = (">" + line).encode("ascii")
        if not encoded.startswith(b">ptest.") or len(encoded) >= 256 or any(c < 32 or c > 126 for c in encoded):
            raise ValueError("invalid PTEST command")
        self.command_started = time.monotonic()
        self._first_received = None
        self._pending = True
        self._expected = {"ptest.arm": "arm_ack", "ptest.status": "status",
                          "ptest.results": "results", "ptest.abort": "abort_ack"}.get(line.split("=", 1)[0])
        # No automatic retry: a lost acknowledgement may already have queued arm.
        self._rpc(1, struct.pack("<I", self.boot) + encoded)

    def lines(self, seconds):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            while b"\n" in self.buffer:
                line, self.buffer = self.buffer.split(b"\n", 1)
                received = self._first_received or time.monotonic()
                started = self.command_started if self.command_started is not None else received
                self.line_time_s = (started + received) / 2
                self.clock_uncertainty_s = max(0, (received - started) / 2)
                kind = json.loads(line[6:]).get("type") if line.startswith(b"PTEST ") else None
                if kind in (self._expected, "error"):
                    self._pending = False
                else:
                    self._first_received = None
                yield line.decode("ascii").strip()
            # PTEST emits one line per command. Do not poll USB throughout a
            # capture once that reply has arrived; status is requested at 10 s.
            if not self._pending or not self._read():
                time.sleep(0.1)

    def battery(self):
        raw = self._rpc(3, struct.pack("<I", self.boot))
        if len(raw) != 12:
            raise ValueError("invalid PTEST battery response")
        pct, mv, ma = struct.unpack("<iii", raw)
        return dict(battery_pct=pct, battery_mv=mv, battery_ma=ma)

    def close(self):
        pass  # The shared Gateway retains ownership of USB.
