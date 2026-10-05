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

"""Finds a board's serial port by the USB device behind it.

  tools/muse/ports.py BOARD [SERIAL]    prints BOARD's port
  tools/muse/ports.py --list            lists the boards on USB

Port names follow the USB hub position and change when boards are re-cabled,
so boards are found by the USB device carrying their console instead. Several
boards of one kind are told apart by the device's USB serial number, which on
Espressif's own USB Serial/JTAG is the ESP's MAC. BOARD is tools/muse/board.sh's
name. Listing ports doesn't open them, so no board is reset.
"""

import sys

USJ = (0x303A, 0x1001)      # the chip's own USB Serial/JTAG
CH342 = (0x1A86, 0x55D2)    # WCH dual UART bridge: two ports per device
CH9102 = (0x1A86, 0x55D4)   # WCH single UART bridge
CP210X = (0x10C4, 0xEA60)   # Silicon Labs CP2104 bridge


def _usbs(usb):
    """The board.sh name maps to one USB device, or several (the Core2 shipped
    with a CP2104 or a CH9102F bridge). A device is a (vid, pid) pair; several
    is a tuple of pairs."""
    if isinstance(usb, tuple) and usb and isinstance(usb[0], tuple):
        return usb
    return (usb,)


# board.sh's name -> the USB device carrying its console.
USB = {
    "s3": USJ,
    "s3n": USJ,
    "s3-185b": USJ,
    "aipi": USJ,
    "box3": USJ,
    "c6": USJ,
    "sticks3": USJ,
    "cardputer-adv": USJ,
    "stopwatch": USJ,
    "cores3": USJ,
    "fnk0104b": USJ,
    "jc3248w535": USJ,
    "watcher": CH342,   # the ESP32-S3 on the second port; the Himax camera chip is on the first
    "plus2": CH9102,
    "core2": (CP210X, CH9102),
}
# Boards whose console takes Muse's serial commands (tools/muse/chat.py). The
# Watcher reads them on its CH342 port with MUSE_CONSOLE_UART.
COMMANDS = ("s3", "s3n", "s3-185b", "aipi", "box3", "c6", "sticks3", "watcher", "plus2", "cardputer-adv", "stopwatch", "cores3", "core2", "fnk0104b", "jc3248w535")
# Bridges that drop bytes when a whole packet arrives at once, so writes to them
# go 64 bytes at a time at the line rate (paced_esptool.py, chat.Board.write).
PACED = (CH342,)
KINDS = {USJ: "Espressif USB Serial/JTAG", CH342: "CH342 bridge", CH9102: "CH9102 bridge",
         CP210X: "CP2104 bridge"}


class NotFound(Exception):
    """No one port fits; str() says what was found."""


def comports():
    from serial.tools import list_ports

    return list_ports.comports()


def devices(usb, ports=None):
    """One port per attached USB device of this kind: a two-port bridge's second."""
    kinds = _usbs(usb)
    found = {}
    for p in sorted(comports() if ports is None else ports, key=lambda p: p.device):
        if (p.vid, p.pid) in kinds:
            found[p.serial_number or p.device] = p
    return list(found.values())


def find(board, serial=None, ports=None):
    """BOARD's port, or NotFound. SERIAL picks one of several (or is a port)."""
    if serial and serial.startswith("/dev/"):
        return serial
    usb = USB[board]
    cands = devices(usb, ports)
    if serial:
        cands = [p for p in cands if (p.serial_number or "").lower() == serial.lower()]
    if len(cands) == 1:
        return cands[0].device
    kind = " / ".join(KINDS[u] for u in _usbs(usb))
    if not cands:
        raise NotFound(f"No {board} board on USB (no {kind}{' ' + serial if serial else ''}).")
    listed = ", ".join(f"{p.device} ({p.serial_number})" for p in cands)
    raise NotFound(f"More than one {kind} on USB: {listed}. Pass the {board}'s serial number.")


def paced(port, ports=None):
    """Whether writes to PORT have to be paced: it's on a bridge in PACED."""
    return any(p.device == port and (p.vid, p.pid) in PACED
               for p in (comports() if ports is None else ports))


def command_ports(ports=None):
    """Ports of boards that take serial commands, USB Serial/JTAG first."""
    kinds = dict.fromkeys(USB[b] for b in COMMANDS)
    return [p.device for usb in kinds for p in devices(usb, ports)]


def main(argv):
    if argv[1:2] == ["--list"]:
        for usb, kind in KINDS.items():
            boards = "/".join(b for b, u in USB.items() if usb in _usbs(u))
            for p in devices(usb):
                print(f"{p.device}  {kind} {p.serial_number}  ({boards})")
        return 0
    if len(argv) not in (2, 3) or argv[1] not in USB:
        print(__doc__.split("\n\n")[1] + "\nboards: " + " ".join(USB), file=sys.stderr)
        return 2
    try:
        print(find(argv[1], argv[2] if len(argv) == 3 else None))
    except NotFound as e:
        print(e, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
