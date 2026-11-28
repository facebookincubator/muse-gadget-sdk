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
"""The smp_link harness's supervisor and plumbing (README.md).

  run.py serve --exe zephyr.exe --flash flash.bin [--init IMAGE] [--port 47000] [-- server args]
      Runs the gadget: each start is a boot (MCUboot, then the app); status 77
      (an SMP reset, a crash) starts it again, as a reset would. --init writes a
      fresh flash with IMAGE in slot 0 (as flashed over SWD). The file
      FLASH.next-args, if present, is read before every boot: one line of
      extra server arguments for that boot (then deleted), e.g. --crash-ms=2000.

  run.py relay-in  --tcp 47100 --udp 47000     (inside the container)
  run.py relay-out --tcp 47100 --udp 47000     (on the Mac)
      colima forwards TCP only: the two ends carry the UDP link (smplink.py) over
      one TCP connection, datagram for datagram, so clients on the Mac reach a
      server in the container at 127.0.0.1:UDP. relay-in hands the client's
      writes to the gadget like a BLE link does: at most --per-event of them
      every --event-ms (default 10 per 7.5 ms, twice the XIAO's 2M / 15 ms
      link), so a burst of hundreds of small writes (MTU 23) isn't lost in the
      gadget's UDP receive buffer. BLE never drops a write without response.

  run.py image IN OUT --version X.Y.Z+B [--header-size 0x800 --align 16 --slot-size 729088]
      Re-signs a board image with another version (imgtool, MCUboot's dev key):
      a second, different image of the same code.
"""
import argparse
import collections
import os
import selectors
import shlex
import socket
import struct
import shutil
import subprocess
import sys
import tempfile
import time

# The XIAO's RRAM map (boards/native_sim.overlay).
FLASH_SIZE = 0x17D000
SLOT0, SLOT1, SLOT_SIZE = 0x10000, 0xC2000, 0xB2000
EXIT_REBOOT = 77


def write_private(path, data):
    """A board image (and a flash holding one) may carry the owner's SDK
    token: owner-only from the first byte."""
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, "wb") as f:
        os.fchmod(f.fileno(), 0o600)
        f.write(data)


def serve(a):
    if a.init:
        img = open(a.init, "rb").read()
        assert len(img) <= int(a.slot_size, 0), "image larger than slot 0"
        slot0 = int(a.slot0, 0)
        flash = bytearray(b"\xff" * int(a.flash_size, 0))
        flash[slot0:slot0 + len(img)] = img
        write_private(a.flash, flash)
        print(f"run: fresh flash, slot 0 = {a.init} ({len(img)} B)", flush=True)
    boot = 0
    while True:
        boot += 1
        extra = list(a.args)
        nxt = a.flash + ".next-args"
        if os.path.exists(nxt):
            extra += shlex.split(open(nxt).read())
            os.remove(nxt)
        cmd = [a.exe, f"--flash={a.flash}", f"--port={a.port}", f"--boot={boot}", *extra]
        print(f"run: boot {boot}: {' '.join(cmd[1:])}", flush=True)
        rc = subprocess.call(cmd)
        if rc != EXIT_REBOOT:
            print(f"run: gadget exited ({rc})", flush=True)
            return rc
        time.sleep(a.reset_ms / 1000.0)


def _frame(d):
    return struct.pack(">H", len(d)) + d


class _TcpFrames:
    def __init__(self, s):
        self.s, self.buf = s, b""

    def feed(self):
        chunk = self.s.recv(65536)
        if not chunk:
            raise ConnectionError("closed")
        self.buf += chunk
        out = []
        while len(self.buf) >= 2:
            n = struct.unpack(">H", self.buf[:2])[0]
            if len(self.buf) < 2 + n:
                break
            out.append(self.buf[2:2 + n])
            self.buf = self.buf[2 + n:]
        return out


def relay_in(a):
    """Container end: TCP from the Mac <-> UDP to the gadget at 127.0.0.1:udp."""
    ls = socket.socket()
    ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    ls.bind(("0.0.0.0", a.tcp))
    ls.listen(1)
    print(f"relay-in: TCP {a.tcp} <-> UDP 127.0.0.1:{a.udp}", flush=True)
    while True:
        c, _ = ls.accept()
        c.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        u = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        u.connect(("127.0.0.1", a.udp))
        tf = _TcpFrames(c)
        sel = selectors.DefaultSelector()
        sel.register(c, selectors.EVENT_READ, "tcp")
        sel.register(u, selectors.EVENT_READ, "udp")
        queue = collections.deque()
        next_event = time.monotonic()
        try:
            while True:
                timeout = max(0.0, next_event - time.monotonic()) if queue else None
                for key, _ in sel.select(timeout):
                    if key.data == "tcp":
                        queue.extend(tf.feed())
                    else:
                        try:
                            c.sendall(_frame(u.recv(70000)))
                        except ConnectionRefusedError:
                            pass
                now = time.monotonic()
                if queue and now >= next_event:
                    # One connection event's worth of writes.
                    for _ in range(min(a.per_event, len(queue))):
                        try:
                            u.send(queue.popleft())
                        except ConnectionRefusedError:
                            pass  # the gadget is restarting
                    next_event = now + a.event_ms / 1000.0
        except (ConnectionError, OSError):
            pass
        finally:
            sel.close()
            c.close()
            u.close()


def relay_out(a):
    """Mac end: UDP from clients at 127.0.0.1:udp <-> TCP to the container."""
    u = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    u.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1 << 20)
    u.bind(("127.0.0.1", a.udp))
    print(f"relay-out: UDP 127.0.0.1:{a.udp} <-> TCP 127.0.0.1:{a.tcp}", flush=True)
    peer = None
    while True:
        try:
            c = socket.create_connection(("127.0.0.1", a.tcp))
        except OSError:
            time.sleep(0.5)
            continue
        c.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        tf = _TcpFrames(c)
        sel = selectors.DefaultSelector()
        sel.register(c, selectors.EVENT_READ, "tcp")
        sel.register(u, selectors.EVENT_READ, "udp")
        try:
            while True:
                for key, _ in sel.select():
                    if key.data == "udp":
                        d, peer = u.recvfrom(70000)
                        c.sendall(_frame(d))
                    else:
                        for d in tf.feed():
                            if peer:
                                u.sendto(d, peer)
        except (ConnectionError, OSError):
            time.sleep(0.5)
        finally:
            sel.unregister(u)
            sel.close()
            c.close()


def private_dir(path):
    """True when path's directory is the caller's own and mode 0700 (no group or other access)."""
    d = os.path.dirname(os.path.abspath(path)) or "."
    st = os.stat(d)
    return st.st_uid == os.getuid() and (st.st_mode & 0o077) == 0


def image(a):
    imgtool = os.path.expanduser(a.imgtool)
    key = os.path.expanduser(a.key)
    raw = open(a.input, "rb").read()
    # An image with the Muse Gadgets SDK token compiled in is a secret (AGENTS.md):
    # its re-signed copies and the temporary payload go only into a private
    # (0700) directory, files 0600.
    # Test images don't need the token: build them without one.
    if b"mgst_" in raw and not private_dir(a.output):
        sys.exit(f"image: {a.input} has the SDK token compiled in; refusing to write it to "
                 f"{os.path.dirname(os.path.abspath(a.output))} (not a 0700 directory of yours). "
                 "Build the test image without the token, or write to a private directory.")
    os.umask(0o077)
    hs = int(a.header_size, 0)
    hdr_len = struct.unpack_from("<H", raw, 8)[0] if raw[:4] == b"\x3d\xb8\xf3\x96" else 0
    if hdr_len:
        # A signed image: take its payload back out (header area of zeros, as built).
        size = struct.unpack_from("<I", raw, 12)[0]
        raw = (b"" if a.pad_header else bytes(hs)) + raw[hdr_len:hdr_len + size]
    work = tempfile.mkdtemp(prefix="mg-smp-image-")  # owner-only
    try:
        tmp = os.path.join(work, "in.bin")
        write_private(tmp, raw)
        # imgtool truncates and rewrites the file, keeping its mode.
        write_private(a.output, b"")
        cmd = [sys.executable, imgtool, "sign", "--version", a.version, "--header-size",
               a.header_size, "--slot-size", str(a.slot_size), "--align", str(a.align), "--key",
               key, tmp, a.output]
        if a.pad_header:
            cmd[3:3] = ["--pad-header", "--max-sectors", "256"]
        subprocess.run(cmd, check=True)
    finally:
        shutil.rmtree(work, ignore_errors=True)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("serve")
    s.add_argument("--exe", required=True)
    s.add_argument("--flash", required=True)
    s.add_argument("--init")
    s.add_argument("--port", type=int, default=47000)
    s.add_argument("--reset-ms", type=int, default=200, help="pause between boots")
    s.add_argument("--slot0", default=hex(SLOT0), help="slot 0's offset")
    s.add_argument("--slot-size", default=hex(SLOT_SIZE))
    s.add_argument("--flash-size", default=hex(FLASH_SIZE))
    s.add_argument("args", nargs="*")
    for name in ("relay-in", "relay-out"):
        r = sub.add_parser(name)
        r.add_argument("--tcp", type=int, default=47100)
        r.add_argument("--udp", type=int, default=47000)
        r.add_argument("--event-ms", type=float, default=7.5)
        r.add_argument("--per-event", type=int, default=10)
    i = sub.add_parser("image")
    i.add_argument("input")
    i.add_argument("output")
    i.add_argument("--version", required=True)
    i.add_argument("--header-size", default="0x800")
    i.add_argument("--align", type=int, default=16)
    i.add_argument("--slot-size", type=lambda v: int(v, 0), default=729088)
    i.add_argument("--pad-header", action="store_true", help="the input has no header room (an app_to_sign.bin)")
    i.add_argument("--imgtool", default="~/zephyrproject/bootloader/mcuboot/scripts/imgtool.py")
    i.add_argument("--key", default="~/zephyrproject/bootloader/mcuboot/root-ec-p256.pem")
    a = p.parse_args()
    return {"serve": serve, "relay-in": relay_in, "relay-out": relay_out, "image": image}[a.cmd](a)


if __name__ == "__main__":
    sys.exit(main() or 0)
