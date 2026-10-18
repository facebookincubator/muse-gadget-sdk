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

# libvpx_vp8

The decoder behind video streaming's second codec (`CONFIG_MUSE_VIDEO_VP8`,
`../../../protocols/VIDEO.md`): the VP8
decoder of [libvpx](https://chromium.googlesource.com/webm/libvpx) v1.16.0,
trimmed to what decoding needs, C only. libvpx is under the BSD-3-Clause
license (`LICENSE`) with the WebM project's additional patent grant
(`PATENTS`); `AUTHORS` lists its authors. `src/` keeps libvpx's layout, and
its generated headers (`vpx_config.h`, `*_rtcd.h`) are libvpx's generic C
configuration (`--target=generic-gnu --disable-vp9 --disable-vp8-encoder
--disable-multithread --disable-postproc`).

Changes from upstream, each marked `Muse gadget port` in the source. All but
the progress hook are speed-ups, bit-exact: the decoded pictures match stock
libvpx byte for byte on 933 test streams (profiles 0 to 3, loop filter
normal, simple and off, one and two token partitions, alt-ref frames,
segmentation, 240 to 480 px).

- Sub-pixel prediction (`vp8/common/filter.c`): a whole-pixel component is
  the identity filter, so a block with one fractional component takes one
  pass instead of two; the 6-tap 2-D case keeps its first pass in bytes;
  the odd 6-tap positions (outer taps zero) use four taps; bilinear works
  two pixels per 32-bit multiply.
- Loop filters (`vp8/common/loopfilter_filters.c`): in int arithmetic, a
  pixel at a time, leaving early when its mask is off or the filter value
  rounds to no change.
- Inverse transform (`vp8/common/idct_blk.c`): a DC-only block whose DC
  rounds to zero is skipped.
- The bool decoder is always inlined and normalises with a leading-zero
  count instead of a table (`vp8/decoder/dboolhuff.h`).
- With segmentation, the dequantisation tables are rebuilt only when a
  macroblock's Q index changes (`vp8/decoder/decodeframe.c`).
- On the ESP32-S3, bilinear prediction of 16x16 and 8x8 blocks runs on its
  SIMD (`src/vp8/common/xtensa/bilinear_pie.S`, `MGX_PIE_S3`): 16-bit lanes,
  the same arithmetic as the C; checked against the C on 4000 random blocks
  in Espressif's QEMU (esp32s3). About 2 ms a frame less at 232 px.
- Bench builds (`CONFIG_MUSE_VIDEO_VP8_PROF`, `MGX_PROF`) time each stage
  with the cycle counter; `mg.video` logs them.
- Progress (`vp8dx_mgx_set_progress()`, `vpx/vp8dx.h`): the decoder reports
  the luma rows of the shown picture that are final as it goes, so the panel
  draws the top while the bottom decodes (`components/muse/muse_vdec.c`).

Cost, from instruction counts in QEMU (the S31's base ISA), per frame at
600 kb/s, 480 px (crew, soccer, city, ice): 6.0 M with the phone's settings
(version 1, loop filter off, decoder-friendly motion vectors), 7.2 M
without the motion vector restriction; tinyh264 takes 7.5 M on the same
clips at the same rate. At 240 px and 250 kb/s: 1.8 M against 2.7 M. The
per-macroblock code can run from IRAM (`linker.lf`, about 28 KB on the S3,
with `CONFIG_MUSE_VIDEO_DECODER_IRAM`).
Memory: four frame buffers with 32-pixel borders, in PSRAM (1.8 MB at 480
px, 0.55 MB at 240).

On the Waveshare ESP32-S3-Touch-AMOLED-1.75C (232 px, two times upscaled,
350 kb/s, Muse Gadgets Camera's live path on four xiph.org test clips, decoder in
IRAM): 13-15 ms a frame against tinyh264's 13-20 ms. Per stage
(city): tokens 3.0 ms, prediction 6.5 (mostly reference reads from PSRAM),
residual 3.1, border extension 0.9, modes 1.2.
