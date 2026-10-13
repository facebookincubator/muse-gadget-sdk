# Copyright (c) Meta Platforms, Inc. and affiliates.
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy at http://www.apache.org/licenses/LICENSE-2.0
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""Startup cleanup diagnostics and retained-control tests; no hardware."""

import json
import os
import selectors
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from tools.power import ppk2_profile as cli
from tools.power.ppk2_control import send_command
from tools.power.ppk2_session import PPK2Session
from test_ppk2_profile import ACK, Clock
import test_ppk2_backend as backend_tests


class StartupCleanupTest(unittest.TestCase):
    def test_configure_failed_emergency_off_is_preserved_and_retried(self):
        fixture = backend_tests.AdapterFailureTest()
        fixture.setUp()
        try:
            backend = __import__("tools.power.ppk2_backend", fromlist=["PPK2Backend"]).PPK2Backend("fake-a")
            backend.api.set_source_voltage = mock.Mock(side_effect=OSError("voltage write failed"))
            def write(payload):
                if payload == b"\x0c\x00":
                    raise OSError("OFF failed")
                return len(payload)
            fixture.serial.write.side_effect = write
            fixture.serial.close.side_effect = OSError("close failed")
            clock = Clock()
            session = PPK2Session(port="fake-a", voltage_mv=3872, acknowledgment=ACK,
                                  backend_factory=lambda _: backend, clock=clock, sleep=clock.advance)
            with self.assertRaisesRegex(OSError, "voltage write failed"):
                session.start(background=False)
            self.assertTrue(session.closed)
            self.assertEqual(backend.cleanup_errors[0]["operation"], "configure_emergency_power_off")
            self.assertIn("UNKNOWN", backend.cleanup_errors[0]["output_state"])
            self.assertIn(backend.cleanup_errors[0], session.cleanup_errors)
            self.assertTrue(any(isinstance(e, str) and "power_off" in e for e in session.cleanup_errors))
            self.assertTrue(any(isinstance(e, str) and "close:" in e for e in session.cleanup_errors))
            calls = fixture.serial.write.call_args_list
            self.assertEqual(sum(call == mock.call(b"\x0c\x00") for call in calls), 2)
        finally:
            fixture.doCleanups()

    def test_cli_warns_unknown_before_control_teardown(self):
        with tempfile.TemporaryDirectory() as directory:
            session = mock.Mock()
            session.start.side_effect = RuntimeError("original startup failure")
            session.started = True
            session.closed = True
            session.cleanup_errors = ["power_off: OFF failed", "close: close failed"]
            control = Path(directory) / "control"
            argv = ["record", "--port", "fake", "--voltage-mv", "3872",
                    "--output", str(Path(directory) / "out.jsonl"), "--control-dir", str(control),
                    "--ack-source-wiring", "--ack-battery-isolated", "--ack-no-charging-backfeed",
                    "--ack-usb-connected-before-start"]
            observations = []
            original_close = cli.ControlServer.close
            def close(server):
                observations.append("teardown")
                original_close(server)
            def output(value, **kwargs):
                observations.append(value)
            with mock.patch.object(cli, "PPK2Session", return_value=session), mock.patch.object(cli, "_print", side_effect=output), mock.patch.object(cli.ControlServer, "close", close):
                self.assertEqual(cli.main(argv), 1)
            warning_index = next(i for i, value in enumerate(observations) if isinstance(value, dict) and "warning" in value)
            self.assertIn("SOURCE STATE UNKNOWN; OFF NOT VERIFIED", observations[warning_index]["warning"])
            self.assertEqual(observations[warning_index]["cleanup_errors"], session.cleanup_errors)
            self.assertLess(warning_index, observations.index("teardown"))

    def test_ready_write_failure_and_stuck_consumer_never_publish_ready(self):
        esp32 = Path(__file__).resolve().parents[1]
        code = '''
import json, os, sys
from pathlib import Path
sys.path.insert(0, sys.argv[1])
sys.path.insert(0, str(Path(sys.argv[1]) / "tests"))
from test_ppk2_profile import FakeBackend
from tools.power import ppk2_profile as cli
from tools.power.ppk2_session import PPK2Session
from tools.power.ppk2_stats import JSONLWriter
class ReadyFailureWriter(JSONLWriter):
    def write(self, record):
        if record["type"] == "ready":
            raise OSError("fake READY disk failure")
        super().write(record)
class PowerFakeBackend(FakeBackend):
    def __init__(self, port):
        super().__init__(port)
        self.ready_chunks = [[123]]
    def power(self, on):
        super().power(on)
        with open(sys.argv[3], "a") as log:
            log.write(json.dumps(["power", on]) + "\\n")
    def close(self):
        super().close()
        with open(sys.argv[3], "a") as log:
            log.write(json.dumps(["close"]) + "\\n")
class StuckReadyConsumer(PPK2Session):
    def _run(self):
        self.pump()  # Finite sample successfully opens the READY gate.
        # Reader then blocks despite the startup stop request: HOST FIFO only.
        fd = os.open(sys.argv[2], os.O_RDONLY)
        try:
            os.read(fd, 1)
        finally:
            os.close(fd)
cli.JSONLWriter = ReadyFailureWriter
cli.PPK2Session = lambda **kwargs: StuckReadyConsumer(**kwargs, backend_factory=PowerFakeBackend)
raise SystemExit(cli.main(sys.argv[4:]))
'''
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            release = directory / "ready-release.fifo"
            os.mkfifo(release, 0o600)
            events, control = directory / "events.jsonl", directory / "control"
            argv = [sys.executable, "-c", code, str(esp32), str(release), str(events),
                    "record", "--port", "fake-only", "--voltage-mv", "3872",
                    "--output", str(directory / "out.jsonl"), "--control-dir", str(control),
                    "--ack-source-wiring", "--ack-battery-isolated", "--ack-no-charging-backfeed",
                    "--ack-usb-connected-before-start"]
            process = subprocess.Popen(argv, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
                                       env=dict(os.environ, PYTHONDONTWRITEBYTECODE="1"))
            try:
                with selectors.DefaultSelector() as selector:
                    selector.register(process.stderr, selectors.EVENT_READ)
                    self.assertTrue(selector.select(8), "retained-owner notice did not arrive")
                first_line = process.stderr.readline()
                notice = json.loads(first_line)
                self.assertFalse(notice["ready"])
                self.assertTrue(notice["holding"])
                status = send_command(control, {"op": "status"}, timeout_s=1)
                self.assertTrue(status["holding"])
                self.assertFalse(status["transport_ready"])
                self.assertEqual(status["total_valid_samples"], 1)
                self.assertIn("UNKNOWN", status["fault"])
                self.assertEqual(events.read_text().splitlines(), ['["power", true]'])
                with self.assertRaises(RuntimeError):
                    send_command(control, {"op": "finish", "usb_reconnected": False}, timeout_s=1)
                fd = os.open(release, os.O_WRONLY | os.O_NONBLOCK)
                try:
                    os.write(fd, b"!")
                finally:
                    os.close(fd)
                result = send_command(control, {"op": "finish", "usb_reconnected": True}, timeout_s=5)
                self.assertFalse(result["holding"])
                self.assertEqual(process.wait(timeout=5), 1)
                console = (first_line + process.stderr.read()).decode()
                self.assertNotIn('"ready": true', console)
                self.assertNotIn("Only now remove DUT USB", console)
                self.assertEqual(events.read_text().splitlines(), ['["power", true]', '["power", false]', '["close"]'])
            finally:
                if process.poll() is None:
                    process.kill()  # fake-only subprocess, never a real owner
                    process.wait(timeout=5)
                process.stderr.close()

    def test_startup_join_timeout_keeps_fifo_usable(self):
        esp32 = Path(__file__).resolve().parents[1]
        code = '''
import json, os, sys
from pathlib import Path
sys.path.insert(0, sys.argv[1])
sys.path.insert(0, str(Path(sys.argv[1]) / "tests"))
from test_ppk2_profile import FakeBackend
from tools.power import ppk2_profile as cli
from tools.power.ppk2_session import PPK2Session
class BlockingFakeBackend(FakeBackend):
    enabled = False
    def start_measuring(self):
        super().start_measuring()
        self.enabled = True
    def read(self):
        if self.enabled:
            # A test-only HOST FIFO simulates a stuck driver without polling.
            fd = os.open(sys.argv[2], os.O_RDONLY)
            try:
                os.read(fd, 1)
            finally:
                os.close(fd)
            self.enabled = False
        return []
    def power(self, on):
        super().power(on)
        with open(sys.argv[3], "a") as log:
            log.write(json.dumps(["power", on]) + "\\n")
    def close(self):
        super().close()
        with open(sys.argv[3], "a") as log:
            log.write(json.dumps(["close"]) + "\\n")
cli.PPK2Session = lambda **kwargs: PPK2Session(**kwargs, backend_factory=BlockingFakeBackend, first_sample_timeout_s=0.02)
raise SystemExit(cli.main(sys.argv[4:]))
'''
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            release = directory / "release.fifo"
            os.mkfifo(release, 0o600)
            events = directory / "events.jsonl"
            control = directory / "control"
            argv = [sys.executable, "-c", code, str(esp32), str(release), str(events),
                    "record", "--port", "fake-only", "--voltage-mv", "3872",
                    "--output", str(directory / "out.jsonl"), "--control-dir", str(control),
                    "--ack-source-wiring", "--ack-battery-isolated", "--ack-no-charging-backfeed",
                    "--ack-usb-connected-before-start"]
            process = subprocess.Popen(argv, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
                                       env=dict(os.environ, PYTHONDONTWRITEBYTECODE="1"))
            try:
                with selectors.DefaultSelector() as selector:
                    selector.register(process.stderr, selectors.EVENT_READ)
                    self.assertTrue(selector.select(8), "retained-owner notice did not arrive")
                notice = json.loads(process.stderr.readline())
                self.assertFalse(notice["ready"])
                self.assertTrue(notice["holding"])
                self.assertIn("UNKNOWN", notice["warning"])
                status = send_command(control, {"op": "status"}, timeout_s=1)
                self.assertTrue(status["holding"])
                self.assertFalse(status["transport_ready"])
                self.assertIn("startup reader did not stop", status["fault"])
                self.assertEqual(events.read_text().splitlines(), ['["power", true]'])
                with self.assertRaises(RuntimeError):
                    send_command(control, {"op": "finish", "usb_reconnected": False}, timeout_s=1)
                # Release ONLY the fake stuck host reader, then confirm finish.
                fd = os.open(release, os.O_WRONLY | os.O_NONBLOCK)
                try:
                    os.write(fd, b"!")
                finally:
                    os.close(fd)
                result = send_command(control, {"op": "finish", "usb_reconnected": True}, timeout_s=5)
                self.assertFalse(result["holding"])
                self.assertEqual(process.wait(timeout=5), 1)  # startup fault remains an invalid capture
                self.assertEqual(events.read_text().splitlines(), ['["power", true]', '["power", false]', '["close"]'])
                self.assertFalse((control / "commands.fifo").exists())
            finally:
                if process.poll() is None:
                    # This is a fake-only test process, never a real PPK owner.
                    process.kill()
                    process.wait(timeout=5)
                process.stderr.close()


if __name__ == "__main__":
    unittest.main()
