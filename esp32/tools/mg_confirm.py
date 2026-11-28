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

"""Confirm a gadget's Muse Link setup over its serial console, for automated tests.

    python3 tools/mg_confirm.py PORT                  wait for "@pair.pending", answer
                                                      ">pair.confirm", exit 0 once confirmed
    python3 tools/mg_confirm.py PORT --count 0 --secs 1800 --log console.log
                                                      keep answering every setup for 30 min,
                                                      saving the console
    python3 tools/mg_confirm.py PORT --now            confirm the setup waiting right now

Setup over Muse Link (protocols/README.md, Gadget setup over Muse Link) waits
for one press of the gadget's button or one tap on its Confirm after
confirm_required (a set-up gadget's re-setup never by the talk key outside its
setup mode). On a test rig
nobody is there to press it, so the firmware takes the press over the USB
serial console:

- ESP32 boards with the Muse UI (components/muse, every musegadgets build)
  built for the bench (MUSE_BENCH=1, CONFIG_MUSE_PAIR_CONFIRM_CONSOLE): the
  console line ">pair.confirm".
- The Zephyr gadget's bench build (zephyr/overlay-bench.conf): the same line.

Both print "@pair.pending" when a confirmation starts waiting, and answer
">pair.confirm" with "@pair.confirm confirmed", or "@pair.confirm none" if
nothing was waiting (never push-to-talk). A release build of either answers
"@pair.confirm disabled": only the button confirms there. The console is the USB cable, so
this asks for the same physical access as the button.

The port is opened without touching DTR or RTS (an ESP32's USB serial resets
on some transitions) and reopened if the board restarts. The Muse app or
`tools/mg_ble_client.py setup` drives the other side; on a host where one
process can't have both Bluetooth and the serial port (a sandboxed agent),
run this one on its own. `mg_ble_client.py setup --confirm-serial PORT` does
both in one process where it can.
"""

from __future__ import annotations

import argparse
import os
import sys
import termios
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import redact  # noqa: E402  (tools/redact.py: the console log never keeps a secret)

PENDING = b"@pair.pending"
COMMAND = b">pair.confirm\n"
CONFIRMED = b"@pair.confirm confirmed"
NONE = b"@pair.confirm none"
DISABLED = b"@pair.confirm disabled"


class ConfirmDisabled(RuntimeError):
    """The firmware doesn't take the console's press (not a bench build)."""

    def __init__(self):
        super().__init__("this build doesn't confirm setup over the console (\"@pair.confirm disabled\"): "
                         "build it for the bench (ESP32: MUSE_BENCH=1, CONFIG_MUSE_PAIR_CONFIRM_CONSOLE; Zephyr: "
                         "overlay-bench.conf, CONFIG_MG_AUTOMATION), or confirm on the gadget")


class Console:
    """A serial console at 115200 8N1 that never changes DTR or RTS."""

    def __init__(self, port: str, log: str | None = None, echo=None):
        self.port = port
        self.fd: int | None = None
        # The saved console: private (an existing file too), and with any secret a line carries masked.
        self.log = None
        if log:
            fd = os.open(log, os.O_WRONLY | os.O_CREAT | os.O_APPEND, 0o600)
            os.fchmod(fd, 0o600)
            self.log = os.fdopen(fd, "ab", buffering=0)
        self.echo = echo
        self.lines: list[bytes] = []
        self._partial = b""

    def _open(self) -> bool:
        try:
            fd = os.open(self.port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        except OSError:
            return False
        try:
            a = termios.tcgetattr(fd)
            a[0] = 0
            a[1] = 0
            a[2] = (a[2] & ~(termios.HUPCL | termios.CSIZE | termios.PARENB | termios.CSTOPB)) \
                | termios.CS8 | termios.CLOCAL | termios.CREAD
            a[3] = 0
            a[4] = a[5] = termios.B115200
            termios.tcsetattr(fd, termios.TCSANOW, a)
        except termios.error:
            pass   # a pty in tests; a real port takes the settings
        self.fd = fd
        return True

    def close(self) -> None:
        self._close_port()
        if self.log:
            if self._partial:
                self.log.write(redact.redact_bytes(self._partial))
            self.log.close()
            self.log = None

    def _close_port(self) -> None:
        if self.fd is not None:
            os.close(self.fd)
            self.fd = None

    def read_lines(self, wait: float = 0.05) -> list[bytes]:
        """Whole lines read since the last call; waits up to `wait` for some."""
        if self.fd is None and not self._open():
            time.sleep(wait)
            return []
        try:
            chunk = os.read(self.fd, 4096)
        except BlockingIOError:
            time.sleep(wait)
            return []
        except OSError:
            self._close_port()   # unplugged or restarting: reopen on the next read
            return []
        if not chunk:
            self._close_port()   # end of file: the port hung up (unplugged, re-enumerated); reopen on the next read
            time.sleep(wait)
            return []
        data = self._partial + chunk
        *whole, self._partial = data.split(b"\n")
        lines = [ln.rstrip(b"\r") for ln in whole]
        for ln in lines:
            if self.log or self.echo:
                shown = redact.redact_bytes(ln)
                if self.log:
                    self.log.write(shown + b"\n")
                if self.echo:
                    self.echo(shown)
        return lines

    def send(self, data: bytes) -> bool:
        if self.fd is None and not self._open():
            return False
        try:
            os.write(self.fd, data)
            return True
        except OSError:
            self._close_port()
            return False


def answer(console: Console, timeout: float) -> bool | None:
    """Send ">pair.confirm"; True if confirmed, False for "none", None for no answer.
    Raises ConfirmDisabled if the build doesn't take the line."""
    if not console.send(COMMAND):
        return None
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        for ln in console.read_lines():
            if CONFIRMED in ln:
                return True
            if NONE in ln:
                return False
            if DISABLED in ln:
                raise ConfirmDisabled()
    return None


def confirm_now(port: str, tries: float = 5.0, log: str | None = None) -> bool:
    """Confirm the setup waiting now, retrying "none" for `tries` seconds (the
    firmware notices confirm_required a moment after the client sees it)."""
    console = Console(port, log)
    try:
        end = time.monotonic() + tries
        while time.monotonic() < end:
            if answer(console, 2.0):
                return True
            time.sleep(0.3)
        return False
    finally:
        console.close()


def _say(message: str) -> None:
    print(message, flush=True)   # live, even into a file


def watch(port: str, count: int, secs: float, log: str | None = None, out=_say) -> int:
    """Answer each "@pair.pending" until `count` confirmations (0: any number) or `secs`."""
    console = Console(port, log)
    done = 0
    end = time.monotonic() + secs
    try:
        while time.monotonic() < end:
            for ln in console.read_lines():
                if PENDING not in ln:
                    continue
                try:
                    result = answer(console, 3.0)
                except ConfirmDisabled as e:
                    out(str(e))
                    return 2
                if result:
                    done += 1
                    out(f"confirmed setup ({done})")
                    if count and done >= count:
                        return 0
                else:
                    out("setup was waiting, but the confirmation didn't take: "
                        + ("nothing pending" if result is False else "no answer"))
        return 0 if count == 0 else 1
    finally:
        console.close()


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("port", help="the gadget's serial console, e.g. /dev/cu.usbmodem1101")
    ap.add_argument("--count", type=int, default=1, help="confirmations to make before exiting (0: no limit)")
    ap.add_argument("--secs", type=float, default=120.0, help="give up (or, with --count 0, stop) after this long")
    ap.add_argument("--now", action="store_true", help="confirm the setup waiting now, without waiting for @pair.pending")
    ap.add_argument("--log", help="append everything the console prints to this file")
    args = ap.parse_args(argv)
    if args.now:
        try:
            ok = confirm_now(args.port, log=args.log)
        except ConfirmDisabled as e:
            print(e)
            return 2
        print("confirmed setup" if ok else "nothing to confirm")
        return 0 if ok else 1
    rc = watch(args.port, args.count, args.secs, args.log)
    if rc:
        print(f"no setup confirmed within {args.secs:.0f} s")
    return rc


if __name__ == "__main__":
    sys.exit(main())
