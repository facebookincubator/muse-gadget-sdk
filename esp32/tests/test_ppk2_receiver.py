# Copyright (c) Meta Platforms, Inc. and affiliates.
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy at http://www.apache.org/licenses/LICENSE-2.0
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""Independent receiver, starvation, fault, framing, arrival, and GIL fakes.

No serial device, USB discovery, Bluetooth, firmware, or hardware is accessed.
"""

import json
import selectors
import struct
import sys
import tempfile
import threading
import time
import unittest
from collections import deque
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from tools.power import ppk2_profile as cli
from tools.power.ppk2_backend import PPK2Backend
from tools.power.ppk2_control import ControlServer, send_command
from tools.power.ppk2_receiver import FastRawReceiver, ReceiverStopTimeout, TransportFault
from tools.power.ppk2_session import PPK2Session
from test_ppk2_profile import ACK, Clock, MemoryWriter
import test_ppk2_backend as adapter_tests


class ByteSource:
    def __init__(self):
        self.condition = threading.Condition()
        self.chunks = deque()
        self.errors = 0
        self.read_packets = 0
        self.read_threads = set()

    def feed(self, *chunks):
        with self.condition:
            self.chunks.extend(chunks)
            self.condition.notify_all()

    def read(self):
        with self.condition:
            self.read_threads.add(threading.current_thread().name)
            if self.errors:
                self.errors -= 1
                raise OSError("fake receive failure")
            if not self.chunks:
                return b"", 0
            backlog = sum(len(chunk) for chunk in self.chunks)
            chunk = self.chunks.popleft()
            self.read_packets += 1
            self.condition.notify_all()
            return chunk, backlog


def wait_receiver(receiver, predicate, timeout=1):
    with receiver._condition:
        return receiver._condition.wait_for(predicate, timeout)


class ReceiverTest(unittest.TestCase):
    def make(self, **kwargs):
        self.source = ByteSource()
        receiver = FastRawReceiver(self.source.read, **kwargs).start()
        self.addCleanup(receiver.stop)
        return receiver

    def test_independent_receiver_collects_while_consumer_busy(self):
        receiver = self.make()
        self.source.feed(*([b"x" * 1024] * 50))
        # Do conversion-like CPU work without calling read_batch at all.
        until = time.monotonic() + 0.05
        value = 0
        while time.monotonic() < until:
            value = (value * 17 + 3) % 1234567
        self.assertTrue(wait_receiver(receiver, lambda: receiver.received_bytes == 51200))
        snapshot = receiver.snapshot()
        self.assertEqual(snapshot["queued_bytes"], 51200)
        self.assertIsNone(snapshot["fault"])
        self.assertEqual(self.source.read_threads, {"ppk2-fast-raw"})
        data, _ = receiver.read_batch()
        self.assertEqual(len(data), 51200)

    def test_overflow_is_explicit_and_still_drains(self):
        receiver = self.make(max_bytes=1024)
        self.source.feed(b"a" * 1024, b"b" * 1024)
        self.assertTrue(wait_receiver(receiver, lambda: receiver.fault is not None))
        with self.assertRaises(TransportFault):
            receiver.read_batch()
        self.source.feed(b"c" * 512)
        self.assertTrue(wait_receiver(receiver, lambda: receiver.received_bytes >= 2560))
        snapshot = receiver.snapshot()
        self.assertTrue(snapshot["running"])
        self.assertEqual(snapshot["overflow_count"], 1)
        self.assertEqual(snapshot["queued_bytes"], 1024)
        self.assertEqual(snapshot["invalidated_queue_bytes"], 1024)
        self.assertEqual(snapshot["discarded_bytes"], 1536)

    def test_entry_budget_bounds_metadata_for_tiny_packets(self):
        receiver = self.make(max_bytes=1024, max_chunks=2)
        self.source.feed(b"a", b"b", b"c")
        self.assertTrue(wait_receiver(receiver, lambda: receiver.fault is not None))
        snapshot = receiver.snapshot()
        self.assertEqual(snapshot["queued_chunks"], 2)
        self.assertEqual(snapshot["queued_bytes"], 2)
        self.assertEqual(snapshot["discarded_bytes"], 1)

    def test_read_failure_latches_and_future_bytes_are_invalidated(self):
        receiver = self.make()
        self.source.errors = 1
        self.assertTrue(wait_receiver(receiver, lambda: receiver.fault is not None))
        self.source.feed(b"a" * 64)
        self.assertTrue(wait_receiver(receiver, lambda: receiver.received_bytes == 64))
        self.assertEqual(receiver.snapshot()["discarded_bytes"], 64)
        with self.assertRaises(TransportFault):
            receiver.read_batch()
        self.assertTrue(receiver.running)

    def test_bounded_split_dequeue_retains_arrival_and_byte_ordinals(self):
        clock = Clock()
        receiver = self.make(clock=clock)
        self.source.feed(b"abcde")
        self.assertTrue(wait_receiver(receiver, lambda: receiver.received_bytes == 5))
        first_arrival = clock()
        clock.advance(1)
        self.source.feed(b"fghij")
        self.assertTrue(wait_receiver(receiver, lambda: receiver.received_bytes == 10))
        clock.advance(2)
        data, segments = receiver.read_batch(7)
        self.assertEqual(data, b"abcdefg")
        self.assertEqual([s["byte_count"] for s in segments], [5, 2])
        self.assertEqual([s["received_byte_index"] for s in segments], [0, 5])
        self.assertEqual(segments[0]["arrival_monotonic_s"], first_arrival)
        tail, segments = receiver.read_batch(7)
        self.assertEqual(tail, b"hij")
        self.assertEqual(segments[0]["received_byte_index"], 7)
        self.assertEqual(receiver.snapshot()["max_consumer_delay_s"], 3)
        self.assertEqual(receiver.snapshot()["consumed_bytes"], 10)

    def test_atomic_boundary_orders_post_begin_commits(self):
        clock = Clock()
        receiver = self.make(clock=clock)
        self.source.feed(b"1234")
        self.assertTrue(wait_receiver(receiver, lambda: receiver.received_bytes == 4))
        boundary = receiver.boundary_snapshot()
        clock.advance(0.01)
        self.source.feed(b"5678")
        self.assertTrue(wait_receiver(receiver, lambda: receiver.received_bytes == 8))
        _, segments = receiver.read_batch()
        self.assertEqual(boundary["received_bytes"], 4)
        self.assertGreaterEqual(segments[1]["arrival_monotonic_s"], boundary["boundary_monotonic_s"])

    def test_maximum_consumer_batch_is_64kib(self):
        receiver = self.make()
        self.source.feed(*([b"x" * 16384] * 8))
        self.assertTrue(wait_receiver(receiver, lambda: receiver.received_bytes == 131072))
        data, _ = receiver.read_batch()
        self.assertEqual(len(data), 65536)
        self.assertEqual(receiver.snapshot()["queued_bytes"], 65536)
        with self.assertRaises(ValueError):
            receiver.read_batch(65537)

    def test_live_thread_after_start_error_is_not_unpublished(self):
        entered, release = threading.Event(), threading.Event()
        def blocked():
            entered.set()
            release.wait(2)
            return b"", 0
        receiver = FastRawReceiver(blocked)
        original_start = threading.Thread.start
        def launched_then_failed(thread):
            original_start(thread)
            raise RuntimeError("fake error AFTER native launch")
        try:
            with mock.patch.object(threading.Thread, "start", launched_then_failed):
                with self.assertRaisesRegex(RuntimeError, "AFTER native launch"):
                    receiver.start()
            self.assertTrue(entered.wait(1))
            handle = receiver._thread
            self.assertIsNotNone(handle.ident)
            self.assertTrue(receiver.join_required)
            with self.assertRaises(ReceiverStopTimeout):
                receiver.stop(0.02)
            self.assertIs(receiver._thread, handle)
            self.assertTrue(receiver.running)
        finally:
            release.set()
            receiver.stop()
        self.assertFalse(receiver.join_required)

    def test_budget_validation_and_no_restart(self):
        for value in (0, 1023, 16777217, True, 1024.0):
            with self.assertRaises(ValueError):
                FastRawReceiver(lambda: (b"", 0), max_bytes=value)
        receiver = self.make()
        receiver.stop()
        with self.assertRaises(RuntimeError):
            receiver.start()


class BufferedBackendTest(unittest.TestCase):
    def setUp(self):
        self.fixture = adapter_tests.AdapterFailureTest()
        self.fixture.setUp()
        self.addCleanup(self.fixture.doCleanups)
        self.source = ByteSource()
        self.backend = PPK2Backend("fake-a")
        self.backend._read_direct = self.source.read  # pure memory, NOT serial
        self.backend.api.start_measuring = lambda: self.backend.api._write_serial((6,))
        self.backend.api.get_samples = lambda data: (list(struct.unpack("<" + "I" * (len(data) // 4), data)), [])
        self.sessions = []
        self.addCleanup(self.cleanup)

    def cleanup(self):
        for session in self.sessions:
            if session.started and not session.closed:
                session.finish(usb_reconnected=True)
        if not self.sessions:
            self.backend.stop_receiver()
            self.backend.close()

    def session(self, **kwargs):
        self.writer = MemoryWriter()
        session = PPK2Session(port="fake-a", voltage_mv=3872, acknowledgment=ACK,
                              backend_factory=lambda _: self.backend, writer=self.writer, **kwargs)
        self.sessions.append(session)
        return session

    def start_ready(self):
        original = self.backend.api.start_measuring
        def start():
            original()
            self.source.feed(struct.pack("<I", 42))
        self.backend.api.start_measuring = start
        return self.session().start()

    def test_raw_thread_start_failure_after_source_on_cleans_up(self):
        session = self.session()
        with mock.patch.object(threading.Thread, "start", side_effect=RuntimeError("fake raw spawn failure")):
            with self.assertRaisesRegex(RuntimeError, "fake raw spawn failure"):
                session.start()
        self.assertTrue(session.closed)
        self.assertFalse(session.status()["transport_ready"])
        self.assertIsNone(self.backend.receiver._thread)
        self.assertFalse(self.backend.receiver.join_required)
        calls = self.fixture.serial.write.call_args_list
        self.assertIn(mock.call(b"\x0c\x01"), calls)  # source ON was attempted
        self.assertIn(mock.call(b"\x06"), calls)  # acquisition command before spawn
        self.assertEqual(calls.count(mock.call(b"\x07")), 2)  # initial + cleanup STOP
        self.assertEqual(calls.count(mock.call(b"\x0c\x00")), 1)
        self.fixture.serial.close.assert_called_once()
        self.assertFalse(session.finish(usb_reconnected=True)["holding"])
        with self.assertRaises(RuntimeError):
            self.backend.receiver.start()

    def test_consumer_thread_start_failure_never_joins_unstarted_thread(self):
        session = self.session()
        original_start, original_join = threading.Thread.start, threading.Thread.join
        def start(thread):
            if thread.name == "ppk2-owner-drain":
                raise RuntimeError("fake consumer spawn failure")
            return original_start(thread)
        def join(thread, *args, **kwargs):
            self.assertTrue(thread._started.is_set(), "unstarted consumer must not be joined")
            return original_join(thread, *args, **kwargs)
        with mock.patch.object(threading.Thread, "start", start), mock.patch.object(threading.Thread, "join", join):
            with self.assertRaisesRegex(RuntimeError, "fake consumer spawn failure"):
                session.start()
            self.assertTrue(session.closed)
            self.assertFalse(session.status()["transport_ready"])
            self.assertIsNone(session._thread.ident)
            self.assertFalse(session.finish(usb_reconnected=True)["holding"])
        self.assertFalse(self.backend.receiver.running)
        self.assertFalse(self.backend.receiver.join_required)
        self.fixture.serial.close.assert_called_once()
        self.assertIn(mock.call(b"\x0c\x00"), self.fixture.serial.write.call_args_list)

    def test_once_open_once_start_and_no_consumer_direct_serial_reads(self):
        session = self.start_ready()
        receiver = self.backend.receiver
        self.assertTrue(receiver.running)
        session.begin("idle")
        self.source.feed(struct.pack("<I", 100))
        self.assertTrue(wait_receiver(receiver, lambda: receiver.received_bytes >= 8))
        session.end()
        session.begin("next")
        self.assertIs(self.backend.receiver, receiver)
        with self.assertRaises(RuntimeError):
            self.backend.start_measuring()
        self.fixture.serial.read.assert_not_called()
        with self.assertRaises(RuntimeError):
            self.backend.close()
        self.fixture.serial.close.assert_not_called()
        session.finish(usb_reconnected=True)
        self.assertFalse(receiver.running)
        self.fixture.serial.close.assert_called_once()
        starts = self.fixture.serial.write.call_args_list.count(mock.call(b"\x06"))
        self.assertEqual(starts, 1)

    def test_slow_decode_does_not_block_receiver_or_fifo_status(self):
        session = self.start_ready()
        session.begin("busy")
        entered, release = threading.Event(), threading.Event()
        decode = self.backend.decode
        def slow(data):
            entered.set()
            if not release.wait(2):
                raise TimeoutError("fake decode release")
            return decode(data)
        self.backend.decode = slow
        self.source.feed(struct.pack("<I", 10))
        self.assertTrue(entered.wait(1))
        self.source.feed(*([struct.pack("<I", 20) * 256] * 25))
        self.assertTrue(wait_receiver(self.backend.receiver, lambda: self.backend.receiver.received_bytes >= 25608))
        with tempfile.TemporaryDirectory() as directory:
            server = ControlServer(Path(directory) / "control")
            stop = threading.Event()
            def serve():
                with selectors.DefaultSelector() as selector:
                    selector.register(server.fd, selectors.EVENT_READ)
                    while not stop.is_set():
                        if selector.select(0.05):
                            server.service(session)
            thread = threading.Thread(target=serve)
            thread.start()
            try:
                status = send_command(server.directory, {"op": "status"}, timeout_s=0.5)
                self.assertTrue(status["holding"])
                self.assertGreater(status["receiver"]["queued_bytes"], 0)
                self.assertIsNone(status["fault"])
                self.assertEqual(status["total_valid_samples"], 1)
            finally:
                release.set()
                stop.set()
                thread.join(1)
                server.close()
        session.finish(usb_reconnected=True)

    def test_overflow_latches_fault_without_off_and_no_valid_suffix(self):
        self.backend.receiver_max_bytes = 1024
        session = self.start_ready()
        session.begin("overflow")
        entered, release = threading.Event(), threading.Event()
        decode = self.backend.decode
        def slow(data):
            entered.set()
            release.wait(2)
            return decode(data)
        self.backend.decode = slow
        self.source.feed(struct.pack("<I", 10))
        self.assertTrue(entered.wait(1))
        self.source.feed(b"x" * 1024, b"y" * 1024)
        self.assertTrue(wait_receiver(self.backend.receiver, lambda: self.backend.receiver.fault is not None))
        status = session.status()
        self.assertIn("queue_overflow", status["fault"])
        self.assertTrue(status["holding"])
        self.assertTrue(status["receiver"]["running"])
        self.fixture.serial.close.assert_not_called()
        self.assertNotIn(mock.call(b"\x0c\x00"), self.fixture.serial.write.call_args_list)
        self.source.feed(b"z" * 512)
        self.assertTrue(wait_receiver(self.backend.receiver, lambda: self.backend.receiver.discarded_bytes >= 1536))
        with self.assertRaises(RuntimeError):
            session.begin("not-valid")
        release.set()
        session.finish(usb_reconnected=True)
        self.assertEqual(session.total_samples, 1)

    def test_partial_frames_across_queue_arrivals(self):
        session = self.session().start(background=False)
        raw = struct.pack("<I", 77)
        for count, part in ((1, raw[:1]), (3, raw[1:3]), (4, raw[3:])):
            self.source.feed(part)
            self.assertTrue(wait_receiver(self.backend.receiver, lambda: self.backend.receiver.received_bytes >= count))
            session.pump()
        self.assertEqual(session.total_samples, 1)
        self.assertEqual(session.decoded_missing, 0)
        self.assertEqual(self.backend.remainder, b"")

    def test_queued_prebegin_prefix_excluded_and_anchor_exact(self):
        session = self.session().start(background=False)
        self.source.feed(struct.pack("<II", 1, 2))
        self.assertTrue(wait_receiver(self.backend.receiver, lambda: self.backend.receiver.received_bytes == 8))
        session.begin("watcher-sweep")
        begin = [r for r in self.writer.records if r["type"] == "begin"][-1]
        self.assertEqual(begin["first_valid_received_index"], 0)
        self.assertEqual(begin["window_sample_index_start"], 2)
        self.assertEqual(begin["queued_prefix_bytes"], 8)
        self.assertEqual(begin["decoded_missing_frame_count_at_begin"], 0)
        self.assertEqual(begin["total_invalid_samples_at_begin"], 0)
        self.assertEqual(begin["late_unassigned_sample_count_at_begin"], 0)
        self.source.feed(struct.pack("<I", 200))
        self.assertTrue(wait_receiver(self.backend.receiver, lambda: self.backend.receiver.received_bytes == 12))
        session.pump()
        result = session.end()
        self.assertEqual(session.total_samples, 3)
        self.assertEqual(result["sample_count"], 1)
        self.assertEqual(result["mean_uA"], 200)
        self.assertEqual(result["label_late_unassigned_sample_count"], 0)
        self.assertEqual(session.status()["late_unassigned_sample_count"], 0)
        self.assertGreaterEqual(result["label_max_consumer_delay_s"], 0)
        windows = [r for r in self.writer.records if r["type"] == "window"]
        self.assertEqual(windows[-1]["sample_index_start"], 2)
        self.assertEqual(windows[-1]["sample_index_end"], 3)
        for window in windows:
            self.assertEqual(window["sample_index_end"] - window["sample_index_start"], window["sample_count"])
            self.assertGreaterEqual(window["host_start_offset_s"], begin["host_offset_s"])

    def test_zero_windows_before_prefix_decode_do_not_fabricate_samples(self):
        session = self.session().start(background=False)
        self.source.feed(struct.pack("<II", 1, 2))
        self.assertTrue(wait_receiver(self.backend.receiver, lambda: self.backend.receiver.received_bytes == 8))
        session.begin("prefix")
        session._tick(session.clock() + 0.25)
        windows = [r for r in self.writer.records if r["type"] == "window"]
        self.assertTrue(windows)
        self.assertEqual(session.total_samples, 0)
        for window in windows:
            self.assertEqual((window["sample_index_start"], window["sample_index_end"], window["sample_count"]), (2, 2, 0))

    def test_begin_baselines_immutable_and_label_late_delta_explicit(self):
        session = self.session().start(background=False)
        session.begin("baselines")
        begin = [r for r in self.writer.records if r["type"] == "begin"][-1]
        session._ingest([float("nan")], 2, session.clock())
        session._tick(session.clock() + 0.25)
        session._ingest([10], 1, session.clock())  # deliberately late fake arrival
        status = session.status()
        result = session.end()
        self.assertEqual(begin["decoded_missing_frame_count_at_begin"], 0)
        self.assertEqual(begin["total_invalid_samples_at_begin"], 0)
        self.assertEqual(begin["late_unassigned_sample_count_at_begin"], 0)
        self.assertEqual(status["decoded_missing_frame_count"], 1)
        self.assertEqual(status["total_invalid_samples"], 1)
        self.assertEqual(status["late_unassigned_sample_count"], 1)
        self.assertEqual(result["label_late_unassigned_sample_count"], 1)
        self.assertEqual(result["invalid_sample_count"], 1)
        for window in self.writer.records:
            if window["type"] == "window":
                self.assertEqual(window["sample_index_end"] - window["sample_index_start"], window["sample_count"])

    def test_consumer_join_timeout_retains_raw_drain_and_control_state(self):
        session = self.start_ready()
        entered, release = threading.Event(), threading.Event()
        decode = self.backend.decode
        def blocked(data):
            entered.set()
            release.wait(2)
            return decode(data)
        self.backend.decode = blocked
        self.source.feed(struct.pack("<I", 10))
        self.assertTrue(entered.wait(1))
        join = session._thread.join
        session._thread.join = lambda timeout: join(0.02)
        try:
            with self.assertRaises(RuntimeError):
                session.finish(usb_reconnected=True)
            status = session.status()
            self.assertTrue(status["holding"])
            self.assertFalse(status["transport_ready"])
            self.assertIn("UNKNOWN", status["fault"])
            self.assertTrue(self.backend.receiver.running)
            self.fixture.serial.close.assert_not_called()
            self.assertNotIn(mock.call(b"\x0c\x00"), self.fixture.serial.write.call_args_list)
        finally:
            release.set()
            session._thread.join = join
        session.finish(usb_reconnected=True)
        self.fixture.serial.close.assert_called_once()

    def test_fast_join_timeout_retains_serial_and_can_retry_finish(self):
        entered, release = threading.Event(), threading.Event()
        def blocked_read():
            if self.backend._acquisition_started:
                entered.set()
                release.wait(2)
            return b"", 0
        self.backend._read_direct = blocked_read
        session = self.session().start(background=False)
        self.assertTrue(entered.wait(1))
        original_stop = self.backend.receiver.stop
        self.backend.receiver.stop = lambda: original_stop(0.02)
        with self.assertRaises(RuntimeError):
            session.finish(usb_reconnected=True)
        self.assertFalse(session.closed)
        self.assertTrue(session.status()["holding"])
        self.fixture.serial.close.assert_not_called()
        self.assertNotIn(mock.call(b"\x0c\x00"), self.fixture.serial.write.call_args_list)
        release.set()
        self.backend.receiver.stop = original_stop
        session.finish(usb_reconnected=True)
        self.fixture.serial.close.assert_called_once()


class ControlBudgetTest(unittest.TestCase):
    def test_long_fault_status_remains_usable_with_explicit_compaction(self):
        status = {"holding": True, "fault": "UNKNOWN; OFF NOT VERIFIED " + "🚫" * 512,
                  "decoded_missing_frame_count": 7, "late_unassigned_sample_count": 3,
                  "total_invalid_samples": 2, "total_valid_samples": 123456,
                  "receiver": {"running": True, "max_combined_backlog_bytes": 4096,
                               "fault": {"kind": "read_error", "message": "🚫" * 512}},
                  "last_record": {"label": "x" * 128, "detail": "x" * 10000},
                  "cleanup_errors": ["🚫" * 512] * 3}
        with tempfile.TemporaryDirectory() as directory:
            server = ControlServer(Path(directory) / "control")
            session = mock.Mock()
            session.status.return_value = status
            def serve():
                with selectors.DefaultSelector() as selector:
                    selector.register(server.fd, selectors.EVENT_READ)
                    if selector.select(1):
                        server.service(session)
            thread = threading.Thread(target=serve)
            thread.start()
            try:
                result = send_command(server.directory, {"op": "status"}, timeout_s=1)
                self.assertTrue(result["holding"])
                self.assertTrue(result["control_details_truncated"])
                self.assertIn("UNKNOWN; OFF NOT VERIFIED", result["fault"])
                self.assertIsNone(result["last_record"])
                self.assertEqual(result["cleanup_errors_omitted_count"], 1)
                for key in ("decoded_missing_frame_count", "late_unassigned_sample_count", "total_invalid_samples", "total_valid_samples"):
                    self.assertEqual(result[key], status[key])
                self.assertEqual(result["receiver"]["max_combined_backlog_bytes"], 4096)
            finally:
                thread.join(1)
                server.close()


class CLIGILTest(unittest.TestCase):
    def run_fake(self, isolated, fail=False, evidence_override=None):
        session = mock.Mock()
        session.started = not fail
        session.closed = fail
        session.cleanup_errors = []
        session.fault = None
        session.status.return_value = {"transport_ready": True, "holding": True, "fault": None,
                                       "cleanup_errors": [], "total_valid_samples": 1,
                                       "first_finite_sample_host_offset_s": 0,
                                       "last_data_host_offset_s": 0, "receiver": {}}
        session.status.return_value.update(evidence_override or {})
        self.notices = []
        observed = []
        def start():
            observed.append(sys.getswitchinterval())
            if fail:
                raise ValueError("fake startup")
        session.start.side_effect = start
        selector = mock.Mock()
        key = mock.Mock(data="control")
        selector.select.return_value = [(key, 1)]
        server = mock.Mock(fd=123)
        server.service.side_effect = lambda _: setattr(session, "closed", True)
        argv = ["hold", "--port", "fake", "--voltage-mv", "3872", "--output", "fake.jsonl",
                "--ack-source-wiring", "--ack-battery-isolated", "--ack-no-charging-backfeed",
                "--ack-usb-connected-before-start"]
        with mock.patch.object(cli, "PPK2Session", return_value=session), mock.patch.object(cli, "ControlServer", return_value=server), mock.patch.object(cli, "JSONLWriter"), mock.patch.object(cli.selectors, "DefaultSelector", return_value=selector), mock.patch.object(cli, "_print", side_effect=lambda value, **kwargs: self.notices.append(value)):
            code = cli.main(argv, isolated=isolated)
        return observed[0], code

    def test_cli_ready_requires_fault_free_holding_and_complete_cleanup_evidence(self):
        for override in ({"fault": "UNKNOWN"}, {"holding": False},
                         {"cleanup_errors": ["OFF failed"]},
                         {"cleanup_errors_omitted_count": 1}):
            with self.subTest(override=override):
                self.run_fake(False, evidence_override=override)
                self.assertFalse(any(isinstance(value, dict) and value.get("ready") is True for value in self.notices))
                self.assertFalse(any(isinstance(value, str) and "Only now remove" in value for value in self.notices))
        self.run_fake(False)
        self.assertTrue(any(isinstance(value, dict) and value.get("ready") is True for value in self.notices))

    def test_library_entrypoint_does_not_change_gil(self):
        before = sys.getswitchinterval()
        observed, code = self.run_fake(False)
        self.assertEqual(code, 0)
        self.assertEqual(observed, before)
        self.assertEqual(sys.getswitchinterval(), before)

    def test_isolated_cli_restores_gil_after_finish_or_startup_failure(self):
        before = sys.getswitchinterval()
        for fail in (False, True):
            observed, code = self.run_fake(True, fail)
            self.assertAlmostEqual(observed, 0.0005)
            self.assertEqual(code, 1 if fail else 0)
            self.assertEqual(sys.getswitchinterval(), before)


if __name__ == "__main__":
    unittest.main()
