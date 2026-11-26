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

"""pack.py: wrap a vendor firmware payload in an MCUboot envelope for the
Muse apps' mcumgr update.

    [MCUboot header, 32 B (or --header-size)] [payload] [protected TLVs]
    [TLV info 0x6907] [SHA-256 TLV 0x10]

Hash-only: no signature, no key. The SHA-256 covers the
header, its padding, the payload and the protected TLV area, exactly as
`imgtool sign --header-size 32 --pad-header --version X.Y.Z+B` computes it;
without --build-id the output is byte-identical to imgtool's (the suite
checks that). --build-id adds protected TLV 0x00B1 (4 bytes, big-endian: what
`imgtool sign --custom-tlv 0xb1 0x<8 hex digits>` writes, so the two stay
byte-identical), which the gadget's core records at commit, so "same version,
different build" never reports the new hash.

    pack.py pack --version 1.3.7+0 [--build-id 0x1234abcd | --build-id-from-idf]
                 [--check-elf app.elf | --no-smp-check REASON] payload.bin out.bin
    pack.py info out.bin            # header, TLVs, hash check, app-side view

SMP check (an LTO build once silently dropped a whole SMP path and still
linked): an image that can't take the next update over SMP must not be
sent over SMP. pack refuses unless the payload holds the SMP service UUID
(either byte order), or --check-elf names the build's ELF and it holds both
the UUID bytes and the core (one of its public symbols, or its log strings,
which survive LTO inlining): use that for
compressed or encrypted payloads (a vendor's packed or encrypted update
formats). --no-smp-check REASON packs a vendor-only image on purpose.
"""
import argparse
import hashlib
import os
import re
import struct
import sys

MAGIC = 0x96F3B83D
SMP_UUID_BE = bytes.fromhex("8D53DC1D1DB74CD3868B8A527460AA84")
SMP_UUID_LE = SMP_UUID_BE[::-1]
# Any one of these proves the core is linked. LTO inlines single-call-site
# functions and drops their names (in one vendor build mg_smp_init and mg_smp_rx vanished
# from .symtab in a good image), so a log string of the core counts too: it
# is passed through a function pointer and survives as long as the core does.
SMP_ANCHOR_SYMBOLS = ("mg_smp_init", "mg_smp_rx", "mg_smp_poll", "mg_smp_backend_done", "mg_smp_upload_active",
                      "mg_smp_disconnect", "mg_smp_abort")
SMP_ANCHOR_STRING = b"smp: image committed for the next boot"
TLV_INFO = 0x6907
TLV_PROT_INFO = 0x6908
TLV_SHA256 = 0x10
TLV_BUILD_ID = 0x00B1


def parse_version(text):
    m = re.fullmatch(r"(\d+)\.(\d+)\.(\d+)(?:\+(\d+))?", text)
    if not m:
        raise SystemExit(f"bad --version {text!r} (want X.Y.Z or X.Y.Z+B)")
    major, minor, rev, build = (int(g) if g else 0 for g in m.groups())
    if major > 255 or minor > 255 or rev > 0xFFFF or build > 0xFFFFFFFF:
        raise SystemExit(f"--version {text!r} out of range (u8.u8.u16+u32)")
    return major, minor, rev, build


def idf_build_id(payload):
    """First 4 bytes (LE) of an ESP-IDF app's app_elf_sha256 (esp_app_desc_t)."""
    if len(payload) < 24 + 8 + 176 or payload[0] != 0xE9:
        raise SystemExit("--build-id-from-idf: not an ESP-IDF app image")
    desc = 24 + 8
    if struct.unpack_from("<I", payload, desc)[0] != 0xABCD5432:
        raise SystemExit("--build-id-from-idf: no esp_app_desc_t at the first segment")
    return struct.unpack_from("<I", payload, desc + 144)[0]


def has_uuid(blob):
    return SMP_UUID_LE in blob or SMP_UUID_BE in blob


def elf_symbols(data):
    """Names in an ELF's .symtab/.dynsym (32/64-bit, little-endian)."""
    if data[:4] != b"\x7fELF" or data[5] != 1:
        raise SystemExit("--check-elf: not a little-endian ELF")
    is64 = data[4] == 2
    if is64:
        shoff, = struct.unpack_from("<Q", data, 0x28)
        shentsize, shnum = struct.unpack_from("<HH", data, 0x3A)
    else:
        shoff, = struct.unpack_from("<I", data, 0x20)
        shentsize, shnum = struct.unpack_from("<HH", data, 0x2E)
    secs = []
    for i in range(shnum):
        o = shoff + i * shentsize
        if is64:
            _, typ, _, _, off, size, link, _, _, entsize = struct.unpack_from("<IIQQQQIIQQ", data, o)
        else:
            _, typ, _, _, off, size, link, _, _, entsize = struct.unpack_from("<IIIIIIIIII", data, o)
        secs.append((typ, off, size, link, entsize))
    names = set()
    for typ, off, size, link, entsize in secs:
        if typ not in (2, 11) or not entsize:   # SHT_SYMTAB, SHT_DYNSYM
            continue
        _, stroff, strsize, _, _ = secs[link]
        for j in range(size // entsize):
            nm, = struct.unpack_from("<I", data, off + j * entsize)
            end = data.index(b"\0", stroff + nm)
            names.add(data[stroff + nm:end].decode(errors="replace"))
    return names


def smp_check(payload, elf_path=None, override=None):
    """Raise SystemExit unless the image will be able to take the next update over SMP."""
    if override:
        print(f"SMP check skipped on purpose: {override}", file=sys.stderr)
        return
    if elf_path:
        data = open(elf_path, "rb").read()
        anchors = [n for n in SMP_ANCHOR_SYMBOLS if n in elf_symbols(data)]
        if SMP_ANCHOR_STRING in data:
            anchors.append("core log string")
        if not anchors or not has_uuid(data):
            raise SystemExit(f"refused: {elf_path} has no SMP path (core {'found: ' + ', '.join(anchors) if anchors else 'absent'}, "
                             f"service UUID bytes {'present' if has_uuid(data) else 'absent'}). "
                             "An image without SMP can't be updated over SMP again.")
        return
    if not has_uuid(payload):
        raise SystemExit("refused: the payload doesn't hold the SMP service UUID 8D53DC1D-... in either byte "
                         "order. A compressed or encrypted payload: pass --check-elf <the build's ELF>. A "
                         "vendor-only image on purpose: --no-smp-check REASON.")


def pack(payload, version, build_id=None, header_size=32):
    if header_size < 32:
        raise SystemExit("--header-size must be at least 32")
    prot = b""
    if build_id is not None:
        tlvs = struct.pack("<HH", TLV_BUILD_ID, 4) + struct.pack(">I", build_id & 0xFFFFFFFF)
        prot = struct.pack("<HH", TLV_PROT_INFO, 4 + len(tlvs)) + tlvs
    major, minor, rev, build = version
    hdr = struct.pack(
        "<IIHHIIBBHII",
        MAGIC,
        0,  # load address
        header_size,
        len(prot),  # protected TLV area size
        len(payload),
        0,  # flags
        major,
        minor,
        rev,
        build,
        0,  # pad
    )
    body = hdr + b"\0" * (header_size - 32) + payload + prot
    digest = hashlib.sha256(body).digest()
    tlv = struct.pack("<HH", TLV_SHA256, 32) + digest
    return body + struct.pack("<HH", TLV_INFO, 4 + len(tlv)) + tlv


def info(image):
    """What the apps and the gadget's core read from an envelope."""
    if len(image) < 32 or struct.unpack_from("<I", image)[0] != MAGIC:
        raise SystemExit("not an MCUboot image")
    _, _, hsize, psize, isize, flags, maj, mnr, rev, bld, _ = struct.unpack_from("<IIHHIIBBHII", image)
    out = {
        "header_size": hsize,
        "protected_tlv_size": psize,
        "image_size": isize,
        "flags": flags,
        "version": f"{maj}.{mnr}.{rev}+{bld}",
        "file_sha256": hashlib.sha256(image).hexdigest(),
    }
    end = hsize + isize
    computed = hashlib.sha256(image[: end + psize]).digest()
    off = end
    tlvs = []
    while off + 4 <= len(image):
        t, n = struct.unpack_from("<HH", image, off)
        if t in (TLV_INFO, TLV_PROT_INFO):
            off += 4
            continue
        tlvs.append((t, image[off + 4 : off + 4 + n]))
        off += 4 + n
    for t, v in tlvs:
        if t == TLV_SHA256:
            out["tlv_sha256"] = v.hex()
            out["hash_ok"] = v == computed
            break
    for t, v in tlvs:
        if t == TLV_BUILD_ID and len(v) == 4:
            out["build_id"] = f"0x{struct.unpack('>I', v)[0]:08x}"
    out["tlvs"] = [f"0x{t:04x}:{len(v)}" for t, v in tlvs]
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("pack", help="wrap a payload")
    p.add_argument("--version", required=True, help="X.Y.Z[+B], the firmware's own version")
    g = p.add_mutually_exclusive_group()
    g.add_argument("--build-id", type=lambda s: int(s, 0), help="32-bit build id the firmware reports")
    g.add_argument("--build-id-from-idf", action="store_true", help="ESP-IDF app: first 4 bytes of app_elf_sha256")
    p.add_argument("--header-size", type=lambda s: int(s, 0), default=32)
    c = p.add_mutually_exclusive_group()
    c.add_argument("--check-elf", help="the build's ELF: must hold the SMP UUID bytes and mg_smp symbols")
    c.add_argument("--no-smp-check", metavar="REASON", help="pack a vendor-only image on purpose")
    p.add_argument("payload")
    p.add_argument("out")
    i = sub.add_parser("info", help="describe an envelope")
    i.add_argument("image")
    a = ap.parse_args(argv)
    if a.cmd == "pack":
        payload = open(a.payload, "rb").read()
        smp_check(payload, a.check_elf, a.no_smp_check)
        build_id = idf_build_id(payload) if a.build_id_from_idf else a.build_id
        image = pack(payload, parse_version(a.version), build_id, a.header_size)
        # owner-only: the payload may be an app with a token compiled in
        fd = os.open(a.out, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
        with os.fdopen(fd, "wb") as f:
            os.fchmod(f.fileno(), 0o600)   # an existing file keeps its mode through O_CREAT
            f.write(image)
        d = info(image)
        print(f"{a.out}: {len(image)} bytes, version {d['version']}, "
              f"hash {d['tlv_sha256']}" + (f", build id {d['build_id']}" if "build_id" in d else ""))
    else:
        for k, v in info(open(a.image, "rb").read()).items():
            print(f"{k}: {v}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
