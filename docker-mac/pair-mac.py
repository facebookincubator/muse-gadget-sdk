#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
# Licensed under the Apache License, Version 2.0 (see LICENSE).
"""Pair with Mac BLE, then let the Linux container use the saved identity."""
from __future__ import annotations
import getpass
import os
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "linux/src"))
import mac_ble
from musegadget import config

save_token = config.save_sdk_token


def main(argv=None) -> int:
    argv = list(sys.argv[1:] if argv is None else argv)
    os.environ.setdefault(config.STATE_DIR_ENV, str(ROOT / ".muse-state"))
    if not mac_ble.HELPER.is_file():
        print("Build the Mac helper first: sh docker-mac/setup-mac.sh", file=sys.stderr)
        return 1
    if "--help" not in argv and "-h" not in argv:
        try:
            token = config.sdk_token()
            if token and os.environ.get(config.SDK_TOKEN_ENV):
                # The container reads the shared state, not the Mac environment.
                save_token(token, config.state_dir())
            elif not token:
                if not sys.stdin.isatty():
                    print("Run pairing in a terminal to enter your SDK token, or save it in the state directory.", file=sys.stderr)
                    return 1
                save_token(getpass.getpass("Muse SDK token (input hidden): "), config.state_dir())
        except (ValueError, OSError, EOFError, KeyboardInterrupt) as exc:
            print(f"Could not prepare Muse pairing: {exc}", file=sys.stderr)
            return 1
    # Replace only the platform-specific transport. The SDK owns cryptography,
    # confirmation, provisioning, timeout and credential logic.
    sys.modules["musegadget.ble_server"] = mac_ble
    from musegadget.cli import main as sdk_main
    return sdk_main(["pair", *argv])


if __name__ == "__main__":
    raise SystemExit(main())
