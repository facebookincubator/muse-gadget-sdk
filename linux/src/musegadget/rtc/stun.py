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

"""Minimal STUN for ICE-lite connectivity checks and consent (RFC 5389/8445)."""

from __future__ import annotations

import binascii
import hashlib
import hmac
import os
import struct

MAGIC = 0x2112A442
BINDING_REQUEST = 0x0001
BINDING_SUCCESS = 0x0101

USERNAME = 0x0006
MESSAGE_INTEGRITY = 0x0008
PRIORITY = 0x0024
USE_CANDIDATE = 0x0025
FINGERPRINT = 0x8028
ICE_CONTROLLING = 0x802A


def is_stun(data: bytes) -> bool:
    return len(data) >= 20 and data[0] < 2 and struct.unpack_from("!I", data, 4)[0] == MAGIC


def _attr(kind: int, value: bytes) -> bytes:
    pad = (4 - len(value) % 4) % 4
    return struct.pack("!HH", kind, len(value)) + value + bytes(pad)


def binding_request(username: str, password: str, priority: int, tie_breaker: int,
                    *, nominate: bool = True) -> tuple[bytes, bytes]:
    """Return (packet, transaction id) for a controlling check, nominating the path if asked."""
    txid = os.urandom(12)
    attrs = (
        _attr(USERNAME, username.encode())
        + _attr(PRIORITY, struct.pack("!I", priority))
        + _attr(ICE_CONTROLLING, struct.pack("!Q", tie_breaker))
        + (_attr(USE_CANDIDATE, b"") if nominate else b"")
    )
    header = struct.pack("!HHI", BINDING_REQUEST, len(attrs) + 24, MAGIC) + txid
    mac = hmac.new(password.encode(), header + attrs, hashlib.sha1).digest()
    attrs += _attr(MESSAGE_INTEGRITY, mac)
    header = struct.pack("!HHI", BINDING_REQUEST, len(attrs) + 8, MAGIC) + txid
    crc = (binascii.crc32(header + attrs) ^ 0x5354554E) & 0xFFFFFFFF
    attrs += _attr(FINGERPRINT, struct.pack("!I", crc))
    return header + attrs, txid


def parse_response(data: bytes, txid: bytes, password: str) -> bool:
    """True for an authenticated success response to txid; False for anything else."""
    if not is_stun(data) or data[8:20] != txid:
        return False
    kind, length = struct.unpack_from("!HH", data, 0)
    if kind != BINDING_SUCCESS or len(data) < 20 + length:
        return False
    offset = 20
    while offset + 4 <= 20 + length:
        attr, attr_len = struct.unpack_from("!HH", data, offset)
        if attr == MESSAGE_INTEGRITY:
            prefix = bytearray(data[:offset])
            struct.pack_into("!H", prefix, 2, offset - 20 + 24)
            expected = hmac.new(password.encode(), bytes(prefix), hashlib.sha1).digest()
            return hmac.compare_digest(expected, data[offset + 4 : offset + 24])
        offset += 4 + attr_len + (4 - attr_len % 4) % 4
    return False
