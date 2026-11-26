<!--
Copyright (c) Meta Platforms, Inc. and affiliates.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
-->

# tinyh264 regression corpus

From fuzzing tinyh264 (10.2 M runs),
replayed by `tests/test_video_route.py` (TinyH264Regress) through
`muse_vdec.c` with one reused buffer, under ASan and UBSan.

Format: byte 0 the fuzzing harness's decoder mode (ignored here), then records of
`[u16 LE length][access unit]`. The frames come from a 224 x 224 x264 encode
of Big Buck Bunny (c) copyright 2008, Blender Foundation,
www.bigbuckbunny.org, licensed CC BY 3.0. `crash_plain_resume_oob.au` is the
fuzzer's crash input as it found it (its SPS says 192 x 160; none of its
access units decodes to a picture).

| File | What | Without the fix |
|---|---|---|
| `carry_cut.au` | IDR, P1, P2 cut to 300 B, the IDR again, P1 again | the second IDR returned P1's old picture, then P1 failed |
| `crash_plain_resume_oob.au` | the fuzzer's crasher | ASan global-buffer-overflow in `h264bsdExtractNalUnit` (a NAL unit resumed from an earlier buffer) |
| `fngap.au` | frames 0, 1, 2, 4, 5 (3 lost before the device) | 4 and 5 decoded against the wrong reference |
| `sei_aud_only.au` | IDR, an SEI + AUD access unit, P1 | |
| `ps_only.au` | IDR, an SPS + PPS access unit, P1 | an error: a false loss |
| `ub_vlc_shift_overflow.au` | a 32-bit Exp-Golomb code | UBSan: `1 << 31` in `h264bsd_vlc.c` |
| `ub_sps_dpb_div0.au` | pic_width_in_mbs_minus1 = 2^32 - 1 | refused already (the 512 macroblock limit) |
| `ub_sps_crop_overflow_w.au`, `_h.au` | crafted crop offsets | refused already, before the crop check |
| `ub_sps_crop_overflow_i32.au` | crop offsets of 2^31 - 1 on a 208 px SPS (ours) | UBSan: i32 overflow in the crop check |

The streams with deblocking on also showed UBSan's left shift of a negative
value in `h264bsd_deblocking.c` without the fix.
