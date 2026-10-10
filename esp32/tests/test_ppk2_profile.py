# Copyright (c) Meta Platforms, Inc. and affiliates.
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy at http://www.apache.org/licenses/LICENSE-2.0
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""No serial/Bluetooth/hardware access; all ownership tests use fakes."""

import json
import math
import struct
import sys
import tempfile
import threading
import time
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from tools.power.ppk2_backend import PPK2Backend, port_descriptions, validate_voltage
from tools.power.ppk2_control import ControlServer, dispatch, send_command
from tools.power.ppk2_profile import build_parser, interactive_command
from tools.power.ppk2_session import PPK2Session, SafetyAcknowledgment, USBReconnectRequired
from tools.power.ppk2_stats import BudgetExceeded, CurrentStats, JSONLWriter, RawWriter


ACK = SafetyAcknowledgment(True, True, True, True)


class Clock:
    def __init__(self):
        self.value = 1000.0

    def __call__(self):
        return self.value

    def advance(self, seconds):
        self.value += seconds


class MemoryWriter:
    def __init__(self):
        self.records = []
        self.closed = False

    def write(self, record):
        self.records.append(record)

    def close(self):
        self.closed = True


class FakeBackend:
    def __init__(self, port):
        self.events = [("open", port)]
        self.chunks = []
        self.ready_chunks = []
        self.failure_after_start = None
        self.stale = False
        self.failure = None
        self.last_backlog_bytes = 0
        self.last_frame_count = 0

    def stop_measuring(self):
        self.events.append("stop")
        if self.failure == "stop":
            raise OSError("stop failed")

    def read(self):
        if self.failure == "read":
            raise OSError("read failed")
        if self.stale:
            return [10]
        chunk = self.chunks.pop(0) if self.chunks else []
        self.last_backlog_bytes = len(chunk) * 4
        return chunk

    def configure(self, voltage):
        self.events.append(("configure", voltage))
        if self.failure == "configure":
            raise ValueError("wrong interface")

    def power(self, on):
        self.events.append(("power", on))
        if self.failure == "power":
            raise OSError("command failed")

    def start_measuring(self):
        self.events.append("start")
        if self.failure == "start":
            raise OSError("start failed")
        self.chunks.extend(self.ready_chunks)
        if self.failure_after_start is not None:
            self.failure = self.failure_after_start

    def decode(self, data):
        self.last_frame_count = len(data)
        if self.failure == "decode":
            raise ValueError("decode failed")
        return data

    def close(self):
        self.events.append("close")
        if self.failure == "close":
            raise OSError("close failed")


class SessionTest(unittest.TestCase):
    def make(self, **kwargs):
        self.clock = Clock()
        self.backend = FakeBackend("fake-ppk")
        self.writer = MemoryWriter()
        defaults = dict(port="fake-ppk", voltage_mv=3872, acknowledgment=ACK,
                        backend_factory=lambda port: self.backend, writer=self.writer,
                        clock=self.clock, sleep=self.clock.advance)
        defaults.update(kwargs)
        session = PPK2Session(**defaults)
        self.addCleanup(lambda: session.finish(usb_reconnected=True) if session.started and not session.closed else None)
        return session

    def test_voltage_no_clamping_and_bool_rejected(self):
        for value in (799, 5001, 0, -1, 3.872, 3872.0, "3872", True, None):
            with self.subTest(value=value), self.assertRaises(ValueError):
                validate_voltage(value)
        for value in (800, 3872, 5000):
            self.assertEqual(validate_voltage(value), value)

    def test_safety_prevents_even_open(self):
        factory = mock.Mock()
        for field in range(4):
            acks = [True] * 4
            acks[field] = False
            with self.assertRaises(ValueError):
                PPK2Session(port="x", voltage_mv=3872, acknowledgment=SafetyAcknowledgment(*acks), backend_factory=factory)
        factory.assert_not_called()

    def test_config_validation_before_open(self):
        factory = mock.Mock()
        for kwargs in ({"window_s": 0}, {"window_s": math.nan}, {"window_s": 2},
                       {"reservoir_capacity": 1}, {"stale_timeout_s": 0},
                       {"dut_voltage_mv": 0}, {"dut_voltage_mv": True}):
            with self.subTest(kwargs=kwargs), self.assertRaises(ValueError):
                PPK2Session(port="x", voltage_mv=3872, acknowledgment=ACK, backend_factory=factory, **kwargs)
        factory.assert_not_called()

    def test_continuous_owner_across_states_and_confirmation(self):
        s = self.make().start(background=False)
        self.assertEqual(self.backend.events, [("open", "fake-ppk"), "stop", ("configure", 3872), ("power", True), "start"])
        for label in ("idle", "display", "sleep"):
            s.begin(label)
            self.backend.chunks.append([100, 200, 300])
            self.clock.advance(0.02)
            s.pump()
            result = s.end()
            self.assertEqual(result["sample_count"], 3)
            self.assertEqual(result["mean_uA"], 200)
            self.assertNotIn("close", self.backend.events)
        for confirm in (False, None, "true", 1):
            with self.assertRaises(USBReconnectRequired):
                s.finish(usb_reconnected=confirm)
        self.assertTrue(s.status()["holding"])
        result = s.finish(usb_reconnected=True)
        self.assertFalse(result["holding"])
        self.assertEqual(self.backend.events[-3:], ["stop", ("power", False), "close"])
        self.assertEqual(self.backend.events.count(("power", True)), 1)
        self.assertEqual(self.backend.events.count("start"), 1)
        before = self.backend.events[:]
        s.finish(usb_reconnected=True)
        self.assertEqual(self.backend.events, before)
        with self.assertRaises(RuntimeError):
            s.start()

    def test_stale_drain_timeout_and_no_power_on(self):
        s = self.make(stale_timeout_s=0.1)
        self.backend.stale = True
        with self.assertRaises(TimeoutError):
            s.start(background=False)
        # The explicit 250 ms CDC settle precedes (does not replace or
        # extend) the configured stale-drain deadline.
        elapsed = self.clock() - 1000
        self.assertGreaterEqual(elapsed, 0.25)
        self.assertLess(elapsed - 0.25, 0.2)
        self.assertNotIn(("power", True), self.backend.events)
        self.assertEqual(self.backend.events[-1], "close")
        self.assertTrue(self.writer.closed)

    def test_startup_failures_close_before_ready(self):
        for failure in ("configure", "power", "start"):
            with self.subTest(failure=failure):
                s = self.make()
                self.backend.failure = failure
                with self.assertRaises((OSError, ValueError)):
                    s.start(background=False)
                self.assertTrue(s.closed)
                self.assertIn("close", self.backend.events)
                if failure != "configure":
                    self.assertIn(("power", False), self.backend.events)

    def test_cleanup_continues_after_stop_error(self):
        s = self.make().start(background=False)
        self.backend.failure = "stop"
        result = s.finish(usb_reconnected=True)
        self.assertEqual(self.backend.events[-3:], ["stop", ("power", False), "close"])
        self.assertIn("stop: OSError", result["cleanup_errors"][0])
        self.assertTrue(self.writer.closed)

    def test_cleanup_continues_after_power_off_and_close_errors(self):
        for failure in ("power", "close"):
            with self.subTest(failure=failure):
                s = self.make().start(background=False)
                self.backend.failure = failure
                result = s.finish(usb_reconnected=True)
                self.assertTrue(s.closed)
                self.assertEqual(self.backend.events[-3:], ["stop", ("power", False), "close"])
                self.assertTrue(result["cleanup_errors"])
                self.assertTrue(self.writer.closed)

    def test_decode_error_retains_owner_and_records_fault(self):
        s = self.make().start(background=False)
        s.begin("decode")
        self.backend.chunks.append([1, 2])
        self.backend.failure = "decode"
        s.pump()
        self.assertTrue(s.status()["holding"])
        self.assertIn("decode failed", s.status()["fault"])
        self.assertNotIn(("power", False), self.backend.events)

    def test_read_error_latches_but_never_closes_owner(self):
        s = self.make().start(background=False)
        s.begin("idle")
        self.backend.failure = "read"
        s.pump()
        self.assertTrue(s.status()["holding"])
        self.assertIsNotNone(s.status()["fault"])
        self.assertEqual(s.status()["last_record"]["reason"], "collector_error")
        self.assertNotIn("close", self.backend.events)
        self.backend.failure = None
        self.backend.chunks.append([1, 2])
        s.pump()
        self.assertEqual(self.backend.chunks, [])  # still draining when paused
        self.assertEqual(s.total_samples, 0)
        with self.assertRaises(RuntimeError):
            s.begin("next")

    def test_disk_error_retains_owner(self):
        s = self.make().start(background=False)
        self.writer.write = mock.Mock(side_effect=OSError("disk full"))
        s.begin("idle")
        self.backend.chunks.append([1, 2])
        s.pump()
        self.assertIn("output", s.status()["fault"])
        self.assertTrue(s.status()["holding"])
        self.assertNotIn("close", self.backend.events)

    def test_interrupt_and_context_exit_retain_power(self):
        s = self.make().start(background=False)
        s.begin("idle")
        s.interrupt("SIGINT")
        self.assertTrue(s.status()["holding"])
        self.assertIsNone(s.status()["active_label"])
        self.assertNotIn(("power", False), self.backend.events)
        with self.assertRaises(USBReconnectRequired):
            s.__exit__(None, None, None)
        self.assertFalse(s.closed)
        self.assertFalse(s.__exit__(ValueError, ValueError("runner fault"), None))
        self.assertFalse(s.closed)

    def test_empty_samples_and_fixed_windows_and_sample_axis(self):
        s = self.make().start(background=False)
        s.begin("empty")
        self.clock.advance(0.25)
        s.pump()
        s.end()
        windows = [r for r in self.writer.records if r["type"] == "window"]
        self.assertEqual(len(windows), 3)
        self.assertAlmostEqual(windows[0]["host_duration_s"], 0.1)
        self.assertAlmostEqual(windows[2]["host_duration_s"], 0.05)
        self.assertEqual(windows[0]["coverage_ratio_estimate"], 0)
        self.assertEqual(windows[0]["sample_index_start"], 0)
        self.assertIsNone(windows[0]["mean_uA"])
        self.assertNotEqual(windows[0]["time_s"], s.origin)
        status = s.status()
        self.assertEqual(status["host_monotonic_origin_s"], s.origin)
        self.assertAlmostEqual(status["elapsed_host_s"], 0.25)
        self.assertAlmostEqual(status["sample_vs_host_drift_s"], -0.25)

    def test_timed_end_keeps_owner(self):
        s = self.make().start(background=False)
        s.begin("timed", duration_s=0.2)
        self.clock.advance(0.3)
        s.pump()
        self.assertTrue(s.status()["holding"])
        self.assertIsNone(s.status()["active_label"])
        result = s.status()["last_record"]
        self.assertAlmostEqual(result["host_duration_s"], 0.2)
        self.assertEqual(result["reason"], "duration_elapsed")
        with self.assertRaises(ValueError):
            s.end()

    def test_sample_axis_backlog_and_measured_dut_energy(self):
        s = self.make(dut_voltage_mv=3800).start(background=False)
        s.begin("test")
        self.backend.chunks.append([1000, 3000])
        self.clock.advance(0.01)
        s.pump()
        result = s.end()
        window = [r for r in self.writer.records if r["type"] == "window"][-1]
        self.assertEqual(window["sample_index_start"], 0)
        self.assertEqual(window["sample_index_end"], 2)
        self.assertAlmostEqual(window["nominal_sample_end_time_s"], 0.00002)
        self.assertEqual(window["max_observed_backlog_bytes"], 8)
        self.assertAlmostEqual(result["sampled_charge_uC"], 0.04)
        self.assertAlmostEqual(result["sampled_energy_uJ"], 0.15488)
        self.assertAlmostEqual(result["sampled_dut_energy_uJ"], 0.152)

    def test_invalid_samples_and_label_guard(self):
        s = self.make().start(background=False)
        for label in ("", "x\n", "a" * 129, None):
            with self.assertRaises(ValueError):
                s.begin(label)
        s.begin("valid")
        with self.assertRaises(ValueError):
            s.begin("overlap")
        self.backend.chunks.append([math.nan, None, math.inf, True, 10, 20])
        s.pump()
        result = s.end()
        self.assertEqual(result["sample_count"], 2)
        self.assertEqual(result["invalid_sample_count"], 4)
        json.dumps(result, allow_nan=False)

    def test_long_host_suspend_bounded_gap(self):
        s = self.make().start(background=False)
        s.begin("suspend")
        self.clock.advance(3600)
        s.pump()
        gaps = [r for r in self.writer.records if r["type"] == "empty_window_gap"]
        self.assertEqual(len(gaps), 1)
        self.assertGreater(gaps[0]["window_count"], 35000)
        self.assertLess(len(self.writer.records), 110)

    def test_background_ready_requires_first_finite_decoded_sample(self):
        s = self.make(first_sample_timeout_s=0.1)
        self.backend.ready_chunks = [[math.nan, None], [], [12.5]]
        s.start()
        status = s.status()
        self.assertTrue(status["transport_ready"])
        self.assertGreaterEqual(status["total_valid_samples"], 1)
        self.assertIsNotNone(status["first_finite_sample_host_offset_s"])
        self.assertIsNotNone(status["last_data_host_offset_s"])
        self.assertTrue(any(r["type"] == "ready" for r in self.writer.records))
        self.assertNotIn(("power", False), self.backend.events)

    def test_background_start_timeout_cleans_up_before_ready(self):
        for chunks in ([], [[math.nan, None, math.inf]]):
            with self.subTest(chunks=chunks):
                s = self.make(first_sample_timeout_s=0.025)
                self.backend.ready_chunks = chunks
                before = time.monotonic()
                with self.assertRaises(TimeoutError):
                    s.start()
                self.assertLess(time.monotonic() - before, 0.5)
                self.assertTrue(s.closed)
                self.assertFalse(s._thread.is_alive())
                self.assertEqual(self.backend.events[-3:], ["stop", ("power", False), "close"])
                self.assertFalse(any(r["type"] == "ready" for r in self.writer.records))
                self.assertTrue(self.writer.closed)

    def test_background_start_transport_errors_cleanup_before_ready(self):
        for failure in ("read", "decode"):
            with self.subTest(failure=failure):
                s = self.make(first_sample_timeout_s=0.1)
                self.backend.ready_chunks = [[1]]
                self.backend.failure_after_start = failure
                before = time.monotonic()
                with self.assertRaises(RuntimeError):
                    s.start()
                self.assertLess(time.monotonic() - before, 0.5)
                self.assertTrue(s.closed)
                self.assertFalse(s._thread.is_alive())
                self.assertFalse(s.transport_ready)
                self.assertEqual(self.backend.events[-3:], ["stop", ("power", False), "close"])

    def test_cooperative_start_does_not_claim_transport_readiness(self):
        s = self.make().start(background=False)
        self.assertFalse(s.status()["transport_ready"])
        self.assertIsNone(s.status()["first_finite_sample_host_offset_s"])
        self.backend.chunks.append([42])
        s.pump()
        self.assertEqual(s.status()["total_valid_samples"], 1)
        self.assertIsNotNone(s.status()["first_finite_sample_host_offset_s"])
        self.assertFalse(s.status()["transport_ready"])
        # Bytes containing no decoded finite values do not refresh last DATA;
        # the raw receive-side activity remains separately observable.
        last_finite = s.status()["last_data_host_offset_s"]
        self.clock.advance(2)
        self.backend.chunks.append([math.nan])
        s.pump()
        self.assertEqual(s.status()["last_data_host_offset_s"], last_finite)
        self.assertGreater(s.status()["last_receive_host_offset_s"], last_finite)

    def test_background_drains_and_joins(self):
        backend = FakeBackend("fake")
        backend.ready_chunks = [[10] * 100] * 10
        s = PPK2Session(port="fake", voltage_mv=3872, acknowledgment=ACK,
                        backend_factory=lambda _: backend)
        s.start()
        try:
            deadline = time.monotonic() + 1
            while s.status()["total_valid_samples"] < 1000 and time.monotonic() < deadline:
                time.sleep(0.005)
            self.assertEqual(s.status()["total_valid_samples"], 1000)
        finally:
            s.finish(usb_reconnected=True)
        self.assertFalse(s._thread.is_alive())


class StatsTest(unittest.TestCase):
    def test_exact_units_stats_coverage(self):
        stats = CurrentStats()
        stats.add([1000, 2000, 3000, 4000])
        r = stats.summary(0.00008, 3872, 100000)
        self.assertEqual(r["mean_uA"], 2500)
        self.assertEqual((r["min_uA"], r["max_uA"]), (1000, 4000))
        self.assertEqual(r["p50_uA"], 2500)
        self.assertAlmostEqual(r["p95_uA"], 3850)
        self.assertAlmostEqual(r["p99_uA"], 3970)
        self.assertEqual(r["coverage_ratio_estimate"], 0.5)
        self.assertEqual(r["missing_sample_count_estimate"], 4)
        self.assertAlmostEqual(r["nominal_sample_duration_s"], 0.00004)
        self.assertAlmostEqual(r["sampled_charge_uC"], 0.1)
        self.assertAlmostEqual(r["sampled_charge_mAh"], 0.1 / 3_600_000)
        self.assertAlmostEqual(r["sampled_energy_uJ"], 0.3872)
        self.assertAlmostEqual(r["sampled_energy_mWh"], 0.3872 / 3_600_000)

    def test_coverage_over_one_not_falsely_clamped(self):
        stats = CurrentStats()
        stats.add([1] * 4)
        r = stats.summary(0.00002, 3872, 100000)
        self.assertEqual(r["coverage_ratio_estimate"], 2)
        self.assertEqual(r["missing_sample_count_estimate"], 0)

    def test_reservoir_bound_and_approximation_explicit(self):
        stats = CurrentStats(capacity=16)
        stats.add(list(range(10000)))
        self.assertEqual(len(stats.reservoir), 16)
        r = stats.summary(0.1, 3872, 100000)
        self.assertEqual(r["mean_uA"], 4999.5)
        self.assertEqual(r["percentile_method"], "uniform_reservoir_linear_estimate")
        self.assertEqual(r["percentile_sample_count"], 16)

    def test_raw_budget_and_index_format(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "raw.bin"
            writer = RawWriter(path, max_bytes=24)
            writer.write(100, [1.5, 2.5, 3.5])
            writer.close()
            self.assertTrue(writer.truncated)
            self.assertEqual(path.stat().st_size, 24)
            self.assertEqual(list(struct.iter_unpack("<Qf", path.read_bytes())), [(100, 1.5), (101, 2.5)])

    def test_json_budget_never_overwrites_and_rejects_nan(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "out.jsonl"
            writer = JSONLWriter(path, max_bytes=4096)
            writer.write({"label": "μA"})
            with self.assertRaises(BudgetExceeded):
                writer.write({"large": "x" * 4096})
            with self.assertRaises(ValueError):
                writer.write({"bad": math.nan})
            writer.close()
            self.assertLessEqual(path.stat().st_size, 4096)
            with self.assertRaises(FileExistsError):
                JSONLWriter(path)


class AdapterTest(unittest.TestCase):
    def test_split_frame_never_invents_partial_sample(self):
        class API:
            remainder = None
            def get_samples(self, data):
                assert len(data) % 4 == 0
                return list(struct.unpack("<" + "I" * (len(data) // 4), data)), []
        backend = PPK2Backend.__new__(PPK2Backend)
        backend.api = API()
        backend.remainder = b""
        for fragment in (b"\x01", b"\x00\x00"):
            self.assertEqual(backend.decode(fragment), [])
        self.assertEqual(backend.decode(b"\x00\x02\x00\x00\x00\x03"), [1, 2])
        self.assertEqual(backend.last_frame_count, 2)
        self.assertEqual(backend.decode(b"\x00\x00\x00"), [3])

    def test_descriptor_discovery_returns_both_without_open(self):
        def port(name, product="PPK2", vid=0x1915, description="PPK2"):
            return mock.Mock(device=name, product=product, vid=vid, pid=1,
                             description=description, serial_number="fake-id", location="fake-loc", interface="CDC")
        records = port_descriptions([port("fake-a"), port("fake-b"), port("other", product="ESP32", vid=1)])
        self.assertEqual([r["port"] for r in records], ["fake-a", "fake-b"])


class ControlTest(unittest.TestCase):
    def test_dispatch_rejects_unsafe_or_unknown_commands(self):
        session = mock.Mock()
        for command in (None, {"op": "power-cycle"}, {"op": "finish", "force": True}, {"op": "status", "extra": 1}):
            with self.assertRaises(ValueError):
                dispatch(session, command)
        session.assert_not_called()
        dispatch(session, {"op": "begin", "label": "idle", "duration_s": 1})
        session.begin.assert_called_once_with("idle", 1)
        dispatch(session, {"op": "finish", "usb_reconnected": False})
        session.finish.assert_called_once_with(usb_reconnected=False)

    def test_cli_has_no_voltage_default_or_unconfirmed_finish(self):
        parser = build_parser()
        with mock.patch("sys.stderr"):
            with self.assertRaises(SystemExit):
                parser.parse_args(["hold", "--port", "fake", "--output", "fake.jsonl"])
            with self.assertRaises(SystemExit):
                parser.parse_args(["control", "--control-dir", "fake", "finish"])
        args = parser.parse_args(["record", "--port", "fake", "--voltage-mv", "3872", "--output", "fake.jsonl"])
        self.assertEqual(args.voltage_mv, 3872)
        self.assertEqual(interactive_command('begin "display on" 2'), {"op": "begin", "label": "display on", "duration_s": 2})
        with self.assertRaises(ValueError):
            interactive_command("finish")

    def test_real_fifo_blocking_handshake_without_serial(self):
        with tempfile.TemporaryDirectory() as directory:
            server = ControlServer(Path(directory) / "control")
            session = mock.Mock()
            session.status.return_value = {"holding": True, "fault": None}
            stop = threading.Event()
            def service():
                import selectors
                with selectors.DefaultSelector() as selector:
                    selector.register(server.fd, selectors.EVENT_READ)
                    while not stop.is_set():
                        if selector.select(0.1):
                            server.service(session)
            thread = threading.Thread(target=service)
            thread.start()
            try:
                result = send_command(server.directory, {"op": "status"}, timeout_s=1)
                self.assertEqual(result, {"holding": True, "fault": None})
                with self.assertRaises(RuntimeError):
                    send_command(server.directory, {"op": "bad"}, timeout_s=1)
                self.assertEqual(list(server.directory.glob("reply-*")), [])
                with self.assertRaises(FileExistsError):
                    ControlServer(server.directory)
            finally:
                stop.set()
                thread.join()
                server.close()

    def test_control_timeout_does_not_modify_or_close_owner(self):
        with tempfile.TemporaryDirectory() as directory:
            server = ControlServer(Path(directory) / "control")
            try:
                with self.assertRaises(TimeoutError):
                    send_command(server.directory, {"op": "status"}, timeout_s=0.01)
                self.assertIsNotNone(server.fd)
                self.assertEqual(list(server.directory.glob("reply-*")), [])
            finally:
                server.close()


if __name__ == "__main__":
    unittest.main()
