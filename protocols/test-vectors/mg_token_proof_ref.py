#!/usr/bin/env python3
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

"""Reference token proof for mgcommands.h (Token proof), and its test vectors.

Python standard library only (hmac, hashlib). Check the vectors next to this
file:

    python3 protocols/test-vectors/mg_token_proof_ref.py

and regenerate them (the inputs are fixed, so the output never changes):

    python3 protocols/test-vectors/mg_token_proof_ref.py --write

Firmware and apps check themselves against mg-token-proof-v1.json.
"""

import hashlib
import hmac
import json
import sys
from pathlib import Path

VECTORS = Path(__file__).with_name("mg-token-proof-v1.json")

SALT = b"mg token proof v1"
DEVICE_LABEL = b"mg token proof v1 device"
CLIENT_LABEL = b"mg token proof v1 client"
NONCE_LEN = 16
MAC_LEN = 32

# From mgcommands.h.
MG_COMMAND_TOKEN_PROOF = 34
CHALLENGE, RESPONSE, CONFIRM, RESULT, CLEAR = 0, 1, 2, 3, 4


def hkdf_sha256(salt: bytes, ikm: bytes, info: bytes, length: int) -> bytes:
    """RFC 5869 extract and expand."""
    prk = hmac.new(salt, ikm, hashlib.sha256).digest()
    out, block, i = b"", b"", 1
    while len(out) < length:
        block = hmac.new(prk, block + info + bytes([i]), hashlib.sha256).digest()
        out += block
        i += 1
    return out[:length]


def proof_key(access_token: str, node_id: str) -> bytes:
    """K = HKDF-SHA256(salt "mg token proof v1", IKM access token, info node_id, 32)."""
    return hkdf_sha256(SALT, access_token.encode("utf-8"), node_id.encode("utf-8"), 32)


def device_mac(k: bytes, client_nonce: bytes, device_nonce: bytes) -> bytes:
    return hmac.new(k, DEVICE_LABEL + client_nonce + device_nonce, hashlib.sha256).digest()


def client_mac(k: bytes, client_nonce: bytes, device_nonce: bytes) -> bytes:
    return hmac.new(k, CLIENT_LABEL + client_nonce + device_nonce, hashlib.sha256).digest()


def frames(cn: bytes, dn: bytes, dmac: bytes, cmac: bytes) -> dict:
    """The Control writes and notifications of one matching exchange."""
    c = MG_COMMAND_TOKEN_PROOF
    return {
        "challenge": bytes([c, CHALLENGE]) + cn,
        "response": bytes([c, RESPONSE]) + dn + dmac,
        "confirm": bytes([c, CONFIRM]) + cmac,
        "result": bytes([c, RESULT, 1]),
        "clear": bytes([c, CLEAR]),
    }


def det(label: str, n: int) -> bytes:
    """Fixed pseudo-random bytes, so the vectors are reproducible."""
    out, i = b"", 0
    while len(out) < n:
        out += hashlib.sha256(f"{label}/{i}".encode()).digest()
        i += 1
    return out[:n]


INPUTS = [
    {
        "name": "basic",
        "access_token": "mga_test_access_0123456789abcdef",
        "node_id": "homelink-1a2b3c",
        "client_nonce": bytes(range(16)),
        "device_nonce": bytes(range(0xF0, 0x100)),
    },
    {
        "name": "bench tokens",
        "access_token": "bench-access-" + det("bench access", 16).hex(),
        "node_id": "homelink-a7f5b4",
        "client_nonce": det("bench client nonce", 16),
        "device_nonce": det("bench device nonce", 16),
    },
    {
        # A token longer than one SHA-256 block (HMAC hashes long keys; HKDF
        # takes the token as IKM, which is never hashed first).
        "name": "long token",
        "access_token": "eyJ" + det("long access", 300).hex(),
        "node_id": "homelink-000001",
        "client_nonce": det("long client nonce", 16),
        "device_nonce": det("long device nonce", 16),
    },
    {
        # node_id is UTF-8 exactly as device_info reports it.
        "name": "non-ASCII node_id",
        "access_token": "mga_test_access_\u00fc",
        "node_id": "gadget-\u00e9\u00e8-01",
        "client_nonce": bytes(16),
        "device_nonce": bytes([0xFF] * 16),
    },
]


def build() -> dict:
    vectors = []
    for v in INPUTS:
        k = proof_key(v["access_token"], v["node_id"])
        cn, dn = v["client_nonce"], v["device_nonce"]
        dmac, cmac = device_mac(k, cn, dn), client_mac(k, cn, dn)
        vectors.append({
            "name": v["name"],
            "access_token": v["access_token"],
            "node_id": v["node_id"],
            "client_nonce": cn.hex(),
            "device_nonce": dn.hex(),
            "K": k.hex(),
            "device_mac": dmac.hex(),
            "client_mac": cmac.hex(),
            "frames": {name: f.hex() for name, f in frames(cn, dn, dmac, cmac).items()},
        })
    return {
        "description": "Token proof (protocols/mgcommands.h, Token proof). Hex strings are bytes; "
                       "access_token and node_id are UTF-8. K = HKDF-SHA256(salt, access_token, "
                       "node_id, 32); device_mac = HMAC-SHA256(K, device_label || client_nonce || "
                       "device_nonce); client_mac = HMAC-SHA256(K, client_label || client_nonce || "
                       "device_nonce). Labels have no NUL. frames are the Control payloads of a "
                       "matching exchange. Generated by mg_token_proof_ref.py.",
        "version": 1,
        "salt": SALT.decode(),
        "device_label": DEVICE_LABEL.decode(),
        "client_label": CLIENT_LABEL.decode(),
        "vectors": vectors,
    }


def render() -> str:
    return json.dumps(build(), indent=2, ensure_ascii=False) + "\n"


def check() -> list[str]:
    """Recomputes every vector in the file from its own inputs; returns the problems."""
    data = json.loads(VECTORS.read_text(encoding="utf-8"))
    bad = []
    for v in data["vectors"]:
        k = proof_key(v["access_token"], v["node_id"])
        cn, dn = bytes.fromhex(v["client_nonce"]), bytes.fromhex(v["device_nonce"])
        want = {"K": k, "device_mac": device_mac(k, cn, dn), "client_mac": client_mac(k, cn, dn)}
        for field, value in want.items():
            if bytes.fromhex(v[field]) != value:
                bad.append(f"{v['name']}: {field}")
        if len(cn) != NONCE_LEN or len(dn) != NONCE_LEN:
            bad.append(f"{v['name']}: nonce length")
        f = frames(cn, dn, want["device_mac"], want["client_mac"])
        for name, value in f.items():
            if bytes.fromhex(v["frames"][name]) != value:
                bad.append(f"{v['name']}: frame {name}")
    if VECTORS.read_text(encoding="utf-8") != render():
        bad.append("file differs from the generator's output")
    return bad


def main() -> int:
    if sys.argv[1:] == ["--write"]:
        VECTORS.write_text(render(), encoding="utf-8")
        print(f"wrote {VECTORS}")
        return 0
    if sys.argv[1:]:
        print(__doc__)
        return 2
    bad = check()
    for b in bad:
        print(f"FAIL {b}")
    if not bad:
        print(f"OK {VECTORS.name}: {len(INPUTS)} vectors")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
