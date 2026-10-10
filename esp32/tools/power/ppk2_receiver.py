# Copyright (c) Meta Platforms, Inc. and affiliates.
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy at http://www.apache.org/licenses/LICENSE-2.0
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""Original bounded raw receiver, independent of decode/stats/file writes.

The callback reads the existing owner's serial connection; this class never
opens, configures, powers, restarts, or closes a device. Arrival times are HOST
receive observations, NOT instrument sample timestamps. After overflow/error,
queued delivery is invalidated explicitly, but raw draining continues until the
owner requests stop. No upstream PPK2_MP code or silent-drop policy is used.
"""

import threading
import time
from collections import deque
from dataclasses import dataclass

MAX_BATCH_BYTES = 65_536
DEFAULT_QUEUE_BYTES = 1024 * 1024
DEFAULT_MIN_QUEUE_CHUNKS = 4096
MAX_QUEUE_CHUNKS = 16 * 1024


def validate_queue_bytes(value):
    if type(value) is not int or not 1024 <= value <= 16 * 1024 * 1024:
        raise ValueError("raw receiver byte cap must be an integer in [1024, 16777216]")
    return value


class TransportFault(RuntimeError):
    def __init__(self, detail):
        self.detail = dict(detail)
        super().__init__(f"raw receiver {detail['kind']}: {detail['message']}; capture INVALID, source owner retained")


class ReceiverStopTimeout(RuntimeError):
    pass


@dataclass(frozen=True)
class RawChunk:
    data: bytes
    arrival_monotonic_s: float
    received_byte_index: int
    os_backlog_bytes: int


class FastRawReceiver:
    def __init__(self, read_chunk, *, max_bytes=DEFAULT_QUEUE_BYTES,
                 max_chunks=None, clock=time.monotonic):
        validate_queue_bytes(max_bytes)
        if max_chunks is None:
            # Keep tiny-packet metadata bounded, without imposing the old 4 MiB
            # effective limit on a 16 MiB queue receiving approximately 1 KiB.
            max_chunks = max(DEFAULT_MIN_QUEUE_CHUNKS, (max_bytes + 1023) // 1024)
        if type(max_chunks) is not int or not 1 <= max_chunks <= MAX_QUEUE_CHUNKS:
            raise ValueError("raw receiver entry cap must be 1..16384")
        self.read_chunk = read_chunk  # returns (bounded bytes, OS backlog snapshot)
        self.max_bytes = max_bytes
        self.max_chunks = max_chunks
        self.clock = clock
        self._queue = deque()
        self._queued_bytes = 0
        self._condition = threading.Condition()
        self._stop = threading.Event()
        self._thread = None
        self._started = False
        self._start_completed = False
        self._joined = False
        self.fault = None
        self.received_bytes = 0
        self.consumed_bytes = 0
        self.discarded_bytes = 0
        self.invalidated_queue_bytes = 0
        self.read_errors = 0
        self.overflow_count = 0
        self.max_queue_bytes = 0
        self.max_os_backlog_bytes = 0
        self.max_combined_backlog_bytes = 0
        self.last_arrival_monotonic_s = None
        self.started_monotonic_s = None
        self.max_consumer_delay_s = 0.0

    @property
    def running(self):
        return self._thread is not None and self._thread.is_alive()

    @property
    def join_required(self):
        return self._thread is not None and not self._joined

    @staticmethod
    def _proven_unstarted(thread):
        # CPython publishes a launch-pending thread in _limbo before spawning
        # it, and removes that entry on spawn failure. ident/_started alone
        # cannot distinguish spawn failure from a native thread still pending.
        # An unfamiliar runtime is uncertain: retain ownership, never guess.
        try:
            with threading._active_limbo_lock:
                return thread.ident is None and not thread._started.is_set() and thread not in threading._limbo
        except AttributeError:
            return False

    def start(self):
        with self._condition:
            if self._started:
                raise RuntimeError("raw receiver cannot be restarted")
            self._started = True
            self.started_monotonic_s = self.clock()
            candidate = threading.Thread(target=self._run, name="ppk2-fast-raw", daemon=False)
            self._thread = candidate
            try:
                candidate.start()
                self._start_completed = True
            except BaseException:
                if self._proven_unstarted(candidate):
                    # A failed spawn has no reader to join. Remove ONLY that
                    # proven-unstarted handle so USB-present cleanup can OFF.
                    self._thread = None
                raise
        return self

    def _fault(self, kind, message):
        # Called with the short queue lock held; retain the first failure.
        if self.fault is None:
            self.invalidated_queue_bytes = self._queued_bytes
            self.fault = {"kind": kind, "message": message[:512],
                          "arrival_monotonic_s": self.clock(),
                          "received_bytes": self.received_bytes,
                          "queued_bytes": self._queued_bytes}
        self._condition.notify_all()

    def _run(self):
        while not self._stop.is_set():
            try:
                data, os_backlog = self.read_chunk()
                if not isinstance(data, bytes) or len(data) > MAX_BATCH_BYTES:
                    raise ValueError("raw read callback must return bytes <=65536")
                if type(os_backlog) is not int or os_backlog < 0:
                    raise ValueError("OS backlog snapshot must be nonnegative integer bytes")
                with self._condition:
                    # Stamp queue commit under the same lock as begin's byte
                    # boundary: a post-begin commit cannot be excluded by an
                    # earlier timestamp obtained before acquiring this lock.
                    arrival = self.clock()
                    self.max_os_backlog_bytes = max(self.max_os_backlog_bytes, os_backlog)
                    # OS snapshot was taken BEFORE this read, so adding the
                    # prior queue avoids double-counting the just-read chunk.
                    self.max_combined_backlog_bytes = max(
                        self.max_combined_backlog_bytes, self._queued_bytes + os_backlog)
                    if data:
                        first_index = self.received_bytes
                        self.received_bytes += len(data)
                        self.last_arrival_monotonic_s = arrival
                        if self.fault is not None:
                            self.discarded_bytes += len(data)
                            self._condition.notify_all()
                        elif self._queued_bytes + len(data) > self.max_bytes or len(self._queue) >= self.max_chunks:
                            self.overflow_count += 1
                            self.discarded_bytes += len(data)
                            self._fault("queue_overflow", "raw byte/entry budget exhausted; no valid continuation")
                        else:
                            self._queue.append(RawChunk(data, arrival, first_index, os_backlog))
                            self._queued_bytes += len(data)
                            self.max_queue_bytes = max(self.max_queue_bytes, self._queued_bytes)
                            self._condition.notify()
                if not data:
                    self._stop.wait(0.0002)
            except Exception as exc:
                with self._condition:
                    self.read_errors += 1
                    self._fault("read_error", f"{type(exc).__name__}: {exc}")
                # Continue attempting to drain without busy-spinning. Source
                # power and the exclusive serial owner are completely untouched.
                self._stop.wait(0.005)

    def read_batch(self, max_bytes=MAX_BATCH_BYTES):
        """Nonblocking bounded dequeue with per-segment arrival provenance.

        On any transport fault, raise even if an earlier prefix remains queued;
        never silently return a supposedly valid continuation after a gap.
        """
        if type(max_bytes) is not int or not 1 <= max_bytes <= MAX_BATCH_BYTES:
            raise ValueError("consumer read bound must be 1..65536 bytes")
        with self._condition:
            if self.fault is not None:
                raise TransportFault(self.fault)
            output = bytearray()
            segments = []
            while self._queue and len(output) < max_bytes:
                chunk = self._queue.popleft()
                take = min(len(chunk.data), max_bytes - len(output))
                segments.append({"byte_offset": len(output), "byte_count": take,
                                 "arrival_monotonic_s": chunk.arrival_monotonic_s,
                                 "received_byte_index": chunk.received_byte_index,
                                 "os_backlog_bytes": chunk.os_backlog_bytes})
                output.extend(chunk.data[:take])
                self._queued_bytes -= take
                self.consumed_bytes += take
                self.max_consumer_delay_s = max(self.max_consumer_delay_s,
                                               max(0.0, self.clock() - chunk.arrival_monotonic_s))
                if take < len(chunk.data):
                    self._queue.appendleft(RawChunk(chunk.data[take:], chunk.arrival_monotonic_s,
                                                   chunk.received_byte_index + take, chunk.os_backlog_bytes))
            return bytes(output), segments

    def wait_for_data(self, timeout_s=0.05):
        with self._condition:
            self._condition.wait_for(lambda: self._queue or self.fault is not None or self._stop.is_set(), timeout_s)

    def snapshot(self):
        with self._condition:
            return {"running": self.running, "start_completed": self._start_completed,
                    "join_required": self.join_required, "capacity_bytes": self.max_bytes,
                    "capacity_chunks": self.max_chunks, "queued_bytes": self._queued_bytes,
                    "queued_chunks": len(self._queue), "received_bytes": self.received_bytes,
                    "consumed_bytes": self.consumed_bytes, "discarded_bytes": self.discarded_bytes,
                    "invalidated_queue_bytes": self.invalidated_queue_bytes,
                    "overflow_count": self.overflow_count, "read_errors": self.read_errors,
                    "max_queue_bytes": self.max_queue_bytes,
                    "max_os_backlog_bytes": self.max_os_backlog_bytes,
                    "max_combined_backlog_bytes": self.max_combined_backlog_bytes,
                    "last_arrival_monotonic_s": self.last_arrival_monotonic_s,
                    "max_consumer_delay_s": self.max_consumer_delay_s,
                    "fault": None if self.fault is None else dict(self.fault)}

    def boundary_snapshot(self):
        """Atomic receive-prefix/host boundary, ordered with producer commits."""
        with self._condition:
            result = self.snapshot()
            result["boundary_monotonic_s"] = self.clock()
            return result

    def stop(self, timeout_s=3):
        self._stop.set()
        with self._condition:
            self._condition.notify_all()
        if self._thread is not None:
            candidate = self._thread
            deadline = time.monotonic() + timeout_s
            if not candidate._started.wait(max(0.0, timeout_s)):
                if self._proven_unstarted(candidate):
                    self._thread = None
                    return
                raise ReceiverStopTimeout("raw thread launch unresolved; serial and owner/control must be retained")
            candidate.join(max(0.0, deadline - time.monotonic()))
            if candidate.is_alive():
                raise ReceiverStopTimeout("fast raw reader did not stop; serial must remain open and owner/control retained")
            self._joined = True
