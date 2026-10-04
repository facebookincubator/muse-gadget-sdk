# Copyright (c) Meta Platforms, Inc. and affiliates.
# Licensed under the Apache License, Version 2.0 (see LICENSE).
"""Target enrollment endpoint, launched only inside an authenticated SSH session."""
from __future__ import annotations

import argparse
import contextlib
import os
import queue
import sys
import types

from musegadget import cli, config, stdio_ble


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--timeout", type=int, default=600)
    parser.add_argument("--force", action="store_true")
    args = parser.parse_args(argv)
    if not 1 <= args.timeout <= 3600:
        parser.error("timeout must be between 1 and 3600 seconds")
    if config.load_json(config.PAIRING_FILE) and not args.force:
        print("Board is already paired; pass --force to replace its enrollment.", file=sys.stderr)
        return 1

    channel = stdio_ble.JsonPipe(sys.stdin, sys.stdout)
    previous = sys.modules.get("musegadget.ble_server")
    previous_token = os.environ.get(config.SDK_TOKEN_ENV)
    try:
        request = channel.receive(timeout=30)
        if not request or request.get("op") != "start" or request.get("version") != stdio_ble.PROTOCOL_VERSION:
            raise ValueError("Invalid enrollment relay handshake")
        supplied = request.get("sdk_token")
        if supplied is not None:
            if not isinstance(supplied, str):
                raise ValueError("Invalid SDK token")
            config.save_sdk_token(supplied)
            # Explicit relay input takes priority over a board login's old env.
            os.environ[config.SDK_TOKEN_ENV] = config.validate_sdk_token(supplied)
        if not config.sdk_token():
            raise ValueError("Supply an SDK token through the Mac relay or the board's sdk_token file")
        config.state_dir().mkdir(mode=0o700, parents=True, exist_ok=True)
        config.state_dir().chmod(0o700)

        transport = types.ModuleType("musegadget.ble_server")
        transport.BleServer = lambda *a, **kw: stdio_ble.BleServer(*a, channel=channel, **kw)
        sys.modules["musegadget.ble_server"] = transport
        options = ["pair", "--timeout", str(args.timeout)]
        if args.force:
            options.append("--force")
        # SDK status and logging go to stderr. stdout carries only relay JSON.
        with contextlib.redirect_stdout(sys.stderr):
            return cli.main(options)
    except (OSError, ValueError, queue.Empty):
        print("Board enrollment relay failed; check the SDK token, pipe and board state permissions.",
              file=sys.stderr)
        return 1
    finally:
        channel.close()
        if previous_token is None:
            os.environ.pop(config.SDK_TOKEN_ENV, None)
        else:
            os.environ[config.SDK_TOKEN_ENV] = previous_token
        if previous is None:
            sys.modules.pop("musegadget.ble_server", None)
        else:
            sys.modules["musegadget.ble_server"] = previous


if __name__ == "__main__":
    raise SystemExit(main())
