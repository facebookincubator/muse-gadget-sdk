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
"""The phone's side of Muse Link setup v5 and the token proof, written from
the contract and the reference firmware (not from the C core), with
`cryptography`, plus a driver for the host gadget (sim.c).
"""

from __future__ import annotations

import base64
import hashlib
import hmac
import json
import os
import subprocess

from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives.ciphers.aead import AESGCM
from cryptography.hazmat.primitives.serialization import Encoding, PublicFormat

VERSION = 5
MODEL = "hatch_link"
SUITE = "p256-hkdf-sha256-aes-gcm-v1"
AUTH = "none"
POLICY = "confirm_press"
RECORD_LABEL = b"hatch-link ble setup v1"
SID_LABEL = b"hatch-link session id v1"
PROOF_SALT = b"mg token proof v1"


def b64u(b: bytes) -> str:
    return base64.urlsafe_b64encode(b).rstrip(b"=").decode()


def unb64u(s: str) -> bytes:
    return base64.urlsafe_b64decode(s + "=" * (-len(s) % 4))


def hkdf32(salt: bytes, ikm: bytes, info: bytes) -> bytes:
    prk = hmac.new(salt, ikm, hashlib.sha256).digest()
    return hmac.new(prk, info + b"\x01", hashlib.sha256).digest()


def proof_key(access: str, node_id: str) -> bytes:
    return hkdf32(PROOF_SALT, access.encode(), node_id.encode())


def proof_mac(k: bytes, device: bool, cn: bytes, dn: bytes) -> bytes:
    label = b"mg token proof v1 device" if device else b"mg token proof v1 client"
    return hmac.new(k, label + cn + dn, hashlib.sha256).digest()


def transcript(ready: dict, mobile_pub: str, mobile_nonce: str) -> bytes:
    return "\n".join([
        f"hatch-link-pairing-v{VERSION}",
        f"version={VERSION}",
        "initiator_role=mobile",
        "responder_role=link",
        f"device_id={ready['device_id']}",
        f"node_id={ready['node_id']}",
        f"mac={ready['mac']}",
        f"model={MODEL}",
        f"firmware_version={ready['firmware_version']}",
        f"selected_cipher_suite={SUITE}",
        f"pairing_auth={AUTH}",
        "pairing_auth_epoch=0",
        f"pairing_policy={POLICY}",
        "confirm_timeout_seconds=60",
        f"mobile_pub={mobile_pub}",
        f"device_pub={ready['device_pub']}",
        f"mobile_nonce={mobile_nonce}",
        f"device_nonce={ready['device_nonce']}",
    ]).encode()


def nonce(direction: int, counter: int) -> bytes:
    return bytes([direction, 0, 0, 0]) + counter.to_bytes(8, "big")


def aad(sid: str, direction: int, counter: int) -> bytes:
    return b"%s|%s|%s|%d" % (RECORD_LABEL, sid.encode(), b"m2d" if direction == 0 else b"d2m",
                             counter)


class Sim:
    """The host gadget, one command at a time; collects what it says."""

    def __init__(self, exe: str, *args: str):
        self.p = subprocess.Popen([exe, *args], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  text=True, bufsize=1)
        self.notes: list[bytes] = []  # notifications not yet consumed
        self.events: list[str] = []
        self.proof: list[bytes] = []
        self.problems: list[str] = []
        self.logs: list[str] = []
        self.disconnects = 0
        self.lines: list[str] = []
        self.asks: list[str] = []      # may_prompt() calls: "since=<ms|none> ok=<0|1>"
        self.other: list[str] = []     # notify_to to another connection: "CONN HEX"

    def cmd(self, line: str) -> list[str]:
        self.p.stdin.write(line + "\n")
        self.p.stdin.flush()
        out = []
        while True:
            r = self.p.stdout.readline()
            if not r:
                raise RuntimeError(f"sim exited during {line!r}")
            r = r.rstrip("\n")
            if r == "OK":
                return out
            out.append(r)
            self.lines.append(r)
            tag, _, rest = r.partition(" ")
            if tag == "N":
                self.notes.append(bytes.fromhex(rest))
            elif tag == "E":
                self.events.append(rest)
            elif tag == "P":
                self.proof.append(bytes.fromhex(rest))
            elif tag == "D":
                self.disconnects += 1
            elif tag == "X":
                self.problems.append(rest)
            elif tag == "L":
                self.logs.append(rest)
            elif tag == "M":
                self.asks.append(rest)
            elif tag == "O":
                self.other.append(rest)

    def state(self) -> dict:
        line = next(x for x in self.cmd("state") if x.startswith("S "))
        return {k: int(v) for k, v in (kv.split("=") for kv in line[2:].split())}

    def dump(self) -> dict:
        out = {}
        for x in self.cmd("dump"):
            if x.startswith("T ") and x != "T none":
                _, key, val = x.split(" ")
                out[key] = bytes.fromhex(val)
        return out

    def close(self):
        try:
            self.p.stdin.write("quit\n")
            self.p.stdin.flush()
        except BrokenPipeError:
            pass
        self.p.stdin.close()
        rest = self.p.stdout.read()
        self.p.stdout.close()
        self.p.wait(timeout=10)
        self.problems += [x[2:] for x in rest.splitlines() if x.startswith("X ")]
        if self.p.returncode != 0:
            self.problems.append(f"sim exited with {self.p.returncode}")


class Reassembler:
    """TX notifications into messages: chunked [0xFE, idx, total] or bare."""

    def __init__(self):
        self.parts: list[bytes] = []
        self.total = 0

    def feed(self, n: bytes):
        if len(n) < 3 or n[0] != 0xFE:
            return n
        idx, total = n[1], n[2]
        if idx == 0:
            self.parts, self.total = [], total
        assert total == self.total and idx == len(self.parts), "chunks out of order"
        self.parts.append(n[3:])
        if len(self.parts) < self.total:
            return None
        out = b"".join(self.parts)
        self.parts, self.total = [], 0
        return out


class Phone:
    """The app's side of one setup connection to the sim."""

    def __init__(self, sim: Sim, mtu: int = 247, private_value: int | None = None,
                 mobile_nonce: bytes | None = None, t0: int = 1000):
        self.sim = sim
        self.mtu = mtu
        self.t = t0
        self.priv = (ec.derive_private_key(private_value, ec.SECP256R1()) if private_value
                     else ec.generate_private_key(ec.SECP256R1()))
        self.mobile_pub = b64u(self.priv.public_key().public_bytes(
            Encoding.X962, PublicFormat.UncompressedPoint))
        self.mobile_nonce = mobile_nonce or os.urandom(16)
        self.rx = Reassembler()
        self.inbox: list = []
        self.tx_ctr = 0
        self.rx_ctr = 0
        self.keys: dict = {}
        self.ready: dict = {}

    # ---- transport

    def connect(self):
        self.sim.cmd(f"connect {self.t}")
        if self.mtu != 23:
            self.sim.cmd(f"mtu {self.mtu}")

    def poll(self, ms: int = 10):
        self.t += ms
        self.sim.cmd(f"poll {self.t}")
        self._drain()

    def _drain(self):
        while self.sim.notes:
            n = self.sim.notes.pop(0)
            assert len(n) <= self.mtu - 3, "notification over MTU - 3"
            m = self.rx.feed(n)
            if m is None:
                continue
            try:
                self.inbox.append(json.loads(m))
            except ValueError:
                self.inbox.append(m.decode())

    def chunks(self, data: bytes) -> list[bytes]:
        room = self.mtu - 3 - 3  # Android writes up to MTU - 3 bytes per write
        parts = [data[i:i + room] for i in range(0, len(data), room)] or [b""]
        assert len(parts) <= 255
        return [bytes([0xFE, i, len(parts)]) + p for i, p in enumerate(parts)]

    def write_raw(self, data: bytes, poll: bool = True):
        for c in self.chunks(data):
            self.sim.cmd("rx " + c.hex())
        if poll:
            self.poll()

    def write(self, obj: dict, poll: bool = True):
        self.write_raw(json.dumps(obj, separators=(",", ":")).encode(), poll)

    def recv(self):
        """The next message (dict or bare status string), or None."""
        self._drain()
        return self.inbox.pop(0) if self.inbox else None

    # ---- the handshake

    def hello(self) -> dict:
        return {"action": "pairing_client_hello", "version": VERSION, "pairing_auth": AUTH,
                "pairing_policy": POLICY, "mobile_pub": self.mobile_pub,
                "mobile_nonce": b64u(self.mobile_nonce)}

    def accept_ready(self, ready: dict):
        assert ready["type"] == "pairing_ready" and ready["version"] == VERSION, ready
        assert ready["pairing_auth"] == AUTH and ready["pairing_policy"] == POLICY
        assert ready["pairing_auth_epoch"] == 0 and ready["model"] == MODEL
        th = hashlib.sha256(transcript(ready, self.mobile_pub, b64u(self.mobile_nonce))).digest()
        assert b64u(th) == ready["transcript_hash"], "transcript hash"
        peer = ec.EllipticCurvePublicKey.from_encoded_point(ec.SECP256R1(),
                                                            unb64u(ready["device_pub"]))
        ss = self.priv.exchange(ec.ECDH(), peer)
        dn = unb64u(ready["device_nonce"])
        salt = hashlib.sha256(self.mobile_nonce + dn + th).digest()
        secret = hkdf32(salt, ss, RECORD_LABEL)
        prk_expand = lambda label: hmac.new(secret, label + b"\x01", hashlib.sha256).digest()
        sid = b64u(hashlib.sha256(SID_LABEL + th + ss).digest()[:16])
        assert sid == ready["session_id"], "session id"
        self.keys = {"tx": prk_expand(b"mobile->device"), "rx": prk_expand(b"device->mobile"),
                     "sid": sid, "secret": secret, "th": th}
        self.ready = ready
        self.tx_ctr = self.rx_ctr = 0

    def seal(self, obj: dict | bytes, counter: int | None = None) -> dict:
        pt = obj if isinstance(obj, bytes) else json.dumps(obj, separators=(",", ":")).encode()
        c = self.tx_ctr if counter is None else counter
        ct = AESGCM(self.keys["tx"]).encrypt(nonce(0, c), pt, aad(self.keys["sid"], 0, c))
        if counter is None:
            self.tx_ctr += 1
        return {"action": "pairing_encrypted", "session_id": self.keys["sid"],
                "counter": str(c), "ciphertext": b64u(ct[:-16]), "tag": b64u(ct[-16:])}

    def open(self, env) -> dict:
        assert isinstance(env, dict), f"expected a record, got {env!r}"
        assert env.get("type") == "pairing_encrypted", env
        assert env["session_id"] == self.keys["sid"]
        assert env["counter"] == str(self.rx_ctr), (env["counter"], self.rx_ctr)
        pt = AESGCM(self.keys["rx"]).decrypt(nonce(1, self.rx_ctr),
                                             unb64u(env["ciphertext"]) + unb64u(env["tag"]),
                                             aad(self.keys["sid"], 1, self.rx_ctr))
        self.rx_ctr += 1
        return json.loads(pt)

    def status(self) -> dict:
        m = self.recv()
        assert m is not None, "no message from the gadget"
        return self.open(m)

    def get_device_info(self) -> dict:
        self.write({"action": "get_device_info"})
        info = self.recv()
        assert isinstance(info, dict) and info["type"] == "device_info", info
        return info

    def pair(self, press: bool = True) -> dict:
        """hello -> pairing_ready -> client_finished -> confirm_required [-> press
        -> pairing_confirmed]. Returns the last status."""
        self.write(self.hello())
        self.accept_ready(self.recv())
        self.write(self.seal({"action": "pairing_client_finished"}))
        st = self.status()
        assert st == {"type": "status", "status": "confirm_required"}, st
        self.poll()                         # the prompt is up from the next poll
        if not press:
            return st
        r = self.sim.cmd("presskey other")
        assert "R press 2" in r, r          # one press of a key that isn't the talk key, setup or re-setup
        self.poll()
        st = self.status()
        assert st["status"] == "pairing_confirmed", st
        return st

    def provision(self, access: str, refresh: str, extra: dict | None = None,
                  ios_escaping: bool = True) -> dict:
        obj = {"action": "provision_v2", "access_token": access, "refresh_token": refresh,
               "token_type": "device"}
        obj.update(extra or {})
        pt = json.dumps(obj, separators=(",", ":"))
        if ios_escaping:  # Swift's JSONEncoder writes "/" as "\/"
            pt = pt.replace("/", "\\/")
        self.write(self.seal(pt.encode()))
        return self.status()

    # ---- token proof over mgcommands (payload after command byte 34)

    def proof(self, k: bytes, clear: bool = False) -> int | str:
        self.sim.proof.clear()
        cn = os.urandom(16)
        self.sim.cmd("proof 00" + cn.hex())
        r = self.sim.proof.pop(0)
        if r[0] == 25:  # mg_command_error
            return {5: "not_found"}.get(int.from_bytes(r[3:5], "little"), r.hex())
        assert r[:2] == bytes([34, 1]) and len(r) == 50, r.hex()
        dn, mac = r[2:18], r[18:50]
        device_ok = hmac.compare_digest(mac, proof_mac(k, True, cn, dn))
        self.sim.cmd("proof 02" + proof_mac(k, False, cn, dn).hex())
        res = self.sim.proof.pop(0)
        assert res[:2] == bytes([34, 3]), res.hex()
        assert res[2] == (1 if device_ok else 0)
        if clear and res[2] == 1:
            self.sim.cmd("proof 04")
            assert self.sim.proof.pop(0) == bytes([34, 3, 1])
        return res[2]
