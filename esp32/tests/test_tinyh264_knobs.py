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

"""tinyh264's knobs for other platforms (xplat/tinyh264/README.md), each
built into the decoder and compared with the decoder as it ships, picture for
picture (tests/tinyh264_knobs_harness.c): with a knob built in but not asked
for, every call's result and every picture are the same; asked for, every
picture it outputs is the same, and what it refuses is what it says it
refuses.

The streams are tests/vectors/h264 (and its regression records); set
TINYH264_CORPUS to a directory of more .264 streams to run those too.
"""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import tempfile
import unittest
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TINY = next((p for p in (ROOT.parent / "xplat/tinyh264/src", ROOT / "components/tinyh264/src") if p.is_dir()),
            ROOT / "components/tinyh264/src")
HARNESS = ROOT / "tests/tinyh264_knobs_harness.c"
VUI_HARNESS = ROOT / "tests/tinyh264_vui_lean_harness.c"
VEC = ROOT / "tests/vectors/h264"
# TINYH264_DSP_MC's first-use check: a mismatch with the C aborts instead of falling back to it
DSP_CHECKED = ("-includestring.h", "-includestdlib.h",
               "-DTINYH264_LOG(f, ...)=(strstr(f, \"differs\") ? abort() : (void)0)")


def streams() -> list[Path]:
    out = sorted(VEC.glob("*.264")) + sorted((VEC / "regress").glob("*.au"))
    extra = os.environ.get("TINYH264_CORPUS")
    if extra:
        out += sorted(Path(extra).glob("*.264"))
    return out


class Bits:
    """An RBSP's bits, for rewriting one field of a sequence parameter set."""

    def __init__(self, data: bytes) -> None:
        self.bits = "".join(f"{b:08b}" for b in data)
        self.pos = 0

    def u(self, n: int) -> int:
        v = int(self.bits[self.pos:self.pos + n] or "0", 2)
        self.pos += n
        return v

    def ue(self) -> int:
        z = 0
        while self.bits[self.pos] == "0":
            z += 1
            self.pos += 1
        self.pos += 1
        v = int(self.bits[self.pos:self.pos + z], 2) if z else 0
        self.pos += z
        return (1 << z) - 1 + v

    def se(self) -> int:
        k = self.ue()
        return (k + 1) // 2 if k & 1 else -(k // 2)


def ue_bits(v: int) -> str:
    b = bin(v + 1)[2:]
    return "0" * (len(b) - 1) + b


def sps_with_max_refs(rbsp: bytes, refs: int) -> tuple[bytes, int]:
    """A baseline SPS RBSP with max_num_ref_frames (and the VUI's
    max_dec_frame_buffering, which may not be smaller) set to refs."""
    r = Bits(rbsp)
    edits = []   # (start, end, new bits)

    def field(value: int) -> None:
        at = r.pos
        r.ue()
        edits.append((at, r.pos, ue_bits(value)))

    profile = r.u(8)
    r.u(16)
    r.ue()
    assert profile in (66, 77, 88), profile
    r.ue()
    poc = r.ue()
    if poc == 0:
        r.ue()
    elif poc == 1:
        r.u(1), r.se(), r.se()
        for _ in range(r.ue()):
            r.se()
    at = r.pos
    most = r.ue()
    edits.append((at, r.pos, ue_bits(refs)))
    r.u(1), r.ue(), r.ue()
    if not r.u(1):
        r.u(1)
    r.u(1)
    if r.u(1):
        r.ue(), r.ue(), r.ue(), r.ue()
    if r.u(1):   # vui_parameters
        if r.u(1) and r.u(8) == 255:
            r.u(32)
        if r.u(1):
            r.u(1)
        if r.u(1):
            r.u(4)
            if r.u(1):
                r.u(24)
        if r.u(1):
            r.ue(), r.ue()
        if r.u(1):
            r.u(32), r.u(32), r.u(1)
        hrd = 0
        for _ in range(2):
            if r.u(1):
                hrd = 1
                cnt = r.ue() + 1
                r.u(8)
                for _ in range(cnt):
                    r.ue(), r.ue(), r.u(1)
                r.u(20)
        if hrd:
            r.u(1)
        r.u(1)
        if r.u(1):   # bitstream_restriction
            r.u(1)
            for _ in range(4):
                r.ue()
            r.ue()
            field(refs)   # max_dec_frame_buffering
    bits = r.bits
    for a, b, v in reversed(edits):
        bits = bits[:a] + v + bits[b:]
    bits = bits.rstrip("0")   # the stop bit ends it again
    bits += "0" * (-len(bits) % 8)
    return bytes(int(bits[k:k + 8], 2) for k in range(0, len(bits), 8)), most


def with_max_refs(stream: bytes, refs: int) -> tuple[bytes, int]:
    """The stream with every (baseline) SPS's max_num_ref_frames set to refs,
    and the most any of them had."""
    out, most = bytearray(), 0
    starts = [m for m in range(len(stream) - 3) if stream[m:m + 3] == b"\x00\x00\x01"] + [len(stream)]
    out += stream[:starts[0]]
    for a, b in zip(starts, starts[1:]):
        nal = stream[a + 3:b]
        while nal.endswith(b"\x00") and b < len(stream):
            nal = nal[:-1]
        tail = stream[a + 3 + len(nal):b]
        if nal and nal[0] & 0x1F == 7:
            rbsp, z = bytearray(), 0
            for x in nal[1:]:
                if z >= 2 and x == 3:
                    z = 0
                    continue
                rbsp.append(x)
                z = z + 1 if x == 0 else 0
            data, m = sps_with_max_refs(bytes(rbsp), refs)
            most = max(most, m)
            new, z = bytearray([nal[0]]), 0
            for x in data:
                if z >= 2 and x <= 3:
                    new.append(3)
                    z = 0
                new.append(x)
                z = z + 1 if x == 0 else 0
            nal = bytes(new)
        out += b"\x00\x00\x01" + nal + tail
    return bytes(out), most


class BitWriter:
    """An RBSP, bit by bit, for the crafted streams below."""

    def __init__(self) -> None:
        self.bits: list[int] = []

    def u(self, n: int, v: int) -> None:
        self.bits += [(v >> (n - 1 - i)) & 1 for i in range(n)]

    def ue(self, v: int) -> None:
        b = bin(v + 1)[2:]
        self.bits += [0] * (len(b) - 1) + [int(c) for c in b]

    def se(self, v: int) -> None:
        self.ue(2 * v - 1 if v > 0 else -2 * v)

    def align(self) -> None:
        self.bits += [0] * (-len(self.bits) % 8)

    def nal(self, ref_idc: int, kind: int) -> bytes:
        """The NAL unit with a start code: trailing bits, emulation prevention."""
        self.bits.append(1)
        self.align()
        rbsp = bytes(int("".join(map(str, self.bits[k:k + 8])), 2) for k in range(0, len(self.bits), 8))
        out, z = bytearray([ref_idc << 5 | kind]), 0
        for x in rbsp:
            if z >= 2 and x <= 3:
                out.append(3)
                z = 0
            out.append(x)
            z = z + 1 if x == 0 else 0
        return b"\x00\x00\x00\x01" + bytes(out)


def sps(w_mbs: int, h_mbs: int, refs: int = 1) -> bytes:
    """Baseline, level 3, frame_num 4 bits, POC type 2, one reference frame
    (or refs)."""
    b = BitWriter()
    b.u(8, 66), b.u(8, 0), b.u(8, 30), b.ue(0), b.ue(0), b.ue(2), b.ue(refs), b.u(1, 0)
    b.ue(w_mbs - 1), b.ue(h_mbs - 1), b.u(1, 1), b.u(1, 1), b.u(1, 0), b.u(1, 0)
    return b.nal(3, 7)


def sps_reorder(w_mbs: int, h_mbs: int) -> bytes:
    """As sps() but POC type 0 (5-bit pic_order_cnt_lsb) and a VUI that
    lets the DPB hold 2 frames for output reordering."""
    b = BitWriter()
    b.u(8, 66), b.u(8, 0), b.u(8, 30), b.ue(0), b.ue(0), b.ue(0), b.ue(1), b.ue(1), b.u(1, 0)
    b.ue(w_mbs - 1), b.ue(h_mbs - 1), b.u(1, 1), b.u(1, 1), b.u(1, 0), b.u(1, 1)
    b.u(1, 0), b.u(1, 0), b.u(1, 0), b.u(1, 0), b.u(1, 0), b.u(1, 0), b.u(1, 0), b.u(1, 0)
    b.u(1, 1), b.u(1, 1), b.ue(0), b.ue(0), b.ue(16), b.ue(16), b.ue(2), b.ue(2)
    return b.nal(3, 7)


def pps(redundant: bool, deblock_ctl: bool = False) -> bytes:
    b = BitWriter()
    b.ue(0), b.ue(0), b.u(1, 0), b.u(1, 0), b.ue(0), b.ue(0), b.ue(0), b.u(1, 0), b.u(2, 0)
    b.se(0), b.se(0), b.se(0), b.u(1, int(deblock_ctl)), b.u(1, 0), b.u(1, int(redundant))
    return b.nal(3, 8)


def slice_(first_mb: int, mbs: list, *, idr: bool = True, ref: bool = True, p: bool = False,
           frame_num: int = 0, idr_id: int = 0, redundant: int | None = None,
           adaptive: bool = False, poc: int | None = None, qp_delta: int = 0,
           deblock: tuple[int, int] | None = None) -> bytes:
    """One slice. mbs: "dc" (I_16x16, DC prediction, no residual), bytes (an
    I_PCM macroblock's 384 samples) or, in a P slice, an int (a skip run), a
    tuple (P_L0_16x16 with that motion vector difference) or "zero_dc"
    (P_L0_16x16, no motion, a single DC coefficient of 1 in luma block 0:
    at QP 9 it reconstructs to nothing; the macroblock must have no
    neighbours above or to its left). adaptive: a non-IDR reference slice's
    adaptive reference marking, with no operations (nothing is released for
    it). poc: pic_order_cnt_lsb, for sps_reorder(). deblock: the slice's
    filter offsets (div2), for pps(deblock_ctl=True)."""
    b = BitWriter()
    b.ue(first_mb), b.ue(5 if p else 7), b.ue(0), b.u(4, frame_num)
    if idr:
        b.ue(idr_id)
    if poc is not None:
        b.u(5, poc)
    if redundant is not None:
        b.ue(redundant)
    if p:
        b.u(1, 0), b.u(1, 0)
    if ref:
        if idr:
            b.u(1, 0), b.u(1, 0)
        elif adaptive:
            b.u(1, 1), b.ue(0)
        else:
            b.u(1, 0)
    b.se(qp_delta)
    if deblock is not None:
        b.ue(0), b.se(deblock[0]), b.se(deblock[1])
    for mb in mbs:
        if isinstance(mb, int):
            b.ue(mb)
        elif mb == "dc":
            b.ue(3 + (5 if p else 0)), b.ue(0), b.se(0), b.u(1, 1)
        elif isinstance(mb, tuple):   # P_L0_16x16, this motion vector difference, no residual
            b.ue(0), b.se(mb[0]), b.se(mb[1]), b.ue(0)
        elif mb == "zero_dc":
            # coded_block_pattern 1 (code 2), then block 0: one trailing +1
            # at DC (coeff_token 01, sign 0, total_zeros 1); blocks 1-3: none
            b.ue(0), b.se(0), b.se(0), b.ue(2), b.se(0)
            b.u(2, 1), b.u(1, 0), b.u(1, 1), b.u(1, 1), b.u(1, 1), b.u(1, 1)
        else:
            b.ue(25 + (5 if p else 0))
            b.align()
            for x in mb:
                b.u(8, x)
    return b.nal(3 if ref else 0, 5 if idr else 1)


def pcm(seed: int) -> bytes:
    """A macroblock's samples, every luma row its own"""
    return bytes(20 + (seed * 37 + i // 16 * 11 + i % 16) % 200 for i in range(256)) + \
        bytes(20 + (seed * 53 + i * 7) % 200 for i in range(128))


def crafted(tmp: str) -> list[Path]:
    """Small streams that each take a mode down a path the vectors don't:
    strip_jump: a picture's first slice starts on the row just past strip
    mode's ring (k = 1), nothing above it decoded; redundant_wrap: 65,536
    redundant slices of one macroblock between two primary ones, the other
    macroblock never coded; inplace_*: an in-place reference partly
    overwritten by a picture that never finished, then a redundant IDR slice,
    a non-reference intra picture or an intra reference picture reference
    marking can't store (the DPB full), then a P picture that would predict
    from the damaged reference (inplace_unstored: nothing lost before, the
    intra picture marking can't store alone overwrote it); inplace_corner: a
    32 x 32 P picture whose last macroblock interpolates from the reference's
    last row and column (in place, the end of each plane's buffer);
    reorder: one reference frame, a DPB of 2 for output reordering,
    pictures out of output order, then a non-reference picture all skipped
    (with output reordering on, the call that decodes it returns an
    earlier one), then an IDR picture that flushes the pictures still
    waiting (its call returns one of them); inplace_skipped: a damaged
    reference, two P pictures skipped while the decoder waits, then a
    non-IDR intra reference picture (its frame_num past the skipped ones)
    and a P picture from it; chm_coded_zero: a P picture, deblocked, whose
    first macroblock codes a coefficient that reconstructs to nothing (the
    loop filter still filters its inner edge, across a step in the
    reference); limits_same_id: a sequence parameter set past the limits
    with the active one's id, then an IDR picture and a P picture that the
    old one would take whole; limits_kept: one past the limits with the
    kept one's id outside a keyframe's access unit, then P pictures."""
    a = [pcm(k) for k in range(4)]
    step = bytes(100 if i % 16 < 4 else 104 for i in range(256)) + bytes([128] * 128)   # a step at column 4
    damaged = slice_(0, [3], idr=False, p=True, frame_num=1)   # 3 of 4 macroblocks
    reordered = [slice_(0, [0, pcm(k), 0, pcm(k + 1)], idr=False, p=True, frame_num=fn, poc=poc)
                 for fn, poc, k in ((1, 8, 10), (2, 4, 20), (3, 2, 30))]
    after = slice_(0, [4], idr=False, p=True, frame_num=1)
    unstored = slice_(0, [pcm(30 + k) for k in range(4)], idr=False, frame_num=1, adaptive=True) + \
        slice_(0, [4], idr=False, p=True, frame_num=2)
    out = {
        "strip_jump": sps(1, 4) + pps(False) + slice_(3, ["dc"]),
        "redundant_wrap": sps(1, 2) + pps(True) + slice_(1, ["dc"], redundant=0) +
        slice_(1, ["dc"], redundant=1) * 65535 + slice_(1, ["dc"], redundant=0),
        "inplace_redundant_idr": sps(1, 4) + pps(True) + slice_(0, a, redundant=0) +
        slice_(0, [3], idr=False, p=True, frame_num=1, redundant=0) +
        slice_(0, [pcm(10 + k) for k in range(4)], idr_id=1, redundant=1) +
        slice_(0, [4], idr=False, p=True, frame_num=1, redundant=0),
        "inplace_nonref_intra": sps(1, 4) + pps(False) + slice_(0, a) + damaged +
        slice_(0, [pcm(20 + k) for k in range(4)], idr=False, ref=False, frame_num=1) + after,
        "inplace_full_dpb": sps(1, 4) + pps(False) + slice_(0, a) + damaged + unstored,
        "inplace_unstored": sps(1, 4) + pps(False) + slice_(0, a) + unstored,
        "inplace_corner": sps(2, 2) + pps(False) + slice_(0, a) +
        slice_(0, [3, (-11, -11)], idr=False, p=True, frame_num=1),
        "inplace_skipped": sps(1, 4) + pps(False) + slice_(0, a) + damaged +
        slice_(0, [4], idr=False, p=True, frame_num=2) + slice_(0, [4], idr=False, p=True, frame_num=3) +
        slice_(0, [pcm(50 + k) for k in range(4)], idr=False, frame_num=4) +
        slice_(0, [4], idr=False, p=True, frame_num=5),
        "chm_coded_zero": sps(2, 2) + pps(False, deblock_ctl=True) +
        slice_(0, [step] + a[1:], deblock=(0, 0)) +
        slice_(0, [0, "zero_dc", 3], idr=False, p=True, frame_num=1, qp_delta=-17, deblock=(6, 6)),
        "limits_same_id": sps(1, 2) + pps(False) + slice_(0, a[:2]) + sps(2, 2) + pps(False) +
        slice_(0, a[:2], idr_id=1) + slice_(0, [2], idr=False, p=True, frame_num=1),
        "limits_kept": sps(1, 2) + pps(False) + slice_(0, a[:2]) + sps(1, 2, refs=2) +
        slice_(0, [2], idr=False, p=True, frame_num=1) + slice_(0, [2], idr=False, p=True, frame_num=2),
        "reorder": sps_reorder(1, 2) + pps(False) + slice_(0, a[:2], poc=0) + b"".join(reordered) +
        slice_(0, [2], idr=False, ref=False, p=True, frame_num=4, poc=10) +
        slice_(0, [pcm(40), pcm(41)], idr_id=1, poc=0),
    }
    d = Path(tmp, "crafted")
    d.mkdir(exist_ok=True)
    for name, data in out.items():
        (d / f"{name}.264").write_bytes(data)
    return sorted(d.glob("*.264"))

class Builds:
    """One harness build per set of defines, sanitized where the compiler can."""

    def __init__(self) -> None:
        self.tmp = tempfile.mkdtemp(prefix="tinyh264_knobs_")
        self.cc = os.environ.get("CC", "cc")
        self.cache: dict[tuple[str, ...], Path] = {}
        self.san = ["-fsanitize=address,undefined", "-fno-sanitize=shift-base", "-fno-sanitize-recover=undefined"]
        self.no_san = ""   # why the builds aren't sanitized ("": they are)
        exe = os.path.join(self.tmp, "probe")
        probe = subprocess.run([self.cc, *self.san, "-x", "c", "-", "-o", exe],
                               input="int main(void){return 0;}", capture_output=True, text=True, timeout=120)
        if probe.returncode:
            self.no_san = f"{self.cc} can't build with ASan/UBSan: {probe.stderr.strip()[-300:]}"
        else:
            try:
                ran = subprocess.run([exe], capture_output=True, text=True, timeout=60,
                                     env=dict(os.environ, ASAN_OPTIONS="detect_leaks=1"))   # as run() below
                if ran.returncode:
                    self.no_san = f"ASan/UBSan runtime fails here (exit {ran.returncode}): {ran.stderr.strip()[-300:]}"
            except (OSError, subprocess.TimeoutExpired) as e:
                self.no_san = f"ASan/UBSan runtime fails here: {e}"
        if self.no_san:
            self.san = []

    def get(self, *defs: str) -> Path:
        """defs: NAME=VALUE defines, or compiler options as they are (a leading -)"""
        key = tuple(sorted(defs))
        if key not in self.cache:
            exe = Path(self.tmp, f"h{len(self.cache)}")
            cmd = [self.cc, "-O1", "-g", "-w", "-std=gnu11", f"-I{TINY}", *self.san,
                   *(d if d.startswith("-") else f"-D{d}" for d in key),
                   str(HARNESS), *sorted(str(p) for p in TINY.glob("*.c")), "-o", str(exe)]
            r = subprocess.run(cmd, capture_output=True, text=True, timeout=600)
            if r.returncode:
                raise AssertionError(f"build {key} failed:\n{r.stdout}{r.stderr}")
            self.cache[key] = exe
        return self.cache[key]

    def run(self, exe: Path, *args: str, files: list[Path] | None = None) -> dict[str, dict]:
        def one(s: Path) -> dict:
            r = subprocess.run([str(exe), *args, str(s)], capture_output=True, text=True, timeout=600,
                               env=dict(os.environ, ASAN_OPTIONS="detect_leaks=1"))
            if r.returncode or not r.stdout.strip():
                raise AssertionError(f"{exe.name} {' '.join(args)} {s.name}: exit {r.returncode}\n{r.stderr[-3000:]}")
            return json.loads(r.stdout.strip().splitlines()[-1])
        with ThreadPoolExecutor(os.cpu_count() or 4) as ex:
            return {d["file"]: d for d in ex.map(one, files if files is not None else streams())}

    def vui_equivalence(self) -> Path:
        """tinyh264_vui_lean_harness: the SPS parser twice, with the full VUI
        parser and with the lean one, public names renamed apart."""
        exe = Path(self.tmp, "vui_lean")
        objs = []

        def cc(src: Path, out: str, *defs: str) -> None:
            o = Path(self.tmp, out)
            r = subprocess.run([self.cc, "-O1", "-g", "-w", "-std=gnu11", f"-I{TINY}", *self.san,
                                *(f"-D{d}" for d in defs), "-c", str(src), "-o", str(o)],
                               capture_output=True, text=True, timeout=600)
            if r.returncode:
                raise AssertionError(f"{src.name} {defs}: {r.stderr}")
            objs.append(str(o))

        cc(TINY / "h264bsd_vui.c", "vui_full.o", "h264bsdDecodeVuiParameters=vui_full")
        cc(TINY / "h264bsd_vui.c", "vui_lean.o", "H264BSD_LEAN_VUI=1", "h264bsdDecodeVuiParameters=vui_lean")
        for k in ("full", "lean"):
            cc(TINY / "h264bsd_seq_param_set.c", f"sps_{k}.o", f"h264bsdDecodeSeqParamSet=sps_{k}",
               f"h264bsdCompareSeqParamSets=cmp_{k}", f"h264bsdDecodeVuiParameters=vui_{k}_hook")
        for f in ("stream", "vlc", "byte_stream", "nal_unit", "util"):
            cc(TINY / f"h264bsd_{f}.c", f"{f}.o")
        r = subprocess.run([self.cc, "-O1", "-g", "-std=gnu11", f"-I{TINY}", *self.san, str(VUI_HARNESS), *objs,
                            "-o", str(exe)], capture_output=True, text=True, timeout=600)
        if r.returncode:
            raise AssertionError(r.stderr)
        return exe

    def close(self) -> None:
        shutil.rmtree(self.tmp, ignore_errors=True)


class TinyH264KnobsTest(unittest.TestCase):
    builds: Builds
    ref: dict[str, dict[str, dict]]
    crafted: list[Path]
    cref: dict[str, dict[str, dict]]

    @classmethod
    def setUpClass(cls) -> None:
        if not shutil.which(os.environ.get("CC", "cc")):
            raise unittest.SkipTest("no C compiler")
        cls.builds = Builds()
        plain = cls.builds.get()
        cls.ref = {m: cls.builds.run(plain, "-m", m) for m in ("vdec", "raw")}
        cls.crafted = crafted(cls.builds.tmp)
        cls.cref = {m: cls.builds.run(plain, "-m", m, files=cls.crafted) for m in ("vdec", "raw")}

    @classmethod
    def tearDownClass(cls) -> None:
        cls.builds.close()

    def assert_same(self, got: dict[str, dict], mode: str, what: str) -> None:
        for name, r in self.ref[mode].items():
            self.assertEqual(got[name]["aus"], r["aus"], f"{what} ({mode}): {name}")
            self.assertEqual(got[name].get("leak", 0), 0, f"{what} ({mode}): {name} leaks")

    def test_sanitized(self) -> None:
        """The builds run under ASan and UBSan, so an overread or overflow in
        any mode fails the comparisons below; where they can't run here, the
        comparisons still run, unsanitized, and this says why."""
        if self.builds.no_san:
            self.skipTest(self.builds.no_san)

    def test_off_unless_asked(self) -> None:
        """Each knob built in, not asked for: the decoder as it ships."""
        for defs in (["TINYH264_CUSTOM_ALLOC=1"], ["TINYH264_PIC_ALIGN=64"], ["H264BSD_LIMITS=1"],
                     ["H264BSD_BUFFER_GATE=1"], ["H264BSD_LEAN_VUI=1"], ["H264BSD_LOWMEM=1"],
                     ["H264BSD_STRIP=1"], ["H264BSD_LOWMEM=1", "H264BSD_ONEREF=1"],
                     ["H264BSD_CHANGED_MAP=1"]):
            exe = self.builds.get(*defs)
            for mode in ("vdec", "raw"):
                self.assert_same(self.builds.run(exe, "-m", mode), mode, " ".join(defs))

    def test_limits(self) -> None:
        """At most 300 macroblocks and one reference frame: the streams within
        those decode as before, the others are refused before any picture
        (with output reordering on, also one whose DPB holds more frames)."""
        exe = self.builds.get("H264BSD_LIMITS=1")
        got = self.builds.run(exe, "-m", "vdec", "-L", "300,1")
        refused = 0
        for name, r in self.ref["vdec"].items():
            if got[name]["aus"] != r["aus"]:
                refused += 1
                self.assertFalse(any(h for _, h in got[name]["aus"]), f"{name}: a picture past the limits")
        self.assertGreater(refused, 0)
        self.assertLess(refused, len(got))
        # with output reordering on, also a DPB of more frames than that: the
        # reorder stream's 2 frames are refused under 1, decode under 2
        plain = self.builds.get()
        files = [p for p in self.crafted if p.name == "reorder.264"]
        ref = self.builds.run(plain, "-m", "vdec", "-R", files=files)["reorder.264"]
        self.assertGreater(ref["pics"], 0)
        got = self.builds.run(exe, "-m", "vdec", "-L", "300,1", files=files)["reorder.264"]
        self.assertEqual(got["aus"], self.cref["vdec"]["reorder.264"]["aus"], "reorder, output in order")
        got = self.builds.run(exe, "-m", "vdec", "-R", "-L", "300,1", files=files)["reorder.264"]
        self.assertEqual(got["pics"], 0, "reorder: a DPB past the limit")
        got = self.builds.run(exe, "-m", "vdec", "-R", "-L", "300,2", files=files)["reorder.264"]
        self.assertEqual(got["aus"], ref["aus"], "reorder within the limit")
        # a sequence parameter set past the limits with the active one's id:
        # an error, and the old one is dropped with it, so the pictures after
        # it, which the old one would take, aren't decoded with it
        files = [p for p in self.crafted if p.name == "limits_same_id.264"]
        aus = self.builds.run(exe, "-m", "vdec", "-L", "3,1", files=files)["limits_same_id.264"]["aus"]
        self.assertEqual(aus[0], self.cref["vdec"]["limits_same_id.264"]["aus"][0])
        self.assertGreaterEqual(aus[1][0], 3, "limits_same_id: the refused SPS isn't an error")
        self.assertFalse(any(h for _, h in aus[1:]), "limits_same_id: decoded with the old SPS")
        # with the parameter sets kept (as muse_vdec.c), one past the limits
        # that the kept ones refuse is refused the same way: the kept ones
        # still decode the P pictures after it
        files = [p for p in self.crafted if p.name == "limits_kept.264"]
        ref = self.builds.run(exe, "-m", "vdec", "-k", files=files)["limits_kept.264"]["aus"]
        aus = self.builds.run(exe, "-m", "vdec", "-k", "-L", "3,1", files=files)["limits_kept.264"]["aus"]
        self.assertTrue(all(h for _, h in ref), "limits_kept: the kept sets didn't decode it")
        self.assertEqual(aus, ref, "limits_kept: the kept sets dropped")

    def test_buffer_gate(self) -> None:
        """Every picture is decoded into a buffer the gate let go, and the
        last one output is let go before its buffer is freed (a parameter set
        change, h264bsdShutdown)."""
        exe = self.builds.get("H264BSD_BUFFER_GATE=1", "TINYH264_CUSTOM_ALLOC=1")
        for mode in ("vdec", "raw"):
            got = self.builds.run(exe, "-m", mode, "-g")
            self.assert_same(got, mode, "gate")
            for name, r in got.items():
                self.assertEqual(r["gate_bad"], 0, f"{name}: a picture output from a buffer not let go")
                self.assertEqual(r["free_bad"], 0, f"{name}: a picture's buffer freed before it was let go")
                if r["pics"]:
                    self.assertGreater(r["gates"], 0, name)

    def first_error(self, r: dict) -> int:
        """The first access unit the shipped decoder failed or left
        unfinished (len(aus): none)."""
        bad = [k for k, (rc, _) in enumerate(r["aus"]) if rc not in (0, 1)] + r.get("dropped", [])
        return min(bad) if bad else len(r["aus"])

    def check_refusing_mode(self, got: dict[str, dict], mode: str, what: str, exact: bool,
                            extra: bool = False) -> list[str]:
        """A mode that refuses what it can't decode exactly: every picture it
        outputs in vdec mode is the shipped decoder's (it conceals nothing, as
        vdec mode doesn't), and up to the shipped decoder's first error every
        result is (exact: everywhere on streams it decodes without one).
        extra: it may also output a picture where the shipped decoder output
        none (strip mode decodes an IDR picture of a damaged access unit the
        shipped decoder fails on whole). Returns the streams it refused some
        of."""
        refused = []
        for name, r in self.ref[mode].items():
            g = got[name]
            self.assertEqual(g.get("leak", 0), 0, f"{what}: {name} leaks")
            if g["aus"] == r["aus"]:
                continue
            refused.append(name)
            e = self.first_error(r)
            if exact:
                self.assertEqual(g["aus"][:e], r["aus"][:e], f"{what} ({mode}): {name} before the first error at {e}")
            if mode == "vdec":
                for k, ((_, h0), (_, h1)) in enumerate(zip(r["aus"], g["aus"])):
                    if h1 and (h0 or not extra):
                        self.assertEqual(h1, h0, f"{what}: {name} picture {k} differs")
        return refused

    def test_lowmem(self) -> None:
        """The macroblock ring (1) and in-place pictures (2, D from the
        stream's motion vector bound): what the shipped decoder decodes
        without an error, they decode the same; damaged streams they drop
        instead of concealing. With a D smaller than the stream's motion
        (0x402: 32 rows) pictures are refused, never decoded wrong."""
        exe = self.builds.get("H264BSD_LOWMEM=1")
        for flags, exact in (("1", True), ("2", True), ("0x402", False)):
            for mode in ("vdec", "raw"):
                got = self.builds.run(exe, "-m", mode, "-l", flags)
                refused = self.check_refusing_mode(got, mode, f"lowmem {flags}", exact)
                if flags == "0x402" and mode == "vdec":
                    self.assertTrue(refused)   # the clips move more than 32 - 18 rows a picture
        # with the motion compensation backends, which follow the split
        # planes (the DSP one on its stand-ins, a first-use mismatch aborting)
        for backend in ([], ["TINYH264_PACKED4_MC=1"], ["TINYH264_DSP_MC=1", *DSP_CHECKED]):
            exe = self.builds.get("H264BSD_LOWMEM=1", *backend)
            if backend:
                got = self.builds.run(exe, "-m", "vdec", "-l", "2")
                self.check_refusing_mode(got, "vdec", f"lowmem 2, {backend[0]}", True)
            # reads up to the end of the in-place buffers (D = 32 rows)
            got = self.builds.run(exe, "-m", "vdec", "-l", "0x402", files=self.crafted)
            self.assertGreater(got["inplace_corner.264"]["inplace_rows"], 0)
            self.assertEqual(got["inplace_corner.264"]["aus"], self.cref["vdec"]["inplace_corner.264"]["aus"],
                             f"inplace_corner {backend[:1]}")
        # with the buffer gate: in place, the buffer let go is the one shared
        exe = self.builds.get("H264BSD_LOWMEM=1", "H264BSD_BUFFER_GATE=1", "TINYH264_CUSTOM_ALLOC=1")
        got = self.builds.run(exe, "-m", "vdec", "-l", "2", "-g")
        self.check_refusing_mode(got, "vdec", "lowmem 2, gate", True)
        for name, r in got.items():
            self.assertEqual(r["gate_bad"], 0, f"{name}: a picture output from a buffer not let go")
            self.assertEqual(r["free_bad"], 0, f"{name}: a picture's buffer freed before it was let go")
        # built in, not asked for: redundant slices count as they ship
        exe = self.builds.get("H264BSD_LOWMEM=1")
        got = self.builds.run(exe, "-m", "vdec", files=self.crafted)
        r = self.cref["vdec"]["redundant_wrap.264"]
        self.assertEqual(r["pics"], 0)
        self.assertEqual(got["redundant_wrap.264"]["aus"], r["aus"], "redundant_wrap")
        # in place (D = 16 rows): once the reference is damaged, nothing is
        # predicted from it; the intra picture between is the shipped one
        got = self.builds.run(exe, "-m", "vdec", "-l", "0x202", files=self.crafted)
        for name in ("inplace_redundant_idr.264", "inplace_nonref_intra.264", "inplace_full_dpb.264",
                     "inplace_unstored.264"):
            g, r = got[name]["aus"], self.cref["vdec"][name]["aus"]
            self.assertGreater(got[name]["inplace_rows"], 0, name)
            for k, ((_, h0), (_, h1)) in enumerate(zip(r, g)):
                if h1:
                    self.assertEqual(h1, h0, f"{name} picture {k}")
            self.assertEqual(g[-1][1], 0, f"{name}: predicted from the damaged reference")
        # the wait ends at a non-IDR intra reference picture even when the
        # pictures skipped before it leave a frame_num gap
        g, r = got["inplace_skipped.264"]["aus"], self.cref["vdec"]["inplace_skipped.264"]["aus"]
        self.assertEqual([h for _, h in g[2:4]], [0, 0], "inplace_skipped: predicted from the damaged reference")
        self.assertEqual(g[4:], r[4:], "inplace_skipped: the intra picture and the P picture after it")
        self.assertTrue(g[-1][1])

    def test_strip(self) -> None:
        """Strip mode, rows handed out from a ring of K + 2 (K = 1, 4): every
        picture it outputs is the shipped decoder's, every row once and in
        order (the harness assembles them); intra pictures decode, the rest
        are refused, and intra-only streams decode as before."""
        for defs in (["H264BSD_STRIP=1"], ["H264BSD_STRIP=1", "H264BSD_LOWMEM=1"]):
            exe = self.builds.get(*defs)
            for k in ("1", "4"):
                for mode in ("vdec", "raw"):
                    got = self.builds.run(exe, "-m", mode, "-s", k)
                    what = f"strip {k} {' '.join(defs)}"
                    self.check_refusing_mode(got, mode, what, False, extra=True)
                    pics = 0
                    for name, r in self.ref[mode].items():
                        g = got[name]
                        self.assertNotIn(98, [rc for rc, _ in g["aus"]], f"{what}: {name} rows missing")
                        pics += g["pics"]
                        if r["pics"] and all(rc in (0, 1) for rc, _ in r["aus"]) and ("_intra_" in name or "_i1" in name):
                            self.assertEqual(g["aus"], r["aus"], f"{what} ({mode}): {name}")
                    self.assertGreater(pics, 0, what)   # the IDR pictures at least
            # a slice that jumps past the ring: refused before any row is handed out
            got = self.builds.run(exe, "-m", "vdec", "-s", "1", files=self.crafted)
            self.assertEqual(got["strip_jump.264"]["pics"], 0)
            self.assertEqual(got["strip_jump.264"]["rows_lost"], 0, "rows handed out that were never decoded")

    def test_oneref(self) -> None:
        """One reference kept in place for an SPS allowing several: the
        streams, their SPSs rewritten to allow 4 (max_num_ref_frames), decode
        as the shipped decoder decodes the originals wherever it does
        without an error; streams that really use several references are
        refused, never decoded wrong."""
        exe = self.builds.get("H264BSD_LOWMEM=1", "H264BSD_ONEREF=1")
        tmp = Path(self.builds.tmp, "refs4")
        tmp.mkdir(exist_ok=True)
        files = []
        for p in streams():
            if p.suffix == ".264":
                data, most = with_max_refs(p.read_bytes(), 4)
                if most == 1:
                    q = tmp / p.name
                    q.write_bytes(data)
                    files.append(q)
        plain = self.builds.get()
        for mode in ("vdec", "raw"):
            ref4 = self.builds.run(plain, "-m", mode, files=files)
            got = self.builds.run(exe, "-m", mode, "-l", "2", files=files)
            same = 0
            for name, r in ref4.items():
                r1 = self.ref[mode][name]
                if r1["aus"] != r["aus"]:
                    continue   # a stream whose SPS the rewrite changed how it decodes (none so far)
                e = self.first_error(r1)
                self.assertEqual(got[name]["aus"][:e], r1["aus"][:e], f"oneref ({mode}): {name}")
                same += got[name]["aus"] == r1["aus"]
            self.assertGreater(same, 0)
            # the originals, a few of them with several references in use
            got = self.builds.run(exe, "-m", mode, "-l", "2")
            self.check_refusing_mode(got, mode, "oneref", False)

    def test_changed_map(self) -> None:
        """Every macroblock the map calls clean is the previous output
        picture's, byte for byte (but along an edge with a changed one when
        the picture deblocked); the pictures are the shipped decoder's. Also
        with the low-memory modes, and with output reordering on."""
        cases = ((["H264BSD_CHANGED_MAP=1"], []),
                 (["H264BSD_CHANGED_MAP=1", "H264BSD_LOWMEM=1"], ["-l", "2"]))
        for defs, extra in cases:
            exe = self.builds.get(*defs)
            for mode in ("vdec", "raw"):
                got = self.builds.run(exe, "-m", mode, "-c", *extra)
                if not extra:
                    self.assert_same(got, mode, "changed map")
                else:
                    self.check_refusing_mode(got, mode, "changed map, in place", True)
                self.assertEqual(sum(r["map_bad"] for r in got.values()), 0,
                                 [n for n, r in got.items() if r["map_bad"]])
                self.assertGreater(sum(r["map_pics"] for r in got.values()), 0)
                self.assertGreater(sum(r["map_clean"] for r in got.values()), 0)
        # a coded coefficient that reconstructs to nothing still makes the
        # loop filter filter the macroblock's inner edges where it deblocks
        files = [p for p in self.crafted if p.name == "chm_coded_zero.264"]
        g = self.builds.run(self.builds.get("H264BSD_CHANGED_MAP=1"), "-m", "vdec", "-c",
                            files=files)["chm_coded_zero.264"]
        self.assertEqual(g["aus"], self.cref["vdec"]["chm_coded_zero.264"]["aus"])
        self.assertEqual((g["map_pics"], g["map_bad"]), (1, 0), "chm_coded_zero")
        # output reordering on: a map only with the picture it describes (the
        # reorder stream's last call returns another picture)
        exe, plain = self.builds.get("H264BSD_CHANGED_MAP=1"), self.builds.get()
        files = streams() + [p for p in self.crafted if p.name == "reorder.264"]
        ref = self.builds.run(plain, "-m", "vdec", "-R", files=files)
        got = self.builds.run(exe, "-m", "vdec", "-R", "-c", files=files)
        for name, r in ref.items():
            self.assertEqual(got[name]["aus"], r["aus"], f"changed map, reordering: {name}")
        self.assertEqual(sum(r["map_bad"] for r in got.values()), 0,
                         [n for n, r in got.items() if r["map_bad"]])
        # bit 2 marks the IDR pictures among those returned, not the IDR
        # pictures decoded (output in decode order: the first and last)
        aus = self.cref["vdec"]["reorder.264"]["aus"]
        idr = {aus[0][1], aus[-1][1]}
        g = got["reorder.264"]
        self.assertEqual(g["map_idr"], [h for _, h in g["aus"] if h in idr], "reorder: IDR bit")
        self.assertTrue(g["map_idr"])

    def test_lean_vui(self) -> None:
        """The lean VUI parser accepts and refuses what the full one does,
        ends on the same bit and keeps the same fields the decoder reads: on
        every SPS of the streams, 200 mutations of each and 20,000 crafted
        ones cut at every byte (tests/tinyh264_vui_lean_harness.c). And the
        decoder built with it decodes every stream as before (in
        test_off_unless_asked: it has nothing to ask for)."""
        exe = self.builds.vui_equivalence()
        files = [str(p) for p in streams() if p.suffix == ".264"]
        r = subprocess.run([str(exe), os.environ.get("TINYH264_VUI_MUTATIONS", "200"), "1", *files],
                           capture_output=True, text=True, timeout=1800)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr[-3000:])
        self.assertIn(" 0 mismatches", r.stdout)
        print(r.stdout.strip())


if __name__ == "__main__":
    unittest.main()
