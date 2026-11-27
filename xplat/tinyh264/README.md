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

# tinyh264

The H.264 constrained baseline decoder behind Muse video streaming
(`CONFIG_MUSE_VIDEO`, `../../protocols/VIDEO.md`). It's the Android
Open Source Project's software decoder (`h264bsd`, originally Hantro's) as
packaged by [udevbe/tinyh264](https://github.com/udevbe/tinyh264) at
`524a702`, Apache-2.0 (`LICENSE`); Espressif's `esp_h264` ships the same code
as a prebuilt library. The ESP32 firmware builds it with its ESP-IDF
component, `../../esp32/components/tinyh264` (`CMakeLists.txt`, `linker.lf`).

Changes from upstream, each marked `OPT:` in the source, all bit-exact (the
decoded pictures match upstream and ffmpeg byte for byte on 406 test
streams, among them QP 4 intra-only, four slices, four reference frames,
deblocking off and 2 Mb/s VGA):

- The bit reader (`h264bsdShowBits32`, `h264bsdFlushBits`,
  `h264bsdGetBits`) is inlined into its callers.
- A macroblock's 1.6 KB of coefficients are cleared only for the blocks its
  coded block pattern says will be read.
- The start code search skips four bytes at a time while none can start one.
- Deblocking runs macroblock by macroblock, as soon as the last macroblock
  that predicts from a block unfiltered has been decoded, instead of in a
  pass over the whole picture: the pixels are still in the data cache, which
  saves 15-37% of the PSRAM traffic at 240-412 px. Out-of-order slices (or a
  damaged stream) fall back to filtering the rest of the picture at its end.
- Error recovery, which clean streams never reach (their pictures are
  unchanged): `h264bsdPictureStarted` / `h264bsdAbandonPicture` let a
  caller that hands over whole access units drop a picture an error left
  unfinished, instead of upstream's concealing it at the next access unit's
  first NAL unit and returning it for that one (whose slices then went
  undecoded: after one damaged frame, every keyframe "decoded" into the old
  picture and the next frame failed). `h264bsdKeepParamSets` /
  `h264bsdRestoreParamSets` keep the parameter sets of the last IDR picture
  and lock the stream's size and profile (other sequence parameter sets are
  refused in `h264bsdStoreSeqParamSet`; one that changes the kept one, SPS
  or PPS, only while `h264bsdAllowParamSetChange`, a keyframe's access
  unit), and put them back after an error. `h264bsdDecode` returns an
  error, not a NULL dereference, for a picture that reached no output, and
  stops at the buffer's end with `H264BSD_RDY` (parameter sets alone)
  instead of failing on an empty buffer. With `gaps_in_frame_num_allowed`
  0, a gap in frame_num (a lost reference frame) is an error
  (`h264bsdCheckGapsInFrameNum`) where upstream decoded on regardless.
  `muse_vdec.c` uses all of it; `../../esp32/tests/test_video_route.py` checks it.
- Undefined behavior out (found by fuzzing tinyh264; the
  same results): the Exp-Golomb decoder's `1 << 31` is unsigned, the
  crop check sums in 64 bits, deblocking multiplies `q0 - p0` by 4 instead
  of shifting a negative value, and `h264bsdDecode` never steps past its
  buffer. `../../esp32/tests/test_video_route.py` (TinyH264Regress) replays the corpus
  in `../../esp32/tests/vectors/h264/regress` under ASan and UBSan.
- Frame cropping: `h264bsdCroppingOffsets`
  hands the active sequence parameter set's four frame_crop offsets to the
  caller, which shows that window of the coded picture (`muse_vdec.c`);
  `h264bsdSetMaxCropped` refuses a sequence parameter set whose picture
  after cropping is larger than the stream's, beside `h264bsdSetMaxSize`'s
  bound on the coded size, which may be larger by an encoder's padding
  (iPhone streams code at least 192 x 96). Decoding itself is unchanged.
- Experimental, off by default: on the ESP32-S31
  (`CONFIG_MUSE_VIDEO_SIMD_MC`), the luma 6-tap
  positions (a, b, c, d, h, n) and their averages (e, g, p, r) for
  partitions 8 or 16 pixels wide, and the
  chroma of 8-wide partitions, run on its SIMD
  (`src/h264bsd_interp_pie_s31.S`): pixels in 16-bit lanes split by parity,
  so every step is lane-wise and the same arithmetic as the C. QEMU can't
  run it, so the first inter block checks it against the C on 1248 random
  blocks and keeps the C if any differs (logged on `tinyh264`;
  `h264bsdPieState()`). `../../esp32/tests/test_tinyh264_pie.py` checks a model of its
  lane operations against the C and runs the hooks and the check on the
  host; with the model in place of the assembly, 110 test streams decode
  byte for byte as with the C. It passes its check on the board but is
  slower there than the C (480 px in two bands: 40 ms a frame against 28),
  hence off.

One `OPT:` change is a behaviour change, for input from the network:
`h264bsdDecode` takes one whole access unit per call and starts each call
fresh. Upstream resumed a NAL unit an earlier call left unfinished whenever
it got the same buffer address again; `muse_vdec.c` reuses one buffer, so by
then that address holds another frame. Such a NAL unit is now dropped, a
resumed one must fit the buffer it's resumed in, and no step reads past the
input. Resuming within one call, after new parameter sets, works as before.
A picture that concealment finishes without a single valid slice has
nothing to output: it isn't reported as decoding progress either, and the
call goes on with the rest of the access unit instead of returning a
picture that isn't there.

On the S3 at 240 MHz a 232 px picture decodes in about 21 ms at 600 kb/s and
25 ms at 1000 (x264, no deblocking), with the code running from cache.
`CONFIG_MUSE_VIDEO_DECODER_IRAM` puts the per-macroblock code and its tables
in IRAM instead (the component's `linker.lf`, about 44 KB at -O2; -O3 would more than double
it): on the S3 that comes out of the heap, which a board running Wi-Fi, BLE
and the UI can't spare, so it's off by default.
