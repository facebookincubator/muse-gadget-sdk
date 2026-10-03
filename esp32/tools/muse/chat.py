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

"""Chat with your Muse through a board on USB.

    python3 tools/muse/chat.py [--port PORT] "message"     print the reply as it arrives
    python3 tools/muse/chat.py [--port PORT] --file FILE    send a file's text
    python3 tools/muse/chat.py [--port PORT] --status       the board's state, as JSON

The board posts the text to your Muse as a typed turn and streams the reply
back over its console (muse_hatch_text_turn in components/muse/muse_chat.h),
so this machine needs no token or network setup. Boards with PSRAM (s3, s3-18,
aipi, sticks3, stopwatch, watcher, plus2) can do it; the C6 can't. The Watcher needs
firmware with MUSE_CONSOLE_UART, and its CH342 drops bytes from whole
packets, so writes to it are paced.

As a library: Board(port) opens the board, Board.status() and Board.chat()
do the rest. tools/muse/avatar.py uses them. Needs pyserial.
"""
import argparse
import collections
import errno
import json
import os
import sys
import time

import ports

LINE_BYTES = 480            # message bytes per console line; escaped, a line stays under the board's 1024
ACK_S = 3.0
SEND_S = 90.0               # last line -> "sent": includes connecting to Muse
TURN_S = 16 * 60            # the board ends a typed turn within 15 minutes
BOOT_MARKS = ("ESP-ROM:", "rst:0x")


class BoardError(Exception):
    """Something the person at the keyboard can act on; str() says what."""


def find_ports():
    """Serial ports of boards that take serial commands, by their USB device (ports.py)."""
    try:
        return ports.command_ports()
    except ImportError:
        raise BoardError("pyserial is missing: python3 -m pip install pyserial")


def pick_port():
    found = find_ports()
    if not found:
        raise BoardError("No board found on USB. Plug it in with a data cable "
                         "(charge-only cables don't carry data) and try again.")
    if len(found) > 1:
        raise BoardError("More than one board on USB (%s). Pass --port." % ", ".join(found))
    return found[0]


def escape(data):
    """A message's bytes as one console line's worth: muse_hatch_unescape undoes it."""
    return data.replace(b"\\", b"\\\\").replace(b"\n", b"\\n").replace(b"\r", b"\\r").replace(b"\t", b"\\t")


def split_message(text):
    """The console lines that carry `text` ("chat+=" pieces, then the "chat=" that sends it),
    each with the message bytes the board should have once it's in."""
    data = text.replace("\0", "").encode("utf-8")
    starts = range(0, len(data), LINE_BYTES) if data else [0]
    return [(b">chat" + (b"+=" if i + LINE_BYTES < len(data) else b"=") + escape(data[i:i + LINE_BYTES]) + b"\n",
             min(i + LINE_BYTES, len(data))) for i in starts]


class Reply:
    """A typed turn's reply, put back together from its "@chat" lines."""

    def __init__(self):
        self.parts = collections.defaultdict(list)    # msg -> streamed text
        self.finals = collections.defaultdict(list)   # msg -> the whole text, when the pieces fell short
        self.sizes = {}                               # msg -> bytes, as the board counted them
        self.complete = False                         # ended on its own rather than cut off
        self.lost = 0                                 # lines missing or garbled

    def feed(self, f):
        t, msg = f.get("type"), f.get("msg")
        if t == "text":
            self.parts[msg].append(f.get("text", ""))
        elif t == "final":
            self.finals[msg].append(f.get("text", ""))
        elif t == "message_done":
            self.sizes[msg] = f.get("bytes")

    def message(self, msg):
        return "".join(self.finals[msg] if self.finals.get(msg) else self.parts.get(msg, []))

    @property
    def messages(self):
        return [self.message(m) for m in sorted(set(self.parts) | set(self.finals) | set(self.sizes))]

    @property
    def text(self):
        return "\n\n".join(m for m in self.messages if m)

    def intact(self):
        """Nothing went missing on the cable: every line arrived and every message adds up."""
        return not self.lost and all(len(self.message(m).encode("utf-8")) == n for m, n in self.sizes.items())


class Board:
    def __init__(self, port):
        try:
            import serial
        except ImportError:
            raise BoardError("pyserial is missing: python3 -m pip install pyserial")
        self.port = port
        try:
            self.ser = serial.Serial(port, 115200, timeout=0.05)
        except (serial.SerialException, OSError) as e:
            code = getattr(e, "errno", None)
            if code in (errno.EPERM, errno.EACCES, errno.EBUSY) or "Permission" in str(e) or "busy" in str(e):
                raise BoardError(
                    f"Can't open {port}: {e}. Close anything else using it (a serial monitor, "
                    "idf.py monitor, another agent). On Linux, join the dialout group. "
                    "An agent in a sandbox needs the sandbox off for serial ports.")
            raise BoardError(f"Can't open {port}: {e}")
        self._keep_lines_on_close()
        self.paced = ports.paced(port)
        self.buf = b""
        self.log = collections.deque(maxlen=30)   # recent log lines, for error reports
        self.last_seq = None
        self.lost_lines = 0

    def _keep_lines_on_close(self):
        """Closing drops DTR, and DTR low with RTS high resets the chip: keep them as they are."""
        try:
            import termios

            attrs = termios.tcgetattr(self.ser.fd)
            attrs[2] &= ~termios.HUPCL
            termios.tcsetattr(self.ser.fd, termios.TCSANOW, attrs)
        except (ImportError, AttributeError, OSError):
            pass

    def close(self):
        self.ser.close()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    def write(self, data):
        """Writes and flushes `data`; 64 bytes at a time at the line rate on a paced bridge."""
        step = 64 if self.paced else len(data) or 1
        for i in range(0, len(data), step):
            chunk = data[i:i + step]
            self.ser.write(chunk)
            self.ser.flush()
            if self.paced:
                time.sleep(len(chunk) * 10 / self.ser.baudrate)   # 10 bits a byte on the wire

    def wake(self):
        """Wakes a board whose console UART sleeps (the Watcher): the characters that wake it are lost."""
        if self.paced:
            self.write(b"\n\n")
            time.sleep(0.1)

    def write_line(self, cmd):
        self.write(b">" + cmd.encode() + b"\n")

    def read_line(self, deadline):
        """The next console line, or None at the deadline."""
        while True:
            i = self.buf.find(b"\n")
            if i >= 0:
                line, self.buf = self.buf[:i], self.buf[i + 1:]
                return line.decode("utf-8", "replace").rstrip("\r")
            if time.monotonic() >= deadline:
                return None
            self.buf += self.ser.read(max(1, self.ser.in_waiting))

    def frame(self, deadline, kind="@chat"):
        """The next `kind` line as a dict, or None at the deadline. Other lines go to the log."""
        while True:
            line = self.read_line(deadline)
            if line is None:
                return None
            at = line.find(kind + " {")
            if at < 0:
                if any(m in line for m in BOOT_MARKS):
                    raise BoardError("The board restarted.")
                if line.strip():
                    self.log.append(line)
                continue
            try:
                f = json.loads(line[at + len(kind) + 1:])
            except ValueError:
                self.log.append(line)
                if kind == "@chat":
                    self.last_seq = None   # can't tell what was lost; the next line resyncs
                    self.lost_lines += 1
                continue
            if kind == "@chat":
                seq = f.get("seq")
                if self.last_seq is not None and isinstance(seq, int) and seq > self.last_seq + 1:
                    self.lost_lines += seq - self.last_seq - 1
                self.last_seq = seq
            return f

    def drain(self, secs=0.3):
        end = time.monotonic() + secs
        while self.read_line(end) is not None:
            pass

    def status(self, timeout=3.0):
        """The board's "@status" dict (board, chat, device), or None if it doesn't answer."""
        for _ in range(2):
            self.wake()
            self.write(b"\n")   # ends any half line
            self.write_line("status")
            try:
                f = self.frame(time.monotonic() + timeout, "@status")
            except BoardError:
                self.drain(3.0)   # it just booted: let it settle and ask again
                continue
            if f is not None:
                return f
        return None

    def cancel(self):
        self.write_line("chat.cancel")
        self.drain(0.5)

    def _expect(self, types, secs):
        """The next "@chat" line of one of `types`; raises on "error", None at the deadline."""
        deadline = time.monotonic() + secs
        while True:
            f = self.frame(deadline)
            if f is None or f.get("type") in types:
                return f
            if f.get("type") == "error":
                raise BoardError("Muse: " + f.get("text", "error"))

    def _send(self, text):
        """Types `text` into the board a line at a time, each acknowledged. False to start over."""
        self.wake()
        for line, total in split_message(text):
            self.write(line)
            f = self._expect(("ack",), ACK_S)
            if f is None or f.get("bytes") != total:
                return False
        return self._expect(("sent",), SEND_S) is not None

    def chat(self, text, progress=None):
        """Sends `text` as a typed turn and returns its Reply. progress(frame) sees every line as it comes."""
        self.drain(0.2)
        for attempt in range(3):
            if self._send(text):
                break
            self.cancel()
        else:
            raise BoardError("The board didn't take the message (no answer on the console). "
                             "Is its firmware older than serial chat? " + self.recent_log())
        reply = Reply()
        self.lost_lines = 0
        deadline = time.monotonic() + TURN_S
        while True:
            f = self.frame(deadline)
            if f is None:
                raise BoardError("The reply stopped coming. " + self.recent_log())
            if progress:
                progress(f)
            reply.feed(f)
            t = f.get("type")
            if t == "done":
                reply.complete = bool(f.get("complete"))
                reply.lost = self.lost_lines
                return reply
            if t == "error":
                raise BoardError("Muse: " + f.get("text", "error"))

    def recent_log(self):
        tail = [l for l in self.log if "muse_chat" in l or " E (" in l or " W (" in l][-5:]
        return ("Board log:\n  " + "\n  ".join(tail)) if tail else ""


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("message", nargs="*")
    ap.add_argument("--port")
    ap.add_argument("--file", help="send this file's text (after the message, if any)")
    ap.add_argument("--status", action="store_true", help="print the board's state and stop")
    ap.add_argument("--out", help="also write the reply here")
    args = ap.parse_args()
    try:
        with Board(args.port or pick_port()) as board:
            if args.status:
                st = board.status()
                if st is None:
                    raise BoardError("The board doesn't answer: is its firmware older than the status command?")
                print(json.dumps(st, indent=2))
                return
            text = " ".join(args.message)
            if args.file:
                with open(args.file, encoding="utf-8") as f:
                    text = (text + "\n\n" if text else "") + f.read()
            if not text:
                ap.error("nothing to send")

            def progress(f):
                if f.get("type") in ("text", "final"):
                    sys.stdout.write(f.get("text", ""))
                    sys.stdout.flush()
                elif f.get("type") == "message_done":
                    sys.stdout.write("\n")
                elif f.get("type") == "busy":
                    print("[working]" if f.get("on") else "[idle]", file=sys.stderr)

            try:
                reply = board.chat(text, progress)
            except KeyboardInterrupt:
                board.cancel()
                raise
            if args.out:
                with open(args.out, "w", encoding="utf-8") as f:
                    f.write(reply.text)
            if not reply.intact():
                print(f"warning: {reply.lost} console line(s) went missing; the reply may be incomplete",
                      file=sys.stderr)
            if not reply.complete:
                print("warning: the reply was cut off", file=sys.stderr)
    except BoardError as e:
        sys.exit(str(e))


if __name__ == "__main__":
    main()
