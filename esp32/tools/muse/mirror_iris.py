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

"""The mirror's messages over ESP-Iris, on the board's USB data port (protocols/MIRROR.md)."""
import json
import os
import pathlib
import queue
import re
import secrets
import struct
import sys
import tempfile
import threading
import time
import zlib

SERVICE = 0x4D55
BOARD, STREAM, INPUT = 1, 2, 3
START, END = 0, 2
MARKER = 0x4D55              # the stream's quality: the board's pipe, not a screenshot
PIPE = dict(width=128, height=65535, format=1, quality=MARKER)
TILE = struct.Struct("<II")  # epoch, valid bytes
MESSAGE_MAX = 65536
# ESP-Iris (its protocol/spec.md): the frame header, a media description, and the types used here.
ENVELOPE = struct.Struct("<4sBBBBHHIIIII")
MEDIA = struct.Struct("<HHHHIHH")
CONTROL, SCREEN = 0, 3
HELLO, HELLO_ACK, CREDIT, REQUEST, RESPONSE, AUTH_RESULT, ERROR = 0x01, 0x02, 0x09, 0x10, 0x11, 0x16, 0x7F
MEDIA_DATA, MIRROR_START, MIRROR_STOP, MIRROR_STATE = 0x04, 0x06, 0x07, 0x08
LINK_ROLE = 0x17
CREDIT_GRANT = 128 * 1024    # topped up below half
DEVICE_PREFIX = "4553502d495249530100"   # "ESP-IRIS" 01 00; the device ID ends with the board's MAC


class Gap(ValueError):
    """Tiles went missing: the stream starts again with a new epoch."""


class Refused(ConnectionError):
    """The board refused a request; it may take the next one."""


class Pipe:
    """The board's byte pipe from SCREEN tiles: consecutive, the current epoch's only, cut into messages."""

    def __init__(self, description):
        _, _, _, self.height, self.stride, fmt, quality = MEDIA.unpack_from(description)
        if fmt != 1 or quality != MARKER or not TILE.size < self.stride <= 3840 or self.stride % 2 or not self.height:
            raise ValueError("the board's pipe has an unexpected description")
        self.frame, self.next_y = None, 0
        self.reset(0)

    def reset(self, epoch):
        self.epoch = epoch
        self.buffer = bytearray()

    def feed(self, body):
        """The messages one media DATA frame's tile completes, in order."""
        _, frame_id, dropped, _, size = struct.unpack_from("<QIIHH", body)
        x, y, _, height, stride, fmt, _ = MEDIA.unpack_from(body, 20)
        data = memoryview(body)[36:]
        if (x or stride != self.stride or fmt != 1 or not height or y + height > self.height
                or size != len(data) or len(data) != stride * height or dropped):
            raise Gap("a tile that does not fit the pipe")
        follows = (self.frame is None or frame_id == self.frame and y == self.next_y
                   or y == 0 and self.next_y in (0, self.height))
        self.frame, self.next_y = frame_id, y + height    # what follows this tile is in order again
        if not follows:
            raise Gap("tiles are missing")
        epoch, valid = TILE.unpack_from(data)
        if not 0 < valid <= len(data) - TILE.size:
            raise Gap("invalid tile length")
        if epoch != self.epoch:
            return []                         # the last viewer's, still on its way
        self.buffer += data[TILE.size:TILE.size + valid]
        messages = []
        while len(self.buffer) >= 4:
            n, = struct.unpack_from("<I", self.buffer)
            if not 0 < n <= MESSAGE_MAX:
                raise Gap("invalid message length")
            if len(self.buffer) < 4 + n:
                break
            messages.append(bytes(self.buffer[4:4 + n]))
            del self.buffer[:4 + n]
        return messages


def is_move(payload):
    return len(payload) == 5 and payload[0] == 1


def cobs_encode(data):
    out, code_at, code = bytearray(b"\0"), 0, 1
    for byte in data:
        if byte:
            out.append(byte)
            code += 1
        if not byte or code == 0xFF:
            out[code_at] = code
            code_at, code = len(out), 1
            out.append(0)
    out[code_at] = code
    return bytes(out)


def cobs_decode(data):
    out, at = bytearray(), 0
    while at < len(data):
        code = data[at]
        if not code or at + code > len(data):
            raise ValueError("invalid COBS")
        out += data[at + 1:at + code]
        at += code
        if code != 0xFF and at < len(data):
            out.append(0)
    return bytes(out)


def usb_boards():
    """ESP-Iris boards on this computer's USB as (device ID, data port), from descriptors only."""
    from serial.tools import list_ports
    ports = {}
    for p in list_ports.comports():
        if (p.vid, p.pid) == (0x303A, 0x4002) and re.fullmatch(r"[0-9a-fA-F]{12}", p.serial_number or ""):
            ports.setdefault(p.serial_number.lower(), []).append(p)
    # CDC0 is the console, CDC1 the data link: the higher USB interface, which pyserial ends the location with
    # on Linux and Windows; on macOS the port name carries it.
    def interface(p):
        m = re.search(r"[.:](\d+)$", p.location or "")
        return (int(m.group(1)) if m else -1, len(p.device), p.device)
    return [(DEVICE_PREFIX + serial, max(found, key=interface).device) for serial, found in sorted(ports.items())
            if len(found) == 2]


def gateway_running():
    """Whether an ESP-Iris Gateway runs on this computer: it may be installing firmware through a board's port,
    which then disappears and comes back. A Gateway holds its locks (ESP-Iris link.py EndpointLock) in the
    temporary directory's esp-iris-locks for as long as it runs; this probes them as its own held() does."""
    root = pathlib.Path(tempfile.gettempdir()) / "esp-iris-locks"
    for path in sorted(root.glob("*.lock")) if root.is_dir() else ():
        try:
            with path.open("r+b") as stream:
                if sys.platform == "win32":
                    import msvcrt
                    try:
                        msvcrt.locking(stream.fileno(), msvcrt.LK_NBLCK, 1)
                    except OSError:
                        return True
                    msvcrt.locking(stream.fileno(), msvcrt.LK_UNLCK, 1)
                else:
                    import fcntl
                    try:
                        fcntl.flock(stream.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
                    except BlockingIOError:
                        return True
                    fcntl.flock(stream.fileno(), fcntl.LOCK_UN)
        except OSError:
            continue
    return False


def open_data_port(name):
    """The port, locked as the Gateway locks it, so neither takes it while the other has it."""
    import serial
    return serial.Serial(name, timeout=0.1, exclusive=True if os.name == "posix" else None)


class IrisClient:
    """One viewer of one board, on its ESP-Iris data port. receive() runs on one thread, input() on another; a
    reader thread takes every frame: RPC answers wake their caller, screen tiles queue for receive()."""

    def __init__(self, port, fps=25, open_port=None):
        self.port, self.fps = port, fps
        self.open_port = open_port or open_data_port
        self.link = self.reader = None
        self.write_lock, self.lock = threading.Lock(), threading.Lock()
        self.session, self.sequence, self.request_id = 0, [0] * 10, 0
        self.waiting = {}                     # request ID -> [event, frame]
        self.tiles = queue.Queue(256)
        self.ready = threading.Event()
        self.stream = self.credit = 0
        self.started, self.epoch, self.pipe = False, 0, None
        self.messages, self.resyncs, self.broken = [], 0, None

    def send(self, channel, type_, payload=b"", request_id=0, stream_id=0):
        with self.write_lock:
            link = self.link              # cancel_read() may clear it from another thread
            if link is None:
                raise ConnectionError(self.broken or "closed")
            self.sequence[channel] = (self.sequence[channel] + 1) & 0xFFFFFFFF
            plain = ENVELOPE.pack(b"IRIS", 2, ENVELOPE.size, channel, type_, 0, 0, self.session, request_id,
                                  stream_id, self.sequence[channel], len(payload)) + payload
            link.write(cobs_encode(plain + struct.pack("<I", zlib.crc32(plain))) + b"\0")

    def request(self, channel, type_, payload=b"", stream_id=0, timeout=2):
        with self.lock:
            self.request_id = self.request_id % 0xFFFFFFFF + 1
            rid, slot = self.request_id, [threading.Event(), None]
            self.waiting[rid] = slot
        try:
            self.send(channel, type_, payload, rid, stream_id)
            if not slot[0].wait(timeout) or slot[1] is None:
                raise ConnectionError(self.broken or "the board did not answer over ESP-Iris")
        finally:
            with self.lock:
                self.waiting.pop(rid, None)
        if slot[1][1] == ERROR:
            raise Refused("the board refused an ESP-Iris request")
        return slot[1]

    def rpc(self, method, payload):
        _, _, _, body = self.request(CONTROL, REQUEST, struct.pack("<HHIHH", SERVICE, method, 1000, len(payload), 0)
                                     + payload)
        service, returned, result, size, _ = struct.unpack_from("<HHiHH", body)
        if (service, returned) != (SERVICE, method) or len(body) != 12 + size:
            raise ConnectionError("an invalid RPC answer over ESP-Iris")
        if result:
            raise Refused("the board refused the RPC (0x%08x)" % (result & 0xFFFFFFFF))
        return body[12:]

    def read(self):
        buffer = bytearray()
        try:
            while self.link is not None:
                link = self.link
                buffer += link.read(max(1, link.in_waiting))
                while (end := buffer.find(b"\0")) >= 0:
                    raw = bytes(buffer[:end])
                    del buffer[:end + 1]
                    self.dispatch(raw)
                if len(buffer) > 1 << 20:     # no frame end in a megabyte: not this protocol
                    buffer.clear()
        except (OSError, TypeError, AttributeError, ConnectionError):
            pass
        self.broken = self.broken or "the board's ESP-Iris port closed"
        with self.lock:
            for slot in self.waiting.values():
                slot[0].set()
        while True:                           # wake receive() even with the queue full
            try:
                self.tiles.put_nowait(None)
                break
            except queue.Full:
                try:
                    self.tiles.get_nowait()
                except queue.Empty:
                    pass

    def dispatch(self, raw):
        try:
            plain = cobs_decode(raw)
            magic, _, _, channel, type_, _, _, session, rid, stream, _, size = ENVELOPE.unpack_from(plain)
        except (ValueError, struct.error):
            return
        if magic != b"IRIS" or len(plain) != ENVELOPE.size + size + 4 \
                or zlib.crc32(plain[:-4]) != struct.unpack_from("<I", plain, len(plain) - 4)[0]:
            return
        body = plain[ENVELOPE.size:-4]
        if channel == CONTROL and type_ == HELLO and not self.ready.is_set():
            at, role = 0, None
            while at + 3 <= len(body):        # tag:u8, length:u16, value
                tag, length = body[at], struct.unpack_from("<H", body, at + 1)[0]
                if at + 3 + length > len(body):
                    break
                role = body[at + 3] if tag == LINK_ROLE and length == 1 else role
                at += 3 + length
            if role != 1:
                self.broken = "%s is not the board's ESP-Iris data port" % self.port
                return
            self.session = session
            self.send(CONTROL, HELLO_ACK, bytes([1]) + secrets.token_bytes(15) + b"\1")
        elif session != self.session:
            return
        elif channel == CONTROL and type_ == AUTH_RESULT:
            if body[:1] == b"\1":
                self.ready.set()
            else:
                self.broken = "the board refused this viewer's ESP-Iris session"
        elif rid and rid in self.waiting:
            with self.lock:
                slot = self.waiting.get(rid)
                if slot:
                    slot[1] = (channel, type_, stream, body)
                    slot[0].set()
        elif channel == SCREEN and type_ == MEDIA_DATA and stream == self.stream and len(body) >= 36:
            self.credit -= len(body)
            if self.credit < CREDIT_GRANT // 2:
                self.send(CONTROL, CREDIT, bytes([SCREEN, 0, 0, 0]) + struct.pack("<I", CREDIT_GRANT), stream_id=stream)
                self.credit += CREDIT_GRANT
            try:
                self.tiles.put_nowait(body)
            except queue.Full:
                pass                          # the pipe sees the gap and starts a new epoch

    def open(self):
        self.link = self.open_port(self.port)
        self.reader = threading.Thread(target=self.read, daemon=True)
        self.reader.start()
        if not self.ready.wait(5):
            raise ConnectionError(self.broken or "no ESP-Iris HELLO on %s" % self.port)
        board = json.loads(self.rpc(BOARD, b""))
        if not isinstance(board, dict) or board.get("iris_pipe") != 1:
            raise ValueError("this board's firmware has no Muse mirror over ESP-Iris")
        desc = MEDIA.pack(0, 0, PIPE["width"], PIPE["height"], 0, PIPE["format"], PIPE["quality"])
        _, type_, stream, body = self.request(SCREEN, MIRROR_START, desc + struct.pack("<HH", self.fps, 0))
        if type_ != MIRROR_STATE or not stream or len(body) < MEDIA.size:
            raise ConnectionError("the board did not start its ESP-Iris stream")
        self.stream, self.started = stream, True
        self.credit = CREDIT_GRANT
        self.send(CONTROL, CREDIT, bytes([SCREEN, 0, 0, 0]) + struct.pack("<I", CREDIT_GRANT), stream_id=stream)
        self.pipe = Pipe(body)
        self.restart()
        return board

    def restart(self):
        """A new epoch: the board takes this viewer afresh and sends 'B' first."""
        epoch = secrets.randbits(32) or 1
        if epoch == self.epoch:
            epoch = epoch % 0xffffffff + 1
        self.pipe.reset(epoch)
        self.messages = []
        self.epoch = epoch
        self.command(START, epoch)

    def command(self, op, epoch):
        wire = struct.pack("<BIII", op, epoch, 0, 0)
        if self.rpc(STREAM, wire) != wire:
            raise ConnectionError("the board's stream answer differs from the request")

    def receive(self, timeout=2):
        """One whole message, or None while the stream starts again."""
        if self.broken:
            raise ConnectionError(self.broken)
        if self.messages:
            return self.messages.pop(0)
        try:
            body = self.tiles.get(timeout=timeout)
            if body is None:
                raise ConnectionError(self.broken)
            self.messages = self.pipe.feed(body)
        except (queue.Empty, ValueError, struct.error):
            self.resyncs += 1
            if self.resyncs > 3:
                raise ConnectionError("no stream from the board over ESP-Iris")
            self.restart()
            return None
        if not self.messages:
            return None
        self.resyncs = 0
        return self.messages.pop(0)

    def input(self, payload):
        """One message to the board; the queue there may be full for a moment."""
        for attempt in range(3):
            wire = struct.pack("<I", self.epoch) + payload   # a restart in between brings a new epoch
            try:
                if self.rpc(INPUT, wire) == wire[:4]:
                    return
            except Refused:
                pass
            if is_move(payload):
                return                        # a newer position follows
            time.sleep(0.03)
        self.broken = "the board did not take input over ESP-Iris"
        raise ConnectionError(self.broken)

    def cancel_read(self):
        """Any thread: unblock receive() and input()."""
        self.broken = self.broken or "closed"
        link, self.link = self.link, None
        if link is not None:
            link.close()

    def close(self):
        """The receiving thread: tell the board this viewer left and stop the stream, within about a second."""
        try:
            if self.epoch:
                self.rpc(STREAM, struct.pack("<BIII", END, self.epoch, 0, 0))
            if self.started:
                self.request(SCREEN, MIRROR_STOP, stream_id=self.stream, timeout=1)
        except (OSError, ValueError, ConnectionError):
            pass
        finally:
            self.started, self.epoch = False, 0
            self.cancel_read()
            if self.reader is not None and self.reader is not threading.current_thread():
                self.reader.join(1)
