#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
# SPDX-License-Identifier: Apache-2.0
"""Compile and install a data-only pixel avatar over the existing USB console.

No token, public image upload, HTTP fetch, avatar generation, firmware rebuild,
or cache formatting. Uses the same avatar_pixel library as the self-hosted API.
"""
import argparse
import base64
import io
import json
import os
from pathlib import Path
import struct
import sys
import time
import zlib

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT))
from avatar_pixel import compile_image, validate_pack  # noqa: E402
from avatar_pixel.compiler import AvatarError  # noqa: E402
import chat  # noqa: E402


def receive(board, prefix, seconds=10):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        line = board.read_line(end)
        if not line:
            continue
        if '@pixel_export_error ' in line:
            raise RuntimeError(line[line.index('@pixel_export_error '):])
        at = line.find(prefix)
        if at >= 0:
            return line[at + len(prefix):]
    raise TimeoutError('No USB response for ' + prefix)


def generation(board):
    board.write_line('pixel.assets')
    data = json.loads(receive(board, '@pixel_assets '))
    key = data.get('source_key')
    if type(key) is not str or len(key) != 64:
        raise ValueError('Invalid USB generation digest')
    bytes.fromhex(key)
    return key


def read_image(board):
    from PIL import Image
    board.write_line('pixel.image')
    meta = json.loads(receive(board, '@pixel_image ', 15))
    width, height = meta.get('width'), meta.get('height')
    if type(width) is not int or type(height) is not int or not 1 <= width <= 466 or not 1 <= height <= 466:
        raise ValueError('Image dimensions outside the gadget bound')
    expected = width * height * 2
    if meta.get('bytes') != expected:
        raise ValueError('Invalid exported image size')
    body = bytearray()
    end = time.monotonic() + 90
    while time.monotonic() < end:
        line = board.read_line(end)
        if not line:
            continue
        if '@pixel_export_error ' in line:
            raise RuntimeError('Export invalidated')
        if line == '@pixel_end':
            break
        at = line.find('@pixel_bytes ')
        if at >= 0:
            body.extend(base64.b64decode(line[at + len('@pixel_bytes '):], validate=True))
            if len(body) > expected:
                raise ValueError('Export exceeded its advertised bound')
    else:
        raise TimeoutError('Image export incomplete')
    if len(body) != expected or zlib.crc32(body) != meta.get('crc'):
        raise ValueError('Image export checksum mismatch')
    source_key = generation(board)
    if source_key != meta.get('source_key'):
        raise ValueError('Avatar changed during export; retry with the current image')
    rgb = bytearray()
    for (pixel,) in struct.iter_unpack('<H', body):
        r, g, b = (pixel >> 11) & 31, (pixel >> 5) & 63, pixel & 31
        rgb.extend(((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)))
    image = Image.frombytes('RGB', (width, height), bytes(rgb))
    output = io.BytesIO()
    image.save(output, format='PNG')
    image.close()
    return output.getvalue(), source_key


def private_new(path, data):
    path = Path(path)
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    try:
        with os.fdopen(fd, 'wb') as output:
            output.write(data)
            output.flush()
            os.fsync(output.fileno())
    except BaseException:
        path.unlink(missing_ok=True)
        raise


def compile_current(port, output, preview, profile, colors, dither):
    with chat.Board(port) as board:
        status = board.status(timeout=5)
        if not status or not status.get('device', {}).get('link', {}).get('paired'):
            raise RuntimeError('The paired gadget must answer before exporting its image')
        encoded, key = read_image(board)
    result = compile_image(encoded, profile=profile, colors=colors, dither=dither, source_key=key)
    private_new(output, result.pack)
    if preview:
        private_new(preview, result.preview_png)
    print(json.dumps({key: result.manifest[key] for key in ('width', 'height', 'palette_count', 'total_size')}, sort_keys=True))


def install(port, path, enable=True):
    with open(path, 'rb') as source:
        pack = source.read(65537)
    meta = validate_pack(pack)
    with chat.Board(port) as board:
        status = board.status(timeout=5)
        if not status or not status.get('device', {}).get('link', {}).get('paired'):
            raise RuntimeError('The paired gadget must answer before installation')
        if generation(board) != meta['source_key']:
            raise ValueError('Avatar generation changed: compile the current avatar first')
        def command(line, prefix='@pixel_install ', timeout=10):
            board.write_line(line)
            if receive(board, prefix, timeout) != 'ok':
                raise RuntimeError('Device rejected the pixel pack or storage is unavailable')
        try:
            command('pixel.begin=%d,%s' % (len(pack), meta['source_key']))
            for offset in range(0, len(pack), 600):
                command('pixel.chunk=%d:%s' % (offset, base64.b64encode(pack[offset:offset + 600]).decode('ascii')))
            command('pixel.commit', timeout=30)
        except BaseException:
            try:
                command('pixel.abort')
            except Exception:
                pass
            raise
        if enable:
            command('pixel.on', prefix='@pixel_setting ')
        board.write_line('pixel')
        print(receive(board, '@pixel '))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='action', required=True)
    comp = commands.add_parser('compile-current')
    comp.add_argument('--port', required=True)
    comp.add_argument('--output', required=True)
    comp.add_argument('--preview')
    comp.add_argument('--profile', type=int, choices=(64, 96), default=64)
    comp.add_argument('--colors', type=int, default=24)
    comp.add_argument('--dither', action='store_true')
    inst = commands.add_parser('install')
    inst.add_argument('--port', required=True)
    inst.add_argument('pack')
    args = parser.parse_args()
    try:
        if args.action == 'compile-current':
            compile_current(args.port, args.output, args.preview, args.profile, args.colors, args.dither)
        else:
            install(args.port, args.pack)
    except (AvatarError, RuntimeError, ValueError, OSError, TimeoutError, chat.BoardError) as error:
        # No credential or private image content is part of these fixed failures.
        print(str(error), file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
