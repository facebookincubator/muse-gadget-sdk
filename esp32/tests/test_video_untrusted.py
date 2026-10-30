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
any whose sizes would overflow, and muse_vdec refuses VP8 keyframes larger
than the stream's."""
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


def sps(width_mbs, height_mbs):
    """A baseline-profile sequence parameter set for width x height macroblocks."""
    b = Bits()
    b.u(8, 66)   # profile_idc: baseline
    b.u(8, 0)    # constraint flags
    b.u(8, 30)   # level_idc
    b.ue(0)      # seq_parameter_set_id
    b.ue(0)      # log2_max_frame_num_minus4
    b.ue(2)      # pic_order_cnt_type
    b.ue(1)      # num_ref_frames
    b.u(1, 0)    # gaps_in_frame_num_value_allowed_flag
    b.ue(width_mbs - 1)
    b.ue(height_mbs - 1)
    b.u(1, 1)    # frame_mbs_only_flag
    b.u(1, 1)    # direct_8x8_inference_flag
    b.u(1, 0)    # frame_cropping_flag
    b.u(1, 0)    # vui_parameters_present_flag
    return nal(0x67, b)


def pps(slice_groups=None):
    """slice_groups(b) writes num_slice_groups_minus1 and what follows it."""
    b = Bits()
    b.ue(0)      # pic_parameter_set_id
    b.ue(0)      # seq_parameter_set_id
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


def idr(mbs):
    """An IDR slice of mbs grey I_PCM macroblocks (mbs=0: the header alone)."""
    b = Bits()
    b.ue(0)      # first_mb_in_slice
    b.ue(7)      # slice_type: I (all slices)
    b.ue(0)      # pic_parameter_set_id
    b.u(4, 0)    # frame_num
    b.ue(0)      # idr_pic_id
    b.u(1, 0)    # no_output_of_prior_pics_flag
    b.u(1, 0)    # long_term_reference_flag
    b.ue(0)      # slice_qp_delta (se 0)
    b.ue(1)      # disable_deblocking_filter_idc
    for _ in range(mbs):
        b.ue(25)   # mb_type: I_PCM
        while len(b.bits) % 8:
            b.u(1, 0)
        for _ in range(384):
            b.u(8, 0x80)
    return nal(0x65, b)


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
        self.assertIn('muse_video_wire_error(m, sizeof(m), "start", "closed")', wifi)
        self.assertIn('close_client("frame too long")', wifi)
        self.assertIn('close_client("said nothing")', wifi)
        self.assertIn('close_client("not reading")', wifi)
        self.assertIn("turned away: a client is connected", wifi)
        # Turning one away leaves the talking client's parser where it was.
        self.assertIn("if (accept_client(lsock)) {\n                st = RX_TYPE;", wifi)
        video = (MUSE / "muse_video.c").read_text()
        self.assertIn("if (vis_w <= 0 || vis_h <= 0) {", video)
        kc = (MUSE / "Kconfig").read_text()
        opt = kc[kc.index("config MUSE_VIDEO_WIFI_START_ANYTIME"):]
        self.assertIn("default n", opt[:opt.index("help")])


if __name__ == "__main__":
    unittest.main()
