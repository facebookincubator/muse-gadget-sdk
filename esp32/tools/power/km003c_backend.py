# Copyright (c) Meta Platforms, Inc. and affiliates.
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy at http://www.apache.org/licenses/LICENSE-2.0
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""POWER-Z KM003C USB-C meter, polled for single readings over its vendor interface.

The meter sits in the board's USB power, so it reads the 5 V input: the board
plus its charger, regulators and any battery charging. Only pyusb is needed;
the packet layout follows km003c-rs (github.com/okhsunrog/km003c-rs).
"""

import struct
import time
from dataclasses import dataclass

VID = 0x5FC9
PID = 0x0063
INTERFACE = 0       # vendor interface, bulk
ENDPOINT_OUT = 0x01
ENDPOINT_IN = 0x81
GET_DATA = 12       # request packet type
PUT_DATA = 65       # data reply packet type
ATTRIBUTE_ADC = 1

READ_TIMEOUT_MS = 1000
MAX_RESPONSE_BYTES = 2048
# A reply carries its request's id; older ones (a reply to an earlier
# session, or a Disconnect) are dropped. Bounded so a silent meter fails.
MAX_STALE_RESPONSES = 8

# vbus, ibus, vbus avg, ibus avg (uV, uA), two uncalibrated averages, then the
# temperature register (1/128 C); the rest of the 44 bytes is unused here.
ADC = struct.Struct("<iiiiiih")
ADC_BYTES = 44


class MeterError(RuntimeError):
    pass


@dataclass(frozen=True)
class Reading:
    host_time_s: float  # time.monotonic() when the reply arrived
    vbus_v: float
    ibus_a: float       # signed: negative when the meter is plugged in reversed
    temp_c: float

    @property
    def power_w(self):
        return self.vbus_v * self.ibus_a


def request(request_id):
    """A GetData request for one ADC reading."""
    return struct.pack("<BBH", GET_DATA, request_id, ATTRIBUTE_ADC << 1)


def parse_reply(data, request_id):
    """The Reading in a reply to `request_id`, or None if the reply is another one's."""
    if len(data) < 8:
        return None
    header, extended = struct.unpack_from("<II", data)
    if header & 0x7F != PUT_DATA or (header >> 8) & 0xFF != request_id:
        return None
    attribute, size = extended & 0x7FFF, extended >> 22
    if attribute != ATTRIBUTE_ADC or size < ADC_BYTES or len(data) < 8 + ADC_BYTES:
        raise MeterError(f"unexpected ADC reply: attribute {attribute}, {size} bytes")
    vbus_uv, ibus_ua, _, _, _, _, temp = ADC.unpack_from(data, 8)
    return Reading(time.monotonic(), vbus_uv / 1e6, ibus_ua / 1e6, temp / 128)


class KM003CBackend:
    """One KM003C, claimed until close(). Close POWER-Z's own app first."""

    def __init__(self, serial_number=None, *, usb_core=None, usb_util=None):
        if usb_core is None:
            import usb.core as usb_core
            import usb.util as usb_util
        self.usb_util = usb_util
        match = None if serial_number is None else (lambda d: d.serial_number == serial_number)
        self.device = usb_core.find(idVendor=VID, idProduct=PID, custom_match=match)
        if self.device is None:
            raise MeterError("no POWER-Z KM003C found" + (f" with serial {serial_number}" if serial_number else ""))
        try:
            self.device.set_configuration()
        except usb_core.USBError:
            pass  # already configured; claiming the interface is what matters
        # Linux powerz hwmon claims interface 0. Platforms without kernel
        # driver support (including macOS) can still claim the vendor interface.
        self.detached = False
        try:
            if self.device.is_kernel_driver_active(INTERFACE):
                self.device.detach_kernel_driver(INTERFACE)
                self.detached = True
        except (NotImplementedError, usb_core.USBError):
            pass
        try:
            usb_util.claim_interface(self.device, INTERFACE)
        except BaseException:
            self.reattach()
            raise
        self.next_id = 0

    def reattach(self):
        """Give interface 0 back to the kernel driver this backend detached."""
        if self.detached:
            self.detached = False
            try:
                self.device.attach_kernel_driver(INTERFACE)
            except Exception:
                pass  # best effort: replugging the meter also rebinds it

    def read(self):
        """One reading, from the request this call sends."""
        request_id = self.next_id
        self.next_id = (self.next_id + 1) & 0xFF
        self.device.write(ENDPOINT_OUT, request(request_id), timeout=READ_TIMEOUT_MS)
        for _ in range(MAX_STALE_RESPONSES):
            data = bytes(self.device.read(ENDPOINT_IN, MAX_RESPONSE_BYTES, timeout=READ_TIMEOUT_MS))
            reading = parse_reply(data, request_id)
            if reading is not None:
                return reading
        raise MeterError("no reply to this request from the meter")

    def close(self):
        try:
            self.usb_util.release_interface(self.device, INTERFACE)
        finally:
            self.usb_util.dispose_resources(self.device)
            self.reattach()
