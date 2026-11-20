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

"""The media-only call JOIN: request encoding and response decoding.

A JOIN is a Thrift Compact message header followed by a message body. The client mints every credential: ICE fragments and
passwords for both ends, a DTLS identity for each end (required fields, unused
because the SRTP key travels in the JOIN), and the SRTP master key.
"""

from __future__ import annotations

import base64
import datetime
import secrets
import string
from dataclasses import dataclass

from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.x509.oid import NameOID

AUDIO_TRACK_ASSISTANT = 1
VIDEO_TRACK_ASSISTANT = 2
VIDEO_TRACK_NONE = 8
DTLS_ACTIVE = 1
# The media endpoint's `vip_type`, as the JOIN's numeric code.
ENDPOINT_TYPES = {"META_AI": 2, "HATCH": 8}

_ICE_CHARS = string.ascii_letters + string.digits + "+/"


class _Writer:
    def __init__(self) -> None:
        self.out = bytearray()
        self._last = [0]

    def _varint(self, n: int) -> None:
        while True:
            byte = n & 0x7F
            n >>= 7
            if n:
                self.out.append(byte | 0x80)
            else:
                self.out.append(byte)
                return

    def _field(self, fid: int, kind: int) -> None:
        delta = fid - self._last[-1]
        if 0 < delta <= 15:
            self.out.append(delta << 4 | kind)
        else:
            self.out.append(kind)
            self._varint((fid << 1) ^ (fid >> 63))
        self._last[-1] = fid

    def i32(self, fid: int, value: int) -> None:
        self._field(fid, 5)
        self._varint(((value << 1) ^ (value >> 31)) & 0xFFFFFFFF)

    def boolean(self, fid: int, value: bool) -> None:
        self._field(fid, 1 if value else 2)

    def binary(self, fid: int, value: bytes | str) -> None:
        data = value.encode() if isinstance(value, str) else value
        self._field(fid, 8)
        self._varint(len(data))
        self.out += data

    def begin(self, fid: int) -> None:
        self._field(fid, 12)
        self._last.append(0)

    def end(self) -> None:
        self.out.append(0)
        self._last.pop()



MAX_RESPONSE_BYTES = 16 * 1024
_MAX_DEPTH = 8


class _Reader:
    """Thrift Compact decoder for one small message; malformed input raises ValueError."""

    def __init__(self, data: bytes) -> None:
        self.data = data
        self.pos = 0
        self.depth = 0

    def _byte(self) -> int:
        if self.pos >= len(self.data):
            raise ValueError("truncated thrift message")
        self.pos += 1
        return self.data[self.pos - 1]

    def _take(self, length: int) -> bytes:
        if length < 0 or self.pos + length > len(self.data):
            raise ValueError("truncated thrift message")
        self.pos += length
        return self.data[self.pos - length : self.pos]

    def _varint(self) -> int:
        shift = result = 0
        while shift < 64:
            byte = self._byte()
            result |= (byte & 0x7F) << shift
            if not byte & 0x80:
                return result
            shift += 7
        raise ValueError("thrift varint too long")

    def _count(self, count: int) -> int:
        # Every element takes at least one byte.
        if count > len(self.data) - self.pos:
            raise ValueError("thrift container larger than its message")
        return count

    def _enter(self) -> None:
        self.depth += 1
        if self.depth > _MAX_DEPTH:
            raise ValueError("thrift message nested too deeply")

    def struct(self) -> dict[int, object]:
        self._enter()
        fields: dict[int, object] = {}
        last = 0
        while True:
            header = self._byte()
            if header == 0:
                self.depth -= 1
                return fields
            delta, kind = header >> 4, header & 0x0F
            if delta:
                fid = last + delta
            else:
                raw = self._varint()
                fid = (raw >> 1) ^ -(raw & 1)
            last = fid
            fields[fid] = self._value(kind)

    def _value(self, kind: int, *, in_container: bool = False) -> object:
        if kind in (1, 2):
            # A struct field's bool lives in its type; a container element's is a byte.
            return self._byte() == 1 if in_container else kind == 1
        if kind == 3:
            return self._byte()
        if kind in (4, 5, 6):
            raw = self._varint()
            return (raw >> 1) ^ -(raw & 1)
        if kind == 7:
            self._take(8)
            return None
        if kind == 8:
            return self._take(self._varint())
        if kind in (9, 10):
            header = self._byte()
            count, elem = header >> 4, header & 0x0F
            if count == 15:
                count = self._varint()
            self._enter()
            items = [self._value(elem, in_container=True) for _ in range(self._count(count))]
            self.depth -= 1
            return items
        if kind == 11:
            count = self._count(self._varint())
            if not count:
                return {}
            kinds = self._byte()
            self._enter()
            items = {self._value(kinds >> 4, in_container=True): self._value(kinds & 0x0F, in_container=True)
                     for _ in range(count)}
            self.depth -= 1
            return items
        if kind == 12:
            return self.struct()
        raise ValueError(f"unsupported thrift compact type {kind}")


def _ice_token(length: int) -> str:
    return "".join(secrets.choice(_ICE_CHARS) for _ in range(length))


def _dtls_identity() -> tuple[str, str, str]:
    """Return (private key PEM, certificate PEM, 'sha-256 AA:BB..' fingerprint)."""
    key = ec.generate_private_key(ec.SECP256R1())
    name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, "musegadget")])
    now = datetime.datetime.now(datetime.timezone.utc)
    cert = (
        x509.CertificateBuilder()
        .subject_name(name)
        .issuer_name(name)
        .public_key(key.public_key())
        .serial_number(x509.random_serial_number())
        .not_valid_before(now - datetime.timedelta(days=1))
        .not_valid_after(now + datetime.timedelta(days=30))
        .sign(key, hashes.SHA256())
    )
    key_pem = key.private_bytes(
        serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8, serialization.NoEncryption()
    ).decode()
    cert_pem = cert.public_bytes(serialization.Encoding.PEM).decode()
    digest = cert.fingerprint(hashes.SHA256()).hex().upper()
    fingerprint = "sha-256 " + ":".join(digest[i : i + 2] for i in range(0, len(digest), 2))
    return key_pem, cert_pem, fingerprint


def session_params(thread_id: str | None) -> bytes:
    """The call's session parameters: use case, callee, entry point and thread."""
    w = _Writer()
    w.i32(1, 0)
    w.binary(2, b"")
    w.binary(3, b"")
    if thread_id:
        w.binary(4, thread_id)
    w.out.append(0)
    return bytes(w.out)


@dataclass(frozen=True)
class JoinOffer:
    payload: bytes
    client_ufrag: str
    server_ufrag: str
    client_pwd: str
    server_pwd: str
    srtp_key: bytes
    client_session_id: str
    transaction_id: int


@dataclass(frozen=True)
class JoinAnswer:
    server_ufrag: str
    status: int | None
    ipv4: str
    ipv6: str


def build_join(*, thread_id: str | None, edge_host: str, edge_port: int, vip_type: int, video: bool) -> JoinOffer:
    client_ufrag, server_ufrag = _ice_token(4), _ice_token(16)
    client_pwd, server_pwd = _ice_token(24), _ice_token(24)
    srtp_key = secrets.token_bytes(30)
    _, _, client_fingerprint = _dtls_identity()
    server_key_pem, server_cert_pem, _ = _dtls_identity()
    client_session_id = str(secrets.randbelow(9 * 10**9) + 10**9)
    transaction_id = secrets.randbelow(2**31 - 1) + 1

    w = _Writer()
    w.binary(1, client_session_id)
    w.i32(3, transaction_id)
    w.out.append(0)
    w._last = [0]
    w.begin(1)  # body.joinRequest
    w.binary(1, client_ufrag)
    w.binary(2, server_ufrag)
    w.begin(3)
    w.binary(1, base64.b64encode(srtp_key))
    w.binary(2, client_pwd)
    w.binary(3, server_pwd)
    w.binary(4, server_key_pem)
    w.end()
    w.binary(4, client_fingerprint)
    w.binary(5, server_cert_pem)
    w.binary(6, f"{edge_host}:{edge_port}")
    params = session_params(thread_id)
    w.i32(8, DTLS_ACTIVE)
    w.i32(11, AUDIO_TRACK_ASSISTANT)
    w.i32(12, VIDEO_TRACK_ASSISTANT if video else VIDEO_TRACK_NONE)
    w.i32(13, vip_type)
    w.boolean(14, False)
    w.binary(16, params)
    w.end()
    w.out.append(0)
    return JoinOffer(
        bytes(w.out), client_ufrag, server_ufrag, client_pwd, server_pwd, srtp_key, client_session_id, transaction_id
    )


def parse_join_response(payload: bytes) -> JoinAnswer:
    if len(payload) > MAX_RESPONSE_BYTES:
        raise ValueError("JOIN response is too large")
    reader = _Reader(payload)
    header = reader.struct()
    body = reader.struct()
    answer = body.get(2)
    if not isinstance(answer, dict):
        raise ValueError("JOIN response carried no joinResponse")

    def text(fid: int) -> str:
        value = answer.get(fid, b"")
        if not isinstance(value, bytes):
            raise ValueError("malformed joinResponse")
        return value.decode()

    if not text(1):
        raise ValueError("JOIN response carried no server ufrag")
    status = header.get(4)
    if isinstance(status, int) and status != 200:
        raise ValueError(f"JOIN refused ({status})")
    return JoinAnswer(
        server_ufrag=text(1),
        status=status if isinstance(status, int) else None,
        ipv4=text(2),
        ipv6=text(3),
    )
