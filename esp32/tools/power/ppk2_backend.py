# Copyright (c) Meta Platforms, Inc. and affiliates.
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy at http://www.apache.org/licenses/LICENSE-2.0
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""Checked adapter over ppk2-api 0.9.2; no connection is made at import time.

Do not use PPK2_MP: it silently discards queued samples and restarts acquisition.
The API's current values are microamps, at a nominal 100,000 samples/second.
Only this adapter knows the upstream private hooks (tested against the pin).
"""

import hashlib
import importlib.metadata
import os
import tempfile
import time
from pathlib import Path

from .ppk2_receiver import DEFAULT_QUEUE_BYTES, FastRawReceiver, validate_queue_bytes

API_VERSION = "0.9.2"
SERIAL_VERSION = "3.5"
SAMPLE_RATE_HZ = 100_000
MAX_READ_BYTES = 65_536
MAX_METADATA_BYTES = 8192
METADATA_TIMEOUT_S = 2.0


def validate_voltage(voltage_mv):
    if type(voltage_mv) is not int or not 800 <= voltage_mv <= 5000:
        raise ValueError("source voltage must be an explicit integer mV in [800, 5000]")
    return voltage_mv


def port_descriptions(ports=None):
    """Descriptor-only discovery; never probes or changes any PPK2 output.

    Both CDC interfaces are returned. Choose the control/measurement interface
    explicitly; names/order are not portable. Wrong interfaces fail metadata
    validation BEFORE source configuration or power ON.
    """
    if ports is None:
        from serial.tools.list_ports import comports
        ports = comports()
    return [
        {"port": p.device, "product": p.product, "description": p.description,
         "serial_number": p.serial_number, "location": p.location,
         "interface": p.interface, "vid": p.vid, "pid": p.pid}
        for p in ports
        if p.product == "PPK2" or (
            p.vid == 0x1915 and (p.description or "").startswith("nRF Connect USB CDC ACM")
        )
    ]


class DeviceLease:
    """In addition to pyserial exclusive, serialize tools across CDC aliases.

    flock is advisory: also close Nordic's GUI/other tools yourself. A USB
    serial number groups both PPK2 interfaces. The fallback is location/port;
    unknown descriptor identities cannot prove cross-interface exclusion.
    """
    def __init__(self, descriptor):
        import fcntl
        identity = descriptor.get("serial_number") or descriptor.get("location") or os.path.realpath(descriptor["port"])
        key = hashlib.sha256(identity.encode()).hexdigest()[:24]
        path = Path(tempfile.gettempdir()) / f"muse-ppk2-{os.getuid()}-{key}.lock"
        self.fd = os.open(path, os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW, 0o600)
        try:
            fcntl.flock(self.fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BaseException:
            os.close(self.fd)
            self.fd = None
            raise RuntimeError("another profiler owns this PPK2; do not kill its keeper") from None

    def close(self):
        if self.fd is not None:
            os.close(self.fd)
            self.fd = None


class PPK2Backend:
    """Exactly one serial open; bounded reads and strict write/metadata errors."""
    def __init__(self, port, *, receiver_max_bytes=DEFAULT_QUEUE_BYTES):
        validate_queue_bytes(receiver_max_bytes)
        self.receiver_max_bytes = receiver_max_bytes
        self.receiver = None
        self.last_arrival_segments = []
        self._acquisition_started = False
        for package, expected in (("ppk2-api", API_VERSION), ("pyserial", SERIAL_VERSION)):
            if importlib.metadata.version(package) != expected:
                raise RuntimeError(f"install the pinned dependency: {package}=={expected}")
        from ppk2_api.ppk2_api import PPK2_API

        class CheckedAPI(PPK2_API):
            # Upstream logs and swallows write failures. Never report success
            # after a failed/short voltage, mode, ON, OFF, or stop command.
            def _write_serial(self, cmd_tuple):
                payload = self._pack_struct(cmd_tuple)
                if self.ser.write(payload) != len(payload):
                    raise OSError("short PPK2 serial command write")

            def _read_metadata(self):
                # 0.9.2 discards earlier CDC chunks and returns only the chunk
                # containing END. Real devices can split HW/calibration from
                # that final chunk; accumulate the complete bounded response.
                deadline = time.monotonic() + METADATA_TIMEOUT_S
                received = bytearray()
                while time.monotonic() < deadline:
                    available = self.ser.in_waiting
                    if available:
                        chunk = self.ser.read(min(available, MAX_METADATA_BYTES + 1 - len(received)))
                        received.extend(chunk)
                        if len(received) > MAX_METADATA_BYTES:
                            raise ValueError("PPK2 metadata exceeds bounded response size")
                        if b"END" in bytes(received).splitlines():
                            return received.decode("utf-8", errors="strict")
                    time.sleep(0.01)
                raise TimeoutError("PPK2 complete metadata terminator not received before deadline")

        descriptors = port_descriptions()
        matches = [d for d in descriptors if d["port"] == port]
        if len(matches) != 1:
            raise ValueError("--port must match one PPK2 descriptor; use discover, never probe all ports")
        self.descriptor = matches[0]
        self.lease = DeviceLease(self.descriptor)
        self.api = None
        self.cleanup_errors = []
        self.source_configuration_attempted = False
        self.remainder = b""
        self.last_frame_count = 0
        try:
            # Reads must not wait for a stale in_waiting snapshot to fill:
            # a 200 ms read timeout can overrun the device's tiny CDC FIFO.
            # Metadata has its own bounded accumulating deadline above.
            self.api = CheckedAPI(port, timeout=0, write_timeout=0.5, exclusive=True)
        except BaseException:
            self.lease.close()
            raise

    def stop_receiver(self):
        receiver = getattr(self, "receiver", None)
        if receiver is not None:
            receiver.stop()

    def stop_measuring(self):
        # Called only during bounded initial setup or final owner cleanup.
        # Join the independent reader BEFORE serial stop/close; no restarts.
        self.stop_receiver()
        self.api.stop_measuring()

    def _read_direct(self):
        backlog = self.api.ser.in_waiting
        data = self.api.ser.read(min(backlog, MAX_READ_BYTES))
        return data, backlog

    def read(self):
        receiver = getattr(self, "receiver", None)
        self.last_arrival_segments = []
        if receiver is None:
            # Stale drain and metadata remain direct, BEFORE initial ON/start.
            data, self.last_backlog_bytes = self._read_direct()
            return data
        data, self.last_arrival_segments = receiver.read_batch(MAX_READ_BYTES)
        snapshot = receiver.boundary_snapshot()
        self.last_backlog_bytes = snapshot["max_combined_backlog_bytes"]
        self.last_empty_receive_boundary_s = snapshot["boundary_monotonic_s"] if not data and snapshot["queued_bytes"] == 0 else None
        return data

    def receiver_snapshot(self):
        receiver = getattr(self, "receiver", None)
        return {} if receiver is None else receiver.snapshot()

    def receive_boundary(self):
        receiver = getattr(self, "receiver", None)
        return {} if receiver is None else receiver.boundary_snapshot()

    def wait_for_data(self, timeout_s=0.05):
        receiver = getattr(self, "receiver", None)
        if receiver is not None:
            receiver.wait_for_data(timeout_s)

    def configure(self, voltage_mv):
        validate_voltage(voltage_mv)  # upstream silently clamps invalid values
        if self.api.get_modifiers() is not True:
            raise RuntimeError("PPK2 metadata failed; wrong CDC interface or stale data")
        if self.api.modifiers.get("Calibrated") is None or self.api.modifiers.get("HW") is None:
            raise RuntimeError("incomplete PPK2 metadata; source output not enabled")
        self.source_configuration_attempted = True
        try:
            self.api.use_source_meter()
            self.api.set_source_voltage(voltage_mv)
        except BaseException:
            # Metadata identified the selected PPK2. A partial configuration
            # failure is still before READY/USB removal; explicitly try OFF.
            try:
                self.api.toggle_DUT_power("OFF")
            except Exception as exc:
                self.cleanup_errors.append({
                    "operation": "configure_emergency_power_off",
                    "error": f"{type(exc).__name__}: {exc}"[:512],
                    "output_state": "UNKNOWN; OFF NOT VERIFIED",
                })
            raise

    def power(self, on):
        self.api.toggle_DUT_power("ON" if on else "OFF")

    def start_measuring(self):
        if self._acquisition_started:
            raise RuntimeError("PPK2 acquisition cannot restart inside an owner session")
        self._acquisition_started = True
        self.receiver = FastRawReceiver(self._read_direct, max_bytes=self.receiver_max_bytes)
        self.api.start_measuring()
        self.acquisition_origin_monotonic_s = time.monotonic()
        self.receiver.start()

    def decode(self, data):
        # 0.9.2 get_samples() invents a reading from an incomplete first frame.
        # Give it ONLY complete four-byte frames, retaining leftovers here.
        joined = self.remainder + data
        complete = len(joined) // 4 * 4
        self.remainder = joined[complete:]
        self.last_frame_count = complete // 4
        if not complete:
            return []
        self.api.remainder = {"sequence": b"", "len": 0}
        samples, _ = self.api.get_samples(joined[:complete])
        return samples

    def close(self):
        receiver = getattr(self, "receiver", None)
        if receiver is not None and (receiver.running or receiver.join_required):
            raise RuntimeError("refusing serial close under active/pending fast reader; owner/control must be retained")
        try:
            if self.api is not None:
                self.api.ser.close()
        finally:
            self.lease.close()
