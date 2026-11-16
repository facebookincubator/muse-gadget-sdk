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

"""Muse's live screen and controls over the console cable.

Use --list for passive native USB discovery, or pass UART console ports explicitly.
Drag the device to move, resize at the corners. Ctrl/Cmd+S saves screenshots,
Ctrl/Cmd+R records, Ctrl/Cmd+0 shows actual size, Ctrl/Cmd+T floats on top.
The speaker and microphone icons route sound on a native USB console or a UART at 460800 baud or more.
Digits 1..4 press board buttons; arrows, Enter and Escape send navigation keys.
"""
import argparse
import errno
import importlib.util
import json
import math
import os
import queue
import re
import shutil
import struct
import subprocess
import sys
import threading
import time
import zlib

import numpy as np

MARK = b"\0MIRROR1\n"     # the board's console turned into the mirror's link
BREAK = b"\xff" * 4        # not a message length: ends a cable session another viewer left open
ROLES = {1: "talk", 2: "aux", 3: "power", 4: "speaker"}

# What a board looks like: (a fragment of the name the board reports, a profile).
# The first fragment found in the name, standing on its own ("1.75" is not in
# "1.75C"), picks the profile. Lengths in millimetres, from the makers' outline
# drawings, x to the right and y down from the body's top left corner.
#   screen_mm  the active area's width (the lit pixels; their count comes from the board)
#   body       shape="rect": w, h; x, y the active area's top left; r the corner radius
#              shape="round": d the diameter, the active area centred
#              finish: a name in FINISHES (the colours of the body and its buttons)
#   glass      border: the cover glass's dark border around the active area; r its corner radius
#   studs      optional: d the diameter of the corner holes, at their centres (x, y)
#   labels     optional: what is printed next to the key Muse uses in a role (1 talk, 2 aux)
#   extra      optional: keys that share a role with another: (its printing, the role, the edge, mm along it);
#              role None: a key the firmware cannot read (a reset), drawn but not pressed
#   photo      optional: file, a front view with the background cut out (PNG with alpha) in devices/ next to
#              this file, stretched to the body's w x h; screen the active area's x, y, w, h in its pixels.
#              A "face" key of a device drawn this way is a small button over that key in the picture.
# A board whose w x h lies the other way from the profile's active area gets the profile turned a quarter
# counter-clockwise (its bottom edge on the right). The buttons are the board's own, from its hints.
DEVICES = [('2.16',
  {'screen_mm': 38.99,
   'body': {'shape': 'rect', 'w': 44.6, 'h': 44.6, 'x': 2.8, 'y': 2.8, 'r': 5.5, 'finish': 'white'},
   'glass': {'border': 2.15, 'r': 4.5},
   'keys': [('key3', 1, 'top', 12.3, 5.3), ('pwr', 3, 'top', 22.3, 5.3), ('boot', 2, 'top', 32.3, 5.3)],
   'labels': {1: 'KEY3', 2: 'BOOT'},
   'extra': [('PWR', 1, 'top', 22.3)]}),
 ('3.49',
  {'screen_mm': 84.0,
   'body': {'shape': 'rect', 'w': 98.5, 'h': 34.8, 'x': 7.25, 'y': 6.11, 'r': 4.0, 'finish': 'blue'},
   'glass': {'border': 3.2, 'r': 2.5},
   'keys': [('boot', 1, 'bottom', 19.7, 5.0), ('pwr', 2, 'bottom', 29.6, 5.0)],
   'labels': {1: 'BOOT', 2: 'PWR'},
   'extra': [('RST', None, 'bottom', 39.4)]}),
 ('3.5',
  {'screen_mm': 73.44,
   'body': {'shape': 'rect', 'w': 95.11, 'h': 63.67, 'x': 10.84, 'y': 7.34, 'r': 7.3, 'finish': 'black'},
   'glass': {'border': 2.5, 'r': 4.0},
   'keys': [('pwr', 2, 'bottom', 23.8, 4.0), ('boot', 1, 'bottom', 42.8, 4.0)],
   'extra': [('RST', None, 'bottom', 33.3)],
   'labels': {1: 'BOOT', 2: 'PWR'},
   'hold': False}),
 ('1.75C',
  {'screen_mm': 43.76,
   'body': {'shape': 'round', 'd': 55.0, 'finish': 'silver'},
   'glass': {'border': 2.6, 'r': 0},
   'keys': [('key3', 1, 'angle', 50, 7.0), ('boot', 2, 'angle', 130, 3.5)],
   'labels': {1: 'PWR', 2: 'BOOT'}}),
 ('AIPI',
  {'screen_mm': 25.9,
   'body': {'shape': 'rect', 'w': 55.5, 'h': 47.5, 'x': 14.8, 'y': 5.0, 'r': 5.0, 'finish': 'red'},
   'studs': {'d': 2.4, 'at': ((6.0, 6.0), (49.5, 6.0), (6.0, 41.5), (49.5, 41.5))},
   'glass': {'border': 1.0, 'r': 1.0},
   'keys': [('pwr', 2, 'face', (17.6, 43.2), 6.2), ('boot', 1, 'face', (37.8, 43.2), 6.2)]}),
 ('SenseCAP Watcher',
  {'screen_mm': 36.83,
   'body': {'shape': 'rect', 'w': 65.5, 'h': 70.4, 'x': 14.4, 'y': 19.5, 'r': 8.0, 'finish': 'white'},
   'glass': {'border': 0, 'r': 0},
   'photo': {'file': 'sensecap-watcher.png', 'screen': (160, 217, 409, 409)},
   'keys': [('wheel', 1, 'face', (49.6, 12.3), 7.0)]})]
GENERIC = dict(screen_mm=None, body=dict(shape="rect", w=None, h=None, x=3.0, y=3.0, r=3.0, finish="white"),
               glass=dict(border=1.5, r=2.0))
GENERIC_ROUND = dict(screen_mm=None, body=dict(shape="round", d=None, x=3.0, finish="white"),
                     glass=dict(border=1.5, r=0))
FINISHES = {
    "white": dict(fill=(0xf2, 0xf2, 0xee), fill2=(0xe4, 0xe4, 0xdf), edge=(0xc4, 0xc4, 0xbe), label=(0x8a, 0x8a, 0x84),
                  key=((0xee, 0xee, 0xea), (0xc2, 0xc2, 0xbc)), key_down=((0xd4, 0xd4, 0xcf), (0xa8, 0xa8, 0xa2))),
    "silver": dict(fill=(0xdd, 0xdd, 0xe0), fill2=(0xa4, 0xa4, 0xaa), edge=(0x7a, 0x7a, 0x80), label=(0x50, 0x50, 0x56),
                   key=((0xe2, 0xe2, 0xe5), (0x9e, 0x9e, 0xa4)), key_down=((0xc0, 0xc0, 0xc4), (0x80, 0x80, 0x86))),
    "red": dict(fill=(0xe0, 0x30, 0x2e), fill2=(0xc4, 0x22, 0x22), edge=(0x8e, 0x16, 0x16), label=(0xff, 0xdc, 0xd8),
                key=((0x30, 0x30, 0x32), (0x18, 0x18, 0x1a)), key_down=((0x22, 0x22, 0x24), (0x10, 0x10, 0x12))),
    "blue": dict(fill=(0x2c, 0x9c, 0xf0), fill2=(0x1f, 0x7f, 0xcc), edge=(0x16, 0x5a, 0x96), label=(0xe6, 0xf2, 0xff),
                 key=((0x2c, 0x9c, 0xf0), (0x1f, 0x7f, 0xcc)), key_down=((0x1f, 0x7f, 0xcc), (0x16, 0x5a, 0x96))),
    "black": dict(fill=(0x2e, 0x2e, 0x30), fill2=(0x1c, 0x1c, 0x1e), edge=(0x0c, 0x0c, 0x0e), label=(0xa0, 0xa0, 0xa6),
                  key=((0x3a, 0x3a, 0x3c), (0x20, 0x20, 0x22)), key_down=((0x28, 0x28, 0x2a), (0x14, 0x14, 0x16))),
}

def device_profile(name):
    """The profile whose fragment stands on its own in the name ("1.75" is not "1.75C"), else GENERIC."""
    for fragment, profile in DEVICES:
        if re.search(r"(?<![0-9A-Za-z.])" + re.escape(fragment) + r"(?![0-9A-Za-z])", name or ""):
            return profile
    return GENERIC

def turned(prof, w, h):
    """The profile as the board runs its screen: a quarter turn counter-clockwise (its bottom edge on the
    right) when the board's w x h and the profile's active area lie the other way."""
    body = prof["body"]
    if not prof.get("screen_mm") or body["shape"] != "rect" or not body.get("w"):
        return prof
    inner = (body["w"] - 2 * body["x"]) / max(1e-6, body["h"] - 2 * body["y"])
    if not ((w / h > 1.15 and inner < 1 / 1.15) or (w / h < 1 / 1.15 and inner > 1.15)):
        return prof
    W, along = body["w"], prof["screen_mm"]          # the active width runs along the board's h
    nb = dict(body, w=body["h"], h=W, x=body["y"], y=W - body["x"] - along)
    out = dict(prof, body=nb, screen_mm=along * w / h, turned=True)
    if prof.get("studs"):
        out["studs"] = dict(prof["studs"], at=[(y, W - x) for x, y in prof["studs"]["at"]])
    if prof.get("keys"):
        keys = []
        for label, role, edge, pos, length in prof["keys"]:
            if edge == "face":
                pos = (pos[1], W - pos[0])
            elif edge == "angle":
                pos = (pos - 90) % 360
            else:
                if edge in ("top", "bottom"):
                    pos = W - pos
                edge = {"top": "left", "bottom": "right", "right": "top", "left": "bottom"}[edge]
            keys.append((label, role, edge, pos, length))
        out["keys"] = keys
    return out

def board_look(info):
    """The board's 'B' as the window draws it: sizes, buttons and its profile, turned to its screen."""
    w, h = max(1, int(info.get("w") or 1)), max(1, int(info.get("h") or 1))
    diag = float(info.get("diag") or 0)
    prof = device_profile(info.get("name"))
    if prof is GENERIC and info.get("round"):
        prof = GENERIC_ROUND
    return dict(name=info.get("name") or "", w=w, h=h, round=bool(info.get("round")),
                ppi=(min(w, h) if info.get("round") else math.hypot(w, h)) / diag if diag > 0 else 0,
                touch=bool(info.get("touch")), keyboard=bool(info.get("keyboard")),
                buttons=[b for b in info.get("buttons") or [] if isinstance(b, dict) and 1 <= b.get("role", 0) <= 10],
                profile=turned(prof, w, h))

def is_port(target):
    return target.startswith("/dev/") or re.fullmatch(r"COM\d+", target.removeprefix("\\\\.\\"), re.I) is not None

SYNC = b"\xa5\x5a"
CABLE_MAX = 65536

def cable_frame(msg):
    body = struct.pack('<I', len(msg)) + msg
    return SYNC + body + struct.pack('<I', zlib.crc32(body))

class ConsoleLog:
    def __init__(self, port, file=None):
        self.port, self.file = port, file
        self.pending = bytearray()

    def feed(self, data):
        self.pending += data
        while b'\n' in self.pending or len(self.pending) > 4096:
            end = self.pending.find(b'\n')
            end = end + 1 if 0 <= end < 4096 else min(4096, len(self.pending))
            raw = bytes(self.pending[:end])
            del self.pending[:end]
            line = '[%s %s] %s' % (time.strftime('%Y-%m-%d %H:%M:%S'), self.port,
                                   raw.decode('utf-8', errors='replace').rstrip('\r\n'))
            with _LOG_LOCK:
                print(line, flush=True)
                if self.file:
                    try:
                        with open(self.file, 'a', encoding='utf-8') as out:
                            out.write(line + '\n')
                    except OSError as error:
                        print('console log disabled (%s): %s' % (self.file, error), file=sys.stderr, flush=True)
                        self.file = None

    def flush(self):
        if self.pending:
            self.feed(b'\n')

_LOG_LOCK = threading.Lock()

class CableDemux:
    """Incremental bounded cable framing; a partial frame expires after the board's send deadline."""
    def __init__(self, on_text, on_error):
        self.buffer = bytearray()
        self.on_text, self.on_error = on_text, on_error
        self.since = None
        self.recovering = False
        self.fragment = bytearray()
        self.line_start = False

    def text(self, data):
        if not self.recovering:
            self.on_text(data)
        else:
            # Only complete text lines can be recovered from a damaged binary payload.
            self.fragment += data
            while b'\n' in self.fragment:
                line, _, rest = self.fragment.partition(b'\n')
                self.fragment = bytearray(rest)
                if self.line_start and line and all(c in (9, 13) or 32 <= c < 127 for c in line):
                    self.on_text(bytes(line) + b'\n')
                self.line_start = True
            if len(self.fragment) > 4096:
                self.fragment.clear()
                self.line_start = False

    def feed(self, data, now=None):
        now = time.monotonic() if now is None else now
        self.buffer += data
        messages = []
        while self.buffer:
            start = self.buffer.find(SYNC)
            if start < 0:
                n = len(self.buffer) - (self.buffer[-1:] == SYNC[:1])
                self.text(bytes(self.buffer[:n]))
                del self.buffer[:n]
                self.since = None
                break
            if start:
                self.text(bytes(self.buffer[:start]))
                del self.buffer[:start]
                self.since = None
            if self.since is None:
                self.since = now
            if len(self.buffer) < 6:
                if now - self.since <= 3:
                    break
                valid_length = False
            else:
                n, = struct.unpack_from('<I', self.buffer, 2)
                valid_length = 0 < n <= CABLE_MAX
            if valid_length:
                end = 6 + n
                if len(self.buffer) < end + 4 and now - self.since <= 3:
                    break
                if len(self.buffer) >= end + 4:
                    crc, = struct.unpack_from('<I', self.buffer, end)
                    if zlib.crc32(self.buffer[2:end]) == crc:
                        messages.append(bytes(self.buffer[6:end]))
                        del self.buffer[:end + 4]
                        self.since = None
                        self.recovering = False
                        self.fragment.clear()
                        self.line_start = False
                        continue
            if not self.recovering:
                self.on_error()
            self.recovering = True
            del self.buffer[:2]
            self.since = None
        return messages

def serial_open_error(port, error):
    message = "Cannot open %s: %s. Close idf.py monitor, serial terminals, or another viewer using this port." % (port, error)
    if sys.platform.startswith("linux") and (getattr(error, "errno", None) in (errno.EACCES, errno.EPERM)
                                              or "permission" in str(error).lower()):
        message += " Check the device's group with ls -l %s; join dialout (or uucp on your distribution), then log out and back in." % port
    elif getattr(error, "errno", None) in (errno.ENOENT, errno.ENODEV):
        message += " Reconnect the USB cable and select the board's current port."
    return ConnectionError(message)

def keep_serial_lines_on_close(ser):
    """Keep POSIX close from changing the USB control lines."""
    if os.name != "posix":
        return
    try:
        import termios
        fd = ser.fileno()
        settings = termios.tcgetattr(fd)
        settings[2] = settings[2] & ~termios.HUPCL
        termios.tcsetattr(fd, termios.TCSANOW, settings)
    except (ImportError, AttributeError, OSError):
        return

def open_usb_serial(port, baudrate=115200):
    """Open native USB Serial/JTAG without requesting a reset."""
    try:
        import serial
    except ImportError as e:
        raise ConnectionError("USB mirror needs pyserial: python3 -m pip install pyserial") from e
    ser = serial.Serial(port=None, baudrate=baudrate, timeout=0.5, write_timeout=2,
                        rtscts=False, dsrdtr=False, xonxoff=False, exclusive=True)
    try:
        # POSIX applies DTR before RTS: don't drop DTR while RTS may still be asserted.
        ser.dtr = True
        ser.rts = False
        ser.port = port
        ser.open()
        keep_serial_lines_on_close(ser)
        ser.rts = False
        ser.dtr = False
        ser.rts = False       # usbser.sys may defer DTR until the next RTS update.
        return ser
    except OSError as e:
        ser.close()
        raise serial_open_error(port, e) from e
    except BaseException:
        ser.close()
        raise

def usb_serial_mac(value):
    """Only a complete MAC; Windows composite-device instance IDs are not MACs."""
    value = (value or "").strip()
    if re.fullmatch(r"[0-9a-fA-F]{12}", value):
        digits = value.lower()
    elif re.fullmatch(r"[0-9a-fA-F]{2}([:_-])[0-9a-fA-F]{2}(?:\1[0-9a-fA-F]{2}){4}", value):
        digits = re.sub(r"[:_-]", "", value).lower()
    else:
        return ""
    return ":".join(digits[i:i + 2] for i in range(0, 12, 2))


def open_serial(port, wait=5.0, on_text=None, baudrate=115200, fast=0):
    """The board's console as the mirror's link: ">mirror", then everything after the marker is the protocol.
    With fast, a UART console is asked to run the session at that speed, and after its marker so do we."""
    ser = open_usb_serial(port, baudrate)
    ser.timeout = 0.1
    on_text = on_text or (lambda data: None)

    def marked(command, wait):
        ser.write(BREAK)
        time.sleep(0.5)              # longer than the pause the board waits for before it reads commands again
        on_text(ser.read(ser.in_waiting))
        ser.write(command)
        seen, end = bytearray(), time.monotonic() + wait
        while time.monotonic() <= end:
            seen += ser.read(1)
            if seen.endswith(MARK):
                on_text(bytes(seen[:-len(MARK)]))
                return True
            if len(seen) > len(MARK):
                on_text(bytes(seen[:-len(MARK)]))
                del seen[:-len(MARK)]
        on_text(bytes(seen))
        return False

    try:
        command = b"\n>mirror=%d\n" % fast if fast and fast != baudrate else b"\n>mirror\n"
        if not marked(command, wait):
            raise ConnectionError("No mirror reply on %s within %.1f s. Check that this is the board's console port, "
                                  "flash firmware built with CONFIG_MUSE_MIRROR=y, and close idf.py monitor or other viewers." % (port, wait))
        if command != b"\n>mirror\n":
            ser.baudrate = fast
        return ser
    except BaseException:
        ser.close()
        raise

ADPCM_INDEX = (-1, -1, -1, -1, 2, 4, 6, 8)
ADPCM_STEP = (7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97,
              107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724,
              796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026,
              4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500,
              20350, 22385, 24623, 27086, 29794, 32767)

def adpcm_decode(data):
    """One IMA ADPCM chunk as PCM16: the predictor (i16), the step index (u8), a zero, then a nibble a sample."""
    predictor, index = struct.unpack_from("<hB", data)
    if index > 88 or len(data) < 5:
        raise ConnectionError("bad audio message")
    out = []
    for byte in data[4:]:
        for code in (byte & 15, byte >> 4):
            step = ADPCM_STEP[index]
            delta = step >> 3
            if code & 4:
                delta += step
            if code & 2:
                delta += step >> 1
            if code & 1:
                delta += step >> 2
            predictor = max(-32768, min(32767, predictor - delta if code & 8 else predictor + delta))
            index = max(0, min(88, index + ADPCM_INDEX[code & 7]))
            out.append(predictor)
    return struct.pack("<%dh" % len(out), *out)

def adpcm_encode(pcm, state):
    """PCM16 as one IMA ADPCM chunk; state is [predictor, index] and carries over to the next chunk."""
    predictor, index = state
    out = bytearray(struct.pack("<hBB", predictor, index, 0))
    low = None
    for sample, in struct.iter_unpack("<h", pcm):
        step = ADPCM_STEP[index]
        diff, code, delta = sample - predictor, 0, step >> 3
        if diff < 0:
            code, diff = 8, -diff
        for bit in (4, 2, 1):
            if diff >= step:
                code |= bit
                diff -= step
                delta += step
            step >>= 1
        predictor = max(-32768, min(32767, predictor - delta if code & 8 else predictor + delta))
        index = max(0, min(88, index + ADPCM_INDEX[code & 7]))
        if low is None:
            low = code
        else:
            out.append(low | code << 4)
            low = None
    state[:] = predictor, index
    return bytes(out)

def inflate_message(msg):
    """The message inside a 'Z': raw deflate, never longer than a message may be."""
    z = zlib.decompressobj(-15)
    try:
        out = z.decompress(bytes(msg[1:]), 65537)
    except zlib.error as e:
        raise ConnectionError("bad deflated message") from e
    if not out or len(out) > 65536 or not z.eof or z.unused_data:
        raise ConnectionError("bad deflated message")
    return out

def usb_boards():
    """USB descriptors only; never open a console during discovery."""
    try:
        from serial.tools import list_ports
    except ImportError:
        print("USB discovery needs pyserial: python3 -m pip install pyserial", file=sys.stderr)
        return []
    return [(p.device, usb_serial_mac(p.serial_number)) for p in list_ports.comports() if is_board_port(p)]

def is_board_port(port):
    """Passive discovery is limited to native Espressif Serial/JTAG."""
    return (port.vid, port.pid) == (0x303a, 0x1001)

def native_usb(device):
    """The chip's own USB: no bridge ring to overrun, and its speed is not the nominal baud."""
    try:
        from serial.tools import list_ports
    except ImportError:
        return False
    # The name pyserial lists: macOS lists cu., Linux resolves by-id links, Windows COM names ignore case.
    name = os.path.realpath(device).replace("/dev/tty.", "/dev/cu.", 1) if os.name == "posix" else \
        device.removeprefix("\\\\.\\").upper()
    return any(p.device == name and is_board_port(p) for p in list_ports.comports())

def apply_frame(fb, msg, seen=None):
    """One 'F' message onto the screen copy; returns the screen state (0 bright, 2 dark).
    seen, a bool array the screen's size, gets every rectangle drawn marked.
    Raises ValueError on a message that doesn't add up, or a coding this viewer doesn't know."""
    if len(msg) < 8 or msg[:1] != b"F":
        raise ValueError("short frame")
    state, rects, w, h = msg[1], *struct.unpack_from("<HHH", msg, 2)
    if (h, w) != fb.shape:
        raise ValueError("frame size %dx%d, screen %dx%d" % (w, h, fb.shape[1], fb.shape[0]))
    off = 8
    for _ in range(rects):
        if off + 14 > len(msg):
            raise ValueError("short rectangle header")
        x, y, rw, rh, enc, _pad, n = struct.unpack_from("<HHHHBBI", msg, off)
        off += 14
        data = msg[off:off + n]
        off += n
        if _pad or len(data) != n or not rw or not rh or x + rw > w or y + rh > h:
            raise ValueError("bad rectangle")
        if enc == 0:
            if n != rw * rh * 2:
                raise ValueError("bad raw size")
            px = np.frombuffer(data, "<u2").reshape(rh, rw)
        elif enc in (1, 2):
            if n % 4:
                raise ValueError("bad runs")
            pairs = np.frombuffer(data, "<u2").reshape(-1, 2)
            counts = pairs[:, 0].astype(np.int64)
            if not counts.all() or int(counts.sum()) != rw * rh:     # before np.repeat allocates by them
                raise ValueError("runs don't fill the rectangle")
            flat = np.repeat(pairs[:, 1], counts)
            px = flat.reshape(rw, rh).T if enc == 2 else flat.reshape(rh, rw)
        else:
            raise ValueError("unknown coding %d" % enc)
        fb[y:y + rh, x:x + rw] = px
        if seen is not None:
            seen[y:y + rh, x:x + rw] = True
    if off != len(msg):
        raise ValueError("trailing frame bytes")
    return state

TEXT_EDITS = {"Backspace": 1, "Delete": 2, "Left": 3, "Right": 4, "Enter": 5}


def text_messages(text):
    """'I' messages of whole UTF-8 characters, 63 bytes of text at most each."""
    chunk = bytearray(b"I")
    for char in text:
        if char == "\0":
            continue
        encoded = char.encode("utf-8")
        if len(chunk) + len(encoded) > 64:
            yield bytes(chunk)
            chunk = bytearray(b"I")
        chunk.extend(encoded)
    if len(chunk) > 1:
        yield bytes(chunk)


def route_text_key(link, key, text="", *, paste=None, modified=False):
    """True when a field open on the board took the key; nothing typed goes to a log."""
    if not link.text_open:
        return False
    if paste is not None:
        for msg in text_messages(paste):
            link.send(msg)
        return True
    if modified:
        return False
    if key in TEXT_EDITS:
        link.send(bytes((ord("K"), TEXT_EDITS[key])))
        return True
    if key == "Escape":
        return True       # LVGL has no generic back action for a field; it stays with the field
    if text and all(c.isprintable() for c in text):
        for msg in text_messages(text):
            link.send(msg)
        return True
    return False


class Link:
    """One board's connection, on a thread of its own; it reconnects until closed. on_board(info),
    on_frame() and on_status(text) are called on that thread; status "" means live."""

    def __init__(self, host, port=115200, log_file=None, fast=0):
        self.host, self.port = host, port
        self.native = native_usb(host)      # the chip's own USB: no line speed, room for PCM both ways
        self.fast = 0 if self.native else fast
        self.logs = ConsoleLog(host, log_file)
        self.demux = None
        self.incoming = []
        self.fb = None
        self.have_board = False
        self.text_open = False
        self.seen = None            # pixels drawn since 'B': the board sends its whole screen first, in bands
        self.state = 0
        self.changes = 0            # frames that changed pixels
        self.held = False           # pixels taken in but not shown: the rest of their screen follows
        self.damaged = 0            # cable frames that failed their CRC
        self.live = False
        self.audio_capable = False  # the board sends its sound and takes the viewer's microphone
        self.audio = queue.Queue(maxsize=8)
        self.lock = threading.Lock()
        self.out = queue.Queue()
        self.alive = True
        self.stopped = threading.Event()
        self.tx_lock = threading.Lock()
        self.sock = None
        self.thread = self.writer_thread = None
        self.on_board = self.on_frame = self.on_status = lambda *a: None

    def start(self):
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.thread.start()
        return self

    def send(self, payload):
        self.out.put(payload)

    def pointer(self, x, y, down):
        self.send(struct.pack("<BHH", 1 if down else 0, x, y))

    def tap(self, role):
        self.send(bytes([3, role]))

    def key(self, role, down):
        self.send(bytes([6, role, 1 if down else 0]))

    def close(self):
        """Stop workers before releasing the serial handle."""
        self.alive = False
        self.live = False
        self.stopped.set()
        try:
            with self.tx_lock:
                if self.sock is not None and hasattr(self.sock, 'cancel_read'):
                    self.sock.cancel_read()
        except OSError:
            pass
        current = threading.current_thread()
        if current in (self.thread, self.writer_thread):
            return
        for worker in (self.thread, self.writer_thread):
            if worker is not None and worker.ident is not None:
                worker.join()
        self.disconnect()

    def disconnect(self):
        self.text_open = False
        with self.tx_lock:
            sock, self.sock = self.sock, None
            if sock:
                try:
                    sock.write(BREAK)
                except OSError:
                    pass
                finally:
                    try:
                        sock.close()
                    except OSError:
                        pass

    def status(self, text):
        if self.stopped.is_set():
            return
        self.live = not text
        self.on_status(text)

    def connect(self):
        self.have_board = False
        self.audio_capable = False
        self.incoming = []
        self.demux = CableDemux(self.logs.feed, self.refresh)
        return open_serial(self.host, on_text=self.logs.feed, baudrate=self.port, fast=self.fast)

    def refresh(self):
        self.damaged += 1
        with self.lock:
            if self.seen is not None:
                self.seen.fill(False)
        self.send(b'R')

    def receive(self):
        until = time.monotonic() + 6
        while self.alive:
            if self.incoming:
                return self.incoming.pop(0)
            sock = self.sock
            if sock is None:
                raise ConnectionError("closed")
            data = sock.read(max(1, min(sock.in_waiting, 8192)))
            self.incoming.extend(self.demux.feed(data))
            if time.monotonic() > until:
                raise ConnectionError("no valid cable frame within 6 seconds")
        raise ConnectionError("closed")

    def writer(self, sock):
        last = time.monotonic()
        while self.alive and self.sock is sock:
            try:
                payload = self.out.get(timeout=.1)
            except queue.Empty:
                payload = b"\x02" if time.monotonic() - last > 2 else None   # keepalive, well inside 6 s
            if payload is None:
                continue
            try:
                data = struct.pack("<I", len(payload)) + payload
                with self.tx_lock:
                    if not self.alive or self.sock is not sock:
                        return
                    # Bridge RX rings are small; pace each chunk at the session's wire rate.
                    rate = getattr(sock, "baudrate", None) or self.port
                    for at in range(0, len(data), 64):
                        chunk = data[at:at + 64]
                        if sock.write(chunk) != len(chunk):
                            raise ConnectionError("short serial write")
                        if not self.native and len(data) > 64:
                            time.sleep(len(chunk) * 10 / rate)
                last = time.monotonic()
            except OSError:
                return

    def run(self):
        while self.alive:
            writer = sock = None
            try:
                self.status("connecting to %s" % self.host)
                if not self.alive:
                    break
                sock = self.connect()
                with self.tx_lock:
                    self.sock = sock
                    if not self.alive:
                        break
                while not self.out.empty():
                    self.out.get_nowait()
                writer = self.writer_thread = threading.Thread(target=self.writer, args=(sock,), daemon=True)
                writer.start()
                while self.alive:
                    msg = self.receive()
                    if not self.alive:
                        break
                    kind = msg[:1]
                    if kind == b"Z":                      # a message deflated by the board, asked for with 'Z'
                        msg = inflate_message(msg)
                        kind = msg[:1]
                    if kind == b"B":
                        info = json.loads(msg[1:])
                        if not (isinstance(info, dict) and all(type(info.get(k)) is int and 0 < info[k] <= 4096
                                                               for k in ("w", "h"))):
                            raise ConnectionError("a board description without a usable screen size")
                        with self.lock:
                            self.fb = np.zeros((info["h"], info["w"]), "<u2")
                            self.seen = np.zeros((info["h"], info["w"]), bool)
                        self.text_open = False
                        self.audio_capable = bool(info.get("audio"))
                        self.send(b"O")
                        self.send(b"Z")
                        self.have_board = True
                        self.on_board(info)
                        self.status("")
                    elif kind == b"F":
                        if not self.have_board:
                            raise ConnectionError("frame before board geometry")
                        with self.lock:
                            was, self.state = self.state, apply_frame(self.fb, msg, self.seen) & 0x7F
                            rects = struct.unpack_from("<H", msg, 2)[0]
                            self.changes += 1 if rects else 0
                            self.held |= bool(rects)
                        # Bit 7: more of this screen follows; it shows once whole.
                        if not msg[1] & 0x80 and (self.held or self.state != was):
                            self.held = False
                            self.on_frame()
                    elif kind == b"O" and len(msg) == 2 and msg[1] <= 1:
                        self.text_open = bool(msg[1])
                    elif kind in (b"A", b"a"):
                        if len(msg) < 4 or len(msg) > 1282 or len(msg) % 2 or msg[1] not in (1, 2):
                            raise ConnectionError("bad audio message")
                        if kind == b"a":                  # ADPCM from a slow cable
                            msg = msg[:2] + adpcm_decode(msg[2:])
                        if self.audio.full():             # nobody plays it: keep the newest
                            try:
                                self.audio.get_nowait()
                            except queue.Empty:
                                pass
                        self.audio.put((msg[1], msg[2:]))
                    elif kind == b"E":                    # the board ended the cable session: ask again
                        self.status("cable session ended, reconnecting")
                        break
            except (OSError, ValueError, ConnectionError, struct.error, json.JSONDecodeError) as e:
                if self.fast and not self.have_board and getattr(sock, "baudrate", None) == self.fast:
                    self.fast = 230400 if self.fast > 230400 else 0   # the bridge did not take that speed
                if self.alive:
                    self.status("%s: %s, retrying" % (self.host, e))
            finally:
                self.disconnect()
                if writer is not None:
                    writer.join()
                self.logs.flush()
            if self.alive:
                self.stopped.wait(2)

def rgb565_to_rgb(fb):
    p = fb.astype(np.uint32)
    r, g, b = (p >> 11) & 31, (p >> 5) & 63, p & 31
    out = np.empty(fb.shape + (3,), np.uint8)
    out[..., 0] = (r * 527 + 23) >> 6
    out[..., 1] = (g * 259 + 33) >> 6
    out[..., 2] = (b * 527 + 23) >> 6
    return out

def png_write(path, rgb):
    """A PNG from h x w x 3 bytes, without Pillow or Qt."""
    h, w = rgb.shape[:2]
    raw = b"".join(b"\x00" + rgb[y].tobytes() for y in range(h))

    def chunk(tag, body):
        return struct.pack(">I", len(body)) + tag + body + struct.pack(">I", zlib.crc32(tag + body) & 0xFFFFFFFF)

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
                chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))

def desktop_path(ext, tag="", stamp=None):
    home = os.path.expanduser("~")
    try:
        from PySide6.QtCore import QStandardPaths
    except ImportError:
        folder = os.path.join(home, "Desktop")
    else:
        folder = QStandardPaths.writableLocation(QStandardPaths.StandardLocation.DesktopLocation)
    stamp = stamp or time.strftime("%Y%m%d-%H%M%S")
    return os.path.join(folder if os.path.isdir(folder) else home, "mirror-%s%s.%s" % (stamp, tag, ext))

def save_png(link, path, timeout=15):
    """The screen as a PNG once every pixel of it has arrived."""
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        time.sleep(0.05)
        with link.lock:
            rgb = rgb565_to_rgb(link.fb) if link.seen is not None and link.seen.all() else None
        if rgb is not None:
            png_write(path, rgb)
            print("saved", path)
            return 0
    print("no whole screen within %d s" % timeout, file=sys.stderr)
    return 1

# ---------------------------------------------------------------------------------------------------------------
# The window. Everything below needs PySide6; the protocol above does not.

def run_window(targets, scale, on_top, wall, log_file=None, fast=None):
    from PySide6.QtCore import QEvent, QObject, QPointF, QRectF, Qt, QTimer, Signal
    from PySide6.QtGui import (QAction, QBrush, QColor, QCursor, QFont, QFontMetricsF, QGuiApplication, QIcon,
                               QImage, QKeySequence, QLinearGradient, QPainter, QPainterPath, QPen, QPixmap,
                               QPolygonF, QTransform)
    from PySide6.QtWidgets import QApplication, QMenu, QMenuBar, QToolTip, QWidget

    BUTTON = 34
    BUTTONS = ("move", "talk", "listen", "rec", "shot", "close")
    CORNER = 24                      # points from a corner of the devices' area that resize the window
    GAP = 28                         # between boards side by side
    MIN_GLASS = 120                  # points: the screen is never drawn narrower, a tiny device is no use
    KEY_MM = 6.0                     # a button: .75 of this along the edge, .3 out of it
    TOUCH = (150, 120, 255)          # the accent: a held button's name; a ring where the mouse touches the glass
    TOUCH_S = 0.6                    # the ring fades in this many seconds
    WHEEL_SLOP = 16                  # board pixels a two-finger touch moves before it lands: past LVGL's scroll limit
    KEY_ROLES = {Qt.Key.Key_1: 1, Qt.Key.Key_2: 2, Qt.Key.Key_3: 3, Qt.Key.Key_4: 4, Qt.Key.Key_Up: 5,
                 Qt.Key.Key_Down: 6, Qt.Key.Key_Left: 7, Qt.Key.Key_Right: 8, Qt.Key.Key_Return: 9,
                 Qt.Key.Key_Enter: 9, Qt.Key.Key_Escape: 10}
    AA = QPainter.RenderHint.Antialiasing
    SMOOTH = QPainter.RenderHint.SmoothPixmapTransform
    CENTER = int(Qt.AlignmentFlag.AlignCenter)
    NOPEN = Qt.PenStyle.NoPen

    app = QApplication.instance() or QApplication(sys.argv[:1])
    app.setApplicationName("Muse mirror")

    def rgb(r, g, b, a=1.0):
        c = QColor(r, g, b)
        c.setAlphaF(a)
        return c

    def font(px):
        f = QFont()
        f.setPixelSize(max(1, int(round(px))))
        return f

    def rounded(r, radius):
        path = QPainterPath()
        if radius > 0:
            path.addRoundedRect(QRectF(*r), radius, radius)
        else:
            path.addRect(QRectF(*r))
        return path

    def grad_fill(p, path, angle, c1, c2):
        """A linear gradient across the path's bounds, from c1 to c2 along the angle in degrees:
        -60 runs from the lower left to the upper right."""
        b = path.boundingRect()
        a = math.radians(angle)
        dx, dy = math.cos(a), math.sin(a)
        ext = (abs(b.width() * dx) + abs(b.height() * dy)) / 2
        c = b.center()
        g = QLinearGradient(c.x() - dx * ext, c.y() - dy * ext, c.x() + dx * ext, c.y() + dy * ext)
        g.setColorAt(0, c1)
        g.setColorAt(1, c2)
        p.fillPath(path, QBrush(g))

    def pen(c, width):
        q = QPen(c, width)
        q.setCapStyle(Qt.PenCapStyle.FlatCap)
        q.setJoinStyle(Qt.PenJoinStyle.MiterJoin)
        return q

    photos, scaled = {}, {}

    def photo_image(prof):
        """The profile's picture, the screen's rectangle in it and where its four corners are drawn (the first
        opaque pixel on each diagonal, as fractions of the picture), or None (no picture, or the profile turned)."""
        spec = prof.get("photo")
        if not spec or prof.get("turned"):
            return None
        if spec["file"] not in photos:
            img = QImage(os.path.join(os.path.dirname(os.path.abspath(__file__)), "devices", spec["file"]))
            if img.isNull():
                photos[spec["file"]] = None
            else:
                w, h, corners = img.width(), img.height(), {}
                for sx in (-1, 1):
                    for sy in (-1, 1):
                        x0, y0 = (0 if sx < 0 else w - 1), (0 if sy < 0 else h - 1)
                        k = next((k for k in range(min(w, h) // 2)
                                  if img.pixelColor(x0 - sx * k, y0 - sy * k).alpha() >= 128), 0)
                        corners[(sx, sy)] = ((x0 - sx * k + .5) / w, (y0 - sy * k + .5) / h)
                photos[spec["file"]] = (img, tuple(spec["screen"]), corners)
        return photos[spec["file"]]

    def scaled_photo(img, w, h, dpr):
        key = (img.cacheKey(), round(w * dpr), round(h * dpr))
        if key not in scaled:
            if len(scaled) > 16:
                scaled.clear()
            out = img.scaled(max(1, key[1]), max(1, key[2]), Qt.AspectRatioMode.IgnoreAspectRatio,
                             Qt.TransformationMode.SmoothTransformation)
            out.setDevicePixelRatio(dpr)
            scaled[key] = out
        return scaled[key]

    # -- the device's parts ---------------------------------------------------------------------------------
    def device_layout(look, s, ox, oy):
        """The device at s points per millimetre with its body's top left at (ox, oy): the body, the glass, the
        active area, the studs, and the buttons as shapes (centre, rotation in degrees where 0 is a
        vertical bar on the right edge, thickness w, length h, the outward direction, and where the label goes)."""
        prof, cw, ch = look["profile"], look["w"], look["h"]
        body, glass = prof["body"], prof["glass"]
        screen_mm = prof["screen_mm"] or (cw / look["ppi"] * 25.4 if look["ppi"] else 40.0)
        mm = screen_mm / cw                                   # millimetres per screen pixel
        aw, ah = cw * mm * s, ch * mm * s
        disc = body["shape"] == "round"
        if disc:
            d = (body["d"] or max(cw, ch) * mm + 2 * body["x"]) * s
            brect, body_r = (ox, oy, d, d), d / 2
            ax, ay = ox + (d - aw) / 2, oy + (d - ah) / 2
        else:
            bw = (body["w"] or cw * mm + 2 * body["x"]) * s
            bh = (body["h"] or ch * mm + 2 * body["y"]) * s
            brect, body_r = (ox, oy, bw, bh), body["r"] * s
            ax, ay = ox + body["x"] * s, oy + body["y"] * s
        photo = photo_image(prof)
        if photo:                                             # the screen goes where the picture has it
            img, (px, py, pw, ph), _ = photo
            fx, fy = brect[2] / img.width(), brect[3] / img.height()
            ax, ay, aw, ah = ox + px * fx, oy + py * fy, pw * fx, ph * fy
            if ah / aw > ch / cw:                             # the board's pixels keep their proportions
                ay, ah = ay + (ah - aw * ch / cw) / 2, aw * ch / cw
            else:
                ax, aw = ax + (aw - ah * cw / ch) / 2, ah * cw / ch
        g = glass["border"] * s
        grect = (ax - g, ay - g, aw + 2 * g, ah + 2 * g)
        glass_r = (aw + 2 * g) / 2 if look["round"] else glass["r"] * s
        active_r = aw / 2 if look["round"] else 0
        keys = place_keys(look, brect, body_r, disc, (ax, ay, aw, ah), s)
        studs = [(brect[0] + x * s, brect[1] + y * s, prof["studs"]["d"] * s)
                 for x, y in (prof.get("studs") or {}).get("at", ())]
        L = dict(body=brect, body_r=body_r, glass=grect, glass_r=glass_r, active=(ax, ay, aw, ah),
                 active_r=active_r, keys=keys, s=s, studs=studs, photo=photo[0] if photo else None,
                 corners={k: (brect[0] + fx_ * brect[2], brect[1] + fy_ * brect[3])
                          for k, (fx_, fy_) in photo[2].items()} if photo else None)
        bx, by, bw, bh = brect
        reach = [key_path(k, False).boundingRect() for k in keys]
        L["over"] = (max([bx - r.left() for r in reach] + [0]), max([by - r.top() for r in reach] + [0]),
                     max([r.right() - bx - bw for r in reach] + [0]), max([r.bottom() - by - bh for r in reach] + [0]))
        return L

    def place_keys(look, brect, body_r, disc, active, s):
        """Each button on the body's edge nearest its hint, at the hint's place along it (on the rim, at its
        angle, for a disc); buttons without a hint along the bottom."""
        bx, by, bw, bh = brect
        ax, ay, aw, ah = active
        cw, ch = look["w"], look["h"]
        cx, cy = bx + bw / 2, by + bh / 2
        T, Lk = KEY_MM * s * .3, KEY_MM * s * .75
        edges = {"right": [], "left": [], "top": [], "bottom": []}
        rim, loose, face = [], [], []
        defaults = {k[1]: k for k in look["profile"].get("keys", [])}
        printed = look["profile"].get("labels", {})

        def caption(b, fallback=""):
            """The key's own name on a known device; else the word the board's captions use for it."""
            return str(b.get("label") or printed.get(b["role"]) or b.get("name") or fallback
                       or ROLES.get(b["role"], ""))
        def placed(b):
            x, y = b.get("x", -1), b.get("y", -1)
            return isinstance(x, (int, float)) and isinstance(y, (int, float)) and x >= 0 and y >= 0

        shown = {b.get("name") for b in look["buttons"] if placed(b) and b.get("name")}
        for b in look["buttons"]:
            x, y = b.get("x", -1), b.get("y", -1)
            key = defaults.get(b["role"])
            pictured = key and key[2] == "face" and look["profile"].get("photo")   # where the picture has it
            if not placed(b) or pictured:
                if not placed(b) and b.get("name") in shown:  # one key with two roles (the Watcher's wheel,
                    continue                                   # the P4's PWR)
                if not placed(b) and b.get("name"):
                    shown.add(b["name"])
                if key:
                    label, role, edge, pos, diameter = key
                    if edge == "angle":                    # as long as the key, the board's longest drawn as usual
                        rim.append([pos, b, Lk * diameter / max(k[4] for k in defaults.values())])
                    elif edge == "face":
                        face.append(dict(cx=bx + pos[0] * s, cy=by + pos[1] * s, rot=0, dx=0, dy=1, role=role, label=caption(b, label),
                                         w=diameter * s, h=diameter * s, face=True, pictured=bool(pictured)))
                    else:
                        edges[edge].append([pos * s, b])
                else:
                    loose.append(b)
                continue
            hx, hy = ax + min(x + .5, cw) / cw * aw, ay + min(y + .5, ch) / ch * ah
            if disc:
                rim.append([math.degrees(math.atan2(hx - cx, cy - hy)) % 360, b, Lk])
                continue
            # the screen edge the hint hugs, not the nearest body edge: bezels differ (the AIPI's screen sits
            # high, its buttons below it would land on the sides)
            edge = min((ax + aw - hx, "right"), (hx - ax, "left"), (ay + ah - hy, "bottom"), (hy - ay, "top"))[1]
            edges[edge].append([hy - by if edge in ("left", "right") else hx - bx, b])
        for label, role, edge, pos in look["profile"].get("extra", ()):
            if role is None or any(b["role"] == role for b in look["buttons"]):
                edges[edge].append([pos * s, dict(role=role, label=label)])
        for i, b in enumerate(loose):
            off = i - (len(loose) - 1) / 2
            if disc:
                rim.append([(180 - off * 20) % 360, b, Lk])
            else:
                edges["bottom"].append([bw / 2 + off * Lk * 1.6, b])
        keys = face
        for edge, items in edges.items():
            if not items:
                continue
            length = bh if edge in ("left", "right") else bw
            lo, hi = body_r + Lk / 2, length - body_r - Lk / 2
            if lo > hi:
                lo = hi = length / 2
            items.sort(key=lambda it: it[0])
            for j, it in enumerate(items):                  # keep them apart, inside the straight part
                it[0] = min(max(it[0], lo), hi)
                if j:
                    it[0] = max(it[0], items[j - 1][0] + Lk * 1.3)
            over = items[-1][0] - hi
            for it in items:
                it[0] -= max(0, over)
            for pos, b in items:
                if edge == "right":
                    k = dict(cx=bx + bw + T / 2 - 1, cy=by + pos, rot=0, dx=1, dy=0)
                elif edge == "left":
                    k = dict(cx=bx - T / 2 + 1, cy=by + pos, rot=0, dx=-1, dy=0)
                elif edge == "top":
                    k = dict(cx=bx + pos, cy=by - T / 2 + 1, rot=90, dx=0, dy=-1)
                else:
                    k = dict(cx=bx + pos, cy=by + bh + T / 2 - 1, rot=90, dx=0, dy=1)
                keys.append(dict(k, role=b["role"], label=caption(b), w=T, h=Lk))
        if rim:
            rr = body_r + T / 2 - 1
            step = math.degrees(Lk * 1.3 / max(1.0, rr))
            rim.sort(key=lambda it: it[0])
            for j, it in enumerate(rim):
                if j:
                    it[0] = max(it[0], rim[j - 1][0] + step)
            for angle, b, length in rim:
                a = math.radians(angle)
                keys.append(dict(cx=cx + rr * math.sin(a), cy=cy - rr * math.cos(a), rot=angle - 90,
                                 dx=math.sin(a), dy=-math.cos(a), role=b["role"],
                                 label=caption(b), w=T, h=length))
        for k in keys:            # the name on the button, reading along the edge; above a button on the front
            if k.get("face"):
                size = max(6.0, min(12.0, 1.6 * s))
                off = k["h"] / 2 + size * 1.2
            else:                                             # the button grows to carry its name
                size = max(6.0, min(9.0, 1.3 * s))
                w = max(k["w"], size * 1.7)
                k["cx"] += k["dx"] * (w - k["w"]) / 2
                k["cy"] += k["dy"] * (w - k["w"]) / 2
                k["w"], k["h"] = w, max(k["h"], len(k["label"]) * size * .62 + size)
                off = 0.0
            turn = math.degrees(math.atan2(-k["dx"], k["dy"]))
            turn = turn - 180 if turn > 90.5 else turn + 180 if turn < -90.5 else turn
            k.update(lx=k["cx"] - k["dx"] * off, ly=k["cy"] - k["dy"] * off, lrot=turn, lsize=size)
        return keys

    def key_path(k, pushed):
        """The button as a path; pushed in, it is half as thick and sits closer to the body."""
        w = k["w"] * (.5 if pushed else 1)
        path = QPainterPath()
        rad = min(2.5, w / 2)
        if k.get("face"):
            path.addEllipse(QRectF(-k["w"] / 2, -k["h"] / 2, k["w"], k["h"]))
        else:
            path.addRoundedRect(QRectF(-w / 2, -k["h"] / 2, w, k["h"]), rad, rad)
        t = QTransform()
        t.translate(k["cx"] - (k["w"] / 4 * k["dx"] if pushed else 0), k["cy"] - (k["w"] / 4 * k["dy"] if pushed else 0))
        t.rotate(k["rot"])
        return t.map(path)

    def key_hit(k, p):
        r = math.radians(k["rot"])
        dx, dy = p.x() - k["cx"], p.y() - k["cy"]
        lx = math.cos(r) * dx + math.sin(r) * dy
        ly = -math.sin(r) * dx + math.cos(r) * dy
        return abs(lx) <= k["w"] / 2 + 5 and abs(ly) <= k["h"] / 2

    def on_glass(L, p):
        gx, gy, gw, gh = L["active"]
        if not (gx <= p.x() <= gx + gw and gy <= p.y() <= gy + gh):
            return False
        rad = L["active_r"]
        cx = min(max(p.x(), gx + rad), gx + gw - rad)
        cy = min(max(p.y(), gy + rad), gy + gh - rad)
        return (p.x() - cx) ** 2 + (p.y() - cy) ** 2 <= rad * rad + 1e-9

    def paint_labels(p, keys, colour, pressed):
        """Each button's name; a held one's in the accent colour."""
        for k in keys:
            p.setPen(rgb(*TOUCH) if k["role"] in pressed else colour)
            p.save()
            p.translate(k["lx"], k["ly"])
            p.rotate(k["lrot"])
            p.setFont(font(k["lsize"]))
            p.drawText(QRectF(-60, -k["lsize"], 120, 2 * k["lsize"]), CENTER, k["label"])
            p.restore()

    def paint_device(p, L, prof, img, pressed, veil, text, labels=True):
        """The device from a layout: the body with its finish, the buttons, the glass, the screen."""
        style = FINISHES[prof["body"]["finish"]]
        bx, by, bw, bh = L["body"]
        dpr = p.device().devicePixelRatioF() if p.device() else 1.0
        for k in L["keys"]:                                   # the buttons sit behind the body's edge, so first
            if k.get("face"):
                continue
            pushed = k["role"] in pressed
            c1, c2 = style["key_down"] if pushed else style["key"]
            grad_fill(p, key_path(k, pushed), k["rot"] + 90, rgb(*c1), rgb(*c2))
        if L["photo"] is not None:
            p.drawImage(QPointF(bx, by), scaled_photo(L["photo"], bw, bh, dpr))
        else:
            path = rounded(L["body"], L["body_r"])
            grad_fill(p, path, -60, rgb(*style["fill"]), rgb(*style["fill2"]))
            p.strokePath(path, QPen(rgb(*style["edge"]), 1))
        for k in L["keys"]:                                   # buttons on the front, over the body or its picture:
            if k.get("face"):                                 # a press darkens them where they are
                c1, c2 = style["key_down"] if k["role"] in pressed else style["key"]
                path = key_path(k, False)
                grad_fill(p, path, k["rot"] + 90, rgb(*c1), rgb(*c2))
                p.strokePath(path, QPen(rgb(*style["edge"]), 1))
        for sx, sy, sd in L["studs"]:                     # the corner holes: a dark ring, a lighter floor
            p.setPen(NOPEN)
            p.setBrush(rgb(*style["edge"]))
            p.drawEllipse(QRectF(sx - sd / 2, sy - sd / 2, sd, sd))
            p.setBrush(rgb(*style["fill2"]))
            p.drawEllipse(QRectF(sx - sd / 2 + 1, sy - sd / 2 + 1, sd - 2, sd - 2))
        if labels:                                           # a pushed button on an edge hides its name
            named = [k for k in L["keys"] if not k.get("pictured") and (k.get("face") or k["role"] not in pressed)]
            paint_labels(p, named, rgb(*style["label"]), pressed)
        p.fillPath(rounded(L["glass"], L["glass_r"]), rgb(0x08, 0x08, 0x0a))   # the cover glass, the screen in it
        p.save()
        a = L["active"]
        p.setClipPath(rounded(a, L["active_r"]), Qt.ClipOperation.IntersectClip)
        p.fillRect(QRectF(*a), rgb(0, 0, 0))
        if img is not None:
            p.drawImage(QRectF(*a), img)
        if veil:
            p.fillRect(QRectF(*a), rgb(0, 0, 0, veil))
        if text:
            inset = a[2] * .15 if L["active_r"] else 8
            p.setPen(rgb(0xc9, 0xc9, 0xc9))
            p.setFont(font(12))
            p.drawText(QRectF(*a).adjusted(inset, inset, -inset, -inset), CENTER | int(Qt.TextFlag.TextWordWrap), text)
        p.restore()
        grad_fill(p, rounded(L["glass"], L["glass_r"]), -70, rgb(255, 255, 255, .07), rgb(255, 255, 255, 0))

    def paint_button(p, name, br, recording, lit, enabled, mode=0):
        x, y, w, h = br
        cx, cy = x + w / 2, y + h / 2
        ink = rgb(0xdd, 0xdd, 0xdd)
        p.save()
        if not enabled:
            p.setOpacity(.4)
        p.setPen(NOPEN)
        p.setBrush(rgb(0xeb, 0x57, 0x57) if name == "rec" and lit else rgb(0, 0x9b, 0x6d) if lit
                   else rgb(0x3a, 0x3a, 0x40, .75))
        p.drawEllipse(QRectF(*br))
        if name == "close":
            p.setPen(rgb(0xc9, 0xc9, 0xc9))
            p.setFont(font(17))
            p.drawText(QRectF(*br), CENTER, "✕")
        elif name == "listen":                                # a speaker: a box and a cone, and a sound wave
            p.setBrush(ink)
            p.drawPolygon(QPolygonF([QPointF(cx - 8, cy - 3), QPointF(cx - 4, cy - 3), QPointF(cx + 1, cy - 7),
                                     QPointF(cx + 1, cy + 7), QPointF(cx - 4, cy + 3), QPointF(cx - 8, cy + 3)]))
            p.setBrush(Qt.BrushStyle.NoBrush)
            p.setPen(pen(ink, 1.5))
            p.drawArc(QRectF(cx + 1 - 6, cy - 6, 12, 12), -40 * 16, 80 * 16)
        elif name == "talk":                                  # a microphone: a capsule on a stand
            p.setBrush(ink)
            p.drawEllipse(QRectF(cx - 3.5, cy - 9, 7, 11))
            p.fillRect(QRectF(cx - .75, cy + 4.5, 1.5, 4), ink)
            p.fillRect(QRectF(cx - 4, cy + 8, 8, 1.5), ink)
            p.setBrush(Qt.BrushStyle.NoBrush)
            p.setPen(pen(ink, 1.5))
            p.drawArc(QRectF(cx - 6.5, cy - 2 - 6.5, 13, 13), 180 * 16, 180 * 16)
        elif name == "move":                                  # four arrows from the middle
            p.setPen(pen(ink, 1.5))
            for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                tip = QPointF(cx + dx * 8, cy + dy * 8)
                p.drawLine(QPointF(cx, cy), tip)
                for side in (1, -1):                          # the head: two short strokes back from the tip
                    p.drawLine(tip, QPointF(tip.x() - dx * 3.5 + side * dy * 3, tip.y() - dy * 3.5 + side * dx * 3))
        elif name == "rec":
            if recording:
                p.fillRect(QRectF(cx - 6, cy - 6, 12, 12), rgb(255, 255, 255))
            else:
                p.setBrush(rgb(0xeb, 0x57, 0x57))
                p.drawEllipse(QRectF(cx - 7, cy - 7, 14, 14))
        else:                                                 # a camera
            p.setBrush(Qt.BrushStyle.NoBrush)
            p.setPen(pen(ink, 1.5))
            p.drawRoundedRect(QRectF(cx - 9, cy - 4, 18, 12), 2, 2)
            p.drawEllipse(QRectF(cx - 3.5, cy - 1.5, 7, 7))
            p.fillRect(QRectF(cx - 4, cy - 7, 8, 3), ink)
        if mode:                    # where the sound goes, under the icon: both the board and here, or here alone
            text = "BOTH" if mode == 1 else "PC"
            f = font(7.5)
            f.setBold(True)
            fm = QFontMetricsF(f)
            tw, th = fm.horizontalAdvance(text) + 7, fm.height() + 1
            tag = QRectF(cx - tw / 2, y + h - th + 3, tw, th)
            p.setOpacity(1)
            p.setPen(pen(rgb(0x14, 0x14, 0x18), 1.25))
            p.setBrush(rgb(0xf2, 0xf2, 0xf2))
            p.drawRoundedRect(tag, th / 2, th / 2)
            p.setPen(rgb(0x14, 0x14, 0x18))
            p.setFont(f)
            p.drawText(tag, CENTER, text)
        p.restore()

    PLACEHOLDER = dict(name="", w=240, h=240, round=False, diag=0, touch=False, buttons=[])

    class Dev:
        def __init__(self, link):
            self.link = link
            self.look = board_look(PLACEHOLDER)
            self.image = None
            self.pressed = set()
            self.L = None
            self.slot = None                                  # the device's part of the window
            self.icon_dirty = True
            self.said = None

    class Bridge(QObject):
        board = Signal(int, object)
        frame = Signal(int)
        status = Signal(int, str)
        saved = Signal(str)

    class Window(QWidget):
        def __init__(self, links):
            flags = Qt.WindowType.FramelessWindowHint | Qt.WindowType.NoDropShadowWindowHint
            if on_top:
                flags |= Qt.WindowType.WindowStaysOnTopHint
            super().__init__(None, flags)
            self.setWindowTitle("Muse mirror")
            self.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground)
            self.setMouseTracking(True)
            self.setAttribute(Qt.WidgetAttribute.WA_InputMethodEnabled)   # composed text: inputMethodEvent
            self.setFocusPolicy(Qt.FocusPolicy.StrongFocus)
            self.devs = [Dev(link) for link in links]
            self.zoom, self.ss, self.lay = scale, None, None
            self.zooming, self.zoomed_at = False, 0.0
            self.flash_until, self.toast = 0.0, None
            self.touches = []                                 # (device, x, y in screen pixels, time)
            self.touch_at = 0.0
            self.button_down = self.mouse_key = self.finger = self.grip = self.drag_from = None
            self.wheel = None                                 # two fingers down: [device, cursor x, y, finger x, y, axis, on glass]
            self.wheel_idle = QTimer(self, singleShot=True, timeout=self.wheel_up)
            self.held = {}                                    # keyboard role -> the device it went to
            self.rec = None
            self.audio_io = None                              # QtMultimedia loads with the first sound
            self.anim = QTimer(self, interval=50, timeout=self.animate)
            self.rec_timer = QTimer(self, interval=20, timeout=self.rec_frame)
            self.icon_timer = QTimer(self, interval=1000, timeout=self.refresh_icon)
            self.active_dev = self.devs[0]
            self.build_actions()

        # -- geometry ---------------------------------------------------------------------------------------
        def px_mm(self):
            """Logical pixels per millimetre on the screen the window is on."""
            screen = self.screen() or QGuiApplication.primaryScreen()
            size = screen.physicalSize()
            if size.width() > 0:
                return screen.geometry().width() / size.width()
            return screen.logicalDotsPerInch() / 25.4

        def scales(self, zoom):
            """Points per millimetre for each device at zoom (a share of the real size on this display), each
            raised as far as its screen needs to be MIN_GLASS wide."""
            pxmm = self.px_mm()
            return [max(zoom * pxmm, MIN_GLASS / device_layout(d.look, 1.0, 0, 0)["active"][2]) for d in self.devs]

        def measure(self, ss):
            """The devices at their scales, in rows (up to three side by side, more in a grid), and the window
            around them: a thin margin, more above for the buttons, room for keys that stick out."""
            parts = [device_layout(d.look, s, 0, 0) for d, s in zip(self.devs, ss)]
            m = 4
            cells = [(L["over"][0] + L["body"][2] + L["over"][2], L["over"][1] + L["body"][3] + L["over"][3])
                     for L in parts]
            n = len(parts)
            cols = n if n <= 3 else math.ceil(math.sqrt(n))
            rows = [list(range(i, min(n, i + cols))) for i in range(0, n, cols)]
            row_w = [sum(cells[i][0] for i in row) + GAP * (len(row) - 1) for row in rows]
            row_h = [max(cells[i][1] for i in row) for row in rows]
            mt = max(m, BUTTON + 14)
            W = max(row_w) + 2 * m
            W += max(0, len(BUTTONS) * (BUTTON + 6) + 8 - W)  # the toolbar never shrinks
            return dict(W=W, H=mt + sum(row_h) + GAP * (len(rows) - 1) + m, m=m, mt=mt, parts=parts, cells=cells,
                        rows=rows, row_w=row_w, row_h=row_h)

        def apply_zoom(self, zoom, say=True):
            """The size as a share of the real size on the display the window is on, kept across displays."""
            if self.zooming:
                return
            self.zooming = True
            try:
                self.fit(zoom)
            finally:
                self.zooming, self.zoomed_at = False, time.monotonic()
            if say:
                self.notice("actual size" if abs(self.zoom - 1) < .02 else "%.0f %% of actual" % (self.zoom * 100))

        def fit(self, zoom):
            self.ss = self.scales(zoom)
            self.zoom = min(self.ss) / self.px_mm()           # after the floor: Bigger acts at once
            g = self.measure(self.ss)
            self.lay = None
            # Fixed: the system's edge drag would stretch the window off the devices' proportions; corners resize.
            self.setFixedSize(int(math.ceil(g["W"])), int(math.ceil(g["H"])))
            if self.isVisible() and not self.grip:            # grown past the screen's edge: back onto it
                area = (self.screen() or QGuiApplication.primaryScreen()).availableGeometry()
                x = max(area.left(), min(self.x(), area.right() + 1 - self.width()))
                y = max(area.top(), min(self.y(), area.bottom() + 1 - self.height()))
                if (x, y) != (self.x(), self.y()):
                    self.move(x, y)
            self.update()

        def screen_changed(self, _screen):
            """Moved to another display, where a millimetre is another number of points: the same share of the
            real size there. Not while a resize of ours is settling, which can itself cross displays and back."""
            if time.monotonic() - self.zoomed_at > .5:
                QTimer.singleShot(0, lambda: self.apply_zoom(self.zoom, say=False))

        def arrange(self):
            """The parts at the current size: each device, the buttons, the handles."""
            if self.lay is not None:
                return self.lay
            if self.ss is None:
                self.ss = self.scales(self.zoom)
            g = self.measure(self.ss)
            W, H = self.width(), self.height()
            if abs(W - g["W"]) > 2 or abs(H - g["H"]) > 2:    # resized from outside: the devices fit the window
                zoom = self.zoom
                for _ in range(3):                            # the margins don't scale: a pass or two settles it
                    zoom *= min(W / g["W"], H / g["H"])
                    self.ss = self.scales(zoom)
                    g = self.measure(self.ss)
                self.zoom = min(self.ss) / self.px_mm()
            y = g["mt"] + (H - g["H"]) / 2
            for row, rw, rh in zip(g["rows"], g["row_w"], g["row_h"]):
                x = (W - rw) / 2
                for i in row:
                    d, P, (cw, ch) = self.devs[i], g["parts"][i], g["cells"][i]
                    d.L = device_layout(d.look, self.ss[i], x + P["over"][0], y + (rh - ch) / 2 + P["over"][1])
                    d.slot = QRectF(x - GAP / 2, y - GAP / 2, cw + GAP, rh + GAP)
                    x += cw + GAP
                y += rh + GAP
            x0 = (W - (len(BUTTONS) * (BUTTON + 6) - 6)) / 2
            # The corners of the drawn devices resize, as a window's own do.
            bodies = [d.L["body"] for d in self.devs]
            left, top = min(b[0] for b in bodies), min(b[1] for b in bodies)
            right, bottom = max(b[0] + b[2] for b in bodies), max(b[1] + b[3] for b in bodies)
            self.lay = dict(buttons={name: (x0 + i * (BUTTON + 6), 4, BUTTON, BUTTON)
                                     for i, name in enumerate(BUTTONS)},
                            corners=[((sx, sy), self.corner_at(sx, sy, left, top, right, bottom))
                                     for sx in (-1, 1) for sy in (-1, 1)])
            return self.lay

        def corner_at(self, sx, sy, left, top, right, bottom):
            """Where a drawn corner is: on the rounding of the device that sits furthest that way, not on the
            empty point of its bounding box."""
            x, y = left if sx < 0 else right, top if sy < 0 else bottom
            d = max(self.devs, key=lambda d: sx * (d.L["body"][0] + d.L["body"][2] * (sx > 0))
                    + sy * (d.L["body"][1] + d.L["body"][3] * (sy > 0)))
            if d.L["corners"]:                                   # a picture's corner is where it is drawn
                return d.L["corners"][(sx, sy)]
            inset = d.L["body_r"] * (1 - math.sqrt(.5))
            return x - sx * inset, y - sy * inset

        def resizeEvent(self, e):
            self.lay = None
            super().resizeEvent(e)

        # -- drawing ----------------------------------------------------------------------------------------
        def paintEvent(self, e):
            lay = self.arrange()
            p = QPainter(self)
            p.setRenderHint(AA)
            p.setRenderHint(SMOOTH)
            if wall:
                p.setPen(NOPEN)
                p.setBrush(rgb(0x14, 0x14, 0x18))
                p.drawRoundedRect(QRectF(self.rect()), 12, 12)
            now = time.monotonic()
            self.paint_devices(p, now, flash=True)
            for name, br in lay["buttons"].items():
                lit = name == "rec" and self.rec is not None or name == "shot" and now < self.flash_until + .4
                link, io = self.active_dev.link, self.audio_io
                mode = 0
                if io and name in ("talk", "listen"):
                    mode = io.speaker_mode(link) if name == "listen" else io.mic_mode(link)
                enabled = name not in ("talk", "listen") or link.live and link.audio_capable
                paint_button(p, name, br, self.rec is not None, lit or mode > 0, enabled, mode)
            if self.toast and now < self.toast[1]:            # the toast, under the buttons
                f = font(11)
                fm = QFontMetricsF(f)
                text = fm.elidedText(self.toast[0], Qt.TextElideMode.ElideMiddle, max(40, self.width() - 40))
                tw, th = fm.horizontalAdvance(text), fm.height()
                box = QRectF(self.width() - 8 - tw - 16, 4 + BUTTON + 6, tw + 16, th + 8)
                p.setPen(NOPEN)
                p.setBrush(rgb(0x2a, 0x2a, 0x2e, .92))
                p.drawRoundedRect(box, 6, 6)
                p.setPen(rgb(0xdd, 0xdd, 0xdd))
                p.setFont(f)
                p.drawText(box.adjusted(8, 4, -8, -4), int(Qt.AlignmentFlag.AlignLeft | Qt.AlignmentFlag.AlignVCenter), text)
            elif self.toast:
                self.toast = None
            p.end()

        def paint_devices(self, p, now, flash):
            """The devices with their screens and the touches on them; the flash only in the window."""
            for d in self.devs:
                link = d.link
                veil = {1: .4, 2: .65}.get(link.state, 0)
                if not link.live:
                    veil = max(veil, .5)
                text = "connecting..." if not link.live else None
                paint_device(p, d.L, d.look["profile"], d.image, d.pressed, veil, text)
                if flash and now < self.flash_until:          # a screenshot: the glass flashes
                    p.save()
                    p.setClipPath(rounded(d.L["active"], d.L["active_r"]), Qt.ClipOperation.IntersectClip)
                    p.fillRect(QRectF(*d.L["active"]), rgb(255, 255, 255, .7 * (self.flash_until - now) / .35))
                    p.restore()
            self.touches = [t for t in self.touches if now - t[3] < TOUCH_S]
            p.setBrush(Qt.BrushStyle.NoBrush)
            for d, x, y, t in self.touches:
                a = 1 - (now - t) / TOUCH_S
                ax, ay, aw, ah = d.L["active"]
                rad = (3 + 5 * (1 - a)) * d.L["s"]
                p.setPen(QPen(rgb(*TOUCH, .8 * a), 2))
                p.drawEllipse(QPointF(ax + (x + .5) * aw / d.look["w"], ay + (y + .5) * ah / d.look["h"]), rad, rad)
            if self.wheel and (flash or self.touches_act.isChecked()):
                # Two fingers as iOS shows a trackpad scroll: one grey disc that stays under the cursor
                # while the screen moves beneath it.
                d, x, y = self.wheel[:3]
                bx, by = self.screen_xy(d, QPointF(x, y))
                ax, ay, aw, ah = d.L["active"]
                at = QPointF(ax + (bx + .5) * aw / d.look["w"], ay + (by + .5) * ah / d.look["h"])
                r = 4 * d.L["s"]
                p.save()
                p.setClipPath(rounded(d.L["active"], d.L["active_r"]), Qt.ClipOperation.IntersectClip)
                p.setPen(QPen(rgb(255, 255, 255, .55), 1.5))
                p.setBrush(rgb(140, 140, 140, .45))
                p.drawEllipse(at, r, r)
                p.restore()

        def device_bounds(self):
            """Where the devices are, their buttons included: (x0, y0, x1, y1)."""
            self.arrange()
            boxes = []
            for d in self.devs:
                (bx, by, bw, bh), (left, top, right, bottom) = d.L["body"], d.L["over"]
                boxes.append((bx - left, by - top, bx + bw + right, by + bh + bottom))
            return (min(b[0] for b in boxes), min(b[1] for b in boxes), max(b[2] for b in boxes),
                    max(b[3] for b in boxes))

        def devices_size(self):
            x0, y0, x1, y1 = self.device_bounds()
            dpr = self.devicePixelRatioF()
            return int(math.ceil((x1 - x0) * dpr)), int(math.ceil((y1 - y0) * dpr))

        def render_devices(self, size=None, ground=None):
            """The devices alone, through the window's own drawing, without its buttons, handles, toast or
            flash: an image of size (pixels; default their own) with them fitted and centred on ground
            (default transparent)."""
            x0, y0, x1, y1 = self.device_bounds()
            dpr = self.devicePixelRatioF()
            size = size or self.devices_size()
            img = QImage(size[0], size[1], QImage.Format.Format_ARGB32_Premultiplied)
            img.setDevicePixelRatio(dpr)
            img.fill(ground or Qt.GlobalColor.transparent)
            p = QPainter(img)
            p.setRenderHint(AA)
            p.setRenderHint(SMOOTH)
            lw, lh = size[0] / dpr, size[1] / dpr
            k = min(lw / (x1 - x0), lh / (y1 - y0))
            p.translate((lw - (x1 - x0) * k) / 2, (lh - (y1 - y0) * k) / 2)
            p.scale(k, k)
            p.translate(-x0, -y0)
            self.paint_devices(p, time.monotonic(), flash=False)
            p.end()
            return img

        def animate(self):
            """Redraws while a flash, a toast or a touch is on, then stops."""
            self.update()
            if time.monotonic() > max(self.flash_until + .4, self.toast[1] if self.toast else 0,
                                      self.touch_at + TOUCH_S):
                self.anim.stop()

        def touch(self, d, x, y, every=0.0):
            """A ring where the finger is, if touches are shown; while it moves, one every so often."""
            now = time.monotonic()
            if self.touches_act.isChecked() and now - self.touch_at >= every:
                self.touches.append((d, x, y, now))
                self.touch_at = now
                self.anim.start()

        def show_touches(self, on):
            if not on:
                self.touches.clear()
                self.update()

        def notice(self, text, flash=False):
            now = time.monotonic()
            if flash:
                self.flash_until = now + .35
            self.toast = (text, now + 2.5)
            self.anim.start()
            self.update()

        # -- from the links ---------------------------------------------------------------------------------
        def on_board(self, i, info):
            d = self.devs[i]
            look = board_look(info)
            if (look["w"], look["h"]) != (d.look["w"], d.look["h"]):
                d.image = None
            d.look, d.icon_dirty = look, True
            d.pressed.clear()
            if self.audio_io:
                self.audio_io.resume(d.link)
            print("%s %s: %s, %dx%d" % (time.strftime("%H:%M:%S"), d.link.host, look["name"], look["w"], look["h"]),
                  flush=True)
            self.apply_zoom(self.zoom, say=False)

        def on_frame(self, i):
            d = self.devs[i]
            with d.link.lock:
                fb = d.link.fb
                if fb is None:
                    return
                d.image = QImage(fb.tobytes(), fb.shape[1], fb.shape[0], fb.shape[1] * 2,
                                 QImage.Format.Format_RGB16).copy()
            d.icon_dirty = True
            if d.L:
                self.update(QRectF(*d.L["glass"]).adjusted(-2, -2, 2, 2).toAlignedRect())
            else:
                self.update()

        def on_status(self, i, text):
            d = self.devs[i]
            if text and text != d.said and not text.startswith("connecting"):
                print("%s %s" % (time.strftime("%H:%M:%S"), text), flush=True)
            d.said = text
            self.update()

        def refresh_icon(self):
            """The Dock (taskbar) icon follows the first board's screen: once a second when its pixels changed."""
            d = self.devs[0]
            if not d.icon_dirty:
                return
            d.icon_dirty = False
            size, margin = 512, 44
            probe = device_layout(d.look, 1.0, 0, 0)
            left, top, right, bottom = probe["over"]
            fw, fh = left + probe["body"][2] + right, top + probe["body"][3] + bottom
            s = (size - 2 * margin) / max(fw, fh)
            L = device_layout(d.look, s, (size - fw * s) / 2 + left * s, (size - fh * s) / 2 + top * s)
            pm = QPixmap(size, size)
            pm.fill(Qt.GlobalColor.transparent)
            p = QPainter(pm)
            p.setRenderHint(AA)
            p.setRenderHint(SMOOTH)
            paint_device(p, L, d.look["profile"], d.image, set(), {1: .4, 2: .65}.get(d.link.state, 0), None,
                         labels=False)
            p.end()
            app.setWindowIcon(QIcon(pm))

        # -- mouse ------------------------------------------------------------------------------------------
        def where(self, pos):
            lay = self.arrange()
            x, y = pos.x(), pos.y()
            for name, (bx, by, bw, bh) in lay["buttons"].items():
                if (x - bx - bw / 2) ** 2 + (y - by - bh / 2) ** 2 <= (bw / 2) ** 2:
                    return name, None, None
            for d in self.devs:                                  # a key wins over the corner it sits in
                for k in d.L["keys"]:
                    if k["role"] is not None and key_hit(k, pos):
                        return "key", k["role"], d
            for side, (cx, cy) in lay["corners"]:                # any corner resizes, as a window's does
                if abs(x - cx) <= CORNER and abs(y - cy) <= CORNER:
                    return "corner", side, None
            for d in self.devs:
                if on_glass(d.L, pos):
                    return "glass", None, d
            for d in self.devs:
                if rounded(d.L["body"], d.L["body_r"]).contains(pos):
                    return "body", None, d
            return "none", None, None

        def screen_xy(self, d, pos):
            ax, ay, aw, ah = d.L["active"]
            w, h = d.look["w"], d.look["h"]
            x = int((pos.x() - ax) * w / aw)
            y = int((pos.y() - ay) * h / ah)
            return max(0, min(w - 1, x)), max(0, min(h - 1, y))

        def press(self, d, role, down):
            if down and role in d.pressed:
                return
            if not down and role not in d.pressed:            # a refresh forgot it: the board may still hold it
                if d.look["profile"].get("hold", True):
                    d.link.key(role, False)
                return
            (d.pressed.add if down else d.pressed.discard)(role)
            if d.look["profile"].get("hold", True):
                d.link.key(role, down)
            elif not down:
                d.link.tap(role)
            self.update()

        def start_move(self, e):
            self.activateWindow()                             # the system's move takes the click: keys come here
            handle = self.windowHandle()
            if not (handle and handle.startSystemMove()):
                self.drag_from = e.globalPosition().toPoint() - self.frameGeometry().topLeft()

        def mousePressEvent(self, e):
            if e.button() != Qt.MouseButton.LeftButton:
                return super().mousePressEvent(e)
            pos = e.position()
            self.wheel_up()
            what, role, d = self.where(pos)
            if d is not None:
                self.active_dev = d
            if what in ("talk", "listen"):
                self.audio_cycle(what)
                return
            if what in ("rec", "shot", "close"):
                self.button_down = what
            elif what == "key":
                self.mouse_key = (d, role)
                self.press(d, role, True)
            elif what == "glass" and d.look["touch"]:
                self.finger = d
                at = self.screen_xy(d, pos)
                d.link.pointer(*at, True)
                self.touch(d, *at)
            elif what == "corner":
                at, corners = e.globalPosition(), dict(self.arrange()["corners"])
                (x0, y0), (x1, y1) = corners[(-1, -1)], corners[(1, 1)]
                # The window's opposite corner stays where it is on the desk, not the device's: while the buttons
                # hold the window's width, a device corner would slide the whole window sideways.
                self.grip = dict(side=role, x0=at.x(), y0=at.y(), zoom=self.zoom, w=x1 - x0, h=y1 - y0,
                                 anchor=(self.x() + (self.width() if role[0] < 0 else 0),
                                         self.y() + (self.height() if role[1] < 0 else 0)))
            elif what in ("move", "body", "glass"):
                self.start_move(e)

        def mouseMoveEvent(self, e):
            pos = e.position()
            if self.finger:
                at = self.screen_xy(self.finger, pos)
                self.finger.link.pointer(*at, True)
                self.touch(self.finger, *at, every=.08)
            elif self.grip:
                g, at = self.grip, e.globalPosition()
                sx, sy = g["side"]
                # Only the drag along the corner's diagonal counts; across it, nothing changes.
                dx, dy = sx * (at.x() - g["x0"]), sy * (at.y() - g["y0"])
                grow = 1 + (dx * g["w"] + dy * g["h"]) / (g["w"] ** 2 + g["h"] ** 2)
                self.apply_zoom(g["zoom"] * grow, say=False)
                self.move(g["anchor"][0] - (self.width() if sx < 0 else 0),
                          g["anchor"][1] - (self.height() if sy < 0 else 0))
            elif self.drag_from is not None:
                self.move(e.globalPosition().toPoint() - self.drag_from)
            elif not e.buttons():
                what, _role, d = self.where(pos)
                shape = (Qt.CursorShape.PointingHandCursor if what == "key" or what == "glass" and
                         d.look["touch"] else Qt.CursorShape.OpenHandCursor if what == "move"
                         else (Qt.CursorShape.SizeFDiagCursor if _role[0] == _role[1] else Qt.CursorShape.SizeBDiagCursor)
                         if what == "corner" else Qt.CursorShape.ArrowCursor)
                if self.cursor().shape() != shape:
                    self.setCursor(shape)

        def mouseReleaseEvent(self, e):
            if e.button() != Qt.MouseButton.LeftButton:
                return super().mouseReleaseEvent(e)
            if self.finger:
                self.finger.link.pointer(*self.screen_xy(self.finger, e.position()), False)
                self.finger = None
            if self.mouse_key:
                self.press(*self.mouse_key, False)
                self.mouse_key = None
            self.grip = self.drag_from = None
            down, self.button_down = self.button_down, None
            if down and self.where(e.position())[0] == down:
                {"rec": self.record, "shot": self.screenshot, "close": self.close}[down]()

        # -- two fingers on a trackpad (or a wheel) over a touch screen: one finger on it ------------------------
        def wheelEvent(self, e):
            phase = e.phase()
            if phase == Qt.ScrollPhase.ScrollMomentum:        # the fingers left the trackpad: the board's list coasts
                return self.wheel_up()
            if self.wheel is None:
                what, _role, d = self.where(e.position())
                if phase == Qt.ScrollPhase.ScrollEnd or what != "glass" or not d.look["touch"] or self.finger:
                    return super().wheelEvent(e)
                cx, cy = e.position().x(), e.position().y()
                self.wheel = [d, cx, cy, cx, cy, None, False]
                self.update(QRectF(*d.L["glass"]).toAlignedRect())
            if phase == Qt.ScrollPhase.ScrollEnd:
                return self.wheel_up()
            d, cx, cy, x, y, axis, down = self.wheel
            # A pause ends the touch: a wheel or most touchpads off a Mac send no end, and a Mac's can come late.
            self.wheel_idle.start(150)
            # The content follows the delta as in any scrolled view, so the finger does: the system's
            # natural-scrolling setting is already in it. A wheel notch has no pixels: 40 points.
            px = e.pixelDelta()
            dx, dy = (px.x(), px.y()) if not px.isNull() else (e.angleDelta().x() / 3, e.angleDelta().y() / 3)
            if axis is None and max(abs(dx), abs(dy)) >= 2:   # the first clear move picks the axis, as scroll views do
                axis = "h" if abs(dx) > abs(dy) else "v"
                self.wheel[5] = axis
            if axis is None:
                return
            dx, dy = (dx, 0) if axis == "h" else (0, dy)
            if QRectF(*d.L["active"]).contains(QPointF(x + dx, y + dy)):
                x, y = x + dx, y + dy
                start, at = self.screen_xy(d, QPointF(cx, cy)), self.screen_xy(d, QPointF(x, y))
                if down:
                    d.link.pointer(*at, True)
                elif math.hypot(at[0] - start[0], at[1] - start[1]) >= WHEEL_SLOP:
                    # Down only once the move is past what the board takes for a tap, and the move right
                    # after it: a short scroll must not click what is under the cursor.
                    d.link.pointer(*start, True)
                    d.link.pointer(*at, True)
                    down = True
            else:                                             # off the glass: lift; the next stretch starts at the cursor
                if down:
                    d.link.pointer(*self.screen_xy(d, QPointF(x, y)), False)
                x, y, down = cx, cy, False
            self.wheel[3:] = [x, y, axis, down]
            self.update(QRectF(*d.L["glass"]).toAlignedRect())

        def wheel_up(self):
            self.wheel_idle.stop()
            if self.wheel:
                d, _cx, _cy, x, y, _axis, down = self.wheel
                if down:
                    d.link.pointer(*self.screen_xy(d, QPointF(x, y)), False)
                self.wheel = None
                self.update(QRectF(*d.L["glass"]).toAlignedRect())

        def event(self, e):
            if e.type() == QEvent.Type.ToolTip:
                what = self.where(QPointF(e.pos()))[0]
                tip = {"move": "Move the window (or drag a device)",
                       "talk": "Your microphone to the board: off, with the board's, yours only",
                       "listen": "The board's sound: off, computer only, board and computer",
                       "rec": "Record / Stop (%s)" % self.native("Ctrl+R"),
                       "shot": "Screenshot (%s)" % self.native("Ctrl+S"), "close": "Close"}.get(what)
                if tip:
                    QToolTip.showText(e.globalPos(), tip, self)
                else:
                    QToolTip.hideText()
                    e.ignore()
                return True
            return super().event(e)

        @staticmethod
        def native(keys):
            return QKeySequence(keys).toString(QKeySequence.SequenceFormat.NativeText)

        # -- keyboard ---------------------------------------------------------------------------------------
        def device_under_mouse(self):
            at = self.mapFromGlobal(QCursor.pos())
            for d in self.devs:
                if d.slot and d.slot.contains(QPointF(at)):
                    return d
            return self.devs[0]

        def keyPressEvent(self, e):
            d = self.device_under_mouse()
            edits = {Qt.Key.Key_Backspace: "Backspace", Qt.Key.Key_Delete: "Delete",
                     Qt.Key.Key_Left: "Left", Qt.Key.Key_Right: "Right", Qt.Key.Key_Return: "Enter",
                     Qt.Key.Key_Enter: "Enter", Qt.Key.Key_Escape: "Escape"}
            paste = app.clipboard().text() if d.link.text_open and e.matches(QKeySequence.StandardKey.Paste) else None
            mods = e.modifiers()
            ctrl_alt = Qt.KeyboardModifier.ControlModifier | Qt.KeyboardModifier.AltModifier
            altgr = sys.platform == "win32" and mods & ctrl_alt == ctrl_alt   # Windows reports AltGr as Ctrl+Alt
            modified = bool(mods & (Qt.KeyboardModifier.ControlModifier | Qt.KeyboardModifier.MetaModifier)) and not altgr
            if route_text_key(d.link, edits.get(e.key()), e.text(), paste=paste, modified=modified):
                e.accept()
                return
            role = KEY_ROLES.get(e.key())
            mods = e.modifiers() & (Qt.KeyboardModifier.ControlModifier | Qt.KeyboardModifier.MetaModifier |
                                    Qt.KeyboardModifier.AltModifier)
            if not role or mods:
                return super().keyPressEvent(e)
            if e.isAutoRepeat():
                return
            if role <= 4 and not any(b["role"] == role for b in d.look["buttons"]):
                return
            if role > 4 and not d.look["keyboard"]:
                return
            if role <= 4:
                self.held[role] = d
                self.press(d, role, True)
            else:
                d.link.key(role, True)
                d.link.key(role, False)

        def inputMethodEvent(self, e):
            """Text an input method composed (CJK, dead keys) goes to the board's open field."""
            d = self.device_under_mouse()
            if e.commitString() and route_text_key(d.link, None, e.commitString()):
                e.accept()
            else:
                e.ignore()

        def keyReleaseEvent(self, e):
            role = KEY_ROLES.get(e.key())
            if role in self.held and not e.isAutoRepeat():
                self.press(self.held.pop(role), role, False)
            elif not role:
                super().keyReleaseEvent(e)

        def let_go(self):
            """Every key and the finger let go, each on the board that got it."""
            for role, d in list(self.held.items()):
                self.press(d, role, False)
            self.held.clear()
            if self.mouse_key:
                self.press(*self.mouse_key, False)
                self.mouse_key = None
            for d in self.devs:
                for role in list(d.pressed):
                    self.press(d, role, False)
            if self.finger:
                at = QPointF(self.mapFromGlobal(QCursor.pos()))
                self.finger.link.pointer(*self.screen_xy(self.finger, at), False)
                self.finger = None
            self.wheel_up()

        def focusOutEvent(self, e):
            self.let_go()
            super().focusOutEvent(e)

        def changeEvent(self, e):
            if e.type() == QEvent.Type.ActivationChange and not self.isActiveWindow():
                self.let_go()
            super().changeEvent(e)

        # -- sound ------------------------------------------------------------------------------------------

        def sound(self):
            """The audio session for the selected board, or None with the reason on screen."""
            link = self.active_dev.link
            if not link.live or not link.audio_capable:
                self.notice("This board does not offer sound over this cable")
                return None
            if self.audio_io is None:
                try:
                    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
                    from mirror_audio import AudioSession
                    self.audio_io = AudioSession(self, self.notice, adpcm_encode)
                except ImportError as error:
                    self.notice("No sound: %s" % error)
                finally:
                    sys.path.pop(0)
            return self.audio_io

        def audio_cycle(self, what):
            """The icons step through off, the board and this computer, this computer alone."""
            io, link = self.sound(), self.active_dev.link
            if io is None:
                return
            if what == "listen":
                mode = io.cycle_speaker(link)
                self.notice(("Sound off", "Sound: board and computer", "Sound: computer only")[mode])
            else:
                mode = io.cycle_mic(link)
                self.notice(("Microphone off", "Microphone: board's and yours", "Microphone: yours only")[mode])
            self.update()

        def hear_microphone(self):
            io = self.sound()
            if io is not None:
                io.listen(self.active_dev.link, 1)
                self.notice("Hearing the board's microphone while it records" if io.listen_link else "Sound off")
                self.update()

        # -- menu -------------------------------------------------------------------------------------------

        def build_actions(self):
            def act(text, keys, fn, enabled=True, checkable=False):
                a = QAction(text, self)
                a.setShortcuts([QKeySequence(k) for k in keys])
                a.setEnabled(enabled)
                a.setCheckable(checkable)
                a.triggered.connect(fn)
                self.addAction(a)
                return a

            self.top_act = act("Float on Top", ["Ctrl+T"], self.toggle_top, checkable=True)
            self.top_act.setChecked(on_top)
            self.touches_act = act("Show touches", [], self.show_touches, checkable=True)
            self.touches_act.setChecked(True)
            self.items = [act("Screenshot", ["Ctrl+S"], self.screenshot), act("Record / Stop", ["Ctrl+R"], self.record),
                          None,
                          act("Sound: hear what the board plays", ["Ctrl+L"], lambda: self.audio_cycle("listen")),
                          act("Sound: hear the board's microphone", ["Ctrl+Shift+L"], self.hear_microphone),
                          act("Microphone: lend yours to the board", ["Ctrl+K"], lambda: self.audio_cycle("talk")),
                          None,
                          act("Bigger", ["Ctrl+=", "Ctrl++"], lambda: self.apply_zoom(self.zoom * 1.25)),
                          act("Smaller", ["Ctrl+-"], lambda: self.apply_zoom(self.zoom * .8)),
                          act("Actual size", ["Ctrl+0"], lambda: self.apply_zoom(1.0)),
                          None, self.touches_act, self.top_act]
            self.quit_act = act("Quit Muse mirror", ["Ctrl+Q"], self.close)
            self.quit_act.setMenuRole(QAction.MenuRole.QuitRole)

        def fill_menu(self, menu):
            menu.setToolTipsVisible(True)
            for a in self.items:
                if a is None:
                    menu.addSeparator()
                else:
                    menu.addAction(a)
            return menu

        def contextMenuEvent(self, e):
            menu = self.fill_menu(QMenu(self))
            menu.addSeparator()
            menu.addAction(self.quit_act)
            menu.exec(e.globalPos())

        def toggle_top(self, on):
            self.setWindowFlag(Qt.WindowType.WindowStaysOnTopHint, on)
            self.show()

        def restore_position(self):
            area = QGuiApplication.primaryScreen().availableGeometry()
            x, y = area.center().x() - self.width() // 2, area.center().y() - self.height() // 2
            self.move(max(area.left(), min(x, area.right() + 1 - self.width())),
                      max(area.top(), min(y, area.bottom() + 1 - self.height())))

        # -- screenshot, recording --------------------------------------------------------------------------
        def screenshot(self):
            shots = [d for d in self.devs if d.image is not None]
            if not shots:
                self.notice("no frame yet")
                return
            stamp = time.strftime("%Y%m%d-%H%M%S")
            paths = []
            for i, d in enumerate(shots):
                path = desktop_path("png", "" if len(shots) == 1 else "-%d" % (i + 1), stamp)
                if not d.image.save(path):
                    self.notice("could not save screenshot: " + path)
                    return
                paths.append(path)
            paths.append(desktop_path("png", "-window", stamp))
            if not self.render_devices().save(paths[-1]):
                self.notice("could not save screenshot: " + paths[-1])
                return
            for path in paths:
                print("saved", path, flush=True)
            self.notice("saved " + os.path.basename(paths[0]), flash=True)

        def video_frame(self):
            """The devices as one video frame: on a dark ground (mp4 has no transparency), at the recording's size."""
            return bytes(self.render_devices(self.rec["size"], rgb(0x1e, 0x1f, 0x22)).constBits())

        def record(self):
            if self.rec:
                self.rec_timer.stop()
                rec, self.rec = self.rec, None
                rec["queue"].put(None)
                self.notice("recording stopped, encoding...")
                return
            ffmpeg = shutil.which("ffmpeg")
            if not ffmpeg:
                self.notice("recording needs ffmpeg")
                return
            w, h = self.devices_size()
            size = (w + w % 2, h + h % 2)                     # x264 wants even sides; kept for the whole take
            path = desktop_path("mp4")
            if not os.path.isdir(os.path.dirname(path)):
                self.notice("recording folder is unavailable: " + os.path.dirname(path))
                return
            try:
                proc = subprocess.Popen([ffmpeg, "-loglevel", "error", "-y", "-f", "rawvideo", "-pix_fmt", "bgra",
                                         "-s", "%dx%d" % size, "-r", "25", "-i", "-", "-c:v", "libx264",
                                         "-preset", "veryfast", "-crf", "18", "-pix_fmt", "yuv420p", path],
                                        stdin=subprocess.PIPE)
            except OSError as error:
                self.notice("could not start recording: " + str(error))
                return
            q = queue.Queue(maxsize=50)

            def write():
                ok = True
                while True:
                    frame = q.get()
                    if frame is None:
                        break
                    if ok:
                        try:
                            proc.stdin.write(frame)
                        except OSError:
                            ok = False
                try:
                    proc.stdin.close()
                except OSError:
                    pass
                good = proc.wait() == 0 and ok
                print(("saved " if good else "ffmpeg failed: ") + path, flush=True)
                bridge.saved.emit(("saved " if good else "ffmpeg failed, ") + os.path.basename(path))

            self.rec = dict(size=size, queue=q, t0=time.monotonic(), n=0, process=proc,
                            thread=threading.Thread(target=write))
            self.rec["thread"].start()
            self.rec_timer.start()
            self.notice("recording")

        def rec_frame(self):
            """25 frames a second by the clock: a late tick repeats the frame it has."""
            rec = self.rec
            if not rec:
                return
            if rec["process"].poll() is not None:
                self.record()
                return
            due = int((time.monotonic() - rec["t0"]) * 25) + 1
            if due <= rec["n"]:
                return
            frame = self.video_frame()
            for _ in range(min(10, due - rec["n"])):
                try:
                    rec["queue"].put_nowait(frame)
                except queue.Full:
                    break
            rec["n"] = due

        def closeEvent(self, e):
            rec = self.rec
            if rec:
                self.record()
                rec["thread"].join(30)
            if self.audio_io:
                self.audio_io.close()
            for d in self.devs:
                d.link.close()
            super().closeEvent(e)

    links = [Link(h, p, log_file=log_file, fast=(fast or {}).get(h, 0)) for h, p in targets]
    bridge = Bridge()
    win = Window(links)
    bridge.board.connect(win.on_board)
    bridge.frame.connect(win.on_frame)
    bridge.status.connect(win.on_status)
    bridge.saved.connect(win.notice)
    try:
        for i, link in enumerate(links):
            link.on_board = lambda info, i=i: bridge.board.emit(i, info)
            link.on_frame = lambda i=i: bridge.frame.emit(i)
            link.on_status = lambda text, i=i: bridge.status.emit(i, text)
            link.start()
        if sys.platform == "darwin":                          # the View menu in the menu bar too
            bar = QMenuBar(None)
            win.fill_menu(bar.addMenu("View")).addAction(win.quit_act)
            win.menu_bar = bar
        win.apply_zoom(scale, say=False)
        win.restore_position()
        win.show()
        win.raise_()
        win.activateWindow()
        win.windowHandle().screenChanged.connect(win.screen_changed)
        win.refresh_icon()
        win.icon_timer.start()
        return app.exec()
    finally:
        win.close()

def board_name(port, baud, seconds=3.0):
    """The board's name from a short session of its own, or None."""
    names = []
    link = Link(port, baud)
    link.on_status = lambda text: None
    link.on_board = lambda info: names.append(info.get("name"))
    link.start()
    end = time.monotonic() + seconds
    while not names and time.monotonic() < end:
        time.sleep(0.05)
    link.close()
    return names[0] if names and isinstance(names[0], str) else None

def mac_app_name(name):
    """macOS names an app in the Dock and the menu bar from its bundle, read when the app starts: for a
    script that is Python's own. Set the bundle's name before Qt starts it."""
    import ctypes
    import ctypes.util
    objc = ctypes.cdll.LoadLibrary(ctypes.util.find_library("objc"))
    ctypes.cdll.LoadLibrary(ctypes.util.find_library("Foundation"))
    objc.objc_getClass.restype = objc.sel_registerName.restype = ctypes.c_void_p
    objc.objc_getClass.argtypes = objc.sel_registerName.argtypes = [ctypes.c_char_p]
    send = ctypes.CFUNCTYPE(ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p)(("objc_msgSend", objc))
    send_ptr = ctypes.CFUNCTYPE(ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_char_p)(("objc_msgSend", objc))
    set_key = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p)(
        ("objc_msgSend", objc))
    sel = objc.sel_registerName
    info = send(send(objc.objc_getClass(b"NSBundle"), sel(b"mainBundle")), sel(b"infoDictionary"))
    text = lambda s: send_ptr(objc.objc_getClass(b"NSString"), sel(b"stringWithUTF8String:"), s.encode())
    if info:
        set_key(info, sel(b"setObject:forKey:"), text(name), text("CFBundleName"))

def main():
    ap = argparse.ArgumentParser(description="Muse screen, touch and keys over the console cable")
    ap.add_argument("ports", nargs="*", help="console serial ports; none: discover native USB boards")
    ap.add_argument("--list", action="store_true", help="list native USB boards without opening them")
    ap.add_argument("--baud", type=int, default=115200, help="console baud rate for UART bridges")
    ap.add_argument("--fast", type=int, default=460800, choices=(0, 230400, 460800, 921600),
                    help="the speed a UART bridge runs at during the session; 0 keeps --baud")
    ap.add_argument("--log", help="append timestamped console lines to FILE")
    ap.add_argument("--png", help="save the first board's screen and exit")
    ap.add_argument("--stats", action="store_true", help="measure received updates without a window")
    ap.add_argument("--seconds", type=float, default=5, help="measurement duration")
    ap.add_argument("--scale", type=float, default=1, help="times the real size")
    args = ap.parse_args()
    if not math.isfinite(args.scale) or args.scale <= 0:
        ap.error("--scale must be finite and positive")
    if not math.isfinite(args.seconds) or args.seconds <= 0 or args.baud <= 0:
        ap.error("--seconds and --baud must be positive")
    if any(not is_port(p) for p in args.ports):
        ap.error("targets must be serial ports, such as /dev/ttyACM0 or COM7")
    usb = usb_boards() if args.list or not args.ports else []
    if args.list:
        for port, mac in usb:
            print("%-40s %s" % ("USB " + mac, port))
        return 0
    targets = [(p, args.baud) for p in dict.fromkeys(args.ports or [p for p, _ in usb])]
    fast = {p: args.fast for p in args.ports}
    if not targets:
        print("No native USB boards found. For a UART bridge, pass its console port explicitly.", file=sys.stderr)
        return 1
    if args.stats or args.png:
        link = Link(*targets[0], log_file=args.log, fast=fast.get(targets[0][0], 0))
        link.on_status = lambda text: text and not text.startswith("connecting") and print(text, file=sys.stderr)
        try:
            link.start()
            if args.stats:
                start = time.monotonic()
                time.sleep(args.seconds)
                elapsed = time.monotonic() - start
                print("%d updates in %.2f s, %.1f updates/s, %d damaged" % (link.changes, elapsed, link.changes / elapsed,
                                                                           link.damaged))
            return save_png(link, args.png) if args.png else (0 if link.fb is not None else 1)
        finally:
            link.close()
    if importlib.util.find_spec("PySide6") is None:
        print("Install the viewer dependencies from requirements-mirror.txt", file=sys.stderr)
        return 1
    if sys.platform == "darwin":
        try:
            mac_app_name(board_name(*targets[0]) or "Muse mirror")
        except (OSError, AttributeError, TypeError):
            pass                                              # the name stays Python's
    return run_window(targets, args.scale, False, len(targets) > 1, log_file=args.log, fast=fast)

if __name__ == "__main__":
    sys.exit(main())
