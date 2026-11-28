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
"""The bench console of an ESP-Mosaico over ESP-Iris: its console is on a UART header, not its USB-C.

    python3 tools/muse/iris_bench.py line '>ui.menu'      # one console line (1: a command took it)
    python3 tools/muse/iris_bench.py uidump OUT.log      # the last UI picture, as >ui.dump prints it
    python3 tools/muse/uidump.py OUT.log PREFIX --ref PHONE.png

It talks to the firmware's bench methods (components_mosaico/muse_mosaico_iris) through Espressif's
mosaico.py workspace (MOSAICO_WORKSPACE, default ~/mosaico/esp-mosaico-vibe) and the Gateway of a
project there (--project, default projects/muse_gadget), as tools/muse/board.sh flash mosaico does.
Opening the board's USB needs a process outside a sandbox.
"""
import argparse
import base64
import contextlib
import io
import json
import os
import struct
import sys
import time
import urllib.error
import urllib.request
import uuid
from pathlib import Path

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import redact  # noqa: E402  (tools/redact.py: never print a secret the board's output carries)

ROOT = Path(__file__).resolve().parents[2]
WORKSPACE = Path(os.environ.get("MOSAICO_WORKSPACE", Path.home() / "mosaico/esp-mosaico-vibe"))
TOOLS = WORKSPACE / "submodule/esp-mosaico-utils/mosaico-tools"
SERVICE = 0x4D42   # muse_mosaico_iris.c: BENCH_RPC_SERVICE
M_LINE, M_DUMP, M_BOOT = 1, 2, 3
# A 480 px picture's dump is about 1 MB: an answer past these is a broken or hostile one.
MAX_RESPONSE = 4 << 20
MAX_DUMP = 32 << 20


class Device:
    def __init__(self, url, device_id):
        self.url, self.device_id = url.rstrip("/"), device_id

    def rpc(self, method, payload=b"", deadline_ms=3000, retries=3):
        body = json.dumps({"service_id": SERVICE, "method_id": method, "deadline_ms": deadline_ms,
                           "payload_base64": base64.b64encode(payload).decode()}).encode()
        for attempt in range(retries):
            req = urllib.request.Request(f"{self.url}/v1/devices/{self.device_id}/rpc/raw", data=body, method="POST",
                                         headers={"Content-Type": "application/json", "X-Operation-ID": str(uuid.uuid4())})
            try:
                try:
                    with urllib.request.urlopen(req, timeout=deadline_ms / 1000 + 10) as resp:
                        raw = resp.read(MAX_RESPONSE + 1)
                    if len(raw) > MAX_RESPONSE:
                        raise RuntimeError(f"RPC {method}: an answer over {MAX_RESPONSE >> 20} MB")
                    out = json.loads(raw)
                except urllib.error.HTTPError as e:
                    try:
                        text = e.read(601)[:600]
                    finally:
                        e.close()
                    raise RuntimeError(f"RPC {method} HTTP {e.code}: "
                                       f"{redact.redact_text(text.decode('utf-8', 'replace'))!r}") from None
                op = out.get("operation", {})
                if op.get("status") not in (None, "succeeded"):
                    raise RuntimeError(f"RPC {method} {op.get('status')}: {json.dumps(op)[:400]}")
                return base64.b64decode(out.get("payload_base64") or "")
            except (urllib.error.URLError, RuntimeError) as error:
                if attempt == retries - 1:
                    raise
                print(f"  retry {method}: {error}", file=sys.stderr)
                time.sleep(0.5)


def line(dev, text):
    return dev.rpc(M_LINE, text.encode())[:1] == b"\x01"


def uidump(dev, out, timeout=30, read_timeout=300):
    """Asks for the dump into the board's buffer, waits for it, reads it whole, writes OUT."""
    if not line(dev, ">ui.dump=buf"):
        raise SystemExit("the board took no ui.dump=buf (not a UI-stream build?)")
    t0 = time.time()
    while True:
        r = dev.rpc(M_DUMP, struct.pack("<I", 0))
        if len(r) < 4:
            raise SystemExit(f"a {len(r)} B dump header")
        total = struct.unpack_from("<I", r)[0]
        if total:
            break
        if time.time() - t0 > timeout:
            raise SystemExit("no dump: is a UI stream showing a picture?")
        time.sleep(0.3)
    try:
        if total > MAX_DUMP:
            raise SystemExit(f"refusing a {total} B dump (over {MAX_DUMP >> 20} MB)")
        data, t1 = bytearray(r[4:4 + total]), time.time()
        while len(data) < total:
            if time.time() - t1 > read_timeout:
                raise SystemExit(f"dump read timed out at {len(data)} of {total} bytes")
            r = dev.rpc(M_DUMP, struct.pack("<I", len(data)))
            if len(r) <= 4:
                raise SystemExit(f"dump read stopped at {len(data)} of {total} bytes")
            data += r[4:4 + total - len(data)]
    finally:
        with contextlib.suppress(Exception):   # the board frees its copy (1 MB of PSRAM), after a failure too
            dev.rpc(M_DUMP, struct.pack("<I", 0xFFFFFFFF))
    Path(out).write_bytes(bytes(data))
    return {"bytes": total, "read_s": round(time.time() - t1, 1), "end": data.rstrip().endswith(b"@uidump end")}


def with_device(device_id, no_mdns, fn, project=None):
    """Starts or joins the project Gateway with mosaico.py's own session code, then calls fn(Device)."""
    sys.path.insert(0, str(TOOLS / "tools"))
    from mosaico_cli import cli
    from mosaico_cli.gateway import connected_devices, ensure_gateway, select_device
    if no_mdns:   # for a host where another process holds UDP 5353 (adb's mDNS on macOS)
        from mosaico_cli import iris, session_runtime
        orig = iris.host_api

        def host_api(workspace):
            api = orig(workspace)
            la = api.LocalProject.launch_arguments
            if not getattr(api.LocalProject, "_no_mdns", False):
                api.LocalProject.launch_arguments = lambda self, sid: la(self, sid) + ["--no-discover-mdns"]
                api.LocalProject._no_mdns = True
            return api

        iris.host_api = session_runtime.host_api = host_api
    result = {}

    def grab(arguments, context):
        session = ensure_gateway(context, None)
        device = select_device(connected_devices(context, session), device_id)
        result["out"] = fn(Device(session.connection_args[1], str(device["device_id"])))
        return {"command": "rpc", "status": "succeeded", "device_id": str(device["device_id"]), "response": result["out"]}

    cli.invoke_rpc = grab
    if project:
        project = Path(project).expanduser().resolve()
        if not project.exists():
            raise SystemExit(f"no project at {project}")
    else:
        project = WORKSPACE / "projects/muse_gadget"
        if not project.exists():   # board.sh links it on the first flash
            (WORKSPACE / "projects").mkdir(exist_ok=True)
            project.symlink_to(ROOT)
    argv = ["--json", "iris", "rpc", str(SERVICE), str(M_LINE), "--project", str(project)]
    if device_id:
        argv += ["--device-id", device_id]
    buf = io.StringIO()
    os.chdir(WORKSPACE)
    with contextlib.redirect_stdout(buf):
        rc = cli.main(argv, tool_root=TOOLS)
    if rc:
        print(redact.redact_text(buf.getvalue()))
        raise SystemExit(rc)
    return result.get("out")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--device-id")
    # The board is on USB: its Gateway needs no mDNS, and with it the Gateway fails to
    # start ("[Errno 48] Address already in use", zeroconf binding UDP 5353) whenever
    # another process holds that port without sharing it (adb's mDNS while a phone is
    # on adb), so most console lines failed during phone tests. --mdns turns it back on.
    ap.add_argument("--mdns", action="store_true", help="start the Gateway with mDNS discovery (off by default)")
    ap.add_argument("--no-mdns", action="store_true", help=argparse.SUPPRESS)   # the default now
    ap.add_argument("--project", help="mosaico.py project whose Gateway to use (default: projects/muse_gadget)")
    ap.add_argument("cmd", choices=["line", "uidump", "boot"])
    ap.add_argument("arg", nargs="?", default="")
    a = ap.parse_args()
    out = Path(a.arg).resolve() if a.cmd == "uidump" else None
    if a.cmd == "boot":   # how the running boot began: intentional restart, crash, power-on (muse_restart.h)
        fn = lambda d: d.rpc(M_BOOT).decode(errors="replace")
    else:
        fn = (lambda d: line(d, a.arg)) if a.cmd == "line" else (lambda d: uidump(d, out))
    print(json.dumps(redact.redact_record(with_device(a.device_id, not a.mdns, fn, a.project))))


if __name__ == "__main__":
    main()
