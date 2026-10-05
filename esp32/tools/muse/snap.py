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

"""Send bench keys, then save a screenshot as PNG: tools/muse/snap.py <port> <keys> <out.png> [reset_wait_s]

Keys are the serial bench hooks in muse_input.c ('a' menu Down, 's' menu Select, ...),
sent 350 ms apart. A '>' starts a console command that runs to the end of the keys,
sent as one line: '>face=thinking' puts the avatar in a mode, '>face=happy' pets it.
Screenshots are off by default; build and flash with MUSE_BENCH=1 tools/muse/board.sh.
With reset_wait_s the board is reset first and given that long to boot.
The PNG is scaled 3x so small screens are readable.
"""
import base64
import re
import struct
import sys
import time
import zlib

import serial

SCALE = 3
port, keys, out = sys.argv[1], sys.argv[2], sys.argv[3]
s = serial.Serial(port, 115200, timeout=0.2)


def rd(t, *until):
    end = time.time() + t
    o = b''
    while time.time() < end and not any(u in o for u in until):
        o += s.read(65536)
    return o


if len(sys.argv) > 4:
    s.dtr = False; s.rts = True; time.sleep(0.1); s.rts = False
    rd(float(sys.argv[4]))
rd(0.3)
keys, command, line = keys.partition('>')
for k in keys:
    s.write(k.encode()); time.sleep(0.35)
if command:
    s.write(f'>{line}\n'.encode()); time.sleep(0.35)
s.write(b'p')
log = rd(90, b'SNAP END', b'SNAP OFF').decode('latin1')   # a UART console at 115200: ~8 s for 135x240, ~45 s for 412x412
if 'SNAP BEGIN' not in log:
    sys.exit('screenshots are off in this build: MUSE_BENCH=1 tools/muse/board.sh build|flash <board>'
             if 'SNAP OFF' in log else 'no screenshot: the board never answered')
head, body = log[log.index('SNAP BEGIN'):].split('\n', 1)
f = head.split()
w, h = int(f[2]), int(f[3])
per_line = int(f[4]) if len(f) > 4 else 144   # bytes per base64 line; older firmware doesn't say
# The log shares the port and can come between the base64 lines, a character at
# a time on a UART console, so each base64 line is the tail of the line it's on.
lines = iter(body.split('SNAP END')[0].split('\n'))
raw = b''
for n in [min(per_line, w * 2 - x) for _ in range(h) for x in range(0, w * 2, per_line)]:
    k = 4 * -(-n // 3)
    for l in lines:
        tail = re.search(r'[A-Za-z0-9+/]*={0,2}$', l.rstrip('\r')).group()
        if len(tail) >= k:
            raw += base64.b64decode(tail[-k:])
            break
if len(raw) != w * h * 2:
    print(f'{w * h * 2 - len(raw)} bytes lost (a log line broke into the image): the rest is shifted', file=sys.stderr)
    raw = raw.ljust(w * h * 2, b'\0')

img = b''
for y in range(h):
    line = bytearray([0])
    for x in range(w):
        v = struct.unpack_from('<H', raw, (y * w + x) * 2)[0]
        line += bytes([(v >> 11) << 3, ((v >> 5) & 63) << 2, (v & 31) << 3]) * SCALE
    img += bytes(line) * SCALE


def chunk(tag, data):
    c = struct.pack('>I', len(data)) + tag + data
    return c + struct.pack('>I', zlib.crc32(tag + data))


with open(out, 'wb') as f:
    f.write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w * SCALE, h * SCALE, 8, 2, 0, 0, 0))
            + chunk(b'IDAT', zlib.compress(img)) + chunk(b'IEND', b''))
print(f'{out}: {w}x{h}')
