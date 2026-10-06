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

from __future__ import annotations

import hashlib
import hmac as stdlib_hmac
import threading
from dataclasses import dataclass
from enum import IntEnum
from typing import Optional, Tuple

from cryptography.exceptions import InvalidTag
from cryptography.hazmat.primitives import hashes, hmac, serialization
from cryptography.hazmat.primitives.asymmetric import x25519
from cryptography.hazmat.primitives.ciphers.aead import AESGCM


PROTOCOL_NAME = b"Noise_XX_25519_AESGCM_SHA256"
DH_KEY_LEN = 32
AEAD_TAG_LEN = 16
MIN_MSG2_LEN = DH_KEY_LEN + (DH_KEY_LEN + AEAD_TAG_LEN) + AEAD_TAG_LEN
MIN_MSG3_LEN = DH_KEY_LEN + AEAD_TAG_LEN + AEAD_TAG_LEN
MAX_SAFE_NONCE = (1 << 53) - 1

ALL_ZEROS_32 = b"\x00" * 32

X25519_LOW_ORDER_POINTS = (
    bytes(32),
    bytes([1]) + bytes(31),
    bytes(
        [
            0xE0,
            0xEB,
            0x7A,
            0x7C,
            0x3B,
            0x41,
            0xB8,
            0xAE,
            0x16,
            0x56,
            0xE3,
            0xFA,
            0xF1,
            0x9F,
            0xC4,
            0x6A,
            0xDA,
            0x09,
            0x8D,
            0xEB,
            0x9C,
            0x32,
            0xB1,
            0xFD,
            0x86,
            0x62,
            0x05,
            0x16,
            0x5F,
            0x49,
            0xB8,
            0x00,
        ]
    ),
    bytes(
        [
            0x5F,
            0x9C,
            0x95,
            0xBC,
            0xA3,
            0x50,
            0x8C,
            0x24,
            0xB1,
            0xD0,
            0xB1,
            0x55,
            0x9C,
            0x83,
            0xEF,
            0x5B,
            0x04,
            0x44,
            0x5C,
            0xC4,
            0x58,
            0x1C,
            0x8E,
            0x86,
            0xD8,
            0x22,
            0x4E,
            0xDD,
            0xD0,
            0x9F,
            0x11,
            0x57,
        ]
    ),
    bytes(
        [
            0xEC,
            0xFF,
            0xFF,
            0xFF,
            0xFF,
            0xFF,
            0xFF,
            0xFF,
            0xFF,
            0xFF,
            0xFF,
            0xFF,
            0xFF,
            0xFF,
            0xFF,
            0xFF,
            0xFF,
            0xFF,
            0xFF,
            0xFF,
            0xFF,
            0xFF,
            0xFF,
            0xFF,
            0xFF,
            0xFF,
            0xFF,
            0xFF,
            0xFF,
            0xFF,
            0xFF,
            0x7F,
        ]
    ),
    bytes(
        [
            0xED,
            0xFF,
            0xFF,
            0xFF,
            0xFF,
            0xFF,
            0xFF,
            0xFF,
            0xFF,
            0xFF,
            0xFF,
            0xFF,
            0xFF,
            0xFF,
            0xFF,
            0xFF,
            0xFF,
            0xFF,
            0xFF,
            0xFF,
            0xFF,
            0xFF,
            0xFF,
            0xFF,
            0xFF,
            0xFF,
            0xFF,
            0xFF,
            0xFF,
            0xFF,
            0xFF,
            0x7F,
        ]
    ),
    bytes(
        [
            0xEE,
            0xFF,
            0xFF,
            0xFF,
            0xFF,
            0xFF,
            0xFF,
            0xFF,
            0xFF,
            0xFF,
            0xFF,
            0xFF,
            0xFF,
            0xFF,
            0xFF,
            0xFF,
            0xFF,
            0xFF,
            0xFF,
            0xFF,
            0xFF,
            0xFF,
            0xFF,
            0xFF,
            0xFF,
            0xFF,
            0xFF,
            0xFF,
            0xFF,
            0xFF,
            0xFF,
            0x7F,
        ]
    ),
)


class NoiseProtocolError(RuntimeError):
    """Raised when a Noise handshake or transport state machine is violated."""


def _concat(*chunks: bytes) -> bytes:
    return b"".join(chunks)


def _hmac_sha256(key: bytes, data: bytes) -> bytes:
    h = hmac.HMAC(key, hashes.SHA256())
    h.update(data)
    return h.finalize()


def _hkdf(chaining_key: bytes, input_key_material: bytes, num_outputs: int) -> Tuple[bytes, ...]:
    if num_outputs not in (2, 3):
        raise ValueError("hkdf supports 2 or 3 outputs")
    temp_key = _hmac_sha256(chaining_key, input_key_material)
    output1 = _hmac_sha256(temp_key, b"\x01")
    output2 = _hmac_sha256(temp_key, output1 + b"\x02")
    if num_outputs == 2:
        return output1, output2
    output3 = _hmac_sha256(temp_key, output2 + b"\x03")
    return output1, output2, output3


def build_nonce_iv(nonce: int) -> bytes:
    if nonce < 0 or nonce > 0xFFFFFFFFFFFFFFFF:
        raise NoiseProtocolError("nonce outside uint64 range")
    return b"\x00\x00\x00\x00" + nonce.to_bytes(8, "big")


class CipherState:
    def __init__(self) -> None:
        self._aesgcm: Optional[AESGCM] = None
        self._keyed = False
        self._nonce = 0
        self._poisoned = False
        self._lock = threading.Lock()

    def _assert_alive(self) -> None:
        if self._poisoned:
            raise NoiseProtocolError("CipherState: poisoned after prior failure")

    def initialize_key(self, key: bytes) -> None:
        self._assert_alive()
        raw_key = bytes(key)
        if len(raw_key) != 32:
            raise NoiseProtocolError("CipherState: AES-GCM key must be 32 bytes")
        self._aesgcm = AESGCM(raw_key)
        self._keyed = True
        self._nonce = 0

    def has_key(self) -> bool:
        return self._keyed

    def encrypt_with_ad(self, ad: bytes, plaintext: bytes) -> bytes:
        with self._lock:
            return self._do_encrypt(bytes(ad), bytes(plaintext))

    def decrypt_with_ad(self, ad: bytes, ciphertext: bytes) -> bytes:
        with self._lock:
            return self._do_decrypt(bytes(ad), bytes(ciphertext))

    def _next_nonce(self) -> int:
        if self._nonce >= MAX_SAFE_NONCE:
            self._poisoned = True
            raise NoiseProtocolError("CipherState: nonce exhausted")
        current = self._nonce
        self._nonce += 1
        return current

    def _do_encrypt(self, ad: bytes, plaintext: bytes) -> bytes:
        self._assert_alive()
        if not self._keyed:
            return plaintext
        nonce = self._next_nonce()
        try:
            return self._aesgcm.encrypt(build_nonce_iv(nonce), plaintext, ad)  # type: ignore[union-attr]
        except Exception:
            self._poisoned = True
            raise

    def _do_decrypt(self, ad: bytes, ciphertext: bytes) -> bytes:
        self._assert_alive()
        if not self._keyed:
            return ciphertext
        nonce = self._next_nonce()
        try:
            return self._aesgcm.decrypt(build_nonce_iv(nonce), ciphertext, ad)  # type: ignore[union-attr]
        except InvalidTag as exc:
            self._poisoned = True
            raise NoiseProtocolError("CipherState: decrypt failed") from exc
        except Exception:
            self._poisoned = True
            raise


@dataclass
class _X25519KeyPair:
    private_key: x25519.X25519PrivateKey
    public_key_bytes: bytes


def _generate_x25519_key_pair() -> _X25519KeyPair:
    private_key = x25519.X25519PrivateKey.generate()
    public_key_bytes = private_key.public_key().public_bytes(
        encoding=serialization.Encoding.Raw,
        format=serialization.PublicFormat.Raw,
    )
    return _X25519KeyPair(private_key=private_key, public_key_bytes=public_key_bytes)


def _x25519_dh(private_key: x25519.X25519PrivateKey, public_key_bytes: bytes) -> bytes:
    public_key_bytes = bytes(public_key_bytes)
    if len(public_key_bytes) != DH_KEY_LEN:
        raise NoiseProtocolError("x25519: invalid public key length")
    for low_order in X25519_LOW_ORDER_POINTS:
        if stdlib_hmac.compare_digest(public_key_bytes, low_order):
            raise NoiseProtocolError("x25519: rejected low-order public key")
    public_key = x25519.X25519PublicKey.from_public_bytes(public_key_bytes)
    shared = private_key.exchange(public_key)
    if stdlib_hmac.compare_digest(shared, ALL_ZEROS_32):
        raise NoiseProtocolError("x25519: DH produced all-zeros output")
    return shared


class _SymmetricState:
    def __init__(self) -> None:
        self._ck = bytes(32)
        self._h = bytes(32)
        self._cipher = CipherState()

    def initialize(self) -> None:
        padded = bytearray(32)
        padded[: len(PROTOCOL_NAME)] = PROTOCOL_NAME
        self._h = bytes(padded)
        self._ck = self._h
        self.mix_hash(b"")

    def mix_hash(self, data: bytes) -> None:
        self._h = hashlib.sha256(self._h + bytes(data)).digest()

    def mix_key(self, input_key_material: bytes) -> None:
        ck, temp_k = _hkdf(self._ck, bytes(input_key_material), 2)
        self._ck = ck
        self._cipher = CipherState()
        self._cipher.initialize_key(temp_k)

    def encrypt_and_hash(self, plaintext: bytes) -> bytes:
        ciphertext = self._cipher.encrypt_with_ad(self._h, plaintext)
        self.mix_hash(ciphertext)
        return ciphertext

    def decrypt_and_hash(self, ciphertext: bytes) -> bytes:
        plaintext = self._cipher.decrypt_with_ad(self._h, ciphertext)
        self.mix_hash(ciphertext)
        return plaintext

    def split(self) -> Tuple[CipherState, CipherState]:
        temp_k1, temp_k2 = _hkdf(self._ck, b"", 2)
        self._ck = bytes(32)
        self._h = bytes(32)
        c1 = CipherState()
        c1.initialize_key(temp_k1)
        c2 = CipherState()
        c2.initialize_key(temp_k2)
        return c1, c2

    def handshake_hash(self) -> bytes:
        return bytes(self._h)


class _Phase(IntEnum):
    CREATED = 0
    INITIALIZED = 1
    MSG1_SENT = 2
    MSG2_READ = 3
    MSG3_SENT = 4
    SPLIT = 5
    DEAD = 6


def _require_phase(actual: _Phase, expected: _Phase, method: str) -> None:
    if actual == _Phase.DEAD:
        raise NoiseProtocolError(f"NoiseXX: {method} called on dead handshake")
    if actual != expected:
        raise NoiseProtocolError(
            f"NoiseXX: {method} called in wrong phase (expected {int(expected)}, got {int(actual)})"
        )


class NoiseXXInitiator:
    def __init__(self) -> None:
        self._ss = _SymmetricState()
        self._e: Optional[_X25519KeyPair] = None
        self._s: Optional[_X25519KeyPair] = None
        self._re: Optional[bytes] = None
        self._rs: Optional[bytes] = None
        self._phase = _Phase.CREATED

    def initialize(self) -> None:
        _require_phase(self._phase, _Phase.CREATED, "initialize")
        self._ss.initialize()
        self._phase = _Phase.INITIALIZED

    def write_message1(self) -> bytes:
        _require_phase(self._phase, _Phase.INITIALIZED, "write_message1")
        try:
            self._e = _generate_x25519_key_pair()
            self._ss.mix_hash(self._e.public_key_bytes)
            self._ss.encrypt_and_hash(b"")
            self._phase = _Phase.MSG1_SENT
            return self._e.public_key_bytes
        except Exception:
            self._phase = _Phase.DEAD
            raise

    def read_message2(self, msg: bytes) -> bytes:
        _require_phase(self._phase, _Phase.MSG1_SENT, "read_message2")
        msg = bytes(msg)
        if len(msg) < MIN_MSG2_LEN:
            self._phase = _Phase.DEAD
            raise NoiseProtocolError(
                f"NoiseXX: message 2 too short ({len(msg)} < {MIN_MSG2_LEN})"
            )
        try:
            offset = 0
            self._re = msg[offset : offset + DH_KEY_LEN]
            self._ss.mix_hash(self._re)
            offset += DH_KEY_LEN

            if self._e is None:
                raise NoiseProtocolError("NoiseXX: missing initiator ephemeral key")
            ee = _x25519_dh(self._e.private_key, self._re)
            self._ss.mix_key(ee)

            self._rs = self._ss.decrypt_and_hash(msg[offset : offset + DH_KEY_LEN + AEAD_TAG_LEN])
            offset += DH_KEY_LEN + AEAD_TAG_LEN

            es = _x25519_dh(self._e.private_key, self._rs)
            self._ss.mix_key(es)

            payload = self._ss.decrypt_and_hash(msg[offset:])
            self._phase = _Phase.MSG2_READ
            return payload
        except Exception:
            self._phase = _Phase.DEAD
            raise

    def write_message3(self) -> bytes:
        _require_phase(self._phase, _Phase.MSG2_READ, "write_message3")
        try:
            self._s = _generate_x25519_key_pair()
            enc_s = self._ss.encrypt_and_hash(self._s.public_key_bytes)

            if self._re is None:
                raise NoiseProtocolError("NoiseXX: missing responder ephemeral key")
            se = _x25519_dh(self._s.private_key, self._re)
            self._ss.mix_key(se)

            enc_payload = self._ss.encrypt_and_hash(b"")
            self._phase = _Phase.MSG3_SENT
            return _concat(enc_s, enc_payload)
        except Exception:
            self._phase = _Phase.DEAD
            raise

    def split(self) -> Tuple[CipherState, CipherState]:
        _require_phase(self._phase, _Phase.MSG3_SENT, "split")
        self._phase = _Phase.SPLIT
        result = self._ss.split()
        self._e = None
        self._s = None
        self._re = None
        self._rs = None
        return result

    def destroy(self) -> None:
        self._e = None
        self._s = None
        self._re = None
        self._rs = None
        self._phase = _Phase.DEAD

    def remote_static_public_key(self) -> Optional[bytes]:
        return None if self._rs is None else bytes(self._rs)

    def handshake_hash(self) -> bytes:
        return self._ss.handshake_hash()


class NoiseXXResponder:
    """Test responder for protocol verification."""

    def __init__(self, payload: bytes = b"") -> None:
        self._ss = _SymmetricState()
        self._e: Optional[_X25519KeyPair] = None
        self._s: Optional[_X25519KeyPair] = None
        self._re: Optional[bytes] = None
        self._payload = bytes(payload)
        self._phase = _Phase.CREATED

    def initialize(self) -> None:
        _require_phase(self._phase, _Phase.CREATED, "initialize")
        self._ss.initialize()
        self._phase = _Phase.INITIALIZED

    def read_message1_and_write_message2(self, msg1: bytes) -> bytes:
        _require_phase(self._phase, _Phase.INITIALIZED, "read_message1_and_write_message2")
        msg1 = bytes(msg1)
        if len(msg1) < DH_KEY_LEN:
            self._phase = _Phase.DEAD
            raise NoiseProtocolError(
                f"NoiseXX: message 1 too short ({len(msg1)} < {DH_KEY_LEN})"
            )
        try:
            self._re = msg1[:DH_KEY_LEN]
            self._ss.mix_hash(self._re)
            self._ss.decrypt_and_hash(msg1[DH_KEY_LEN:])

            self._e = _generate_x25519_key_pair()
            self._ss.mix_hash(self._e.public_key_bytes)

            ee = _x25519_dh(self._e.private_key, self._re)
            self._ss.mix_key(ee)

            self._s = _generate_x25519_key_pair()
            enc_s = self._ss.encrypt_and_hash(self._s.public_key_bytes)

            es = _x25519_dh(self._s.private_key, self._re)
            self._ss.mix_key(es)

            enc_payload = self._ss.encrypt_and_hash(self._payload)
            self._phase = _Phase.MSG2_READ
            return _concat(self._e.public_key_bytes, enc_s, enc_payload)
        except Exception:
            self._phase = _Phase.DEAD
            raise

    def read_message3(self, msg3: bytes) -> None:
        _require_phase(self._phase, _Phase.MSG2_READ, "read_message3")
        msg3 = bytes(msg3)
        if len(msg3) < MIN_MSG3_LEN:
            self._phase = _Phase.DEAD
            raise NoiseProtocolError(
                f"NoiseXX: message 3 too short ({len(msg3)} < {MIN_MSG3_LEN})"
            )
        try:
            offset = 0
            rs = self._ss.decrypt_and_hash(msg3[offset : offset + DH_KEY_LEN + AEAD_TAG_LEN])
            offset += DH_KEY_LEN + AEAD_TAG_LEN

            if self._e is None:
                raise NoiseProtocolError("NoiseXX: missing responder ephemeral key")
            se = _x25519_dh(self._e.private_key, rs)
            self._ss.mix_key(se)

            self._ss.decrypt_and_hash(msg3[offset:])
            self._phase = _Phase.MSG3_SENT
        except Exception:
            self._phase = _Phase.DEAD
            raise

    def split(self) -> Tuple[CipherState, CipherState]:
        _require_phase(self._phase, _Phase.MSG3_SENT, "split")
        self._phase = _Phase.SPLIT
        c1, c2 = self._ss.split()
        self._e = None
        self._s = None
        self._re = None
        return c2, c1

    def handshake_hash(self) -> bytes:
        return self._ss.handshake_hash()
