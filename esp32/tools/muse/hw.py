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

"""Runs one of the board's hardware commands over USB serial, as the agent would.

  tools/muse/hw.py COMMAND [PARAMS_JSON] [--port PORT] [--board watcher] [--timeout S] [--log]

  tools/muse/hw.py device.status
  tools/muse/hw.py led.set '{"color": "blue", "effect": "breathe"}'
  tools/muse/hw.py input.read '{"wait_ms": 10000}'

Prints the result's JSON and exits 0 if it was ok, 1 if not, 2 if nothing came
back. The board needs CONFIG_MUSE_HW_COMMANDS (main/muse_hw_commands.c). It
isn't reset: the port opens without a reset pulse (see open_port). --log also prints the
board's log lines while waiting. Needs pyserial.
"""

import argparse
import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ports  # noqa: E402

PIECE = 900        # the console reads lines of up to 1 KB; longer commands go in pieces
PIECE_GAP_S = 0.05


def open_port(port):
    import serial

    s = serial.Serial()
    s.port, s.baudrate, s.timeout = port, 115200, 0.1
    # Opening raises DTR and RTS, and pyserial then sets DTR before RTS. RTS
    # with DTR off pulls EN low through the auto-reset circuit, so drop RTS
    # first (DTR alone only holds the boot pin, which matters at reset), then DTR.
    s.dtr, s.rts = True, False
    s.open()
    s.dtr = False
    s.muse_paced = ports.paced(port)   # a bridge that drops bytes sent at once (the Watcher's CH342)
    return s


def write(s, data):
    """Writes and flushes `data`; 64 bytes at a time at the line rate on a paced bridge."""
    paced = getattr(s, "muse_paced", False)
    step = 64 if paced else len(data) or 1
    for i in range(0, len(data), step):
        chunk = data[i:i + step]
        s.write(chunk)
        s.flush()
        if paced:
            time.sleep(len(chunk) * 10 / s.baudrate)   # 10 bits a byte on the wire


def send(s, line):
    """One console command, in hw+= pieces if it's long. A board whose console
    UART sleeps (the Watcher) is woken first: the characters that wake it are lost."""
    if getattr(s, "muse_paced", False):
        write(s, b"\n\n")
        time.sleep(0.1)
    if len(line) <= PIECE:
        write(s, b">hw " + line.encode() + b"\n")
        return
    for i in range(0, len(line), PIECE):
        piece = line[i:i + PIECE]
        last = i + PIECE >= len(line)
        write(s, (">hw=" if last else ">hw+=").encode() + piece.encode() + b"\n")
        time.sleep(PIECE_GAP_S)


def wait_result(s, request_id, timeout, log):
    buf = b""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        buf += s.read(4096)
        while b"\n" in buf:
            raw, buf = buf.split(b"\n", 1)
            line = raw.decode("utf-8", "replace").strip()
            if line.startswith("@hw "):
                try:
                    msg = json.loads(line[4:])
                except ValueError:
                    continue
                if msg.get("id") == request_id or msg.get("id") is None:
                    return msg.get("result")
            elif log and line:
                print(line, file=sys.stderr)
    return None


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("command")
    ap.add_argument("params", nargs="?", default="{}")
    ap.add_argument("--port")
    ap.add_argument("--board", default="watcher")
    ap.add_argument("--timeout", type=float, default=75.0)
    ap.add_argument("--log", action="store_true")
    a = ap.parse_args(argv[1:])
    try:
        params = json.loads(a.params)
    except ValueError as e:
        print(f"PARAMS_JSON isn't JSON: {e}", file=sys.stderr)
        return 2
    try:
        port = a.port or ports.find(a.board)
    except ports.NotFound as e:
        print(e, file=sys.stderr)
        return 2
    request_id = f"hw-{os.getpid()}-{int(time.time() * 1000) % 100000}"
    s = open_port(port)
    try:
        s.reset_input_buffer()
        send(s, json.dumps({"id": request_id, "command": a.command, "params": params}, separators=(",", ":")))
        result = wait_result(s, request_id, a.timeout, a.log)
    finally:
        s.close()
    if result is None:
        print(f"no answer from {port} within {a.timeout:g} s", file=sys.stderr)
        return 2
    print(json.dumps(result, indent=2))
    return 0 if result.get("ok") else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
