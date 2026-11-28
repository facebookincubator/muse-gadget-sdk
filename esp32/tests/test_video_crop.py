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

"""H.264 frame cropping (every decoder honors SPS cropping) through components/muse/muse_vdec.c on the real
tinyh264: the picture muse_vdec hands the show task is the sequence parameter
set's frame cropping window, all four offsets, and a cropped stream is never
refused for its padding.

Vectors (tests/vectors/h264):
- vt_112_crop.264: Apple VideoToolbox's hardware H.264 encoder (baseline,
  real time), asked for 112x112: it codes 192x112 (its 192 px floor, as the
  iPhone 15 Pro Max's A17 does) and crops 80 px on the right. 60 frames, IDR at 0 and 30.
- vt_80_crop.264: the same encoder asked for 80x80: coded 192x96, cropped
  112 px right and 16 px bottom.
The left and top offsets come from the real 112 capture with only its SPS
rewritten (crop left 48 px, right 32, top 8, bottom 8: a 112x96 window at
(48, 8)), and with the cropping removed (the whole coded picture), so every
window is checked byte for byte against the coded pictures. The expected MD5s
of each clip's pictures are ffmpeg 7's (`ffmpeg -i CLIP -f rawvideo -pix_fmt
yuv420p -`, which honors cropping), measured once.
"""

import hashlib
import json
import os
import shlex
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TINY = ROOT.parent / "xplat/tinyh264/src"
MUSE = ROOT / "components/muse"
VSTREAM = ROOT / "components/muse_vstream"
VEC = ROOT / "tests/vectors/h264"

# ffmpeg's decode (cropping honored) of each clip, all 60 pictures, I420.
FFMPEG_MD5 = {
    "vt_112_crop.264": "827198f29bf76cfbb0eb300f77e75251",
    "vt_80_crop.264": "a8d292a1072d38564d23efa69af27810",
    "lt": "daf50ba3af626d9cd0c687eb81069fef",
}


# ---- a sequence parameter set rewriter (baseline, as the iPhone sends) ----

def nal_units(b: bytes) -> list[tuple[int, int]]:
    """(start, end) of each NAL unit's payload (after its start code)."""
    starts = []
    i = 0
    while True:
        j = b.find(b"\x00\x00\x01", i)
        if j < 0:
            break
        starts.append(j + 3)
        i = j + 3
    out = []
    for k, s in enumerate(starts):
        e = starts[k + 1] - 3 if k + 1 < len(starts) else len(b)
        while e > s and b[e - 1] == 0 and k + 1 < len(starts):
            e -= 1
        out.append((s, e))
    return out


def unescape(d: bytes) -> list[int]:
    bits, zeros = [], 0
    for x in d:
        if zeros >= 2 and x == 3:
            zeros = 0
            continue
        zeros = zeros + 1 if x == 0 else 0
        bits.extend((x >> (7 - k)) & 1 for k in range(8))
    return bits


def escape(rbsp: bytes) -> bytes:
    out, zeros = bytearray(), 0
    for x in rbsp:
        if zeros >= 2 and x <= 3:
            out.append(3)
            zeros = 0
        out.append(x)
        zeros = zeros + 1 if x == 0 else 0
    return bytes(out)


class Bits:
    def __init__(self, bits: list[int]) -> None:
        self.b, self.p = bits, 0

    def u(self, n: int) -> int:
        v = 0
        for _ in range(n):
            v = v << 1 | self.b[self.p]
            self.p += 1
        return v

    def ue(self) -> int:
        z = 0
        while self.u(1) == 0:
            z += 1
        return (1 << z) - 1 + self.u(z)

    def se(self) -> int:
        k = self.ue()
        return (k + 1) // 2 if k & 1 else -(k // 2)


def ue_bits(v: int) -> list[int]:
    v += 1
    n = v.bit_length()
    return [0] * (n - 1) + [(v >> (n - 1 - k)) & 1 for k in range(n)]


def rewrite_sps(sps: bytes, crop: tuple[int, int, int, int] | None, width_mbs: int | None = None) -> bytes:
    """The SPS NAL unit (header byte first) with its frame cropping replaced
    (None: none) and optionally its pic_width_in_mbs; everything after the
    cropping (VUI, trailing bits) kept bit for bit."""
    bits = unescape(sps[1:])
    r = Bits(bits)
    profile = r.u(8)
    assert profile == 66, profile   # baseline: no chroma_format_idc
    r.u(16)
    r.ue()                          # seq_parameter_set_id
    r.ue()                          # log2_max_frame_num_minus4
    poc = r.ue()
    if poc == 0:
        r.ue()
    elif poc == 1:
        r.u(1); r.se(); r.se()
        for _ in range(r.ue()):
            r.se()
    r.ue()                          # max_num_ref_frames
    r.u(1)                          # gaps_in_frame_num_value_allowed_flag
    w_at = r.p
    r.ue()                          # pic_width_in_mbs_minus1
    w_end = r.p
    r.ue()                          # pic_height_in_map_units_minus1
    assert r.u(1) == 1              # frame_mbs_only_flag
    r.u(1)                          # direct_8x8_inference_flag
    c_at = r.p
    if r.u(1):
        for _ in range(4):
            r.ue()
    c_end = r.p
    new_crop = [0] if crop is None else [1] + sum((ue_bits(x) for x in crop), [])
    width = bits[w_at:w_end] if width_mbs is None else ue_bits(width_mbs - 1)
    tail = bits[c_end:]
    while tail and tail[-1] == 0:   # alignment zeros; the stop bit stays
        tail.pop()
    out = bits[:w_at] + width + bits[w_end:c_at] + new_crop + tail
    out += [0] * (-len(out) % 8)
    rbsp = bytes(int("".join(map(str, out[k:k + 8])), 2) for k in range(0, len(out), 8))
    return sps[:1] + escape(rbsp)


def rewrite_clip(clip: bytes, **kw) -> bytes:
    out, at = bytearray(), 0
    for s, e in nal_units(clip):
        out += clip[at:s]
        nal = clip[s:e]
        out += rewrite_sps(nal, **kw) if nal and nal[0] & 0x1F == 7 else nal
        at = e
    out += clip[at:]
    return bytes(out)


# ---- the harness ----------------------------------------------------------

def build(tmp: Path) -> Path:
    cc = shlex.split(os.environ.get("CC", "cc"))
    exe = tmp / "crop"
    flags = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all"]
    probe = tmp / "probe.c"
    probe.write_text("int main(void) { return 0; }\n")
    if subprocess.run([*cc, *flags, str(probe), "-o", str(tmp / "probe")], capture_output=True).returncode:
        flags = []
    objs = []
    for i, src in enumerate(sorted(TINY.glob("*.c"))):   # vendored: its own style
        obj = tmp / f"t{i}.o"
        r = subprocess.run([*cc, "-std=c11", "-O1", "-g", *flags, "-w", "-I", str(TINY), "-c", str(src), "-o",
                            str(obj)], text=True, capture_output=True)
        if r.returncode:
            raise AssertionError(r.stdout + r.stderr)
        objs.append(str(obj))
    r = subprocess.run([*cc, "-std=c11", "-O1", "-g", *flags, "-Wall", "-Wextra", "-Werror",
                        "-Wno-unused-parameter", "-Wno-unused-function", "-I", str(MUSE), "-I", str(TINY), "-I", str(VSTREAM / "include"),
                        str(ROOT / "tests/video_crop_harness.c"), str(MUSE / "muse_vdec.c"),
                        str(MUSE / "muse_vdec_same.c"), *objs, "-lm", "-o", str(exe)],
                       text=True, capture_output=True)
    if r.returncode:
        raise AssertionError(r.stdout + r.stderr)
    return exe


class VideoCrop(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.tmpd = tempfile.TemporaryDirectory(prefix="video-crop-")
        cls.tmp = Path(cls.tmpd.name)
        cls.exe = build(cls.tmp)
        real = (VEC / "vt_112_crop.264").read_bytes()
        cls.clips = {
            "real": real,
            "lt": rewrite_clip(real, crop=(24, 16, 4, 4)),
            "whole": rewrite_clip(real, crop=None),
            "wide": rewrite_clip(real, crop=(0, 40, 0, 0), width_mbs=20),
            "v80": (VEC / "vt_80_crop.264").read_bytes(),
        }

    @classmethod
    def tearDownClass(cls) -> None:
        cls.tmpd.cleanup()

    def decode(self, name: str, w: int, h: int) -> tuple[list[dict], bytes]:
        clip = self.tmp / f"{name}.264"
        clip.write_bytes(self.clips[name])
        out = self.tmp / f"{name}_{w}x{h}.yuv"
        r = subprocess.run([str(self.exe), "decode", str(clip), str(w), str(h), str(out)], text=True,
                           capture_output=True, timeout=120)
        self.assertEqual(r.returncode, 0, r.stdout[-2000:] + r.stderr[-4000:])
        self.assertNotIn("runtime error", r.stderr)
        return [json.loads(x) for x in r.stdout.splitlines()], out.read_bytes()

    def window(self, *args: int) -> dict:
        r = subprocess.run([str(self.exe), "window", *map(str, args)], text=True, capture_output=True)
        self.assertEqual(r.returncode, 0, r.stderr)
        return json.loads(r.stdout)

    def assert_pictures(self, aus: list[dict], n: int, w: int, h: int) -> None:
        pics = [a for a in aus if a["rc"] == 1]
        self.assertEqual(len(pics), n, aus[:3])
        self.assertFalse([a for a in aus if a["rc"] == -1], "decode errors")
        for a in pics:
            self.assertEqual((a["w"], a["h"]), (w, h), a)
            # progress: never past the window or back, ends on the whole window
            self.assertEqual((a["past"], a["back"], a["bad_planes"], a["last_ok"]), (0, 0, 0, 1), a)

    @staticmethod
    def crop_frames(whole: bytes, cw: int, ch: int, x: int, y: int, w: int, h: int) -> bytes:
        """The window (x, y, w, h) of each cw x ch I420 picture."""
        out, fs = bytearray(), cw * ch * 3 // 2
        for f in range(0, len(whole), fs):
            pic = whole[f:f + fs]
            for r in range(h):
                out += pic[(y + r) * cw + x:(y + r) * cw + x + w]
            for plane in (cw * ch, cw * ch + cw * ch // 4):
                for r in range(h // 2):
                    o = plane + (y // 2 + r) * (cw // 2) + x // 2
                    out += pic[o:o + w // 2]
        return bytes(out)

    def test_crop_window_math(self) -> None:
        # 4:2:0 frames (the iPhone's): units of 2 x 2
        self.assertEqual(self.window(192, 112, 1, 1, 0, 40, 0, 0), {"ok": 1, "x": 0, "y": 0, "w": 112, "h": 112})
        self.assertEqual(self.window(192, 112, 1, 1, 24, 16, 4, 4), {"ok": 1, "x": 48, "y": 8, "w": 112, "h": 96})
        self.assertEqual(self.window(1088, 1920, 1, 1, 0, 4, 0, 0), {"ok": 1, "x": 0, "y": 0, "w": 1080, "h": 1920})
        self.assertEqual(self.window(1920, 1088, 1, 1, 0, 0, 0, 4), {"ok": 1, "x": 0, "y": 0, "w": 1920, "h": 1080})
        # fields (frame_mbs_only 0): 4:2:0 crops 2 x 4, monochrome 1 x 2
        self.assertEqual(self.window(64, 64, 1, 0, 1, 1, 1, 1), {"ok": 1, "x": 2, "y": 4, "w": 60, "h": 56})
        self.assertEqual(self.window(64, 64, 0, 0, 1, 1, 1, 1), {"ok": 1, "x": 1, "y": 2, "w": 62, "h": 60})
        # monochrome, 4:2:2 and 4:4:4 frames: 1 x 1, 2 x 1, 1 x 1
        self.assertEqual(self.window(64, 64, 0, 1, 3, 0, 5, 0), {"ok": 1, "x": 3, "y": 5, "w": 61, "h": 59})
        self.assertEqual(self.window(64, 64, 2, 1, 3, 0, 5, 0), {"ok": 1, "x": 6, "y": 5, "w": 58, "h": 59})
        self.assertEqual(self.window(64, 64, 3, 1, 3, 1, 5, 1), {"ok": 1, "x": 3, "y": 5, "w": 60, "h": 58})
        # nothing left, overflowing offsets, an unknown format: the whole picture
        for args in ((192, 112, 1, 1, 48, 48, 0, 0), (192, 112, 1, 1, 0, 0, 28, 28),
                     (192, 112, 1, 1, 0xFFFFFFFF, 0xFFFFFFFF, 0, 0), (64, 64, 4, 1, 0, 0, 0, 0)):
            with self.subTest(args=args):
                self.assertEqual(self.window(*args), {"ok": 0, "x": 0, "y": 0, "w": args[0], "h": args[1]})

    def test_real_iphone_capture_is_cropped_and_never_refused(self) -> None:
        # start_streaming says 112x112 (after cropping); the SPS codes 192x112.
        # The decoder must not refuse it (coded wider than the stream).
        aus, yuv = self.decode("real", 112, 112)
        self.assert_pictures(aus, 60, 112, 112)
        self.assertEqual(hashlib.md5(yuv).hexdigest(), FFMPEG_MD5["vt_112_crop.264"])

    def test_right_and_bottom_crop(self) -> None:
        aus, yuv = self.decode("v80", 80, 80)
        self.assert_pictures(aus, 60, 80, 80)
        self.assertEqual(hashlib.md5(yuv).hexdigest(), FFMPEG_MD5["vt_80_crop.264"])

    def test_left_and_top_offsets_are_honored(self) -> None:
        _, whole = self.decode("whole", 192, 112)
        aus, yuv = self.decode("lt", 112, 96)
        self.assert_pictures(aus, 60, 112, 96)
        self.assertEqual(yuv, self.crop_frames(whole, 192, 112, 48, 8, 112, 96))
        self.assertEqual(hashlib.md5(yuv).hexdigest(), FFMPEG_MD5["lt"])
        # The real capture's window is the coded pictures' top left 112x112.
        _, real = self.decode("real", 112, 112)
        self.assertEqual(real, self.crop_frames(whole, 192, 112, 0, 0, 112, 112))

    def test_macroblock_map_only_where_it_lines_up(self) -> None:
        # Right/bottom cropping: the map's macroblocks are the picture's.
        aus, _ = self.decode("real", 112, 112)
        self.assertTrue(any(a["same"] for a in aus if a["rc"] == 1))
        # A left/top offset shifts the grid: no map (everything compared).
        aus, _ = self.decode("lt", 112, 96)
        self.assertFalse(any(a["same"] for a in aus if a["rc"] == 1))

    def test_size_mismatch_rules(self) -> None:
        # A cropped picture smaller than the stream said: shown (the show
        # task fits it), never refused.
        aus, _ = self.decode("lt", 112, 112)
        self.assert_pictures(aus, 60, 112, 96)
        # Larger than the stream's size in whole macroblocks after cropping:
        # refused (VIDEO.md Trust), here 112 wide against a 96 stream.
        aus, yuv = self.decode("real", 96, 112)
        self.assertFalse([a for a in aus if a["rc"] == 1], aus[:2])
        self.assertEqual(yuv, b"")
        # The whole coded picture (no cropping) against a 112 stream: refused.
        aus, _ = self.decode("whole", 112, 112)
        self.assertFalse([a for a in aus if a["rc"] == 1])
        # Within the macroblocks (an uncropped stream padded to them, as
        # before): taken, and the show task shows the stream's size of it.
        aus, _ = self.decode("whole", 190, 100)
        self.assert_pictures(aus, 60, 192, 112)
        # A coded picture past the padding allowance (20 macroblocks for a
        # 112 stream: at most max(7 + 2, 12)) is refused before anything is
        # allocated, cropped or not.
        aus, _ = self.decode("wide", 112, 112)
        self.assertFalse([a for a in aus if a["rc"] == 1])


if __name__ == "__main__":
    unittest.main()
