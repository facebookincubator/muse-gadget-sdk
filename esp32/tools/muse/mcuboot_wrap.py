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

"""Wrap a signed ESP-IDF app image in an MCUboot envelope for a BLE update.

The Muse app (and tools/mg_ble_client.py dfu) update a gadget over MCUmgr SMP
with an MCUboot image: a 32-byte header, the payload, a protected TLV with the
image's build id, then a TLV with the SHA-256 of all of that. On the ESP32
the payload is the app .bin as ESP-IDF builds and signs it; the gadget strips the envelope, writes the app to its
other OTA slot, and checks both the envelope's hash and the app's own
signature (components/muse_gadget_ble/mg_smp_esp.c).

    tools/muse/mcuboot_wrap.py BUILD/muse-gadget.bin [OUT] [--version X.Y.Z]

The packing is components/muse_gadget_ble/smp/pack.py's (byte-identical to
imgtool). The version defaults to the app's
own (esp_app_desc_t), its X.Y.Z part; a build suffix (+g<commit>) doesn't fit
MCUboot's version, so the build id (the first four bytes of the app's ELF
SHA-256, what the running firmware reports) tells builds of one version apart:
the gadget shows the envelope's hash as its running image only while both
match. tools/muse/board.sh writes muse-gadget.mcuboot.bin beside every BLE
build's image. The image holds the SDK token if the build did: keep it private.
"""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import os
import re
import struct
import sys
import tempfile
from pathlib import Path

ESP_IMAGE_MAGIC = 0xE9
ESP_APP_DESC_MAGIC = 0xABCD5432
HDR_SIZE = 32

_PACK = Path(__file__).resolve().parents[2] / "components" / "muse_gadget_ble" / "smp" / "pack.py"
_spec = importlib.util.spec_from_file_location("mg_smp_pack", _PACK)
smp_pack = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(smp_pack)


def app_version(app: bytes) -> str:
    """The version string in an ESP-IDF app image's esp_app_desc_t."""
    if len(app) < 32 + 48 or app[0] != ESP_IMAGE_MAGIC:
        raise ValueError("not an ESP-IDF app image")
    off = 24 + 8   # esp_image_header_t, then the first segment's header
    magic, = struct.unpack_from("<I", app, off)
    if magic != ESP_APP_DESC_MAGIC:
        raise ValueError("no esp_app_desc_t in the first segment")
    raw = app[off + 16:off + 48]
    return raw.split(b"\0", 1)[0].decode("ascii", "replace")


def app_build_id(app: bytes) -> int:
    """The build id the firmware reports (mg_smp_esp_build_id): the app's ELF
    SHA-256 (esp_app_desc_t.app_elf_sha256), its first four bytes as a
    little-endian number. pack.py --build-id-from-idf reads the same."""
    return smp_pack.idf_build_id(app)


def parse_version(v: str) -> tuple[int, int, int, int]:
    """X.Y.Z of a firmware version ("1.2.3", "1.2.3+g0123abcd", "v1.2.3.dirty"),
    with build 0: what mg_smp_esp_parse_version() makes of the same string."""
    m = re.match(r"^v?(\d+)\.(\d+)\.(\d+)", v)
    if not m:
        raise ValueError(f"version {v!r} isn't MAJOR.MINOR.PATCH")
    major, minor, rev = (int(x) for x in m.groups())
    if major > 255 or minor > 255 or rev > 65535:
        raise ValueError(f"version {v!r} out of MCUboot's range")
    return major, minor, rev, 0


def wrap(app: bytes, version: str, build_id: int | None = None) -> bytes:
    """The envelope, with the app's build id unless one is given (an app that
    isn't an ESP-IDF image, as in some tests, gets none)."""
    if build_id is None:
        try:
            build_id = app_build_id(app)
        except SystemExit:
            build_id = None
    return smp_pack.pack(app, parse_version(version), build_id)


def write_private(out: Path, data: bytes) -> None:
    """Writes data over out through a new 0600 file beside it: the image holds the
    SDK token when the build did, whatever the umask or out's old mode."""
    fd, tmp = tempfile.mkstemp(prefix=out.name + ".", dir=out.parent)
    try:
        with os.fdopen(fd, "wb") as f:
            f.write(data)
        os.replace(tmp, out)
    except BaseException:
        Path(tmp).unlink(missing_ok=True)
        raise


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("app", type=Path)
    ap.add_argument("out", type=Path, nargs="?")
    ap.add_argument("--version")
    a = ap.parse_args(argv)
    app = a.app.read_bytes()
    smp_pack.smp_check(app)   # an image without SMP could never take the next update over BLE
    version = a.version or app_version(app)
    out = a.out or a.app.with_name(a.app.stem + ".mcuboot.bin")
    data = wrap(app, version)
    write_private(out, data)
    d = smp_pack.info(data)
    print(f"{out}: {len(data)} bytes, version {'.'.join(map(str, parse_version(version)[:3]))}, "
          f"build id {d.get('build_id', 'none')}, sha256 {d['tlv_sha256'][:16]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
