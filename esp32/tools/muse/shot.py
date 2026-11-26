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

"""The panel as it is, from the bench console's >shot (muse_video_shot: the next
video frame with subtitles, the call's controls and the mic ring on it).

  tools/muse/shot.py PORT OUT.png [--round] [--scale N]   send >shot on a serial console
  tools/muse/shot.py --log LOG OUT.png [--round]          the lines from a saved log

The pixels are RGB565 big-endian. --round blacks out what a round panel
doesn't show. Needs pyserial for a port; no other dependencies."""

import argparse
import base64
import re
import struct
import sys
import time
import zlib


def png(path, w, h, rgb):
    raw = b"".join(b"\x00" + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))

    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


MAX_SIDE = 4096           # the largest panel is 480 px; a header past these is a broken or hostile log
MAX_PIXELS = 4 << 20
MAX_OUT_PIXELS = 16 << 20 # after --scale


def check_size(w, h, scale=1):
    """Refuse a frame size before anything is allocated for it."""
    if not (0 < w <= MAX_SIDE and 0 < h <= MAX_SIDE and w * h <= MAX_PIXELS):
        raise SystemExit(f"refusing a {w}x{h} shot (at most {MAX_SIDE} a side, {MAX_PIXELS} pixels)")
    if not (1 <= scale <= 8 and w * h * scale * scale <= MAX_OUT_PIXELS):
        raise SystemExit(f"refusing --scale {scale} for {w}x{h} (1-8, at most {MAX_OUT_PIXELS} pixels out)")


def parse(text):
    m = re.search(r"@shot (\d+) (\d+)", text)
    if not m:
        raise SystemExit("no @shot header: " + (re.search(r"@shot[^\n]*", text) or [""])[0])
    w, h = int(m.group(1)), int(m.group(2))
    check_size(w, h)
    tail = text[m.end():]
    end = tail.find("@shot end")
    # "@s OFFSET BASE64" (each line placed where it says; a lost one leaves zeros),
    # or the older "@s BASE64" in order.
    need = w * h * 2
    data = bytearray(need)
    got = bytearray(need)   # 1 where a line landed
    pos = 0
    for line in tail[:end if end >= 0 else None].splitlines():
        i = line.find("@s ")
        if i < 0:
            continue
        m = re.match(r"([0-9a-f]+) ([A-Za-z0-9+/=]+)$", line[i + 3:].strip())
        try:
            if m:
                chunk = base64.b64decode(m.group(2))
                off = int(m.group(1), 16)
            else:
                chunk = base64.b64decode(re.match(r"[A-Za-z0-9+/=]+", line[i + 3:]).group(0))
                off = pos
        except Exception:
            continue
        if off >= need:
            continue   # a line placed past the frame: ignored, as a lost one is
        data[off:off + len(chunk)] = chunk[:max(0, need - off)]
        got[off:off + len(chunk)] = b"\x01" * len(chunk[:max(0, need - off)])
        pos = off + len(chunk)
    return w, h, data, got


def to_rgb(w, h, data, round_panel, scale):
    out = bytearray(w * scale * h * scale * 3)
    cx, cy, r2 = (w - 1) / 2, (h - 1) / 2, (min(w, h) / 2) ** 2
    for y in range(h):
        for x in range(w):
            v = data[(y * w + x) * 2] << 8 | data[(y * w + x) * 2 + 1]
            px = bytes(((v >> 11) * 255 // 31, (v >> 5 & 63) * 255 // 63, (v & 31) * 255 // 31))
            if round_panel and (x - cx) ** 2 + (y - cy) ** 2 > r2:
                px = b"\x00\x00\x00"
            for sy in range(scale):
                o = ((y * scale + sy) * w * scale + x * scale) * 3
                out[o:o + 3 * scale] = px * scale
    return bytes(out)


def capture(port):
    import serial
    s = serial.Serial(port, 115200, timeout=0.2)
    try:
        import termios
        attrs = termios.tcgetattr(s.fd)
        attrs[2] &= ~termios.HUPCL
        termios.tcsetattr(s.fd, termios.TCSANOW, attrs)
    except Exception:
        pass
    s.reset_input_buffer()
    s.write(b">shot\n")
    buf, deadline = b"", time.time() + 60
    while time.time() < deadline and b"@shot end" not in buf and b"@shot none" not in buf:
        buf += s.read(65536)
    s.close()
    return buf.decode("utf-8", "replace")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("port", nargs="?")
    ap.add_argument("out")
    ap.add_argument("--log")
    ap.add_argument("--round", action="store_true")
    ap.add_argument("--scale", type=int, default=1)
    ap.add_argument("--tries", type=int, default=2, help="captures merged: lines a capture lost come from another")
    a = ap.parse_args()
    if a.log:
        w = h = None
        for f in a.log.split(","):
            w2, h2, d, g = parse(open(f, errors="replace").read())
            if w is None:
                w, h, data, got = w2, h2, d, g
            elif (w2, h2) != (w, h):
                raise SystemExit(f"{f}: a {w2}x{h2} shot can't be merged into {w}x{h}")
            else:
                for k in range(len(got)):
                    if not got[k] and g[k]:
                        data[k] = d[k]
                        got[k] = 1
        missing = got.count(0)
        if missing:
            rows = sorted({k // (w * 2) for k in range(len(got)) if not got[k]})
            print(f"note: {missing} bytes missing, rows {rows[0]}..{rows[-1]} ({len(rows)} rows)", file=sys.stderr)
    else:
        data = got = None
        for _ in range(a.tries):
            w2, h2, d, g = parse(capture(a.port))
            if data is None:
                w, h, data, got = w2, h2, d, g
            elif (w2, h2) != (w, h):
                raise SystemExit(f"a {w2}x{h2} capture can't be merged into {w}x{h}")
            else:
                for k in range(len(got)):
                    if not got[k] and g[k]:
                        data[k] = d[k]
                        got[k] = 1
            if all(got):
                break
        missing = got.count(0)
        if missing:
            print(f"note: {missing} bytes never arrived (left black)", file=sys.stderr)
    check_size(w, h, a.scale)
    png(a.out, w * a.scale, h * a.scale, to_rgb(w, h, bytes(data), a.round, a.scale))
    print(f"{a.out}: {w}x{h}")


if __name__ == "__main__":
    sys.exit(main())
