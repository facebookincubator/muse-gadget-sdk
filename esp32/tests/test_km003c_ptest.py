# Copyright (c) Meta Platforms, Inc. and affiliates.
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy at http://www.apache.org/licenses/LICENSE-2.0
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
import io, json, struct, sys, tempfile, unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path
from unittest.mock import Mock, call, patch
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from tools.power import km003c_ptest as tool, km003c_backend as kb
class KM003CPtestTest(unittest.TestCase):
    def test_capture(self):
        for case in ("ok", "retry", "short", "sparse", "empty", "missing", "incomplete", "plan", "run"):
            with self.subTest(case=case), tempfile.TemporaryDirectory() as tmp:
                ack = dict(type="arm_ack", now_us=1000000, usb_powered=True, charging_off=True,
                           plan=[dict(id="idle", timed=True)])
                record = dict(id="idle", repeat=0, status="ok", measure_start_us=2000000,
                              end_us=2100000 if case == "short" else 6000000)
                results = dict(type="results", state="error" if case == "incomplete" else "complete",
                               run_id="other" if case == "run" else "test", records=[] if case == "plan" else [record])
                console, meter, order = Mock(), Mock(), Mock()
                console.lines.return_value = []; order.attach_mock(console, "console")
                recorder = tool.Recorder(meter, 20)
                samples = [] if case == "empty" else [(102 + i * .05, 10000, 5) for i in range(41)]
                if case == "sparse": samples = samples[:31]
                recorder.start, recorder.stop = Mock(), Mock(return_value=samples); order.attach_mock(recorder.stop, "stop")
                output = Path(tmp) / "run.jsonl"
                batches = [[(100, ack)], [(110, dict(type="status", now_us=11000000, run_id="test", state="complete"))]]
                batches += ([[]] if case == "retry" else []) + [[] if case == "missing" else [(111, results)]] * 2
                argv = ["--console", "PORT", "--output", str(output), "--run-id", "test", "--charging-off"]
                with patch.object(tool, "Recorder", return_value=recorder), redirect_stdout(io.StringIO()), \
                     redirect_stderr(io.StringIO()), patch.object(tool.time, "monotonic", return_value=100), \
                     patch.object(tool, "ptest_lines", side_effect=batches):
                    code = tool.main(argv, meter_factory=lambda _: meter, console_factory=lambda _: console)
                    saved = output.read_bytes()
                    with self.assertRaises(FileExistsError): tool.main(argv, meter_factory=lambda _: meter, console_factory=lambda _: console)
                    self.assertEqual(output.read_bytes(), saved)
                rows = [json.loads(line) for line in saved.splitlines()]
                self.assertEqual(code, 0 if case in ("ok", "retry") else 1)
                self.assertEqual(order.mock_calls[0], call.console.lines(20.0))
                arm = json.loads(console.send.call_args_list[0].args[0].split("=", 1)[1])
                self.assertEqual(arm["charging_off"], 1)
                self.assertLess(order.mock_calls.index(call.stop()), order.mock_calls.index(call.console.send("ptest.results", wake=True)))
                console.send.assert_any_call("ptest.status")
                self.assertNotIn(call("ptest.abort"), console.send.call_args_list)
                if case in ("ok", "retry"):
                    self.assertEqual((rows[1]["start"], rows[1]["end"], rows[1]["mean_uA"], rows[1]["included_s"]), (101, 105, 10000, 2))
                else:
                    self.assertEqual(rows[-1]["type"], "partial_capture")
                    self.assertEqual(rows[-1]["samples"], json.loads(json.dumps(samples)))

    def test_recorder_skips_slots_after_a_slow_read(self):
        clock, waits = [0.0], []
        meter = Mock()
        def read():
            if meter.read.call_count == 2: clock[0] += 1.0  # one read stalls for a second
            return Mock(host_time_s=clock[0], ibus_a=-0.01, vbus_v=5.0)
        meter.read.side_effect = read
        recorder = tool.Recorder(meter, 20)
        recorder.stopped = Mock()
        recorder.stopped.is_set.side_effect = lambda: meter.read.call_count >= 4
        recorder.stopped.wait.side_effect = lambda t: (waits.append(t), clock.__setitem__(0, clock[0] + t))
        with patch.object(tool.time, "monotonic", side_effect=lambda: clock[0]):
            recorder.run()
        self.assertEqual(len(waits), 4)
        self.assertTrue(all(w > 0.01 for w in waits), waits)  # no back-to-back catch-up reads
        times = [s[0] for s in recorder.samples]
        self.assertTrue(all(b - a >= 0.04 for a, b in zip(times, times[1:])), times)

    def test_offset_and_drift(self):
        records = {("idle", 0): dict(id="idle", repeat=0, measure_start_us=2000000, end_us=4000000)}
        window, = tool.windows_from(records, [(1, 100), (11, 111), (21, 122)])
        self.assertAlmostEqual(window["start"], 101.1)
        self.assertAlmostEqual(window["end"], 103.3)

def reply(request_id, vbus_uv, ibus_ua, temp_raw=4000):
    payload = struct.pack("<iiiiiih", vbus_uv, ibus_ua, 0, 0, 0, 0, temp_raw).ljust(44, b"\0")
    header = 65 | request_id << 8 | (1 + 44 // 4) << 22
    extended = 1 | 44 << 22
    return struct.pack("<II", header, extended) + payload

class PacketTest(unittest.TestCase):
    def test_packets(self):
        self.assertEqual(kb.request(5), bytes([0x0C, 0x05, 0x02, 0x00]))
        reading = kb.parse_reply(reply(7, 5_116_825, -27_013, 4023), 7)
        self.assertAlmostEqual(reading.vbus_v, 5.116825)
        self.assertAlmostEqual(reading.ibus_a, -0.027013)
        self.assertAlmostEqual(reading.temp_c, 4023 / 128)
        self.assertAlmostEqual(reading.power_w, 5.116825 * -0.027013)
        self.assertIsNone(kb.parse_reply(reply(6, 5_000_000, 1000), 7))
        self.assertIsNone(kb.parse_reply(bytes([0x03, 0x00, 0x00, 0x00]), 7))  # a Disconnect

class CollectorCleanupTest(unittest.TestCase):
    def test_abort_on_all_armed_exits_and_signal_handlers_restored(self):
        import signal
        for case in ("exception", "keyboard", "sigterm", "sighup", "send", "meter", "timeout", "ack"):
            with self.subTest(case=case), tempfile.TemporaryDirectory() as tmp:
                console, meter, order = Mock(), Mock(), Mock()
                console.lines.return_value = []
                order.attach_mock(console, "console")
                order.attach_mock(meter, "meter")
                recorder = tool.Recorder(meter, 20)
                recorder.start = Mock()
                recorder.stop = Mock(return_value=[])
                clock, calls = [0], [0]
                ack = dict(type="arm_ack", now_us=0, max_duration_ms=1000, usb_powered=True, plan=[dict(id="idle", timed=True)])
                old_handlers = {sig: signal.getsignal(sig) for sig in (signal.SIGTERM, signal.SIGHUP)}
                def messages(console, seconds):
                    calls[0] += 1
                    self.assertLess(calls[0], 5, "collector ignored a failed meter or missing marker")
                    if calls[0] == 1 and case not in ("exception", "keyboard", "sigterm", "sighup", "ack"):
                        return [(0, ack)]
                    if case == "exception": raise RuntimeError("console disconnected")
                    if case == "keyboard": raise KeyboardInterrupt()
                    if case in ("sigterm", "sighup"):
                        sig = signal.SIGTERM if case == "sigterm" else signal.SIGHUP
                        signal.getsignal(sig)(sig, None)
                    if case == "meter": recorder.error = RuntimeError("meter disconnected")
                    if case == "timeout": clock[0] = 41
                    return []
                if case == "send":
                    def send(line, **kwargs):
                        if line.startswith("ptest.arm="): raise OSError("partial write")
                    console.send.side_effect = send
                output = Path(tmp) / "run.jsonl"
                with patch.object(tool, "Recorder", return_value=recorder), \
                     patch.object(tool, "ptest_lines", side_effect=messages), \
                     patch.object(tool.time, "monotonic", side_effect=lambda: clock[0]), \
                     redirect_stdout(io.StringIO()), redirect_stderr(io.StringIO()):
                    code = tool.main(["--console", "PORT", "--output", str(output)],
                                     console_factory=lambda _: console, meter_factory=lambda _: meter)
                self.assertEqual(code, 1)
                console.send.assert_any_call("ptest.abort")
                self.assertLess(order.mock_calls.index(call.console.send("ptest.abort")),
                                order.mock_calls.index(call.console.close()))
                self.assertEqual({sig: signal.getsignal(sig) for sig in old_handlers}, old_handlers)
                saved = json.loads(output.read_text().splitlines()[0])
                if case == "meter": self.assertIn("meter disconnected", saved["results_error"])
                if case == "timeout": self.assertIn("duration bound", saved["results_error"])

    def test_arm_error_names_firmware_reason(self):
        with tempfile.TemporaryDirectory() as tmp:
            console, meter = Mock(), Mock()
            console.lines.return_value = []
            recorder = tool.Recorder(meter, 20)
            recorder.start, recorder.stop = Mock(), Mock(return_value=[])
            error = dict(type="error", error=259, message="already_active")
            output = Path(tmp) / "run.jsonl"
            with patch.object(tool, "Recorder", return_value=recorder), \
                 patch.object(tool, "ptest_lines", side_effect=[[(0, error)]]), \
                 redirect_stdout(io.StringIO()), redirect_stderr(io.StringIO()):
                code = tool.main(["--console", "PORT", "--output", str(output)],
                                 console_factory=lambda _: console, meter_factory=lambda _: meter)
            self.assertEqual(code, 1)
            saved = json.loads(output.read_text().splitlines()[0])
            self.assertIn("did not arm: already_active (259)", saved["results_error"])
            self.assertNotIn(call("ptest.abort"), console.send.call_args_list)  # Another host's run survives.

    def test_missing_meter_does_not_create_output(self):
        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp) / "run.jsonl"
            with self.assertRaises(kb.MeterError):
                tool.main(["--console", "PORT", "--output", str(output)],
                          meter_factory=Mock(side_effect=kb.MeterError("missing meter")))
            self.assertFalse(output.exists())

    def test_sample_coverage_threshold(self):
        window = dict(start=0, end=12)
        samples = [(1 + i / 20, 10000, 5) for i in range(160)]
        summary = tool.summarise(samples, window, 1, 20)
        self.assertEqual((summary["expected_sample_count_estimate"], summary["coverage_ratio_estimate"],
                          summary["missing_sample_count_estimate"]), (200, 0.8, 40))
        self.assertIsNone(tool.summarise(samples[:-1], window, 1, 20))

class KernelDriverTest(unittest.TestCase):
    def test_detach_active_driver_before_claim(self):
        class USBError(Exception): pass
        for active, failure in ((True, None), (False, None), (True, NotImplementedError), (True, USBError)):
            with self.subTest(active=active, failure=failure):
                core, util, device, order = Mock(), Mock(), Mock(), Mock()
                core.USBError = USBError
                core.find.return_value = device
                device.is_kernel_driver_active.return_value = active
                device.detach_kernel_driver.side_effect = failure
                order.attach_mock(device, "device")
                order.attach_mock(util, "util")
                backend = kb.KM003CBackend(usb_core=core, usb_util=util)
                device.is_kernel_driver_active.assert_called_once_with(0)
                if active:
                    device.detach_kernel_driver.assert_called_once_with(0)
                    self.assertLess(order.mock_calls.index(call.device.detach_kernel_driver(0)),
                                    order.mock_calls.index(call.util.claim_interface(device, 0)))
                else: device.detach_kernel_driver.assert_not_called()
                backend.close()
                if active and failure is None:  # Give the powerz hwmon driver its interface back.
                    device.attach_kernel_driver.assert_called_once_with(0)
                    self.assertLess(order.mock_calls.index(call.util.release_interface(device, 0)),
                                    order.mock_calls.index(call.device.attach_kernel_driver(0)))
                else: device.attach_kernel_driver.assert_not_called()

    def test_reattach_when_claim_fails(self):
        core, util, device = Mock(), Mock(), Mock()
        core.USBError = type("USBError", (Exception,), {})
        core.find.return_value = device
        device.is_kernel_driver_active.return_value = True
        util.claim_interface.side_effect = core.USBError("busy")
        with self.assertRaises(core.USBError):
            kb.KM003CBackend(usb_core=core, usb_util=util)
        device.attach_kernel_driver.assert_called_once_with(0)
