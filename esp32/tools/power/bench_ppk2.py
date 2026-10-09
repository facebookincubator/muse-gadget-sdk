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

"""Synthetic saturation benchmark: real pinned decoder, zero device access.

Feeds fake serial memory at maximum CPU speed, writes real bounded temp files,
measures both reservoirs and JSON/raw pipeline. It proves throughput headroom,
NOT physical USB reliability, sample timing, calibration, or safe wiring.
"""

import argparse
import importlib.metadata
import json
import struct
import sys
import tempfile
import time
from pathlib import Path
from unittest.mock import patch

if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
    from power.ppk2_backend import API_VERSION, PPK2Backend, SAMPLE_RATE_HZ
    from power.ppk2_session import PPK2Session, SafetyAcknowledgment
    from power.ppk2_stats import JSONLWriter, RawWriter
else:
    from .ppk2_backend import API_VERSION, PPK2Backend, SAMPLE_RATE_HZ
    from .ppk2_session import PPK2Session, SafetyAcknowledgment
    from .ppk2_stats import JSONLWriter, RawWriter


def benchmark(seconds=5, raw=True):
    if importlib.metadata.version("ppk2-api") != API_VERSION:
        raise RuntimeError(f"benchmark requires ppk2-api=={API_VERSION}")
    from ppk2_api.ppk2_api import PPK2_API
    # ADC/range encoded fake frames. No USB enumeration, leases, device opens,
    # voltage configuration on real hardware, or calibration claims.
    batch = struct.pack("<I", 5000 | (3 << 14)) * 10_000

    class FakeSerial:
        baudrate = 9600
        enabled = False
        @property
        def in_waiting(self):
            return len(batch) if self.enabled else 0
        def read(self, size):
            return batch[:size]
        def write(self, payload):
            return len(payload)
        def close(self):
            pass

    class SyntheticBackend(PPK2Backend):
        def __init__(self, port):
            with patch("serial.Serial", return_value=FakeSerial()):
                self.api = PPK2_API("synthetic-memory-only")
            self.remainder = b""
            self.last_frame_count = 0
            self.descriptor = {"port": "synthetic-memory-only"}
        def configure(self, voltage_mv):
            self.api.use_source_meter()
            self.api.set_source_voltage(voltage_mv)
        def start_measuring(self):
            self.api.start_measuring()
            self.api.ser.enabled = True
        def stop_measuring(self):
            self.api.stop_measuring()
            self.api.ser.enabled = False
        def close(self):
            self.api.ser.close()

    with tempfile.TemporaryDirectory(prefix="ppk2-synthetic-") as directory:
        output = JSONLWriter(Path(directory) / "aggregate.jsonl")
        raw_output = RawWriter(Path(directory) / "raw.bin") if raw else None
        session = PPK2Session(port="synthetic-memory-only", voltage_mv=3872,
                              acknowledgment=SafetyAcknowledgment(True, True, True, True),
                              backend_factory=SyntheticBackend, writer=output, raw_writer=raw_output)
        session.start(background=False)
        session.begin("synthetic")
        chunks = int(seconds * SAMPLE_RATE_HZ / 10000)
        start = time.perf_counter()
        cpu_start = time.process_time()
        try:
            for _ in range(chunks):
                session.pump()
            session.end()
        finally:
            session.finish(usb_reconnected=True)
        elapsed = time.perf_counter() - start
        cpu_elapsed = time.process_time() - cpu_start
        return {
            "synthetic_only": True, "hardware_access": False,
            "ppk2_api_version": API_VERSION, "raw_enabled": raw,
            "sample_count": session.total_samples,
            "elapsed_s": elapsed, "samples_per_s": session.total_samples / elapsed,
            "elapsed_cpu_s": cpu_elapsed,
            "cpu_samples_per_s": session.total_samples / cpu_elapsed if cpu_elapsed else None,
            "realtime_headroom_ratio": session.total_samples / elapsed / SAMPLE_RATE_HZ,
            "aggregate_bytes": output.bytes_written,
            "raw_bytes": 0 if raw_output is None else raw_output.bytes_written,
            "fault": session.fault,
        }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--seconds", type=int, default=5, choices=range(1, 31))
    parser.add_argument("--no-raw", action="store_true")
    args = parser.parse_args()
    result = benchmark(args.seconds, not args.no_raw)
    print(json.dumps(result))
    return 1 if result["fault"] or result["realtime_headroom_ratio"] < 1 else 0


if __name__ == "__main__":
    raise SystemExit(main())
