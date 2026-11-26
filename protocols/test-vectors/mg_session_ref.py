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

"""Reference device-session device for mgcommands.h (Device session), and its test vectors.

Python standard library only. It plays the device side of mg_command_session
(hello, credentials, state, data, credit, abort) the way mgcommands.h describes it, and
the device end of the VM session those messages carry: it opens POST
/link-control on stream 1, registers TEST_COMMANDS with link.register, and
answers link.invoke with link.result, the way esp32/main/noise_control.cpp
does over Wi-Fi. While it has an active task (device flag `task`, event
`task`: a user action, traffic it owes such as a credentials write-back, or a
wake) a state it didn't ask for (closed, no_network) makes it ask again at
once with open_request [1]; with no task it asks nothing, and it never asks
after not_enrolled, auth_failed or too_large until a new hello, a
credentials_update or an open
(mgcommands.h, Device session). The backoff after that first re-ask is timed,
so the vectors don't show it. There are two series: one after closed (1, 2,
4 ... 60 s) and one after no_network (1, 2, 4 ... 15 s; the phone's network
or an OS that killed the app's socket, not the server); neither restarts the
other. An open restarts a series only after a session that stayed open 60 s,
so after a short one (every session in these untimed vectors) another end of
the same kind continues that series instead of asking at once. A new hello
or a saved credentials_update restarts both. The messages are real ServiceRequestEnvelopes and
ServiceResponseEnvelopes, encoded byte for byte as
esp32/components/noise_core (ServiceCodec.cpp) encodes them. Check the
vectors next to this file:

    python3 protocols/test-vectors/mg_session_ref.py [--check]

and regenerate them (the inputs are fixed, so the output never changes):

    python3 protocols/test-vectors/mg_session_ref.py --write

Credentials leave and are taken only sealed: every unsealed hello is refused
insecure, whatever the link's encryption (a token proof can be relayed, and
the relay's own bond is encrypted too).
Every session runs sealed under session_seal (K and the proof's nonces,
fake) with node_id, sealed as mg_session_seal_ref.py seals: the credentials,
credentials_update, credentials_saved and data both ways, windows and credit
in sealed bytes. A proof step is a new matching token proof (fresh nonces,
the session reset at its challenge), before any further sealed hello. A new
proof and a new sealed hello both drop every partial sealed message and
restart the counters, so nothing half-sent in the old session completes in
the new one or gives a counter back there.

Firmware checks itself against mg-session-v1.json: a device with the vector's
device_hello, node_id and test_commands (answering each with its
test_payload) must answer every write of every exchange with exactly the
notifications listed. An exchange whose device has vm_binding holds
test_credentials_bound instead: the device key stored with the VM it is
bound to (vm_id, vm_kind). An app checks its fragmenting, reassembly and credit
against the same exchanges from the other end.
"""

import base64
import json
import struct
import sys
from pathlib import Path

VECTORS = Path(__file__).with_name("mg-session-v1.json")
EXPIRES_KEY = "access_expires_at"

# From mgcommands.h.
MG_COMMAND_ERROR = 25
MG_COMMAND_SESSION = 0x89
HELLO, STATE, OPEN_REQUEST, DATA, CREDIT, ABORT = 0, 1, 2, 3, 4, 5
CREDENTIALS, CREDENTIALS_UPDATE, CREDENTIALS_SAVED = 6, 7, 8
CLEAR = 12   # mg_session_clear: the sealed Forget (mgcommands.h)
WAKE = 11    # mg_session_wake: "open your Muse session now"
MAX_CREDENTIALS = 2048
CLOSED, CONNECTING, OPEN, NO_NETWORK, AUTH_FAILED, NOT_ENROLLED, TOO_LARGE = range(7)
FIRST, LAST = 1, 2
ERROR_DATA_CODE = 1
UNSUPPORTED, INVALID_LENGTH, INVALID_VALUE, BUSY, NOT_FOUND, PROOF_REQUIRED, INSECURE = 1, 2, 3, 4, 5, 6, 7
MG_COMMAND_TOKEN_PROOF = 34
SESSION_VERSION = 1
MIN_MESSAGE = 4096
MAX_MESSAGE = 16 * 1024 * 1024
FIRST_HEADER = 7
DATA_HEADER = 3
SEAL_OVERHEAD = 20   # counter (4) || ciphertext || tag (16)
MAX_ATT_VALUE = 512  # MG_MAX_ATT_VALUE: the Core spec's attribute cap, whatever the MTU
COUNTER_BYTES = 4
COUNTER_MAX = 0xFFFFFFFF   # the last counter a direction has: counters never wrap
MIN_ATT_MTU = 100          # MG_MIN_ATT_MTU: the sealed session's one-write messages need it

# The reference device.
NODE_ID = "gadget-ref-0001"
DEVICE_MAX_MESSAGE = 16384
DEVICE_WINDOW = 4096
CTRL_STREAM_ID = 1
REGISTER_ID = "reg-1"
TEST_COMMANDS = {
    "display.card": {
        "description": "Show a card on the home screen: a title and a body.",
        "required": {"title": {"type": "string", "description": "Up to 40 characters"},
                     "body": {"type": "string", "description": "Up to 200 characters"}},
        "optional": {},
        "timeout_ms": 5000,
    },
    "device.health": {
        "description": "Battery and connection status.",
        "required": {},
        "optional": {},
        "timeout_ms": 5000,
    },
}
TEST_PAYLOADS = {"display.card": {"shown": True}, "device.health": {"battery": 87, "overall": "ok"}}
# Fixed, fake credentials the reference device holds.
TEST_CREDENTIALS = {
    "node_id": NODE_ID,
    "access_token": "test-access-token-0001",
    "refresh_token": "test-refresh-token-0001",
    "device_key": base64.b64encode(bytes(range(32))).decode(),
    "key_id": base64.b64encode(bytes(range(0xA0, 0xB0))).decode(),
}
# The same with the VM binding setup stored with the key (Gadget setup over
# Muse Link): the client opens the session to that
# fetch_vms entry, with the attestation handshake its vm_kind asks for.
TEST_CREDENTIALS_BOUND = dict(TEST_CREDENTIALS, vm_id="00000000-0000-4000-8000-00000000c0de",
                              vm_kind="cvm")


def compact(value) -> str:
    """JSON as the devices write it: no spaces, keys in insertion order, UTF-8."""
    return json.dumps(value, separators=(",", ":"), ensure_ascii=False)


def le16(v: int) -> bytes:
    return struct.pack("<H", v)


def le32(v: int) -> bytes:
    return struct.pack("<I", v)


# ---- protobuf, as noise_core writes it (zero values omitted, fields in order) ----

def varint(v: int) -> bytes:
    if v < 0:
        v += 1 << 64
    out = bytearray()
    while True:
        b = v & 0x7F
        v >>= 7
        out.append(b | (0x80 if v else 0))
        if not v:
            return bytes(out)


def vfield(num: int, v: int) -> bytes:
    return varint(num << 3) + varint(v) if v else b""


def dfield(num: int, data: bytes, keep_empty: bool = False) -> bytes:
    if not data and not keep_empty:
        return b""
    return varint((num << 3) | 2) + varint(len(data)) + data


def headers_bytes(num: int, headers) -> bytes:
    return b"".join(dfield(num, dfield(1, k.encode()) + dfield(2, v.encode()), True)
                    for k, v in headers)


def request_frame(stream, verb, path, headers=(), body=b"", end_body=False) -> bytes:
    req = (dfield(1, verb.encode()) + dfield(2, path.encode()) + headers_bytes(3, headers)
           + dfield(4, body) + vfield(5, 1 if end_body else 0))
    return vfield(1, stream) + dfield(2, req, True)


def response_frame(stream, status, headers=(), body=b"", end_body=False) -> bytes:
    resp = vfield(1, status) + headers_bytes(2, headers) + dfield(3, body) + vfield(4, 1 if end_body else 0)
    return vfield(1, stream) + dfield(3, resp, True)


def body_chunk_frame(stream, data, end_body=False) -> bytes:
    return vfield(1, stream) + dfield(4, dfield(1, data) + vfield(2, 1 if end_body else 0), True)


def request_envelope(frame: bytes, service: int = 0) -> bytes:
    """ServiceRequestEnvelope: service (Daemon, 0, omitted) and the frame."""
    return vfield(1, service) + dfield(2, frame)


def response_envelope(frame: bytes) -> bytes:
    return dfield(1, frame)


def parse(data: bytes) -> dict:
    """Decodes one protobuf message into {field: [values]} (varints as ints, the rest as bytes)."""
    out, i = {}, 0
    while i < len(data):
        key, i = read_varint(data, i)
        num, wire = key >> 3, key & 7
        if wire == 0:
            v, i = read_varint(data, i)
        elif wire == 2:
            n, i = read_varint(data, i)
            v, i = data[i:i + n], i + n
        else:
            raise ValueError(f"wire type {wire}")
        out.setdefault(num, []).append(v)
    return out


def read_varint(data: bytes, i: int):
    v = shift = 0
    while True:
        b = data[i]
        i += 1
        v |= (b & 0x7F) << shift
        shift += 7
        if not b & 0x80:
            return v, i


def envelope_payload(message: bytes):
    """A ServiceResponseEnvelope's payload (field 1, the last one if repeated; b"" if absent), or None if the bytes
    don't decode as noise_core's ServiceCodec decodes them: each key a field number 1..2^29-1 outside 19000-19999 and
    wire type 0, 1, 2 or 5 (field 1 delimited), varints of at most 10 bytes, every value inside the message, unknown
    fields skipped. So a lone 0x00, or anything starting 0x00, 0x01 or '{', is never an envelope."""
    def varint(i):
        v = 0
        for n in range(10):
            if i >= len(message) or (n == 9 and message[i] & 0xFE):
                raise ValueError
            v |= (message[i] & 0x7F) << (7 * n)
            i += 1
            if not message[i - 1] & 0x80:
                return v, i
        raise ValueError
    i, payload = 0, b""
    try:
        while i < len(message):
            key, i = varint(i)
            num, wire = key >> 3, key & 7
            if not 0 < num < 1 << 29 or 19000 <= num <= 19999 or wire not in (0, 1, 2, 5) or (num == 1 and wire != 2):
                return None
            if wire == 0:
                _, i = varint(i)
                continue
            n, i = varint(i) if wire == 2 else ({1: 8, 5: 4}[wire], i)
            if n > len(message) - i:
                return None
            if num == 1:
                payload = message[i:i + n]
            i += n
    except ValueError:
        return None
    return payload


def control_message(obj) -> bytes:
    """One message on the /link-control stream: u32-LE length, then compact JSON."""
    body = compact(obj).encode()
    return le32(len(body)) + body


# ---- the device session ----

def error_frame(code: int) -> bytes:
    return bytes([MG_COMMAND_ERROR, MG_COMMAND_SESSION, ERROR_DATA_CODE]) + le16(code)


def hello(max_message: int, window: int, version: int = SESSION_VERSION) -> bytes:
    return bytes([MG_COMMAND_SESSION, HELLO, version]) + le32(max_message) + le32(window)


def state(st: int, epoch: int) -> bytes:
    return bytes([MG_COMMAND_SESSION, STATE, st]) + le16(epoch)


def open_request(want_open: bool) -> bytes:
    return bytes([MG_COMMAND_SESSION, OPEN_REQUEST, 1 if want_open else 0])


def credit(n: int) -> bytes:
    return bytes([MG_COMMAND_SESSION, CREDIT]) + le32(n)


def saved(ok: bool) -> bytes:
    return bytes([MG_COMMAND_SESSION, CREDENTIALS_SAVED, 1 if ok else 0])


def max_value(mtu: int) -> int:
    """The largest write or notification: min(ATT_MTU - 3, MG_MAX_ATT_VALUE)."""
    return min(mtu - 3, MAX_ATT_VALUE)


def fragments(message: bytes, mtu: int, sub: int = DATA):
    """Splits a message into data (or credentials) fragments of at most min(ATT_MTU - 3, 512) bytes, each
    filled but the last."""
    out, off = [], 0
    while off < len(message):
        first = off == 0
        room = max_value(mtu) - (FIRST_HEADER if first else DATA_HEADER)
        n = min(room, len(message) - off)
        flags = (FIRST if first else 0) | (LAST if off + n == len(message) else 0)
        head = bytes([MG_COMMAND_SESSION, sub, flags]) + (le32(len(message)) if first else b"")
        out.append(head + message[off:off + n])
        off += n
    return out


def register_request() -> dict:
    return {"type": "req", "id": REGISTER_ID, "method": "link.register",
            "params": {"node_id": NODE_ID, "display_name": "Reference Gadget", "platform": "mg-ref",
                       "version": "1", "device_family": "gadget", "model_id": "mg-ref",
                       "is_wakeup_supported": True, "commands_v2": TEST_COMMANDS}}


class Device:
    """The device side of mg_command_session, and of the VM session over it."""

    def __init__(self, mtu: int, holds_token=True, proof=True, encrypted=True, vm_binding=False, task=False,
                 credentials_wait=False):
        self.mtu = mtu
        # credentials_wait: no room for the sealed credentials when the hello is answered; they wait for the board's
        # stack (event buffer), their counter not yet used.
        self.credentials_wait = credentials_wait
        self.creds_waiting = False
        # encrypted is never read: link encryption changes nothing (every session is sealed).
        self.holds_token, self.proof, self.encrypted = holds_token, proof, encrypted
        self.hello_done = False
        # What the device stores; a credentials_update replaces the pair and
        # keeps the rest, the VM binding included.
        self.creds = dict(TEST_CREDENTIALS_BOUND if vm_binding else TEST_CREDENTIALS)
        self.cred_in = None                  # a credentials_update being received
        self.cred_total = 0
        self.task = task                     # an active task: it asks to reopen only while one lasts
        self.no_reask = False                # after not_enrolled, auth_failed, too_large
        self.reask_series = {}               # running re-ask series by end kind (closed, no_network)
        self.ended = False                   # the last state ended a session (not open)
        self.ended_kind = CLOSED             # which series a save while ended starts
        self.seal_keys = None                # (enc_key, mac_key) of the sealed session
        self.tx = self.rx = 0                # the next device -> client / client -> device counter
        self.saved_sealed = False            # the hello asked for the sealed credentials_saved
        self.nonces_taken = False            # one sealed session per token proof
        self.inputs = None                   # the latest proof's (K, client_nonce, device_nonce); None: session_seal
        self.rx_hole = 0                     # a counter refused at its tag that may still come once (distance back)
        self.out_ctr = None                  # the device counter reserved for outq[0]
        self.held = []                       # sealed credentials_saved answers waiting for a reserved data message
        self.cred_ctr = self.in_ctr = None   # the counters of the messages being received
        self.clear_in = None                 # a sealed clear arriving
        self.clear_ctr = None
        self.clear_pending = False           # a sealed clear taken and erased, its answer waiting for what goes out
        self.overruns = 0                    # client fragments past the device's credit, dropped
        self.reset_session()

    def reset_session(self):
        if getattr(self, "out_ctr", None) is not None and self.out_off == 0:
            self.tx -= 1        # reserved, no byte out: the client never saw it, the counter goes back
        self.out_ctr = None
        self.open = False
        self.send_credit = getattr(self, "client_window", 0)
        self.outq = []          # whole messages waiting to go out
        self.out_off = 0        # bytes of outq[0] already sent
        self.in_msg = None      # bytearray of the message being received
        self.in_credited = 0    # bytes of it already given back (a message larger than the window)
        self.in_total = 0
        self.ctrl = bytearray()  # /link-control body bytes not yet a whole message

    def write(self, frame: bytes) -> list:
        out = []
        if len(frame) < 2 or frame[0] != MG_COMMAND_SESSION:
            return [error_frame(INVALID_LENGTH)] if len(frame) < 2 else []
        sub, p = frame[1], frame[2:]
        if sub == HELLO:
            out += self.on_hello(p)
        elif sub == STATE:
            out += self.on_state(p)
        elif sub == DATA:
            out += self.on_data(p)
        elif sub == CREDIT:
            out += self.on_credit(p)
        elif sub == ABORT:
            out += self.on_abort(p)
        elif sub == CREDENTIALS_UPDATE:
            out += self.on_credentials_update(p)
        elif sub == CLEAR:
            out += self.on_clear(p)
        elif sub == WAKE:
            out += self.on_wake()
        else:
            out.append(error_frame(UNSUPPORTED))
        out += self.flush()
        if self.clear_pending and self.out_ctr is None and not self.held:
            out += self.clear_answer()
        return out

    def on_wake(self) -> list:
        """mg_session_wake: "open your Muse session now", never sealed, no reply. Honored only after the proof and
        the hello; the device takes it as a task of its own and asks under the usual series rules."""
        if not self.hello_done:
            return []
        return self.set_task(True)

    def set_task(self, active: bool) -> list:
        """A task starts or ends (a user action, traffic it owes, a wait, a wake). One starting while no session is
        open asks for one at once, under the series rules; with no task the device asks nothing."""
        self.task = active
        if not active or self.open or self.no_reask or self.reask_series.get(self.ended_kind):
            return []
        self.reask_series[self.ended_kind] = True
        return [open_request(True)]

    def drop_partials(self):
        """A new proof or a new sealed hello: every sealed message part way in or out goes, with the counters it
        touched, so nothing from the old session completes in the new one or gives one of its counters back."""
        self.cred_in = self.clear_in = None
        self.clear_pending = self.creds_waiting = False
        self.cred_ctr = self.clear_ctr = self.in_ctr = None
        self.held = []
        self.out_ctr = None
        self.rx_hole = 0
        self.reset_session()

    def on_clear(self, p: bytes) -> list:
        """mg_session_clear, sealed only, fragmented like a credentials_update (outside credit): counter || 0 || tag.
        Taken, it erases the tokens and K at once (the gadget back to Link setup) and answers a sealed [1] on the next
        device counter, once what goes out before it is out; then the session ends."""
        if len(p) < 1:
            return [error_frame(INVALID_LENGTH)]
        if not self.hello_done:
            return [error_frame(INVALID_VALUE)]
        if self.spent():
            self.clear_in = None
            return [error_frame(INVALID_VALUE)]
        flags, rest = p[0], p[1:]
        if flags & FIRST:
            if len(rest) < 4:
                return [error_frame(INVALID_LENGTH)]
            total, rest = struct.unpack("<I", rest[:4])[0], rest[4:]
            if self.seal_keys is None or not flags & FLAG_SEALED:
                self.clear_in = None
                return [error_frame(INSECURE)]   # never unsealed: nothing erased
            if len(rest) < COUNTER_BYTES:
                self.clear_in = None
                return [error_frame(INVALID_LENGTH)]
            if total != SEAL_OVERHEAD + 1 or self.clear_pending or not self.rx_ok(struct.unpack(">I", rest[:4])[0]):
                self.clear_in = None
                return [error_frame(INVALID_VALUE)]
            self.clear_ctr = struct.unpack(">I", rest[:4])[0]
            self.rx_take(self.clear_ctr)
            if self.cred_in is not None:
                # Nowhere to hold it (a credentials_update arriving): busy, its counter used (the first fragment
                # carries the whole counter); the client retries on the next one.
                self.clear_in = None
                return [error_frame(BUSY)]
            self.clear_in = bytearray()
        elif self.clear_in is None:
            return [error_frame(INVALID_VALUE)]
        if not rest or len(self.clear_in) + len(rest) > SEAL_OVERHEAD + 1 \
                or bool(flags & LAST) != (len(self.clear_in) + len(rest) == SEAL_OVERHEAD + 1) \
                or not flags & FLAG_SEALED:
            self.clear_in = None
            return [error_frame(INVALID_VALUE)]
        self.clear_in.extend(rest)
        if not flags & LAST:
            return []
        blob, self.clear_in = bytes(self.clear_in), None
        s = phone_seal()
        pt = s.unseal(*self.seal_keys, s.CLIENT_TO_DEVICE, self.clear_ctr, blob)
        if pt is None:
            self.rx_unuse(self.clear_ctr)   # refused at its tag: the counter isn't used
            return [error_frame(INVALID_VALUE)]
        if pt != b"\x00":
            return [error_frame(INVALID_VALUE)]
        self.creds = {}          # erased now, whatever is still going out
        self.holds_token = False
        self.clear_pending = True
        self.creds_waiting = False   # credentials not started never go out; their counter was never used
        if self.out_off == 0:
            # Nothing of a reserved message went out: it never will, and its counter goes back.
            if self.out_ctr is not None:
                self.tx -= 1
                self.out_ctr = None
            self.outq = []
        else:
            self.outq = self.outq[:1]   # only the message part way out goes before the answer
        if self.out_ctr is None and not self.held:
            return self.clear_answer()
        return []

    def clear_answer(self) -> list:
        """The sealed [1] (sub-command clear) on the next device counter, once what was going out before the clear
        is out; then the session ends. The session's derived keys stay only to seal this answer."""
        s = phone_seal()
        self.clear_pending = False
        blob = s.seal(*self.seal_keys, s.DEVICE_TO_CLIENT, self.tx, b"\x01")
        out = s.fragments(CLEAR, blob, True, max_value(self.mtu))
        self.tx += 1
        self.hello_done = False
        self.seal_keys = None
        self.cred_in = None
        self.reset_session()
        return out

    def seal_inputs(self) -> tuple:
        return self.inputs or session_seal_inputs()

    def new_proof(self, k: bytes, cn: bytes, dn: bytes) -> list:
        """A new matching token proof: the board resets the session at its challenge; the next sealed hello takes
        these nonces. With no token (after a clear) the challenge gets [token_proof, not_found] and nothing
        changes."""
        if not self.holds_token:
            return [bytes([MG_COMMAND_ERROR, MG_COMMAND_TOKEN_PROOF, ERROR_DATA_CODE]) + le16(NOT_FOUND)]
        self.proof = True        # a match on this link
        self.inputs = (k, cn, dn)
        self.hello_done = False
        self.seal_keys = None
        self.nonces_taken = False
        self.drop_partials()
        self.tx = self.rx = 0
        return []

    def spent(self) -> bool:
        """The device has sealed COUNTER_MAX: it seals nothing more in this session and refuses the client's sealed
        messages, which ends the session (a new proof and sealed hello start fresh keys and counters)."""
        return self.seal_keys is not None and self.tx > COUNTER_MAX

    # The client -> device counter (mgseal_rx_ok / _take / _unuse): the next one, or the one hole, once. A counter is
    # 32 bits, so once rx passes COUNTER_MAX no counter is the next one: none is ever taken twice.
    def rx_ok(self, c: int) -> bool:
        return c == self.rx or (self.rx_hole != 0 and c == self.rx - self.rx_hole)

    def rx_take(self, c: int):
        if c == self.rx:
            self.rx += 1
            self.rx_hole = 0 if self.rx_hole in (0, 255) else self.rx_hole + 1
        else:
            self.rx_hole = 0

    def rx_unuse(self, c: int):
        if self.rx == c + 1:
            self.rx = c
            self.rx_hole = self.rx_hole - 1 if self.rx_hole > 1 else 0
        elif c < self.rx and self.rx - c <= 255:
            self.rx_hole = self.rx - c

    def on_hello(self, p: bytes) -> list:
        if len(p) < 9:
            return [error_frame(INVALID_LENGTH)]
        version = p[0]
        max_message, window = struct.unpack("<II", p[1:9])
        flags = p[9] if len(p) > 9 else 0
        if not self.holds_token:
            return [error_frame(NOT_FOUND)]
        if not self.proof:
            return [error_frame(PROOF_REQUIRED)]
        if not flags & HELLO_FLAG_SEALED:
            return [error_frame(INSECURE)]   # every unsealed hello, whatever the link's encryption
        if version != SESSION_VERSION or not MIN_MESSAGE <= max_message <= MAX_MESSAGE \
                or not COUNTER_BYTES <= window <= max_message \
                or self.mtu < MIN_ATT_MTU:   # a sealed window holds the counter; a link under MG_MIN_ATT_MTU can't
            return [error_frame(INVALID_VALUE)]
        if self.nonces_taken:
            return [error_frame(PROOF_REQUIRED)]   # one sealed session per token proof
        self.nonces_taken = True
        self.hello_done = True
        self.no_reask = False
        self.reask_series = {}
        self.ended = False
        self.client_max, self.client_window = max_message, window
        self.drop_partials()
        s = phone_seal()
        k, cn, dn = self.seal_inputs()
        self.seal_keys = s.seal_keys(k, cn, dn, NODE_ID)
        self.tx, self.rx, self.rx_hole = 1, 0, 0   # the credentials take device -> client counter 0
        self.saved_sealed = bool(flags & HELLO_FLAG_SEALED_SAVED)
        echo = HELLO_FLAG_SEALED | (HELLO_FLAG_SEALED_SAVED if self.saved_sealed else 0)
        if self.credentials_wait:
            self.tx, self.creds_waiting = 0, True   # counter 0 waits with them, unused
            return [hello(DEVICE_MAX_MESSAGE, DEVICE_WINDOW) + bytes([echo])]
        sealed = s.seal(*self.seal_keys, s.DEVICE_TO_CLIENT, 0, compact(self.creds).encode())
        return [hello(DEVICE_MAX_MESSAGE, DEVICE_WINDOW) + bytes([echo])] + \
            s.fragments(CREDENTIALS, sealed, True, max_value(self.mtu))

    def on_credentials_update(self, p: bytes) -> list:
        """The client's refreshed token pair, sealed, fragmented as data, outside credit. Its counter is checked
        at the first fragment and used from there, unless its tag is refused at the last."""
        if len(p) < 1:
            return [error_frame(INVALID_LENGTH)]
        if not self.hello_done or self.clear_pending or self.spent():
            self.cred_in = None if self.spent() else self.cred_in
            return [error_frame(INVALID_VALUE)]   # after a clear's erase nothing is stored, so its [1] holds
        flags, rest = p[0], p[1:]
        if flags & FIRST:
            if len(rest) < 4:
                return [error_frame(INVALID_LENGTH)]
            total, rest = struct.unpack("<I", rest[:4])[0], rest[4:]
            if not flags & FLAG_SEALED:
                self.cred_in = None
                return [error_frame(INSECURE)]   # no unsealed update in a sealed session
            if len(rest) < COUNTER_BYTES:
                self.cred_in = None
                return [error_frame(INVALID_LENGTH)]   # not the whole counter
            if self.cred_in is not None or total < SEAL_OVERHEAD + 1 or total > MAX_CREDENTIALS + SEAL_OVERHEAD \
                    or not self.rx_ok(struct.unpack(">I", rest[:4])[0]):
                self.cred_in = None
                return [error_frame(INVALID_VALUE)]
            self.cred_ctr = struct.unpack(">I", rest[:4])[0]
            self.rx_take(self.cred_ctr)
            self.cred_in, self.cred_total = bytearray(), total
        elif self.cred_in is None:
            return [error_frame(INVALID_VALUE)]
        if not rest or len(self.cred_in) + len(rest) > self.cred_total \
                or bool(flags & LAST) != (len(self.cred_in) + len(rest) == self.cred_total) \
                or not flags & FLAG_SEALED:
            self.cred_in = None   # a later fragment without the sealed flag too: the update is dropped
            return [error_frame(INVALID_VALUE)]
        self.cred_in.extend(rest)
        if not flags & LAST:
            return []
        blob, self.cred_in = bytes(self.cred_in), None
        s = phone_seal()
        text = s.unseal(*self.seal_keys, s.CLIENT_TO_DEVICE, self.cred_ctr, blob)
        if text is None:
            self.rx_unuse(self.cred_ctr)         # refused at its tag: the counter isn't used
            return [error_frame(INVALID_VALUE)]
        if len(text) > MAX_CREDENTIALS:
            return [error_frame(INVALID_VALUE)]
        known = ("access_token", "refresh_token", EXPIRES_KEY)

        def pairs(items):
            seen = set()
            for k, _ in items:
                if k in known:
                    if k in seen:
                        raise ValueError("duplicate " + k)   # a duplicate of these three: invalid_value
                    seen.add(k)
            return dict(items)
        try:
            # The first byte is '{': json.loads alone would take leading whitespace.
            update = json.loads(text, object_pairs_hook=pairs) if text[:1] == b"{" else None
        except ValueError:
            update = None
        if not isinstance(update, dict) or not all(
                isinstance(update.get(k), str) and update[k] for k in ("access_token", "refresh_token")):
            return [error_frame(INVALID_VALUE)]
        # Both in one commit, with access_expires_at: a positive integer that fits 64 bits is kept
        # and handed back last in the next credentials; absent, or any other form, none. The whole stored
        # document goes back in that credentials, so it must fit MG_SESSION_MAX_CREDENTIALS too.
        stored = dict(self.creds)
        stored["access_token"] = update["access_token"]
        stored["refresh_token"] = update["refresh_token"]
        exp = update.get(EXPIRES_KEY)
        stored.pop(EXPIRES_KEY, None)
        if type(exp) is int and 0 < exp < 1 << 64:
            stored[EXPIRES_KEY] = exp
        if len(compact(stored).encode()) > MAX_CREDENTIALS:
            return [error_frame(INVALID_VALUE)]
        self.creds = stored
        self.no_reask = False
        self.reask_series = {}
        self.task = True        # a credentials write-back is traffic it owes
        if not self.saved_sealed:
            answer = [saved(True)]
        elif self.out_ctr is not None:
            self.held.append(update["refresh_token"])   # after the reserved data message's last fragment
            answer = []
        else:
            self.held.append(update["refresh_token"])
            answer = self.release_held()
        if self.ended and not self.open:
            # New credentials may fix what ended the session (auth_failed,
            # not_enrolled): a device that must stay reachable asks again at
            # once and starts a new series. It takes a new save each time,
            # so it can't loop.
            # The ask is the first of the ended kind's series (no_network
            # after a no_network end, closed after anything else).
            self.reask_series = {self.ended_kind: True}
            return answer + [open_request(True)]
        return answer

    def release_held(self) -> list:
        """The sealed credentials_saved answers owed, each on the next device counter."""
        s = phone_seal()
        out = [s.sealed_saved_frame(*self.seal_keys, self.tx + i, True, r) for i, r in enumerate(self.held)]
        self.tx += len(self.held)
        self.held = []
        return out

    def on_state(self, p: bytes) -> list:
        if len(p) < 3:
            return [error_frame(INVALID_LENGTH)]
        if not self.hello_done:
            return [error_frame(INVALID_VALUE)]
        st = p[0]
        if st > TOO_LARGE:
            return [error_frame(INVALID_VALUE)]
        self.reset_session()
        if self.held:
            return self.release_held() + self.on_state_ended(st)
        return self.on_state_ended(st)

    def on_state_ended(self, st: int) -> list:
        self.ended = st in (CLOSED, NO_NETWORK, NOT_ENROLLED, AUTH_FAILED, TOO_LARGE)
        self.ended_kind = NO_NETWORK if st == NO_NETWORK else CLOSED
        if st == OPEN:
            self.open = True
            self.no_reask = False
            self.start_session()
            return []
        if st in (NOT_ENROLLED, AUTH_FAILED, TOO_LARGE):
            self.no_reask = True    # asking again can't fix these
            return []
        if st in (CLOSED, NO_NETWORK) and self.task and not self.no_reask:
            # An end the device didn't ask for (it never sends open_request [0]
            # here), with a task still active: it asks again. The first ask of a
            # series goes at once; while a series runs (the session since lasted
            # under 60 s, as every session in these untimed vectors does) the next
            # ask waits for the next backoff step, which the vectors don't show.
            # closed and no_network run separate series (60 s / 15 s caps).
            if not self.reask_series.get(st):
                self.reask_series[st] = True
                return [open_request(True)]
        return []

    def start_session(self):
        """What a Wi-Fi device does once its Noise session is up."""
        self.outq.append(request_envelope(request_frame(CTRL_STREAM_ID, "POST", "/link-control")))
        self.outq.append(request_envelope(body_chunk_frame(CTRL_STREAM_ID, control_message(register_request()))))

    def on_credit(self, p: bytes) -> list:
        if len(p) < 4:
            return [error_frame(INVALID_LENGTH)]
        if not self.hello_done:
            return [error_frame(INVALID_VALUE)]
        self.send_credit += struct.unpack("<I", p[:4])[0]
        return []

    def buffer(self) -> list:
        """The board's stack has room: credentials that waited go out now, sealed on the next device counter."""
        if not (self.creds_waiting and self.seal_keys):
            return []
        s = phone_seal()
        self.creds_waiting = False
        sealed = s.seal(*self.seal_keys, s.DEVICE_TO_CLIENT, self.tx, compact(self.creds).encode())
        self.tx += 1
        return s.fragments(CREDENTIALS, sealed, True, max_value(self.mtu))

    def nudge(self) -> list:
        """The keepalive: while a session is open, the credit the device owes for messages it has fully taken
        and not yet given back, or credit 0 when it owes none (a message still arriving earns none); nothing with
        no session open. This device gives a taken message's credit back at once, so it always owes none."""
        if not (self.hello_done and self.open):
            return []
        return [credit(0)]

    def on_abort(self, p: bytes) -> list:
        if not self.hello_done:
            return [error_frame(INVALID_VALUE)]
        return self.drop_in()

    def drop_in(self, extra: int = 0) -> list:
        """Drops the message being received; gives back its bytes and `extra` more."""
        n = (len(self.in_msg) - self.in_credited if self.in_msg is not None else 0) + extra
        self.in_msg = None
        return [credit(n)] if n else []

    def on_data(self, p: bytes) -> list:
        """A client message, sealed in a sealed session: its counter taken at the first fragment (used from there:
        an abort, a drop, a state change), its tag checked at the last (a refused tag gives the counter back);
        credit counts sealed bytes."""
        if len(p) < 1:
            return [error_frame(INVALID_LENGTH)]
        if not self.hello_done or not self.open:
            return [error_frame(INVALID_VALUE)]
        if self.clear_pending or self.spent():   # erased: the keys seal only the clear's answer; or spent
            return [error_frame(INVALID_VALUE)] + self.drop_in(len(p[5:] if p[0] & FIRST else p[1:]))
        flags, rest = p[0], p[1:]
        sealed = self.seal_keys is not None
        if flags & FIRST:
            if len(rest) < 4:
                return [error_frame(INVALID_LENGTH)]
            total, rest = struct.unpack("<I", rest[:4])[0], rest[4:]
            if sealed and not flags & FLAG_SEALED:
                return [error_frame(INSECURE)] + ([credit(len(rest))] if rest else [])   # nothing changes
            if sealed and len(rest) < COUNTER_BYTES:
                return [error_frame(INVALID_LENGTH)] + self.drop_in(len(rest))   # not the whole counter
            if self.in_msg is not None or total == 0 or total > DEVICE_MAX_MESSAGE \
                    or (sealed and total <= SEAL_OVERHEAD):
                return [error_frame(INVALID_VALUE)] + self.drop_in(len(rest))
            if sealed:
                ctr = struct.unpack(">I", rest[:4])[0]
                if not self.rx_ok(ctr):
                    return [error_frame(INVALID_VALUE)] + self.drop_in(len(rest))
                self.rx_take(ctr)
                self.in_ctr = ctr
            self.in_msg, self.in_total, self.in_credited = bytearray(), total, 0
        elif self.in_msg is None:
            return [error_frame(INVALID_VALUE)] + self.drop_in(len(rest))
        if not rest or len(self.in_msg) + len(rest) > self.in_total \
                or bool(flags & LAST) != (len(self.in_msg) + len(rest) == self.in_total) \
                or (sealed and not flags & FLAG_SEALED):
            return [error_frame(INVALID_VALUE)] + self.drop_in(len(rest))
        if len(self.in_msg) - self.in_credited + len(rest) > DEVICE_WINDOW:
            self.overruns += 1   # past the credit the device granted: dropped and counted, never buffered
            return [error_frame(INVALID_VALUE)] + self.drop_in(len(rest))
        self.in_msg.extend(rest)
        if not flags & LAST:
            held = len(self.in_msg) - self.in_credited
            if held >= DEVICE_WINDOW:   # buffered: give it back so a message larger than the window completes
                self.in_credited += held
                return [credit(held)]
            return []
        message, self.in_msg = bytes(self.in_msg), None
        back = len(message) - self.in_credited
        if sealed:
            s = phone_seal()
            pt = s.unseal(*self.seal_keys, s.CLIENT_TO_DEVICE, self.in_ctr, message)
            if pt is None:
                self.rx_unuse(self.in_ctr)
                return [error_frame(INVALID_VALUE)] + ([credit(back)] if back else [])
            message = pt
        payload = envelope_payload(message)
        if payload is None:
            return [error_frame(INVALID_VALUE)] + ([credit(back)] if back else [])   # its counter stays used
        return ([credit(back)] if back else []) + self.handle(payload)

    def handle(self, payload: bytes) -> list:
        """A ServiceResponseEnvelope's payload from the VM: one ServiceFrame."""
        frame = parse(payload)
        stream = frame.get(1, [0])[0]
        if stream == CTRL_STREAM_ID and 4 in frame:
            self.ctrl.extend(parse(frame[4][0]).get(1, [b""])[0])
            while len(self.ctrl) >= 4:
                n = struct.unpack("<I", self.ctrl[:4])[0]
                if len(self.ctrl) < 4 + n:
                    break
                body, self.ctrl = bytes(self.ctrl[4:4 + n]), self.ctrl[4 + n:]
                if body:   # an empty record is a keepalive
                    self.on_control(json.loads(body))
        return []

    def on_control(self, msg: dict):
        if msg.get("method") != "link.invoke":
            return  # the link.register reply and anything else: nothing to do
        name = msg.get("command", "")
        if name in TEST_PAYLOADS:
            result = {"method": "link.result", "id": msg.get("id", ""), "ok": True,
                      "payload": TEST_PAYLOADS[name]}
        else:
            result = {"method": "link.result", "id": msg.get("id", ""), "ok": False,
                      "error": f"no command named {name}"}
        self.outq.append(request_envelope(body_chunk_frame(CTRL_STREAM_ID, control_message(result))))

    def flush(self) -> list:
        """Sends queued message bytes as far as the client's credit allows."""
        out = []
        sealed = self.seal_keys is not None
        if sealed and self.creds_waiting:
            return out   # nothing sealed goes before the credentials
        if sealed and self.out_ctr is None and self.held:
            out += self.release_held()   # an answer owed goes before the next data message is reserved
        while self.open and self.outq:
            msg = self.outq[0]
            if sealed:
                if self.out_ctr is None:
                    if self.tx > COUNTER_MAX:
                        self.outq = []   # no counter left: nothing more goes out in this session
                        break
                    # Reserved when it is handed over (the next device counter), sealed whole, then fragmented.
                    s = phone_seal()
                    self.out_ctr = self.tx
                    self.tx += 1
                    self.out_blob = s.seal(*self.seal_keys, s.DEVICE_TO_CLIENT, self.out_ctr, msg)
                msg = self.out_blob
            off = self.out_off
            first = off == 0
            if self.send_credit <= 0 or (sealed and first and self.send_credit < COUNTER_BYTES):
                break   # a sealed first fragment carries the whole counter: wait for 4 bytes of credit
            room = max_value(self.mtu) - (FIRST_HEADER if first else DATA_HEADER)
            n = min(room, len(msg) - off, self.send_credit)
            flags = (FIRST if first else 0) | (LAST if off + n == len(msg) else 0) | (FLAG_SEALED if sealed else 0)
            out.append(bytes([MG_COMMAND_SESSION, DATA, flags]) + (le32(len(msg)) if first else b"")
                       + msg[off:off + n])
            self.send_credit -= n
            self.out_off += n
            if self.out_off == len(msg):
                self.outq.pop(0)
                self.out_off = 0
                if sealed:
                    self.out_ctr = None
                    out += self.release_held()
        return out


# ---- the exchanges ----

SH = 0x03   # the hello flags today's clients send: sealed | sealed_saved


class Recorder:
    """Writes the client's side: sealed under the latest proof (session_seal, then each proof step's nonces), its
    counter from 0 at every answered sealed hello."""

    def __init__(self, name, mtu, note, **device):
        self.dev = Device(mtu, **device)
        self.mtu = mtu
        self.ctr = 0
        self.inputs = session_seal_inputs()
        self.ex = {"name": name, "mtu": mtu, "device": {k: v for k, v in device.items()}, "note": note,
                   "steps": []}

    def write(self, frame: bytes, note: str = None):
        notify = self.dev.write(frame)
        if frame[:2] == bytes([MG_COMMAND_SESSION, HELLO]) and notify[:1] and \
                notify[0][:2] == bytes([MG_COMMAND_SESSION, HELLO]):
            self.ctr = 0
        step = {"write": frame.hex(), "notify": [f.hex() for f in notify]}
        if note:
            step["note"] = note
        self.ex["steps"].append(step)

    def proof(self, n: int, note: str = None):
        """A new matching token proof (fresh nonces from both sides): the session reset at its challenge."""
        sm = phone_seal()
        k, cn, dn = self.inputs[0], sm.det(f"session cn {n}", 16), sm.det(f"session dn {n}", 16)
        self.inputs = (k, cn, dn)
        step = {"event": "proof", "client_nonce": cn.hex(), "device_nonce": dn.hex(),
                "notify": [f.hex() for f in self.dev.new_proof(k, cn, dn)]}
        if note:
            step["note"] = note
        self.ex["steps"].append(step)

    def counters(self, rx: int = None, tx: int = None, note: str = None):
        """Stands in for a long session: the device's next client -> device (rx) and device -> client (tx) counters
        jump there, as if that many sealed messages had gone each way."""
        step = {"event": "counters"}
        if rx is not None:
            self.dev.rx = self.ctr = step["rx"] = rx
        if tx is not None:
            self.dev.tx = step["tx"] = tx
        step["notify"] = []
        if note:
            step["note"] = note
        self.ex["steps"].append(step)

    def nudge(self, note: str = None):
        """The device's keepalive nudge (event nudge)."""
        step = {"event": "nudge", "notify": [f.hex() for f in self.dev.nudge()]}
        if note:
            step["note"] = note
        self.ex["steps"].append(step)

    def buffer(self, note: str = None):
        """The board's stack has room (event buffer): credentials that waited go out."""
        step = {"event": "buffer", "notify": [f.hex() for f in self.dev.buffer()]}
        if note:
            step["note"] = note
        self.ex["steps"].append(step)

    def task(self, active: bool, note: str = None):
        """A task starts or ends on the device: it asks for a session only while one lasts."""
        step = {"event": "task", "active": active, "notify": [f.hex() for f in self.dev.set_task(active)]}
        if note:
            step["note"] = note
        self.ex["steps"].append(step)

    def drain(self, note: str = None):
        """Credit back what the client consumed, a window at a time, until the device's queue is out (a client
        never gives back more than its window)."""
        while self.dev.outq:
            before = (len(self.dev.outq), self.dev.out_off)
            self.write(credit(self.dev.client_window), note)
            note = None
            if (len(self.dev.outq), self.dev.out_off) == before:
                break

    def blob(self, message: bytes, counter: int = None) -> bytes:
        """message sealed on the next client counter (or the one given, which leaves the client's own alone)."""
        sm = phone_seal()
        c = self.ctr if counter is None else counter
        if counter is None:
            self.ctr += 1
        return sm.seal(*sm.seal_keys(*self.inputs, NODE_ID), sm.CLIENT_TO_DEVICE, c, message)

    def frames(self, blob: bytes, sub: int = DATA, max_write: int = None) -> list:
        return phone_seal().fragments(sub, blob, True, (max_write or max_value(self.mtu)))

    def send(self, message: bytes, note: str = None):
        """The client sends one message from the VM, sealed, fragment by fragment."""
        for i, f in enumerate(self.frames(self.blob(message))):
            self.write(f, note if i == 0 else None)

    def update(self, body: bytes, note: str = None, counter: int = None):
        """A credentials_update, sealed."""
        for i, f in enumerate(self.frames(self.blob(body, counter), CREDENTIALS_UPDATE)):
            self.write(f, note if i == 0 else None)


def ctrl_response(obj) -> bytes:
    return response_envelope(body_chunk_frame(CTRL_STREAM_ID, control_message(obj)))


def split(sub: int, blob: bytes, at: int) -> list:
    """One sealed message in two fragments, cut where the client chose (it may cut anywhere)."""
    return [bytes([MG_COMMAND_SESSION, sub, FIRST | FLAG_SEALED]) + le32(len(blob)) + blob[:at],
            bytes([MG_COMMAND_SESSION, sub, LAST | FLAG_SEALED]) + blob[at:]]


def invoke(rid: str, command: str, params: dict) -> bytes:
    return ctrl_response({"method": "link.invoke", "id": rid, "command": command, "params": params,
                          "timeout_ms": 5000})


def opened(r: Recorder, client_window=65536, epoch=1, reply=True):
    r.write(hello(65536, client_window) + bytes([SH]), "sealed hello: the client takes 64 KiB messages; the "
                                                         "credentials go sealed (device counter 0)")
    r.write(state(OPEN, epoch), "the VM session is open: the device opens /link-control and registers")
    r.send(response_envelope(response_frame(CTRL_STREAM_ID, 200)), "the VM accepts the /link-control stream")
    if reply:
        register_reply(r, "the link.register reply: credit back, nothing else")


def register_reply(r: Recorder, note: str):
    """The VM's answer to link.register, once the registration is out."""
    r.send(ctrl_response({"type": "res", "id": REGISTER_ID,
                          "result": {"status": "registered", "node_id": NODE_ID}}), note)


def open_exchange(mtu: int) -> dict:
    r = Recorder(f"open and register at MTU {mtu}", mtu, "A new connection after a matching token proof.")
    opened(r)
    return r.ex


def invoke_exchange(mtu: int) -> dict:
    r = Recorder(f"invoke at MTU {mtu}", mtu, "Muse runs the device's commands through the VM.")
    opened(r)
    r.send(invoke("inv-1", "display.card", {"title": "Standup moved", "body": "Now at 10:30 in Aspen"}),
           "link.invoke display.card: credit, then link.result")
    r.send(invoke("inv-2", "device.health", {}), "link.invoke device.health")
    r.send(invoke("inv-3", "device.fly", {}), "an unknown command: ok false with an error string")
    return r.ex


def credit_exchange() -> dict:
    r = Recorder("credit at MTU 247", 247, "The client's window is 64 bytes: the device waits for credit, and the "
                                           "client gives back only what it has consumed, never more than its "
                                           "window at a time.")
    opened(r, client_window=64, reply=False)
    r.write(credit(64), "64 more bytes")
    r.drain("64 at a time until the registration is out")
    register_reply(r, "the link.register reply, once the registration is out: credit back, nothing else")
    return r.ex


def credit_overrun_exchange() -> dict:
    mtu = 517
    r = Recorder("credit overrun at MTU 517", mtu,
                 f"The device's window is {DEVICE_WINDOW} bytes. A client fragment that would take the client past "
                 "the credit the device granted is dropped and counted with the message it belongs to: "
                 "invalid_value, every byte of it given back, nothing buffered. The next message goes through.")
    opened(r)
    blob = r.blob(bytes(6000))
    first = max_value(mtu) - 7          # command, sub-command, flags, total_len
    more = max_value(mtu) - 3
    r.write(bytes([MG_COMMAND_SESSION, DATA, FIRST | FLAG_SEALED]) + le32(len(blob)) + blob[:first],
            f"a {len(blob)}-byte message starts (client counter {r.ctr - 1}, used from here)")
    off = first
    while off + more <= DEVICE_WINDOW:
        r.write(bytes([MG_COMMAND_SESSION, DATA, FLAG_SEALED]) + blob[off:off + more])
        off += more
    r.write(bytes([MG_COMMAND_SESSION, DATA, FLAG_SEALED]) + blob[off:off + more],
            f"{off} bytes held: {more} more would pass the {DEVICE_WINDOW}-byte credit: invalid_value, the message "
            f"dropped, all {off + more} bytes given back")
    r.send(invoke("inv-1", "device.health", {}), f"link.invoke (counter {r.ctr}): credit, then link.result")
    return r.ex


def credit_counter_exchange() -> dict:
    first = len(request_envelope(request_frame(CTRL_STREAM_ID, "POST", "/link-control"))) + SEAL_OVERHEAD
    r = Recorder("credit below the counter at MTU 247", 247,
                 f"The client's window ({first + 2} bytes) takes the device's first sealed message ({first} bytes) and "
                 "leaves 2: a sealed message's first fragment carries the whole 4-byte counter, so the device starts "
                 "the next one only at 4 bytes of credit, then sends just the counter.")
    opened(r, client_window=first + 2, reply=False)
    r.write(credit(1), "3 bytes of credit: still under the counter, nothing")
    r.write(credit(1), "4 bytes: the next message's first fragment, its counter alone")
    r.drain("the rest of the registration, a window at a time")
    register_reply(r, "the link.register reply, once the registration is out: credit back, nothing else")
    return r.ex


def clear_exchange() -> dict:
    r = Recorder("sealed clear at MTU 247", 247,
                 "The Forget in a sealed session (mg_session_clear): sealed only, on the next "
                 "client counter, outside credit; taken, the device erases its tokens and K (back to Link setup), "
                 "answers a sealed [1] on its next counter and ends the session. Unsealed gets insecure, a wrong tag "
                 "invalid_value (its counter unused), a plaintext other than 0 invalid_value (its counter used); none "
                 "of them erases anything.")
    r.write(hello(65536, 65536) + bytes([SH]), "sealed hello: the credentials, sealed (device counter 0)")
    r.write(bytes([MG_COMMAND_SESSION, CLEAR, FIRST | LAST]) + le32(1) + b"\x00",
            "an unsealed clear: insecure, nothing erased")
    good0 = r.blob(b"\x00", counter=0)
    forged = good0[:-1] + bytes([good0[-1] ^ 1])
    r.write(r.frames(forged, CLEAR)[0], "a sealed clear with a wrong tag (counter 0): invalid_value, the counter unused")
    r.write(r.frames(r.blob(b"\x01"), CLEAR)[0], "a sealed clear whose plaintext isn't 0 (counter 0): invalid_value, "
                                                   "the counter used")
    r.write(r.frames(r.blob(b"\x00"), CLEAR)[0], "the sealed clear (counter 1): erased; the answer sealed [1] on device "
                                                   "counter 1; the session ends")
    r.update(compact({"access_token": "a", "refresh_token": "r"}).encode(), "a credentials_update after it: no session",
             counter=2)
    r.proof(2, "a new token proof")
    r.write(hello(65536, 65536) + bytes([SH]), "a sealed hello: not_found, the gadget holds no token")
    return r.ex


def clear_busy_exchange() -> dict:
    r = Recorder("sealed clear while a credentials_update arrives at MTU 247", 247,
                 "A sealed clear whose first fragment finds nowhere to be held (here a credentials_update part-way "
                 "holds the storage) is answered busy and has used its counter (the first fragment carries "
                 "the whole counter); the update goes on, and the client's retry comes on the "
                 "next counter.")
    r.write(hello(65536, 65536) + bytes([SH]), "sealed hello: the credentials, sealed (device counter 0)")
    body = compact({"access_token": "a" * 300, "refresh_token": "r"}).encode()
    upd = r.frames(r.blob(body), CREDENTIALS_UPDATE)
    assert len(upd) >= 2
    r.write(upd[0], "a credentials_update's first fragment (client counter 0)")
    r.write(r.frames(r.blob(b"\x00"), CLEAR)[0], "a sealed clear (counter 1) while it arrives: busy, counter 1 used")
    for i, f in enumerate(upd[1:]):
        r.write(f, "the rest of the update: committed, credentials_saved sealed on device counter 1"
                if i == len(upd) - 2 else None)
    r.write(r.frames(r.blob(b"\x00"), CLEAR)[0], "the retry on counter 2: erased; the answer sealed [1] on device "
                                                   "counter 2; the session ends")
    return r.ex


def clear_hold_exchange() -> dict:
    r = Recorder("sealed clear while a device message is going out at MTU 247", 247,
                 "The clear is taken the moment its tag checks: the tokens and K go at once, and only the sealed "
                 "[1] waits for the device message already part way out (its counter was spent at its first "
                 "fragment). A credentials_update or data meanwhile is refused, so the [1] still means erased. "
                 "The client's window here is 64 bytes, so the registration is still going out when the clear "
                 "arrives.")
    opened(r, client_window=64, reply=False)
    clear_ctr = r.ctr
    r.write(r.frames(r.blob(b"\x00"), CLEAR)[0],
            f"the sealed clear (counter {clear_ctr}): erased at once, its answer waiting for the registration")
    r.send(invoke("inv-1", "device.health", {}), f"data after it (counter {r.ctr}): invalid_value, credit back, the "
                                                 "command doesn't run")
    r.update(compact({"access_token": "a", "refresh_token": "r"}).encode(),
             f"a credentials_update after it (counter {r.ctr}): invalid_value, nothing stored")
    r.drain(f"credit: the rest of the registration, then the sealed [1] on device counter {r.dev.tx}; the session "
            "ends")
    r.proof(2, "a new token proof: the gadget holds no token any more")
    return r.ex


def replay_after_proof_exchange() -> dict:
    r = Recorder("a sealed message replayed after a new hello at MTU 247", 247,
                 "A new proof and the sealed hello after it drop every sealed message part way in: the old "
                 "session's half-sent clear is gone, so its last fragment finds no message open and can't give a "
                 "counter back, and the new session's counter 2 stays used, so the invoke recorded off the link "
                 "can't be replayed into a second run.")
    opened(r)
    first, last = split(CLEAR, r.blob(b"\x00"), 10)
    r.write(first, "a sealed clear's first fragment (counter 2): held, waiting for the rest")
    r.proof(2, "a new token proof: the partial clear goes with the old session")
    r.write(hello(65536, 65536) + bytes([SH]), "the sealed hello: the counters restart, the credentials sealed")
    r.write(state(OPEN, 2), "open: the device registers again")
    r.send(response_envelope(response_frame(CTRL_STREAM_ID, 200)), "the VM accepts the stream (counter 0)")
    r.send(ctrl_response({"type": "res", "id": REGISTER_ID,
                          "result": {"status": "registered", "node_id": NODE_ID}}),
           "the link.register reply (counter 1)")
    inv = r.blob(invoke("inv-1", "device.health", {}))
    for i, f in enumerate(r.frames(inv)):
        r.write(f, "link.invoke device.health (counter 2): credit, then link.result" if i == 0 else None)
    r.write(last, "the old session's clear fragment: no message open, invalid_value, no counter given back")
    for i, f in enumerate(r.frames(inv)):
        r.write(f, "the invoke replayed (counter 2): refused, credit back, the command doesn't run again"
                if i == 0 else None)
    return r.ex


def stored_too_large_exchange() -> dict:
    r = Recorder("a credentials_update the stored document can't hold at MTU 517", 517,
                 "The device hands its whole stored document back in its next credentials, so that document must "
                 "fit MG_SESSION_MAX_CREDENTIALS as well: an update small enough on its own, but too large once "
                 "stored beside node_id and the device key, is refused invalid_value and nothing is stored.")
    r.write(hello(65536, 65536) + bytes([SH]), "sealed hello: the stored credentials, sealed")
    fixed = len(compact(dict(TEST_CREDENTIALS, access_token="", refresh_token="")).encode())
    body = compact({"access_token": "a" * (MAX_CREDENTIALS + 7 - fixed), "refresh_token": "r"}).encode()
    r.update(body, f"an update of {len(body)} bytes: stored it would be {MAX_CREDENTIALS + 8} bytes, so "
                   "invalid_value, nothing stored")
    r.proof(2, "a new token proof")
    r.write(hello(65536, 65536) + bytes([SH]), "a new sealed hello: the stored pair is unchanged")
    return r.ex


def small_window_exchange() -> dict:
    r = Recorder("a sealed window under 4 at MTU 247", 247,
                 "A sealed session's window holds at least the 4-byte counter (a sealed first fragment carries it "
                 "whole): a sealed hello with a window under 4 is refused invalid_value at once; 4 is accepted.")
    r.write(hello(65536, 3) + bytes([SH]), "window 3: invalid_value, no session")
    r.write(hello(65536, 4) + bytes([SH]), "window 4: the session opens, the credentials sealed")
    return r.ex


def session_exchange() -> dict:
    r = Recorder("session end and reopen at MTU 247", 247, "Sessions don't carry over.", task=True)
    opened(r)
    frames = r.frames(r.blob(invoke("inv-1", "device.health", {})), max_write=97)
    r.write(frames[0], "first fragment of an invoke (its counter used from here)")
    r.write(state(NO_NETWORK, 1), "the phone lost its network: the partial message and the session go; "
                                  "the device owes that invoke an answer, so it asks again at once")
    r.write(frames[1], "the rest arrives after the session ended: error")
    r.write(state(CONNECTING, 1), "the phone is reconnecting")
    r.write(state(OPEN, 2), "a new session: the device starts again from /link-control")
    return r.ex


def reask_exchange() -> dict:
    r = Recorder("re-ask after an unasked end at MTU 247", 247,
                 "A device asks for a session only while a task is active (a user action, traffic it owes, a wait, "
                 "a wake): after a state it didn't ask for (closed, no_network) it sends open_request [1] at "
                 "once, then backs off (1, 2, 4 ... 60 s after closed, 1, 2, 4 ... 15 s after no_network; not "
                 "shown); the two series are separate, and an open restarts a series only after a session that "
                 "stayed open 60 s, so after a short one the same kind of end continues it; registered commands "
                 "alone make it ask nothing; none after not_enrolled, auth_failed or too_large until a new hello, "
                 "a credentials_update or an open; a credentials_update that saves is traffic it owes and asks at "
                 "once. The client never reopens on its own.")
    opened(r)
    r.write(state(CLOSED, 1), "closed with no task active: the device asks nothing")
    r.task(True, "a task starts (a talk press): it asks for a session at once, the closed series' first ask")
    r.write(state(CONNECTING, 1), "the client is opening: nothing")
    r.write(state(OPEN, 2), "open: the device starts again from /link-control")
    r.write(state(NO_NETWORK, 2), "no_network after a short session: its own series (not the closed "
                                  "one), so the device asks again at once")
    r.task(False, "the task ends")
    r.write(state(CLOSED, 2), "a closed with no task: nothing")
    r.task(True, "a task starts again: the closed series is running, so the next ask waits for its step (timed, "
                 "not shown)")
    r.write(state(NOT_ENROLLED, 2), "not_enrolled: asking again can't fix it, no re-ask")
    r.write(state(CLOSED, 2), "a closed after it: still no re-ask")
    r.proof(2, "a new token proof: the session reset at its challenge")
    r.write(hello(65536, 65536) + bytes([SH]), "a new sealed hello: re-asks allowed again")
    r.write(state(AUTH_FAILED, 1), "auth_failed: no re-ask")
    new = {"access_token": "test-access-token-0002", "refresh_token": "test-refresh-token-0002"}
    r.update(compact(new).encode(), "a credentials_update that saves while closed: answered sealed, re-asks allowed "
                                    "again, and the device asks at once (new credentials may fix it)")
    r.write(state(TOO_LARGE, 1), "too_large: no re-ask")
    r.write(state(CLOSED, 1), "a closed after it: still no re-ask")
    return r.ex


def wake_exchange() -> dict:
    r = Recorder("wake at MTU 247", 247,
                 "mg_session_wake (mg_session_cap_wake): the VM asked the phone to wake this device, so the client "
                 "writes wake, never sealed, with no reply of its own. Before the hello it is ignored; after it, "
                 "the device takes it as a task of its own and asks for a session at once, then registers.")
    r.write(bytes([MG_COMMAND_SESSION, WAKE]), "wake before the hello: ignored")
    r.write(hello(65536, 65536) + bytes([SH]), "sealed hello: the credentials, sealed (device counter 0)")
    r.write(state(CLOSED, 1), "no session and no task: the device asks nothing")
    r.write(bytes([MG_COMMAND_SESSION, WAKE]), "wake: a task of its own, open_request [1] at once")
    r.write(bytes([MG_COMMAND_SESSION, WAKE]), "a second wake while the series runs: the next ask waits for its "
                                               "step (timed, not shown)")
    r.write(state(OPEN, 1), "the client opened one: the device registers")
    return r.ex


def failed_open_exchange() -> dict:
    r = Recorder("failed open at MTU 247", 247,
                 "A closed or no_network after connecting, with no session ever open (the client's open "
                 "attempt failed), is an end the device didn't ask for like any other: with a task active, the "
                 "first of a series asks at once; while that kind's series runs, the next ask waits for its step "
                 "(timed, not shown). The device never drops it, or it would go silent after a failed open.",
                 task=True)
    r.write(hello(65536, 65536) + bytes([SH]), "sealed hello: the client takes 64 KiB messages")
    r.write(state(CONNECTING, 1), "the client is opening: nothing")
    r.write(state(CLOSED, 1), "the open failed before a session was up: not asked for, so the device "
                              "asks again at once (the closed series' first ask)")
    r.write(state(CONNECTING, 1), "opening again: nothing")
    r.write(state(NO_NETWORK, 1), "that open failed for the network: the no_network series' first ask, "
                                  "at once")
    r.write(state(CONNECTING, 1), "opening again: nothing")
    r.write(state(CLOSED, 1), "failed again: the closed series runs, so the next ask waits for its "
                              "step (timed, not shown), never dropped")
    return r.ex


def expires_exchange() -> dict:
    r = Recorder("credentials refresh with access_expires_at at MTU 100", 100,
                 "The client's refreshed pair carries access_expires_at (unix seconds): the device keeps it with "
                 "the pair and hands it back last in its next credentials; an update without it clears it; any "
                 "form but a positive integer that fits 64 bits counts as none; a duplicate is invalid_value.")
    r.write(hello(65536, 65536) + bytes([SH]), "sealed hello: the credentials, no access_expires_at kept yet")
    new = {"access_token": "test-access-token-0002", "refresh_token": "test-refresh-token-0002",
           EXPIRES_KEY: 1893456000}
    r.update(compact(new).encode(), "credentials_update with access_expires_at: saved, kept with the pair")
    r.proof(2, "a new token proof")
    r.write(hello(65536, 65536) + bytes([SH]), "a new sealed hello: the new pair, access_expires_at last")
    new = {"access_token": "test-access-token-0003", "refresh_token": "test-refresh-token-0003",
           EXPIRES_KEY: "1893456000"}
    r.update(compact(new).encode(), "access_expires_at as a string: saved, kept as none")
    r.proof(3, "a new token proof")
    r.write(hello(65536, 65536) + bytes([SH]), "a new sealed hello: no access_expires_at")
    new = {"access_token": "test-access-token-0004", "refresh_token": "test-refresh-token-0004",
           EXPIRES_KEY: (1 << 64) - 1}
    r.update(compact(new).encode(), "the largest that fits (2^64 - 1): saved, kept")
    dup = (b'{"access_token":"test-access-token-0005","refresh_token":"test-refresh-token-0005",'
           b'"access_expires_at":1,"access_expires_at":2}')
    r.update(dup, "a duplicate access_expires_at: error, nothing stored (its counter used)")
    new = {"access_token": "test-access-token-0006", "refresh_token": "test-refresh-token-0006"}
    r.update(compact(new).encode(), "an update without access_expires_at: saved, the stored one cleared")
    r.proof(4, "a new token proof")
    r.write(hello(65536, 65536) + bytes([SH]), "a new sealed hello: the last pair, no access_expires_at")
    return r.ex


def credentials_exchange() -> dict:
    r = Recorder("credentials refresh at MTU 100", 100,
                 "The client refreshed the token pair and hands it back for the device to store.")
    r.write(hello(65536, 65536) + bytes([SH]), "sealed hello: the device sends its credentials, sealed, right "
                                                 "after its own hello")
    new = {"access_token": "test-access-token-0002", "refresh_token": "test-refresh-token-0002"}
    r.update(compact(new).encode(), "credentials_update with the new pair: saved, answered sealed")
    r.update(b'["not","an","object"]', "not an object: error, nothing stored")
    r.update(compact({"access_token": "x"}).encode(), "no refresh_token: error, nothing stored")
    r.update(b" " + compact({"access_token": "test-access-token-0003", "refresh_token": "test-refresh-token-0003"})
             .encode(), "a space before the object (the first byte must be '{'): error, nothing stored")
    r.write(bytes([MG_COMMAND_SESSION, CREDENTIALS_UPDATE, FIRST | FLAG_SEALED])
            + le32(MAX_CREDENTIALS + SEAL_OVERHEAD + 1) + struct.pack(">I", r.ctr) + b"{",
            "over MG_SESSION_MAX_CREDENTIALS")
    r.proof(2, "a new token proof")
    r.write(hello(65536, 65536) + bytes([SH]), "a new sealed hello: the stored pair is the new one")
    return r.ex


def bound_exchange() -> dict:
    r = Recorder("credentials with a VM binding at MTU 100", 100,
                 "Setup stored the device key with the VM it is bound to: the credentials carry "
                 "vm_id and vm_kind after key_id, and a refreshed pair keeps them.",
                 vm_binding=True)
    r.write(hello(65536, 65536) + bytes([SH]), "sealed hello: the credentials, vm_id and vm_kind last")
    new = {"access_token": "test-access-token-0002", "refresh_token": "test-refresh-token-0002"}
    r.update(compact(new).encode(), "credentials_update with the new pair: saved")
    r.proof(2, "a new token proof")
    r.write(hello(65536, 65536) + bytes([SH]), "a new sealed hello: the new pair, the same binding")
    return r.ex


def client_counter_top_exchange() -> dict:
    r = Recorder("the client's counter at the top at MTU 247", 247,
                 "Counters never wrap: once the device has taken client counter 0xFFFFFFFF it takes no other in that "
                 "session, so a message at a counter that would wrap to 0 (here the session's first, replayed) is "
                 "refused; the client starts a new session (a new proof and sealed hello) instead of wrapping.")
    opened(r)
    first = r.frames(r.blob(response_envelope(response_frame(CTRL_STREAM_ID, 200)), counter=0))
    r.counters(rx=COUNTER_MAX, note="a stand-in for a long session: the device's next client counter is 0xFFFFFFFF")
    r.send(invoke("inv-1", "device.health", {}), "link.invoke at client counter 0xFFFFFFFF, the last: credit, then "
                                                 "link.result")
    for i, f in enumerate(first):
        r.write(f, "the session's first message (client counter 0) replayed: refused, credit back" if i == 0 else None)
    r.proof(2, "a new token proof")
    r.write(hello(65536, 65536) + bytes([SH]), "a new sealed hello: fresh keys, both counters from 0")
    return r.ex


def device_counter_top_exchange() -> dict:
    r = Recorder("the device's counter at the top at MTU 247", 247,
                 "Counters never wrap: once the device has sealed counter 0xFFFFFFFF it seals nothing more in that "
                 "session and refuses each sealed message from the client with invalid_value, which ends it; a new "
                 "proof and sealed hello start fresh keys and counters.")
    opened(r)
    r.counters(tx=COUNTER_MAX, note="a stand-in for a long session: the device's next counter is 0xFFFFFFFF")
    r.send(invoke("inv-1", "device.health", {}), "link.invoke: credit, then link.result on device counter "
                                                 "0xFFFFFFFF, the last")
    r.send(invoke("inv-2", "device.health", {}), "link.invoke: invalid_value, credit back, no result (the device's "
                                                 "counters are spent: the session is over)")
    r.proof(2, "a new token proof")
    r.write(hello(65536, 65536) + bytes([SH]), "a new sealed hello: the credentials on device counter 0")
    return r.ex


def low_mtu_exchange() -> dict:
    r = Recorder("a sealed hello below MG_MIN_ATT_MTU", 23,
                 "The sealed session runs only at ATT_MTU >= MG_MIN_ATT_MTU (100), where its one-write messages "
                 "(such as credentials_saved, 36 bytes) always fit: a sealed hello on a link below it "
                 "gets invalid_value and changes nothing.")
    r.write(hello(65536, 65536) + bytes([SH]), "a sealed hello at ATT_MTU 23: invalid_value, no session")
    return r.ex


def errors_exchange() -> dict:
    r = Recorder("errors at MTU 247", 247, "Each step stands alone unless it says otherwise.")
    r.write(bytes([MG_COMMAND_SESSION, DATA, FIRST | LAST]) + le32(2) + b"{}", "data before hello")
    r.write(credit(10), "credit before hello")
    r.write(bytes([MG_COMMAND_SESSION, CREDENTIALS_UPDATE, FIRST | LAST]) + le32(2) + b"{}",
            "credentials_update before hello")
    r.write(hello(65536, 65536, version=2) + bytes([SH]), "an unknown version")
    r.write(hello(MIN_MESSAGE - 1, 1024) + bytes([SH]), "max_message below MG_SESSION_MIN_MESSAGE")
    r.write(hello(65536, 0) + bytes([SH]), "window 0")
    r.write(bytes([MG_COMMAND_SESSION, HELLO, 1, 0, 0]), "hello too short")
    r.write(hello(65536, 65536) + bytes([SH]), "a good hello")
    r.write(bytes([MG_COMMAND_SESSION, DATA, FIRST | LAST | FLAG_SEALED]) + le32(2) + b"{}", "data while not open")
    r.write(state(OPEN, 1), "open")
    good = response_envelope(response_frame(CTRL_STREAM_ID, 200))
    sd = FLAG_SEALED
    r.write(bytes([MG_COMMAND_SESSION, DATA, LAST | sd]) + b"x", "a continuation with no message open")
    r.write(bytes([MG_COMMAND_SESSION, DATA, FIRST | LAST | sd]) + le32(0) + struct.pack(">I", r.ctr),
            "total_len 0 (the whole counter, so only the total is wrong)")
    r.write(bytes([MG_COMMAND_SESSION, DATA, FIRST | sd]) + le32(DEVICE_MAX_MESSAGE + 1) + struct.pack(">I", r.ctr)
            + b"x", "total_len over the device's max_message (the whole counter, so only the total is wrong)")
    r.write(bytes([MG_COMMAND_SESSION, DATA, FIRST | LAST | sd]) + le32(SEAL_OVERHEAD) + struct.pack(">I", r.ctr)
            + bytes(16), "total_len not over the seal's 20 bytes: no plaintext")
    r.write(bytes([MG_COMMAND_SESSION, DATA, FIRST | sd]) + le32(40) + b"\x00\x00\x00",
            "a sealed first fragment shorter than the 4-byte counter: invalid_length, its bytes back")
    r.write(bytes([MG_COMMAND_SESSION, CREDENTIALS_UPDATE, FIRST | FLAG_SEALED]) + le32(40) + b"\x00\x00",
            "the same for a credentials_update: invalid_length, nothing stored")
    b0 = r.blob(good)
    r.write(bytes([MG_COMMAND_SESSION, DATA, FIRST | sd]) + le32(len(b0)) + b0[:7],
            "start a message (client counter 0, used from here)")
    r.write(bytes([MG_COMMAND_SESSION, DATA, FIRST | sd]) + le32(len(b0)) + b0[:7],
            "a first fragment while one is open: error, both dropped, credit back")
    b1 = r.blob(b"abcd")
    r.write(bytes([MG_COMMAND_SESSION, DATA, FIRST | sd]) + le32(len(b1)) + b1[:20],
            "start a 24-byte message (counter 1)")
    r.write(bytes([MG_COMMAND_SESSION, DATA, LAST | sd]) + b1[20:] + b"e", "bytes past total_len: error, credit back")
    b2 = r.blob(b"abcd")
    r.write(bytes([MG_COMMAND_SESSION, DATA, FIRST | sd]) + le32(len(b2)) + b2[:10], "start a message (counter 2)")
    r.write(bytes([MG_COMMAND_SESSION, ABORT]), "abort: dropped, credit back, its counter used")
    b3 = r.blob(good)
    for i, f in enumerate(r.frames(b3)):
        r.write(f, "a whole message still goes through (counter 3)" if i == 0 else None)
    for i, f in enumerate(r.frames(b3)):
        r.write(f, "the same message again: its counter (3) refused, credit back" if i == 0 else None)
    r.write(bytes([MG_COMMAND_SESSION, DATA, FIRST | LAST]) + le32(len(good)) + good,
            "unsealed data in a sealed session: insecure, its bytes back, nothing changes")
    b4 = r.blob(good)
    forged = b4[:-1] + bytes([b4[-1] ^ 1])
    for i, f in enumerate(r.frames(forged)):
        r.write(f, "counter 4 with a bad tag: error, credit back, the counter not used" if i == 0 else None)
    for i, f in enumerate(r.frames(b4)):
        r.write(f, "the real counter 4: goes through" if i == 0 else None)
    r.write(bytes([MG_COMMAND_SESSION, OPEN_REQUEST, 1]), "a device -> client sub-command from the client")
    r.write(bytes([MG_COMMAND_SESSION, CREDENTIALS, FIRST | LAST]) + le32(2) + b"{}",
            "credentials from the client: a device -> client sub-command")
    r.write(bytes([MG_COMMAND_SESSION, 0x7F]), "an unknown sub-command")
    r.write(bytes([MG_COMMAND_SESSION, STATE, 9]) + le16(1), "an unknown state")
    r.write(bytes([MG_COMMAND_SESSION, STATE, OPEN]), "state too short")
    r.write(bytes([MG_COMMAND_SESSION, CREDIT, 1]), "credit too short")
    r.write(bytes([MG_COMMAND_SESSION]), "no sub-command")
    return r.ex


def malformed_data_exchange() -> dict:
    r = Recorder("data that isn't a service envelope at MTU 247", 247,
                 "A client data message whose plaintext doesn't decode as a ServiceResponseEnvelope gets "
                 "invalid_value, its credit back and its counter used. Here the plaintexts of a clear, a "
                 "credentials_update, as a relabel to data would bring them, and six bytes that aren't one; then a "
                 "real envelope goes through.")
    opened(r)
    r.send(b"\x00", f"a clear's plaintext [0] as data (counter {r.ctr}): invalid_value, credit back")
    r.send(compact({"access_token": "a", "refresh_token": "r"}).encode(),
           f"a credentials_update's plaintext as data (counter {r.ctr}): invalid_value, credit back")
    r.send(bytes([1, 1, 0, 0, 0x08, 0x07]),
           f"six bytes that aren't an envelope as data (counter {r.ctr}): invalid_value, credit back")
    r.send(invoke("inv-1", "device.health", {}), f"link.invoke device.health (counter {r.ctr}): credit, then "
                                                 "link.result")
    return r.ex


def sealed_update_frames(counter: int, plaintext: bytes, mtu: int) -> list:
    s = phone_seal()
    e, m = s.seal_keys(*session_seal_inputs(), NODE_ID)
    return s.fragments(CREDENTIALS_UPDATE, s.seal(e, m, s.CLIENT_TO_DEVICE, counter, plaintext), True, max_value(mtu))


def sealed_credentials_exchange(mtu: int) -> dict:
    r = Recorder(f"sealed credentials at MTU {mtu}", mtu,
                 "A sealed session (session_seal): the device's credentials sealed on device -> client counter 0, "
                 "the client's refreshed pair sealed on client -> device counter 0 and answered with a sealed "
                 "credentials_saved (counter 1), then the refusals.")
    r.write(hello(65536, 65536) + bytes([0x03]), "sealed hello (sealed | sealed_saved): the device echoes both "
                                                   "flags, then its credentials, sealed")
    new = {"access_token": "test-access-token-0002", "refresh_token": "test-refresh-token-0002"}
    for i, f in enumerate(sealed_update_frames(0, compact(new).encode(), mtu)):
        r.write(f, "credentials_update, sealed (counter 0): saved, answered sealed (counter 1)" if i == 0 else None)
    for i, f in enumerate(sealed_update_frames(0, compact(new).encode(), mtu)):
        r.write(f, "the same update replayed (counter 0 again): error, nothing stored" if i == 0 else None)
    for i, f in enumerate(sealed_update_frames(1, b'["not","an","object"]', mtu)):
        r.write(f, "not an object (counter 1): error, nothing stored; its counter is used" if i == 0 else None)
    for i, f in enumerate(sealed_update_frames(2, compact({"access_token": "x"}).encode(), mtu)):
        r.write(f, "no refresh_token (counter 2): error, nothing stored" if i == 0 else None)
    r.write(bytes([MG_COMMAND_SESSION, CREDENTIALS_UPDATE, FIRST | FLAG_SEALED])
            + le32(MAX_CREDENTIALS + SEAL_OVERHEAD + 1) + le32(3)[::-1] + b"{",
            "over MG_SESSION_MAX_CREDENTIALS sealed")
    s = phone_seal()
    e, m = s.seal_keys(*session_seal_inputs(), NODE_ID)
    split = s.fragments(CREDENTIALS_UPDATE, s.seal(e, m, s.CLIENT_TO_DEVICE, 3, compact(new).encode()), True,
                        min(max_value(mtu), 40))
    split[-1] = split[-1][:2] + bytes([split[-1][2] & ~FLAG_SEALED]) + split[-1][3:]
    for i, f in enumerate(split):
        r.write(f, "a sealed credentials_update (counter 3) whose last fragment lacks the sealed flag: error, "
                "nothing stored; its counter is used" if i == 0 else None)
    for i, f in enumerate(fragments(compact(new).encode(), mtu, CREDENTIALS_UPDATE)):
        r.write(f, "an unsealed credentials_update in a sealed session: insecure, nothing stored" if i == 0 else None)
    r.write(hello(65536, 65536) + bytes([0x03]),
            "a second sealed hello under the same token proof: proof_required, the session goes on")
    return r.ex


def sealed_bound_exchange() -> dict:
    r = Recorder("sealed credentials with a VM binding at MTU 100", 100,
                 "Setup stored the device key with the VM it is bound to: the sealed credentials carry vm_id and "
                 "vm_kind after key_id, and a refreshed pair keeps them.", vm_binding=True)
    r.write(hello(65536, 65536) + bytes([0x03]), "sealed hello: the credentials, vm_id and vm_kind last, sealed")
    new = {"access_token": "test-access-token-0002", "refresh_token": "test-refresh-token-0002"}
    for i, f in enumerate(sealed_update_frames(0, compact(new).encode(), 100)):
        r.write(f, "credentials_update, sealed: saved, answered sealed" if i == 0 else None)
    return r.ex


def sealed_legacy_exchange() -> dict:
    r = Recorder("sealed credentials for an older client at MTU 247", 247,
                 "A sealed hello without sealed_saved (a client from before sealed_saved): the credentials sealed, "
                 "the update sealed, its answer the one-byte credentials_saved, no device counter.")
    r.write(hello(65536, 65536) + bytes([0x01]), "sealed hello (sealed only): the device echoes sealed")
    new = {"access_token": "test-access-token-0002", "refresh_token": "test-refresh-token-0002"}
    for i, f in enumerate(sealed_update_frames(0, compact(new).encode(), 247)):
        r.write(f, "credentials_update, sealed: saved, the one-byte answer" if i == 0 else None)
    return r.ex


def unsealed_refused_exchange() -> dict:
    r = Recorder("an unsealed hello refused at MTU 247", 247,
                 "Credentials leave only sealed: an unsealed hello is refused insecure on any link (a relayed token "
                 "proof on the relay's own encrypted bond gets nothing), and leaves the proof's nonces untaken, so "
                 "the client's sealed hello after it opens the session.")
    r.write(hello(65536, 65536), "an unsealed hello on an encrypted link: insecure")
    for i, f in enumerate(fragments(compact({"access_token": "a", "refresh_token": "r"}).encode(), 247,
                                    CREDENTIALS_UPDATE)):
        r.write(f, "a credentials_update with no session: error, nothing stored" if i == 0 else None)
    r.write(hello(65536, 65536) + bytes([0x03]), "the sealed hello: the session opens, the credentials sealed")
    return r.ex


def no_proof_exchange() -> dict:
    r = Recorder("no proof", 247, "Before a matching token proof the hello is refused; after one it opens.", proof=False)
    r.write(hello(65536, 65536) + bytes([SH]), "before a matching token proof: proof_required")
    r.proof(2, "the token proof matches")
    r.write(hello(65536, 65536) + bytes([SH]), "the sealed hello: answered, the credentials sealed")
    return r.ex


def keepalive_exchange() -> dict:
    r = Recorder("an empty control record at MTU 247", 247, "An empty record on /link-control (length 0) is a "
                 "keepalive: the device skips it and reads the next.")
    opened(r)
    records = ctrl_body(b"") + ctrl_body(compact({"method": "link.invoke", "id": "inv-1", "command": "device.health",
                                                  "params": {}, "timeout_ms": 5000}).encode())
    r.send(response_envelope(body_chunk_frame(CTRL_STREAM_ID, records)),
           "an empty record, then link.invoke in the same body chunk: credit, then link.result")
    return r.ex


def nudge_exchange() -> dict:
    r = Recorder("the credit keepalive at MTU 247", 247,
                 "While a session is open the device may nudge the phone (event nudge, every 20 s while a screen "
                 "waits on the session, say): it sends the credit it owes for messages it has fully taken, or "
                 "credit 0 when it owes none (a message still arriving earns none); with no session open it sends "
                 "nothing. credit 0 changes nothing either way.")
    r.nudge("a nudge before any hello: no session, nothing")
    opened(r)
    r.nudge("a nudge with nothing owed: credit 0")
    frames = r.frames(r.blob(invoke("inv-1", "device.health", {})), max_write=100)
    r.write(frames[0], "the first fragment of a link.invoke: held (a message still arriving earns no credit)")
    r.nudge("a nudge while it arrives: credit 0")
    for f in frames[1:]:
        r.write(f, None)
    r.ex["steps"][-1]["note"] = "the rest of the link.invoke: the whole message's credit, then link.result"
    r.write(credit(0), "credit 0 from the client: valid, nothing changes, no answer")
    r.write(state(CLOSED, 1), "the VM session closes")
    r.nudge("a nudge with no session open: nothing")
    return r.ex


def ctrl_body(body: bytes) -> bytes:
    return le32(len(body)) + body


def clear_unsent_exchange() -> dict:
    first = len(request_envelope(request_frame(CTRL_STREAM_ID, "POST", "/link-control"))) + SEAL_OVERHEAD
    r = Recorder("sealed clear while a device message is reserved but not started at MTU 247", 247,
                 f"The client's window ({first + 2} bytes) leaves 2 bytes after the stream's open, so the "
                 "registration has its counter but no byte out when the clear arrives: it never goes out, its "
                 "counter goes back, and the sealed [1] goes at once on that counter.")
    opened(r, client_window=first + 2, reply=False)
    clear_ctr = r.ctr
    r.write(r.frames(r.blob(b"\x00"), CLEAR)[0], f"the sealed clear (counter {clear_ctr}): erased, the registration "
                                                   "dropped, the sealed [1] at once on device counter 2, the "
                                                   "registration's; the session ends")
    return r.ex


def credentials_wait_exchange() -> dict:
    r = Recorder("sealed credentials that wait for room at MTU 247", 247,
                 "A board with no room for the sealed credentials when it answers the hello sends them once its "
                 "stack has room, still on device counter 0: no device counter is skipped.", credentials_wait=True)
    r.write(hello(65536, 65536) + bytes([SH]), "sealed hello: answered; the credentials wait (counter 0 unused)")
    r.buffer("the stack has room: the credentials, sealed on device counter 0")
    return r.ex


def clear_credentials_unsent_exchange() -> dict:
    r = Recorder("sealed clear while the credentials wait at MTU 247", 247,
                 "A device message not yet started never goes out and its counter is never used: here the "
                 "credentials still wait for room when the clear arrives, so they're dropped and the sealed [1] "
                 "goes on device counter 0, the next counter the client takes.", credentials_wait=True)
    r.write(hello(65536, 65536) + bytes([SH]), "sealed hello: answered; the credentials wait (counter 0 unused)")
    r.write(r.frames(r.blob(b"\x00"), CLEAR)[0], "the sealed clear (client counter 0): erased, the credentials "
                                                   "dropped, the sealed [1] at once on device counter 0; the session "
                                                   "ends")
    r.buffer("the stack has room: nothing goes (the credentials were dropped)")
    return r.ex


def gating_exchange(name: str, note: str, **device) -> dict:
    r = Recorder(name, 247, note, **device)
    r.write(hello(65536, 65536) + bytes([SH]))
    return r.ex


def unencrypted_exchange() -> dict:
    r = Recorder("not encrypted", 247, "On an unencrypted connection: the sealed hello needs no bond; an unsealed "
                                       "one is refused insecure, as on any link.", encrypted=False)
    r.write(hello(65536, 65536), "an unsealed hello: insecure")
    r.write(hello(65536, 65536) + bytes([SH]), "the sealed hello: answered, the credentials sealed")
    return r.ex


HELLO_FLAG_SEALED, HELLO_FLAG_SEALED_SAVED = 1 << 0, 1 << 1
FLAG_SEALED = 1 << 2


def phone_seal():
    """mg_session_seal_ref.py's seal (next to this file): every session is sealed."""
    mod = sys.modules.get("mg_session_seal_ref")
    if mod is None:
        import importlib.util
        spec = importlib.util.spec_from_file_location("mg_session_seal_ref",
                                                      Path(__file__).with_name("mg_session_seal_ref.py"))
        mod = importlib.util.module_from_spec(spec)
        keep, sys.dont_write_bytecode = sys.dont_write_bytecode, True  # no __pycache__ beside the vectors
        try:
            spec.loader.exec_module(mod)
        finally:
            sys.dont_write_bytecode = keep
        sys.modules["mg_session_seal_ref"] = mod
    return mod


def session_seal_inputs() -> tuple:
    """The sessions' fixed test K and proof nonces (fake), as mg_session_seal_ref.py makes its own."""
    s = phone_seal()
    return s.det("session K", 32), s.det("session cn", 16), s.det("session dn", 16)


# ---- the microphone gate ----------------------------------------------------------------------------------------
# On a release build of a set-up gadget a client's start_mic needs, besides the token proof, a press, gesture or wake
# word the gadget itself detected in the last PRESS_WINDOW_MS; console words and protocol commands (mg_session_wake
# included) never count. Otherwise error press_required, and nothing is captured. Automation builds need the proof
# alone.

MG_COMMAND_START_MIC, MG_COMMAND_STOP_MIC, MG_COMMAND_CHANGE_DATA_TYPE = 1, 2, 8
PRESS_REQUIRED = 9
PRESS_WINDOW_MS = 5000
AUDIO_SBC = 1


def command_error(command: int, code: int) -> bytes:
    return bytes([MG_COMMAND_ERROR, command, ERROR_DATA_CODE]) + le16(code)


def start_mic() -> bytes:
    return bytes([MG_COMMAND_START_MIC, AUDIO_SBC])


class MicGateDevice(Device):
    """start_mic and stop_mic on a set-up gadget, with the device session beside them. Events are the gadget's own:
    press (its talk control), wake_word (a wake word it heard), console_press (the console's press word), charger (a
    charger plugged in), console_capture (a console word that starts capture; source wireless or wired: a wireless
    one follows start_mic's rule, a wired one is the owner's), time (its clock moves ms; nothing else moves it) and
    reconnect (a new link, its token proof still to come). It captures SBC at 16 kHz whatever codec is asked for."""

    def __init__(self, mtu: int, release=True, **flags):
        super().__init__(mtu, **flags)
        self.release = release
        self.now = 0
        self.pressed_at = None
        self.capturing = False
        self.console_answer = None   # what the last console capture word got, on the console

    def write(self, frame: bytes) -> list:
        if frame[:1] == bytes([MG_COMMAND_START_MIC]):
            return self.on_start_mic()
        if frame[:1] == bytes([MG_COMMAND_STOP_MIC]):
            return self.on_stop_mic()
        return super().write(frame)

    def event(self, what: str, ms: int = 0, source: str = None) -> list:
        if what == "console_capture":
            return self.on_console_capture(source)
        if what == "time":
            self.now += ms
        elif what in ("press", "wake_word"):
            self.pressed_at = self.now
        elif what == "reconnect":
            self.proof = self.capturing = self.hello_done = self.nonces_taken = False
            self.seal_keys = None
            self.drop_partials()
            self.tx = self.rx = 0
        return []   # console_press, charger: never a press

    def mic_allowed(self) -> bool:
        return not self.release or (self.pressed_at is not None and self.now - self.pressed_at <= PRESS_WINDOW_MS)

    def mic_needs_proof(self) -> bool:
        """Waiting for this connection's proof; a release device that holds no token has none that can match."""
        return (self.holds_token and not self.proof) or (self.release and not self.holds_token)

    def on_start_mic(self) -> list:
        if self.mic_needs_proof():
            return [command_error(MG_COMMAND_START_MIC, PROOF_REQUIRED)]
        if not self.mic_allowed():
            return [command_error(MG_COMMAND_START_MIC, PRESS_REQUIRED)]
        self.capturing = True
        return [bytes([MG_COMMAND_CHANGE_DATA_TYPE, AUDIO_SBC]) + le16(16000)]

    def on_console_capture(self, source: str) -> list:
        """The console's answer goes to the console (console_answer); the client hears only of a capture that starts."""
        if self.mic_needs_proof():
            self.console_answer = "proof_required"
        elif source != "wired" and not self.mic_allowed():
            self.console_answer = "press_required"
        else:
            self.console_answer = "taken"
            self.capturing = True
            return [bytes([MG_COMMAND_CHANGE_DATA_TYPE, AUDIO_SBC]) + le16(16000)]
        return []

    def on_stop_mic(self) -> list:
        if not self.capturing:
            return []
        self.capturing = False
        return [bytes([MG_COMMAND_STOP_MIC])]


class UnsetTakesStartMic(MicGateDevice):
    """Takes start_mic on a release device that holds no token: must fail the mic gate."""

    def mic_needs_proof(self) -> bool:
        return self.holds_token and not self.proof


class MicGateRecorder(Recorder):
    def __init__(self, name, note, device_cls=MicGateDevice, **device):
        self.dev = device_cls(247, **device)
        self.mtu = 247
        self.ctr = 0
        self.inputs = session_seal_inputs()
        self.ex = {"name": name, "mtu": 247, "device": dict(device), "note": note, "steps": []}

    def write(self, frame: bytes, note: str = None):
        super().write(frame, note)
        self.ex["steps"][-1]["mic"] = self.dev.capturing

    def proof(self, n: int, note: str = None):
        super().proof(n, note)
        self.ex["steps"][-1]["mic"] = self.dev.capturing

    def event(self, what: str, note: str = None, **args):
        step = {"event": what, **args, "notify": [f.hex() for f in self.dev.event(what, **args)]}
        if what == "console_capture":
            step["console"] = self.dev.console_answer
        if note:
            step["note"] = note
        step["mic"] = self.dev.capturing
        self.ex["steps"].append(step)


def mic_press_exchange() -> dict:
    r = MicGateRecorder("start_mic after a press on a release build", "A release build takes a client's start_mic "
                        f"only within {PRESS_WINDOW_MS} ms after a press on the gadget.")
    r.write(start_mic(), "no press yet: press_required, nothing captured")
    r.event("press", "a press on the gadget's talk control")
    r.event("time", f"{PRESS_WINDOW_MS} ms later", ms=PRESS_WINDOW_MS)
    r.write(start_mic(), f"{PRESS_WINDOW_MS} ms after the press: taken, change_data_type (then audio)")
    r.write(bytes([MG_COMMAND_STOP_MIC]), "stop_mic: capture ends, stop_mic back")
    r.event("time", "1 ms later", ms=1)
    r.write(start_mic(), f"{PRESS_WINDOW_MS + 1} ms after the press: press_required")
    return r.ex


def mic_wake_word_exchange() -> dict:
    r = MicGateRecorder("a wake word counts, a console press doesn't", "A wake word the gadget hears itself is a press; "
                        "the console's press word is a console word and never one, nor is a charger plug-in.")
    r.event("console_press", "the console's press word")
    r.write(start_mic(), "press_required")
    r.event("charger", "a charger plugged in")
    r.write(start_mic(), "press_required")
    r.event("wake_word", "the gadget hears its wake word")
    r.write(start_mic(), "taken")
    return r.ex


def mic_proof_exchange() -> dict:
    r = MicGateRecorder("the token proof still comes first", "A press doesn't stand in for the token proof.",
                        proof=False)
    r.event("press", "a press on the gadget's talk control")
    r.write(start_mic(), "no proof on this link: proof_required")
    r.proof(2, "the token proof matches")
    r.write(start_mic(), "taken (the press was just now)")
    return r.ex


def mic_relay_exchange() -> dict:
    r = MicGateRecorder("a relay with a forwarded token proof", "A relay's own link, whose token proof matched "
                        "because the relay forwarded the real phone's answers, gets no mic without a press on the "
                        "gadget: not by itself, and not after a hello and mg_session_wake.", proof=False)
    r.proof(2, "the token proof matches: the relay forwarded the real phone's answers")
    r.write(start_mic(), "no press: press_required")
    r.write(hello(65536, 65536) + bytes([SH]), "a sealed hello: answered (the hello isn't authenticated; the relay "
                                               "can't read the sealed credentials)")
    r.write(bytes([MG_COMMAND_SESSION, WAKE]), "mg_session_wake: the gadget asks for a session (a task of its own), "
                                               "but a protocol command is never a press")
    r.write(start_mic(), "press_required")
    return r.ex


def mic_console_capture_exchange() -> dict:
    r = MicGateRecorder("capture started by a console word", "A console word that starts capture over a wireless link "
                        "(a console over the radio) follows start_mic's rule, even on a proven link; one on a wired "
                        "console is the owner's action.")
    r.event("console_capture", "a capture word on the wireless console, no press: press_required (on the console), "
            "nothing captured", source="wireless")
    r.event("console_capture", "the same word on the wired console: taken", source="wired")
    r.write(bytes([MG_COMMAND_STOP_MIC]), "stop_mic")
    r.event("press", "a press on the gadget's talk control")
    r.event("console_capture", "the wireless console's word just after it: taken", source="wireless")
    return r.ex


def mic_automation_exchange() -> dict:
    r = MicGateRecorder("an automation build", "Automation builds take start_mic with the token proof alone.",
                        release=False)
    r.write(start_mic(), "no press: taken")
    return r.ex


def mic_unset_exchange() -> dict:
    r = MicGateRecorder("a release build that isn't set up", "A release device that holds no token has no proof "
                        "that can match: it answers start_mic with proof_required, press or not.", holds_token=False,
                        proof=False)
    r.write(start_mic(), "start_mic: proof_required")
    r.event("press", "a press on the gadget's talk control")
    r.write(start_mic(), "start_mic after the press: still proof_required")
    return r.ex


def mic_unset_automation_exchange() -> dict:
    r = MicGateRecorder("an automation build that isn't set up", "Bench, automation and test builds that hold no "
                        "token take start_mic.", holds_token=False, proof=False, release=False)
    r.write(start_mic(), "start_mic: taken")
    return r.ex


def mic_gate_section() -> dict:
    return {
        "description": "The microphone gate (mgcommands.h, start_mic). Each step is a write (client -> device), a "
                       "token proof (proof, as in exchanges) or an event on the gadget (press: its talk control; "
                       "wake_word: a wake word it heard itself; console_press: the console's press word; charger: a "
                       "charger plugged in; console_capture: a console word that starts capture, source wireless "
                       "or wired, with the console's answer in console; time: its clock moves ms; reconnect: a new "
                       "link), with what the device notifies and whether it captures afterwards (mic). Each exchange "
                       "starts on a new connection whose token proof matched, unless its device flags have proof "
                       "false; release false is an automation build; holds_token false is a device that isn't set "
                       "up (a release one answers start_mic proof_required). The device captures SBC at 16 kHz and holds "
                       "the session's test_credentials.",
        "press_window_ms": PRESS_WINDOW_MS,
        "exchanges": [mic_press_exchange(), mic_wake_word_exchange(), mic_proof_exchange(), mic_relay_exchange(),
                      mic_console_capture_exchange(), mic_automation_exchange(), mic_unset_exchange(),
                      mic_unset_automation_exchange()],
    }


def mic_outcome(notify: list) -> str:
    """What one start_mic got: taken, an error code's name, or nothing."""
    if not notify:
        return ""
    f = bytes.fromhex(notify[0])
    if f[0] == MG_COMMAND_CHANGE_DATA_TYPE:
        return "taken"
    if f[0] == MG_COMMAND_ERROR and f[1] == MG_COMMAND_START_MIC:
        return {PROOF_REQUIRED: "proof_required", PRESS_REQUIRED: "press_required"}.get(f[3] | f[4] << 8, "error")
    return "other"


EXPECTED_MIC_GATE = [["press_required", "taken", "press_required"], ["press_required", "press_required", "taken"],
                     ["proof_required", "taken"], ["press_required", "press_required"],
                     ["press_required", "taken", "taken"], ["taken"], ["proof_required", "proof_required"], ["taken"]]


def check_mic_gate(section: dict, device_cls=MicGateDevice, expected=None) -> list:
    """Replays a microphone-gate section through device_cls."""
    bad, outcomes = [], []
    k = session_seal_inputs()[0]
    for ex in section["exchanges"]:
        dev = device_cls(ex["mtu"], **ex["device"])
        outcomes.append([])
        for i, step in enumerate(ex["steps"]):
            if "write" in step:
                frame = bytes.fromhex(step["write"])
                got = dev.write(frame)
                if frame[:1] == bytes([MG_COMMAND_START_MIC]):
                    outcomes[-1].append(mic_outcome(step["notify"]))
            elif step["event"] == "proof":
                got = dev.new_proof(k, bytes.fromhex(step["client_nonce"]), bytes.fromhex(step["device_nonce"]))
            else:
                got = dev.event(step["event"], **{a: step[a] for a in ("ms", "source") if a in step})
                if step["event"] == "console_capture":
                    outcomes[-1].append(step["console"])
                    if dev.console_answer != step["console"]:
                        bad.append(f"{ex['name']} step {i}: console")
            if [f.hex() for f in got] != step["notify"] or dev.capturing != step["mic"]:
                bad.append(f"{ex['name']} step {i}")
    if outcomes != (EXPECTED_MIC_GATE if expected is None else expected):
        bad.append(f"mic gate outcomes {outcomes}")
    return bad


# ---- streams to the device ----------------------------------------------------------------------------------------
# A device that holds a token takes start_streaming (stream_audio, stream_video) only after this connection's token
# proof matched, or on an automation build on a connection that turned its bench override on. Bluetooth firmware
# updates are mg_update_ref.py's (mg-update-v2.json); the SMP request encoding below is shared with it.

MG_COMMAND_STREAM_AUDIO, MG_COMMAND_STREAM_VIDEO = 29, 0x84
START_STREAMING, BUFFER_UPDATE = 2, 5
STREAM_BUFFER = 8192
SMP_READ, SMP_WRITE = 0, 2
SMP_GROUP_OS, SMP_GROUP_IMAGE = 0, 1


def cbor(v) -> bytes:
    """The CBOR an SMP request carries: maps with text keys, unsigned integers, byte strings and booleans."""
    def head(major, n):
        if n < 24:
            return bytes([major << 5 | n])
        size = next(s for s in (1, 2, 4, 8) if n < 1 << 8 * s)
        return bytes([major << 5 | {1: 24, 2: 25, 4: 26, 8: 27}[size]]) + n.to_bytes(size, "big")
    if isinstance(v, bool):
        return b"\xf5" if v else b"\xf4"
    if isinstance(v, int):
        return head(0, v)
    if isinstance(v, bytes):
        return head(2, len(v)) + v
    if isinstance(v, str):
        return head(3, len(v.encode())) + v.encode()
    return head(5, len(v)) + b"".join(cbor(k) + cbor(x) for k, x in v.items())


def smp_request(op: int, group: int, command: int, body: dict, seq: int = 0) -> bytes:
    """SMP v1: op, flags, length (BE), group (BE), sequence, command id, then the CBOR body."""
    payload = cbor(body)
    return bytes([op, 0]) + len(payload).to_bytes(2, "big") + group.to_bytes(2, "big") + bytes([seq, command]) + payload



class GatedDevice(MicGateDevice):
    """MicGateDevice with audio and video streams to it. A device flag button_recovery runs it in a recovery the
    user's button entered; the events key_hold and window_closes open and close its local window, and bench_override
    turns the bench override on for this connection (automation builds only); a new link closes both. It answers a
    stream it takes with buffer_update."""

    def __init__(self, mtu: int, button_recovery=False, **flags):
        super().__init__(mtu, **flags)
        self.button_recovery = button_recovery
        self.window = self.bench = False

    def write(self, frame: bytes) -> list:
        if frame[:1] in (bytes([MG_COMMAND_STREAM_AUDIO]), bytes([MG_COMMAND_STREAM_VIDEO])) and \
                frame[1:2] == bytes([START_STREAMING]):
            if self.holds_token and not self.proof and not self.bench:
                return [command_error(frame[0], PROOF_REQUIRED)]
            return [bytes([frame[0], BUFFER_UPDATE]) + le32(STREAM_BUFFER) + le32(0)]
        return super().write(frame)

    def event(self, what: str, ms: int = 0, source: str = None) -> list:
        if what in ("key_hold", "window_closes", "reconnect"):
            self.window = what == "key_hold"
        if what in ("bench_override", "reconnect"):
            self.bench = what == "bench_override" and not self.release
        return super().event(what, ms, source)


class GatedRecorder(MicGateRecorder):
    def __init__(self, name, note, **device):
        super().__init__(name, note, device_cls=GatedDevice, **device)


VIDEO_H264 = 24


def stream_start(command: int) -> bytes:
    """start_streaming: SBC playback, or 240x240 H.264 video."""
    if command == MG_COMMAND_STREAM_AUDIO:
        return bytes([command, START_STREAMING, 1, 1, AUDIO_SBC])
    return bytes([command, START_STREAMING, 3, 1, VIDEO_H264, 2]) + le16(240) + bytes([3]) + le16(240)


def streams_set_up_exchange() -> dict:
    r = GatedRecorder("streams to a set-up device", "A device that holds a token starts audio playback or a video "
                      "stream only after this connection's token proof matched.", proof=False)
    r.write(stream_start(MG_COMMAND_STREAM_AUDIO), "audio start_streaming before a proof: proof_required")
    r.write(stream_start(MG_COMMAND_STREAM_VIDEO), "video start_streaming before a proof: proof_required")
    r.proof(2, "the token proof matches")
    r.write(stream_start(MG_COMMAND_STREAM_AUDIO), "audio: taken, buffer_update")
    r.write(stream_start(MG_COMMAND_STREAM_VIDEO), "video: taken, buffer_update")
    return r.ex


def streams_bench_override_exchange() -> dict:
    r = GatedRecorder("streams and the bench override on an automation build", "An automation build's bench override "
                      "(per connection, off at boot) admits streams without the token proof.", proof=False,
                      release=False)
    r.write(stream_start(MG_COMMAND_STREAM_AUDIO), "no proof, override off: proof_required")
    r.event("bench_override", "the bench override turned on for this connection")
    r.write(stream_start(MG_COMMAND_STREAM_AUDIO), "audio, no proof: taken")
    r.write(stream_start(MG_COMMAND_STREAM_VIDEO), "video, no proof: taken")
    return r.ex


def streams_release_override_exchange() -> dict:
    r = GatedRecorder("streams and the bench override on a release build", "A release build has no bench override: "
                      "streams still need the token proof.", proof=False)
    r.event("bench_override", "the bench override word")
    r.write(stream_start(MG_COMMAND_STREAM_AUDIO), "no proof: proof_required")
    return r.ex


def streams_unset_exchange() -> dict:
    r = GatedRecorder("streams to a device that isn't set up", "A device that holds no token takes streams as before.",
                      holds_token=False, proof=False)
    r.write(stream_start(MG_COMMAND_STREAM_AUDIO), "audio: taken")
    r.write(stream_start(MG_COMMAND_STREAM_VIDEO), "video: taken")
    return r.ex


def stream_update_section() -> dict:
    return {
        "description": "Streams to the device (mgcommands.h, Token proof, start_streaming). Steps as in mic_gate; "
                       "event bench_override turns an automation build's bench override on for the connection; "
                       "holds_token false is a device that isn't set up. A stream the device takes is answered with "
                       "buffer_update. Bluetooth firmware updates are in mg-update-v2.json.",
        "exchanges": [streams_set_up_exchange(), streams_bench_override_exchange(), streams_release_override_exchange(),
                      streams_unset_exchange()],
    }


def stream_update_outcomes(ex: dict) -> list:
    out = []
    for s in ex["steps"]:
        if "write" in s and bytes.fromhex(s["write"])[:1] in (bytes([MG_COMMAND_STREAM_AUDIO]),
                                                                  bytes([MG_COMMAND_STREAM_VIDEO])):
            f = bytes.fromhex(s["notify"][0])
            out.append("taken" if f[1] == BUFFER_UPDATE else "proof_required" if f[3] == PROOF_REQUIRED else "error")
    return out


EXPECTED_STREAM_UPDATE = [["proof_required", "proof_required", "taken", "taken"],
                          ["proof_required", "taken", "taken"], ["proof_required"], ["taken", "taken"]]


def check_stream_update(section: dict) -> list:
    """Replays the stream section through GatedDevice."""
    bad, outcomes = [], []
    k = session_seal_inputs()[0]
    for ex in section["exchanges"]:
        dev = GatedDevice(ex["mtu"], **ex["device"])
        for i, step in enumerate(ex["steps"]):
            if "write" in step:
                got = dev.write(bytes.fromhex(step["write"]))
            elif step["event"] == "proof":
                got = dev.new_proof(k, bytes.fromhex(step["client_nonce"]), bytes.fromhex(step["device_nonce"]))
            else:
                got = dev.event(step["event"], **{a: step[a] for a in ("ms", "source") if a in step})
            if [f.hex() for f in got] != step["notify"]:
                bad.append(f"{ex['name']} step {i}")
        outcomes.append(stream_update_outcomes(ex))
    if outcomes != EXPECTED_STREAM_UPDATE:
        bad.append(f"stream and update outcomes {outcomes}")
    return bad


# ---- settings from a central without the proof -------------------------------------------------------------------
# On a device that holds a token, a change to a persisted setting from a central without this connection's token
# proof is skipped (the write's other pairs still apply), then proof_required once. A device that holds no token takes
# it, writing flash at most once per FLASH_INTERVAL_MS or when setup completes. Per-connection settings are never
# refused for this.

MG_COMMAND_GET_SETTINGS, MG_COMMAND_SET_SETTINGS = 63, 65
MG_COMMAND_DEVICE_ACTION = 30
SET_UNIX_TIME, GET_UNIX_TIME, REPORT_UNIX_TIME = 18, 19, 20
INT64_MIN = -(1 << 63)
CLOCK_FLOOR_MS = 1704067200000   # 2024-01-01T00:00:00Z: a device may refuse an earlier set_unix_time
CLOCK_CEILING_MS = 253402300799999   # 9999-12-31T23:59:59.999Z: every device refuses a later set_unix_time
CLOCK_TEST_MS = 1800000000000    # 2027-01-15T08:00:00Z
FLASH_INTERVAL_MS = 10 * 60 * 1000
# The settings the reference device lists: parameter -> (value bytes, persisted, default). client_app (19) is two
# length-prefixed strings.
SETTINGS = {2: (1, False, 0), 3: (1, False, 0), 7: (1, True, AUDIO_SBC), 10: (1, True, 50), 11: (1, True, 100),
            13: (2, True, 16000), 16: (1, False, 1)}
CLIENT_APP = 19
CONTENT_SETTINGS = {18, CLIENT_APP}   # client_name, client_app: the owner's phone; the rest is status
AUDIO_LC3 = 19


class SettingsDevice(GatedDevice):
    """GatedDevice with get_settings and set_settings over SETTINGS and client_app. The event setup_complete is the
    end of Link setup on this device (it holds a token from then on). Each step records how many times the device has
    written its persisted settings to flash (flash)."""

    def __init__(self, mtu: int, clock_floor=False, local_window=True, **flags):
        super().__init__(mtu, **flags)
        self.clock_floor = clock_floor   # refuses a time before CLOCK_FLOOR_MS (a device may)
        self.local_window = local_window  # a key hold opens a local window (a device may have none)
        self.values = {p: d for p, (_, _, d) in SETTINGS.items()}
        self.client_app = b"\x00\x00"
        self.flash_writes, self.flashed_at, self.dirty = 0, None, False
        self.clock_ms, self.clock_set_at, self.utc_offset, self.clock_flags = None, 0, 0, 0

    def write(self, frame: bytes) -> list:
        if frame[:1] == bytes([MG_COMMAND_SET_SETTINGS]):
            return self.on_set_settings(frame[1:])
        if frame[:2] == bytes([MG_COMMAND_DEVICE_ACTION, SET_UNIX_TIME]):
            return self.on_set_unix_time(frame[2:])
        if frame == bytes([MG_COMMAND_DEVICE_ACTION, GET_UNIX_TIME]):
            if self.content_gated():   # the owner's local time and offset
                return [command_error(MG_COMMAND_DEVICE_ACTION, PROOF_REQUIRED)]
            ms = INT64_MIN if self.clock_ms is None else self.clock_ms + (self.now - self.clock_set_at)
            return [bytes([MG_COMMAND_DEVICE_ACTION, REPORT_UNIX_TIME]) + ms.to_bytes(8, "little", signed=True)
                    + self.utc_offset.to_bytes(2, "little", signed=True) + bytes([self.clock_flags])]
        if frame[:1] == bytes([MG_COMMAND_GET_SETTINGS]):
            asked = list(frame[1:])
            shown = [q for q in asked if not (self.content_gated() and q in CONTENT_SETTINGS)]
            out = [bytes([MG_COMMAND_GET_SETTINGS]) + b"".join(self.encode(q) for q in shown)] if shown else []
            if len(shown) < len(asked):
                out.append(command_error(MG_COMMAND_GET_SETTINGS, PROOF_REQUIRED))   # content skipped, once
            return out
        return super().write(frame)

    def content_gated(self) -> bool:
        """A release build of a device that holds a token answers the owner's content only to a connection whose
        token proof matched, or in its local window."""
        return self.release and self.holds_token and not self.proof and not self.window

    def encode(self, param: int) -> bytes:
        if param == CLIENT_APP:
            return bytes([param]) + self.client_app
        return bytes([param]) + self.values[param].to_bytes(SETTINGS[param][0], "little")

    def event(self, what: str, ms: int = 0, source: str = None) -> list:
        out = super().event(what, ms, source)
        if what == "key_hold" and not self.local_window:
            self.window = False   # no local window: the owner's content answers only the proof
        if what == "reconnect":
            self.values.update({p: d for p, (_, persisted, d) in SETTINGS.items() if not persisted})
            self.client_app = b"\x00\x00"
        elif what == "setup_complete":
            self.holds_token = True
            if self.dirty:
                self.save()
        elif what == "time" and self.dirty and self.now - self.flashed_at >= FLASH_INTERVAL_MS:
            self.save()
        return out

    def on_set_unix_time(self, p: bytes) -> list:
        """The time is taken from any central, proof or not, and reaches no flash (it lives across disconnects)."""
        if len(p) not in (8, 10, 11):
            return [command_error(MG_COMMAND_DEVICE_ACTION, INVALID_LENGTH)]
        ms = int.from_bytes(p[:8], "little", signed=True)
        if ms == INT64_MIN or ms > CLOCK_CEILING_MS or (self.clock_floor and ms < CLOCK_FLOOR_MS):
            return [command_error(MG_COMMAND_DEVICE_ACTION, INVALID_VALUE)]
        self.clock_ms, self.clock_set_at = ms, self.now
        if len(p) >= 10:
            self.utc_offset = int.from_bytes(p[8:10], "little", signed=True)
        if len(p) == 11:
            self.clock_flags = p[10]
        return []

    def save(self):
        self.flash_writes += 1
        self.flashed_at, self.dirty = self.now, False

    def on_set_settings(self, p: bytes) -> list:
        out, refused, changed, i = [], False, False, 0
        while i < len(p):
            param = p[i]
            if param == CLIENT_APP:
                end = i + 1
                for _ in range(2):
                    if end >= len(p):
                        return out + [command_error(MG_COMMAND_SET_SETTINGS, INVALID_LENGTH)]
                    end += 1 + p[end]
                if end > len(p):
                    return out + [command_error(MG_COMMAND_SET_SETTINGS, INVALID_LENGTH)]
                self.client_app, i = p[i + 1:end], end
                continue
            if param not in SETTINGS:
                out.append(command_error(MG_COMMAND_SET_SETTINGS, UNSUPPORTED))   # stops parsing
                break
            width, persisted, _ = SETTINGS[param]
            if i + 1 + width > len(p):
                return out + [command_error(MG_COMMAND_SET_SETTINGS, INVALID_LENGTH)]
            value, i = int.from_bytes(p[i + 1:i + 1 + width], "little"), i + 1 + width
            if not persisted:
                was = self.values[param]
                self.values[param] = value
                if param == 16 and value != was:   # link_mode: the effective mode changed
                    out.append(bytes([MG_COMMAND_GET_SETTINGS]) + self.encode(16))
                continue
            if self.holds_token and not self.proof:
                refused = True   # skipped: nothing changes
                continue
            changed |= self.values[param] != value
            self.values[param] = value
        if changed:   # one flash write per write that changed a persisted setting
            if self.holds_token or self.flashed_at is None or self.now - self.flashed_at >= FLASH_INTERVAL_MS:
                self.save()
            else:
                self.dirty = True   # written once the interval has passed, or at setup
        if refused:
            out.append(command_error(MG_COMMAND_SET_SETTINGS, PROOF_REQUIRED))
        return out


class SettingsRecorder(GatedRecorder):
    def __init__(self, name, note, **device):
        MicGateRecorder.__init__(self, name, note, device_cls=SettingsDevice, **device)

    def write(self, frame: bytes, note: str = None):
        super().write(frame, note)
        self.ex["steps"][-1]["flash"] = self.dev.flash_writes

    def proof(self, n: int, note: str = None):
        super().proof(n, note)
        self.ex["steps"][-1]["flash"] = self.dev.flash_writes

    def event(self, what: str, note: str = None, **args):
        super().event(what, note, **args)
        self.ex["steps"][-1]["flash"] = self.dev.flash_writes


def set_settings(*pairs) -> bytes:
    return bytes([MG_COMMAND_SET_SETTINGS]) + b"".join(
        bytes([p]) + (v if isinstance(v, bytes) else v.to_bytes(SETTINGS[p][0], "little")) for p, v in pairs)


def get_settings(*params) -> bytes:
    return bytes([MG_COMMAND_GET_SETTINGS] + list(params))


def settings_set_up_exchange() -> dict:
    r = SettingsRecorder("saved settings on a set-up device", "Before this connection's token proof, a change to a "
                         "persisted setting is skipped and the write ends with proof_required; the write's "
                         "per-connection settings still apply. After the proof the change is taken.", proof=False)
    r.write(set_settings((2, 1), (7, AUDIO_LC3), (13, 24000)), "push_to_talk_enabled 1, audio_codec LC3, "
            "capture_sample_rate 24000 (as the apps send at connect), no proof: push-to-talk on, the rest skipped, "
            "proof_required")
    r.write(get_settings(2, 7, 13), "get_settings: push-to-talk 1, codec still SBC, rate still 16000")
    r.write(set_settings((10, 20)), "speaker_volume 20, no proof: proof_required, nothing changes")
    r.proof(2, "the token proof matches")
    r.write(set_settings((7, AUDIO_LC3), (13, 24000)), "audio_codec LC3 and 24000 after the proof: taken, written")
    r.write(get_settings(7, 13), "get_settings: LC3, 24000")
    return r.ex


def settings_per_connection_exchange() -> dict:
    r = SettingsRecorder("per-connection settings without the proof", "Per-connection settings and the time are "
                         "taken from a central without the token proof; they reach no flash. A test build (release "
                         "false): it also reads them back unproven, which a release build doesn't (see the next "
                         "exchanges).", proof=False, release=False)
    r.write(set_settings((3, 1), (16, 2), (CLIENT_APP, b"\x08Muse app\x09" + b"16.0.0.86")),
            "haptics_enabled 1, link_mode active, client_app: taken (link_mode reports the new effective mode)")
    r.write(get_settings(3, 16, CLIENT_APP), "get_settings: the values written")
    r.write(set_unix_time(CLOCK_TEST_MS, -480, 0),
            "set_unix_time 1800000000000 ms (2027-01-15 08:00:00 UTC), offset -480, 12-hour, no proof: taken "
            "(no reply), no flash")
    r.write(bytes([MG_COMMAND_DEVICE_ACTION, GET_UNIX_TIME]), "get_unix_time: the time set, offset -480, 12-hour")
    return r.ex


def set_unix_time(ms: int, offset: int, flags: int) -> bytes:
    return bytes([MG_COMMAND_DEVICE_ACTION, SET_UNIX_TIME]) + ms.to_bytes(8, "little", signed=True) \
        + offset.to_bytes(2, "little", signed=True) + bytes([flags])


def settings_clock_floor_exchange() -> dict:
    r = SettingsRecorder("a time before the clock's floor", "A device may refuse a set_unix_time before "
                         "2024-01-01T00:00:00Z (the floor that tells a set clock from an unset one) with "
                         "invalid_value, keeping its clock as it was; this one does (clock_floor). Every later time "
                         "up to 9999-12-31T23:59:59Z is taken. A test build, so its reads answer unproven.",
                         proof=False, clock_floor=True,
                         release=False)
    r.write(set_unix_time(1000000000000, -480, 0), "set_unix_time 1000000000000 ms (2001-09-09 01:46:40 UTC): "
            "invalid_value, the clock unchanged")
    r.write(bytes([MG_COMMAND_DEVICE_ACTION, GET_UNIX_TIME]), "get_unix_time: still never synced (INT64_MIN)")
    r.write(set_unix_time(CLOCK_FLOOR_MS, -480, 0), "set_unix_time at the floor itself (1704067200000 ms): taken")
    r.write(set_unix_time(CLOCK_TEST_MS, -480, 0), "set_unix_time 1800000000000 ms: taken")
    r.write(bytes([MG_COMMAND_DEVICE_ACTION, GET_UNIX_TIME]), "get_unix_time: the time set")
    return r.ex


def settings_clock_ceiling_exchange() -> dict:
    r = SettingsRecorder("a time past year 9999", "Every device takes a set_unix_time up to 9999-12-31T23:59:59Z and "
                         "answers a later one invalid_value, keeping its clock as it was. A test build, so its reads "
                         "answer unproven.", proof=False, release=False)
    r.write(set_unix_time(253402300800000, -480, 0), "set_unix_time 253402300800000 ms (10000-01-01T00:00:00Z): "
            "invalid_value, the clock unchanged")
    r.write(bytes([MG_COMMAND_DEVICE_ACTION, GET_UNIX_TIME]), "get_unix_time: still never synced (INT64_MIN)")
    r.write(set_unix_time(253402300799000, -480, 0), "set_unix_time 253402300799000 ms (9999-12-31T23:59:59Z): taken")
    r.write(bytes([MG_COMMAND_DEVICE_ACTION, GET_UNIX_TIME]), "get_unix_time: the time set")
    return r.ex


def settings_unset_exchange() -> dict:
    r = SettingsRecorder("saved settings on a device that isn't set up", "A device that holds no token takes "
                         "persisted settings from any central, writing flash at most once per 10 min or when setup "
                         "completes.", holds_token=False, proof=False)
    r.write(set_settings((10, 30)), "speaker_volume 30: taken, written (flash 1)")
    r.event("time", "a minute later", ms=60000)
    r.write(set_settings((10, 40)), "speaker_volume 40: taken, not written yet")
    r.write(get_settings(10), "get_settings: 40")
    r.event("time", "10 min after the last write: written (flash 2)", ms=FLASH_INTERVAL_MS - 60000)
    r.write(set_settings((11, 60)), "led_brightness 60: taken, not written yet")
    r.event("setup_complete", "Link setup completes: written (flash 3)")
    r.write(set_settings((11, 70)), "led_brightness 70 now that it holds a token, no proof on this connection: "
            "proof_required")
    return r.ex


def settings_content_exchange() -> dict:
    r = SettingsRecorder("the owner's content without the proof, release build", "On a release build of a set-up "
                         "device, reads of the owner's content wait for this connection's token proof: get_settings "
                         "answers its status fields and skips client_name and client_app (proof_required once), and "
                         "get_unix_time is proof_required. Writes are unchanged.", proof=False)
    r.write(set_settings((3, 1), (CLIENT_APP, b"\x08Muse app\x09" + b"16.0.0.86")),
            "haptics_enabled 1, client_app: taken (a write, never refused for this)")
    r.write(get_settings(3, 16, CLIENT_APP), "get_settings haptics_enabled, link_mode, client_app: the two status "
            "fields answered, client_app skipped, proof_required")
    r.write(get_settings(CLIENT_APP), "get_settings client_app alone: proof_required only")
    r.write(set_unix_time(CLOCK_TEST_MS, -480, 0), "set_unix_time: taken (no reply)")
    r.write(bytes([MG_COMMAND_DEVICE_ACTION, GET_UNIX_TIME]), "get_unix_time: proof_required (local time and offset)")
    r.proof(2, "the token proof matches")
    r.write(get_settings(3, 16, CLIENT_APP), "get_settings: all three answered")
    r.write(bytes([MG_COMMAND_DEVICE_ACTION, GET_UNIX_TIME]), "get_unix_time: the time, offset -480, 12-hour")
    return r.ex


def settings_content_window_exchange() -> dict:
    r = SettingsRecorder("the owner's content in the local window, release build", "A key hold's local window "
                         "opens the owner's content reads without a proof, until it closes. Only for a device "
                         "with a local window (local_window); one without skips this exchange.", proof=False,
                         local_window=True)
    r.write(set_settings((CLIENT_APP, b"\x08Muse app\x09" + b"16.0.0.86")), "client_app: taken")
    r.event("key_hold", "a key hold opens the local window")
    r.write(get_settings(CLIENT_APP), "get_settings client_app: answered")
    r.event("window_closes", "the window closes")
    r.write(get_settings(CLIENT_APP), "get_settings client_app: proof_required")
    return r.ex


def settings_section() -> dict:
    return {
        "description": "Settings from a central without the token proof (mgcommands.h, set_settings). Steps as in "
                       "streams_and_updates; each step also records flash, how many times the device has written "
                       "its persisted settings so far; the event setup_complete ends Link setup on a device that "
                       "held no token. The device lists push_to_talk_enabled, haptics_enabled and link_mode (per "
                       "connection), audio_codec, speaker_volume, led_brightness and capture_sample_rate "
                       "(persisted) and client_app, and takes the clock's device actions (set_unix_time, "
                       "get_unix_time); the time is never refused for want of the proof. The device flag "
                       "clock_floor is a device that refuses a time before 2024-01-01T00:00:00Z (a device may); "
                       "every device refuses one after 9999-12-31T23:59:59Z. On "
                       "a release build (the default) a device that holds a token answers the owner's content "
                       "(client_name, client_app, the time) only after this connection's proof or in its local "
                       "window; release false is a test build, which reads them back unproven. An exchange whose "
                       "device flags say local_window true needs a device whose key hold opens a local window; a "
                       "device without one (it answers the owner's content only to the proof) skips it.",
        "flash_interval_ms": FLASH_INTERVAL_MS,
        "exchanges": [settings_set_up_exchange(), settings_per_connection_exchange(), settings_unset_exchange(),
                      settings_clock_floor_exchange(), settings_clock_ceiling_exchange(), settings_content_exchange(),
                      settings_content_window_exchange()],
    }


def settings_outcomes(ex: dict) -> list:
    out = []
    for s in ex["steps"]:
        if "write" in s and bytes.fromhex(s["write"])[:1] == bytes([MG_COMMAND_SET_SETTINGS]):
            errs = [bytes.fromhex(n) for n in s["notify"] if bytes.fromhex(n)[0] == MG_COMMAND_ERROR]
            out.append("proof_required" if any(e[3] == PROOF_REQUIRED for e in errs) else "taken")
    return out


EXPECTED_SETTINGS = [["proof_required", "proof_required", "taken"], ["taken"],
                     ["taken", "taken", "taken", "proof_required"], [], [], ["taken"], ["taken"]]
EXPECTED_SETTINGS_FLASH = [1, 0, 3, 0, 0, 0, 0]


class LeaksClientApp(SettingsDevice):
    """Answers client_app to a central without the proof on a release build: must fail the settings section."""

    def write(self, frame: bytes) -> list:
        if frame == bytes([MG_COMMAND_GET_SETTINGS, CLIENT_APP]):
            return [bytes([MG_COMMAND_GET_SETTINGS]) + self.encode(CLIENT_APP)]
        return super().write(frame)


def settings_mismatches(section: dict, device_cls) -> int:
    k, n = session_seal_inputs()[0], 0
    for ex in section["exchanges"]:
        dev = device_cls(ex["mtu"], **ex["device"])
        for step in ex["steps"]:
            if "write" in step:
                got = dev.write(bytes.fromhex(step["write"]))
            elif step["event"] == "proof":
                got = dev.new_proof(k, bytes.fromhex(step["client_nonce"]), bytes.fromhex(step["device_nonce"]))
            else:
                got = dev.event(step["event"], **{a: step[a] for a in ("ms", "source") if a in step})
            n += [f.hex() for f in got] != step["notify"]
    return n


def check_settings(section: dict) -> list:
    """Replays the settings section through SettingsDevice."""
    bad, outcomes, flash = [], [], []
    if not settings_mismatches(section, LeaksClientApp):
        bad.append("LeaksClientApp passes the settings section")
    k = session_seal_inputs()[0]
    for ex in section["exchanges"]:
        dev = SettingsDevice(ex["mtu"], **ex["device"])
        for i, step in enumerate(ex["steps"]):
            if "write" in step:
                got = dev.write(bytes.fromhex(step["write"]))
            elif step["event"] == "proof":
                got = dev.new_proof(k, bytes.fromhex(step["client_nonce"]), bytes.fromhex(step["device_nonce"]))
            else:
                got = dev.event(step["event"], **{a: step[a] for a in ("ms", "source") if a in step})
            if [f.hex() for f in got] != step["notify"] or dev.flash_writes != step["flash"]:
                bad.append(f"{ex['name']} step {i}")
        outcomes.append(settings_outcomes(ex))
        flash.append(ex["steps"][-1]["flash"])
    if outcomes != EXPECTED_SETTINGS or flash != EXPECTED_SETTINGS_FLASH:
        bad.append(f"settings outcomes {outcomes}, flash {flash}")
    return bad


# ---- more of the protocol behind the proof ------------------------------------------------------------------------
# On a release build of a device that holds a token, ui_input hid_key, a legacy skill invoke and haptic_play wait for
# this connection's token proof (proof_required before it); haptic_stop, skill list_request and cancel, and
# assistant_state stay open. A device that doesn't implement one answers unsupported first. The re-setup prompt (Link
# setup on a set-up device) shows at most once per RESETUP_PROMPT_MS per connection. Automation builds are exempt.

MG_COMMAND_ASSISTANT_STATE = 0x83
MG_COMMAND_UI_INPUT, UI_INPUT_HID_KEY = 0x86, 5
MG_COMMAND_SKILL = 0x88
HAPTIC_PLAY, HAPTIC_STOP = 15, 17
HAPTIC_PLAYING, HAPTIC_STOPPED, HAPTIC_REFUSED = 0, 2, 4
HAPTIC_REASON_NONE, HAPTIC_REASON_DISABLED, HAPTIC_REASON_REPLACED = 0, 1, 7
HAPTICS_ENABLED = 3   # mg_setting_parameter_haptics_enabled: per connection, 0 until the client writes it
HAPTIC_KIND_PRESET, HAPTIC_PRESET_TAP = 1, 1
RESETUP_PROMPT_MS = 10000
SETUP_MODE_MS = 300000   # a talk-key-only gadget's setup mode lasts at most 5 min after the boot that entered it
UNSUPPORTED = 1


def skills_ref():
    """mg_skills_ref.py (next to this file): the skills engine behind mg_command_skill."""
    mod = sys.modules.get("mg_skills_ref")
    if mod is None:
        import importlib.util
        spec = importlib.util.spec_from_file_location("mg_skills_ref", Path(__file__).with_name("mg_skills_ref.py"))
        mod = importlib.util.module_from_spec(spec)
        keep, sys.dont_write_bytecode = sys.dont_write_bytecode, True
        try:
            spec.loader.exec_module(mod)
        finally:
            sys.dont_write_bytecode = keep
        sys.modules["mg_skills_ref"] = mod
    return mod


def update_ref():
    """mg_update_ref.py (next to this file): the Bluetooth update gate, for where the update prompt meets the rest."""
    mod = sys.modules.get("mg_update_ref")
    if mod is None:
        import importlib.util
        spec = importlib.util.spec_from_file_location("mg_update_ref", Path(__file__).with_name("mg_update_ref.py"))
        mod = importlib.util.module_from_spec(spec)
        keep, sys.dont_write_bytecode = sys.dont_write_bytecode, True
        try:
            spec.loader.exec_module(mod)
        finally:
            sys.dont_write_bytecode = keep
        sys.modules["mg_update_ref"] = mod
    return mod


def hid_key(usage: int) -> bytes:
    return bytes([MG_COMMAND_UI_INPUT, UI_INPUT_HID_KEY]) + le16(usage)


def haptic_play(pattern_id: int) -> bytes:
    """A tap preset, once, at the device's own strength."""
    return bytes([MG_COMMAND_DEVICE_ACTION, HAPTIC_PLAY, 0, 0, pattern_id, 0, 0, 1, HAPTIC_KIND_PRESET,
                  HAPTIC_PRESET_TAP])


def haptic_stop() -> bytes:
    return bytes([MG_COMMAND_DEVICE_ACTION, HAPTIC_STOP, 0])


class ProofGateDevice(GatedDevice):
    """GatedDevice with ui_input hid_key (its HID host connected), mg_skills_ref's skills engine (TEST_SKILLS, its
    payloads and manifest) behind mg_command_skill, haptic_play and haptic_stop (a pattern plays until stopped here;
    haptic_play is refused, disabled, until the client writes haptics_enabled 1 with set_settings on this connection),
    assistant_state and Link setup's re-setup prompt (event resetup_request: a pairing_client_hello on this device;
    the setup answer is confirm_required, the prompt shown or taken over, error_operation_in_progress or
    error_pairing_unavailable; event dismiss: the back gesture; event press while the prompt is up: the confirm key
    or a tap on the Confirm disc, or with key talk the talk key, which only dismisses a re-setup prompt on a set-up
    release device outside a talk-key-only device's setup mode). Device flags hid, skills and haptics false are a
    device that doesn't implement that part."""

    def __init__(self, mtu: int, hid=True, skills=True, haptics=True, dismiss_drops_link=False, talk_key_only=False,
                 bad_hello_encrypted=False, bad_hello_drops_link=False, **flags):
        super().__init__(mtu, **flags)
        # A takeover hello that fails validation: refused encrypted under the session still current (a device that
        # answers before wiping it) instead of in plaintext; the link dropped after the refusal.
        self.bad_hello_encrypted, self.bad_hello_drops_link = bad_hello_encrypted, bad_hello_drops_link
        self.setup_encrypted = False   # the last error answer went out encrypted
        self.hid, self.has_skills, self.haptics = hid, skills, haptics
        self.dismiss_drops_link = dismiss_drops_link   # a dismiss ends the setup link (the board's choice)
        self.talk_key_only = talk_key_only             # its only key is its talk key
        self.setup_mode_at = None                      # when it started in setup mode (its button held at boot)
        self.updates = None                            # mg_update_ref's update gate, once an SMP request comes
        sk = skills_ref()
        self.skills = sk.Device(sk.TEST_SKILLS, sk.TEST_MANIFEST, sk.TEST_PAYLOADS, mtu) if skills else None
        self.hid_keys = []        # usages the HID pressed and released
        self.haptics_enabled = 0  # per connection
        self.pattern = None       # the client's haptic pattern playing (its id)
        self.shown = None         # the assistant_state shown
        self.hellos = {}           # pairing_client_hellos so far, by connection (conn: 1, 2, ...)
        self.prompt = None         # (conn, hello) the re-setup prompt on screen is bound to (None: no prompt up)
        self.confirmed = None      # (conn, hello) the last press confirmed
        self.dismissed_at = {}     # when the user last dismissed a prompt, by the prompt's connection
        self.setup_conn = None     # the connection whose confirmed setup still runs (to its provision_v2 or end)
        self.setup_answer = None
        self.prompt_change = None  # what the last hello did to the prompt: shown, taken_over or none

    def gated(self) -> bool:
        return self.release and self.holds_token and not self.proof

    def write(self, frame: bytes) -> list:
        cmd, sub = frame[0], frame[1:2]
        if cmd == MG_COMMAND_UI_INPUT and sub == bytes([UI_INPUT_HID_KEY]):
            if not self.hid:
                return [command_error(cmd, UNSUPPORTED)]
            if self.gated():
                return [command_error(cmd, PROOF_REQUIRED)]
            if len(frame) != 4:
                return [command_error(cmd, INVALID_LENGTH)]
            self.hid_keys.append(int.from_bytes(frame[2:4], "little"))
            return []
        if cmd == MG_COMMAND_SKILL:
            if not self.has_skills:
                return [command_error(cmd, UNSUPPORTED)]
            if sub == bytes([skills_ref().INVOKE]) and len(frame) >= 5 and frame[3:5] == b"\x00\x00" and self.gated():
                return [command_error(cmd, PROOF_REQUIRED)]   # refused: its later chunks find nothing received
            return self.skills.write(frame)
        if cmd == MG_COMMAND_DEVICE_ACTION and sub in (bytes([HAPTIC_PLAY]), bytes([HAPTIC_STOP])):
            if not self.haptics:
                return [command_error(cmd, UNSUPPORTED)]
            if sub == bytes([HAPTIC_STOP]):   # always taken
                if self.pattern is None:
                    return []
                pid, self.pattern = self.pattern, None
                return [bytes([cmd, HAPTIC_PLAY, pid, HAPTIC_STOPPED, HAPTIC_REASON_NONE])]
            if self.gated():
                return [command_error(cmd, PROOF_REQUIRED)]
            if not self.haptics_enabled:
                return [bytes([cmd, HAPTIC_PLAY, frame[4], HAPTIC_REFUSED, HAPTIC_REASON_DISABLED])]
            out = []
            if self.pattern is not None:
                out.append(bytes([cmd, HAPTIC_PLAY, self.pattern, HAPTIC_STOPPED, HAPTIC_REASON_REPLACED]))
            self.pattern = frame[4]
            return out + [bytes([cmd, HAPTIC_PLAY, self.pattern, HAPTIC_PLAYING, HAPTIC_REASON_NONE])]
        if cmd == MG_COMMAND_ASSISTANT_STATE:
            self.shown = frame[1]
            return []
        if cmd == MG_COMMAND_SET_SETTINGS and self.haptics and len(frame) == 3 and frame[1] == HAPTICS_ENABLED:
            self.haptics_enabled = frame[2]   # per connection: taken with or without the proof, no answer
            return []
        return super().write(frame)

    def update_gate(self):
        if self.updates is None:
            self.updates = update_ref().Device(release=self.release, holds_token=self.holds_token)
        self.updates.proof = self.proof
        return self.updates

    def smp(self, request: bytes) -> int:
        return self.update_gate().smp(request)

    def update_prompt(self) -> str:
        return self.updates.prompt if self.updates else "none"

    def in_setup_mode(self) -> bool:
        """Setup mode lasts until the confirmed re-setup or SETUP_MODE_MS after the boot that entered it."""
        return self.setup_mode_at is not None and self.now - self.setup_mode_at < SETUP_MODE_MS

    def resetup_closed(self) -> bool:
        """On a release build a set-up gadget whose only key is its talk key takes a re-setup only in setup mode."""
        return self.talk_key_only and self.release and self.holds_token and not self.in_setup_mode()

    def talk_dismisses(self) -> bool:
        """On a set-up gadget's release build a talk-key press only dismisses a re-setup prompt (in setup mode a
        talk-key-only gadget's one key confirms it)."""
        return self.release and self.holds_token and not (self.talk_key_only and self.in_setup_mode())

    def event(self, what: str, ms: int = 0, source: str = None, conn: int = 1, key: str = None,
              invalid: bool = False) -> list:
        if what == "press" and self.update_prompt() == "confirm":
            self.updates.event("press")   # the update's own prompt takes the press: that upload, nothing else
            return []
        if what == "injected_press":   # a console or Nordic UART word, or a scripted tap: it confirms nothing
            if self.updates:
                self.updates.event("injected_press")
            return []
        if what in ("time", "reconnect") and self.updates:
            self.updates.event(what, ms)
        if what == "resetup_request":
            hello = (conn, self.hellos.get(conn, 0) + 1)
            self.hellos[conn] = hello[1]
            self.setup_encrypted = False
            if self.resetup_closed():   # error_pairing_unavailable, no session, nothing shows
                self.setup_answer, self.prompt_change = "error_pairing_unavailable", "none"
            elif invalid and self.prompt is not None and self.prompt[0] == conn:
                # a takeover hello that fails validation gets its error and ends the prompt: nothing latched
                self.prompt, self.setup_answer, self.prompt_change = None, "error_pairing_invalid_hello", "ended"
                self.setup_encrypted = self.bad_hello_encrypted
                if self.bad_hello_drops_link:
                    self.event("reconnect", ms, source)   # the link goes after the refusal
            elif invalid and self.prompt is None:
                self.setup_answer, self.prompt_change = "error_pairing_invalid_hello", "none"
            elif self.prompt is not None and self.prompt[0] == conn:
                # never refused while its prompt is up: the newest hello on the prompt's connection takes it over
                self.prompt, self.setup_answer, self.prompt_change = hello, "confirm_required", "taken_over"
            elif self.prompt is not None or (self.setup_conn is not None and self.setup_conn != conn):
                # another connection can't take over the owner's prompt, nor start while that setup still runs
                self.setup_answer, self.prompt_change = "error_operation_in_progress", "none"
            elif self.release and self.holds_token and conn in self.dismissed_at \
                    and self.now - self.dismissed_at[conn] < RESETUP_PROMPT_MS:
                self.setup_answer, self.prompt_change = "error_operation_in_progress", "none"
            else:
                self.prompt, self.setup_answer, self.prompt_change = hello, "confirm_required", "shown"
            return []
        if what == "tap_elsewhere":   # a tap outside the Confirm disc's hit area: nothing
            return []
        if what == "dismiss":   # the back gesture (or, below, a talk-key press on a re-setup prompt)
            if self.prompt is not None:
                self.dismissed_at[self.prompt[0]], self.prompt = self.now, None
                if self.dismiss_drops_link:   # the link goes, and its prompt state with it
                    return self.event("reconnect", ms, source)
            return []
        if what == "press" and self.prompt is not None:   # the confirm key belongs to the prompt
            if key == "talk" and self.talk_dismisses():   # only dismisses: no talk turn, no confirm
                return self.event("dismiss", ms, source, conn)
            self.confirmed, self.prompt = self.prompt, None
            self.setup_conn = self.confirmed[0]   # its setup runs on, provisioning included
            if self.talk_key_only and self.holds_token:
                self.setup_mode_at = None   # setup mode ends at the confirmed re-setup
            return []
        if what == "setup_ends":   # that connection's provision_v2 committed (auth_ok), or its session ended
            if self.setup_conn == conn:
                self.setup_conn = None
            return []
        if what == "setup_mode":   # the gadget restarts in setup mode (its button held at boot), a new connection
            out = self.event("reconnect", ms, source)
            self.setup_mode_at = self.now
            return out
        if what == "reconnect":
            self.pattern = self.prompt = self.confirmed = self.setup_conn = None
            self.hellos, self.dismissed_at = {}, {}
            self.haptics_enabled = 0
            if self.skills:
                self.skills.rx = None
        return super().event(what, ms, source)

    def acted(self) -> dict:
        return {"hid_keys": list(self.hid_keys), "pattern": self.pattern, "shown": self.shown}

    def setup_step(self) -> dict:
        """The setup answer to the last resetup_request: error_operation_in_progress goes out at once in plaintext
        (no keys yet: its ASCII bytes on the setup TX characteristic); confirm_required goes out encrypted once the
        handshake has its keys. prompt: what the hello did to the prompt (shown; taken_over: the prompt stays on
        screen, now bound to this hello, with no new flash; none)."""
        if self.setup_answer.startswith("error_") and not self.setup_encrypted:
            return {"status": self.setup_answer, "plaintext": self.setup_answer.encode().hex(),
                    "prompt": self.prompt_change}
        return {"status": self.setup_answer, "encrypted": True, "prompt": self.prompt_change}

    def resetup_state(self) -> dict:
        """The prompt: the connection and hello (counted per connection) it is bound to (null: none up), and those
        the last press confirmed (pairing_confirmed goes to that hello's session)."""
        p, c = self.prompt or (None, None), self.confirmed or (None, None)
        return {"prompt_conn": p[0], "prompt_hello": p[1], "confirmed_conn": c[0], "confirmed_hello": c[1]}


class ProofGateRecorder(MicGateRecorder):
    def __init__(self, name, note, **device):
        super().__init__(name, note, device_cls=ProofGateDevice, **device)

    def _tag(self):
        st = self.ex["steps"][-1]
        st.pop("mic", None)
        st["acted"] = self.dev.acted()

    def write(self, frame: bytes, note: str = None):
        super().write(frame, note)
        self._tag()

    def proof(self, n: int, note: str = None):
        super().proof(n, note)
        self._tag()

    def event(self, what: str, note: str = None, **args):
        super().event(what, note, **args)
        if what == "resetup_request":
            self.ex["steps"][-1]["setup"] = self.dev.setup_step()
        if what in RESETUP_EVENTS:
            self.ex["steps"][-1]["resetup"] = self.dev.resetup_state()
        self._tag()

    def smp(self, request: bytes, note: str = None):
        """An SMP request to the update gate: its rc and the update prompt after it."""
        step = {"smp": request.hex(), "rc": self.dev.smp(request), "update_prompt": self.dev.update_prompt()}
        if note:
            step["note"] = note
        self.ex["steps"].append(step)
        self._tag()

    def skill_invoke(self, rid: int, request: bytes, note: str = None):
        """Chunked as at ATT MTU 23 (a device takes shorter chunks), so the request takes several."""
        for i, f in enumerate(skills_ref().invoke_frames(rid, request, 23)):
            self.write(f, note if i == 0 else None)


SKILL_LED = b'{"name":"device.led","params":{"r":255,"g":0,"b":0}}'


def gate_hid_key_exchange() -> dict:
    r = ProofGateRecorder("hid_key behind the proof", "A release build of a set-up device takes ui_input hid_key "
                          "only after this connection's token proof matched.", proof=False)
    r.write(hid_key(0x0224), "hid_key AC Back before a proof: proof_required, the HID presses nothing")
    r.proof(2, "the token proof matches")
    r.write(hid_key(0x0224), "hid_key AC Back: pressed and released through the HID, no answer")
    return r.ex


def gate_skills_exchange() -> dict:
    r = ProofGateRecorder("legacy skills behind the proof", "Before the proof, a skill invoke's first chunk gets "
                          "proof_required and its later chunks are ignored; list_request and cancel stay open. After "
                          "the proof the invoke runs.", proof=False)
    r.write(skills_ref().list_request(0), "list_request before a proof: answered (reading acts on nothing)")
    r.skill_invoke(1, SKILL_LED, "invoke device.led before a proof: proof_required at its first chunk, the rest "
                   "ignored, nothing runs")
    r.write(skills_ref().cancel(1), "cancel: open, nothing to drop, no answer")
    r.proof(2, "the token proof matches")
    r.skill_invoke(2, SKILL_LED, "invoke device.led: runs, its result follows")
    return r.ex


def gate_haptics_exchange() -> dict:
    r = ProofGateRecorder("haptics behind the proof", "Before the proof haptic_play gets proof_required and plays "
                          "nothing; haptic_stop is always taken. After the proof haptic_play plays.", proof=False)
    r.write(set_settings((HAPTICS_ENABLED, 1)), "haptics_enabled 1 (per connection, taken without the proof)")
    r.write(haptic_play(1), "haptic_play before a proof: proof_required, nothing plays")
    r.write(haptic_stop(), "haptic_stop before a proof: taken (nothing of the client's plays, no answer)")
    r.proof(2, "the token proof matches")
    r.write(haptic_play(2), "haptic_play: playing")
    r.write(haptic_stop(), "haptic_stop: stopped, reason none")
    return r.ex


def gate_open_exchange() -> dict:
    r = ProofGateRecorder("what stays open without the proof", "assistant_state needs no proof (the throughput "
                          "test neither); a device that doesn't implement hid_key, skills or haptics answers "
                          "unsupported, before any proof check.", proof=False, hid=False, skills=False, haptics=False)
    r.write(bytes([MG_COMMAND_ASSISTANT_STATE, 2]), "assistant_state thinking before a proof: shown")
    r.write(hid_key(0x0223), "hid_key on a device without a HID: unsupported (not proof_required)")
    r.skill_invoke(1, SKILL_LED, "a skill invoke on a device without skills: unsupported")
    r.write(haptic_play(1), "haptic_play on a device without haptics: unsupported")
    return r.ex


def gate_exempt_exchange() -> dict:
    r = ProofGateRecorder("automation builds and unset devices", "An automation build, and a device that holds no "
                          "token, take hid_key, skill invokes and haptic_play without a proof.", release=False,
                          proof=False)
    r.write(hid_key(0x0223), "automation build, no proof: hid_key AC Home pressed")
    r.skill_invoke(1, SKILL_LED, "a skill invoke: runs")
    r.write(set_settings((HAPTICS_ENABLED, 1)), "haptics_enabled 1")
    r.write(haptic_play(1), "haptic_play: playing")
    r.event("reconnect", "a new link, still no proof")
    r.write(set_settings((HAPTICS_ENABLED, 1)), "haptics_enabled 1 again (per connection)")
    r.write(haptic_play(2), "haptic_play: playing")
    return r.ex


def gate_unset_exchange() -> dict:
    r = ProofGateRecorder("a release build that isn't set up", "A device that holds no token has no proof to wait "
                          "for: hid_key, skill invokes and haptic_play are taken.", holds_token=False, proof=False)
    r.write(hid_key(0x0224), "hid_key: pressed")
    r.skill_invoke(1, SKILL_LED, "a skill invoke: runs")
    r.write(set_settings((HAPTICS_ENABLED, 1)), "haptics_enabled 1")
    r.write(haptic_play(1), "haptic_play: playing")
    return r.ex


RESETUP_EVENTS = ("resetup_request", "dismiss", "press", "setup_ends", "tap_elsewhere", "setup_mode",
                  "injected_press")


def resetup_prompt_exchange() -> dict:
    r = ProofGateRecorder("the re-setup prompt's rate", "On a set-up device's release build a hello is never "
                          "refused while a re-setup prompt is up: it takes over the prompt (bound to the newest "
                          "hello, no new flash) and the one press confirms it. Only a hello within 10 s after a "
                          "dismiss, with no prompt up, gets error_operation_in_progress and shows nothing; a new "
                          "connection may show the prompt at once. For a device that keeps the link on a dismiss "
                          "(dismiss_drops_link false); one that drops it runs the next exchange instead.",
                          dismiss_drops_link=False)
    r.event("resetup_request", "a re-setup request (hello 1): the prompt shows (confirm_required)")
    r.event("time", "4 s later", ms=4000)
    r.event("resetup_request", "another hello (2) while the prompt is up: not refused, it takes the prompt over")
    r.event("press", "one press: confirms hello 2's setup (pairing_confirmed to the newest hello), no talk turn")
    r.event("resetup_request", "a later hello (3): the prompt shows")
    r.event("dismiss", "the user dismisses it")
    r.event("time", "4 s after the dismiss", ms=4000)
    r.event("resetup_request", "a hello (4) within 10 s of the dismiss: error_operation_in_progress in plaintext, "
            "no prompt")
    r.event("time", "10 s after the dismiss", ms=6000)
    r.event("resetup_request", "a hello (5): the prompt shows again")
    r.event("dismiss", "dismissed again")
    r.event("reconnect", "a new connection")
    r.event("resetup_request", "its first hello: the prompt shows at once")
    return r.ex


def resetup_prompt_dismiss_drops_link_exchange() -> dict:
    r = ProofGateRecorder("the re-setup prompt's rate, a dismiss drops the link", "The same rule on a device whose "
                          "dismiss ends the setup link (dismiss_drops_link true): the disconnect clears the prompt "
                          "state, so no hello can follow on that connection, and the phone's new connection may "
                          "show the prompt at once.", dismiss_drops_link=True)
    r.event("resetup_request", "a re-setup request (hello 1): the prompt shows (confirm_required)")
    r.event("time", "4 s later", ms=4000)
    r.event("resetup_request", "another hello (2) while the prompt is up: not refused, it takes the prompt over")
    r.event("press", "one press: confirms hello 2's setup, no talk turn")
    r.event("resetup_request", "a later hello (3): the prompt shows")
    r.event("dismiss", "the user dismisses it: the device drops the link, the prompt state goes with it")
    r.event("time", "4 s after the dismiss", ms=4000)
    r.event("reconnect", "the phone connects again")
    r.event("resetup_request", "the new connection's first hello, 4 s after the dismiss: the prompt shows at once")
    return r.ex


def resetup_prompt_two_connections_exchange() -> dict:
    r = ProofGateRecorder("the re-setup prompt across two connections", "Only a hello on the prompt's own "
                          "connection takes it over: a hello from another connection while the prompt is up gets "
                          "error_operation_in_progress and leaves the prompt bound to the first connection's "
                          "hello, which the press confirms. The confirmed setup keeps other connections out until "
                          "it ends (its provision_v2 commits or its session ends).")
    r.event("resetup_request", "connection 1's hello: the prompt shows, bound to it")
    r.event("time", "2 s later", ms=2000)
    r.event("resetup_request", "connection 2's hello while the prompt is up: error_operation_in_progress in "
            "plaintext, the prompt stays connection 1's", conn=2)
    r.event("press", "the press: confirms connection 1's hello; its setup runs on, provisioning included")
    r.event("resetup_request", "connection 2's hello while connection 1 provisions: error_operation_in_progress, "
            "nothing shows", conn=2)
    r.event("setup_ends", "connection 1's provision_v2 commits (auth_ok): its setup has ended")
    r.event("resetup_request", "connection 2's next hello, no setup running: the prompt shows, bound to it", conn=2)
    return r.ex


def resetup_talk_key_exchange() -> dict:
    r = ProofGateRecorder("the talk key never confirms a re-setup", "On a set-up gadget's release build a talk-key "
                          "press while the re-setup prompt shows only dismisses it (no talk turn, no confirm); a "
                          "tap outside the Confirm disc does nothing; the confirm key or a tap on the disc "
                          "confirms.", dismiss_drops_link=False)
    r.event("resetup_request", "a re-setup request (hello 1): the prompt shows")
    r.event("tap_elsewhere", "a tap outside the Confirm disc: nothing, the prompt stays")
    r.event("press", "a talk-key press: dismisses the prompt, no talk turn, no confirm", key="talk")
    r.event("time", "4 s later", ms=4000)
    r.event("resetup_request", "a hello (2) within 10 s of that dismiss: error_operation_in_progress, no prompt")
    r.event("time", "10 s after the dismiss", ms=6000)
    r.event("resetup_request", "a hello (3): the prompt shows")
    r.event("press", "the confirm key (or a tap on the Confirm disc): confirms hello 3")
    return r.ex


def resetup_talk_key_only_exchange() -> dict:
    r = ProofGateRecorder("re-setup on a gadget whose only key is its talk key", "On a release build such a "
                          "set-up gadget takes a re-setup only in setup mode (its button held at boot): outside it a "
                          "hello gets error_pairing_unavailable in plaintext, no session starts and nothing shows; in "
                          "setup mode the same hello prompts and its one key confirms.", talk_key_only=True)
    r.event("resetup_request", "a re-setup hello outside setup mode: error_pairing_unavailable, nothing shows")
    r.event("setup_mode", "the owner holds the button at boot: the gadget starts in setup mode, the phone reconnects")
    r.event("resetup_request", "the same hello in setup mode: the prompt shows")
    r.event("press", "its one key (the talk key) in setup mode: confirms", key="talk")
    return r.ex


def setup_mode_ends_exchange() -> dict:
    r = ProofGateRecorder("a talk-key-only gadget's setup mode ends", "Setup mode ends at the confirmed re-setup or "
                          "5 min after the boot that entered it; after either a re-setup hello gets "
                          "error_pairing_unavailable again.", talk_key_only=True)
    r.event("setup_mode", "the gadget starts in setup mode (its button held at boot), the phone connects")
    r.event("resetup_request", "a re-setup hello: the prompt shows")
    r.event("press", "its one key: confirms, and setup mode ends", key="talk")
    r.event("resetup_request", "another re-setup hello: error_pairing_unavailable, nothing shows")
    r.event("setup_mode", "setup mode again")
    r.event("time", "5 min later", ms=SETUP_MODE_MS)
    r.event("resetup_request", "a re-setup hello: setup mode has ended, error_pairing_unavailable")
    return r.ex


def first_setup_talk_key_exchange() -> dict:
    r = ProofGateRecorder("first setup takes any one press", "A gadget that isn't set up keeps any one press or tap "
                          "for its first setup, the talk key included, with no setup mode needed.",
                          holds_token=False, talk_key_only=True)
    r.event("resetup_request", "a setup hello: the prompt shows")
    r.event("press", "the talk key: confirms", key="talk")
    return r.ex


def resetup_talk_key_automation_exchange() -> dict:
    r = ProofGateRecorder("the talk key on an automation build", "The talk-key rules are release-only: an "
                          "automation build takes a re-setup outside setup mode and a talk press confirms it.",
                          release=False, talk_key_only=True)
    r.event("resetup_request", "a re-setup hello outside setup mode: the prompt shows")
    r.event("press", "the talk key: confirms", key="talk")
    return r.ex


def resetup_bad_takeover_exchange(encrypted: bool = False, drops_link: bool = False) -> dict:
    name = "a takeover hello that fails validation ends the prompt" + (", answered encrypted" if encrypted else "") \
        + (", the link dropped" if drops_link else "")
    r = ProofGateRecorder(name, "A hello on the prompt's own connection whose top-level fields are valid but whose "
                          "mobile_pub or mobile_nonce fails validation gets its error and ends the prompt: nothing is "
                          "latched, a press afterwards confirms nothing, and the next valid hello prompts afresh and "
                          "needs its own press. The error goes out in plaintext, or encrypted under the session still "
                          "current by a device that refuses before wiping it (bad_hello_encrypted); a device may drop "
                          "the link after it (bad_hello_drops_link), and the next hello comes on a new connection.",
                          talk_key_only=False, bad_hello_encrypted=encrypted, bad_hello_drops_link=drops_link)
    r.event("resetup_request", "a re-setup request (hello 1): the prompt shows")
    r.event("resetup_request", "hello 2 on the same connection, a bad mobile_pub: error_pairing_invalid_hello "
            + ("encrypted under the current session" if encrypted else "in plaintext") + ", the prompt ends"
            + (", the link drops" if drops_link else ""), invalid=True)
    if drops_link:
        r.event("reconnect", "the phone connects again")
    r.event("press", "a talk-key press: no prompt is up, so nothing is confirmed", key="talk")
    r.event("resetup_request", f"a valid hello ({1 if drops_link else 3}): the prompt shows afresh")
    r.event("press", f"the confirm key: confirms hello {1 if drops_link else 3}, the one it was shown for")
    return r.ex


def update_press_no_window_exchange() -> dict:
    u = update_ref()
    r = ProofGateRecorder("an update press opens no window", "On a set-up gadget's release build the owner's press on "
                          "an unproven link's update prompt confirms that prompt's upload and nothing else: it opens "
                          "no window, so an injected tap from that link confirms nothing, and a re-setup prompt it "
                          "raises stays unconfirmed.", proof=False, talk_key_only=False)
    r.smp(u.UPLOAD_0, "an unproven link's image upload: rc 10, the update prompt shows")
    r.event("press", "the owner's physical press: confirms that upload")
    r.smp(u.UPLOAD_0, "the retry: rc 0, the confirmed upload")
    r.smp(u.UPLOAD_OTHER_0, "another image's upload: rc 10, not covered (no prompt yet: the last showed under 10 s "
          "ago)")
    r.event("injected_press", "an injected confirm tap from that link: nothing")
    r.event("resetup_request", "the same link raises a re-setup prompt (hello 1): it shows")
    r.event("injected_press", "an injected tap: the re-setup prompt stays, unconfirmed")
    return r.ex


def resetup_prompt_exempt_exchange() -> dict:
    r = ProofGateRecorder("the re-setup prompt on an automation build", "Automation builds are exempt from the "
                          "dismiss rule: a hello after a dismiss shows the prompt at once. For a device that keeps "
                          "the link on a dismiss (dismiss_drops_link false).", release=False,
                          dismiss_drops_link=False)
    r.event("resetup_request", "a re-setup request: the prompt shows")
    r.event("time", "1 s later", ms=1000)
    r.event("resetup_request", "another while it is up: takes it over")
    r.event("dismiss", "dismissed")
    r.event("resetup_request", "a hello right after the dismiss: the prompt shows")
    return r.ex


def proof_gate_section() -> dict:
    return {
        "description": "More of the protocol behind the token proof (mgcommands.h, Token proof; README.md, "
                       "Gadget setup v2). Steps as in streams_and_updates; each also records acted: the usages the "
                       "device's HID pressed, the client's haptic pattern playing (its id) and the assistant_state "
                       "shown. The device lists ui_input hid_key (its HID host connected), mg_command_skill with "
                       "mg-skills-v2.json's test_skills, test_payloads and test_manifest, haptic_play and "
                       "haptic_stop (a pattern plays until stopped; haptic_play is refused, reason disabled, until "
                       "the client writes haptics_enabled 1 with set_settings on that connection, which is taken "
                       "without the proof) and assistant_state; device flags hid, skills or "
                       "haptics false leave that part out. Event resetup_request is a pairing_client_hello in Link "
                       "setup on this device; setup is its answer: confirm_required (encrypted, once the handshake "
                       "has its keys) or error_operation_in_progress (at once, in plaintext: plaintext is its bytes "
                       "on the setup TX characteristic), and prompt what it did to the re-setup prompt (shown, "
                       "taken_over: still on screen, bound to this hello, no new flash; none). Event dismiss is the "
                       "back gesture (a swipe or a separate back key); event press while the prompt is up is the "
                       "confirm key or a tap on the Confirm disc, which confirms it, or with key talk the talk key, "
                       "which on a set-up release device only dismisses a re-setup prompt (a dismiss on its "
                       "connection) and otherwise confirms; event tap_elsewhere (a tap outside the disc) changes "
                       "nothing. Event setup_mode: the device restarts in setup mode (its button held at boot, into "
                       "its normal firmware with Link setup under its normal identity; never a SAFE) and the phone "
                       "reconnects; setup mode ends at the confirmed re-setup or setup_mode_ms after that boot "
                       "(test and bench builds are always in it). Device flag talk_key_only: its only key is its "
                       "talk key; on a release build it takes a re-setup only in setup mode, answering a hello "
                       "outside it "
                       "error_pairing_unavailable (in plaintext, no session). "
                       "resetup, on those events: the connection and hello (counted from 1 on each "
                       "connection) the prompt is bound to (null: none up) and those the last press confirmed. A "
                       "resetup_request step may carry conn, the connection it arrives on when several are open "
                       "(default 1), and invalid true: a hello whose top-level fields are valid but whose "
                       "mobile_pub or mobile_nonce fails validation (error_pairing_invalid_hello; on the prompt's own "
                       "connection it ends the prompt, prompt ended; device flags bad_hello_encrypted, the refusal "
                       "encrypted under the session still current instead of in plaintext, and "
                       "bad_hello_drops_link, the link dropped after it, work like dismiss_drops_link). Event "
                       "setup_ends (with conn) is that "
                       "connection's confirmed setup ending: its "
                       "provision_v2 committed (auth_ok) or its session ended; until then other connections' hellos "
                       "are refused too. Device flag dismiss_drops_link (default false): a device whose dismiss ends "
                       "the setup link runs the exchanges flagged true and skips those flagged false, and the other "
                       "way round; an exchange without the flag holds for both. An smp step is an SMP v1 request to "
                       "the update gate of mg-update-v2.json (mg_update_ref.py's model) with the rc it answers and "
                       "update_prompt after it (none or confirm); a press while the update prompt shows answers only "
                       "that prompt's upload. Event injected_press is a console or Nordic UART word or a scripted "
                       "tap: it confirms neither prompt.",
        "resetup_prompt_ms": RESETUP_PROMPT_MS,
        "setup_mode_ms": SETUP_MODE_MS,
        "exchanges": [gate_hid_key_exchange(), gate_skills_exchange(), gate_haptics_exchange(), gate_open_exchange(),
                      gate_exempt_exchange(), gate_unset_exchange(), resetup_prompt_exchange(),
                      resetup_prompt_dismiss_drops_link_exchange(), resetup_prompt_two_connections_exchange(),
                      resetup_bad_takeover_exchange(), resetup_bad_takeover_exchange(encrypted=True),
                      resetup_bad_takeover_exchange(drops_link=True),
                      resetup_bad_takeover_exchange(encrypted=True, drops_link=True), update_press_no_window_exchange(),
                      resetup_prompt_exempt_exchange(), resetup_talk_key_exchange(),
                      resetup_talk_key_only_exchange(),
                      setup_mode_ends_exchange(), first_setup_talk_key_exchange(),
                      resetup_talk_key_automation_exchange()],
    }


def replay_proof_gates(section: dict, device_cls=None) -> list:
    """Replays the section; returns the steps a device gets wrong."""
    device_cls = device_cls or ProofGateDevice
    bad = []
    k = session_seal_inputs()[0]
    for ex in section["exchanges"]:
        dev = device_cls(ex["mtu"], **ex["device"])
        for i, step in enumerate(ex["steps"]):
            if "smp" in step:
                if dev.smp(bytes.fromhex(step["smp"])) != step["rc"] or dev.update_prompt() != step["update_prompt"] \
                        or dev.acted() != step["acted"]:
                    bad.append(f"{ex['name']} step {i}")
                continue
            if "write" in step:
                got = dev.write(bytes.fromhex(step["write"]))
            elif step["event"] == "proof":
                got = dev.new_proof(k, bytes.fromhex(step["client_nonce"]), bytes.fromhex(step["device_nonce"]))
            else:
                got = dev.event(step["event"], **{a: step[a] for a in ("ms", "source", "conn", "key", "invalid")
                                                  if a in step})
            if [f.hex() for f in got] != step["notify"] or dev.acted() != step["acted"] \
                    or step.get("setup") != (dev.setup_step() if step.get("event") == "resetup_request" else None) \
                    or step.get("resetup") != (dev.resetup_state() if step.get("event") in RESETUP_EVENTS else None):
                bad.append(f"{ex['name']} step {i}")
    return bad


class UngatedDevice(ProofGateDevice):
    """Acts for a central whose proof never matched: must fail the section."""

    def gated(self) -> bool:
        return False


class UnlimitedPromptDevice(ProofGateDevice):
    """Shows a new re-setup prompt on every hello, even right after a dismiss: must fail the section."""

    def event(self, what: str, ms: int = 0, source: str = None, conn: int = 1, key: str = None,
              invalid: bool = False) -> list:
        if what == "resetup_request":
            self.hellos[conn] = self.hellos.get(conn, 0) + 1
            self.prompt, self.setup_answer, self.prompt_change = (conn, self.hellos[conn]), "confirm_required", "shown"
            return []
        return super().event(what, ms, source, conn, key, invalid)


class RefusesWhilePromptUp(ProofGateDevice):
    """Refuses a hello within 10 s of the last prompt shown, even while that prompt is up: must fail the
    section."""

    shown_at = None

    def event(self, what: str, ms: int = 0, source: str = None, conn: int = 1, key: str = None,
              invalid: bool = False) -> list:
        if what == "resetup_request" and self.release and self.holds_token and self.shown_at is not None \
                and self.now - self.shown_at < RESETUP_PROMPT_MS:
            self.hellos[conn] = self.hellos.get(conn, 0) + 1
            self.setup_answer, self.prompt_change = "error_operation_in_progress", "none"
            return []
        out = super().event(what, ms, source, conn, key, invalid)
        if what == "resetup_request" and self.prompt_change == "shown":
            self.shown_at = self.now
        if what == "reconnect":
            self.shown_at = None
        return out


class TakeoverAcrossConnections(ProofGateDevice):
    """Lets a hello from any connection take over the prompt: must fail the section."""

    def event(self, what: str, ms: int = 0, source: str = None, conn: int = 1, key: str = None,
              invalid: bool = False) -> list:
        if what == "resetup_request" and self.prompt is not None and self.prompt[0] != conn:
            self.hellos[conn] = self.hellos.get(conn, 0) + 1
            self.prompt, self.setup_answer, self.prompt_change = (conn, self.hellos[conn]), "confirm_required", \
                "taken_over"
            return []
        return super().event(what, ms, source, conn, key, invalid)


class PromptsDuringOthersSetup(ProofGateDevice):
    """Lets another connection prompt while a confirmed setup is still provisioning: must fail the section."""

    def event(self, what: str, ms: int = 0, source: str = None, conn: int = 1, key: str = None,
              invalid: bool = False) -> list:
        out = super().event(what, ms, source, conn, key, invalid)
        if what == "press":
            self.setup_conn = None
        return out


class TalkKeyConfirmsResetup(ProofGateDevice):
    """Lets the talk key confirm a re-setup prompt: must fail the section."""

    def talk_dismisses(self) -> bool:
        return False


class SetupModeNeverEnds(ProofGateDevice):
    """Keeps setup mode after the confirmed re-setup and past its 5 min: must fail the section."""

    def in_setup_mode(self) -> bool:
        return self.setup_mode_at is not None

    def event(self, what: str, ms: int = 0, source: str = None, conn: int = 1, key: str = None,
              invalid: bool = False) -> list:
        at = self.setup_mode_at
        out = super().event(what, ms, source, conn, key, invalid)
        if what == "press" and at is not None and self.setup_mode_at is None:
            self.setup_mode_at = at
        return out


class LatchesPressAfterBadTakeover(ProofGateDevice):
    """Answers a takeover hello that fails validation but keeps the prompt for the earlier hello, so the next press
    of any key confirms it: must fail the section."""

    def event(self, what: str, ms: int = 0, source: str = None, conn: int = 1, key: str = None,
              invalid: bool = False) -> list:
        if what == "resetup_request" and invalid and self.prompt is not None and self.prompt[0] == conn:
            self.hellos[conn] = self.hellos.get(conn, 0) + 1
            self.setup_answer, self.prompt_change = "error_pairing_invalid_hello", "none"
            self.latched = True
            return []
        if what == "press" and getattr(self, "latched", False) and self.prompt is not None:
            self.latched = False
            key = None   # the latched press confirms, whatever key it was
        return super().event(what, ms, source, conn, key, invalid)


class UpdatePressOpensWindow(ProofGateDevice):
    """Lets the update-confirm press open the link's other words, so an injected tap then counts as a press: must
    fail the section."""

    window = False

    def event(self, what: str, ms: int = 0, source: str = None, conn: int = 1, key: str = None,
              invalid: bool = False) -> list:
        if what == "press" and self.update_prompt() == "confirm":
            self.window = True
        elif what == "injected_press" and self.window:
            what, key = "press", None
        return super().event(what, ms, source, conn, key, invalid)


class ResetupOutsideSetupMode(ProofGateDevice):
    """Prompts a re-setup on a talk-key-only gadget outside setup mode: must fail the section."""

    def resetup_closed(self) -> bool:
        return False


def check_proof_gates(section: dict) -> list:
    bad = replay_proof_gates(section)
    for mutant in (UngatedDevice, UnlimitedPromptDevice, RefusesWhilePromptUp, TakeoverAcrossConnections,
                   PromptsDuringOthersSetup, TalkKeyConfirmsResetup, ResetupOutsideSetupMode, SetupModeNeverEnds,
                   LatchesPressAfterBadTakeover, UpdatePressOpensWindow):
        if not replay_proof_gates(section, mutant):
            bad.append(f"{mutant.__name__} passes proof_gates")
    return bad


def build() -> dict:
    return {
        "description": "Device session (protocols/mgcommands.h, Device session). Hex strings are Control payloads (write: "
                       "client -> device, notify: what the device sends back, in order; an empty list "
                       "means nothing). Every exchange starts on a new connection at the given ATT MTU "
                       "with the given device flags (default: holds a token, proof matched, encrypted). "
                       "The device holds test_credentials (test_credentials_bound when its flags "
                       "have vm_binding) and sends them after its hello (as written there, keys in "
                       "that order), answers hello with device_hello, registers node_id with "
                       "test_commands as commands_v2 and is_wakeup_supported true (it handles wake), and answers "
                       "each command with its test_payload. While a task is "
                       "active (device flag task, event task: a user action, traffic it owes, a wait, a wake), "
                       "after a state it didn't ask for (closed, no_network) it sends open_request [1] at "
                       "once, then backs off (timed, not shown; closed and no_network run separate "
                       "series, capped at 60 s and 15 s); an open restarts a series only after a "
                       "session that stayed open 60 s; a closed or no_network after connecting (a "
                       "failed open) counts the same; with no task it asks nothing, and none after "
                       "not_enrolled, auth_failed or "
                       "too_large until a new hello, a credentials_update or an open. Credentials leave and are "
                       "taken only sealed (mgcommands.h, Sealed credentials, Sealed data): every unsealed hello is "
                       "refused insecure, and every session is sealed "
                       "under session_seal's K and proof nonces with node_id; a step with event proof is a new "
                       "matching token proof with the nonces it lists (the session reset at its challenge), the "
                       "keys for every sealed hello after it. A new proof, and every sealed hello, drop each "
                       "sealed message part way in or out and restart the counters, so nothing from the old "
                       "session completes in the new one or gives one of its counters back. The credentials are "
                       "fake. Generated by mg_session_ref.py.",
        "version": 1,
        "command": MG_COMMAND_SESSION,
        "device_hello": {"max_message": DEVICE_MAX_MESSAGE, "window": DEVICE_WINDOW},
        "node_id": NODE_ID,
        "register_id": REGISTER_ID,
        "test_credentials": TEST_CREDENTIALS,
        "test_credentials_bound": TEST_CREDENTIALS_BOUND,
        "test_commands": TEST_COMMANDS,
        "test_payloads": TEST_PAYLOADS,
        "session_seal": dict(zip(("K", "client_nonce", "device_nonce"), (v.hex() for v in session_seal_inputs()))),
        "mic_gate": mic_gate_section(),
        "streams_and_updates": stream_update_section(),
        "settings": settings_section(),
        "proof_gates": proof_gate_section(),
        "exchanges": [
            open_exchange(100), open_exchange(247), invoke_exchange(100), invoke_exchange(247),
            credit_exchange(), credentials_exchange(), bound_exchange(), session_exchange(),
            credit_counter_exchange(), credit_overrun_exchange(), small_window_exchange(), clear_exchange(),
            clear_busy_exchange(),
            clear_hold_exchange(), replay_after_proof_exchange(), stored_too_large_exchange(),
            reask_exchange(), wake_exchange(),
            errors_exchange(), malformed_data_exchange(),
            gating_exchange("no token", "A device that holds no token.", holds_token=False),
            no_proof_exchange(), keepalive_exchange(), nudge_exchange(), clear_unsent_exchange(),
            credentials_wait_exchange(), clear_credentials_unsent_exchange(),
            client_counter_top_exchange(), device_counter_top_exchange(), low_mtu_exchange(),
            unencrypted_exchange(),
            expires_exchange(),
            failed_open_exchange(),
            sealed_credentials_exchange(100), sealed_credentials_exchange(247), sealed_bound_exchange(),
            sealed_legacy_exchange(), unsealed_refused_exchange(),
        ],
    }


def render() -> str:
    return json.dumps(build(), indent=2, ensure_ascii=False) + "\n"


def check() -> list:
    """Replays every exchange in the file through the reference device; returns the problems."""
    data = json.loads(VECTORS.read_text(encoding="utf-8"))
    bad = []
    for ex in data["exchanges"]:
        dev = Device(ex["mtu"], **ex["device"])
        k = session_seal_inputs()[0]
        for i, step in enumerate(ex["steps"]):
            if step.get("event") == "proof":
                got = [f.hex() for f in dev.new_proof(k, bytes.fromhex(step["client_nonce"]),
                                                     bytes.fromhex(step["device_nonce"]))]
                if got != step["notify"]:
                    bad.append(f"{ex['name']} step {i}")
                continue
            if step.get("event") == "task":
                got = [f.hex() for f in dev.set_task(step["active"])]
                if got != step["notify"]:
                    bad.append(f"{ex['name']} step {i}")
                continue
            if step.get("event") == "counters":
                dev.rx, dev.tx = step.get("rx", dev.rx), step.get("tx", dev.tx)
                continue
            if step.get("event") == "nudge":
                if [f.hex() for f in dev.nudge()] != step["notify"]:
                    bad.append(f"{ex['name']} step {i}")
                continue
            if step.get("event") == "buffer":
                if [f.hex() for f in dev.buffer()] != step["notify"]:
                    bad.append(f"{ex['name']} step {i}")
                continue
            frame = bytes.fromhex(step["write"])
            if len(frame) > max_value(ex["mtu"]):
                bad.append(f"{ex['name']} step {i}: write longer than min(ATT_MTU - 3, 512)")
            got = [f.hex() for f in dev.write(frame)]
            if got != step["notify"]:
                bad.append(f"{ex['name']} step {i}")
            for f in got:
                if len(f) // 2 > max_value(ex["mtu"]):
                    bad.append(f"{ex['name']} step {i}: notification longer than min(ATT_MTU - 3, 512)")
    bad += check_mic_gate(data["mic_gate"])
    if not check_mic_gate(data["mic_gate"], UnsetTakesStartMic):
        bad.append("UnsetTakesStartMic passes mic_gate")
    bad += check_stream_update(data["streams_and_updates"])
    bad += check_settings(data["settings"])
    bad += check_proof_gates(data["proof_gates"])
    if VECTORS.read_text(encoding="utf-8") != render():
        bad.append("file differs from the generator's output")
    return bad


def main() -> int:
    if sys.argv[1:] == ["--write"]:
        VECTORS.write_text(render(), encoding="utf-8")
        print(f"wrote {VECTORS}")
        return 0
    if sys.argv[1:] not in ([], ["--check"]):
        print(__doc__)
        return 2
    bad = check()
    for b in bad:
        print(f"FAIL {b}")
    if not bad:
        data = json.loads(VECTORS.read_text(encoding="utf-8"))
        steps = sum(len(e["steps"]) for e in data["exchanges"])
        print(f"OK {VECTORS.name}: {len(data['exchanges'])} exchanges, {steps} steps")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
