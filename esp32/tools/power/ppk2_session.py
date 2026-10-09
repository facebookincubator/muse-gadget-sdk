# Copyright (c) Meta Platforms, Inc. and affiliates.
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy at http://www.apache.org/licenses/LICENSE-2.0
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""One uninterrupted source-power owner, from USB-connected setup to finish.

Public API: start / begin(label, duration_s=None) / end / status /
finish(usb_reconnected=True). A context DOES NOT implicitly cut power on exit.
Runtime faults pause recording but retain the owner. The caller must arrange
USB reconnection and explicitly finish, even after an exception or SIGINT.
"""

import math
import threading
import time
from dataclasses import dataclass

from .ppk2_backend import PPK2Backend, SAMPLE_RATE_HZ, validate_voltage
from .ppk2_receiver import DEFAULT_QUEUE_BYTES, validate_queue_bytes
from .ppk2_stats import CurrentStats


class USBReconnectRequired(RuntimeError):
    pass


@dataclass(frozen=True)
class SafetyAcknowledgment:
    source_wiring_verified: bool
    battery_isolated: bool
    no_charging_backfeed: bool
    usb_connected_before_start: bool

    def validate(self):
        if any(value is not True for value in (
            self.source_wiring_verified, self.battery_isolated,
            self.no_charging_backfeed, self.usb_connected_before_start,
        )):
            raise ValueError("explicit source wiring, isolated battery, no charging backfeed, and initial USB-connected acknowledgments required")


class PPK2Session:
    def __init__(self, *, port, voltage_mv, acknowledgment, writer=None,
                 raw_writer=None, window_s=0.1, reservoir_capacity=4096,
                 stale_timeout_s=2.0, first_sample_timeout_s=2.0,
                 dut_voltage_mv=None, receiver_max_bytes=DEFAULT_QUEUE_BYTES,
                 backend_factory=PPK2Backend,
                 clock=time.monotonic, sleep=time.sleep):
        validate_voltage(voltage_mv)
        validate_queue_bytes(receiver_max_bytes)
        if not isinstance(acknowledgment, SafetyAcknowledgment):
            raise ValueError("SafetyAcknowledgment required before opening PPK2")
        acknowledgment.validate()
        if not isinstance(port, str) or not port:
            raise ValueError("explicit PPK2 port required")
        if not math.isfinite(window_s) or not 0.01 <= window_s <= 1.0:
            raise ValueError("fixed aggregate window must be 0.01..1.0 seconds")
        if not math.isfinite(stale_timeout_s) or not 0.1 <= stale_timeout_s <= 10:
            raise ValueError("stale drain timeout must be 0.1..10 seconds")
        if isinstance(first_sample_timeout_s, bool) or not isinstance(first_sample_timeout_s, (int, float)) or not math.isfinite(first_sample_timeout_s) or not 0.01 <= first_sample_timeout_s <= 10:
            raise ValueError("first finite sample timeout must be 0.01..10 seconds")
        if dut_voltage_mv is not None and (isinstance(dut_voltage_mv, bool) or not isinstance(dut_voltage_mv, (int, float)) or not math.isfinite(dut_voltage_mv) or not 0 < dut_voltage_mv <= 5000):
            raise ValueError("independently measured DUT voltage must be >0..5000 mV")
        CurrentStats(reservoir_capacity)  # validate before hardware access
        self.dut_voltage_mv = dut_voltage_mv
        self.port = port
        self.voltage_mv = voltage_mv
        self.writer = writer
        self.raw_writer = raw_writer
        self.window_s = window_s
        self.capacity = reservoir_capacity
        self.stale_timeout_s = stale_timeout_s
        self.first_sample_timeout_s = first_sample_timeout_s
        self.transport_ready = False
        self.first_finite_sample_host_s = None
        self.last_receive_host_s = None
        self._first_sample_event = threading.Event()
        self.factory = (lambda port: PPK2Backend(port, receiver_max_bytes=receiver_max_bytes)) if backend_factory is PPK2Backend else backend_factory
        self.receiver_max_bytes = receiver_max_bytes
        self.max_consumer_delay_s = 0.0
        self.last_consumer_delay_s = 0.0
        self.late_unassigned_sample_count = 0
        self.clock = clock
        self.sleep = sleep
        self.backend = None
        self.started = False
        self.closed = False
        self.confirmed = False
        self.fault = None
        self.cleanup_errors = []
        self.record = None
        self.last_record = None
        self.total_samples = 0
        self.decoded_missing = 0
        self.total_invalid_samples = 0
        self.empty_reads = 0
        self.read_errors = 0
        self.max_observed_backlog_bytes = 0
        self.last_data_host_s = None
        self.origin = None
        self._lock = threading.RLock()
        self._stop = threading.Event()
        self._thread = None
        self._power_attempted = False
        self._measuring_attempted = False

    def _emit(self, record):
        if self.writer is None or self.fault is not None:
            return
        try:
            self.writer.write(record)
        except Exception as exc:
            # Disk full/budget exhaustion MUST NOT tear down a live supply.
            self.fault = f"output: {type(exc).__name__}: {exc}"[:512]
            self.transport_ready = False

    def _stale_drain(self):
        deadline = self.clock() + self.stale_timeout_s
        last_stop = self.clock()
        quiet_since = None
        reads = empty_reads = drained_bytes = stop_retries = 0
        longest_quiet = 0.0
        while self.clock() < deadline:
            data = self.backend.read()
            now = self.clock()
            reads += 1
            if data:
                drained_bytes += len(data)
                if quiet_since is not None:
                    longest_quiet = max(longest_quiet, now - quiet_since)
                quiet_since = None
                if now - last_stop >= 0.1:
                    # CDC line-state setup can race the first STOP on open.
                    # Reassert sampling STOP only in this bounded pre-ON
                    # phase. Never restart/stop acquisition after READY here.
                    self.backend.stop_measuring()
                    stop_retries += 1
                    last_stop = self.clock()
            else:
                empty_reads += 1
                if quiet_since is None:
                    quiet_since = now
                else:
                    longest_quiet = max(longest_quiet, now - quiet_since)
                    if now - quiet_since >= 0.05:
                        return
            # Drain queued CDC bursts promptly; a 5 ms delay after every
            # nonempty 1 KiB read cannot keep up with a 400 kB/s stream.
            self.sleep(0.0001 if data else 0.005)
        raise TimeoutError(
            "PPK2 stale drain did not become quiet before deadline; power not enabled "
            f"(reads={reads}, bytes={drained_bytes}, empty_reads={empty_reads}, "
            f"stop_retries={stop_retries}, longest_quiet_s={longest_quiet:.3f})")

    def start(self, *, background=True):
        """Open once, stop stale acquisition, configure, ON, acquire forever.

        Returns only after the reader decodes a finite sample (default timeout
        two seconds). DUT USB must remain connected until this returns. Failed
        startup may explicitly OFF/close because no READY has been handed out.
        background=False is for fake tests/cooperative runners: it does NOT
        guarantee transport readiness; such runners MUST call pump continuously.
        """
        with self._lock:
            if self.started or self.closed or self.backend is not None:
                raise RuntimeError("session cannot be reopened or restarted")
            try:
                self.backend = self.factory(self.port)
                # Allow asynchronous CDC line-state setup to settle before
                # the first sampling command. This is still pre-ON/USB-fed.
                self.sleep(0.25)
                self.backend.stop_measuring()
                self._stale_drain()
                self.backend.configure(self.voltage_mv)
                self._power_attempted = True
                self.backend.power(True)
                self._measuring_attempted = True
                self.backend.start_measuring()
                self.origin = getattr(self.backend, "acquisition_origin_monotonic_s", self.clock())
                self.started = True
                self._emit({
                    "type": "session", "schema_version": 1,
                    "voltage_mv": self.voltage_mv, "sample_rate_hz": SAMPLE_RATE_HZ,
                    "dut_voltage_mv": self.dut_voltage_mv,
                    "host_monotonic_origin_s": self.origin,
                    "aggregate_window_s": self.window_s,
                    "port_descriptor": getattr(self.backend, "descriptor", {"port": self.port}),
                    "timing": "host windows and labels are receive-side boundaries, NOT hardware sample timestamps; nominal sample axis compresses unknown loss",
                    "coverage": "valid samples / (host duration * 100000); receive buffering can exceed 1; missing sample count is only an estimate",
                    "integrals": "observed samples only, no gap interpolation; energy uses configured source voltage, not measured DUT voltage",
                    "raw_format": "little-endian <Qf (valid received ordinal, float32 uA); nominal ordinal/100000 is NOT a hardware timestamp",
                    "percentile_reservoir_capacity": self.capacity,
                    "raw_receiver_capacity_bytes": self.receiver_max_bytes,
                    "receive_timing": "producer stamps each raw CDC segment; windows use arrival observations, NOT consumer processing time or sample timestamps",
                })
                if self.fault:
                    raise RuntimeError(self.fault)
                if background:
                    self._thread = threading.Thread(target=self._run, name="ppk2-owner-drain", daemon=False)
                    self._thread.start()
            except BaseException:
                self.transport_ready = False
                # Startup is still USB-connected; no READY has been handed out.
                if self._cleanup_backend():
                    self.closed = True
                    self._close_writers()
                else:
                    raise RuntimeError(self.fault)
                raise
        if background:
            try:
                # NEVER wait with _lock held: the reader needs that same lock
                # to decode, publish sample evidence, and signal this event.
                if not self._first_sample_event.wait(self.first_sample_timeout_s):
                    raise TimeoutError("no finite PPK2 sample before startup deadline; keep DUT USB connected")
                with self._lock:
                    self._refresh_receiver()
                    if self.fault or self.total_samples == 0:
                        raise RuntimeError(f"PPK2 transport failed before READY: {self.fault}")
                    self.transport_ready = True
                    self._emit({"type": "ready", "finite_sample_count": self.total_samples,
                                "first_finite_sample_host_offset_s": self.first_finite_sample_host_s})
                    if self.fault:
                        raise RuntimeError(self.fault)
            except BaseException:
                # Revoke READY before any possibly failing/timed-out join.
                self.transport_ready = False
                self._stop.set()
                self._thread.join(timeout=3)
                if self._thread.is_alive():
                    self.fault = "startup reader did not stop; source state UNKNOWN, USB must remain connected, ownership retained"
                    raise RuntimeError(self.fault) from None
                with self._lock:
                    # Before READY, USB is still attached: safely attempt the
                    # explicit stop/OFF/close cleanup even for bad transport.
                    self.transport_ready = False
                    if self._cleanup_backend():
                        self.closed = True
                        self._close_writers()
                    else:
                        raise RuntimeError(self.fault)
                raise
        return self

    def __enter__(self):
        return self.start()

    def __exit__(self, exc_type, exc, tb):
        if not self.closed:
            self.interrupt("context exit" if exc is None else f"context exception: {exc}")
            if exc is None:
                raise USBReconnectRequired("owner retained: reconnect DUT USB, then session.finish(usb_reconnected=True)")
        return False

    def _require_ready(self):
        if not self.started or self.closed:
            raise RuntimeError("session is not holding PPK2 power")

    def _refresh_receiver(self):
        method = getattr(self.backend, "receiver_snapshot", None)
        snapshot = method() if callable(method) else {}
        if snapshot:
            self.max_observed_backlog_bytes = max(self.max_observed_backlog_bytes, snapshot["max_combined_backlog_bytes"])
            self.max_consumer_delay_s = max(self.max_consumer_delay_s, snapshot["max_consumer_delay_s"])
            if snapshot["fault"] is not None and self.fault is None:
                self.fault = f"raw receiver: {snapshot['fault']['kind']}: {snapshot['fault']['message']}"[:512]
                self._first_sample_event.set()
        if self.fault is not None:
            self.transport_ready = False
        return snapshot

    def begin(self, label, duration_s=None):
        if not isinstance(label, str) or not label.strip() or len(label) > 128 or any(ord(c) < 32 for c in label):
            raise ValueError("label must be 1..128 printable characters")
        if duration_s is not None and (isinstance(duration_s, bool) or not isinstance(duration_s, (int, float)) or not math.isfinite(duration_s) or not 0.01 <= duration_s <= 86400):
            raise ValueError("duration_s must be 0.01..86400 seconds")
        with self._lock:
            self._require_ready()
            receiver = self._refresh_receiver()
            if self.fault:
                raise RuntimeError(f"recording paused: {self.fault}; reconnect USB and finish")
            if self.record is not None:
                raise ValueError("end the active state before beginning another")
            boundary = getattr(self.backend, "receive_boundary", None)
            receiver = boundary() if callable(boundary) else receiver
            if receiver.get("fault") is not None:
                self._refresh_receiver()
                raise RuntimeError(f"recording paused: {self.fault}; owner retained")
            now = receiver.get("boundary_monotonic_s", self.clock())
            window_first_index = max(self.total_samples, receiver["received_bytes"] // 4 - self.decoded_missing - self.total_invalid_samples) if receiver else self.total_samples
            self.record = {
                "label": label, "start": now, "deadline": None if duration_s is None else now + duration_s,
                "stats": CurrentStats(self.capacity), "window_stats": CurrentStats(self.capacity),
                "window_index": 0, "window_start": now,
                "window_first_index": window_first_index,
                "first_sample_index": window_first_index,
                "decoded_missing_start": self.decoded_missing,
                "invalid_start": self.total_invalid_samples,
                "late_start": self.late_unassigned_sample_count,
                "max_consumer_delay_s": 0.0,
            }
            self._emit({"type": "begin", "label": label, "host_offset_s": now - self.origin,
                        "first_valid_received_index": self.total_samples,
                        "window_sample_index_start": window_first_index,
                        "window_sample_index_start_basis": "atomic producer complete-frame prefix minus known invalids; exact when pending/new invalid count is zero",
                        "producer_received_byte_index": receiver.get("received_bytes"),
                        "producer_received_sample_index_estimate": None if not receiver else receiver["received_bytes"] // 4,
                        "queued_prefix_bytes": receiver.get("queued_bytes", 0),
                        "decoded_missing_frame_count_at_begin": self.decoded_missing,
                        "total_invalid_samples_at_begin": self.total_invalid_samples,
                        "late_unassigned_sample_count_at_begin": self.late_unassigned_sample_count,
                        "duration_s": duration_s})
            return self.status()

    def _window(self, end, *, partial=False):
        r = self.record
        result = self._summary(r["window_stats"], max(0.0, end - r["window_start"]))
        window_end_index = r["window_first_index"] + r["window_stats"].count
        self._emit({"type": "window", "label": r["label"], "window_index": r["window_index"],
                    "sample_index_start": r["window_first_index"],
                    "sample_index_end": window_end_index,
                    "time_s": r["window_first_index"] / SAMPLE_RATE_HZ,
                    "nominal_sample_end_time_s": window_end_index / SAMPLE_RATE_HZ,
                    "host_start_offset_s": r["window_start"] - self.origin,
                    "host_end_offset_s": end - self.origin,
                    "max_observed_backlog_bytes": self.max_observed_backlog_bytes,
                    "max_consumer_delay_s": self.max_consumer_delay_s,
                    "last_consumer_delay_s": self.last_consumer_delay_s,
                    "late_unassigned_sample_count": self.late_unassigned_sample_count,
                    "label_late_unassigned_sample_count": self.late_unassigned_sample_count - r["late_start"],
                    "label_max_consumer_delay_s": r["max_consumer_delay_s"],
                    "elapsed_host_s": end - self.origin,
                    "sample_vs_host_drift_s": self.total_samples / SAMPLE_RATE_HZ - (end - self.origin),
                    "partial": partial, **result})
        r["window_stats"] = CurrentStats(self.capacity)
        r["window_first_index"] = max(window_end_index, self.total_samples)
        r["window_index"] += 1
        r["window_start"] = end

    def _tick(self, now):
        if self.record is None:
            return
        r = self.record
        boundary = min(now, r["deadline"]) if r["deadline"] is not None else now
        windows = 0
        while r["window_start"] + self.window_s <= boundary:
            self._window(r["window_start"] + self.window_s)
            windows += 1
            if windows >= 100:
                # Long host suspend: represent skipped EMPTY windows in O(1),
                # instead of allocating/emitting hours of catch-up work.
                missing = int((boundary - r["window_start"]) / self.window_s)
                if missing:
                    self._emit({"type": "empty_window_gap", "label": r["label"],
                                "first_window_index": r["window_index"], "window_count": missing,
                                "window_s": self.window_s})
                    r["window_index"] += missing
                    r["window_start"] += missing * self.window_s
                break
        if r["deadline"] is not None and now >= r["deadline"]:
            self._end(r["deadline"], "duration_elapsed")

    def _ingest(self, samples, frame_count, arrival):
        with self._lock:
            self._refresh_receiver()
            self.last_consumer_delay_s = max(0.0, self.clock() - arrival)
            self.max_consumer_delay_s = max(self.max_consumer_delay_s, self.last_consumer_delay_s)
            self.last_receive_host_s = arrival - self.origin
            self._tick(arrival)
            if self.fault:
                return
            self.decoded_missing += max(0, frame_count - len(samples))
            record = self.record
            if record is not None and arrival >= record["start"]:
                record["max_consumer_delay_s"] = max(record["max_consumer_delay_s"], self.last_consumer_delay_s)
                valid = record["stats"].add(samples)
                if arrival >= record["window_start"]:
                    record["window_stats"].add(samples)
                else:
                    # A control command already emitted this boundary. Never
                    # pretend delayed arrival data belong to a later window.
                    self.late_unassigned_sample_count += len(valid)
            else:
                # A queued pre-begin prefix remains in raw/global counts, but
                # does NOT contaminate the new label's receive-side statistics.
                valid = [float(v) for v in samples if isinstance(v, (int, float)) and not isinstance(v, bool) and math.isfinite(v)]
            first_index = self.total_samples
            self.total_invalid_samples += len(samples) - len(valid)
            self.total_samples += len(valid)
        if self.raw_writer is not None:
            self.raw_writer.write(first_index, valid)
        with self._lock:
            self._refresh_receiver()
            if valid and not self.fault:
                self.last_data_host_s = arrival - self.origin
                if self.first_finite_sample_host_s is None:
                    self.first_finite_sample_host_s = self.last_data_host_s
                self._first_sample_event.set()

    def pump(self):
        """One bounded queue drain; decode by producer HOST arrival segment.

        The independent raw receiver never performs this conversion/statistics
        work. Missing samples still have no identifiable hardware timestamps.
        Consumer delay is observable, and no receive time is invented from it.
        """
        with self._lock:
            self._require_ready()
        try:
            data = self.backend.read()
            segments = getattr(self.backend, "last_arrival_segments", [])
            with self._lock:
                receiver = self._refresh_receiver()
                self.max_observed_backlog_bytes = max(self.max_observed_backlog_bytes, getattr(self.backend, "last_backlog_bytes", len(data)))
                if not data:
                    self.empty_reads += 1
                    boundary = getattr(self.backend, "last_empty_receive_boundary_s", None)
                    if boundary is not None:
                        self._tick(boundary)
                    elif not receiver:
                        self._tick(self.clock())
                    # If a producer commit raced the empty dequeue, retry
                    # queue draining rather than finalize its arrival window.
                    return
                if self.fault:
                    return
            if not segments:
                # Pure fake/cooperative backends retain their old one-batch
                # contract. The real backend ALWAYS supplies producer stamps.
                segments = [{"byte_offset": 0, "byte_count": len(data),
                             "arrival_monotonic_s": self.clock()}]
            for segment in segments:
                with self._lock:
                    self._refresh_receiver()
                    if self.fault:
                        return
                start = segment["byte_offset"]
                chunk = data[start:start + segment["byte_count"]]
                samples = self.backend.decode(chunk)
                frame_count = getattr(self.backend, "last_frame_count", len(samples))
                self._ingest(samples, frame_count, segment["arrival_monotonic_s"])
        except Exception as exc:
            with self._lock:
                self.read_errors += 1
                self.fault = self.fault or f"collector: {type(exc).__name__}: {exc}"[:512]
                self.transport_ready = False
                self._first_sample_event.set()
                if self.record is not None:
                    self._end(self.clock(), "collector_error")
                # Receiver keeps draining; no OFF, serial close, or restart.

    def _run(self):
        while not self._stop.is_set():
            empty_before = self.empty_reads
            self.pump()
            if self.fault:
                self._stop.wait(0.02)
            elif self.empty_reads != empty_before:
                wait = getattr(self.backend, "wait_for_data", None)
                if callable(wait):
                    wait(0.05)  # condition wake from producer, no file polling
                else:
                    self._stop.wait(0.0002)

    def _summary(self, stats, duration_s):
        result = stats.summary(duration_s, self.voltage_mv, SAMPLE_RATE_HZ)
        result["energy_voltage_basis"] = "configured_source_voltage"
        if self.dut_voltage_mv is not None:
            result["measured_dut_voltage_mv"] = self.dut_voltage_mv
            result["sampled_dut_energy_uJ"] = result["sampled_charge_uC"] * self.dut_voltage_mv / 1000
            result["dut_energy_assumption"] = "independently measured constant DUT voltage; isolator loss not inferred"
        return result

    def _end(self, now, reason):
        r = self.record
        if r is None:
            raise ValueError("no active state")
        if now > r["window_start"] or r["window_stats"].count:
            self._window(now, partial=True)
        result = {
            "type": "end", "label": r["label"], "reason": reason,
            "host_start_offset_s": r["start"] - self.origin,
            "first_valid_received_index": r["first_sample_index"],
            "next_valid_received_index": r["first_sample_index"] + r["stats"].count,
            "decoded_missing_frame_count": self.decoded_missing - r["decoded_missing_start"],
            "invalid_sample_count": self.total_invalid_samples - r["invalid_start"],
            "late_unassigned_sample_count": self.late_unassigned_sample_count,
            "label_late_unassigned_sample_count": self.late_unassigned_sample_count - r["late_start"],
            "max_observed_backlog_bytes": self.max_observed_backlog_bytes,
            "max_consumer_delay_s": self.max_consumer_delay_s,
            "label_max_consumer_delay_s": r["max_consumer_delay_s"],
            **self._summary(r["stats"], max(0.0, now - r["start"])),
        }
        self._emit(result)
        self.last_record = result
        self.record = None
        return result

    def end(self):
        with self._lock:
            self._require_ready()
            self._tick(self.clock())
            if self.record is None:
                raise ValueError("no active state (a timed state may have already ended; use status)")
            return self._end(self.clock(), "command")

    def interrupt(self, reason="SIGINT"):
        """End the window, but retain source power, serial, and sample drain."""
        with self._lock:
            self._require_ready()
            if self.record is not None:
                self._end(self.clock(), reason)
            self._emit({"type": "interrupt", "reason": reason,
                        "message": "owner retained; reconnect DUT USB and confirm finish"})

    def status(self):
        with self._lock:
            receiver = self._refresh_receiver()
            return {
                "holding": self.started and not self.closed,
                "owner_port": self.port, "voltage_mv": self.voltage_mv,
                "active_label": None if self.record is None else self.record["label"],
                "total_valid_samples": self.total_samples,
                "total_invalid_samples": self.total_invalid_samples,
                "host_monotonic_origin_s": self.origin,
                "elapsed_host_s": None if self.origin is None else self.clock() - self.origin,
                "nominal_sample_elapsed_s": self.total_samples / SAMPLE_RATE_HZ,
                "sample_vs_host_drift_s": None if self.origin is None else self.total_samples / SAMPLE_RATE_HZ - (self.clock() - self.origin),
                "max_observed_backlog_bytes": self.max_observed_backlog_bytes,
                "max_consumer_delay_s": self.max_consumer_delay_s,
                "last_consumer_delay_s": self.last_consumer_delay_s,
                "late_unassigned_sample_count": self.late_unassigned_sample_count,
                "receiver": receiver,
                "decoded_missing_frame_count": self.decoded_missing,
                "empty_reads": self.empty_reads, "read_errors": self.read_errors,
                "last_data_host_offset_s": self.last_data_host_s,
                "last_receive_host_offset_s": self.last_receive_host_s,
                "first_finite_sample_host_offset_s": self.first_finite_sample_host_s,
                "transport_ready": self.transport_ready and self.started and not self.closed and self.fault is None,
                "fault": self.fault, "last_record": self.last_record,
                "raw_truncated": bool(self.raw_writer and self.raw_writer.truncated),
                "usb_reconnected_confirmed": self.confirmed,
                "cleanup_errors": list(self.cleanup_errors),
            }

    def _cleanup_backend(self):
        if self.backend is None:
            return True
        stop_receiver = getattr(self.backend, "stop_receiver", None)
        if callable(stop_receiver):
            try:
                stop_receiver()
            except Exception as exc:
                self.cleanup_errors.append({"operation": "fast_receiver_stop", "error": f"{type(exc).__name__}: {exc}"[:512],
                                            "output_state": "UNKNOWN; owner/control retained"})
                self.fault = "fast raw receiver did not join; serial ownership and control retained"
                self.started = True
                if self.origin is None:
                    self.origin = self.clock()
                return False  # NEVER close/lose lease under an active reader
        actions = []
        for error in getattr(self.backend, "cleanup_errors", []):
            if error not in self.cleanup_errors:
                self.cleanup_errors.append(error)
        if self._measuring_attempted:
            actions.append(("stop", self.backend.stop_measuring))
        if self._power_attempted or getattr(self.backend, "source_configuration_attempted", False):
            actions.append(("power_off", lambda: self.backend.power(False)))
        actions.append(("close", self.backend.close))
        for name, action in actions:
            try:
                action()
            except Exception as exc:
                self.cleanup_errors.append(f"{name}: {type(exc).__name__}: {exc}"[:512])
        return True

    def _close_writers(self):
        for writer in (self.raw_writer, self.writer):
            if writer is not None:
                try:
                    writer.close()
                except Exception as exc:
                    self.cleanup_errors.append(f"output close: {exc}"[:512])

    def finish(self, *, usb_reconnected):
        if usb_reconnected is not True:
            raise USBReconnectRequired("reconnect DUT USB before finish; true confirmation required")
        with self._lock:
            if self.closed:
                return self.status()
            self._require_ready()
            self.confirmed = True
            self.transport_ready = False
            if self.record is not None:
                self._end(self.clock(), "finish")
            self._stop.set()
        if self._thread is not None:
            # Adapter reads/writes are finite; never close under an active read.
            self._thread.join(timeout=3)
            if self._thread.is_alive():
                with self._lock:
                    self.fault = self.fault or "consumer reader did not stop; source state UNKNOWN, owner/control retained"
                    self.cleanup_errors.append({"operation": "consumer_reader_stop", "error": "reader did not join",
                                                "output_state": "UNKNOWN; owner/control retained"})
                raise RuntimeError(self.fault)
        with self._lock:
            if not self._cleanup_backend():
                raise RuntimeError(self.fault)
            self.closed = True
            self._emit({"type": "finish", "usb_reconnected_confirmed": True,
                        "fault": self.fault, "cleanup_errors": self.cleanup_errors})
            self._close_writers()
            return self.status()
