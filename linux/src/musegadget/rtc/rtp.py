# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""RTP parsing, H.264 depacketization (RFC 6184), and small RTCP builders."""

from __future__ import annotations

import struct
from dataclasses import dataclass

START_CODE = b"\x00\x00\x00\x01"


def is_rtcp(data: bytes) -> bool:
    return len(data) >= 8 and 192 <= data[1] <= 223


@dataclass(frozen=True)
class RtpPacket:
    payload_type: int
    marker: bool
    seq: int
    timestamp: int
    ssrc: int
    payload: bytes


def parse_rtp(data: bytes) -> RtpPacket:
    if len(data) < 12 or data[0] >> 6 != 2:
        raise ValueError("not RTP")
    offset = 12 + 4 * (data[0] & 0x0F)
    if data[0] & 0x10:
        offset += 4 + 4 * struct.unpack_from("!H", data, offset + 2)[0]
    end = len(data)
    if data[0] & 0x20:
        end -= data[-1]
    if offset > end:
        raise ValueError("bad RTP lengths")
    seq, timestamp, ssrc = struct.unpack_from("!HII", data, 2)
    return RtpPacket(data[1] & 0x7F, bool(data[1] & 0x80), seq, timestamp, ssrc, data[offset:end])


def build_rtp(payload_type: int, seq: int, timestamp: int, ssrc: int, payload: bytes, marker: bool = False) -> bytes:
    return struct.pack("!BBHII", 0x80, payload_type | (0x80 if marker else 0), seq & 0xFFFF, timestamp & 0xFFFFFFFF, ssrc) + payload


def build_pli(sender_ssrc: int, media_ssrc: int) -> bytes:
    return struct.pack("!BBHII", 0x81, 206, 2, sender_ssrc, media_ssrc)



MAX_UNIT_BYTES = 2 * 1024 * 1024
MAX_UNIT_NALS = 256


class H264Depacketizer:
    """Reassemble RTP H.264 into Annex-B access units; drops a unit on loss or overflow."""

    def __init__(self) -> None:
        self._nals: list[bytes] = []
        self._bytes = 0
        self._fu: bytearray | None = None
        self._expected_seq: int | None = None
        self._broken = False
        self.needs_keyframe = True

    def push(self, packet: RtpPacket) -> bytes | None:
        if self._expected_seq is not None and packet.seq != self._expected_seq:
            self._drop()
        self._expected_seq = (packet.seq + 1) & 0xFFFF
        if not self._broken:
            self._consume(packet.payload)
            if self._bytes > MAX_UNIT_BYTES or len(self._nals) > MAX_UNIT_NALS:
                self._drop()
        if not packet.marker:
            return None
        if self._fu is not None:  # a fragmented NAL left unfinished at the marker
            self._drop()
        nals, broken = self._nals, self._broken
        self._nals, self._bytes, self._fu, self._broken = [], 0, None, False
        if broken or not nals:
            return None
        if any(nal[0] & 0x1F == 5 for nal in nals):
            self.needs_keyframe = False
        if self.needs_keyframe:
            return None
        return b"".join(START_CODE + nal for nal in nals)

    def _drop(self) -> None:
        """Discard the unit in progress; the rest of it arrives up to the marker."""
        self._nals, self._bytes, self._fu = [], 0, None
        self._broken = self.needs_keyframe = True

    def _consume(self, payload: bytes) -> None:
        if not payload:
            return
        self._bytes += len(payload)
        kind = payload[0] & 0x1F
        if 1 <= kind <= 23:
            self._nals.append(payload)
        elif kind == 24:  # STAP-A
            offset = 1
            while offset + 2 <= len(payload):
                size = struct.unpack_from("!H", payload, offset)[0]
                offset += 2
                if size and offset + size <= len(payload):
                    self._nals.append(payload[offset : offset + size])
                offset += size
        elif kind == 28 and len(payload) >= 2:  # FU-A; the fragment itself may be empty
            start, end = payload[1] & 0x80, payload[1] & 0x40
            if start:
                self._fu = bytearray([payload[0] & 0xE0 | payload[1] & 0x1F])
            if self._fu is None:  # a fragment without its start
                self._drop()
                return
            self._fu += payload[2:]
            if end:
                self._nals.append(bytes(self._fu))
                self._fu = None


class _Bits:
    def __init__(self, data: bytes) -> None:
        self.data = data.replace(b"\x00\x00\x03", b"\x00\x00")
        self.pos = 0

    def bit(self) -> int:
        byte = self.data[self.pos >> 3]
        value = (byte >> (7 - (self.pos & 7))) & 1
        self.pos += 1
        return value

    def bits(self, n: int) -> int:
        value = 0
        for _ in range(n):
            value = value << 1 | self.bit()
        return value

    def ue(self) -> int:
        zeros = 0
        while self.bit() == 0:
            zeros += 1
            if zeros > 31:
                raise ValueError("malformed Exp-Golomb code")
        return (1 << zeros) - 1 + self.bits(zeros)

    def se(self) -> int:
        value = self.ue()
        return (value + 1) // 2 if value & 1 else -(value // 2)


def sps_dimensions(access_unit: bytes) -> tuple[int, int] | None:
    for nal in access_unit.split(START_CODE):
        if nal and nal[0] & 0x1F == 7:
            try:
                return _parse_sps(nal[1:])
            except (IndexError, ValueError):
                return None
    return None


def _parse_sps(data: bytes) -> tuple[int, int]:
    b = _Bits(data)
    profile = b.bits(8)
    b.bits(16)
    b.ue()
    chroma = 1
    if profile in (100, 110, 122, 244, 44, 83, 86, 118, 128, 138, 139, 134, 135):
        chroma = b.ue()
        if chroma == 3:
            b.bit()
        b.ue()
        b.ue()
        b.bit()
        if b.bit():
            for i in range(8 if chroma != 3 else 12):
                if b.bit():
                    last = nxt = 8
                    for _ in range(16 if i < 6 else 64):
                        if nxt:
                            nxt = (last + b.se() + 256) % 256
                        last = nxt or last
    b.ue()
    poc_type = b.ue()
    if poc_type == 0:
        b.ue()
    elif poc_type == 1:
        b.bit()
        b.se()
        b.se()
        cycle = b.ue()
        if cycle > 255:
            raise ValueError("malformed SPS")
        for _ in range(cycle):
            b.se()
    b.ue()
    b.bit()
    width_mbs = b.ue() + 1
    height_units = b.ue() + 1
    frame_mbs_only = b.bit()
    if not frame_mbs_only:
        b.bit()
    b.bit()
    width, height = width_mbs * 16, height_units * 16 * (2 - frame_mbs_only)
    if b.bit():
        left, right, top, bottom = b.ue(), b.ue(), b.ue(), b.ue()
        crop_x = 2 if chroma in (1, 2) else 1
        crop_y = (2 if chroma == 1 else 1) * (2 - frame_mbs_only)
        width -= (left + right) * crop_x
        height -= (top + bottom) * crop_y
    return width, height
