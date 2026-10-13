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

"""``musegadget`` command line."""

from __future__ import annotations

import argparse
import logging
import os
import sys

from musegadget import __version__, config, identity
from musegadget.pair import SETUP_WINDOW_S, PairOutcome, SetupWindow, pair

log = logging.getLogger("musegadget")


def _setup_open(window: SetupWindow) -> None:
    if not window.has_sdk_token:
        print("No SDK token yet. Get one at gadgets.muse.ai and run "
              "`bash install.sh --sdk-token mgst_…`; gadgets without one will stop pairing.",
              file=sys.stderr)
    print(f"Setup open for {window.timeout_s // 60} minutes. In the Muse app, add a device")
    print(f"and choose {window.ble_name}.")


def cmd_pair(args: argparse.Namespace) -> int:
    result = pair(on_open=_setup_open, force=args.force, timeout_s=args.timeout)
    if result.outcome is PairOutcome.PAIRED:
        print("Paired.")
        return 0
    print({
        PairOutcome.ALREADY_PAIRED: "Already paired. Run `musegadget unpair` first, or pass --force.",
        PairOutcome.INVALID_SDK_TOKEN: f"Can't pair: {result.sdk_token_problem}.",
        PairOutcome.WINDOW_CLOSED: "Setup window closed without pairing.",
    }[result.outcome], file=sys.stderr)
    return 1


def cmd_run(args: argparse.Namespace) -> int:
    from musegadget.executor import Account, Executor
    from musegadget.service import run_service

    if os.geteuid() == 0:
        if not args.run_as:
            print("Pass --run-as ACCOUNT (the account whose permissions commands get).",
                  file=sys.stderr)
            return 1
        try:
            account = Account.lookup(args.run_as)
        except KeyError:
            print(f"Account {args.run_as!r} does not exist. Create it, or pass --run-as.",
                  file=sys.stderr)
            return 1
    else:
        account = Account.current()
    try:
        sdk_token = config.sdk_token()
    except ValueError as exc:
        log.warning("running without an SDK token: %s", exc)
        sdk_token = None
    log.info("musegadget %s: commands run as %s", __version__, account.name)
    run_service(identity.load_or_create(), Executor(account), sdk_token)
    return 0


def cmd_send_user_msg(args: argparse.Namespace) -> int:
    import json
    import socket

    message = sys.stdin.read() if args.message == ["-"] else " ".join(args.message)
    try:
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as sock:
            sock.settimeout(90)
            sock.connect(str(config.socket_path()))
            request = {"message": message}
            if args.session_id:
                request["session_id"] = args.session_id
            sock.sendall(json.dumps(request).encode() + b"\n")
            reply = json.loads(sock.makefile("rb").readline())
    except (OSError, ValueError) as exc:
        print(f"Could not reach the musegadget service: {exc}", file=sys.stderr)
        return 1
    if not reply.get("ok"):
        print(f"Not delivered: {reply.get('error') or reply}", file=sys.stderr)
        return 1
    print("Sent to your Muse.")
    return 0


def cmd_info(args: argparse.Namespace) -> int:
    ident = identity.load_or_create()
    paired = config.load_json(config.PAIRING_FILE) is not None
    print(f"version:   {__version__}")
    print(f"node id:   {ident.node_id}")
    print(f"BLE name:  {ident.ble_name}")
    print(f"paired:    {'yes' if paired else 'no'}")
    return 0


def cmd_unpair(args: argparse.Namespace) -> int:
    config.delete_json(config.PAIRING_FILE)
    print("Pairing removed. The device identity is kept.")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="musegadget")
    parser.add_argument("-v", "--verbose", action="store_true")
    sub = parser.add_subparsers(dest="command", required=True)

    pair_parser = sub.add_parser("pair", help="open Bluetooth setup so the Muse app can pair")
    pair_parser.add_argument("--timeout", type=int, default=SETUP_WINDOW_S,
                             help="seconds to keep setup open (default: %(default)s)")
    pair_parser.add_argument("--force", action="store_true", help="pair even if already paired")
    pair_parser.set_defaults(func=cmd_pair)

    run = sub.add_parser("run", help="stay connected to the Muse and serve its commands")
    run.add_argument(
        "--run-as",
        default=os.environ.get("MUSEGADGET_RUN_AS") or os.environ.get("SUDO_USER"),
        help="account whose permissions commands run with (default: the account that ran sudo)",
    )
    run.set_defaults(func=cmd_run)

    send = sub.add_parser("send-user-msg", help="send a message to your Muse from this device")
    send.add_argument("message", nargs="+", help="the message, or - to read it from stdin")
    send.add_argument("--session-id",
                     help="send to this side chat (a new id starts one) instead of the main chat")
    send.set_defaults(func=cmd_send_user_msg)

    sub.add_parser("info", help="show device identity").set_defaults(func=cmd_info)
    sub.add_parser("unpair", help="forget the saved pairing").set_defaults(func=cmd_unpair)

    args = parser.parse_args(argv)
    logging.basicConfig(
        level=logging.DEBUG if args.verbose else logging.INFO,
        format="%(asctime)s %(levelname)s %(name)s: %(message)s",
    )
    try:
        return args.func(args)
    except PermissionError as exc:
        # The state directory is root-only on an installed device.
        print(f"Can't access {exc.filename or config.state_dir()}; run this with sudo.",
              file=sys.stderr)
        return 1
