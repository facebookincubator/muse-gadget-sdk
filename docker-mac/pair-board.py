#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
# Licensed under the Apache License, Version 2.0 (see LICENSE).
"""Enroll a Linux board using Mac BLE and authenticated SSH over USB networking."""
from __future__ import annotations

import argparse
import getpass
import math
import queue
import re
import shlex
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "linux/src"))
import mac_ble
from musegadget import config
from musegadget.stdio_ble import JsonPipe, PROTOCOL_VERSION, packet_data

TARGET = re.compile(r"(?:[A-Za-z0-9_][A-Za-z0-9_.-]*@)?(?:[A-Za-z0-9][A-Za-z0-9_.-]*|\[[0-9A-Fa-f:]+\])")
NAME = re.compile(r"MuseGadget[0-9A-F]{6}")


def ssh_command(args):
    if not TARGET.fullmatch(args.target):
        raise ValueError("Use an SSH host alias or user@host address")
    remote = ["env", "MUSEGADGET_STATE_DIR=" + args.state_dir, args.remote_python,
              "-m", "musegadget.pair_stdio", "--timeout", str(args.timeout)]
    if args.force:
        remote.append("--force")
    if args.sudo:
        remote = ["sudo", "-n", *remote]
    command = ["ssh", "-T", "-o", "BatchMode=yes", "-o", "StrictHostKeyChecking=yes",
            "-o", "ConnectTimeout=10", "-o", "ServerAliveInterval=10", "-o",
            "ServerAliveCountMax=2", "-p", str(args.port)]
    if getattr(args, "ssh_key", None):
        command += ["-i", str(args.ssh_key), "-o", "IdentitiesOnly=yes"]
    if getattr(args, "known_hosts", None):
        command += ["-o", "UserKnownHostsFile=" + str(args.known_hosts)]
    return [*command, args.target, " ".join(shlex.quote(arg) for arg in remote)]


def close_process(process):
    if process is None:
        return
    if process.stdin and not process.stdin.closed:
        try:
            process.stdin.close()
        except OSError:
            pass
    try:
        process.wait(timeout=3)
    except subprocess.TimeoutExpired:
        process.terminate()
        try:
            process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=3)
    if process.stdout:
        process.stdout.close()


def helper_command(message):
    op = message.get("op")
    if op == "notify":
        packet_data(message.get("data"))
        return {"op": op, "data": message["data"]}
    if op == "disconnect":
        delay = message.get("delay")
        if type(delay) not in (int, float) or not math.isfinite(delay) or not 0 <= delay <= 5:
            raise ValueError("Invalid relay disconnect delay")
        return {"op": op, "delay": delay}
    if op == "stop":
        return {"op": op}
    raise ValueError("Unknown board relay operation")


def phone_event(message):
    kind = message.get("event")
    if kind == "write":
        packet_data(message.get("data"))
        return {"event": kind, "data": message["data"]}
    if kind == "mtu":
        value = message.get("value")
        if type(value) is not int or not 23 <= value <= 515:
            raise ValueError("Invalid relay MTU")
        return {"event": kind, "value": value}
    if kind == "disconnect":
        return {"event": kind}
    if kind == "ready":
        print("Mac BLE ready. Select the board's MuseGadget name in the phone app.", flush=True)
    elif kind == "state":
        print("Mac Bluetooth: " + str(message.get("state", "unknown")), flush=True)
    elif kind == "error":
        raise RuntimeError("Mac Bluetooth failed; check permission and radio state")
    else:
        raise ValueError("Unknown Mac helper event")
    return None


def relay(remote, *, timeout, sdk_token=None, launch_helper=None):
    """Forward encrypted BLE packets; the Mac never loads board credentials."""
    board = JsonPipe(remote.stdout, remote.stdin)
    helper = None
    phone = None
    helper_closed = False
    stopping_at = None
    deadline = time.monotonic() + timeout + 40
    try:
        start = {"op": "start", "version": PROTOCOL_VERSION}
        if sdk_token is not None:
            start["sdk_token"] = config.validate_sdk_token(sdk_token)
        board.send(start)
        try:
            ident = board.receive(timeout=30)
        except queue.Empty:
            raise RuntimeError("Board did not open enrollment within 30 seconds") from None
        if not ident or ident.get("event") != "identity" or ident.get("version") != PROTOCOL_VERSION:
            raise RuntimeError("Board enrollment did not start; check the SSH output above")
        name = ident.get("name")
        if not isinstance(name, str) or not NAME.fullmatch(name):
            raise ValueError("Invalid board identity")
        print("Enroll board: " + name, flush=True)
        launch_helper = launch_helper or (lambda device_name: subprocess.Popen(
            [str(mac_ble.HELPER), device_name], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            text=True, encoding="utf-8", bufsize=1))
        helper = launch_helper(name)
        phone = JsonPipe(helper.stdout, helper.stdin)
        while time.monotonic() < deadline:
            try:
                message = board.receive(timeout=0.05)
            except queue.Empty:
                message = False
            if message is None:
                status = remote.wait(timeout=3)
                if status:
                    raise RuntimeError("Board enrollment failed or was cancelled; check the SSH output above")
                print("Board enrolled. Its credentials remain on the board.", flush=True)
                return 0
            if message is not False:
                command = helper_command(message)
                if not helper_closed:
                    phone.send(command)
                if command["op"] == "stop":
                    stopping_at = time.monotonic()
            if not helper_closed:
                try:
                    event = phone.receive(timeout=0.05)
                except queue.Empty:
                    event = False
                if event is None:
                    helper_closed = True
                    if stopping_at is None:
                        stopping_at = time.monotonic()
                        board.send({"event": "stop"})
                elif event is not False:
                    forwarded = phone_event(event)
                    if forwarded:
                        board.send(forwarded)
            if stopping_at is not None and time.monotonic() - stopping_at > 5:
                raise RuntimeError("Board enrollment did not exit after setup stopped")
        raise RuntimeError("Board enrollment timed out")
    finally:
        if phone:
            if helper.poll() is None:
                try:
                    phone.send({"op": "stop"})
                except (OSError, ValueError):
                    pass
            phone.close()
        board.close()
        close_process(helper)
        close_process(remote)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("target", help="SSH user@USB-network-address or host alias")
    parser.add_argument("--port", type=int, default=22)
    parser.add_argument("--ssh-key", type=Path, help="SSH identity file")
    parser.add_argument("--known-hosts", type=Path, help="verified SSH host-key file")
    parser.add_argument("--remote-python", default="/opt/musegadget/venv/bin/python")
    parser.add_argument("--state-dir", default="/var/lib/musegadget")
    parser.add_argument("--sudo", action="store_true", help="use sudo -n on the board")
    parser.add_argument("--timeout", type=int, default=600)
    parser.add_argument("--force", action="store_true", help="replace the board's pairing")
    tokens = parser.add_mutually_exclusive_group()
    tokens.add_argument("--sdk-token-file", type=Path, help="send this SDK token through SSH")
    tokens.add_argument("--sdk-token-prompt", action="store_true", help="enter SDK token with hidden input")
    args = parser.parse_args(argv)
    if not 1 <= args.timeout <= 3600 or not 1 <= args.port <= 65535:
        parser.error("timeout must be 1–3600 seconds and SSH port 1–65535")
    try:
        command = ssh_command(args)
        if not mac_ble.HELPER.is_file():
            raise ValueError("Build the Mac helper first: sh docker-mac/setup-mac.sh")
        token = None
        if args.sdk_token_file:
            token = args.sdk_token_file.read_text(encoding="utf-8")
        elif args.sdk_token_prompt:
            token = getpass.getpass("Muse SDK token (input hidden): ")
        if token is not None:
            token = config.validate_sdk_token(token)
        remote = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  text=True, encoding="utf-8", bufsize=1)
        return relay(remote, timeout=args.timeout, sdk_token=token)
    except (OSError, ValueError, RuntimeError, EOFError, subprocess.TimeoutExpired, KeyboardInterrupt):
        print("Enrollment relay stopped. Check SSH access, the board SDK and Mac Bluetooth permission.",
              file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
