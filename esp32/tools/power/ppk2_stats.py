# Copyright (c) Meta Platforms, Inc. and affiliates.
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy at http://www.apache.org/licenses/LICENSE-2.0
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""Bounded aggregate statistics and streaming files (standard library only)."""

import json
import math
import random
import struct


class BudgetExceeded(RuntimeError):
    pass


class JSONLWriter:
    def __init__(self, path, max_bytes=16 * 1024 * 1024):
        if type(max_bytes) is not int or not 4096 <= max_bytes <= 512 * 1024 * 1024:
            raise ValueError("aggregate file budget must be 4096..536870912 bytes")
        self.file = open(path, "x", encoding="utf-8")
        self.max_bytes = max_bytes
        self.bytes_written = 0

    def write(self, record):
        line = json.dumps(record, allow_nan=False, separators=(",", ":")) + "\n"
        size = len(line.encode("utf-8"))
        if self.bytes_written + size > self.max_bytes:
            raise BudgetExceeded("aggregate output budget reached; recording paused, owner retained")
        self.file.write(line)
        self.file.flush()
        self.bytes_written += size

    def close(self):
        self.file.close()


class RawWriter:
    """Little-endian <Qf: valid received sample ordinal, float32 current_uA.

    Ordinals are NOT hardware timestamps, and do not locate USB sample loss.
    The header/schema in the aggregate JSONL describes this optional file.
    """
    RECORD = struct.Struct("<Qf")

    def __init__(self, path, max_bytes=64 * 1024 * 1024):
        if type(max_bytes) is not int or not 12 <= max_bytes <= 512 * 1024 * 1024:
            raise ValueError("raw file budget must be 12..536870912 bytes")
        self.file = open(path, "xb")
        self.max_bytes = max_bytes
        self.bytes_written = 0
        self.truncated = False

    def write(self, first_index, samples):
        capacity = (self.max_bytes - self.bytes_written) // self.RECORD.size
        keep = min(capacity, len(samples))
        # Reader chunks are bounded, so this bytearray is bounded too.
        payload = bytearray()
        for index, sample in enumerate(samples[:keep], first_index):
            payload.extend(self.RECORD.pack(index, sample))
        self.file.write(payload)
        self.file.flush()
        self.bytes_written += len(payload)
        if keep < len(samples):
            self.truncated = True

    def close(self):
        self.file.close()


class CurrentStats:
    """Exact count/mean/extrema/integrals, bounded uniform percentile reservoir.

    Percentiles use linear interpolation at (n-1)*p. They are exact up to
    capacity samples; after that they are estimates, explicitly marked.
    All arithmetic is in uA, seconds, mV, uC, mAh, and uJ (never inferred).
    """
    def __init__(self, capacity=4096):
        if type(capacity) is not int or not 16 <= capacity <= 65536:
            raise ValueError("percentile reservoir capacity must be 16..65536")
        self.capacity = capacity
        self.reservoir = []
        self.rng = random.Random(0)
        self.count = 0
        self.total_uA = 0.0
        self.minimum = None
        self.maximum = None
        self.invalid = 0

    def add(self, samples):
        valid = []
        for value in samples:
            if isinstance(value, (int, float)) and not isinstance(value, bool) and math.isfinite(value):
                valid.append(float(value))
            else:
                self.invalid += 1
        if not valid:
            return []
        self.total_uA += math.fsum(valid)
        lo, hi = min(valid), max(valid)
        self.minimum = lo if self.minimum is None else min(self.minimum, lo)
        self.maximum = hi if self.maximum is None else max(self.maximum, hi)
        for value in valid:
            self.count += 1
            if len(self.reservoir) < self.capacity:
                self.reservoir.append(value)
            else:
                index = self.rng.randrange(self.count)
                if index < self.capacity:
                    self.reservoir[index] = value
        return valid

    def summary(self, duration_s, voltage_mv, sample_rate_hz):
        ordered = sorted(self.reservoir)

        def percentile(p):
            if not ordered:
                return None
            position = (len(ordered) - 1) * p
            left = math.floor(position)
            right = math.ceil(position)
            return ordered[left] + (ordered[right] - ordered[left]) * (position - left)

        sampled_duration = self.count / sample_rate_hz
        charge = self.total_uA / sample_rate_hz
        expected = duration_s * sample_rate_hz
        return {
            "sample_count": self.count,
            "invalid_sample_count": self.invalid,
            "host_duration_s": duration_s,
            "nominal_sample_duration_s": sampled_duration,
            "expected_sample_count_estimate": expected,
            "coverage_ratio_estimate": self.count / expected if expected > 0 else None,
            "missing_sample_count_estimate": max(0.0, expected - self.count),
            "mean_uA": self.total_uA / self.count if self.count else None,
            "min_uA": self.minimum, "max_uA": self.maximum,
            "p50_uA": percentile(0.50), "p90_uA": percentile(0.90),
            "p95_uA": percentile(0.95), "p99_uA": percentile(0.99),
            "percentile_method": "exact_linear" if self.count <= self.capacity else "uniform_reservoir_linear_estimate",
            "percentile_sample_count": len(ordered),
            "sampled_charge_uC": charge,
            "sampled_charge_mAh": charge / 3_600_000,
            "sampled_energy_uJ": charge * voltage_mv / 1000,
            "sampled_energy_mWh": charge * voltage_mv / 3_600_000_000,
        }
