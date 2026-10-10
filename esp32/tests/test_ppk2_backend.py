# Copyright (c) Meta Platforms, Inc. and affiliates.
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy at http://www.apache.org/licenses/LICENSE-2.0
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""Adapter and CLI failure/lifecycle tests, strictly fake serial only."""

import json
import os
import selectors
import subprocess
import sys
import tempfile
import types
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from tools.power import ppk2_backend as backend_module
from tools.power.ppk2_backend import DeviceLease, PPK2Backend
from tools.power.ppk2_control import send_command


class AdapterFailureTest(unittest.TestCase):
    def setUp(self):
        self.serial = mock.Mock()
        self.serial.write.side_effect = lambda payload: len(payload)
        self.serial.in_waiting = 1000000
        self.serial.read.return_value = b""
        serial = self.serial

        class FakeAPI:
            def __init__(self, port, **kwargs):
                self.port = port
                self.kwargs = kwargs
                self.ser = serial
                self.modifiers = {"Calibrated": "1", "HW": "fake"}
            def _pack_struct(self, cmd):
                return bytes(cmd)
            def stop_measuring(self):
                self._write_serial((7,))
            def get_modifiers(self):
                return True
            def use_source_meter(self):
                self._write_serial((17, 2))
            def set_source_voltage(self, voltage):
                self._write_serial((13, 1, 1))
            def toggle_DUT_power(self, state):
                self._write_serial((12, 1 if state == "ON" else 0))
        module = types.ModuleType("ppk2_api.ppk2_api")
        module.PPK2_API = FakeAPI
        self.patches = [
            mock.patch.dict(sys.modules, {"ppk2_api": types.ModuleType("ppk2_api"), "ppk2_api.ppk2_api": module}),
            mock.patch.object(backend_module.importlib.metadata, "version", side_effect=lambda package: "0.9.2" if package == "ppk2-api" else "3.5"),
            mock.patch.object(backend_module, "port_descriptions", return_value=[{"port": "fake-a", "serial_number": "fake"}, {"port": "fake-b", "serial_number": "fake"}]),
            mock.patch.object(backend_module, "DeviceLease"),
        ]
        for patch in self.patches:
            patch.start()
            self.addCleanup(patch.stop)

    def test_only_explicit_port_opened_once_with_exclusive(self):
        backend = PPK2Backend("fake-b")
        self.addCleanup(backend.close)
        self.assertEqual(backend.api.port, "fake-b")
        self.assertEqual(backend.api.kwargs, {"timeout": 0, "write_timeout": 0.5, "exclusive": True})
        self.assertEqual(backend.descriptor["port"], "fake-b")
        backend_module.DeviceLease.assert_called_once()

    def test_wrong_port_no_lease_or_open(self):
        with self.assertRaises(ValueError):
            PPK2Backend("unselected")
        backend_module.DeviceLease.assert_not_called()
        self.serial.write.assert_not_called()

    def test_version_mismatch_fails_before_lease(self):
        with mock.patch.object(backend_module.importlib.metadata, "version", return_value="0.0.8"):
            with self.assertRaises(RuntimeError):
                PPK2Backend("fake-a")
        backend_module.DeviceLease.assert_not_called()

    def test_short_command_write_is_not_swallowed(self):
        backend = PPK2Backend("fake-a")
        self.addCleanup(backend.close)
        self.serial.write.side_effect = lambda _: 0
        with self.assertRaises(OSError):
            backend.power(True)
        with self.assertRaises(OSError):
            backend.stop_measuring()

    def test_nonblocking_read_allows_a_short_current_snapshot(self):
        backend = PPK2Backend("fake-a")
        self.addCleanup(backend.close)
        self.assertEqual(backend.api.kwargs["timeout"], 0)
        self.serial.in_waiting = 1020
        self.serial.read.return_value = b"abcd"
        self.assertEqual(backend.read(), b"abcd")
        self.serial.read.assert_called_once_with(1020)
        self.assertEqual(backend.last_backlog_bytes, 1020)
        self.serial.write.assert_not_called()

    def test_metadata_accumulates_split_cdc_chunks(self):
        backend = PPK2Backend("fake-a")
        self.addCleanup(backend.close)
        chunks = [b"Calibrated: 1\n", b"HW: fake\n", b"IA: 0.0\nEND"]
        self.serial.read.side_effect = chunks
        with mock.patch.object(backend_module.time, "sleep"):
            self.assertEqual(backend.api._read_metadata(), b"".join(chunks).decode())
        self.assertEqual(self.serial.read.call_count, 3)
        self.serial.write.assert_not_called()
        for call in self.serial.read.call_args_list:
            self.assertLessEqual(call.args[0], backend_module.MAX_METADATA_BYTES + 1)

    def test_metadata_end_substring_is_not_a_terminator(self):
        backend = PPK2Backend("fake-a")
        self.addCleanup(backend.close)
        self.serial.read.side_effect = [b"HW: FRIEND\n", b"Calibrated: 1\nEND"]
        with mock.patch.object(backend_module.time, "sleep"):
            result = backend.api._read_metadata()
        self.assertIn("Calibrated: 1", result)
        self.assertEqual(self.serial.read.call_count, 2)

    def test_metadata_missing_terminator_times_out_without_commands(self):
        backend = PPK2Backend("fake-a")
        self.addCleanup(backend.close)
        with mock.patch.object(backend_module.time, "monotonic", side_effect=[0.0, 0.01, 3.0]), mock.patch.object(backend_module.time, "sleep"):
            with self.assertRaisesRegex(TimeoutError, "complete metadata terminator"):
                backend.api._read_metadata()
        self.serial.write.assert_not_called()

    def test_metadata_response_is_bounded_and_strict_utf8(self):
        backend = PPK2Backend("fake-a")
        self.addCleanup(backend.close)
        self.serial.read.return_value = b"x" * (backend_module.MAX_METADATA_BYTES + 1)
        with self.assertRaisesRegex(ValueError, "bounded response"):
            backend.api._read_metadata()
        self.serial.read.return_value = b"\xff\nEND"
        with self.assertRaises(UnicodeDecodeError):
            backend.api._read_metadata()
        self.serial.write.assert_not_called()

    def test_metadata_failure_prevents_mode_and_power(self):
        backend = PPK2Backend("fake-a")
        self.addCleanup(backend.close)
        for invalid in (False, None):
            backend.api.get_modifiers = lambda: invalid
            with self.assertRaises(RuntimeError):
                backend.configure(3872)
        self.serial.write.assert_not_called()

    def test_incomplete_metadata_prevents_source_configuration(self):
        backend = PPK2Backend("fake-a")
        self.addCleanup(backend.close)
        backend.api.modifiers["HW"] = None
        with self.assertRaises(RuntimeError):
            backend.configure(3872)
        self.serial.write.assert_not_called()

    def test_configuration_error_explicitly_attempts_off(self):
        backend = PPK2Backend("fake-a")
        self.addCleanup(backend.close)
        backend.api.set_source_voltage = mock.Mock(side_effect=OSError("failed voltage"))
        with self.assertRaises(OSError):
            backend.configure(3872)
        self.assertEqual(self.serial.write.call_args_list[-1], mock.call(b"\x0c\x00"))

    def test_read_has_bounded_allocation_and_reports_backlog(self):
        backend = PPK2Backend("fake-a")
        self.addCleanup(backend.close)
        backend.read()
        self.serial.read.assert_called_once_with(65536)
        self.assertEqual(backend.last_backlog_bytes, 1000000)

    def test_lease_groups_both_interfaces(self):
        # Actual host flock on a new TEMPORARY regular file, never a device.
        with tempfile.TemporaryDirectory() as directory, mock.patch.object(backend_module.tempfile, "gettempdir", return_value=directory):
            lease = DeviceLease({"port": "fake-a", "serial_number": "synthetic-id"})
            try:
                with self.assertRaises(RuntimeError):
                    DeviceLease({"port": "fake-b", "serial_number": "synthetic-id"})
            finally:
                lease.close()
            other = DeviceLease({"port": "fake-b", "serial_number": "synthetic-id"})
            other.close()


class FakeCLIProcessTest(unittest.TestCase):
    def test_sigint_keeps_same_fake_owner_until_confirmed_finish(self):
        # The process replaces PPK2Session's backend with a pure FakeBackend
        # BEFORE run_owner. No discover(), serial import, or hardware path.
        esp32 = Path(__file__).resolve().parents[1]
        code = '''
import json, sys
from pathlib import Path
sys.path.insert(0, sys.argv[1])
sys.path.insert(0, str(Path(sys.argv[1]) / "tests"))
from test_ppk2_profile import FakeBackend
from tools.power import ppk2_profile as cli
from tools.power.ppk2_session import PPK2Session
class LoggingBackend(FakeBackend):
    def start_measuring(self):
        super().start_measuring()
        self.chunks.append([42])  # fake finite sample evidence before READY
    def power(self, on):
        super().power(on)
        with open(sys.argv[2], "a") as log:
            log.write(json.dumps(["power", on]) + "\\n")
    def close(self):
        super().close()
        with open(sys.argv[2], "a") as log:
            log.write(json.dumps(["close"]) + "\\n")
cli.PPK2Session = lambda **kwargs: PPK2Session(**kwargs, backend_factory=LoggingBackend)
raise SystemExit(cli.main(sys.argv[3:]))
'''
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            events = directory / "events.jsonl"
            control = directory / "control"
            argv = [sys.executable, "-c", code, str(esp32), str(events),
                    "record", "--port", "fake-only", "--voltage-mv", "3872",
                    "--output", str(directory / "out.jsonl"), "--control-dir", str(control),
                    "--ack-source-wiring", "--ack-battery-isolated", "--ack-no-charging-backfeed",
                    "--ack-usb-connected-before-start"]
            env = dict(os.environ, PYTHONDONTWRITEBYTECODE="1")
            process = subprocess.Popen(argv, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, env=env)
            try:
                with selectors.DefaultSelector() as selector:
                    selector.register(process.stderr, selectors.EVENT_READ)
                    self.assertTrue(selector.select(5), "fake owner never became READY")
                ready = json.loads(process.stderr.readline())
                self.assertTrue(ready["ready"])
                self.assertGreater(ready["finite_sample_count"], 0)
                self.assertIsNotNone(ready["first_finite_sample_host_offset_s"])
                send_command(control, {"op": "begin", "label": "idle"})
                # Drain the complete three-line startup note before signal.
                for _ in range(3):
                    process.stderr.readline()
                process.send_signal(__import__("signal").SIGINT)
                with selectors.DefaultSelector() as selector:
                    selector.register(process.stderr, selectors.EVENT_READ)
                    self.assertTrue(selector.select(5), "fake owner did not handle SIGINT")
                line = process.stderr.readline().decode()
                self.assertIn("owner and drain retained", line)
                status = send_command(control, {"op": "status"})
                self.assertTrue(status["holding"])
                self.assertIsNone(status["active_label"])
                with self.assertRaises(RuntimeError):
                    send_command(control, {"op": "finish", "usb_reconnected": False})
                self.assertEqual(events.read_text().splitlines(), ['["power", true]'])
                send_command(control, {"op": "finish", "usb_reconnected": True})
                self.assertEqual(process.wait(timeout=5), 0)
                self.assertEqual(events.read_text().splitlines(), ['["power", true]', '["power", false]', '["close"]'])
            finally:
                if process.poll() is None:
                    # Fake-only test cleanup: never applicable to a real owner.
                    process.kill()
                    process.wait(timeout=5)
                process.stderr.close()


if __name__ == "__main__":
    unittest.main()
