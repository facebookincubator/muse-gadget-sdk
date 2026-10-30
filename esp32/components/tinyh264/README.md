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
(`CONFIG_MUSE_VIDEO`, `../../../protocols/VIDEO.md`). It's the Android
Open Source Project's software decoder (`h264bsd`, originally Hantro's) as
packaged by [udevbe/tinyh264](https://github.com/udevbe/tinyh264) at
`524a702`, Apache-2.0 (`LICENSE`); Espressif's `esp_h264` ships the same code
as a prebuilt library.

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
- Experimental, off by default: on the ESP32-S31
  (`CONFIG_MUSE_VIDEO_SIMD_MC`), the luma 6-tap
  positions (a, b, c, d, h, n) and their averages (e, g, p, r) for
  partitions 8 or 16 pixels wide, and the
  chroma of 8-wide partitions, run on its SIMD
  (`src/h264bsd_interp_pie_s31.S`): pixels in 16-bit lanes split by parity,
  so every step is lane-wise and the same arithmetic as the C. QEMU can't
  run it, so the first inter block checks it against the C on 1248 random
  blocks and keeps the C if any differs (logged on `tinyh264`;
  `h264bsdPieState()`). `tests/test_tinyh264_pie.py` checks a model of its
  lane operations against the C and runs the hooks and the check on the
  host; with the model in place of the assembly, 110 test streams decode
  byte for byte as with the C. It passes its check on the board but is
  slower there than the C (480 px in two bands: 40 ms a frame against 28),
  hence off.

On the S3 at 240 MHz a 232 px picture decodes in about 21 ms at 600 kb/s and
25 ms at 1000 (x264, no deblocking), with the code running from cache.
`CONFIG_MUSE_VIDEO_DECODER_IRAM` puts the per-macroblock code and its tables
in IRAM instead (`linker.lf`, about 44 KB at -O2; -O3 would more than double
it): on the S3 that comes out of the heap, which a board running Wi-Fi, BLE
and the UI can't spare, so it's off by default.
