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

"""smplink.py: test-side SMP over the conformance link, a minimal
CBOR codec, and faithful flows of the two Muse app clients:

- AndroidFlow: the Android app's MCUmgr client: params
  first (3 s), indefinite-length maps, chunk 0 alone then min(3, buf_count)
  in flight, 5 s per chunk, resend from the acked offset (3 retries), test,
  reset, disconnect within 10 s, reconnect within 60 s, image state, active
  hash == TLV hash, confirm if not confirmed.
- IOSFlow: the iOS app's MCUmgr client:
  params, a warm-up image state read, 100 ms, window 2 from
  chunk 0, definite maps in random key order, drain on an offset mismatch,
  30 s response timeout, 3 upload attempts restarting at off 0 (resume by
  sha), test, reset, reconnect, image state, confirm.

No third-party packages.
"""
import hashlib
import json
import os
import queue
import random
import socket
import struct
import threading
import time

# ---- CBOR ------------------------------------------------------------------------


class Undefined:
    pass


def _head(major, v):
    if v < 24:
        return bytes([major << 5 | v])
    if v <= 0xFF:
        return bytes([major << 5 | 24, v])
    if v <= 0xFFFF:
        return bytes([major << 5 | 25]) + struct.pack(">H", v)
    if v <= 0xFFFFFFFF:
        return bytes([major << 5 | 26]) + struct.pack(">I", v)
    return bytes([major << 5 | 27]) + struct.pack(">Q", v)


def cbor_encode(v, indefinite=False, shuffle=False):
    if v is True:
        return b"\xf5"
    if v is False:
        return b"\xf4"
    if v is None:
        return b"\xf6"
    if isinstance(v, int):
        return _head(0, v) if v >= 0 else _head(1, -1 - v)
    if isinstance(v, (bytes, bytearray)):
        return _head(2, len(v)) + bytes(v)
    if isinstance(v, str):
        b = v.encode()
        return _head(3, len(b)) + b
    if isinstance(v, list):
        return _head(4, len(v)) + b"".join(cbor_encode(x) for x in v)
    if isinstance(v, dict):
        items = list(v.items())
        if shuffle:
            random.shuffle(items)
        body = b"".join(cbor_encode(k) + cbor_encode(x) for k, x in items)
        return (b"\xbf" + body + b"\xff") if indefinite else (_head(5, len(items)) + body)
    raise TypeError(type(v))


def cbor_decode(b):
    v, i = _dec(b, 0, 0)
    if i != len(b):
        raise ValueError("trailing bytes")
    return v


def _arg(b, i, ai):
    if ai < 24:
        return ai, i
    n = {24: 1, 25: 2, 26: 4, 27: 8}.get(ai)
    if n is None:
        raise ValueError("bad additional info")
    if i + n > len(b):
        raise ValueError("short")
    return int.from_bytes(b[i:i + n], "big"), i + n


def _dec(b, i, depth):
    if depth > 16 or i >= len(b):
        raise ValueError("short/deep")
    ib = b[i]
    i += 1
    major, ai = ib >> 5, ib & 31
    if major in (2, 3, 4, 5) and ai == 31:
        if major in (2, 3):
            out = b""
            while b[i] != 0xFF:
                part, i = _dec(b, i, depth + 1)
                out += part.encode() if isinstance(part, str) else part
            return (out.decode() if major == 3 else out), i + 1
        if major == 4:
            out = []
            while b[i] != 0xFF:
                x, i = _dec(b, i, depth + 1)
                out.append(x)
            return out, i + 1
        out = {}
        while b[i] != 0xFF:
            k, i = _dec(b, i, depth + 1)
            x, i = _dec(b, i, depth + 1)
            out[k] = x
        return out, i + 1
    if major == 7:
        if ai == 20:
            return False, i
        if ai == 21:
            return True, i
        if ai in (22, 23):
            return None, i
        if ai in (25, 26, 27):
            n = {25: 2, 26: 4, 27: 8}[ai]
            return None, i + n
        raise ValueError("simple")
    v, i = _arg(b, i, ai)
    if major == 0:
        return v, i
    if major == 1:
        return -1 - v, i
    if major in (2, 3):
        if i + v > len(b):
            raise ValueError("short string")
        s = b[i:i + v]
        return (s.decode() if major == 3 else bytes(s)), i + v
    if major == 4:
        out = []
        for _ in range(v):
            x, i = _dec(b, i, depth + 1)
            out.append(x)
        return out, i
    if major == 5:
        out = {}
        for _ in range(v):
            k, i = _dec(b, i, depth + 1)
            x, i = _dec(b, i, depth + 1)
            out[k] = x
        return out, i
    if major == 6:
        return _dec(b, i, depth + 1)
    raise ValueError("major")


# ---- MCUboot envelope helpers ----------------------------------------------------------

def image_hash(image):
    """The image hash both apps' MCUmgr clients use: the first SHA-256 (or SHA-512) TLV."""
    hsize = struct.unpack_from("<H", image, 8)[0]
    isize = struct.unpack_from("<I", image, 12)[0]
    off = hsize + isize
    while off + 4 <= len(image):
        t, n = struct.unpack_from("<HH", image, off)
        if t in (0x6907, 0x6908):
            off += 4
            continue
        if t in (0x10, 0x12) and off + 4 + n <= len(image):
            return image[off + 4:off + 4 + n]
        off += 4 + n
    raise ValueError("no hash TLV")


def image_version(image):
    maj, mnr = image[20], image[21]
    rev, bld = struct.unpack_from("<HI", image, 22)
    return f"{maj}.{mnr}.{rev}+{bld}"


def fake_firmware(version=(1, 1, 0, 0), build_id=0x00C0FFEE, size=60000, seed=1):
    """The host_server's fake vendor payload: magic, id, body, CRC32."""
    import zlib
    rnd = random.Random(seed)
    body = bytes(rnd.getrandbits(8) for _ in range(size - 24))
    maj, mnr, rev, bld = version
    head = b"MGFAKEFW" + struct.pack("<BBHII", maj, mnr, rev, bld, build_id)
    data = head + body
    return data + struct.pack("<I", zlib.crc32(data) & 0xFFFFFFFF)


# ---- the link ----------------------------------------------------------------------------

class Link:
    """The link's client side: connect(mtu), write fragments, reassembled responses."""

    ATT_MAX = 512   # the ATT attribute limit: Android refuses a longer write, iOS never sends one

    def __init__(self, port, mtu=247, host="127.0.0.1", android=False):
        self.addr = (host, port)
        self.android = android   # fragment at MTU-3 as the Android app does (and fail over 512 as Android does)
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1 << 20)
        self.sock.settimeout(0.05)
        self.mtu = mtu
        self.connected = threading.Event()
        self.disconnected = threading.Event()
        self.responses = queue.Queue()
        self.status_q = queue.Queue()
        self.buf = b""
        self.notifications = 0
        self.max_notification = 0
        self.bad_notifications = 0
        self.closed = False
        self.reader = threading.Thread(target=self._read, daemon=True)
        self.reader.start()

    def close(self):
        self.closed = True
        self.reader.join(1)
        self.sock.close()

    def _read(self):
        while not self.closed:
            try:
                d, _ = self.sock.recvfrom(70000)
            except (socket.timeout, OSError):
                continue
            t, v = d[0], d[1:]
            if t == 0x04:
                self.disconnected.clear()
                self.connected.set()
            elif t == 0x02:
                self.connected.clear()
                self.disconnected.set()
                self.buf = b""
            elif t == 0x07:
                self.status_q.put(json.loads(v.decode()))
            elif t == 0x00:
                self.notifications += 1
                self.max_notification = max(self.max_notification, len(v))
                if len(v) > min(self.mtu - 3, self.ATT_MAX):
                    # Android refuses an ATT value over 512 even at MTU 517
                    self.bad_notifications += 1
                    raise AssertionError(f"notification {len(v)} > min(MTU-3, 512)")
                self.buf += v
                while len(self.buf) >= 8:
                    n = 8 + (self.buf[2] << 8 | self.buf[3])
                    if len(self.buf) < n:
                        break
                    frame, self.buf = self.buf[:n], self.buf[n:]
                    self.responses.put(frame)

    def connect(self, timeout=60.0):
        deadline = time.time() + timeout
        while time.time() < deadline:
            self.sock.sendto(b"\x01" + struct.pack("<H", self.mtu), self.addr)
            if self.connected.wait(0.2):
                return True
        return False

    def drop_link(self):
        self.sock.sendto(b"\x02", self.addr)
        self.connected.clear()
        self.buf = b""
        time.sleep(0.05)

    def drop_next(self, n=1):
        self.sock.sendto(b"\x05" + bytes([n]), self.addr)

    def auth(self, proven=True):
        """Test-server hook (--auth unproven): prove (or un-prove) this
        connection, as a token proof would."""
        self.sock.sendto(b"\x09" + bytes([1 if proven else 0]), self.addr)
        time.sleep(0.02)

    def press(self):
        """Test-server hook (--confirm): the user presses during the
        gadget's confirm prompt."""
        self.sock.sendto(b"\x0a", self.addr)
        time.sleep(0.02)

    def mem_fail(self, n=1):
        """host_server test hook: refuse the next n memory requests."""
        self.sock.sendto(b"\x08" + bytes([n]), self.addr)

    def vendor_takeover(self):
        self.sock.sendto(b"\x06", self.addr)

    def status(self):
        while not self.status_q.empty():
            self.status_q.get_nowait()
        self.sock.sendto(b"\x07", self.addr)
        return self.status_q.get(timeout=2)

    @property
    def max_write(self):
        """The write size a client uses: Android MTU - 3, iOS
        maximumWriteValueLength(.withoutResponse) = min(MTU-3, 512)."""
        return self.mtu - 3 if self.android else min(self.mtu - 3, self.ATT_MAX)

    def write(self, frag):
        assert len(frag) <= self.mtu - 3
        if len(frag) > self.ATT_MAX:
            raise SmpError(f"IllegalArgumentException: characteristic value of {len(frag)} bytes is over 512")
        self.sock.sendto(b"\x00" + frag, self.addr)

    def write_message(self, msg):
        step = self.max_write
        for i in range(0, len(msg), step):
            self.write(msg[i:i + step])


class SmpError(Exception):
    def __init__(self, msg, rc=None):
        super().__init__(msg)
        self.rc = rc


class Client:
    """Request/response by sequence number over a Link; responses decoded."""

    def __init__(self, link, indefinite=True, shuffle=False):
        self.link = link
        self.seq = 0
        self.indefinite = indefinite
        self.shuffle = shuffle
        self.pending = {}

    def frame(self, op, group, cmd, body):
        payload = cbor_encode(body, indefinite=self.indefinite, shuffle=self.shuffle) if isinstance(body, dict) else body
        seq = self.seq
        self.seq = (self.seq + 1) & 0xFF
        hdr = struct.pack(">BBHHBB", op, 0, len(payload), group, seq, cmd)
        return seq, hdr + payload

    def send(self, op, group, cmd, body):
        seq, msg = self.frame(op, group, cmd, body)
        self.pending[seq] = None
        self.link.write_message(msg)
        return seq

    def wait(self, seq, timeout):
        deadline = time.time() + timeout
        while self.pending.get(seq) is None:
            left = deadline - time.time()
            if left <= 0:
                raise SmpError("Request timed out")
            try:
                frame = self.link.responses.get(timeout=min(left, 0.1))
            except queue.Empty:
                continue
            s = frame[6]
            if s in self.pending:
                self.pending[s] = frame
        frame = self.pending.pop(seq)
        assert frame[0] & 7 in (1, 3), "not a response"
        return cbor_decode(frame[8:])

    def request(self, op, group, cmd, body, timeout=30.0):
        return self.wait(self.send(op, group, cmd, body), timeout)

    @staticmethod
    def rc(rsp):
        rc = rsp.get("rc")
        if isinstance(rsp.get("err"), dict):
            rc = rsp["err"].get("rc")
        return rc or 0

    def check(self, rsp, what):
        rc = self.rc(rsp)
        if rc:
            raise SmpError(f"{what} (error {rc})", rc)
        return rsp

    def params(self, timeout=3.0):
        try:
            r = self.request(0, 0, 6, {}, timeout)
        except SmpError:
            return None
        return r if r.get("buf_size", 0) > 0 else None

    def image_state(self):
        return self.check(self.request(0, 1, 0, {}), "Image list failed").get("images", [])

    def test(self, h):
        return self.check(self.request(2, 1, 0, {"hash": h, "confirm": False}), "Test image failed")

    def confirm(self, h=None):
        body = {"confirm": True} if h is None else {"hash": h, "confirm": True}
        return self.check(self.request(2, 1, 0, body), "Confirm failed")

    def reset(self):
        self.send(2, 0, 5, {})

    def echo(self, text):
        return self.request(2, 0, 0, {"d": text})


def chunk_size(offset, total, sha, capacity, indefinite):
    trial = {"off": offset, "len": total, "sha": sha, "data": b""} if offset == 0 else {"off": offset, "data": b""}
    overhead = 8 + len(cbor_encode(trial, indefinite=indefinite))
    available = capacity - overhead
    if available > 0xFFFF:
        return available - 4
    if available > 0xFF:
        return available - 2
    if available > 23:
        return available - 1
    return max(available, 1)


# ---- the two app flows ---------------------------------------------------------------------

class Result:
    def __init__(self):
        self.ok = False
        self.failure = None
        self.log = []
        self.upload_s = 0.0
        self.reconnect_s = None
        self.resumed_at = None
        self.rcs = []


class AndroidFlow:
    MAX_WINDOW = 3
    CHUNK_TIMEOUT = 5.0
    UPLOAD_TIMEOUT = 900.0
    RECONNECT_TIMEOUT = 60.0
    RESET_DISCONNECT_WAIT = 10.0

    def __init__(self, link, image, interrupt_at=None, on_progress=None):
        self.link = link
        link.android = True
        self.c = Client(link, indefinite=True)
        self.image = image
        self.hash = image_hash(image)
        self.interrupt_at = interrupt_at
        self.on_progress = on_progress

    def upload(self, r):
        image = self.image
        c = self.c
        params = c.params()
        capacity = params["buf_size"] if params else self.link.mtu - 3
        window = min(self.MAX_WINDOW, params.get("buf_count", 1) if params else 1)
        r.log.append(f"buf_size={capacity} window={window}")
        sha = hashlib.sha256(image).digest()
        in_flight = []
        acked = nxt = retries = 0
        t0 = time.time()
        while acked < len(image):
            if time.time() - t0 > self.UPLOAD_TIMEOUT:
                raise SmpError("Upload timed out")
            limit = 1 if acked == 0 else max(window, 1)
            while len(in_flight) < limit and nxt < len(image):
                end = min(nxt + chunk_size(nxt, len(image), sha, capacity, True), len(image))
                body = {"off": 0, "len": len(image), "sha": sha, "data": image[nxt:end]} if nxt == 0 else \
                    {"off": nxt, "data": image[nxt:end]}
                in_flight.append((end, c.send(2, 1, 1, body)))
                nxt = end
            end, seq = in_flight[0]
            try:
                rsp = c.wait(seq, self.CHUNK_TIMEOUT)
            except SmpError:
                retries += 1
                if retries > 3:
                    raise SmpError("Upload stalled")
                r.log.append(f"stalled at {acked}; resending")
                for _, s in in_flight:
                    c.pending.pop(s, None)
                in_flight = []
                nxt = acked
                continue
            in_flight.pop(0)
            rc = c.rc(rsp)
            r.rcs.append(rc)
            if rc:
                raise SmpError(f"Upload failed (error {rc})", rc)
            reported = rsp.get("off", 0)
            if reported > len(image):
                raise SmpError("offset past the end")
            offset = reported if reported > 0 else end
            if offset != end:
                for _, s in in_flight:
                    c.pending.pop(s, None)
                in_flight = []
                nxt = offset
                if acked == 0 and offset > end:
                    r.resumed_at = offset
            if offset > acked:
                retries = 0
            else:
                retries += 1
                if retries > 3:
                    raise SmpError("Upload stalled")
            acked = offset
            if self.on_progress:
                self.on_progress(acked)
            if self.interrupt_at and acked >= self.interrupt_at:
                self.interrupt_at = None
                raise InterruptedError(acked)
        r.upload_s = time.time() - t0

    def run(self):
        r = Result()
        try:
            self.upload(r)
            self.c.test(self.hash)
            self.link.disconnected.clear()
            self.c.reset()
            t_reset = time.time()
            if not self.link.disconnected.wait(self.RESET_DISCONNECT_WAIT):
                raise SmpError("NO_RESTART: no disconnect")
            if not self.link.connect(self.RECONNECT_TIMEOUT - (time.time() - t_reset)):
                raise SmpError("NO_RESTART: no reconnect")
            r.reconnect_s = time.time() - t_reset
            slots = self.c.image_state()
            active = next((s for s in slots if s.get("active") is True), None)
            if active is None:
                raise SmpError("UNKNOWN: no running image")
            if not active.get("hash"):
                raise SmpError("UNKNOWN: no hash")
            if active["hash"] != self.hash:
                r.failure = f"OLD_IMAGE_RUNNING: kept firmware {active.get('version')}"
                return r
            if active.get("confirmed") is not True:
                self.c.confirm()
            r.ok = True
        except SmpError as e:
            r.failure = str(e)
        return r


class IOSFlow:
    # the app: window 3, never more than the device's buf_count; 2 when it
    # reports none
    UPLOAD_WINDOW = 3
    UPLOAD_WINDOW_WITHOUT_BUFFER_COUNT = 2
    RESPONSE_TIMEOUT = 30.0
    RECONNECT_TIMEOUT = 60.0

    def __init__(self, link, image, interrupt_at=None, response_timeout=None):
        self.link = link
        link.android = False
        self.c = Client(link, indefinite=False, shuffle=True)
        self.image = image
        self.hash = image_hash(image)
        self.interrupt_at = interrupt_at
        self.timeout = response_timeout or self.RESPONSE_TIMEOUT

    def upload_once(self, r, buffer_size, buffer_count=None):
        image = self.image
        window = max(min(self.UPLOAD_WINDOW, buffer_count or self.UPLOAD_WINDOW_WITHOUT_BUFFER_COUNT), 1)
        c = self.c
        total = len(image)
        sha = hashlib.sha256(image).digest()
        write_len = self.link.max_write
        capacity = buffer_size or write_len
        nxt = acked = 0
        in_flight = []
        first = True
        while acked < total:
            while len(in_flight) < window and nxt < total:
                end = min(nxt + chunk_size(nxt, total, sha, capacity, False), total)
                body = {"off": nxt}
                if nxt == 0:
                    body["len"] = total
                    body["sha"] = sha
                body["data"] = image[nxt:end]
                in_flight.append((c.send(2, 1, 1, body), end))
                nxt = end
            seq, end = in_flight.pop(0)
            rsp = c.check(c.wait(seq, self.timeout), "Upload failed")
            acked = rsp.get("off", end)
            if first and acked > end:
                r.resumed_at = acked
            first = False
            if acked != end or not in_flight:
                for s, _ in in_flight:
                    rr = c.check(c.wait(s, self.timeout), "Upload failed")
                    acked = rr.get("off", acked)
                in_flight = []
                nxt = acked
            if self.interrupt_at and acked >= self.interrupt_at:
                self.interrupt_at = None
                raise InterruptedError(acked)

    def run(self):
        r = Result()
        c = self.c
        try:
            p = c.params()
            buffer_size = p.get("buf_size") if p else None
            buffer_count = p.get("buf_count") if p else None
            try:
                c.request(0, 1, 0, {}, self.timeout)
            except SmpError:
                pass
            time.sleep(0.1)
            t0 = time.time()
            for attempt in range(1, 4):
                try:
                    self.upload_once(r, buffer_size, buffer_count)
                    break
                except SmpError as e:
                    if "timed out" in str(e) and attempt < 3:
                        r.log.append(f"upload attempt {attempt} timed out, retrying")
                        self.link.buf = b""
                        time.sleep(2)
                        continue
                    raise
            r.upload_s = time.time() - t0
            c.test(self.hash)
            self.link.disconnected.clear()
            c.reset()
            t_reset = time.time()
            if not self.link.disconnected.wait(10):
                raise SmpError("no disconnect after the reset")
            if not self.link.connect(self.RECONNECT_TIMEOUT):
                raise SmpError("The gadget didn't reconnect after restarting.")
            r.reconnect_s = time.time() - t_reset
            slots = c.image_state()
            active = next((s for s in slots if s.get("active") is True), None)
            if active is None or not active.get("hash"):
                raise SmpError("no active hash")
            if active["hash"] != self.hash:
                r.failure = f"OLD_IMAGE_RUNNING: kept firmware {active.get('version')}"
                return r
            if active.get("confirmed") is not True:
                c.confirm()
            r.ok = True
        except SmpError as e:
            r.failure = str(e)
        return r
