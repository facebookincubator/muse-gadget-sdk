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

"""The video decoders on input from the network: tinyh264 refuses parameter
sets for pictures larger than the stream's (before allocating for them) and
any whose sizes would overflow, muse_vdec refuses VP8 keyframes larger
than the stream's, and pictures being drawn aren't freed under the reader;
every decoder takes whole frames of any size from one reused buffer with
nothing of an earlier one deciding what it reads; and the ring and the decode
task agree on the stream's band layout.

And the drawing of what they decode (BandSeams): every
piece the panel driver asks for is written exactly, never a byte past its DMA
buffer, at every band seam, every scale and one or two bands, on pictures from
the real decoder, whatever heights a stream's bands decode to."""
import concurrent.futures
import json
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
TINY = ROOT / "components/tinyh264/src"
MUSE = ROOT / "components/muse"


class Bits:
    def __init__(self):
        self.bits = []

    def u(self, n, v):
        self.bits += [(v >> (n - 1 - i)) & 1 for i in range(n)]

    def ue(self, v):
        v += 1
        n = v.bit_length()
        self.u(n - 1, 0)
        self.u(n, v)

    def rbsp(self):
        self.bits.append(1)
        while len(self.bits) % 8:
            self.bits.append(0)
        out = bytearray()
        for i in range(0, len(self.bits), 8):
            out.append(int("".join(map(str, self.bits[i:i + 8])), 2))
        # emulation prevention: 00 00 0x -> 00 00 03 0x
        res, zeros = bytearray(), 0
        for b in out:
            if zeros >= 2 and b <= 3:
                res.append(3)
                zeros = 0
            res.append(b)
            zeros = zeros + 1 if b == 0 else 0
        return bytes(res)


def nal(kind, b):
    return b"\x00\x00\x00\x01" + bytes([kind]) + b.rbsp()


def sps(width_mbs, height_mbs, crop_bottom=0, sps_id=0, num_ref_frames=1):
    """A baseline-profile sequence parameter set for width x height macroblocks
    (crop_bottom: rows cropped at the bottom, even)."""
    b = Bits()
    b.u(8, 66)   # profile_idc: baseline
    b.u(8, 0)    # constraint flags
    b.u(8, 30)   # level_idc
    b.ue(sps_id)  # seq_parameter_set_id
    b.ue(0)      # log2_max_frame_num_minus4
    b.ue(2)      # pic_order_cnt_type
    b.ue(num_ref_frames)  # num_ref_frames
    b.u(1, 0)    # gaps_in_frame_num_value_allowed_flag
    b.ue(width_mbs - 1)
    b.ue(height_mbs - 1)
    b.u(1, 1)    # frame_mbs_only_flag
    b.u(1, 1)    # direct_8x8_inference_flag
    if crop_bottom:
        b.u(1, 1)    # frame_cropping_flag
        b.ue(0)      # left, right, top (crop units of 2 x 2)
        b.ue(0)
        b.ue(0)
        b.ue(crop_bottom // 2)
    else:
        b.u(1, 0)    # frame_cropping_flag
    b.u(1, 0)    # vui_parameters_present_flag
    return nal(0x67, b)


def pps(slice_groups=None, pps_id=0, sps_id=0):
    """slice_groups(b) writes num_slice_groups_minus1 and what follows it."""
    b = Bits()
    b.ue(pps_id)  # pic_parameter_set_id
    b.ue(sps_id)  # seq_parameter_set_id
    b.u(1, 0)    # entropy_coding_mode_flag: CAVLC
    b.u(1, 0)    # pic_order_present_flag
    if slice_groups:
        slice_groups(b)
    else:
        b.ue(0)  # num_slice_groups_minus1
    b.ue(0)      # num_ref_idx_l0_active_minus1
    b.ue(0)      # num_ref_idx_l1_active_minus1
    b.u(1, 0)    # weighted_pred_flag
    b.u(2, 0)    # weighted_bipred_idc
    b.ue(0)      # pic_init_qp_minus26 (se 0)
    b.ue(0)      # pic_init_qs_minus26
    b.ue(0)      # chroma_qp_index_offset
    b.u(1, 1)    # deblocking_filter_control_present_flag
    b.u(1, 0)    # constrained_intra_pred_flag
    b.u(1, 0)    # redundant_pic_cnt_present_flag
    return nal(0x68, b)


def pcm(b, mbs):
    for _ in range(mbs):
        b.ue(25)   # mb_type: I_PCM
        while len(b.bits) % 8:
            b.u(1, 0)
        for _ in range(384):
            b.u(8, 0x80)


def idr(mbs, pps_id=0, idr_pic_id=0):
    """An IDR slice of mbs grey I_PCM macroblocks (mbs=0: the header alone)."""
    b = Bits()
    b.ue(0)      # first_mb_in_slice
    b.ue(7)      # slice_type: I (all slices)
    b.ue(pps_id)  # pic_parameter_set_id
    b.u(4, 0)    # frame_num
    b.ue(idr_pic_id)
    b.u(1, 0)    # no_output_of_prior_pics_flag
    b.u(1, 0)    # long_term_reference_flag
    b.ue(0)      # slice_qp_delta (se 0)
    b.ue(1)      # disable_deblocking_filter_idc
    pcm(b, mbs)
    return nal(0x65, b)


def non_idr(mbs, pps_id=0, frame_num=1):
    """A reference picture's I slice that isn't an IDR one, of mbs I_PCM macroblocks."""
    b = Bits()
    b.ue(0)      # first_mb_in_slice
    b.ue(7)      # slice_type: I (all slices)
    b.ue(pps_id)  # pic_parameter_set_id
    b.u(4, frame_num)
    b.u(1, 0)    # adaptive_ref_pic_marking_mode_flag
    b.ue(0)      # slice_qp_delta (se 0)
    b.ue(1)      # disable_deblocking_filter_idc
    pcm(b, mbs)
    return nal(0x61, b)


def luma(x, y):
    """The test pictures' luma: every row differs (never 0, which I_PCM disallows)."""
    return 1 + (y * 5 + x * 3) % 254


def chroma(x, y, k):
    return 1 + (y * (11 + k) + x * 7 + 40 * k) % 254


def idr_picture(width_mbs, height_mbs, y0):
    """An IDR slice of I_PCM macroblocks of the test picture from row y0 on
    (written a byte at a time where the samples are byte aligned)."""
    out, acc, n = bytearray(), 0, 0

    def u(bits, v):
        nonlocal acc, n
        acc, n = acc << bits | v, n + bits
        while n >= 8:
            n -= 8
            out.append(acc >> n & 0xFF)
        acc &= (1 << n) - 1

    def ue(v):
        v += 1
        k = v.bit_length()
        u(k - 1, 0)
        u(k, v)

    for v in (0, 7, 0):   # first_mb_in_slice, slice_type I, pic_parameter_set_id
        ue(v)
    u(4, 0)      # frame_num
    ue(0)        # idr_pic_id
    u(2, 0)      # no_output_of_prior_pics_flag, long_term_reference_flag
    ue(0)        # slice_qp_delta
    ue(1)        # disable_deblocking_filter_idc
    for my in range(height_mbs):
        rows = [bytes(luma(x, y0 + my * 16 + yy) for x in range(width_mbs * 16)) for yy in range(16)]
        crow = [[bytes(chroma(x, y0 // 2 + my * 8 + yy, k) for x in range(width_mbs * 8)) for yy in range(8)]
                for k in range(2)]
        for mx in range(width_mbs):
            ue(25)   # mb_type: I_PCM
            if n:
                u(8 - n, 0)   # pcm_alignment_zero_bits
            for yy in range(16):
                out += rows[yy][mx * 16:mx * 16 + 16]
            for k in range(2):
                for yy in range(8):
                    out += crow[k][yy][mx * 8:mx * 8 + 8]
    u(1, 1)      # rbsp_stop_one_bit
    if n:
        u(8 - n, 0)
    res, zeros = bytearray(), 0
    for x in out:   # emulation prevention
        if zeros >= 2 and x <= 3:
            res.append(3)
            zeros = 0
        res.append(x)
        zeros = zeros + 1 if x == 0 else 0
    return b"\x00\x00\x00\x01\x65" + bytes(res)


def band(width, rows, y0, cropped=True):
    """A band of the test picture (rows y0 to y0 + rows), its own H.264 stream:
    coded to whole macroblocks, cropped to rows (cropped=False: rows whole
    macroblocks, uncropped)."""
    hm = (rows + 15) // 16
    return sps(width // 16, hm, hm * 16 - rows if cropped else 0) + pps() + idr_picture(width // 16, hm, y0)


def au(width_mbs, height_mbs, full=True, slice_groups=None):
    return sps(width_mbs, height_mbs) + pps(slice_groups) + idr(width_mbs * height_mbs if full else 0)


LONGEST = 2**32 - 1   # the longest Exp-Golomb code: plus one wraps to 0


def changing_groups(rate_minus1):
    """Two slice groups, box-out (type 3), changing by rate_minus1 + 1."""
    def write(b):
        b.ue(1)              # num_slice_groups_minus1
        b.ue(3)              # slice_group_map_type: box-out
        b.u(1, 0)            # slice_group_change_direction_flag
        b.ue(rate_minus1)    # slice_group_change_rate_minus1
    return write


def interleaved_groups(run_minus1):
    """Two interleaved slice groups (type 0), each of run_minus1 + 1."""
    def write(b):
        b.ue(1)              # num_slice_groups_minus1
        b.ue(0)              # slice_group_map_type: interleaved
        b.ue(run_minus1)     # run_length_minus1, group 0
        b.ue(run_minus1)     # run_length_minus1, group 1
    return write


HARNESS = r'''
#include <stdio.h>
#include <stdlib.h>
#include "h264bsd_decoder.h"
int main(int argc, char **argv)
{
    /* max_w_mbs max_h_mbs file: prints the decoder's result for the stream */
    storage_t *s = h264bsdAlloc();
    if (h264bsdInit(s, 1)) return 2;
    h264bsdSetMaxSize(s, (u32)atoi(argv[1]), (u32)atoi(argv[2]));
    FILE *f = fopen(argv[3], "rb");
    static u8 buf[1 << 20];
    size_t n = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    u8 *pic; u32 w, h, rc = H264BSD_RDY, used = 0;
    /* Each call takes one NAL unit: on until a picture, an error or the end. */
    while (used < n && rc == H264BSD_RDY) {
        u32 left = (u32)n - used;
        rc = h264bsdDecode(s, buf + used, left, &pic, &w, &h);
        used += left - s->strm->strmBuffSize;   /* what the call consumed */
        if (rc == H264BSD_HDRS_RDY) rc = H264BSD_RDY;
    }
    printf("%u %u %u\n", rc, rc == H264BSD_PIC_RDY ? w : 0, rc == H264BSD_PIC_RDY ? h : 0);
    h264bsdShutdown(s);
    h264bsdFree(s);
    return 0;
}
'''


class UntrustedVideoTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.exe = Path(cls.tmp.name, "tiny")
        src = Path(cls.tmp.name, "harness.c")
        src.write_text(HARNESS)
        cc = shlex.split(os.environ.get("CC", "cc"))
        srcs = [str(p) for p in sorted(TINY.glob("*.c"))]
        # With AddressSanitizer: a size that overflows shows as a crash, not a pass.
        # And a division by zero aborts, as on the board (arm64 hosts return 0).
        build = subprocess.run([*cc, "-std=gnu11", "-O1", "-g", "-w", "-fsanitize=address",
                                "-fsanitize=integer-divide-by-zero",
                                "-fno-sanitize-recover=integer-divide-by-zero", f"-I{TINY}", "-o",
                                str(cls.exe), str(src), *srcs], capture_output=True, text=True)
        if build.returncode:
            raise AssertionError(build.stdout + build.stderr)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def decode(self, stream, max_w, max_h):
        f = Path(self.tmp.name, "s.264")
        f.write_bytes(stream)
        out = subprocess.run([str(self.exe), str(max_w), str(max_h), str(f)], capture_output=True, text=True,
                             timeout=30)
        self.assertEqual(out.returncode, 0, out.stdout + out.stderr)
        rc, w, h = map(int, out.stdout.split())
        return (w, h) if rc == 1 else None   # H264BSD_PIC_RDY

    def test_pictures_within_the_stream_decode(self):
        self.assertEqual(self.decode(au(15, 15), 15, 15), (240, 240))   # 232 x 232: the Waveshare's stream
        self.assertEqual(self.decode(au(15, 8), 15, 15), (240, 128))    # a band
        self.assertEqual(self.decode(au(2, 1), 0, 0), (32, 16))         # no caller limit

    def test_pictures_larger_than_the_stream_are_refused(self):
        self.assertIsNone(self.decode(au(16, 15), 15, 15))
        self.assertIsNone(self.decode(au(15, 16), 15, 15))

    def test_sizes_that_would_overflow_are_refused(self):
        # 2^30 macroblocks: every buffer size wraps to 0 in 32 bits.
        self.assertIsNone(self.decode(au(32768, 32768, full=False), 0, 0))
        self.assertIsNone(self.decode(au(513, 1, full=False), 0, 0))
        # The longest Exp-Golomb code, 2^32 - 1: plus one wraps to 0 macroblocks,
        # which GetDpbSize divided by (a crash on the board, SIGFPE here).
        self.assertIsNone(self.decode(au(2**32, 1, full=False), 0, 0))
        self.assertIsNone(self.decode(au(1, 2**32, full=False), 0, 0))

    def test_slice_group_sizes_that_would_wrap_are_refused(self):
        # A change rate of 0 divided in NumSliceGroupChangeCycleBits; a run of
        # 0 never ended the interleaved map (the harness's timeout catches it).
        self.assertIsNone(self.decode(au(2, 2, full=False, slice_groups=changing_groups(LONGEST)), 0, 0))
        self.assertIsNone(self.decode(au(2, 2, full=True, slice_groups=interleaved_groups(LONGEST)), 0, 0))
        # Slice groups aren't taken at all (nothing that streams to a gadget
        # makes them): an in-range rate and run are refused too, and the
        # same stream with one slice group still decodes.
        self.assertIsNone(self.decode(au(2, 2, full=True, slice_groups=changing_groups(1)), 0, 0))
        self.assertIsNone(self.decode(au(2, 2, full=True, slice_groups=interleaved_groups(1)), 0, 0))
        self.assertEqual(self.decode(au(2, 2), 0, 0), (32, 32))

    def test_slice_group_maps_cannot_take_the_heap(self):
        # A map of explicit slice group ids (type 6) was allocated at up to
        # 512 x 512 entries and kept per PPS id: 256 of them, a megabyte each.
        explicit = lambda b: (b.ue(1), b.ue(6), b.ue(512 * 512 - 1))
        self.assertIsNone(self.decode(au(2, 2, full=False, slice_groups=explicit), 0, 0))
        tiny = (ROOT / "components/tinyh264/src/h264bsd_pic_param_set.c").read_text()
        self.assertIn("slice groups unsupported", tiny)

    def test_vp8_keyframes_larger_than_the_stream(self):
        vdec = (MUSE / "muse_vdec.c").read_text()
        self.assertIn("kw > d->max_w || kh > d->max_h", vdec)
        self.assertIn("h264bsdSetMaxSize(d->h264,", vdec)
        video = (MUSE / "muse_video.c").read_text()
        self.assertEqual(video.count("muse_vdec_open(s_params.codec, s_params.width, s_params.height)"), 2)
        # Pictures handed to the show task are never freed under it.
        self.assertIn("bool reset = muse_vdec_may_realloc(", video)
        self.assertIn("need > pp->h || a->vis_w > pp->w", video)

    def test_wifi_port_guards(self):
        wifi = (MUSE / "muse_video_wifi.c").read_text()
        self.assertIn('refuse_start(m, sizeof(m), "closed");', wifi)
        self.assertIn('close_client("frame too long")', wifi)
        self.assertIn('close_client("said nothing")', wifi)
        self.assertIn('close_client("not reading")', wifi)
        self.assertIn("turned away: a client is connected", wifi)
        # Turning one away leaves the talking client's parser where it was.
        self.assertIn("if (accept_client(lsock)) {\n                st = RX_TYPE;", wifi)
        # A stream whose picture is too small to show is dropped, never divided by (muse_video_fit.c).
        video = (MUSE / "muse_video.c").read_text()
        self.assertIn("if (!fits) {", video)
        fit = (MUSE / "muse_video_fit.c").read_text()
        self.assertIn("if (vis_w <= 0 || vis_h <= 0) {\n        return false;", fit)
        kc = (MUSE / "Kconfig").read_text()
        opt = kc[kc.index("config MUSE_VIDEO_WIFI_START_ANYTIME"):]
        self.assertIn("default n", opt[:opt.index("help")])


SEAMS_DOC = """Band seams, by scale (the ESP-Mosaico's 480 x 480 panel, two bands):
2x from 240 px wide, 3/2 from 320, one to one from 480 (video, and UI
streams with the plain and the fine conversion), scaled from 400; and one
band on the 466 and 412 panels (round: the cover) and the 480 one. Heights
that are multiples of 32 are what a BLE start may ask for with two bands
(mg_proto.c); Wi-Fi's start takes any height."""


class BandSeams(unittest.TestCase):
    __doc__ = SEAMS_DOC
    PANEL = (480, 480)
    # (name, width, height, ui, hq): two-band streams at each scale.
    SCALES = [
        ("2x", 240, 68, 0, 0),
        ("2x", 240, 64, 0, 0),
        ("2x", 240, 128, 0, 0),
        ("3/2", 320, 320, 0, 0),
        ("1x", 480, 480, 0, 0),
        ("1x ui", 480, 466, 1, 0),
        ("1x ui hq", 480, 466, 1, 1),
        ("scaled", 400, 400, 0, 0),
    ]

    @classmethod
    def setUpClass(cls):
        cls.tmpd = tempfile.TemporaryDirectory(prefix="video-seams-")
        cls.tmp = Path(cls.tmpd.name)
        cc = shlex.split(os.environ.get("CC", "cc"))
        san = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all"]
        objs = []
        for i, src in enumerate(sorted(TINY.glob("*.c"))):   # vendored: its own style
            obj = cls.tmp / f"t{i}.o"
            r = subprocess.run([*cc, "-std=c11", "-O1", "-g", *san, "-w", "-I", str(TINY), "-c", str(src), "-o",
                                str(obj)], capture_output=True, text=True)
            if r.returncode:
                raise AssertionError(r.stdout + r.stderr)
            objs.append(str(obj))
        # The copy keeps muse_video.c's own directory out of its include path,
        # so the fakes stand in for muse_board.h and the IDF headers.
        (cls.tmp / "muse_video.c").write_bytes((MUSE / "muse_video.c").read_bytes())
        tests = ROOT / "tests"
        inc = ["-I", str(tests / "video_seams_fakes"), "-I", str(tests / "video_slots/fake"), "-I", str(cls.tmp),
               "-I", str(MUSE), "-I", str(ROOT / "components/muse_vstream/include"), "-I", str(TINY)]
        warn = ["-Wall", "-Werror", "-Wno-unused-function"]
        for name in ("muse_vdec", "muse_vdec_same", "muse_yuv"):
            obj = cls.tmp / f"{name}.o"
            r = subprocess.run([*cc, "-std=gnu11", "-O1", "-g", *san, *warn, *inc, "-c", str(MUSE / f"{name}.c"), "-o",
                                str(obj)], capture_output=True, text=True)
            if r.returncode:
                raise AssertionError(r.stdout + r.stderr)
            objs.append(str(obj))
        cls.exe = cls.tmp / "seams"
        r = subprocess.run([*cc, "-std=gnu11", "-O1", "-g", *san, *warn, "-Wno-unused-variable",
                            "-Wno-unused-but-set-variable", *inc, str(tests / "video_seams_harness.c"), *objs,
                            "-lpthread", "-lm", "-o", str(cls.exe)], capture_output=True, text=True)
        if r.returncode:
            raise AssertionError(r.stdout + r.stderr[-8000:])
        cls.streams = {}

    @classmethod
    def tearDownClass(cls):
        cls.tmpd.cleanup()

    def stream(self, width, rows, y0, cropped=True):
        key = (width, rows, y0, cropped)
        if key not in self.streams:
            f = self.tmp / f"b{width}_{rows}_{y0}_{int(cropped)}.264"
            f.write_bytes(band(width, rows, y0, cropped))
            self.streams[key] = f
        return self.streams[key]

    def draw(self, width, height, top, bottom, ui=0, hq=0, phase=1, exact=False, top_rows=0, panel=None,
             bottom_cropped=True):
        """Draws a stream whose top band is top rows and bottom band bottom
        rows (None: one band) and checks every piece's bytes. Returns the
        harness's numbers."""
        pw, ph = panel or self.PANEL
        t = self.stream(width, top, 0)
        b = "-" if bottom is None else str(self.stream(width, bottom, top, bottom_cropped))
        args = [str(self.exe), "seams", str(pw), str(ph), str(ui), str(hq), str(width), str(height), str(t), b,
                str(phase), str(int(exact))]
        if top_rows:
            args.append(str(top_rows))
        r = subprocess.run(args, capture_output=True, text=True, timeout=120)
        what = f"{width}x{height} bands {top}+{bottom} rows {top_rows} phase {phase} panel {pw}x{ph}"
        self.assertEqual(r.returncode, 0, what + "\n" + r.stdout + r.stderr[-3000:])
        self.assertNotIn("runtime error", r.stderr, what)
        self.assertNotIn("AddressSanitizer", r.stderr, what)
        out = json.loads(r.stdout)
        self.assertTrue(out["decoded"] and out["fits"], (what, out))
        if not exact:
            self.assertEqual((out["past_slice"], out["past_chunk"], out["ref_spill"]), (0, 0, 0), (what, out))
        return out

    def test_the_68_row_seam_is_drawn_whole_and_bounded(self):
        # 240 x 68 two times upscaled: bands of 32 and 48 (the bottom coded
        # whole, the stream's 68 in whole macroblocks) put the old seam at
        # output row 54, mid-pair; and 32 + 36 (cropped) while it decodes.
        for bottom, cropped in ((48, False), (36, True)):
            for phase in (0, 1):
                for exact in (False, True):
                    out = self.draw(240, 68, 32, bottom, phase=phase, exact=exact, bottom_cropped=cropped)
                    self.assertEqual((out["scale"], out["out_h"], out["seam"]), (2, 136, 64), out)
                    self.assertEqual(out["mismatch_rows"], 0, out)
        # The old seam: at 54 while the bands decode.
        self.assertEqual(self.draw(240, 68, 32, 48, phase=0, bottom_cropped=False)["old_seam"], 54)

    def test_the_64_row_stream_is_unchanged(self):
        for phase in (0, 1):
            for exact in (False, True):
                out = self.draw(240, 64, 32, 32, phase=phase, exact=exact)
                self.assertEqual((out["seam"], out["old_seam"], out["mismatch_rows"]), (64, 64, 0), out)

    def test_a_cropped_top_band_at_three_halves(self):
        # 320 x 320 at 3/2 (a height a BLE start takes with two bands): a top
        # band cropped to 18 rows put the old seam at 27, odd.
        for phase in (0, 1):
            for exact in (False, True):
                out = self.draw(320, 320, 18, 302, phase=phase, exact=exact)
                self.assertEqual((out["scale"], out["old_seam"], out["seam"]), (3, 27, 24), out)

    def test_conforming_bands_draw_the_picture(self):
        # Bands as the protocol has them (the top whole macroblocks; the
        # bottom cropped to the stream, or whole macroblocks): the picture
        # drawn is the stream's rows, the same as drawn in one band.
        for name, w, h, ui, hq in self.SCALES:
            h16 = (h + 15) & ~15
            tops = sorted({x for x in (16, 32, 48, 64, 96, 288, h16 - 32, h16 - 16) if 0 < x < h})
            for top in tops:
                for bottom, cropped in ((h - top, True), (h16 - top, False)):
                    for phase in (0, 1):
                        out = self.draw(w, h, top, bottom, ui, hq, phase, bottom_cropped=cropped)
                        if name != "scaled" and not hq:   # (scaled and fine conversions blend across rows)
                            self.assertEqual(out["mismatch_rows"], 0, (name, top, bottom, out))

    def test_every_seam_position_at_every_scale(self):
        # A top band of any height (as a VP8 band may be, or a cropped H.264
        # one) at every scale: every row the old seam could fall on mod 8,
        # and the new seam always on the converter's own unit.
        unit = {1: 2, 2: 4, 3: 6, 0: 2}
        old = {}
        for name, w, h, ui, hq in self.SCALES:
            for rows in range(1, 48):
                for phase in (0, 1):
                    out = self.draw(w, h, 48, h - 48, ui, hq, phase, top_rows=rows)
                    old.setdefault(name, set()).add(out["old_seam"] % 8)
                    self.assertEqual(out["seam"] % unit[out["scale"]], 0, (name, rows, out))
        for name, seams in old.items():
            self.assertEqual(seams, set(range(8)), name)
        # And under AddressSanitizer with buffers exactly the driver's.
        for name, w, h, ui, hq in self.SCALES:
            for rows in (1, 3, 5, 7, 9, 18, 27, 33):
                self.draw(w, h, 48, h - 48, ui, hq, 0, exact=True, top_rows=rows)

    def test_one_band_at_every_scale(self):
        cases = [(240, 240, None), (320, 320, None), (480, 480, None), (400, 400, None), (240, 68, None),
                 (208, 208, (412, 412)), (240, 240, (466, 466)), (232, 232, (466, 466))]
        for w, h, panel in cases:
            for exact in (False, True):
                self.draw(w, h, h, None, exact=exact, panel=panel)
        # Round panels (the cover), and odd heights the stream's own picture may decode to.
        for w, h, panel in [(208, 208, (412, "412r")), (240, 240, (466, "466r")), (112, 112, (466, "466r"))]:
            for rows in (h, h - 2, h - 6, h - 10):
                self.draw(w, h, h, None, exact=False, panel=panel, top_rows=rows if rows != h else 0)
                self.draw(w, h, h, None, exact=True, panel=panel, top_rows=rows if rows != h else 0)

    def test_converters_write_exactly_their_slice(self):
        r = subprocess.run([str(self.exe), "conv"], capture_output=True, text=True, timeout=120)
        self.assertEqual(r.returncode, 0, r.stderr[-3000:])
        self.assertNotIn("runtime error", r.stderr)
        lines = [json.loads(x) for x in r.stdout.splitlines()]
        self.assertEqual(len(lines), 6, r.stdout)
        total = lines[-1]
        self.assertGreater(total["cases"], 1000)
        self.assertEqual((total["spill"], total["mismatch_rows"]), (0, 0), r.stdout)

    def test_band_heights(self):
        cases = {
            (32, 36, 68): 1,    # bottom cropped to the stream
            (32, 48, 68): 1,    # both whole macroblocks (68 coded as 80)
            (288, 192, 480): 1,
            (288, 192, 466): 1,
            (288, 178, 466): 1,
            (32, 32, 64): 1,
            (18, 302, 320): 0,  # a cropped top band
            (40, 40, 68): 0,    # slack in the top band
            (30, 38, 68): 0,
            (32, 40, 68): 0,    # the bottom neither cropped to the stream nor whole
            (64, 16, 64): 0,    # the top band the whole stream
            (0, 64, 64): 0,
            (16, 0, 64): 0,
            (48, 48, 68): 0,
            (32, 64, 68): 0,
            # main's bands_fit verdicts (#390), the same under this rule
            (48, 32, 68): 1,
            (18, 62, 68): 0,
            (80, 16, 68): 0,
            (64, 32, 68): 0,
            (0, 80, 68): 0,
            (480, 16, 480): 0,
            (18, 462, 480): 0,
            (288, 208, 480): 0,
        }
        args = [str(self.exe), "bands"] + [str(v) for k in cases for v in k]
        r = subprocess.run(args, capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(list(map(int, r.stdout.split())), list(cases.values()))


def idr16(mbs, idr_pic_id):
    """An IDR slice of mbs flat I_16x16 macroblocks (DC, no residual): a few
    bits each, so a keyframe can be tiny and leave its picture unfinished."""
    b = Bits()
    b.ue(0)      # first_mb_in_slice
    b.ue(7)      # slice_type: I (all slices)
    b.ue(0)      # pic_parameter_set_id
    b.u(4, 0)    # frame_num
    b.ue(idr_pic_id)
    b.u(1, 0)    # no_output_of_prior_pics_flag
    b.u(1, 0)    # long_term_reference_flag
    b.ue(0)      # slice_qp_delta (se 0)
    b.ue(1)      # disable_deblocking_filter_idc
    for _ in range(mbs):
        b.ue(3)    # mb_type: I_16x16_2_0_0 (DC, no coded block pattern)
        b.ue(0)    # intra_chroma_pred_mode: DC
        b.ue(0)    # mb_qp_delta (se 0)
        b.u(1, 1)  # Intra16x16DCLevel coeff_token: no coefficients
    return nal(0x65, b)


AU_MAX = 64 * 1024   # tests/vdec_stale_harness.c's buffer, as a single-band stream's s_au
VSTREAM_INC = ROOT / "components/muse_vstream/include"
VP8 = ROOT / "components/libvpx_vp8/src"
VP8_KEYS = ROOT / "tests/vectors/vp8/p0_keyframes.ivf"   # 240 x 240, a keyframe then inter frames


def ivf_frames(path):
    ivf, frames, at = path.read_bytes(), [], 32
    while at + 12 <= len(ivf):
        n = int.from_bytes(ivf[at:at + 4], "little")
        frames.append(ivf[at + 12:at + 12 + n])
        at += 12 + n
    return frames


def vp8_failing_resize(key):
    """key cut to a key frame of a smaller size (224 wide) whose first
    partition runs past the frame: the decoder frees its pictures for the new
    size, then fails. The ring takes it (muse_vstream.c: the partition's
    length fits the frame)."""
    first = int.from_bytes(key[:3], "little") >> 5
    small = bytearray(key[:10 + first - 5])   # 3-byte tag, 7-byte key frame header
    small[6:8] = (224).to_bytes(2, "little")
    assert first <= len(small)
    return bytes(small)


def sanitizers(cc, tmp):
    """ASan and UBSan where the compiler has them (the harness's guard bytes
    catch a write without them)."""
    flags = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all"]
    probe = Path(tmp, "probe.c")
    probe.write_text("int main(void) { return 0; }\n")
    r = subprocess.run([*cc, *flags, str(probe), "-o", str(Path(tmp, "probe"))], capture_output=True)
    # Vendored decoders shift negative values as upstream ships them.
    return flags + ["-fno-sanitize=shift-base"] if r.returncode == 0 else []


class StaleStreamState(unittest.TestCase):
    """Whole frames of very different sizes, one after another in one reused
    buffer of AU_MAX bytes, through each decoder (tinyh264 alone and behind
    muse_vdec.c, VP8 behind muse_vdec.c): a frame decodes exactly as it does after reopening the
    decoder (result, picture size and pixels), nothing past the frame is read
    (ASan: the earlier frames' bytes after it and the guard past AU_MAX) or
    written (guard bytes), and a keyframe then decodes."""

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix="vdec-stale-")
        tmp = Path(cls.tmp.name)
        cc = shlex.split(os.environ.get("CC", "cc"))
        san = sanitizers(cc, tmp)
        harness = str(ROOT / "tests/vdec_stale_harness.c")
        tiny = [str(p) for p in sorted(TINY.glob("*.c"))]
        glue = ["-I", str(MUSE), "-I", str(VSTREAM_INC), str(MUSE / "muse_vdec.c")]
        builds = {
            "src": ["-DSTALE_DIRECT=1", f"-I{TINY}", *tiny],
            "src_vdec": [f"-I{TINY}", *glue, str(MUSE / "muse_vdec_same.c"), *tiny],
            "vp8_vdec": ["-DCONFIG_MUSE_VIDEO_VP8=1", f"-I{TINY}", f"-I{VP8}", *glue,
                         str(MUSE / "muse_vdec_same.c"), *tiny, *sorted(map(str, VP8.rglob("*.c")))],
        }
        cls.exe = {}

        def build(item):
            name, args = item
            exe = tmp / name
            # Vendored decoders as upstream ships them: their warnings aren't ours.
            r = subprocess.run([*cc, "-std=gnu11", "-O1", "-g", "-w", *san, *args,
                                harness, "-lm", "-o", str(exe)], capture_output=True, text=True)
            if r.returncode:
                raise AssertionError(name + ":\n" + r.stdout + r.stderr)
            return name, exe

        with concurrent.futures.ThreadPoolExecutor(max_workers=len(builds)) as pool:
            cls.exe = dict(pool.map(build, builds.items()))

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def frames(self, named):
        out = {}
        for name, data in named.items():
            self.assertLessEqual(len(data), AU_MAX, name)
            p = Path(self.tmp.name, name)
            p.write_bytes(data)
            out[name] = str(p)
        return out

    def run_harness(self, build, codec, files):
        r = subprocess.run([str(self.exe[build]), codec, *files], capture_output=True, text=True, timeout=120)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertNotIn("runtime error", r.stderr)
        self.assertNotIn("AddressSanitizer", r.stderr)
        lines = r.stdout.splitlines()
        self.assertEqual(lines[-1], "guard ok", r.stdout)
        return [line.split() for line in lines[:-1] if line != "reset"]

    def check(self, build, codec, f, picture, size=None):
        """f: a, b, c, k. The run a, b, c, k against a, b, reopen, c, k: each
        line is NAME LEN RC W H SUM (picture size and checksum)."""
        seq = self.run_harness(build, codec, [f["a"], f["b"], f["c"], f["k"]])
        ref = self.run_harness(build, codec, [f["a"], f["b"], "reset", f["c"], f["k"]])
        self.assertEqual(seq, ref, (build, seq, ref))
        self.assertEqual(seq[3][2], str(picture), (build, seq))   # the keyframe after them decodes
        if size:   # to a whole picture
            self.assertEqual(seq[3][3:5], list(map(str, size)), (build, seq))
        return seq

    def h264(self):
        return self.frames({
            # 28 B: a keyframe whose picture (2 macroblocks) has 1
            "h_a": sps(2, 1) + pps() + idr16(1, 0),
            # one NAL unit as long as the whole buffer (zero padding after its slice)
            "h_b": idr16(1, 1).ljust(AU_MAX, b"\0"),
            "h_c": b"\0\0\0\1",
            "h_k": sps(2, 1) + pps() + idr16(2, 2),
        })

    def test_tinyh264(self):
        f = self.h264()
        self.assertEqual(os.path.getsize(f["h_a"]), 28)
        f = {k[2:]: v for k, v in f.items()}
        seq = self.check("src", "direct", f, 1, (32, 16))   # H264BSD_PIC_RDY
        self.assertEqual(seq[2][2], "3", seq)     # 4 bytes, no NAL unit: H264BSD_ERROR
        seq = self.check("src_vdec", "h264", f, 1, (32, 16))   # MUSE_VDEC_PICTURE
        self.assertEqual(seq[2][2], "-1", seq)

    def test_vp8(self):
        ivf = (ROOT / "tests/vectors/vp8/p0_keyframes.ivf").read_bytes()   # 240 x 240
        frames, at = [], 32
        while at + 12 <= len(ivf):
            n = int.from_bytes(ivf[at:at + 4], "little")
            frames.append(ivf[at + 12:at + 12 + n])
            at += 12 + n
        key = frames[0]
        self.assertEqual(key[0] & 1, 0)   # a keyframe
        f = self.frames({"v_a": key[:28], "v_b": frames[1].ljust(AU_MAX, b"\0"), "v_c": key[:4], "v_k": key})
        self.check("vp8_vdec", "vp8", {k[2:]: v for k, v in f.items()}, 1, (240, 240))

    def test_vp8_inter_frame_after_a_failed_size_change(self):
        # A key frame of another size frees every picture, then fails, leaving
        # the references without memory. The inter frame after it predicted
        # from them through NULL; it is refused now, and the next key frame
        # decodes.
        frames = ivf_frames(VP8_KEYS)
        key, inter = frames[0], frames[1]
        self.assertEqual((key[0] & 1, inter[0] & 1), (0, 1))
        f = self.frames({"v_key": key, "v_small": vp8_failing_resize(key), "v_inter": inter, "v_key2": key})
        seq = self.run_harness("vp8_vdec", "vp8", [f["v_key"], f["v_small"], f["v_inter"], f["v_key2"]])
        self.assertEqual([line[2] for line in seq], ["1", "-1", "-1", "1"], seq)

    def test_decoders_keep_no_stream_position_across_calls(self):
        for path in (TINY / "h264bsd_decoder.c",):
            with self.subTest(path=path.parent.name):
                src = path.read_text()
                body = src[src.index("u32 h264bsdDecode(storage_t *pStorage"):]
                body = body[:body.index("\n}\n")]
                self.assertIn("pStorage->prevBufPointer = NULL;", body)
                self.assertIn("if (readBytes > len)", body)
                self.assertIn("pStorage->prevBytesConsumed <= len)", src)


# PIPE BANDS WIRE: VP8 frames as sent on the network (header, access unit)
# through the ring and admitted as muse_video.c's decode task does: drop all
# but a keyframe while waiting for one (after a gap or a failed decode), and
# split the bands by the layout the ring checked the frame in. Prints
# "NUMBER drop", or the number and each band's muse_vdec result, or "lost".
PIPE_HARNESS = r"""
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include "muse_vdec.h"
#include "muse_vstream.h"
#define AU_MAX (64 * 1024)
static uint8_t wire[1 << 18], ring[1 << 18], au[AU_MAX];
int main(int argc, char **argv)
{
    FILE *f = argc == 3 ? fopen(argv[2], "rb") : NULL;
    if (!f) return 2;
    size_t n = fread(wire, 1, sizeof(wire), f);
    fclose(f);
    muse_vstream_t vs;
    muse_vstream_init(&vs, ring, sizeof(ring), AU_MAX);
    vs.codec = MUSE_VIDEO_CODEC_VP8;
    vs.bands = (uint8_t)atoi(argv[1]);
    muse_vstream_start(&vs);
    muse_vstream_write(&vs, wire, n);
    muse_vdec_t *d[2] = { muse_vdec_open(MUSE_VIDEO_CODEC_VP8, 240, 240), muse_vdec_open(MUSE_VIDEO_CODEC_VP8, 240, 240) };
    if (!d[0] || !d[1]) return 2;
    bool need_key = true;
    muse_vframe_t fr;
    int32_t got;
    while ((got = muse_vstream_pop(&vs, au, AU_MAX, &fr)) != 0) {
        bool key = got > 0 && (fr.flags & MUSE_VIDEO_FRAME_KEY);
        if (got < 0 || (fr.after_gap && !key)) need_key = true;
        if (got < 0 || (need_key && !key)) {
            if (got < 0) printf("lost\n");
            else printf("%u drop\n", fr.number);
            continue;
        }
        need_key = false;
        uint32_t len[2] = { (uint32_t)got, 0 };
        const uint8_t *band[2] = { au, NULL };
        if (fr.bands == 2) {
            len[0] = au[0] | au[1] << 8 | au[2] << 16 | (uint32_t)au[3] << 24;
            if (len[0] > (uint32_t)got - 4) return 3;   /* the ring checked it */
            band[0] = au + 4;
            band[1] = au + 4 + len[0];
            len[1] = (uint32_t)got - 4 - len[0];
        }
        printf("%u", fr.number);
        for (int k = 0; k < fr.bands; k++) {
            muse_pic_t pic;
            int rc = muse_vdec_decode(d[k], band[k], len[k], &pic);
            printf(" %d", rc);
            need_key |= rc == MUSE_VDEC_ERROR;
        }
        printf("\n");
    }
    muse_vdec_close(d[0]);
    muse_vdec_close(d[1]);
    return 0;
}
"""


def wire_frame(number, key, au):
    return bytes([0x56, 1 if key else 0]) + number.to_bytes(2, "little") + len(au).to_bytes(4, "little") + au


def vp8_key_band(n):
    """A VP8 keyframe tag (first partition n - 10), sync code and 240 x 240, padded to n bytes."""
    tag = 1 << 4 | (n - 10) << 5
    return tag.to_bytes(3, "little") + b"\x9d\x01\x2a" + (240).to_bytes(2, "little") * 2 + b"\x55" * (n - 10)


class StreamLayoutTest(unittest.TestCase):
    """Validation and decoding agree on one band layout, the stream's: the
    ring checks a frame in the layout `start` gave and the decode task splits
    it the same way, so a keyframe flag can't carry a frame the decoder reads
    as something else (tests/test_muse_video.py checks the ring alone)."""

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix="vpipe-")
        tmp = Path(cls.tmp.name)
        cc = shlex.split(os.environ.get("CC", "cc"))
        src, cls.exe = tmp / "pipe.c", tmp / "pipe"
        src.write_text(PIPE_HARNESS)
        vstream = ROOT / "components/muse_vstream"
        r = subprocess.run([*cc, "-std=gnu11", "-O1", "-g", "-w", *sanitizers(cc, tmp), "-DCONFIG_MUSE_VIDEO_VP8=1",
                            f"-I{TINY}", f"-I{VP8}", "-I", str(MUSE), "-I", str(VSTREAM_INC), str(src),
                            str(MUSE / "muse_vdec.c"), str(MUSE / "muse_vdec_same.c"), str(vstream / "muse_vstream.c"),
                            *sorted(map(str, TINY.glob("*.c"))), *sorted(map(str, VP8.rglob("*.c"))), "-lm",
                            "-o", str(cls.exe)], capture_output=True, text=True)
        if r.returncode:
            raise AssertionError(r.stdout + r.stderr)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def run_wire(self, bands, data):
        path = Path(self.tmp.name, "stream.bin")
        path.write_bytes(data)
        r = subprocess.run([str(self.exe), str(bands), str(path)], capture_output=True, text=True, timeout=120,
                           env=dict(os.environ, ASAN_OPTIONS="detect_leaks=0"))
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertNotIn("runtime error", r.stderr)
        return [line.split() for line in r.stdout.splitlines()]

    def test_a_keyframe_flag_on_a_frame_shaped_as_two_bands(self):
        # One band. After a key frame of a new size that failed (frame 2), a
        # keyframe-flagged frame laid out as two bands of keyframes: as one
        # frame its first bytes, the band length, are an inter frame's tag.
        # The ring used to take it as two bands, the decode task then decoded
        # it whole, and the inter frame predicted from references the failed
        # key frame had freed. It isn't a frame of this stream now, and the
        # key frame after it decodes.
        frames = ivf_frames(VP8_KEYS)
        key, inter = frames[0], frames[1]
        top = 1 | 1 << 4 | 8 << 5   # as a tag: an inter frame, version 0, shown, first partition 8
        two = top.to_bytes(4, "little") + vp8_key_band(top) + vp8_key_band(40)

        def stream(flag):
            return (wire_frame(0, True, key) + wire_frame(1, False, inter) +
                    wire_frame(2, True, vp8_failing_resize(key)) + wire_frame(3, flag, two) + wire_frame(4, True, key))

        self.assertEqual(self.run_wire(1, stream(True)), [["0", "1"], ["1", "1"], ["2", "-1"], ["4", "1"]])
        # The one-bit control: without the flag, frame 3 is an inter frame to
        # the ring too, dropped while waiting for a keyframe.
        self.assertEqual(self.run_wire(1, stream(False)),
                         [["0", "1"], ["1", "1"], ["2", "-1"], ["3", "drop"], ["4", "1"]])
        # In a two-band stream the same layout is two frames, one per decoder,
        # and a one-band frame isn't one.
        both = len(key).to_bytes(4, "little") + key + key
        self.assertEqual(self.run_wire(2, wire_frame(0, True, both) + wire_frame(1, True, key)),
                         [["0", "1", "1"], ["lost"]])
        self.assertEqual(self.run_wire(1, wire_frame(0, True, both) + wire_frame(1, True, key)), [["1", "1"]])


# The colour converters on their own (main's video_fill_harness.c converters(),
# #390): exactly rows rows written for every count, from source planes holding
# only the rows those rows come from (each an allocation of exactly its size,
# for AddressSanitizer), and each row the same as when the count is whole row
# pairs (whole patterns at 3/2) from a taller picture.
CONVERTERS_HARNESS = r"""
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "muse_yuv.h"

#define PRE 64
#define GUARD 4096
#define UNSET 0x5A
#define FENCE 0xA5

static int fails;
static const char *what;

static void fail(const char *fmt, int a, int b)
{
    if (fails++ < 20) {
        printf("FAIL %s: ", what);
        printf(fmt, a, b);
        printf("\n");
    }
}

/* A decoder's picture: exactly w x h luma and two (w / 2) x (h / 2) chroma
 * planes in one allocation, as muse_vdec.c lays them out. */
typedef struct {
    uint8_t *buf;
    const uint8_t *y, *u, *v;
} pic_t;

static uint32_t s_seed = 1;
static uint8_t rnd(void)
{
    s_seed = s_seed * 1103515245u + 12345u;
    return (uint8_t)(s_seed >> 24);
}

static pic_t make_pic(int w, int h)
{
    size_t n = (size_t)w * h + 2 * (size_t)(w / 2) * (h / 2);
    pic_t pc = { malloc(n ? n : 1), NULL, NULL, NULL };
    for (size_t i = 0; i < n; i++) {
        pc.buf[i] = rnd();
    }
    pc.y = pc.buf;
    pc.u = pc.buf + (size_t)w * h;
    pc.v = pc.u + (size_t)(w / 2) * (h / 2);
    return pc;
}

int main(void)
{
    enum { W = 64, OS = 2 * W * 2, TALL = 32 };
    static uint8_t out[PRE + 16 * OS + GUARD], ref[16 * OS];
    pic_t tall = make_pic(W, TALL);
    for (int conv = 0; conv < 3; conv++) {
        /* Bytes each output row gets: 2x doubles the width, 3/2 halves again. */
        size_t row_bytes = conv == 0 ? 2 * W * 2 : conv == 1 ? W * 2 : W * 3 / 2 * 2;
        for (int rows = 1; rows <= 13; rows++) {
            int src_rows = conv == 0 ? (rows + 1) / 2 : conv == 1 ? rows : muse_yuv_up32_src_rows(1 << 20, rows);
            int h = (src_rows + 1) & ~1;
            pic_t pc = make_pic(W, h);
            /* The same pixels as the tall picture's first rows. */
            memcpy(pc.buf, tall.y, (size_t)W * h);
            memcpy((uint8_t *)pc.u, tall.u, (size_t)(W / 2) * (h / 2));
            memcpy((uint8_t *)pc.v, tall.v, (size_t)(W / 2) * (h / 2));
            memset(out, FENCE, sizeof(out));
            uint8_t *dst = out + PRE;
            memset(dst, UNSET, (size_t)rows * OS);
            memset(ref, UNSET, sizeof(ref));
            what = conv == 0 ? "muse_yuv_up2" : conv == 1 ? "muse_yuv_1x" : "muse_yuv_up32";
            if (conv == 0) {
                muse_yuv_up2(pc.y, pc.u, pc.v, W, W / 2, W, rows, dst, OS);
                muse_yuv_up2(tall.y, tall.u, tall.v, W, W / 2, W, (rows + 3) & ~3, ref, OS);
            } else if (conv == 1) {
                muse_yuv_1x(pc.y, pc.u, pc.v, W, W / 2, W, rows, dst, OS);
                muse_yuv_1x(tall.y, tall.u, tall.v, W, W / 2, W, (rows + 1) & ~1, ref, OS);
            } else {
                muse_yuv_up32(pc.y, pc.u, pc.v, W, W / 2, W, src_rows, 0, rows, dst, OS);
                muse_yuv_up32(tall.y, tall.u, tall.v, W, W / 2, W, TALL, 0, (rows + 1) & ~1, ref, OS);
            }
            for (int i = 0; i < PRE; i++) {
                if (out[i] != FENCE) {
                    fail("%d rows: byte %d before them written", rows, PRE - i);
                    break;
                }
            }
            for (size_t i = (size_t)rows * OS; i < 16 * OS + GUARD; i++) {
                if (dst[i] != FENCE) {
                    fail("%d rows: %d bytes past them written", rows, (int)(i - (size_t)rows * OS) + 1);
                    break;
                }
            }
            for (int r = 0; r < rows; r++) {
                if (memcmp(dst + (size_t)r * OS, ref + (size_t)r * OS, row_bytes) != 0) {
                    fail("%d rows: row %d not the picture's", rows, r);
                    break;
                }
            }
            free(pc.buf);
        }
    }
    free(tall.buf);
    printf("converters: %s\n", fails ? "FAIL" : "ok");
    return fails ? 1 : 0;
}
"""


class VideoDrawingTest(unittest.TestCase):
    """The colour converters on any row count: every row written exactly,
    nothing after it, no source row read past the ones those rows come from
    (BandSeams draws fill() itself, in the panel driver's pieces)."""

    def test_converters_read_and_write_only_their_rows(self):
        cc = shlex.split(os.environ.get("CC", "cc"))
        tests = ROOT / "tests"
        inc = ["-I", str(tests / "video_seams_fakes"), "-I", str(tests / "video_slots/fake"), "-I", str(MUSE),
               "-I", str(ROOT / "components/muse_vstream/include")]
        with tempfile.TemporaryDirectory() as tmp:
            src, exe = Path(tmp, "conv.c"), Path(tmp, "conv")
            src.write_text(CONVERTERS_HARNESS)
            build = subprocess.run(
                [*cc, "-std=gnu11", "-O1", "-g", "-Wall", "-Werror", "-Wno-unused-function",
                 "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer", *inc,
                 "-o", str(exe), str(src), str(MUSE / "muse_yuv.c"), str(MUSE / "muse_video_fit.c"), "-lm"],
                capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=120,
                                 env=dict(os.environ, ASAN_OPTIONS="detect_leaks=0"))
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertNotIn("runtime error", run.stderr)
            self.assertIn("converters: ok", run.stdout)


# muse_vdec on access units in turn, as muse_video.c's decode task feeds it: the
# caller holds the last picture (the show task may still be drawing it) and lets
# go of it first only when muse_vdec_may_realloc() says the next access unit may
# free it. Prints each unit's may_realloc and result; a held picture freed
# under its reader shows as an AddressSanitizer use-after-free, as does one
# that progress reported during the call (the show task draws it as it
# decodes) and the rest of the call freed. With -2 first,
# each unit is a two-band frame's access unit: it goes through the ring
# (muse_vstream) as from the network, then each band to a decoder of its own,
# with may_realloc asked of both bands as the decode task does.
REALLOC_HARNESS = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "muse_vdec.h"
#include "muse_vstream.h"
static uint8_t buf[1 << 20], ring[1 << 21], frame[8 + (1 << 20)];
static const uint8_t *held[2];
static size_t held_n[2];
static void hold(int k, int rc, const muse_pic_t *pic)
{
    if (rc == MUSE_VDEC_PICTURE) {
        held[k] = pic->y;
        held_n[k] = (size_t)pic->y_stride * pic->h;
    }
}
static void read_held(void)
{
    volatile unsigned sum = 0;
    for (int k = 0; k < 2; k++) {
        for (size_t i = 0; held[k] && i < held_n[k]; i++) sum += held[k][i];
    }
}
static const uint8_t *shown[2][4];
static size_t shown_n[2][4];
static int shown_k[2];
static void progress(void *ctx, const muse_pic_t *pic, int rows)
{
    int k = (int)(intptr_t)ctx, j = 0;
    (void)rows;
    while (j < shown_k[k] && shown[k][j] != pic->y) j++;
    if (j == 4) abort();
    shown[k][j] = pic->y;
    shown_n[k][j] = (size_t)pic->y_stride * pic->h;
    shown_k[k] += j == shown_k[k];
}
static void read_shown(void)
{
    volatile unsigned sum = 0;
    for (int k = 0; k < 2; k++) {
        for (int j = 0; j < shown_k[k]; j++) {
            for (size_t i = 0; i < shown_n[k][j]; i++) sum += shown[k][j][i];
        }
        shown_k[k] = 0;
    }
}
int main(int argc, char **argv)
{
    int bands = argc > 1 && !strcmp(argv[1], "-2");
    muse_vdec_t *d[2] = { muse_vdec_open(MUSE_VIDEO_CODEC_H264, 240, 240),
                          muse_vdec_open(MUSE_VIDEO_CODEC_H264, 240, 240) };
    if (!d[0] || !d[1]) return 2;
    for (int k = 0; k < 2; k++) muse_vdec_set_progress(d[k], progress, (void *)(intptr_t)k);
    muse_vstream_t vs;
    muse_vstream_init(&vs, ring, sizeof(ring), sizeof(buf));
    vs.bands = bands ? 2 : 1;
    muse_vstream_start(&vs);
    for (int i = 1 + bands; i < argc; i++) {
        FILE *f = fopen(argv[i], "rb");
        if (!f) return 2;
        uint32_t n = (uint32_t)fread(buf, 1, sizeof(buf), f);
        fclose(f);
        if (!bands) {
            int may = muse_vdec_may_realloc(d[0], buf, n);
            if (may) held[0] = NULL;
            muse_pic_t pic;
            int rc = muse_vdec_decode(d[0], buf, n, &pic);
            read_held();
            read_shown();
            printf("%d %d\n", may, rc);
            hold(0, rc, &pic);
            continue;
        }
        uint8_t h[8] = { MUSE_VIDEO_FRAME_MAGIC, 0, (uint8_t)i, 0, (uint8_t)n, (uint8_t)(n >> 8),
                         (uint8_t)(n >> 16), (uint8_t)(n >> 24) };
        memcpy(frame, h, 8);
        memcpy(frame + 8, buf, n);
        muse_vstream_write(&vs, frame, 8 + n);
        muse_vframe_t fr;
        int32_t got = muse_vstream_pop(&vs, buf, sizeof(buf), &fr);
        if (got != (int32_t)n) return 3;   /* the ring didn't take it whole */
        uint32_t len0 = buf[0] | buf[1] << 8 | buf[2] << 16 | (uint32_t)buf[3] << 24;
        if (len0 > n - 4) return 3;
        const uint8_t *au[2] = { buf + 4, buf + 4 + len0 };
        uint32_t len[2] = { len0, n - 4 - len0 };
        /* muse_video.c asks the top band's decoder about both bands. */
        int may = muse_vdec_may_realloc(d[0], au[0], len[0]) || muse_vdec_may_realloc(d[0], au[1], len[1]);
        if (may) held[0] = held[1] = NULL;
        muse_pic_t pic[2];
        int rc[2];
        for (int k = 0; k < 2; k++) rc[k] = muse_vdec_decode(d[k], au[k], len[k], &pic[k]);
        read_held();
        read_shown();
        printf("%d %d %d\n", may, rc[0], rc[1]);
        for (int k = 0; k < 2; k++) hold(k, rc[k], &pic[k]);
    }
    muse_vdec_close(d[0]);
    muse_vdec_close(d[1]);
    return 0;
}
'''


def two_bands(top, bottom):
    """A two-band frame's access unit: the top band's length, then both bands."""
    return len(top).to_bytes(4, "little") + top + bottom


class PicturesKeptTest(unittest.TestCase):
    """A picture muse_vdec handed out stays allocated until an access unit
    for which muse_vdec_may_realloc() said so: muse_video.c draws it while the
    next one decodes."""

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        tmp = cls.tmp.name
        Path(tmp, "sdkconfig.h").write_text("#pragma once\n")
        src, cls.exe = Path(tmp, "harness.c"), Path(tmp, "vdec")
        src.write_text(REALLOC_HARNESS)
        cc = shlex.split(os.environ.get("CC", "cc"))
        vstream = ROOT / "components/muse_vstream"
        san = sanitizers(cc, tmp) or ["-fsanitize=address"]
        build = subprocess.run([*cc, "-std=gnu11", "-O1", "-g", "-w", *san, f"-I{tmp}",
                                f"-I{TINY}", f"-I{MUSE}", f"-I{vstream / 'include'}",
                                "-o", str(cls.exe), str(src), str(MUSE / "muse_vdec.c"),
                                str(MUSE / "muse_vdec_same.c"), str(vstream / "muse_vstream.c"),
                                *[str(p) for p in sorted(TINY.glob("*.c"))]],
                               capture_output=True, text=True)
        if build.returncode:
            raise AssertionError(build.stdout + build.stderr)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def run_units(self, *units, bands=False):
        with tempfile.TemporaryDirectory() as tmp:
            files = []
            for i, unit in enumerate(units):
                f = Path(tmp, f"au{i}.264")
                f.write_bytes(unit)
                files.append(str(f))
            run = subprocess.run([str(self.exe), *(["-2"] if bands else []), *files], capture_output=True,
                                 text=True, timeout=60, env=dict(os.environ, ASAN_OPTIONS="detect_leaks=0"))
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            return [tuple(map(int, line.split())) for line in run.stdout.splitlines()]

    def test_an_idr_slice_activating_another_sequence_parameter_set(self):
        # Two sequence parameter sets up front; the second access unit is a
        # lone IDR slice that activates the other one (no parameter set in it).
        first = (sps(15, 15, sps_id=0) + sps(2, 2, sps_id=1) + pps(pps_id=0, sps_id=0) + pps(pps_id=1, sps_id=1)
                 + idr(225))
        lines = self.run_units(first, idr(4, pps_id=1, idr_pic_id=1), non_idr(4, pps_id=1))
        self.assertEqual([may for may, _ in lines], [1, 1, 0])
        # The first keyframe's picture locked the stream's size (VIDEO.md, Damage doesn't stick), and the lock
        # holds at activation too: the other set, stored before the lock, is refused, and the slice after it.
        self.assertEqual([rc for _, rc in lines], [1, -1, -1])

    def test_parameter_sets_from_an_earlier_access_unit(self):
        # The second set comes in an access unit of its own, a picture of the
        # first set follows, then an IDR slice switches to the second set.
        lines = self.run_units(au(15, 15), sps(2, 2, sps_id=1) + pps(pps_id=1, sps_id=1), non_idr(225),
                               idr(4, pps_id=1, idr_pic_id=1))
        self.assertEqual([may for may, _ in lines], [1, 1, 0, 1])
        # The IDR slice for another size is refused here (a keyframe locks the
        # stream's size: VIDEO.md, Damage doesn't stick); the held picture
        # still stays allocated until may_realloc said so (ASan).
        self.assertEqual([rc for _, rc in lines][2:], [1, -1])

    def test_an_idr_slice_without_a_start_code(self):
        # tinyh264 takes a unit that doesn't start with a start code as one
        # NAL unit: an IDR slice that way is taken as one with a start code
        # is, so its switch to a set of another size stored before the lock
        # is refused the same way, and the slice after it.
        first = (sps(15, 15, sps_id=0) + sps(2, 2, sps_id=1) + pps(pps_id=0, sps_id=0) + pps(pps_id=1, sps_id=1)
                 + idr(225))
        bare = idr(4, pps_id=1, idr_pic_id=1)[4:]
        self.assertEqual(bare[0], 0x65)
        lines = self.run_units(first, bare, non_idr(4, pps_id=1)[4:])
        self.assertEqual(lines, [(1, 1), (1, -1), (0, -1)])
        # A unit that starts with another kind still counts the parameter
        # sets behind start codes in it, as before.
        self.assertEqual(self.run_units(b"\x61" + sps(2, 2, sps_id=1))[0][0], 1)

    def test_a_bottom_band_without_a_start_code(self):
        # Two bands from the network: the ring checks the top band's start
        # code only, so the bottom band can be a bare IDR slice naming a
        # smaller set sent in the keyframe. Its decoder's format locked at
        # the keyframe, so that switch is refused (and its next slice); the
        # top band decodes on.
        top_key = sps(15, 8, sps_id=0) + pps() + idr(120)
        bottom_key = (sps(15, 7, sps_id=0) + sps(2, 2, sps_id=1) + pps(pps_id=0, sps_id=0) + pps(pps_id=1, sps_id=1)
                      + idr(105))
        top = non_idr(120)
        bottom = idr(4, pps_id=1, idr_pic_id=1)[4:]
        lines = self.run_units(two_bands(top_key, bottom_key), two_bands(top, bottom),
                               two_bands(non_idr(120, frame_num=2), non_idr(4, pps_id=1)[4:]), bands=True)
        self.assertEqual(lines, [(1, 1, 1), (1, 1, -1), (0, 1, -1)])

    def test_a_slice_whose_header_fails_then_a_keyframe(self):
        # A slice header that fails after the picture started (an unknown
        # slice type) leaves a picture with no valid slice; the next access
        # unit conceals it, and tinyh264 had no picture to output for it
        # (h264bsdDecode dereferenced NULL). The keyframe after it decodes.
        b = Bits()
        b.ue(0)       # first_mb_in_slice
        b.ue(30)      # slice_type: none
        b.ue(0)       # pic_parameter_set_id
        b.u(4, 1)     # frame_num
        broken = nal(0x61, b)
        lines = self.run_units(au(2, 2), broken, au(2, 2), non_idr(4, frame_num=1))
        self.assertEqual([rc for _, rc in lines], [1, -1, 1, 1])
        # A picture after it waits for a keyframe: muse_vdec drops the
        # unfinished picture rather than conceal it, so the next one's
        # frame_num follows a gap (VIDEO.md, Damage doesn't stick).
        lines = self.run_units(au(2, 2), broken, non_idr(4, frame_num=2))
        self.assertEqual([rc for _, rc in lines], [1, -1, -1])

    def test_an_idr_slice_whose_header_fails_then_a_delimiter(self):
        # An IDR slice whose first_mb_in_slice is past the picture starts a
        # picture with no valid slice; an access unit delimiter ends it, and
        # concealment stores nothing to output for it. Nothing of it is
        # reported either: a keyframe in the same access unit may free it.
        b = Bits()
        b.ue(4)       # first_mb_in_slice: past a 2x2 picture
        b.ue(7)       # slice_type: I
        b.ue(0)       # pic_parameter_set_id
        b.u(4, 0)     # frame_num
        b.ue(1)       # idr_pic_id
        broken = nal(0x65, b)
        b = Bits()
        b.u(3, 7)     # primary_pic_type
        aud = nal(0x09, b)
        lines = self.run_units(au(2, 2), broken, aud, non_idr(4, frame_num=1))
        self.assertEqual(lines, [(1, 1), (1, -1), (0, -1), (0, 1)])
        # A keyframe of another size is refused: a keyframe locks the
        # stream's size (VIDEO.md, Damage doesn't stick).
        lines = self.run_units(au(2, 2), broken, aud + au(3, 3))
        self.assertEqual(lines, [(1, 1), (1, -1), (1, -1)])
        # In the bottom band, with a picture after the delimiter.
        lines = self.run_units(two_bands(au(2, 2), au(2, 2)), two_bands(non_idr(4, frame_num=1), broken),
                               two_bands(aud + non_idr(4, frame_num=2), aud + non_idr(4, frame_num=1)), bands=True)
        self.assertEqual(lines, [(1, 1, 1), (1, 1, -1), (0, 1, 1)])


class WifiPortGate(unittest.TestCase):
    """The Wi-Fi video port listens only as much as it must: starts refused in a row pause further starts (1 s, doubling to
    30 s; a start taken or the view opening ends it), and the port is
    announced over mDNS only while the Phone camera view is open
    (components/muse/muse_video_wifi_gate.c, tests/video_wifi_gate_harness.c)."""

    def test_gate(self):
        cc = shlex.split(os.environ.get("CC", "cc"))
        with tempfile.TemporaryDirectory() as tmp:
            exe = Path(tmp, "gate")
            r = subprocess.run([*cc, "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                                "-fsanitize=address,undefined", "-fno-sanitize-recover=all", f"-I{MUSE}",
                                str(ROOT / "tests/video_wifi_gate_harness.c"), str(MUSE / "muse_video_wifi_gate.c"),
                                "-o", str(exe)], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=60)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertIn("all ok", run.stdout)
            self.assertNotIn("FAIL", run.stdout)
            self.assertGreater(run.stdout.count("ok "), 30)


if __name__ == "__main__":
    unittest.main()
