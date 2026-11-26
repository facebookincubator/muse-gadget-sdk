# Copyright (c) Meta Platforms, Inc. and affiliates.
# SPDX-License-Identifier: Apache-2.0
"""python -m avatar_pixel: explicit private local conversion or self-hosting."""

import argparse
import os
from pathlib import Path
import stat
import sys

from .compiler import AvatarError, MAX_IMAGE_BYTES, compile_image
from .protocol import json_bytes


def _read_private(path):
    # No URL loader, FIFOs, devices, unbounded read, or implicit directory walk.
    fd = os.open(path, os.O_RDONLY | getattr(os, "O_NONBLOCK", 0))
    with os.fdopen(fd, "rb") as source:
        info = os.fstat(source.fileno())
        if not stat.S_ISREG(info.st_mode):
            raise AvatarError("input_not_regular_file")
        if info.st_size > MAX_IMAGE_BYTES:
            raise AvatarError("image_bytes_exceeded")
        return source.read(MAX_IMAGE_BYTES + 1)


def _write_private(outputs):
    # New files only, mode 0600 irrespective of umask. Never follow an output
    # symlink or truncate existing work. Roll back only files created here.
    names = [str(Path(path).absolute()) for path, _ in outputs]
    if len(set(names)) != len(names):
        raise AvatarError("duplicate_output_path")
    opened = []
    try:
        for path, data in outputs:
            fd = os.open(
                path,
                os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, "O_NOFOLLOW", 0),
                0o600,
            )
            opened.append((path, fd, os.fstat(fd)))
            with os.fdopen(os.dup(fd), "wb") as target:
                target.write(data)
                target.flush()
                os.fsync(target.fileno())
    except Exception:
        for path, _fd, info in opened:
            try:
                current = os.lstat(path)
                if current.st_dev == info.st_dev and current.st_ino == info.st_ino:
                    os.unlink(path)
            except OSError:
                pass
        raise
    finally:
        for _path, fd, _info in opened:
            os.close(fd)


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Image-only pixel avatar converter and self-hostable API"
    )
    commands = parser.add_subparsers(dest="command", required=True)
    compile_parser = commands.add_parser(
        "compile", help="write NEW private local pack/optional PNG and JSON"
    )
    compile_parser.add_argument("--input", required=True)
    compile_parser.add_argument("--output", required=True)
    compile_parser.add_argument("--profile", type=int, choices=(64, 96), default=64)
    compile_parser.add_argument("--colors", type=int, default=24)
    compile_parser.add_argument("--dither", action="store_true")
    compile_parser.add_argument(
        "--source-key", help="opaque 64-hex generation digest (not a credential)"
    )
    compile_parser.add_argument(
        "--preview", help="optional new private nearest-scaled PNG path"
    )
    compile_parser.add_argument(
        "--manifest", help="optional new private bounded JSON path"
    )
    serve_parser = commands.add_parser(
        "serve", help="development/self-hosting only; no public deployment"
    )
    serve_parser.add_argument(
        "--host", default="127.0.0.1", help="literal bind IP (default loopback)"
    )
    serve_parser.add_argument("--port", type=int, default=8080)
    serve_parser.add_argument("--max-connections", type=int, default=2)
    serve_parser.add_argument("--request-timeout", type=float, default=15.0)
    serve_parser.add_argument("--io-timeout", type=float, default=3.0)
    serve_parser.add_argument("--compile-timeout", type=float, default=5.0)
    serve_parser.add_argument(
        "--token-env",
        default="AVATAR_PIXEL_BEARER_TOKEN",
        help="bearer-token environment variable; never put tokens on command line",
    )
    args = parser.parse_args(argv)
    try:
        if args.command == "compile":
            result = compile_image(
                _read_private(args.input),
                profile=args.profile,
                colors=args.colors,
                dither=args.dither,
                source_key=args.source_key,
            )
            outputs = [(args.output, result.pack)]
            if args.preview:
                outputs.append((args.preview, result.preview_png))
            if args.manifest:
                outputs.append((args.manifest, json_bytes(result.manifest) + b"\n"))
            _write_private(outputs)
            print("compiled")
            return 0
        from .api import ServerConfig, make_server

        config = ServerConfig(
            host=args.host,
            port=args.port,
            bearer_token=os.environ.get(args.token_env),
            max_connections=args.max_connections,
            request_timeout=args.request_timeout,
            io_timeout=args.io_timeout,
            compile_timeout=args.compile_timeout,
        )
        with make_server(config) as server:
            print(
                f"avatar_pixel listening on {config.host}:{server.server_port}",
                flush=True,
            )
            try:
                server.serve_forever(poll_interval=0.1)
            except KeyboardInterrupt:
                pass
        return 0
    except AvatarError as error:
        print(error.code, file=sys.stderr)
        return 2
    except OSError:
        # Do not echo local paths, filenames, credentials or image errors.
        print("local_io_failed", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
