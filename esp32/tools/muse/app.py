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

"""Run, install and inspect WebAssembly apps on a board with CONFIG_MUSE_WASM_APPS (MUSE_APPS=1 builds).

    python3 tools/muse/app.py [--port PORT] APP            send APP over USB and run it: a .wasm
                                                           file, or an app's folder holding app.wasm
    python3 tools/muse/app.py [--port PORT] --vm PATH      run an app from the VM, over Noise, as
                                                           app.run does: its folder
                                                           (workspace/gadgets/apps/clock) or a .wasm file
    python3 tools/muse/app.py [--port PORT] --put PATH APP write APP to PATH on the VM through the board;
                                                           a PATH not ending in .wasm is an app's
                                                           folder, and APP goes in as PATH/app.wasm
    python3 tools/muse/app.py [--port PORT] --install PATH [--id ID] [--name NAME] [--icon GLYPH] [--color COLOUR]
                                                           install an app from the VM, as app.install does:
                                                           what isn't given comes from its manifest
    python3 tools/muse/app.py [--port PORT] --install-file APP [--id ID] [--name NAME] [--icon ...] [--color ...]
                                                           install APP from this machine, over USB
    python3 tools/muse/app.py [--port PORT] --run ID       run an installed app, as app.run {id} does
    python3 tools/muse/app.py [--port PORT] --remove ID    remove an installed app
    python3 tools/muse/app.py [--port PORT] --list         the installed apps (app.list)
    python3 tools/muse/app.py [--port PORT] --launcher     open the launcher, as Settings > Apps does
    python3 tools/muse/app.py [--port PORT] --key          the power key: closes the app (back to the
                                                           launcher if it came from there) or the launcher
    python3 tools/muse/app.py [--port PORT] --files        the gadget's own files (files.list)
    python3 tools/muse/app.py [--port PORT] --file-put PATH [--as NAME]
                                                           copy a file from the VM to the gadget's flash,
                                                           as files.put does
    python3 tools/muse/app.py [--port PORT] --file-upload FILE [--as NAME]
                                                           copy one from this machine over USB (256 KB at most)
    python3 tools/muse/app.py [--port PORT] --file-remove NAME
                                                           remove one
    python3 tools/muse/app.py [--port PORT] --stop         close the running app
    python3 tools/muse/app.py [--port PORT] --status       the running app, and how the last one ended
    python3 tools/muse/app.py [--port PORT] --logs         the app's recent log lines (app.logs)
    python3 tools/muse/app.py [--port PORT] --api [--out api.capabilities.json]
                                                           the machine-readable API (app.api,
                                                           every part); --out writes the API's parts,
                                                           joined: api.capabilities.json, for components/muse_apps/sdk/build.py --capabilities
    python3 tools/muse/app.py [--port PORT] --event TYPE,A,B
                                                           hand the app an input event, as a touch or
                                                           press would (EV_ numbers in components/muse_apps/sdk/gadget.h)

Prints the board's answer as JSON: {"ok": true, "payload": {...}}, or
{"ok": false, "error": {...}}. The console lines behind it are in muse_apps.h:
the upload goes in base64 pieces of "app+=" lines, each acknowledged before the
next. --vm and --put need the board paired and connected to its VM: --put
writes with the gadget's own credentials (POST /fs/write), so Muse can then run
the app with app.run. A VM that doesn't let gadgets write answers HTTP 403.
Build apps with components/muse_apps/sdk/build.py. Needs pyserial.
"""
import argparse
import base64
import json
import os
import sys
import time

from chat import Board, BoardError, pick_port

PIECE = 768  # base64 characters per line: under the console's 1024


def answer(board, timeout):
    """The next "@app {...}" line as a dict."""
    f = board.frame(time.monotonic() + timeout, "@app")
    if f is None:
        raise BoardError("The board didn't answer. Is its firmware built with MUSE_APPS=1?\n"
                         + "\n".join(board.log))
    return f


def command(board, line, timeout=5):
    """One console line, and its answer."""
    board.write(b"\n")  # ends any half line
    board.write_line(line)
    return answer(board, timeout)


def send(board, data, put=None, install=None, file=None):
    """Uploads data; then runs it, or with put writes it to that path on the VM, or with
    install ("[ID[:ICON:COLOR]] [NAME]") installs it, or with file keeps it as that file
    on the gadget."""
    text = base64.b64encode(data).decode()
    f = command(board, "app.begin")
    if not f.get("ok"):
        return f
    pieces = [text[i:i + PIECE] for i in range(0, len(text), PIECE)] or [""]
    sent = 0
    for piece in pieces if put or install is not None or file else pieces[:-1]:
        board.write_line("app+=" + piece)
        f = answer(board, 5)
        if not f.get("ok"):
            return f
        # The board counts what it decoded: a line cut short on the way shows as fewer bytes.
        sent += len(base64.b64decode(piece))
        got = f.get("bytes")
        if got != sent:
            raise BoardError("The board has %r bytes of the %d sent: the upload was cut short" % (got, sent))
    if put:
        board.write_line("app.put=" + put)
        return answer(board, 60)
    if install is not None:
        board.write_line("app.install.upload=" + install)
        return answer(board, 30)
    if file:
        board.write_line("files.upload=" + file)
        return answer(board, 30)
    board.write_line("app=" + pieces[-1])
    return answer(board, 30)


def fetch_api(board, out=None):
    """Fetch app.api parts without duplicating device.guide's runtime discovery."""
    first = command(board, "app.api=0")
    if not first.get("ok"):
        return first
    payload = first.get("payload")
    parts = payload.get("parts") if isinstance(payload, dict) else None
    if not isinstance(parts, int) or isinstance(parts, bool) or not 1 <= parts <= 256:
        raise BoardError("The API's part count is invalid")
    texts = []
    for k in range(parts):
        response = first if k == 0 else command(board, "app.api=%d" % k)
        if not response.get("ok"):
            return response
        part = response.get("payload")
        if (not isinstance(part, dict) or part.get("part") != k or part.get("parts") != parts
                or not isinstance(part.get("text"), str)):
            raise BoardError("The API's parts are missing, out of order or inconsistent")
        texts.append(part["text"])
    text = "".join(texts)
    try:
        api = json.loads(text)
    except ValueError as e:
        raise BoardError("The API's parts don't join into JSON: %s" % e)
    if not isinstance(api, dict) or not isinstance(api.get("functions"), dict) or not isinstance(api.get("api"), str):
        raise BoardError("The joined API has no version or functions")
    result = {"ok": True, "payload": {"api": api["api"], "parts": parts,
              "bytes": len(text.encode("utf-8")), "functions": len(api["functions"])}}
    if out:
        with open(out, "w", encoding="utf-8") as fh:
            fh.write(text + "\n")
        result["payload"]["written"] = out
    return result


def install_line(args):
    """The console's "ID[:ICON:COLOR] [NAME]": ID "-", or nothing at all, takes the manifest's name."""
    look = ":%s:%s" % (args.icon or "", args.color or "") if args.icon or args.color else ""
    if not (args.id or look or args.name):
        return ""
    return word((args.id or "-") + look) + (" " + args.name if args.name else "")


def word(s, limit=179):
    """One word of a console line: a blank would split it into two, and the console takes 179 bytes of one."""
    if any(c.isspace() for c in s):
        sys.exit("%r has a blank, which the console would take as two words" % s)
    if len(s.encode()) > limit:
        sys.exit("%r is longer than the console takes (%d bytes)" % (s, limit))
    return s


def app_file(path):
    """A .wasm file, or an app's folder's app.wasm."""
    return os.path.join(path, "app.wasm") if os.path.isdir(path) else path


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port")
    ap.add_argument("--vm", metavar="PATH", help="an app's folder or a .wasm file on the VM, as app.run takes it")
    ap.add_argument("--put", metavar="PATH", help="write APP to PATH on the VM instead of running it")
    ap.add_argument("--install", metavar="PATH", help="install an app from the VM (its folder or a .wasm file)")
    ap.add_argument("--install-file", metavar="APP", help="install an app from this machine, over USB")
    ap.add_argument("--id", help="with --install or --install-file: its id (else its manifest's name)")
    ap.add_argument("--name", help="with --install or --install-file: its name in the launcher")
    ap.add_argument("--icon", help="with --install or --install-file: its glyph in the launcher (timer, game, ...)")
    ap.add_argument("--color", help="with --install or --install-file: its colour in the launcher (red, blue, ...)")
    ap.add_argument("--run", metavar="ID", help="run an installed app")
    ap.add_argument("--remove", metavar="ID", help="remove an installed app")
    ap.add_argument("--list", action="store_true", help="the installed apps")
    ap.add_argument("--launcher", action="store_true", help="open the launcher")
    ap.add_argument("--key", action="store_true", help="the power key, short")
    ap.add_argument("--files", action="store_true", help="the gadget's own files")
    ap.add_argument("--file-put", metavar="PATH", help="copy a file from the VM to the gadget")
    ap.add_argument("--file-upload", metavar="FILE", help="copy a file from this machine to the gadget")
    ap.add_argument("--file-remove", metavar="NAME", help="remove one of the gadget's files")
    ap.add_argument("--as", dest="as_name", metavar="NAME", help="with --file-put or --file-upload: its name there")
    ap.add_argument("--stop", action="store_true", help="close the running app")
    ap.add_argument("--status", action="store_true", help="app.status")
    ap.add_argument("--logs", action="store_true", help="app.logs")
    ap.add_argument("--api", action="store_true", help="fetch all app.api parts")
    ap.add_argument("--out", metavar="FILE", help="with --api: write the API as api.capabilities.json here")
    ap.add_argument("--event", metavar="TYPE,A,B", help="hand the app an input event")
    ap.add_argument("app", nargs="?", metavar="APP", help="a .wasm file or an app's folder")
    args = ap.parse_args()
    actions = [bool(args.app and not args.put), bool(args.vm), bool(args.put), args.stop, args.status, args.logs,
               args.api, bool(args.event), bool(args.install), bool(args.install_file), bool(args.run),
               bool(args.remove), args.list, args.launcher, args.key, args.files, bool(args.file_put),
               bool(args.file_upload), bool(args.file_remove)]
    if sum(actions) != 1:
        ap.error("give one of APP, --vm PATH, --put PATH APP, --install PATH, --install-file APP, --run ID, "
                 "--remove ID, --list, --launcher, --key, --files, --file-put PATH, --file-upload FILE, "
                 "--file-remove NAME, --stop, --status, --logs, --api or --event")
    if args.as_name and not (args.file_put or args.file_upload):
        ap.error("--as goes with --file-put or --file-upload")
    if (args.id or args.name or args.icon or args.color) and not (args.install or args.install_file):
        ap.error("--id, --name, --icon and --color go with --install or --install-file")
    if args.install and ((args.id and " " in args.id) or " " in args.install):
        ap.error("an id or path with a space can't go through the console")
    if args.put and not args.app:
        ap.error("--put PATH needs an APP to write")
    if args.out and not args.api:
        ap.error("--out goes with --api")
    try:
        with Board(args.port or pick_port()) as board:
            board.drain(0.2)
            if args.stop:
                f = command(board, "app.stop")
            elif args.status:
                f = command(board, "app.status")
            elif args.logs:
                f = command(board, "app.logs")
            elif args.api:
                f = fetch_api(board, args.out)
            elif args.event:
                f = command(board, "app.event=" + args.event)
            elif args.vm:
                # A path the console can tell from an installed app's id: a folder ends in "/".
                vm = args.vm if "/" in args.vm or args.vm.endswith(".wasm") else args.vm + "/"
                f = command(board, "app.run=" + word(vm), 60)
            elif args.install:
                # The console's "app.install=ID[:ICON:COLOR] PATH [NAME]", ID "-" for the manifest's:
                # always with an ID, since a bare path's word is limited to an ID's 63 bytes.
                look = ":%s:%s" % (args.icon or "", args.color or "") if args.icon or args.color else ""
                line = "app.install=" + " ".join([word((args.id or "-") + look), word(args.install)]
                                                 + ([args.name] if args.name else []))
                f = command(board, line, 60)
            elif args.install_file:
                with open(app_file(args.install_file), "rb") as fh:
                    f = send(board, fh.read(), install=install_line(args))
            elif args.run:
                f = command(board, "app.run=" + args.run, 30)
            elif args.remove:
                f = command(board, "app.remove=" + args.remove, 10)
            elif args.list:
                f = command(board, "app.list")
            elif args.launcher:
                f = command(board, "app.launcher")
            elif args.key:
                f = command(board, "app.key")
            elif args.files:
                f = command(board, "files.list", 10)
            elif args.file_put:
                f = command(board, "files.put=" + word(args.file_put) + (" " + word(args.as_name, 96) if args.as_name else ""), 240)
            elif args.file_upload:
                with open(args.file_upload, "rb") as fh:
                    f = send(board, fh.read(), file=args.as_name or os.path.basename(args.file_upload))
            elif args.file_remove:
                f = command(board, "files.remove=" + word(args.file_remove, 96), 10)
            else:
                put = args.put
                if put and not put.endswith(".wasm"):
                    put = put.rstrip("/") + "/app.wasm"
                with open(app_file(args.app), "rb") as fh:
                    f = send(board, fh.read(), put)
    except (BoardError, OSError) as e:
        print(e, file=sys.stderr)
        return 2
    print(json.dumps(f))
    return 0 if f.get("ok") else 1


if __name__ == "__main__":
    sys.exit(main())
