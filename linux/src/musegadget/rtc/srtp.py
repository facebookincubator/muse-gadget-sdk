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

"""SRTP/SRTCP with AES_CM_128_HMAC_SHA1_80 (RFC 3711), keyed directly.

The call's JOIN carries one 30-byte master key+salt, used for both
directions; streams are told apart by SSRC.
"""

from __future__ import annotations

import hashlib
import hmac
import struct

from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

TAG_LEN = 10
MASTER_KEY_LEN = 16
MASTER_SALT_LEN = 14


class SrtpError(Exception):
    pass


def _keystream(key: bytes, iv: int, length: int) -> bytes:
    encryptor = Cipher(algorithms.AES(key), modes.CTR(iv.to_bytes(16, "big"))).encryptor()
    return encryptor.update(bytes(length)) + encryptor.finalize()


def _xor(data: bytes, stream: bytes) -> bytes:
    return (int.from_bytes(data, "big") ^ int.from_bytes(stream[: len(data)], "big")).to_bytes(
        len(data), "big"
    )


def _derive(master_key: bytes, master_salt: bytes, label: int, length: int) -> bytes:
    x = int.from_bytes(master_salt, "big") ^ (label << 48)
    return _keystream(master_key, x << 16, length)


class _Keys:
    def __init__(self, master_key: bytes, master_salt: bytes, base_label: int) -> None:
        self.enc = _derive(master_key, master_salt, base_label, 16)
        self.auth = _derive(master_key, master_salt, base_label + 1, 20)
        self.salt = int.from_bytes(_derive(master_key, master_salt, base_label + 2, 14), "big")

    def iv(self, ssrc: int, index: int) -> int:
        return ((self.salt << 16) ^ (ssrc << 64) ^ (index << 16)) & ((1 << 128) - 1)

    def tag(self, data: bytes) -> bytes:
        return hmac.new(self.auth, data, hashlib.sha1).digest()[:TAG_LEN]


def rtp_header_len(packet: bytes) -> int:
    if len(packet) < 12:
        raise SrtpError("short RTP packet")
    length = 12 + 4 * (packet[0] & 0x0F)
    if packet[0] & 0x10:
        if len(packet) < length + 4:
            raise SrtpError("short RTP extension")
        length += 4 + 4 * struct.unpack_from("!H", packet, length + 2)[0]
    if len(packet) < length:
        raise SrtpError("short RTP header")
    return length


class _ReplayWindow:
    """RFC 3711 3.3.2: reject packets already seen or older than 64 behind the newest."""

    SIZE = 64

    def __init__(self) -> None:
        self.top: int | None = None
        self.seen = 0

    def check(self, index: int) -> None:
        if self.top is None:
            self.top, self.seen = index, 1
        elif index > self.top:
            shift = index - self.top
            self.seen = ((self.seen << shift) | 1) & ((1 << self.SIZE) - 1) if shift < self.SIZE else 1
            self.top = index
        else:
            behind = self.top - index
            if behind >= self.SIZE or self.seen >> behind & 1:
                raise SrtpError("replayed or stale SRTP packet")
            self.seen |= 1 << behind


MAX_STREAMS = 16


def _window(windows: dict[int, _ReplayWindow], ssrc: int) -> _ReplayWindow:
    if ssrc not in windows:
        if len(windows) >= MAX_STREAMS:
            raise SrtpError("too many SRTP streams")
        windows[ssrc] = _ReplayWindow()
    return windows[ssrc]


class SrtpSession:
    """One direction-agnostic SRTP context keyed by a 30-byte master key+salt."""

    def __init__(self, key_salt: bytes) -> None:
        if len(key_salt) != MASTER_KEY_LEN + MASTER_SALT_LEN:
            raise SrtpError("SRTP master key+salt must be 30 bytes")
        key, salt = key_salt[:MASTER_KEY_LEN], key_salt[MASTER_KEY_LEN:]
        self._rtp = _Keys(key, salt, 0)
        self._rtcp = _Keys(key, salt, 3)
        self._tx_roc: dict[int, tuple[int, int]] = {}  # ssrc -> (roc, last seq)
        self._rx_roc: dict[int, tuple[int, int]] = {}
        self._tx_rtcp_index: dict[int, int] = {}
        self._rx_rtp_window: dict[int, _ReplayWindow] = {}
        self._rx_rtcp_window: dict[int, _ReplayWindow] = {}

    def protect_rtp(self, packet: bytes) -> bytes:
        header_len = rtp_header_len(packet)
        ssrc = struct.unpack_from("!I", packet, 8)[0]
        seq = struct.unpack_from("!H", packet, 2)[0]
        roc, last = self._tx_roc.get(ssrc, (0, seq))
        if seq < last and last - seq > 0x8000:
            roc = (roc + 1) & 0xFFFFFFFF
        self._tx_roc[ssrc] = (roc, seq)
        index = (roc << 16) | seq
        payload = packet[header_len:]
        body = packet[:header_len] + _xor(payload, _keystream(self._rtp.enc, self._rtp.iv(ssrc, index), len(payload)))
        return body + self._rtp.tag(body + struct.pack("!I", roc))

    def unprotect_rtp(self, packet: bytes) -> bytes:
        if len(packet) < 12 + TAG_LEN:
            raise SrtpError("short SRTP packet")
        body, tag = packet[:-TAG_LEN], packet[-TAG_LEN:]
        header_len = rtp_header_len(body)
        ssrc = struct.unpack_from("!I", body, 8)[0]
        seq = struct.unpack_from("!H", body, 2)[0]
        roc, last = self._rx_roc.get(ssrc, (0, seq))
        guess = roc
        if last < 0x8000:
            if seq - last > 0x8000:
                guess = (roc - 1) & 0xFFFFFFFF
        elif last - 0x8000 > seq:
            guess = (roc + 1) & 0xFFFFFFFF
        if not hmac.compare_digest(self._rtp.tag(body + struct.pack("!I", guess)), tag):
            raise SrtpError("SRTP authentication failed")
        _window(self._rx_rtp_window, ssrc).check((guess << 16) | seq)
        if guess == roc and seq > last or guess == (roc + 1) & 0xFFFFFFFF:
            self._rx_roc[ssrc] = (guess, seq)
        elif ssrc not in self._rx_roc:
            self._rx_roc[ssrc] = (guess, seq)
        index = (guess << 16) | seq
        payload = body[header_len:]
        return body[:header_len] + _xor(payload, _keystream(self._rtp.enc, self._rtp.iv(ssrc, index), len(payload)))

    def protect_rtcp(self, packet: bytes) -> bytes:
        if len(packet) < 8:
            raise SrtpError("short RTCP packet")
        ssrc = struct.unpack_from("!I", packet, 4)[0]
        # Like libsrtp, the first SRTCP index sent is 1.
        index = (self._tx_rtcp_index.get(ssrc, 0) + 1) & 0x7FFFFFFF
        self._tx_rtcp_index[ssrc] = index
        payload = packet[8:]
        body = packet[:8] + _xor(payload, _keystream(self._rtcp.enc, self._rtcp.iv(ssrc, index), len(payload)))
        body += struct.pack("!I", 0x80000000 | index)
        return body + self._rtcp.tag(body)

    def unprotect_rtcp(self, packet: bytes) -> bytes:
        if len(packet) < 8 + 4 + TAG_LEN:
            raise SrtpError("short SRTCP packet")
        body, tag = packet[:-TAG_LEN], packet[-TAG_LEN:]
        if not hmac.compare_digest(self._rtcp.tag(body), tag):
            raise SrtpError("SRTCP authentication failed")
        e_index = struct.unpack_from("!I", body, len(body) - 4)[0]
        body = body[:-4]
        index = e_index & 0x7FFFFFFF
        ssrc = struct.unpack_from("!I", body, 4)[0]
        _window(self._rx_rtcp_window, ssrc).check(index)
        if not e_index & 0x80000000:
            return body
        payload = body[8:]
        return body[:8] + _xor(payload, _keystream(self._rtcp.enc, self._rtcp.iv(ssrc, index), len(payload)))
