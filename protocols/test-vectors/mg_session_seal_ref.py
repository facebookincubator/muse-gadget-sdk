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

"""Reference for the sealed device session: credentials and data (mgcommands.h, Sealed credentials).

Constants from mgcommands.h. Python standard
library only (hmac, hashlib), the same primitives as the token proof, so every
gadget that runs the token proof can seal without new crypto. Check the
vectors:

    python3 protocols/test-vectors/mg_session_seal_ref.py

and regenerate them (fixed inputs, so the output never changes):

    python3 protocols/test-vectors/mg_session_seal_ref.py --write
"""

from __future__ import annotations

import hashlib
import hmac
import json
import sys
from pathlib import Path

VECTORS = Path(__file__).with_name("mg-session-seal-v1.json")

SALT = b"mg session seal v1"
STREAM_LABEL = b"mg session seal v1 stream"
TAG_LABEL = b"mg session seal v1 tag"
TAG_LEN = 16
COUNTER_LEN = 4
DEVICE_TO_CLIENT = 0x01  # credentials, and the device's data
CLIENT_TO_DEVICE = 0x02  # credentials_update, and the client's data

# From mgcommands.h.
MG_COMMAND_ERROR = 25
MG_COMMAND_SESSION = 0x89
MG_SESSION_HELLO = 0
MG_ERROR_DATA_CODE = 1
MG_ERROR_CODE_PROOF_REQUIRED = 6
MG_ERROR_CODE_INVALID_VALUE = 3
MG_SESSION_HELLO_FLAG_SEALED = 0x01
MG_SESSION_CREDENTIALS_SAVED = 8
MG_SESSION_DATA = 3
MG_SESSION_ABORT = 5
MG_SESSION_CREDENTIALS = 6
MG_SESSION_CREDENTIALS_UPDATE = 7
MG_SESSION_CLEAR = 12
MG_SESSION_FLAG_FIRST = 1 << 0
MG_SESSION_FLAG_LAST = 1 << 1
MG_SESSION_FLAG_SEALED = 1 << 2
MG_SESSION_FIRST_HEADER = 7
MG_SESSION_DATA_HEADER = 3
MG_ERROR_CODE_INSECURE = 7
MG_ERROR_CODE_INVALID_LENGTH = 2
MG_SESSION_MAX_MESSAGE = 16 * 1024 * 1024
MAX_HOLE_BACK = 255  # a counter refused at its tag stays open once, at most this far back
EXPIRES_KEY = "access_expires_at"


def hkdf_sha256(salt: bytes, ikm: bytes, info: bytes, length: int) -> bytes:
    """RFC 5869 extract and expand (same as mg_token_proof_ref.py)."""
    prk = hmac.new(salt, ikm, hashlib.sha256).digest()
    out, block, i = b"", b"", 1
    while len(out) < length:
        block = hmac.new(prk, block + info + bytes([i]), hashlib.sha256).digest()
        out += block
        i += 1
    return out[:length]


def seal_keys(k: bytes, client_nonce: bytes, device_nonce: bytes, node_id: str) -> tuple[bytes, bytes]:
    """enc_key || mac_key = HKDF-SHA256(salt, IKM K, info cn || dn || node_id, 64).

    The nonces are the ones of the matching token proof on this connection, so
    the keys are fresh per connection and bound to a proven link.
    """
    keys = hkdf_sha256(SALT, k, client_nonce + device_nonce + node_id.encode("utf-8"), 64)
    return keys[:32], keys[32:]


def keystream(enc_key: bytes, direction: int, counter: int, n: int) -> bytes:
    """HMAC-SHA256 in counter mode: block i = HMAC(enc_key, label || dir || counter || i)."""
    out, i = b"", 0
    head = STREAM_LABEL + bytes([direction]) + counter.to_bytes(4, "big")
    while len(out) < n:
        out += hmac.new(enc_key, head + i.to_bytes(4, "big"), hashlib.sha256).digest()
        i += 1
    return out[:n]


def tag(mac_key: bytes, direction: int, counter: int, ciphertext: bytes) -> bytes:
    data = (TAG_LABEL + bytes([direction]) + counter.to_bytes(4, "big")
            + len(ciphertext).to_bytes(4, "big") + ciphertext)
    return hmac.new(mac_key, data, hashlib.sha256).digest()[:TAG_LEN]


def seal(enc_key: bytes, mac_key: bytes, direction: int, counter: int, plaintext: bytes) -> bytes:
    """Sealed bytes = counter (uint32 BE) || ciphertext || tag (16)."""
    ct = bytes(a ^ b for a, b in zip(plaintext, keystream(enc_key, direction, counter, len(plaintext))))
    return counter.to_bytes(4, "big") + ct + tag(mac_key, direction, counter, ct)


def unseal(enc_key: bytes, mac_key: bytes, direction: int, expected_counter: int, sealed: bytes) -> bytes | None:
    """Returns the plaintext, or None if the counter or the tag is wrong."""
    if len(sealed) < COUNTER_LEN + TAG_LEN:
        return None
    counter = int.from_bytes(sealed[:4], "big")
    ct, t = sealed[4:-TAG_LEN], sealed[-TAG_LEN:]
    if counter != expected_counter or not hmac.compare_digest(t, tag(mac_key, direction, counter, ct)):
        return None
    return bytes(a ^ b for a, b in zip(ct, keystream(enc_key, direction, counter, len(ct))))


def hello_frame(max_message: int, window: int, hello_flags: int) -> bytes:
    """[session, hello, version 1, max_message u32 LE, window u32 LE, hello_flags]."""
    return (bytes([MG_COMMAND_SESSION, MG_SESSION_HELLO, 1]) + max_message.to_bytes(4, "little")
            + window.to_bytes(4, "little") + bytes([hello_flags]))


def error_frame(code: int) -> bytes:
    """[error, session, mg_error_data_code, code u16 LE]."""
    return bytes([MG_COMMAND_ERROR, MG_COMMAND_SESSION, MG_ERROR_DATA_CODE]) + code.to_bytes(2, "little")


class ReferenceDevice:
    """The sealed-hello rule only: one sealed session per matching token proof."""

    def __init__(self) -> None:
        self.proof_nonces: tuple[bytes, bytes] | None = None  # set by a matching proof
        self.consumed = False

    def proof_matched(self, client_nonce: bytes, device_nonce: bytes) -> None:
        self.proof_nonces, self.consumed = (client_nonce, device_nonce), False

    def hello(self, frame: bytes) -> bytes:
        sealed = len(frame) >= 12 and frame[11] & MG_SESSION_HELLO_FLAG_SEALED
        if self.proof_nonces is None or (sealed and self.consumed):
            return error_frame(MG_ERROR_CODE_PROOF_REQUIRED)
        if not sealed:
            return error_frame(MG_ERROR_CODE_INSECURE)  # credentials leave only sealed, on any link
        self.consumed = True
        return hello_frame(4096, 4096, MG_SESSION_HELLO_FLAG_SEALED)


def one_session_per_proof() -> dict:
    """Two sealed hellos under one proof: the second is refused; a new proof allows one more. An unsealed hello is
    refused insecure and leaves the proof's nonces untouched."""
    dev = ReferenceDevice()
    hello = hello_frame(4096, 4096, MG_SESSION_HELLO_FLAG_SEALED)
    plain = hello_frame(4096, 4096, 0)
    dev.proof_matched(bytes(range(16)), bytes(range(0xF0, 0x100)))
    steps = [("proof 1 matched", None, None)]
    steps.append(("unsealed hello: insecure, the nonces untouched", plain, dev.hello(plain)))
    steps.append(("sealed hello 1", hello, dev.hello(hello)))
    steps.append(("sealed hello 2, same proof", hello, dev.hello(hello)))
    dev.proof_matched(det("reproof cn", 16), det("reproof dn", 16))
    steps.append(("proof 2 matched", None, None))
    steps.append(("sealed hello 3, new proof", hello, dev.hello(hello)))
    return {
        "name": "one sealed session per token proof",
        "steps": [{"step": n, "client": c.hex() if c else None, "device": d.hex() if d else None}
                  for n, c, d in steps],
    }


def det(label: str, n: int) -> bytes:
    out, i = b"", 0
    while len(out) < n:
        out += hashlib.sha256(f"{label}/{i}".encode()).digest()
        i += 1
    return out[:n]


def creds_json(access: str, refresh: str, node_id: str | None = None) -> bytes:
    obj = {"node_id": node_id, "access_token": access, "refresh_token": refresh} if node_id else \
          {"access_token": access, "refresh_token": refresh}
    return json.dumps(obj, separators=(",", ":")).encode("utf-8")


def creds_with_expires(access: str, refresh: str, node_id: str, expires: int) -> bytes:
    """The credentials a device sends: node_id, the pair, then access_expires_at when it keeps one."""
    obj = {"node_id": node_id, "access_token": access, "refresh_token": refresh}
    if expires:
        obj[EXPIRES_KEY] = expires
    return json.dumps(obj, separators=(",", ":")).encode("utf-8")


def parse_update(text: bytes):
    """A device's rules for a credentials_update's plaintext (mgcommands.h, Device session): ((access, refresh),
    access_expires_at or 0), or None for invalid_value. The first byte is '{' (json.loads alone would take leading
    whitespace). access_expires_at counts only as a positive integer that fits
    64 bits (any other form: none); a duplicate access_token, refresh_token or access_expires_at is invalid."""
    known = ("access_token", "refresh_token", EXPIRES_KEY)

    def pairs(items):
        seen = set()
        for k, _ in items:
            if k in known:
                if k in seen:
                    raise ValueError("duplicate " + k)
                seen.add(k)
        return dict(items)
    if text[:1] != b"{":
        return None
    try:
        obj = json.loads(text, object_pairs_hook=pairs)
    except ValueError:
        return None
    if not isinstance(obj, dict) or not all(isinstance(obj.get(k), str) and obj[k] for k in known[:2]):
        return None
    v = obj.get(EXPIRES_KEY)
    return (obj["access_token"], obj["refresh_token"]), (v if type(v) is int and 0 < v < 1 << 64 else 0)


def saved_frame(ok: bool) -> bytes:
    """The unsealed answer: older firmware, or a session opened with an unsealed hello."""
    return bytes([MG_COMMAND_SESSION, MG_SESSION_CREDENTIALS_SAVED, 1 if ok else 0])


SAVED_HASH_LEN = 8  # MG_SESSION_SAVED_HASH_LEN
SAVED_WRITE = 4096  # a sealed credentials_saved fits one write at any MTU >= MG_MIN_ATT_MTU


def saved_plaintext(ok: bool, refresh: str) -> bytes:
    """credentials_saved plaintext: result, then SHA-256(UTF-8 refresh_token)[:8] (zeros when not saved)."""
    return bytes([1]) + hashlib.sha256(refresh.encode("utf-8")).digest()[:SAVED_HASH_LEN] if ok \
        else bytes(1 + SAVED_HASH_LEN)


def sealed_saved_frame(enc_key: bytes, mac_key: bytes, counter: int, ok: bool, refresh: str) -> bytes:
    """A sealed credentials_saved: the next device-to-client counter, fragmented as data in one write."""
    blob = seal(enc_key, mac_key, DEVICE_TO_CLIENT, counter, saved_plaintext(ok, refresh))
    frames = fragments(MG_SESSION_CREDENTIALS_SAVED, blob, True, SAVED_WRITE)
    assert len(frames) == 1
    return frames[0]


def app_saved_verdict(enc_key: bytes, mac_key: bytes, expected: int, frame: bytes, sent_refresh: str):
    """How a client reads one credentials_saved after a sealed update:
    (verdict, the next device-to-client counter it expects). Verdicts: "saved", "not saved", "confirms nothing"
    (an unsealed [1]: the client waits for a sealed answer and assumes saved after 35 s of link), "refused"."""
    if len(frame) == 3:  # unsealed [session, credentials_saved, result]: uses no counter
        return ("not saved" if frame[2] == 0 else "confirms nothing"), expected
    if frame[2] & (MG_SESSION_FLAG_FIRST | MG_SESSION_FLAG_LAST | MG_SESSION_FLAG_SEALED) != 0x07:
        return "refused", expected
    blob = frame[MG_SESSION_FIRST_HEADER:]
    if int.from_bytes(frame[3:7], "little") != len(blob):
        return "refused", expected
    pt = unseal(enc_key, mac_key, DEVICE_TO_CLIENT, expected, blob)
    if pt is None or len(pt) != 1 + SAVED_HASH_LEN:
        return "refused", expected
    want = hashlib.sha256(sent_refresh.encode("utf-8")).digest()[:SAVED_HASH_LEN]
    return ("saved" if pt[0] == 1 and pt[1:] == want else "not saved"), expected + 1


def expires_update(access: str, refresh: str, raw: str | None) -> bytes:
    """An update's plaintext with access_expires_at written exactly as raw (None: absent)."""
    body = f'{{"access_token":"{access}","refresh_token":"{refresh}"'
    if raw is not None:
        body += "," + raw
    return (body + "}").encode("utf-8")


EXPIRES_NODE = "homelink-1a2b3c"
EXPIRES_K = det("seal K expires", 32)
EXPIRES_SESSIONS = [(det("expires cn 1", 16), det("expires dn 1", 16)),
                    (det("expires cn 2", 16), det("expires dn 2", 16)),
                    (det("expires cn 3", 16), det("expires dn 3", 16))]
# (session, what, raw access_expires_at member or None[, text before the object]) per update; a credentials step
# opens each session.
EXPIRES_UPDATES = [
    (0, "an update carrying access_expires_at: kept with the pair", '"access_expires_at":1893456000'),
    (1, "an update without it: the stored one is cleared", None),
    (1, "a string: stored as none", '"access_expires_at":"1893456000"'),
    (1, "a fraction: stored as none", '"access_expires_at":1.5'),
    (1, "negative: stored as none", '"access_expires_at":-5'),
    (1, "null: stored as none", '"access_expires_at":null'),
    (1, "2^64, past 64 bits: stored as none", '"access_expires_at":18446744073709551616'),
    (1, "the largest that fits (2^64 - 1): kept", '"access_expires_at":18446744073709551615'),
    (1, "a duplicate access_expires_at: invalid_value, nothing stored",
     '"access_expires_at":1,"access_expires_at":2'),
    (1, "a space before the object (the first byte must be '{'): invalid_value, nothing stored",
     '"access_expires_at":1', " "),
]


def access_expires_at() -> dict:
    """access_expires_at kept with the pair: sealed credentials and credentials_updates over three proofs (one sealed
    session each), byte for byte, with what the device answers and keeps after each step."""
    pair, exp = ("mga_exp_access_0", "mgr_exp_refresh_0"), 0
    steps, n = [], 0
    for sess, (cn, dn) in enumerate(EXPIRES_SESSIONS):
        e, m = seal_keys(EXPIRES_K, cn, dn, EXPIRES_NODE)
        pt = creds_with_expires(pair[0], pair[1], EXPIRES_NODE, exp)
        steps.append({"step": f"session {sess + 1}: the credentials" + (" (access_expires_at handed back)" if exp
                                                                      else " (none kept)"),
                      "session": sess, "direction": DEVICE_TO_CLIENT, "counter": 0, "plaintext": pt.decode(),
                      "sealed": seal(e, m, DEVICE_TO_CLIENT, 0, pt).hex(), "device": None, "stored": exp})
        ctr = 0
        for s_i, what, raw, *lead in EXPIRES_UPDATES:
            if s_i != sess:
                continue
            n += 1
            pt = "".join(lead).encode() + expires_update(f"mga_exp_access_{n}", f"mgr_exp_refresh_{n}", raw)
            r = parse_update(pt)
            if r is None:
                answer = error_frame(MG_ERROR_CODE_INVALID_VALUE)
            else:
                pair, exp = r
                answer = saved_frame(True)  # the storage rule only; the sealed form: sealed_credentials_saved
            steps.append({"step": what, "session": sess, "direction": CLIENT_TO_DEVICE, "counter": ctr,
                          "plaintext": pt.decode(), "sealed": seal(e, m, CLIENT_TO_DEVICE, ctr, pt).hex(),
                          "device": answer.hex(), "stored": exp})
            ctr += 1
    return {
        "name": "access_expires_at kept with the pair",
        "K": EXPIRES_K.hex(),
        "node_id": EXPIRES_NODE,
        "sessions": [{"client_nonce": cn.hex(), "device_nonce": dn.hex()} for cn, dn in EXPIRES_SESSIONS],
        "steps": steps,
    }


def check_expires(v: dict) -> list[str]:
    """Replays the access_expires_at section through the rules above."""
    bad = []
    k = bytes.fromhex(v["K"])
    pair, exp = ("mga_exp_access_0", "mgr_exp_refresh_0"), 0
    for i, st in enumerate(v["steps"]):
        ses = v["sessions"][st["session"]]
        e, m = seal_keys(k, bytes.fromhex(ses["client_nonce"]), bytes.fromhex(ses["device_nonce"]), v["node_id"])
        pt = unseal(e, m, st["direction"], st["counter"], bytes.fromhex(st["sealed"]))
        if pt is None or pt.decode() != st["plaintext"]:
            bad.append(f"access_expires_at step {i}: unseal")
            continue
        if st["direction"] == DEVICE_TO_CLIENT:
            if pt != creds_with_expires(pair[0], pair[1], v["node_id"], exp):
                bad.append(f"access_expires_at step {i}: credentials")
        else:
            r = parse_update(pt)
            want = error_frame(MG_ERROR_CODE_INVALID_VALUE) if r is None else saved_frame(True)
            if r is not None:
                pair, exp = r
            if st["device"] != want.hex():
                bad.append(f"access_expires_at step {i}: answer")
        if st["stored"] != exp:
            bad.append(f"access_expires_at step {i}: stored")
    if not any(s["stored"] == (1 << 64) - 1 and s["direction"] == DEVICE_TO_CLIENT for s in v["steps"]):
        bad.append("access_expires_at: the longest form never handed back")
    return bad


SAVED_NODE = "homelink-5a7e00"
SAVED_K = det("seal K saved", 32)
SAVED_CN, SAVED_DN = det("saved cn", 16), det("saved dn", 16)
# (what, the device's answers to that update: ("sealed", ok, hash of which update's refresh) or ("unsealed", result))
SAVED_PLAN = [
    ("saved: sealed [1] with the hash of this pair", [("sealed", True, 0)]),
    ("not saved: sealed [0] (also a deferred write-back that gave up)", [("sealed", False, 0)]),
    ("a sealed [1] with another pair's hash: not saved", [("sealed", True, -1)]),
    ("an unsealed [1] confirms nothing and uses no counter; the sealed [1] after it decides",
     [("unsealed", 1), ("sealed", True, 0)]),
    ("an unsealed [0]: not saved (it can only cause a resend)", [("unsealed", 0)]),
]
EXPECTED_SAVED_VERDICTS = [["saved"], ["not saved"], ["not saved"], ["confirms nothing", "saved"], ["not saved"]]


def sealed_credentials_saved() -> dict:
    """Sealed credentials_saved: credentials_updates in one sealed session and every form of answer,
    byte for byte, with how the client reads each."""
    e, m = seal_keys(SAVED_K, SAVED_CN, SAVED_DN, SAVED_NODE)
    creds = creds_json("mga_saved_access_0", "mgr_saved_refresh_0", SAVED_NODE)
    steps = [{"step": "device: credentials (counter 0)", "client": None, "update_counter": None,
              "device": [fragments(MG_SESSION_CREDENTIALS, seal(e, m, DEVICE_TO_CLIENT, 0, creds), True,
                                   SAVED_WRITE)[0].hex()], "verdicts": None}]
    refreshes = []
    dev_ctr = 1
    for n, (what, answers) in enumerate(SAVED_PLAN):
        refresh = f"mgr_saved_refresh_{n + 1}"
        refreshes.append(refresh)
        upd = expires_update(f"mga_saved_access_{n + 1}", refresh, '"access_expires_at":1893456000')
        client = fragments(MG_SESSION_CREDENTIALS_UPDATE, seal(e, m, CLIENT_TO_DEVICE, n, upd), True, SAVED_WRITE)
        frames, verdicts = [], []
        for a in answers:
            if a[0] == "unsealed":
                f = saved_frame(a[1] == 1)
            else:
                f = sealed_saved_frame(e, m, dev_ctr, a[1], refreshes[n + a[2]] if a[2] else refresh)
                dev_ctr += 1
            frames.append(f)
        exp = 1
        for f in frames:
            v, exp = app_saved_verdict(e, m, exp if exp > 1 else (dev_ctr - sum(1 for a in answers
                                                                              if a[0] == "sealed")), f, refresh)
            verdicts.append(v)
        steps.append({"step": what, "client": [c.hex() for c in client], "update_counter": n,
                      "update_plaintext": upd.decode(), "device": [f.hex() for f in frames], "verdicts": verdicts})
    return {
        "name": "sealed credentials_saved",
        "K": SAVED_K.hex(),
        "client_nonce": SAVED_CN.hex(),
        "device_nonce": SAVED_DN.hex(),
        "node_id": SAVED_NODE,
        "app_timeout_s": 35,
        "note": "Plaintext = result || SHA-256(refresh_token)[:8] (zeros when not saved), sealed under the next "
                "device-to-client counter and fragmented as data in one write. The client counts only a sealed [1] "
                "with the matching hash as saved; with no sealed answer within 35 s while the link stays up it "
                "assumes saved (older firmware answers unsealed); a dropped link keeps the pair pending.",
        "steps": steps,
    }


def check_saved(v: dict) -> list[str]:
    """Replays the sealed_credentials_saved section through app_saved_verdict and the device's rule."""
    bad = []
    e, m = seal_keys(bytes.fromhex(v["K"]), bytes.fromhex(v["client_nonce"]), bytes.fromhex(v["device_nonce"]),
                     v["node_id"])
    expected = 1
    refreshes = []
    verdicts = []
    for st in v["steps"][1:]:
        upd = st["update_plaintext"].encode()
        frames = [bytes.fromhex(c) for c in st["client"]]
        blob = b"".join([frames[0][MG_SESSION_FIRST_HEADER:]] + [f[MG_SESSION_DATA_HEADER:] for f in frames[1:]])
        if unseal(e, m, CLIENT_TO_DEVICE, st["update_counter"], blob) != upd:
            bad.append(f"{st['step']}: the update doesn't unseal")
        r = parse_update(upd)
        refresh = r[0][1] if r else ""
        refreshes.append(refresh)
        got = []
        for f in (bytes.fromhex(x) for x in st["device"]):
            if len(f) > 3 and len(f) != MG_SESSION_FIRST_HEADER + COUNTER_LEN + 1 + SAVED_HASH_LEN + TAG_LEN:
                bad.append(f"{st['step']}: a sealed credentials_saved is 36 bytes")
            verdict, expected = app_saved_verdict(e, m, expected, f, refresh)
            got.append(verdict)
        verdicts.append(got)
        if got != st["verdicts"]:
            bad.append(f"{st['step']}: verdicts {got} vs {st['verdicts']}")
    if verdicts != EXPECTED_SAVED_VERDICTS:
        bad.append(f"sealed_credentials_saved verdicts {verdicts}")
    # A sealed [1] replayed into the next update's answer slot must not count: wrong counter.
    first = bytes.fromhex(v["steps"][1]["device"][0])
    if app_saved_verdict(e, m, 2, first, refreshes[0])[0] != "refused":
        bad.append("a replayed sealed credentials_saved must be refused")
    return bad


INPUTS = [
    {
        "name": "credentials (device to client)",
        "K": det("seal K basic", 32),
        "client_nonce": bytes(range(16)),
        "device_nonce": bytes(range(0xF0, 0x100)),
        "node_id": "homelink-1a2b3c",
        "direction": DEVICE_TO_CLIENT,
        "counter": 0,
        "plaintext": creds_json("mga_test_access_" + det("acc", 24).hex(), "mga_test_refresh_" + det("ref", 16).hex(),
                                "homelink-1a2b3c"),
    },
    {
        "name": "credentials_update (client to device)",
        "K": det("seal K basic", 32),
        "client_nonce": bytes(range(16)),
        "device_nonce": bytes(range(0xF0, 0x100)),
        "node_id": "homelink-1a2b3c",
        "direction": CLIENT_TO_DEVICE,
        "counter": 0,
        "plaintext": creds_json("mga_test_access_2_" + det("acc2", 24).hex(), "mga_test_refresh_2_" + det("ref2", 16).hex()),
    },
    {
        "name": "token-sized credentials, second message",
        "K": det("seal K long", 32),
        "client_nonce": det("seal cn long", 16),
        "device_nonce": det("seal dn long", 16),
        "node_id": "homelink-a7f5b4",
        "direction": DEVICE_TO_CLIENT,
        "counter": 1,
        "plaintext": creds_json("eyJ" + det("long access", 300).hex(), det("long refresh", 72).hex(), "homelink-a7f5b4"),
    },
]


# --- Sealed data --------------------------------------------------------------------------------------------------
# In a sealed session every message with a payload (credentials, credentials_update, data) is sealed, with one
# counter per direction shared by all of them. A message is sealed whole, then fragmented: total_len counts the
# sealed bytes. The receiver reads the counter from the first fragment (it must be the next one); a message aborted
# after its first fragment has used its counter on both sides. Messages without a payload (state, open_request,
# credit, abort) are not sealed.

# A client's data message carries one ServiceResponseEnvelope from the VM (field 1: a ServiceFrame), which a device
# decodes before anything else; one that doesn't decode is refused invalid_value, its counter used. The client data
# here is real envelopes: a body chunk on stream 3 (a stream the device opened) carrying stand-in bytes.

VM_STREAM = 3


def _varint(v: int) -> bytes:
    out = bytearray()
    while True:
        b, v = v & 0x7F, v >> 7
        out.append(b | (0x80 if v else 0))
        if not v:
            return bytes(out)


def _delimited(num: int, data: bytes) -> bytes:
    return _varint(num << 3 | 2) + _varint(len(data)) + data


def vm_envelope(data: bytes, stream: int = VM_STREAM) -> bytes:
    """ServiceResponseEnvelope{frame: ServiceFrame{stream_id, body_chunk{data}}}, as the VM encoder writes it."""
    return _delimited(1, _varint(1 << 3) + _varint(stream) + _delimited(4, _delimited(1, data)))


def vm_data(label: str, n: int) -> bytes:
    return vm_envelope(det(label, n))


def is_response_envelope(message: bytes) -> bool:
    """Decodes as noise_core's ServiceCodec decodes a ServiceResponseEnvelope: each key a field number 1..2^29-1
    outside 19000-19999 and wire type 0, 1, 2 or 5 (field 1 delimited), varints of at most 10 bytes, every value inside
    the message. A lone 0x00, or anything starting 0x00, 0x01 or '{', never does."""
    i = 0

    def varint() -> int:
        nonlocal i
        v = 0
        for n in range(10):
            if i >= len(message) or (n == 9 and message[i] & 0xFE):
                raise ValueError
            v |= (message[i] & 0x7F) << 7 * n
            i += 1
            if not message[i - 1] & 0x80:
                return v
        raise ValueError
    try:
        while i < len(message):
            key = varint()
            num, wire = key >> 3, key & 7
            if not 0 < num < 1 << 29 or 19000 <= num <= 19999 or wire not in (0, 1, 2, 5) or (num == 1 and wire != 2):
                return False
            if wire == 0:
                varint()
                continue
            n = varint() if wire == 2 else {1: 8, 5: 4}[wire]
            if n > len(message) - i:
                return False
            i += n
    except ValueError:
        return False
    return True


def fragments(sub: int, payload: bytes, sealed: bool, max_write: int) -> list[bytes]:
    """[session, sub, flags, total_len u32 LE (first only), bytes], each write at most max_write bytes."""
    out, i, first = [], 0, True
    while first or i < len(payload):
        room = max_write - (MG_SESSION_FIRST_HEADER if first else MG_SESSION_DATA_HEADER)
        chunk = payload[i:i + room]
        i += len(chunk)
        flags = ((MG_SESSION_FLAG_FIRST if first else 0) | (MG_SESSION_FLAG_LAST if i >= len(payload) else 0)
                 | (MG_SESSION_FLAG_SEALED if sealed else 0))
        out.append(bytes([MG_COMMAND_SESSION, sub, flags])
                   + (len(payload).to_bytes(4, "little") if first else b"") + chunk)
        first = False
    return out


class SealedReceiver:
    """One direction of a sealed session, with the receiving rules of mg_session_ref.py's Device. A message takes its
    counter at its first fragment,
    which must carry the whole counter (else invalid_length) and the next counter (or the one open hole), with a
    total_len over the 20 sealed bytes and within max_message; its fragments then add up to total_len exactly, the
    last one marked last. Data and credentials_update reassemble separately, so two sealed messages can be in flight
    together; a first fragment while one of its kind is open drops both. A message refused at its tag doesn't use its
    counter: if nothing later took a counter it is given back, otherwise it becomes a hole accepted once, while it is
    at most 255 back (one hole; a newer one replaces it). An aborted or malformed message uses its counter, and so does
    client data whose plaintext isn't a ServiceResponseEnvelope (refused invalid_value)."""

    def __init__(self, enc_key: bytes, mac_key: bytes, direction: int,
                 max_message: int = MG_SESSION_MAX_MESSAGE) -> None:
        self.e, self.m, self.direction = enc_key, mac_key, direction
        self.max_message = max_message
        self.expected = 0
        self.hole: int | None = None
        self.bufs: dict[int, tuple[int, int, bytearray]] = {}  # sub-command -> (counter, total_len, sealed bytes)

    def receive(self, frame: bytes) -> tuple[str, bytes | None]:
        """("partial", None), ("message", plaintext), ("aborted", None) or ("refused", error frame)."""
        sub, flags = frame[1], frame[2] if len(frame) > 2 else 0
        if sub == MG_SESSION_ABORT:
            self.bufs.pop(MG_SESSION_DATA, None)  # its counter stays used
            return ("aborted", None)
        if flags & MG_SESSION_FLAG_FIRST:
            if not flags & MG_SESSION_FLAG_SEALED:
                return ("refused", error_frame(MG_ERROR_CODE_INSECURE))  # no unsealed payload in a sealed session
            body = frame[MG_SESSION_FIRST_HEADER:]
            if len(body) < COUNTER_LEN:
                self.bufs.pop(sub, None)
                return ("refused", error_frame(MG_ERROR_CODE_INVALID_LENGTH))
            total = int.from_bytes(frame[3:7], "little")
            counter = int.from_bytes(body[:COUNTER_LEN], "big")
            if sub in self.bufs or not COUNTER_LEN + TAG_LEN < total <= self.max_message:
                self.bufs.pop(sub, None)
                return ("refused", error_frame(MG_ERROR_CODE_INVALID_VALUE))
            if counter == self.expected:
                self.expected += 1
                if self.hole is not None and self.expected - self.hole > MAX_HOLE_BACK:
                    self.hole = None
            elif counter == self.hole:
                self.hole = None  # a hole is accepted once
            else:
                return ("refused", error_frame(MG_ERROR_CODE_INVALID_VALUE))
            self.bufs[sub] = (counter, total, bytearray())
        elif sub not in self.bufs:
            return ("refused", error_frame(MG_ERROR_CODE_INVALID_VALUE))
        else:
            body = frame[MG_SESSION_DATA_HEADER:]
        counter, total, sealed = self.bufs[sub]
        end = len(sealed) + len(body)
        if not body or end > total or bool(flags & MG_SESSION_FLAG_LAST) != (end == total) \
                or not flags & MG_SESSION_FLAG_SEALED:
            del self.bufs[sub]  # its counter stays used
            return ("refused", error_frame(MG_ERROR_CODE_INVALID_VALUE))
        sealed.extend(body)
        if not flags & MG_SESSION_FLAG_LAST:
            return ("partial", None)
        del self.bufs[sub]
        plaintext = unseal(self.e, self.m, self.direction, counter, bytes(sealed))
        if plaintext is None:
            # Refused at its tag: the counter isn't used. Give it back if nothing later took one, else leave a hole.
            if counter == self.expected - 1:
                self.expected = counter
            elif self.expected - counter <= MAX_HOLE_BACK:
                self.hole = counter
            return ("refused", error_frame(MG_ERROR_CODE_INVALID_VALUE))
        if self.direction == CLIENT_TO_DEVICE and sub == MG_SESSION_DATA and not is_response_envelope(plaintext):
            return ("refused", error_frame(MG_ERROR_CODE_INVALID_VALUE))   # its counter stays used
        return ("message", plaintext)


DATA_NODE = "homelink-d47a00"
DATA_K = det("seal K data", 32)
DATA_CN, DATA_DN = det("data cn", 16), det("data dn", 16)
DATA_MAX_WRITE = 97  # ATT_MTU 100


def sealed_data(sealed_saved: bool = False) -> dict:
    """A sealed session carrying data both ways, interleaved with credentials and a credentials_update, plus a
    replayed counter, an unsealed data message and an aborted message. Byte for byte, with the receiver's outcome.
    sealed_saved: the device answers the update with a sealed credentials_saved, which takes the next
    device-to-client counter, so the device's later data moves up one (sealed_data_large)."""
    e, m = seal_keys(DATA_K, DATA_CN, DATA_DN, DATA_NODE)
    link_register = bytes([0x0A, 0x0D]) + b"/link-control" + bytes([0x12, 0x96, 0x01]) + det("link.register", 150)
    plan = [
        # (what, direction, sub, plaintext, sealed?, counter or None to use the next one)
        ("device: credentials (counter 0)", DEVICE_TO_CLIENT, MG_SESSION_CREDENTIALS,
         creds_json("mga_data_access", "mgr_data_refresh", DATA_NODE), True, None),
        ("device: data, its link.register (counter 1, 3 writes)", DEVICE_TO_CLIENT, MG_SESSION_DATA,
         link_register, True, None),
        ("client: data, the VM's answer (counter 0, 4 writes)", CLIENT_TO_DEVICE, MG_SESSION_DATA,
         vm_data("vm reply", 300), True, None),
        ("client: credentials_update (counter 1)", CLIENT_TO_DEVICE, MG_SESSION_CREDENTIALS_UPDATE,
         expires_update("mga_data_access_2", "mgr_data_refresh_2", '"access_expires_at":1893456000'), True, None),
        ("client: data (counter 2)", CLIENT_TO_DEVICE, MG_SESSION_DATA, vm_data("vm event", 40), True, None),
        ("client: counter 2 replayed: refused", CLIENT_TO_DEVICE, MG_SESSION_DATA, vm_data("vm event", 40), True, 2),
        ("client: unsealed data in a sealed session: refused", CLIENT_TO_DEVICE, MG_SESSION_DATA,
         vm_data("plain", 20), False, None),
        ("device: data (counter 2)", DEVICE_TO_CLIENT, MG_SESSION_DATA, det("device event", 30), True, None),
    ]
    rx = {DEVICE_TO_CLIENT: SealedReceiver(e, m, DEVICE_TO_CLIENT), CLIENT_TO_DEVICE: SealedReceiver(e, m, CLIENT_TO_DEVICE)}
    sent = {DEVICE_TO_CLIENT: 0, CLIENT_TO_DEVICE: 0}
    steps = []

    def run(what: str, direction: int, frames: list[bytes], plaintext: bytes, counter, sealed_hex) -> None:
        outcome = [rx[direction].receive(f) for f in frames][-1]
        steps.append({"step": what, "direction": direction, "counter": counter, "plaintext": plaintext.hex(),
                      "sealed": sealed_hex, "frames": [f.hex() for f in frames], "outcome": outcome[0],
                      "answer": outcome[1].hex() if outcome[0] == "refused" else None})

    for what, d, sub, pt, sealed, fixed in plan:
        if sealed:
            ctr = sent[d] if fixed is None else fixed
            if fixed is None:
                sent[d] += 1
            blob = seal(e, m, d, ctr, pt)
            if sealed_saved and d == DEVICE_TO_CLIENT and sub == MG_SESSION_DATA and fixed is None and ctr >= 2:
                what = what.replace(f"(counter {ctr - 1})", f"(counter {ctr})")
            run(what, d, fragments(sub, blob, True, DATA_MAX_WRITE), pt, ctr, blob.hex())
            if sealed_saved and sub == MG_SESSION_CREDENTIALS_UPDATE:
                refresh = parse_update(pt)[0][1]
                sp = saved_plaintext(True, refresh)
                sctr = sent[DEVICE_TO_CLIENT]
                sent[DEVICE_TO_CLIENT] += 1
                sblob = seal(e, m, DEVICE_TO_CLIENT, sctr, sp)
                run(f"device: credentials_saved, sealed (counter {sctr})", DEVICE_TO_CLIENT,
                    fragments(MG_SESSION_CREDENTIALS_SAVED, sblob, True, DATA_MAX_WRITE), sp, sctr, sblob.hex())
        else:
            run(what, d, fragments(sub, pt, False, DATA_MAX_WRITE), pt, None, None)
    # An aborted message: its counter (3) is used on both sides, so the next message is counter 4.
    blob = seal(e, m, CLIENT_TO_DEVICE, sent[CLIENT_TO_DEVICE], vm_data("aborted", 200))
    first = fragments(MG_SESSION_DATA, blob, True, DATA_MAX_WRITE)[0]
    run("client: data counter 3, first write then abort", CLIENT_TO_DEVICE,
        [first, bytes([MG_COMMAND_SESSION, MG_SESSION_ABORT])], vm_data("aborted", 200), sent[CLIENT_TO_DEVICE], None)
    sent[CLIENT_TO_DEVICE] += 1
    pt = vm_data("after abort", 25)
    blob = seal(e, m, CLIENT_TO_DEVICE, sent[CLIENT_TO_DEVICE], pt)
    run("client: data (counter 4, after the abort)", CLIENT_TO_DEVICE,
        fragments(MG_SESSION_DATA, blob, True, DATA_MAX_WRITE), pt, sent[CLIENT_TO_DEVICE], blob.hex())
    sent[CLIENT_TO_DEVICE] += 1
    # A bad tag does not use the counter: counter 5 with a flipped tag is refused, then the real counter 5 is accepted.
    pt = vm_data("tag check", 30)
    blob = seal(e, m, CLIENT_TO_DEVICE, sent[CLIENT_TO_DEVICE], pt)
    forged = blob[:-1] + bytes([blob[-1] ^ 0x01])
    run("client: data counter 5 with a bad tag: refused, counter not used", CLIENT_TO_DEVICE,
        fragments(MG_SESSION_DATA, forged, True, DATA_MAX_WRITE), pt, sent[CLIENT_TO_DEVICE], forged.hex())
    run("client: the real data counter 5: accepted", CLIENT_TO_DEVICE,
        fragments(MG_SESSION_DATA, blob, True, DATA_MAX_WRITE), pt, sent[CLIENT_TO_DEVICE], blob.hex())
    return {
        "name": "sealed data in a sealed session" + (", with a sealed credentials_saved" if sealed_saved else ""),
        "K": DATA_K.hex(),
        "client_nonce": DATA_CN.hex(),
        "device_nonce": DATA_DN.hex(),
        "node_id": DATA_NODE,
        "max_write": DATA_MAX_WRITE,
        "note": "This receiver keeps the session after a refusal to show the counter rules; a device may end it "
                "instead, and a client stops using a session after any refusal (re-prove, one sealed hello).",
        "steps": steps,
    }


INTERLEAVED_CN, INTERLEAVED_DN = det("interleaved cn", 16), det("interleaved dn", 16)


def sealed_interleaved() -> dict:
    """Two sealed messages in flight together (client to device): an update takes counter 0 at its first fragment,
    data takes counter 1 and completes, then the update is refused at its tag. Counter 0 stays open once (a hole): the
    real update 0 is accepted, its replay refused, and counters continue from 2."""
    e, m = seal_keys(DATA_K, INTERLEAVED_CN, INTERLEAVED_DN, DATA_NODE)
    rx = SealedReceiver(e, m, CLIENT_TO_DEVICE)
    update = expires_update("mga_interleaved_access", "mgr_interleaved_refresh", '"access_expires_at":1893456000')
    real_update = seal(e, m, CLIENT_TO_DEVICE, 0, update)
    forged_update = real_update[:-1] + bytes([real_update[-1] ^ 0x01])
    forged_frames = fragments(MG_SESSION_CREDENTIALS_UPDATE, forged_update, True, DATA_MAX_WRITE)
    data1_pt, data2_pt = vm_data("interleaved data 1", 24), vm_data("interleaved data 2", 24)
    data1 = seal(e, m, CLIENT_TO_DEVICE, 1, data1_pt)
    data2 = seal(e, m, CLIENT_TO_DEVICE, 2, data2_pt)
    plan = [
        ("update counter 0 (bad tag): first write", [forged_frames[0]], 0, update),
        ("data counter 1, whole, while the update is in flight", fragments(MG_SESSION_DATA, data1, True,
                                                                        DATA_MAX_WRITE), 1, data1_pt),
        ("the rest of the update: refused at its tag, counter 0 left open once", forged_frames[1:], 0, update),
        ("the real update counter 0: accepted (the hole)", fragments(MG_SESSION_CREDENTIALS_UPDATE, real_update, True,
                                                                     DATA_MAX_WRITE), 0, update),
        ("the real update counter 0 replayed: refused", fragments(MG_SESSION_CREDENTIALS_UPDATE, real_update, True,
                                                                  DATA_MAX_WRITE), 0, update),
        ("data counter 2: accepted, counters continue", fragments(MG_SESSION_DATA, data2, True, DATA_MAX_WRITE), 2,
         data2_pt),
    ]
    steps = []
    for what, frames, counter, pt in plan:
        outcome = [rx.receive(f) for f in frames][-1]
        steps.append({"step": what, "direction": CLIENT_TO_DEVICE, "counter": counter, "plaintext": pt.hex(),
                      "frames": [f.hex() for f in frames], "outcome": outcome[0],
                      "answer": outcome[1].hex() if outcome[0] == "refused" else None})
    return {
        "name": "two sealed messages in flight, the earlier refused at its tag",
        "K": DATA_K.hex(),
        "client_nonce": INTERLEAVED_CN.hex(),
        "device_nonce": INTERLEAVED_DN.hex(),
        "node_id": DATA_NODE,
        "max_write": DATA_MAX_WRITE,
        "steps": steps,
    }


EXPECTED_INTERLEAVED_OUTCOMES = ["partial", "message", "refused", "message", "refused", "message"]


def check_interleaved(v: dict) -> list[str]:
    outcomes = [s["outcome"] for s in v["steps"]]
    bad = [] if outcomes == EXPECTED_INTERLEAVED_OUTCOMES else [f"interleaved outcomes {outcomes}"]
    for i in (2, 4):
        if v["steps"][i]["answer"] != error_frame(MG_ERROR_CODE_INVALID_VALUE).hex():
            bad.append(f"interleaved step {i}: must be refused with invalid_value")
    return bad


# --- The receiver's limits ----------------------------------------------------------------------------------------
# What a receiver refuses before or while it reassembles a sealed message, and what that does to the counter: a first
# fragment without the whole counter (invalid_length), a total_len of 20 or less or over max_message, or a first
# fragment while one of its kind is open (invalid_value, no counter taken); fragments that don't add up to total_len
# exactly or lose the sealed flag (invalid_value, the counter used); a hole that falls more than 255 counters back.

LIMITS_MAX_MESSAGE = 4096
LIMITS_CASES = ["a first write without the whole counter", "total_len over max_message", "total_len 20: no plaintext",
                "a write marked last short of total_len", "bytes past total_len",
                "a first write while a message is open", "a continuation without the sealed flag",
                "a hole more than 255 counters back"]


def receiver_limits() -> dict:
    """Each case in a session of its own (its own nonces), client to device, replayed through SealedReceiver."""
    cases = []
    for i, name in enumerate(LIMITS_CASES):
        cn, dn = det(f"limits cn {i}", 16), det(f"limits dn {i}", 16)
        e, m = seal_keys(DATA_K, cn, dn, DATA_NODE)
        rx = SealedReceiver(e, m, CLIENT_TO_DEVICE, LIMITS_MAX_MESSAGE)
        steps = []

        def run(what: str, frames: list[bytes]) -> None:
            outcome = [rx.receive(f) for f in frames][-1]
            steps.append({"step": what, "frames": [f.hex() for f in frames], "outcome": outcome[0],
                          "plaintext": outcome[1].hex() if outcome[0] == "message" else None,
                          "answer": outcome[1].hex() if outcome[0] == "refused" else None})

        def data(counter: int, n: int = 150) -> bytes:
            return seal(e, m, CLIENT_TO_DEVICE, counter, vm_data(f"limits {i} data {counter}", n))

        def first(blob: bytes, total: int = None, flags: int = MG_SESSION_FLAG_FIRST | MG_SESSION_FLAG_SEALED,
                  n: int = DATA_MAX_WRITE - MG_SESSION_FIRST_HEADER) -> bytes:
            return (bytes([MG_COMMAND_SESSION, MG_SESSION_DATA, flags])
                    + (len(blob) if total is None else total).to_bytes(4, "little") + blob[:n])

        def whole(blob: bytes, sub: int = MG_SESSION_DATA) -> list[bytes]:
            return fragments(sub, blob, True, DATA_MAX_WRITE)

        b0, b1 = data(0), data(1)
        if i == 0:
            run("data counter 0, its first write with 3 of the counter's 4 bytes: invalid_length, the counter not "
                "taken", [first(b0, n=3)])
            run("data counter 0, whole: accepted", whole(b0))
        elif i == 1:
            run(f"data counter 0, its first write with total_len {LIMITS_MAX_MESSAGE + 1} (over max_message "
                f"{LIMITS_MAX_MESSAGE}): invalid_value, the counter not taken",
                [first(b0, total=LIMITS_MAX_MESSAGE + 1)])
            run("data counter 0, whole: accepted", whole(b0))
        elif i == 2:
            empty = seal(e, m, CLIENT_TO_DEVICE, 0, b"")
            run("data counter 0 with no plaintext, total_len 20: invalid_value, the counter not taken",
                [first(empty, flags=0x07)])
            run("data counter 0, whole: accepted", whole(b0))
        elif i == 3:
            small = data(0, 40)
            run("data counter 0 in one write marked last, total_len 5 more than it carries: invalid_value, the "
                "counter used", [first(small, total=len(small) + 5, flags=0x07)])
            run("data counter 1, whole: accepted", whole(b1))
        elif i == 4:
            frames = whole(b0)
            run("data counter 0, its first write", frames[:1])
            run("the rest with one byte past total_len: invalid_value, the counter used",
                frames[1:-1] + [frames[-1] + b"\x00"])
            run("data counter 0 again, whole: refused (used)", whole(b0))
            run("data counter 1, whole: accepted", whole(b1))
        elif i == 5:
            run("data counter 0, its first write", whole(b0)[:1])
            run("data counter 1, a first write while counter 0's is open: invalid_value, both dropped, counter 1 "
                "not taken", whole(b1)[:1])
            run("data counter 1, whole: accepted", whole(b1))
        elif i == 6:
            frames = whole(b0)
            run("data counter 0, its first write", frames[:1])
            run("the rest without the sealed flag: invalid_value, the counter used",
                [f[:2] + bytes([f[2] & ~MG_SESSION_FLAG_SEALED]) + f[3:] for f in frames[1:]])
            run("data counter 1, whole: accepted", whole(b1))
        else:
            update = expires_update("mga_limits_access", "mgr_limits_refresh", '"access_expires_at":1893456000')
            real = seal(e, m, CLIENT_TO_DEVICE, 0, update)
            forged = whole(real[:-1] + bytes([real[-1] ^ 0x01]), MG_SESSION_CREDENTIALS_UPDATE)
            run("credentials_update counter 0 (bad tag): its first write", forged[:1])
            last = MAX_HOLE_BACK - 1
            run(f"data counters 1 to {last}, one write each: each accepted",
                [f for c in range(1, last + 1) for f in whole(data(c, 1))])
            run(f"the rest of the update: refused at its tag, counter 0 left open once ({MAX_HOLE_BACK} back)",
                forged[1:])
            run(f"data counter {last + 1}: accepted; counter 0 is now {MAX_HOLE_BACK + 1} back, so it closes",
                whole(data(last + 1, 1)))
            run("the real update counter 0: refused (no hole any more)", whole(real, MG_SESSION_CREDENTIALS_UPDATE))
            run(f"data counter {last + 2}: accepted", whole(data(last + 2, 1)))
        cases.append({"name": name, "client_nonce": cn.hex(), "device_nonce": dn.hex(), "steps": steps})
    return {
        "name": "the receiver's limits (client to device)",
        "K": DATA_K.hex(),
        "node_id": DATA_NODE,
        "direction": CLIENT_TO_DEVICE,
        "max_write": DATA_MAX_WRITE,
        "max_message": LIMITS_MAX_MESSAGE,
        "note": "Each case is a session of its own (its nonces), the receiver's counter from 0; a step's outcome is "
                "its last frame's. A refusal before the counter is taken leaves it for the next message; one after "
                "it (a malformed fragment) uses it, like an abort.",
        "cases": cases,
    }


EXPECTED_LIMITS_OUTCOMES = [["refused", "message"], ["refused", "message"], ["refused", "message"],
                            ["refused", "message"], ["partial", "refused", "refused", "message"],
                            ["partial", "refused", "message"], ["partial", "refused", "message"],
                            ["partial", "message", "refused", "message", "refused", "message"]]


def check_limits(v: dict) -> list[str]:
    """Replays the receiver_limits section through SealedReceiver."""
    bad = []
    k = bytes.fromhex(v["K"])
    outcomes = []
    for case in v["cases"]:
        e, m = seal_keys(k, bytes.fromhex(case["client_nonce"]), bytes.fromhex(case["device_nonce"]), v["node_id"])
        rx = SealedReceiver(e, m, v["direction"], v["max_message"])
        outcomes.append([s["outcome"] for s in case["steps"]])
        for s in case["steps"]:
            frames = [bytes.fromhex(f) for f in s["frames"]]
            if any(len(f) > v["max_write"] for f in frames):
                bad.append(f"{case['name']}: {s['step']}: a write over max_write")
            got = [rx.receive(f) for f in frames][-1]
            field = {"message": "plaintext", "refused": "answer"}.get(got[0])
            if got[0] != s["outcome"] or (field and got[1].hex() != s[field]):
                bad.append(f"{case['name']}: {s['step']}: {got[0]} on replay")
    if outcomes != EXPECTED_LIMITS_OUTCOMES:
        bad.append(f"receiver limits outcomes {outcomes}")
    short = v["cases"][0]["steps"][0]["answer"]
    if short != error_frame(MG_ERROR_CODE_INVALID_LENGTH).hex():
        bad.append("a first write without the whole counter must be refused with invalid_length")
    return bad


EXPECTED_DATA_OUTCOMES = ["message", "message", "message", "message", "message", "refused", "refused", "message",
                          "aborted", "message", "refused", "message"]


def check_data(v: dict, sealed_saved: bool = False) -> list[str]:
    bad = []
    o = 1 if sealed_saved else 0  # one more step (the sealed credentials_saved) after the update
    outcomes = [s["outcome"] for s in v["steps"]]
    expected = EXPECTED_DATA_OUTCOMES[:4] + ["message"] + EXPECTED_DATA_OUTCOMES[4:] if sealed_saved \
        else EXPECTED_DATA_OUTCOMES
    if outcomes != expected:
        bad.append(f"sealed data outcomes {outcomes}")
    if sealed_saved:
        st = v["steps"][4]
        if st["direction"] != DEVICE_TO_CLIENT or st["counter"] != 2 or len(bytes.fromhex(st["plaintext"])) != 9:
            bad.append("sealed_data_large: the sealed credentials_saved must take device counter 2")
        dev = [s for s in v["steps"] if s["direction"] == DEVICE_TO_CLIENT]
        if [s["counter"] for s in dev] != [0, 1, 2, 3]:
            bad.append(f"sealed_data_large: device counters {[s['counter'] for s in dev]}")
    if v["steps"][5 + o]["answer"] != error_frame(MG_ERROR_CODE_INVALID_VALUE).hex():
        bad.append("a replayed data counter must be refused with invalid_value")
    if v["steps"][6 + o]["answer"] != error_frame(MG_ERROR_CODE_INSECURE).hex():
        bad.append("unsealed data in a sealed session must be refused with insecure")
    if v["steps"][10 + o]["answer"] != error_frame(MG_ERROR_CODE_INVALID_VALUE).hex() or v["steps"][11 + o]["counter"] != v["steps"][10 + o]["counter"]:
        bad.append("a bad tag must be refused with invalid_value without using its counter")
    for s in v["steps"]:
        frames = [bytes.fromhex(f) for f in s["frames"]]
        if any(len(f) > v["max_write"] for f in frames):
            bad.append(f"{s['step']}: a write over max_write")
        if s["sealed"] and int.from_bytes(frames[0][3:7], "little") != len(bytes.fromhex(s["sealed"])):
            bad.append(f"{s['step']}: total_len must count the sealed bytes")
        if s["sealed"] and len(bytes.fromhex(s["sealed"])) != len(bytes.fromhex(s["plaintext"])) + COUNTER_LEN + TAG_LEN:
            bad.append(f"{s['step']}: sealing adds 20 bytes")
    return bad


# --- The VM stream restarted in the same sealed session ----------------------------------------------------------
# A gadget never ends its sealed session silently: a gadget-side stall or error ends only the VM stream (the gadget's
# open_request [0], answered by the client's state closed), and the gadget asks again with open_request [1] in the
# same sealed session. A state that ends the session drops any partial message: its counter, taken at its first
# fragment, stays used on both sides, and the counters go on (they restart only at a sealed hello). A sealed
# credentials_saved owed while a device message is part way out goes after that message's last fragment or after
# the state that cuts it off, never between its fragments: the Muse apps count a device message's counter when it
# completes or a state cuts it off (they never interleave).

MG_SESSION_STATE = 1
MG_SESSION_OPEN_REQUEST = 2
MG_SESSION_STATE_CLOSED = 0
MG_SESSION_STATE_OPEN = 2
RESTART_CN, RESTART_DN = det("restart cn", 16), det("restart dn", 16)


def state_frame(state: int, epoch: int) -> bytes:
    """[session, state, state, epoch (uint16 LE)]."""
    return bytes([MG_COMMAND_SESSION, MG_SESSION_STATE, state]) + epoch.to_bytes(2, "little")


def open_request_frame(want_open: bool) -> bytes:
    return bytes([MG_COMMAND_SESSION, MG_SESSION_OPEN_REQUEST, 1 if want_open else 0])


def end_messages(rx: dict) -> None:
    """A state other than open (or a new epoch) ends the session's messages on both sides: a partial one is dropped,
    its counter used (taken at its first fragment); nothing else changes (the counters restart only at a hello)."""
    for r in rx.values():
        r.bufs.clear()


def sealed_stream_restart() -> dict:
    """A device message cut after its first fragment by an error that ends only the VM stream; the client's update
    meanwhile (saved, its sealed answer held until the state that cuts the message off); the client's closed; the
    gadget's re-ask; the same sealed session reopened; then the next sealed message each way unseals at the next
    counter. Byte for byte, with the receivers' outcomes ("control": an unsealed state or open_request)."""
    e, m = seal_keys(DATA_K, RESTART_CN, RESTART_DN, DATA_NODE)
    rx = {DEVICE_TO_CLIENT: SealedReceiver(e, m, DEVICE_TO_CLIENT), CLIENT_TO_DEVICE: SealedReceiver(e, m, CLIENT_TO_DEVICE)}
    sent = {DEVICE_TO_CLIENT: 0, CLIENT_TO_DEVICE: 0}
    steps = []

    def control(what: str, direction: int, frame: bytes) -> None:
        if frame[1] == MG_SESSION_STATE:
            end_messages(rx)
        steps.append({"step": what, "direction": direction, "counter": None, "plaintext": "", "sealed": None,
                      "frames": [frame.hex()], "outcome": "control", "answer": None})

    def message(what: str, direction: int, sub: int, pt: bytes, cut: bool = False) -> None:
        ctr = sent[direction]
        sent[direction] += 1
        blob = seal(e, m, direction, ctr, pt)
        frames = fragments(sub, blob, True, DATA_MAX_WRITE)
        if cut:
            frames = frames[:1]
        outcome = [rx[direction].receive(f) for f in frames][-1]
        steps.append({"step": what.format(ctr=ctr), "direction": direction, "counter": ctr, "plaintext": pt.hex(),
                      "sealed": blob.hex(), "frames": [f.hex() for f in frames], "outcome": outcome[0],
                      "answer": outcome[1].hex() if outcome[0] == "refused" else None})

    register = det("restart link.register", 150)
    message("device: credentials (counter {ctr})", DEVICE_TO_CLIENT, MG_SESSION_CREDENTIALS,
            creds_json("mga_restart_access", "mgr_restart_refresh", DATA_NODE))
    control("client: state open (epoch 1)", CLIENT_TO_DEVICE, state_frame(MG_SESSION_STATE_OPEN, 1))
    message("device: data, its link.register (counter {ctr})", DEVICE_TO_CLIENT, MG_SESSION_DATA, register)
    message("client: data, the VM's answer (counter {ctr})", CLIENT_TO_DEVICE, MG_SESSION_DATA,
            vm_data("restart vm reply", 120))
    message("device: data (counter {ctr}), its first write only: an error ends the VM stream", DEVICE_TO_CLIENT,
            MG_SESSION_DATA, det("restart cut off", 200), cut=True)
    control("device: open_request [0]: only the stream ends, the sealed session stays", DEVICE_TO_CLIENT,
            open_request_frame(False))
    message("client: credentials_update (counter {ctr}) while the stream ends: saved, its answer held",
            CLIENT_TO_DEVICE, MG_SESSION_CREDENTIALS_UPDATE,
            expires_update("mga_restart_access_2", "mgr_restart_refresh_2", None))
    control("client: state closed (epoch 1): the cut message is dropped, its counter used", CLIENT_TO_DEVICE,
            state_frame(MG_SESSION_STATE_CLOSED, 1))
    message("device: credentials_saved, sealed (counter {ctr}), after the state", DEVICE_TO_CLIENT,
            MG_SESSION_CREDENTIALS_SAVED, saved_plaintext(True, "mgr_restart_refresh_2"))
    control("device: open_request [1] at once: the stream asked for again", DEVICE_TO_CLIENT, open_request_frame(True))
    control("client: state open (epoch 2): the same sealed session", CLIENT_TO_DEVICE,
            state_frame(MG_SESSION_STATE_OPEN, 2))
    message("device: data, the new stream's link.register (counter {ctr})", DEVICE_TO_CLIENT,
            MG_SESSION_DATA, register)
    message("client: data (counter {ctr})", CLIENT_TO_DEVICE, MG_SESSION_DATA, vm_data("restart vm event", 40))
    return {
        "name": "the VM stream restarted in the same sealed session after a message cut by an error",
        "K": DATA_K.hex(),
        "client_nonce": RESTART_CN.hex(),
        "device_nonce": RESTART_DN.hex(),
        "node_id": DATA_NODE,
        "max_write": DATA_MAX_WRITE,
        "steps": steps,
    }


EXPECTED_RESTART_OUTCOMES = ["message", "control", "message", "message", "partial", "control", "message", "control",
                             "message", "control", "control", "message", "message"]


def check_restart(v: dict) -> list[str]:
    bad = []
    outcomes = [s["outcome"] for s in v["steps"]]
    if outcomes != EXPECTED_RESTART_OUTCOMES:
        bad.append(f"stream restart outcomes {outcomes}")
    e, m = seal_keys(bytes.fromhex(v["K"]), bytes.fromhex(v["client_nonce"]), bytes.fromhex(v["device_nonce"]),
                     v["node_id"])
    rx = {DEVICE_TO_CLIENT: SealedReceiver(e, m, DEVICE_TO_CLIENT), CLIENT_TO_DEVICE: SealedReceiver(e, m, CLIENT_TO_DEVICE)}
    for s in v["steps"]:
        frames = [bytes.fromhex(f) for f in s["frames"]]
        if any(len(f) > v["max_write"] for f in frames):
            bad.append(f"{s['step']}: a write over max_write")
        if s["outcome"] == "control":
            if frames[0][1] == MG_SESSION_STATE:
                end_messages(rx)
            continue
        got = [rx[s["direction"]].receive(f) for f in frames][-1]
        if got[0] != s["outcome"] or (got[0] == "message" and got[1] != bytes.fromhex(s["plaintext"])):
            bad.append(f"{s['step']}: {got[0]} on replay")
    dev = [s["counter"] for s in v["steps"] if s["direction"] == DEVICE_TO_CLIENT and s["counter"] is not None]
    cli = [s["counter"] for s in v["steps"] if s["direction"] == CLIENT_TO_DEVICE and s["counter"] is not None]
    if dev != [0, 1, 2, 3, 4] or cli != [0, 1, 2]:
        bad.append(f"stream restart counters: device {dev}, client {cli}")
    # The cut message's counter is spent: a receiver that counted it only on completion would still expect 2 and
    # refuse the held answer and everything after it.
    saved = next(s for s in v["steps"] if s["counter"] == 3 and s["direction"] == DEVICE_TO_CLIENT)
    if unseal(e, m, DEVICE_TO_CLIENT, 2, bytes.fromhex(saved["sealed"])) is not None:
        bad.append("the held answer must not open at the cut message's counter")
    idx = {s["step"]: i for i, s in enumerate(v["steps"])}
    closed = next(i for i, s in enumerate(v["steps"]) if s["frames"][0] == state_frame(MG_SESSION_STATE_CLOSED, 1).hex())
    if not idx[saved["step"]] > closed:
        bad.append("the held credentials_saved must come after the state that cuts the message off")
    return bad


# --- What seal v1 doesn't cover: the sub-command ------------------------------------------------------------------
# The tag is over dir, counter and ciphertext, not the sub-command byte, so the same sealed bytes under another
# sub-command still unseal, and the plaintext often parses under the new label: one byte 0x00 is a valid clear, a
# JSON object a valid credentials_update. A party that picks a plaintext (the VM, through the data the client
# relays) and sits on the link can turn one message into another. Binding the sub-command and the session epoch
# into the tag is seal v2.

RELABEL_NODE = "homelink-3c5e22"
RELABEL_K = det("seal K relabel", 32)
RELABEL_CN, RELABEL_DN = det("relabel cn", 16), det("relabel dn", 16)
RELABEL_CASES = [
    ("one byte 0x00 as data, relabelled clear: the gadget erases its tokens", b"\x00", MG_SESSION_CLEAR),
    ("a JSON object as data, relabelled credentials_update: the stored pair is replaced",
     creds_json("mga_relabel_access", "mgr_relabel_refresh"), MG_SESSION_CREDENTIALS_UPDATE),
]


def relabelled() -> dict:
    """Each case: the client's sealed data message, the same bytes with the sub-command byte changed, and the
    plaintext both carry. Counter 0 in a session of its own."""
    e, m = seal_keys(RELABEL_K, RELABEL_CN, RELABEL_DN, RELABEL_NODE)
    steps = []
    for what, pt, as_sub in RELABEL_CASES:
        blob = seal(e, m, CLIENT_TO_DEVICE, 0, pt)
        sent = fragments(MG_SESSION_DATA, blob, True, SAVED_WRITE)[0]
        as_data = SealedReceiver(e, m, CLIENT_TO_DEVICE).receive(sent)
        steps.append({"step": what, "sent": sent.hex(),
                      "relabelled": (bytes([MG_COMMAND_SESSION, as_sub]) + sent[2:]).hex(),
                      "sub_command": as_sub, "counter": 0, "plaintext": pt.hex(), "sealed": blob.hex(),
                      "sent_as_data": as_data[1].hex() if as_data[0] == "refused" else as_data[0]})
    return {
        "name": "a sealed message relabelled: seal v1's tag doesn't cover the sub-command",
        "K": RELABEL_K.hex(),
        "client_nonce": RELABEL_CN.hex(),
        "device_nonce": RELABEL_DN.hex(),
        "node_id": RELABEL_NODE,
        "direction": CLIENT_TO_DEVICE,
        "note": "Current behaviour, not a wire change: the relabelled frame differs from the sent one in its "
                "sub-command byte alone, unseals under the same counter and tag, and its plaintext is valid for "
                "the label it arrives under. Seal v2 binds the sub-command and the session epoch into the tag. The "
                "plaintexts are the VM's choice and not ServiceResponseEnvelopes: a device that takes a sent frame "
                "as data refuses it (sent_as_data: error invalid_value, its counter used); only the relabel gets "
                "them taken.",
        "steps": steps,
    }


def check_client_envelopes(data: dict) -> list[str]:
    """Every client data message a section has the device take carries a ServiceResponseEnvelope."""
    bad = []
    for sec in ("sealed_data", "sealed_data_large", "sealed_interleaved", "sealed_stream_restart"):
        for st in data[sec]["steps"]:
            first = bytes.fromhex(st["frames"][0]) if st.get("frames") else b""
            if st.get("direction") == CLIENT_TO_DEVICE and first[1:2] == bytes([MG_SESSION_DATA]) \
                    and st["outcome"] == "message" and not is_response_envelope(bytes.fromhex(st["plaintext"])):
                bad.append(f"{sec}: {st['step']}: the plaintext isn't a ServiceResponseEnvelope")
    return bad


def check_relabelled(v: dict) -> list[str]:
    e, m = seal_keys(bytes.fromhex(v["K"]), bytes.fromhex(v["client_nonce"]), bytes.fromhex(v["device_nonce"]),
                     v["node_id"])
    bad = []
    for st in v["steps"]:
        sent, relab, pt = bytes.fromhex(st["sent"]), bytes.fromhex(st["relabelled"]), bytes.fromhex(st["plaintext"])
        if sent[1] != MG_SESSION_DATA or relab[1] != st["sub_command"] or sent[2:] != relab[2:]:
            bad.append(f"{st['step']}: the sub-command byte alone must differ")
        if unseal(e, m, CLIENT_TO_DEVICE, st["counter"], relab[MG_SESSION_FIRST_HEADER:]) != pt:
            bad.append(f"{st['step']}: the relabelled frame must still unseal")
        if st["sub_command"] == MG_SESSION_CLEAR:
            ok = pt == b"\x00"
        else:
            ok = parse_update(pt) is not None
        if not ok:
            bad.append(f"{st['step']}: the plaintext must parse under the new label")
        if SealedReceiver(e, m, CLIENT_TO_DEVICE).receive(sent) != ("refused", error_frame(MG_ERROR_CODE_INVALID_VALUE)) \
                or st.get("sent_as_data") != error_frame(MG_ERROR_CODE_INVALID_VALUE).hex():
            bad.append(f"{st['step']}: the sent frame, taken as data, must be refused (not an envelope)")
    return bad


def build() -> dict:
    vectors = []
    for v in INPUTS:
        enc_key, mac_key = seal_keys(v["K"], v["client_nonce"], v["device_nonce"], v["node_id"])
        sealed = seal(enc_key, mac_key, v["direction"], v["counter"], v["plaintext"])
        vectors.append({
            "name": v["name"],
            "K": v["K"].hex(),
            "client_nonce": v["client_nonce"].hex(),
            "device_nonce": v["device_nonce"].hex(),
            "node_id": v["node_id"],
            "direction": v["direction"],
            "counter": v["counter"],
            "plaintext": v["plaintext"].decode("utf-8"),
            "enc_key": enc_key.hex(),
            "mac_key": mac_key.hex(),
            "sealed": sealed.hex(),
        })
    return {
        "one_session_per_proof": one_session_per_proof(),
        "access_expires_at": access_expires_at(),
        "sealed_data": sealed_data(),
        "sealed_interleaved": sealed_interleaved(),
        "receiver_limits": receiver_limits(),
        "sealed_credentials_saved": sealed_credentials_saved(),
        "sealed_data_large": sealed_data(sealed_saved=True),
        "sealed_stream_restart": sealed_stream_restart(),
        "relabelled": relabelled(),
        "description": "Sealed device-session credentials (mgcommands.h, Sealed credentials). "
                       "enc_key||mac_key = HKDF-SHA256(salt, K, client_nonce||device_nonce||node_id, 64); "
                       "keystream block i = HMAC-SHA256(enc_key, stream_label||dir||counter_be32||i_be32); "
                       "tag = HMAC-SHA256(mac_key, tag_label||dir||counter_be32||len_be32||ciphertext)[:16]; "
                       "sealed = counter_be32||ciphertext||tag. dir 1 = device->client, 2 = client->device. "
                       "One sealed session per token proof: a second sealed hello under the same proof "
                       "is refused with error [session, proof_required] (one_session_per_proof). "
                       "access_expires_at: a device that keeps it stores an update's positive "
                       "integer with the pair (any other form: none; an update without it clears it; a duplicate "
                       "is invalid_value) and hands it back last in its next credentials (access_expires_at: "
                       "the sealed steps over three proofs, with the device's answer and what it keeps). "
                       "sealed_data: in a sealed session every message with a payload (credentials, "
                       "credentials_update, data) is sealed whole and then fragmented (total_len counts sealed "
                       "bytes), one counter per direction shared by all of them, the counter read from the first "
                       "fragment (next only; an aborted message uses its counter, a message refused at its tag "
                       "does not); a replayed counter or a bad tag is refused with invalid_value, unsealed data "
                       "with insecure. sealed_interleaved: two sealed messages in flight together; the earlier one "
                       "refused at its tag after a later counter was taken leaves that counter open once (a hole). "
                       "receiver_limits: what the receiver refuses (a first fragment without the whole counter, "
                       "total_len of 20 or less or over max_message, fragments that don't add up to total_len or "
                       "lose the sealed flag, a first fragment while one is open, a hole over 255 back) and what "
                       "each does to the counter. "
                       "sealed_credentials_saved: in a sealed session the device's "
                       "credentials_saved is sealed too (plaintext: result || SHA-256(refresh_token)[:8], zeros when "
                       "not saved); the client counts only a sealed [1] with the matching hash as saved, an unsealed "
                       "[1] confirms nothing, and with no sealed answer in 35 s of link it assumes saved. The "
                       "access_expires_at steps test the storage rule and keep the legacy one-byte answer; the "
                       "sealed answer is in sealed_credentials_saved. sealed_data_large: sealed_data as a device that "
                       "seals credentials_saved (mg_session_hello_flag_sealed_saved) sends it (its sealed "
                       "credentials_saved takes device counter 2, so its later data is counter 3); an "
                       "implementation that seals credentials_saved replays sealed_data_large, one that doesn't "
                       "sealed_data. "
                       "sealed_stream_restart: an error ends only the VM stream "
                       "(open_request [0], the client's closed, open_request [1], open) in the same sealed session; a "
                       "device message cut after its first fragment used its counter on both sides, a sealed "
                       "credentials_saved owed meanwhile goes after the state that cuts it off, and the next sealed "
                       "message each way unseals at the next counter. "
                       "Client data plaintexts are ServiceResponseEnvelopes (a body chunk on stream 3 carrying "
                       "stand-in bytes), which a device decodes first; client data that isn't one is refused "
                       "invalid_value, its counter used. "
                       "relabelled: what seal v1 doesn't cover - the tag is over dir, counter and ciphertext, not "
                       "the sub-command byte, so the same sealed bytes under another sub-command still unseal and "
                       "the plaintext parses there (0x00 as a clear, a JSON object as "
                       "a credentials_update). "
                       "Generated by mg_session_seal_ref.py.",
        "version": 1,
        "salt": SALT.decode(),
        "stream_label": STREAM_LABEL.decode(),
        "tag_label": TAG_LABEL.decode(),
        "vectors": vectors,
    }


def render() -> str:
    return json.dumps(build(), indent=2, ensure_ascii=False) + "\n"


def check() -> list[str]:
    data = json.loads(VECTORS.read_text(encoding="utf-8"))
    bad = []
    for v in data["vectors"]:
        k, cn, dn = bytes.fromhex(v["K"]), bytes.fromhex(v["client_nonce"]), bytes.fromhex(v["device_nonce"])
        enc_key, mac_key = seal_keys(k, cn, dn, v["node_id"])
        if enc_key.hex() != v["enc_key"] or mac_key.hex() != v["mac_key"]:
            bad.append(f"{v['name']}: keys")
        sealed = bytes.fromhex(v["sealed"])
        if unseal(enc_key, mac_key, v["direction"], v["counter"], sealed) != v["plaintext"].encode("utf-8"):
            bad.append(f"{v['name']}: unseal")
        flipped = sealed[:-1] + bytes([sealed[-1] ^ 1])
        if unseal(enc_key, mac_key, v["direction"], v["counter"], flipped) is not None:
            bad.append(f"{v['name']}: tag not checked")
        other = CLIENT_TO_DEVICE if v["direction"] == DEVICE_TO_CLIENT else DEVICE_TO_CLIENT
        if unseal(enc_key, mac_key, other, v["counter"], sealed) is not None:
            bad.append(f"{v['name']}: reflection accepted")
        if unseal(enc_key, mac_key, v["direction"], v["counter"] + 1, sealed) is not None:
            bad.append(f"{v['name']}: replay accepted")
    # Why one session per proof: two sessions under the same keys reuse the keystream.
    v0 = data["vectors"][0]
    k0, cn0, dn0 = bytes.fromhex(v0["K"]), bytes.fromhex(v0["client_nonce"]), bytes.fromhex(v0["device_nonce"])
    e0, m0 = seal_keys(k0, cn0, dn0, v0["node_id"])
    p1, p2 = b"A" * 40, b"B" * 40
    c1, c2 = seal(e0, m0, DEVICE_TO_CLIENT, 0, p1)[4:-TAG_LEN], seal(e0, m0, DEVICE_TO_CLIENT, 0, p2)[4:-TAG_LEN]
    if bytes(a ^ b for a, b in zip(c1, c2)) != bytes(a ^ b for a, b in zip(p1, p2)):
        bad.append("keystream reuse demonstration")
    if seal_keys(k0, cn0, det("other dn", 16), v0["node_id"]) == (e0, m0):
        bad.append("a new proof must give new keys")
    steps = data["one_session_per_proof"]["steps"]
    if steps[1]["device"] != error_frame(MG_ERROR_CODE_INSECURE).hex():
        bad.append("an unsealed hello must be refused insecure")
    if steps[3]["device"] != error_frame(MG_ERROR_CODE_PROOF_REQUIRED).hex():
        bad.append("second sealed hello under one proof not refused")
    if not steps[2]["device"].startswith("890001") or not steps[5]["device"].startswith("890001"):
        bad.append("sealed hello after a proof not answered")
    bad += check_expires(data["access_expires_at"])
    bad += check_data(data["sealed_data"])
    bad += check_interleaved(data["sealed_interleaved"])
    bad += check_limits(data["receiver_limits"])
    bad += check_saved(data["sealed_credentials_saved"])
    bad += check_data(data["sealed_data_large"], sealed_saved=True)
    bad += check_restart(data["sealed_stream_restart"])
    bad += check_relabelled(data["relabelled"])
    bad += check_client_envelopes(data)
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
        print(f"OK {VECTORS.name}: {len(INPUTS)} vectors, {sum(1 for v in build().values() if isinstance(v, dict))} "
              "sections")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
