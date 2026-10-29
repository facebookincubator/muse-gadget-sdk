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

from musegadget import __version__, config, doctor, identity
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
    from musegadget import commands
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
    drop_ins = commands.load(config.commands_dir())
    run_service(identity.load_or_create(), Executor(account, drop_ins), sdk_token)
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


def cmd_voice_call(args: argparse.Namespace) -> int:
    import asyncio
    import signal

    from musegadget import muse_api
    from musegadget.link_client import DeviceDescription, LinkSession
    from musegadget.service import DEFAULT_NOISE_HOST
    from musegadget.voice_audio import LiveAudio, WavAudio
    from musegadget.voice_call import VoiceCall

    if args.live and (args.in_wav or args.out_wav):
        print("Choose --live or both --in-wav and --out-wav.", file=sys.stderr)
        return 2
    if not args.live and not (args.in_wav and args.out_wav):
        print("WAV mode requires --in-wav and --out-wav.", file=sys.stderr)
        return 2
    pairing = config.load_json(config.PAIRING_FILE)
    if not pairing or not pairing.get("access_token"):
        print("Not paired; run `musegadget pair` first.", file=sys.stderr)
        return 1
    ident = identity.load_or_create()
    # WebSocket debug logs include the Authorization header. Never enable them
    # for a credentialed call, even when the CLI was invoked with -v.
    logging.getLogger("websockets").setLevel(logging.WARNING)

    async def run(audio):
        vms, status = await asyncio.to_thread(
            muse_api.fetch_vms_with_status, pairing["access_token"],
            muse_api.api_root(pairing.get("api_url_v2", "")),
        )
        if status != 200:
            raise ConnectionError("VM lookup failed; refresh the paired device credentials")
        matches = [vm for vm in vms if vm["vm_id"] == args.vm_id]
        if len(matches) != 1:
            raise ValueError("--vm-id must exactly match one leased VM")
        vm = matches[0]
        session = LinkSession(
            noise_host=pairing.get("noise_host") or DEFAULT_NOISE_HOST,
            vm_id=vm["vm_id"], vm_auth_token=vm["vm_auth_token"], app_id=args.app_id,
            register_device=False,
            device=DeviceDescription(ident.node_id, "Muse voice", __version__, {}),
            run_command=lambda *unused: {"ok": False, "error": "unsupported command"},
        )
        stop = asyncio.Event()
        connection = asyncio.create_task(session.run(stop))
        current = asyncio.current_task()
        loop = asyncio.get_running_loop()
        for signum in (signal.SIGINT, signal.SIGTERM):
            loop.add_signal_handler(signum, current.cancel)
        try:
            await session.wait_transport_ready(35)
            call = VoiceCall(
                session, device_id=ident.node_id, thread_id=args.thread_id,
                half_duplex=not args.full_duplex,
                mic_threshold=args.mic_threshold, idle_timeout=args.idle_timeout,
                overall_timeout=args.overall_timeout,
            )
            return await call.run(audio.frames(), audio, realtime_input=args.live)
        finally:
            stop.set()
            try:
                await asyncio.wait_for(connection, 3)
            except (Exception, asyncio.CancelledError):
                connection.cancel()
                await asyncio.gather(connection, return_exceptions=True)
            for signum in (signal.SIGINT, signal.SIGTERM):
                loop.remove_signal_handler(signum)

    audio = None
    try:
        audio = LiveAudio() if args.live else WavAudio(args.in_wav, args.out_wav)
        reason = asyncio.run(run(audio))
        print(f"Voice call ended: {reason}.")
        return 0
    except (KeyboardInterrupt, asyncio.CancelledError):
        print("Voice call stopped.")
        return 130
    except Exception as exc:
        # Exception bodies from network/device libraries may echo credentials
        # or user data. Keep CLI diagnostics limited to the failure class.
        print(f"Voice call failed ({type(exc).__name__}).", file=sys.stderr)
        return 1
    finally:
        if audio is not None:
            try:
                audio.close()
            except Exception:
                log.warning("could not close audio device or WAV file")


def cmd_info(args: argparse.Namespace) -> int:
    ident = identity.load_or_create()
    pairing = config.load_json(config.PAIRING_FILE)
    try:
        sdk_token = config.sdk_token_hint(config.sdk_token())
    except ValueError:
        sdk_token = "not valid"
    paired_with = (pairing or {}).get("sdk_token_hint")
    print(f"version:   {__version__}")
    print(f"node id:   {ident.node_id}")
    print(f"BLE name:  {ident.ble_name}")
    print(f"SDK token: {sdk_token}")
    if pairing is None:
        print("paired:    no")
    elif paired_with is not None and paired_with != sdk_token:
        print(f"paired:    yes, with SDK token {paired_with}; run `musegadget pair --force` to use this one")
    else:
        print("paired:    yes")
    return 0


def cmd_unpair(args: argparse.Namespace) -> int:
    config.delete_json(config.PAIRING_FILE)
    print("Pairing removed. The device identity is kept.")
    return 0


def cmd_doctor(args: argparse.Namespace) -> int:
    print(f"Muse Gadget doctor (version {__version__})")
    results: tuple[doctor.CheckResult, ...] = doctor.run_checks()
    for result in results:
        print(doctor.format_check(result))
    return 1 if any(result["status"] == "FAIL" for result in results) else 0


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

    from musegadget.link_client import validate_app_id
    from musegadget.voice_call import positive_seconds

    voice = sub.add_parser("voice-call", help="start a realtime Muse voice call (may replace another call)")
    voice.add_argument("--vm-id", required=True, help="exact leased VM ID; does not change defaults")
    voice.add_argument("--app-id", type=validate_app_id, default="hatch-web",
                       help="connection app identity: positive int64 or %(default)s")
    voice.add_argument("--in-wav", help="PCM16LE mono 24 kHz input WAV")
    voice.add_argument("--out-wav", help="write played reply audio to this WAV")
    voice.add_argument("--live", action="store_true", help="use microphone and speaker (requires [voice])")
    voice.add_argument("--full-duplex", action="store_true", help="disable speaker mic gating; use headphones")
    voice.add_argument("--mic-threshold", type=int, default=0,
                       help="optional RMS mic gate, 0 disables it (range 0–32767)")
    voice.add_argument("--idle-timeout", type=positive_seconds, default=45,
                       help="hang up after this many inactive seconds (default: %(default)s)")
    voice.add_argument("--overall-timeout", type=positive_seconds, default=300,
                       help="maximum call seconds (default: %(default)s)")
    voice.add_argument("--thread-id", help="target thread, otherwise the primary thread")
    voice.set_defaults(func=cmd_voice_call)

    sub.add_parser("info", help="show device identity").set_defaults(func=cmd_info)
    sub.add_parser("unpair", help="forget the saved pairing").set_defaults(func=cmd_unpair)
    sub.add_parser("doctor", help="check setup and suggest troubleshooting steps").set_defaults(func=cmd_doctor)

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
