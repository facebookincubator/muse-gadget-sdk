# Copyright (c) Meta Platforms, Inc. and affiliates.
# SPDX-License-Identifier: Apache-2.0
"""One-shot in-memory HTTP compiler worker; never a service/device client."""

import sys

from .compiler import AvatarError
from .protocol import MAX_BODY_BYTES, compile_request, json_bytes

MAX_WORKER_ADDRESS_SPACE = 512 * 1024 * 1024


def _apply_resource_limits():
    """Fail closed on Unix limit failures; Darwin has no usable RLIMIT_AS cap.

    Linux/other supported Unix retain the hard address-space ceiling. Darwin
    deliberately does not claim a virtual-memory cap: production operators must
    supply an external memory/decoder isolation boundary. Input/output bounds
    and the parent's kill-and-reap wall timeout remain mandatory everywhere.
    """
    try:
        import resource
    except ImportError:
        if sys.platform != "win32":
            raise  # Never silently remove Linux/Unix resource protection.
        return
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    resource.setrlimit(resource.RLIMIT_FSIZE, (0, 0))
    resource.setrlimit(resource.RLIMIT_CPU, (30, 30))
    if sys.platform != "darwin":
        resource.setrlimit(resource.RLIMIT_AS, (MAX_WORKER_ADDRESS_SPACE,) * 2)


def main():
    try:
        # Limits are configured before even reading any private image body.
        _apply_resource_limits()
        body = sys.stdin.buffer.read(MAX_BODY_BYTES + 1)
        result = compile_request(body)
        sys.stdout.buffer.write(result)
        return 0
    except AvatarError as error:
        sys.stdout.buffer.write(json_bytes({"error": error.code}))
        return 2
    except Exception:
        # Decoder errors, allocation failures and tracebacks are not public data.
        sys.stdout.buffer.write(b'{"error":"compile_failed"}')
        return 3


if __name__ == "__main__":
    raise SystemExit(main())
