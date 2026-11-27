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

"""A Live call's video beside reply audio on Data, replayed through the
gadget's own code (tests/video_route_harness.c): mg_proto.c's routing, the
muse_vstream ring, muse_vgate.h (the decode task's keyframe gate) and
muse_vdec.c on the real tinyh264, every picture checked byte for byte against
the clean clip's decode.

The Watcher's barge-in case: at a push-to-talk barge-in the gadget ended the
reply (stop_streaming drop) while the app was mid-write. Its audio writes in
flight went into the video ring (Data moved to the video when the device ended
the stream) and broke the frame they landed in; after that, for 22 s, every
requested keyframe "decoded" in 2-4 ms and the next frame failed. The ring
resynced on the next real header at once; the decoder was the one stuck:
tinyh264 kept the broken picture started, and at the next access unit's first
NAL unit (the keyframe's SPS) concealed it and returned it as that frame's
picture, never decoding the keyframe. The next P frame then failed (a frame_num
gap), leaving a picture started again: a loop only two keyframes in a row
could break.

Vector: tests/vectors/h264/testsrc2_208.264, 32 frames of ffmpeg's testsrc2 at
208 x 208, 15 fps: x264 0.164 --profile baseline --qp 38 --keyint 8
--min-keyint 8 --scenecut 0 --no-deblock --ref 1 --frames 32 (SPS and PPS
before every IDR, as the Muse app sends them), its SEI removed.
"""

from __future__ import annotations

import json
import os
import shlex
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))   # its helpers, also when run from esp32/ as tests.<module>
from test_mg_ble import MG, PROTOCOLS, ROOT, SEAL_INCS, SEAL_SRCS, codec_build

MUSE = ROOT / "components/muse"
TINY = ROOT.parent / "xplat/tinyh264/src"
VSTREAM = ROOT / "components/muse_vstream"
CLIP = ROOT / "tests/vectors/h264/testsrc2_208.264"
REGRESS = ROOT / "tests/vectors/h264/regress"


def sanitizers(cc: list[str], tmp: Path) -> list[str]:
    """ASan and UBSan where the compiler has them: damaged input must not read
    or write out of bounds, or through NULL (unguarded, the 'sps' scenario
    does, in h264bsdDecode)."""
    flags = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all"]
    probe = tmp / "probe.c"
    probe.write_text("int main(void) { return 0; }\n")
    r = subprocess.run([*cc, *flags, str(probe), "-o", str(tmp / "probe")], capture_output=True)
    return flags if r.returncode == 0 else []


def build(tmp: Path) -> Path:
    cc = shlex.split(os.environ.get("CC", "cc"))
    srcs, defs, incs = codec_build(False)
    common = ["-std=c11", "-D_DEFAULT_SOURCE", "-O1", "-g", *sanitizers(cc, tmp), *[f"-D{d}" for d in defs]]
    for inc in [MG, MG / "include", VSTREAM / "include", PROTOCOLS, MUSE, TINY, *incs, *SEAL_INCS]:
        common += ["-I", str(inc)]
    objs = []
    # Vendored code as upstream ships it (tinyh264 keeps its own style).
    for i, src in enumerate([*srcs, *sorted(TINY.glob("*.c"))]):
        obj = tmp / f"v{i}_{src.stem}.o"
        r = subprocess.run([*cc, *common, "-w", "-c", str(src), "-o", str(obj)], text=True, capture_output=True)
        if r.returncode:
            raise AssertionError(r.stdout + r.stderr)
        objs.append(str(obj))
    exe = tmp / "harness"
    ours = [ROOT / "tests/video_route_harness.c", MG / "mg_proto.c", MG / "mg_linkpol.c", MG / "mg_session.c",
            MG / "mg_ui.c", MG / "mg_queue.c", MG / "mg_route.c", MG / "mg_codec.c", MG / "mg_play.c",
            MG / "mg_resample.c", VSTREAM / "muse_vstream.c", ROOT / "tests/mg_token_proof_ref.c",
            MUSE / "muse_vdec.c", MUSE / "muse_vdec_same.c", *SEAL_SRCS]
    r = subprocess.run([*cc, *common, "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
                        "-Wno-missing-field-initializers", "-Wno-unused-function", *map(str, ours), *objs, "-lm",
                        "-o", str(exe)], text=True, capture_output=True)
    if r.returncode:
        raise AssertionError(r.stdout + r.stderr)
    return exe


class VideoRoute(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.tmp = tempfile.TemporaryDirectory(prefix="video-route-")
        cls.exe = build(Path(cls.tmp.name))

    @classmethod
    def tearDownClass(cls) -> None:
        cls.tmp.cleanup()

    def run_scenario(self, name: str) -> tuple[dict, list[dict]]:
        r = subprocess.run([str(self.exe), str(CLIP), name, "-v"], text=True, capture_output=True, timeout=300)
        self.assertEqual(r.returncode, 0, r.stdout[-3000:] + r.stderr[-3000:])
        lines = [json.loads(x) for x in r.stdout.strip().splitlines()]
        return lines[-1], lines[:-1]

    def test_barge_in_keeps_the_video_whole(self) -> None:
        s, frames = self.run_scenario("barge")
        # The device ended the reply mid-write: what was in flight was
        # discarded, none of it reached the video ring.
        self.assertGreater(s["in_flight"], 0, s)
        self.assertGreaterEqual(s["discarded"], s["in_flight"], s)
        self.assertEqual(s["resyncs"], 0, s)
        # Every frame through the press, the video-only stretch and the next
        # reply's interleaving decoded into the clip's own picture.
        self.assertEqual(s["dec_errors"], 0, s)
        self.assertEqual(s["mismatched"], 0, s)
        self.assertEqual(s["dropped"], 0, s)
        self.assertEqual(s["matched"], s["rx"], s)
        self.assertGreater(s["matched_reply2"], 20, s)
        self.assertGreater(s["select_video_t"], s["press_t"], s)

    def test_damaged_frame_recovers_at_the_next_keyframe(self) -> None:
        # As with the old routing (or any damaged frame): the stray audio lands in the
        # video ring mid-frame.
        s, frames = self.run_scenario("damage")
        self.assertGreater(s["trouble_t"], 0, s)
        # Asked at once, and the first keyframe after it repairs the decoder:
        # its picture is the clip's, as is every one after.
        first_bad = next(f for f in frames if f["t"] >= s["press_t"] and f["what"] != "shown")
        self.assertTrue(any(f["asked"] for f in frames if f["t"] == first_bad["t"]), first_bad)
        self.assertEqual(s["keyframes_to_recover"], 1, s)
        self.assertEqual(s["mismatched"], 0, s)
        self.assertEqual(s["fast_keyframes"], 0, s)
        self.assertEqual(s["bad_after_recovery"], 0, s)
        self.assertLessEqual(s["recovered_t"] - s["trouble_t"], 1000, s)
        self.assertGreater(s["matched_reply2"], 20, s)
        # The ring found the next real header at once: frames after the
        # damaged one keep their numbers in order.
        later = [f["number"] for f in frames if f["t"] > s["trouble_t"] and f["len"] > 0]
        self.assertEqual(later, sorted(later), later)

    def test_damaged_parameter_sets_do_not_stick(self) -> None:
        # SPS and PPS only at the stream's start, as some apps send them.
        s, frames = self.run_scenario("sps")
        # Bytes that parse as an SPS and PPS of the stream's size but other
        # syntax, in a keyframe (where a change is taken): that keyframe fails
        # and asks at once, the kept parameter sets come back, and the next
        # keyframe (without SPS or PPS) decodes into the clip's picture.
        self.assertGreater(s["ps_keyframe"], 0, s)
        injected = next(f for f in frames if f["number"] == s["ps_keyframe"] and f["len"] > 0)
        self.assertEqual(injected["what"], "error", injected)
        self.assertTrue(injected["asked"], injected)
        self.assertEqual(s["dec_errors"], 1, s)
        self.assertEqual(s["keyframes_to_recover"], 1, s)
        self.assertEqual(s["mismatched"], 0, s)
        self.assertEqual(s["bad_after_recovery"], 0, s)
        self.assertGreater(s["matched_reply2"], 20, s)
        # The same in a P frame, or of another size (160 x 160): refused, the
        # frame decodes into its own picture.
        self.assertEqual(s["ps_same_size_p_result"], 1, s)
        self.assertEqual(s["ps_other_size_p_result"], 1, s)

    def test_client_ended_stream_hands_data_to_the_video(self) -> None:
        s, frames = self.run_scenario("client_end")
        self.assertEqual(s["resyncs"], 0, s)
        self.assertEqual(s["dec_errors"], 0, s)
        self.assertEqual(s["mismatched"], 0, s)
        self.assertEqual(s["matched"], s["rx"], s)
        self.assertEqual(s["discarded"], 0, s)

    def test_frame_lost_upstream_waits_for_a_keyframe(self) -> None:
        # Frame 12 never arrives (numbers 10, 11, 13), the bytes around it
        # intact: 13 isn't decoded against the missing reference, a keyframe
        # request goes at once, and the next keyframe repairs.
        s, frames = self.run_scenario("skip")
        numbers = [f["number"] for f in frames if f["len"] > 0]
        self.assertIn(11, numbers)
        self.assertNotIn(12, numbers[:20])
        f13 = next(f for f in frames if f["number"] == 13 and f["len"] > 0)
        self.assertEqual(f13["what"], "skipped", f13)
        self.assertEqual(s["after_skip_result"], -1, s)
        self.assertTrue(s["after_skip_asked"], s)
        self.assertEqual(s["keyframes_to_recover"], 1, s)
        self.assertEqual(s["mismatched"], 0, s)
        self.assertEqual(s["dec_errors"], 0, s)
        self.assertEqual(s["bad_after_recovery"], 0, s)
        self.assertEqual(s["resyncs"], 0, s)
        self.assertGreater(s["matched_reply2"], 20, s)
        # Until the keyframe, the frames after it wait.
        waiting = [f for f in frames if f["t"] >= f13["t"] and f["t"] < s["recovered_t"] and f["len"] > 0]
        self.assertTrue(all(f["what"] in ("skipped", "waiting") for f in waiting[:-1]), waiting)

    def test_frame_lost_before_numbering_is_caught_by_frame_num(self) -> None:
        # Frame 12's encoded picture is lost before the app numbers its
        # frames: the headers stay continuous and only the slice's frame_num
        # jumps. It's a decode error, never a smeared picture: asked at once,
        # repaired by the next keyframe.
        s, frames = self.run_scenario("lost")
        numbers = [f["number"] for f in frames if f["len"] > 0]
        self.assertEqual(numbers[:20], list(range(20)))
        f12 = next(f for f in frames if f["number"] == 12 and f["len"] > 0)
        self.assertEqual(f12["src"], 13, f12)
        self.assertEqual(f12["what"], "error", f12)
        self.assertTrue(f12["asked"], f12)
        self.assertEqual(s["dec_errors"], 1, s)
        self.assertEqual(s["keyframes_to_recover"], 1, s)
        self.assertEqual(s["mismatched"], 0, s)
        self.assertEqual(s["bad_after_recovery"], 0, s)
        self.assertGreater(s["matched_reply2"], 20, s)

    def test_access_units_without_a_picture(self) -> None:
        r = subprocess.run([str(self.exe), str(CLIP), "au"], text=True, capture_output=True, timeout=60)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        s = json.loads(r.stdout.strip().splitlines()[-1])
        picture, none, error = 1, 0, -1
        self.assertEqual(s["keyframe"], picture, s)
        self.assertEqual(s["sei_only"], error, s)      # an SEI alone: an error
        self.assertEqual(s["ps_only"], none, s)        # SPS and PPS alone: not one
        self.assertEqual(s["aud_only"], error, s)      # a delimiter alone: an error
        # and none of them disturbs the frames around them (the clip's pictures)
        self.assertEqual([s["p1"], s["p2"], s["p3"]], [picture] * 3, s)

    def test_too_far_behind_resumes_at_a_keyframe(self) -> None:
        # The C6 in a phone's reply: past LAG_RESYNC_MS of backlog a P frame isn't
        # decoded; the gate waits for a keyframe and asks for one at once, and the keyframe the app sends
        # brings back the clip's own picture with nothing wrong in between or after.
        s, frames = self.run_scenario("behind")
        self.assertGreaterEqual(s["behind_at"], 40, s)
        self.assertTrue(s["behind_asked"], s)
        self.assertEqual(s["dec_errors"], 0, s)
        self.assertEqual(s["mismatched"], 0, s)
        self.assertEqual(s["bad_after_recovery"], 0, s)
        self.assertEqual(s["keyframes_to_recover"], 1, s)
        self.assertLessEqual(s["recovered_t"] - s["trouble_t"], 1000, s)
        at = next(i for i, f in enumerate(frames) if f["what"] == "behind")
        back = next(i for i, f in enumerate(frames) if i > at and f["what"] == "shown")
        self.assertTrue(frames[back]["flags"] & 1, frames[back])   # resumed at a keyframe
        self.assertTrue(all(f["what"] == "waiting" for f in frames[at + 1:back] if f["len"] > 0), frames[at:back + 1])


class TinyH264Regress(unittest.TestCase):
    """tinyh264's regression corpus (tests/vectors/h264/regress/README.md:
    from 10.2 M runs of a tinyh264 fuzzer)
    through muse_vdec.c with one reused buffer, as the video task decodes,
    under ASan and UBSan with no recovery: any report fails the run."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.tmp = tempfile.TemporaryDirectory(prefix="h264-regress-")
        tmp = Path(cls.tmp.name)
        cc = shlex.split(os.environ.get("CC", "cc"))
        exe = tmp / "regress"
        r = subprocess.run([*cc, "-std=c11", "-D_DEFAULT_SOURCE", "-O1", "-g", *sanitizers(cc, tmp), "-w", "-I", str(MUSE), "-I", str(TINY),
                            "-I", str(VSTREAM / "include"), str(ROOT / "tests/h264_regress_harness.c"),
                            str(MUSE / "muse_vdec.c"), str(MUSE / "muse_vdec_same.c"),
                            *map(str, sorted(TINY.glob("*.c"))), "-lm", "-o", str(exe)],
                           text=True, capture_output=True)
        if r.returncode:
            raise AssertionError(r.stdout + r.stderr)
        files = sorted(REGRESS.glob("*.au"))
        r = subprocess.run([str(exe), *map(str, files)], text=True, capture_output=True, timeout=300)
        if r.returncode:
            raise AssertionError(r.stdout[-2000:] + r.stderr[-4000:])
        cls.res = {x["file"]: x["aus"] for x in map(json.loads, r.stdout.strip().splitlines())}
        cls.stderr = r.stderr

    @classmethod
    def tearDownClass(cls) -> None:
        cls.tmp.cleanup()

    def test_no_sanitizer_report(self) -> None:
        self.assertEqual(len(self.res), len(list(REGRESS.glob("*.au"))))
        self.assertNotIn("runtime error", self.stderr)
        self.assertNotIn("AddressSanitizer", self.stderr)

    def test_a_cut_frame_leaves_nothing_behind(self) -> None:
        # IDR, P1, P2 cut to 300 B, the IDR again, P1 again: the cut frame is
        # an error, and the second IDR and P1 decode exactly as the first.
        a = self.res["carry_cut.au"]
        self.assertEqual([x[0] for x in a], [1, 1, -1, 1, 1], a)
        self.assertEqual(a[3], a[0], a)
        self.assertEqual(a[4], a[1], a)

    def test_resumed_nal_from_an_earlier_buffer(self) -> None:
        # The fuzzer's crasher (ASan global-buffer-overflow in
        # h264bsdExtractNalUnit): errors, nothing read past.
        a = self.res["crash_plain_resume_oob.au"]
        self.assertTrue(all(x[0] == -1 for x in a), a)

    def test_frame_num_gap_is_an_error(self) -> None:
        # Frames 0, 1, 2, 4, 5 (3 lost before the device): 4 and 5 are
        # errors, no picture against the wrong reference.
        a = self.res["fngap.au"]
        self.assertEqual([x[0] for x in a], [1, 1, 1, -1, -1], a)

    def test_access_units_without_a_slice(self) -> None:
        sei, ps = self.res["sei_aud_only.au"], self.res["ps_only.au"]
        self.assertEqual([x[0] for x in sei], [1, -1, 1], sei)   # SEI + AUD alone: an error
        self.assertEqual([x[0] for x in ps], [1, 0, 1], ps)      # SPS + PPS alone: not one
        self.assertEqual(sei[2], ps[2])                          # P1 decodes the same after either
        self.assertEqual(ps[2], self.res["carry_cut.au"][1])

    def test_hostile_parameter_sets_are_refused(self) -> None:
        for name in ("ub_vlc_shift_overflow.au", "ub_sps_dpb_div0.au", "ub_sps_crop_overflow_w.au",
                     "ub_sps_crop_overflow_h.au", "ub_sps_crop_overflow_i32.au"):
            with self.subTest(name=name):
                self.assertTrue(all(x[0] in (-1, 0) and x[1] == 0 for x in self.res[name]), self.res[name])


if __name__ == "__main__":
    unittest.main()
