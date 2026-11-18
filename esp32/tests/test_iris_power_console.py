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

"""Exercise the Iris adapter without opening a Gateway, serial port or meter."""
import base64
from contextlib import redirect_stdout, redirect_stderr
import io
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import Mock, patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from tools.power.iris_console import IrisConsole
from tools.power import km003c_ptest as tool

DEVICE = "4553502d4952495301001c2904d0ad4a"


class IrisConsoleTest(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        Path(tmp.name, "mosaico.py").touch()
        reply = dict(status="succeeded", device_id=DEVICE, response=dict(
            payload_base64=base64.b64encode(struct.pack("<IIII", 1, 123, 0, 0)).decode()))
        self.console = IrisConsole(DEVICE, tmp.name, runner=Mock(return_value=Mock(stdout=json.dumps(reply))))

    def test_stream_rejects_reboot_loss_and_bad_headers(self):
        for raw in (b"bad", struct.pack("<IIII", 1, 124, 0, 0),
                    struct.pack("<IIII", 1, 123, 1, 1), struct.pack("<IIII", 9, 123, 0, 0),
                    struct.pack("<IIII", 1, 123, 0, 0) + b"x"):
            with self.subTest(raw=raw), patch.object(self.console, "_rpc", return_value=raw):
                with self.assertRaises(ValueError):
                    self.console._read()

    def test_boot_ready_does_not_hide_command_reply(self):
        self.console.buffer = b'PTEST {"type":"ready"}\n'
        status = b'PTEST {"type":"status"}\n'
        with patch.object(self.console, "_rpc", side_effect=[b"", struct.pack("<IIII", 1, 123, 0, len(status)) + status]):
            self.console.send("ptest.status")
            lines = self.console.lines(.01)
            self.assertEqual(json.loads(next(lines)[6:])["type"], "ready")
            self.assertEqual(json.loads(next(lines)[6:])["type"], "status")

    def test_commands_bounded_and_never_replayed(self):
        for line in ("restart", "ptest.status\n>ptest.arm={}", "ptest." + "x" * 256):
            with self.assertRaises(ValueError):
                self.console.send(line)
        with patch.object(self.console, "_rpc", side_effect=subprocess.TimeoutExpired("rpc", 15)) as rpc:
            with self.assertRaises(subprocess.TimeoutExpired):
                self.console.send("ptest.status")
            rpc.assert_called_once()

    def test_rpc_rejects_wrong_device_and_failure(self):
        for device, status in (("wrong", "succeeded"), (DEVICE, "failed")):
            self.console.runner = Mock(return_value=Mock(stdout=json.dumps(dict(
                device_id=device, status=status, response=dict(payload_base64="")))))
            with self.assertRaisesRegex(ValueError, "wrong device or failed"):
                self.console._rpc(3, struct.pack("<I", self.console.boot))


class IrisFirmwareTransportTest(unittest.TestCase):
    def test_real_transport_ring_and_commands(self):
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            for name in ("esp_heap_caps.h", "esp_iris.h", "esp_pm.h", "esp_random.h",
                         "freertos/FreeRTOS.h", "freertos/semphr.h"):
                header = directory / name
                header.parent.mkdir(parents=True, exist_ok=True)
                header.touch()
            binary = directory / "transport"
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
                            "-I", tmp, str(ROOT / "tests/iris_power_test_harness.c"), "-o", str(binary)], check=True)
            result = subprocess.run([str(binary)], check=True, capture_output=True, text=True, timeout=5)
            self.assertEqual(result.stdout, "ok\n")


class IrisCollectorTest(unittest.TestCase):
    def test_preflight_and_workspace_precedence(self):
        with tempfile.TemporaryDirectory() as tmp:
            Path(tmp, "mosaico.py").touch()
            missing = str(Path(tmp, "missing"))
            for device, env, option, valid in (("bad", tmp, [], False), ("", tmp, [], False),
                    (DEVICE, missing, [], False), (DEVICE, tmp, [], True),
                    (DEVICE, missing, ["--mosaico-workspace", tmp], True),
                    (DEVICE, tmp, ["--mosaico-workspace", missing], False)):
                output = Path(tmp, "capture.jsonl")
                meter = Mock(side_effect=RuntimeError("validated before meter"))
                console = Mock()
                with self.subTest(device=device, env=env, option=option), \
                     patch.dict(tool.os.environ, MOSAICO_WORKSPACE=env), redirect_stderr(io.StringIO()):
                    with self.assertRaises(RuntimeError if valid else SystemExit):
                        tool.main(["--iris-device-id", device, "--output", str(output), *option],
                                  meter_factory=meter, console_factory=console)
                    self.assertEqual(meter.call_count, int(valid))
                    console.assert_not_called()
                    self.assertFalse(output.exists())

    def test_battery_clock_and_arm_rejection(self):
        for pct, ma, error_ack, reason, sends in ((99, 0, False, "battery must be full", 0),
                (100, 20, False, "battery must be full", 0),
                (100, -20, False, "battery must be full", 0),
                (100, 0, False, "clock uncertainty", 2), (100, 0, True, "already_active", 1)):
            with self.subTest(reason=reason, pct=pct, ma=ma), tempfile.TemporaryDirectory() as tmp:
                Path(tmp, "mosaico.py").touch()
                output = Path(tmp, "capture.jsonl")
                console = Mock(spec=IrisConsole, clock_uncertainty_s=10.0, arm_timeout_s=IrisConsole.arm_timeout_s)
                console.lines.return_value = []
                console.battery.return_value = dict(battery_pct=pct, battery_mv=4200, battery_ma=ma)
                recorder = Mock(error=None)
                recorder.stop.return_value = []
                ack = dict(type="error", message="already_active", error=259) if error_ack else dict(
                    type="arm_ack", now_us=1000000, usb_powered=True, plan=[dict(id="idle", timed=True)])
                with patch.object(tool, "Recorder", return_value=recorder), \
                     patch.object(tool, "ptest_lines", side_effect=[[(1.0, ack)], []]) as lines, \
                     redirect_stdout(io.StringIO()), redirect_stderr(io.StringIO()):
                    code = tool.main(["--iris-device-id", DEVICE, "--mosaico-workspace", tmp, "--output", str(output)],
                                     meter_factory=lambda _: Mock(), console_factory=lambda _: console)
                self.assertEqual(code, 1)
                self.assertIn(reason, json.loads(output.read_text().splitlines()[0])["results_error"])
                self.assertEqual(console.send.call_count, sends)
                if sends:
                    self.assertEqual(lines.call_args_list[0].args[1], 120)
                else:
                    recorder.start.assert_not_called()
                if sends == 2:
                    console.send.assert_called_with("ptest.abort")

    def test_collector_waits_for_fragmented_status_before_next_poll(self):
        clock, output, status_calls, read_offset = [100.0], bytearray(), [], [0]
        def rpc(argv, **kwargs):
            self.assertEqual(argv[argv.index("--device-id") + 1], DEVICE)
            self.assertEqual(kwargs["cwd"], Path(tmp).resolve())
            clock[0] += 4
            method = int(argv[5])
            payload = bytes.fromhex(argv[argv.index("--payload-hex") + 1])
            if method == 1:
                command = payload[4:].decode()
                if command.startswith(">ptest.arm="):
                    msg = dict(type="arm_ack", now_us=int(clock[0]*1e6), usb_powered=True,
                               max_duration_ms=300000, plan=[dict(id="idle", timed=True)], padding="x"*7000)
                elif command == ">ptest.status":
                    self.assertEqual(read_offset[0], len(output), "a status reply was still unread")
                    status_calls.append(clock[0])
                    msg = dict(type="status", now_us=int(clock[0]*1e6), run_id="test",
                               state="running" if len(status_calls) == 1 else "complete", padding="x"*3500)
                elif command == ">ptest.results":
                    msg = dict(type="results", run_id="test", state="complete", records=[dict(
                        id="idle", repeat=0, status="ok", measure_start_us=120000000, end_us=150000000)])
                else:
                    self.fail(command)
                output.extend(b"PTEST " + json.dumps(msg).encode() + b"\n")
                raw = b""
            elif method == 2:
                boot, offset = struct.unpack("<II", payload)
                if not boot: offset = len(output)
                chunk = output[offset:offset+1008]
                read_offset[0] = offset + len(chunk)
                raw = struct.pack("<IIII", 1, 123, offset, len(output)) + chunk
            else:
                raw = struct.pack("<iii", 100, 4200, -1)
            return Mock(stdout=json.dumps(dict(status="succeeded", device_id=DEVICE,
                        response=dict(payload_base64=base64.b64encode(raw).decode()))))
        recorder = Mock(error=None)
        recorder.stop.return_value = [(i*.05, 10000, 5) for i in range(5000)]
        with tempfile.TemporaryDirectory() as tmp:
            Path(tmp, "mosaico.py").touch()
            path = Path(tmp, "capture.jsonl")
            with patch("tools.power.iris_console.time.monotonic", side_effect=lambda: clock[0]), \
                 patch("tools.power.iris_console.time.sleep", side_effect=lambda s: clock.__setitem__(0, clock[0]+s)), \
                 patch.object(tool, "Recorder", return_value=recorder), \
                 redirect_stdout(io.StringIO()), redirect_stderr(io.StringIO()):
                code = tool.main(["--iris-device-id", DEVICE, "--mosaico-workspace", tmp, "--output", str(path), "--run-id", "test",
                                  "--boot-wait-s", "0", "--capture-ms", "30000", "--guard-s", "5"],
                                 meter_factory=lambda _: Mock(),
                                 console_factory=lambda _: IrisConsole(DEVICE, tmp, runner=rpc))
            header, window = [json.loads(line) for line in path.read_text().splitlines()]
        self.assertEqual(code, 0, header["errors"])
        self.assertEqual(header["battery_before"]["battery_ma"], -1)
        self.assertEqual(len(status_calls), 2)
        self.assertGreaterEqual(status_calls[1]-status_calls[0], 30)
        self.assertEqual(header["clock_uncertainty_s"], 4)
        self.assertAlmostEqual(window["start"], 120, delta=1e-6)
        self.assertAlmostEqual(window["end"], 150, delta=1e-6)
