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
  two pixels per 32-bit multiply. Both filters slide a window along the row
  or down the column (each source pixel loaded once), and the 6-tap filter
  has one out-of-line copy per block width with its inner loop unrolled.
  In the 2-D 6-tap case an odd vertical filter (four taps) needs two rows
  fewer from the first pass.
- Loop filters (`vp8/common/loopfilter_filters.c`): in int arithmetic, a
  pixel at a time, leaving early when its mask is off or the filter value
  rounds to no change.
- Inverse transform (`vp8/common/idct_blk.c`): a DC-only block whose DC
  rounds to zero is skipped. Clamps to 0..255 here and in the loop filter
  take one rarely taken branch (a forward taken branch costs the C6 four
  cycles) where the core has no min/max instructions.
- The bool decoder is always inlined and normalises with a leading-zero
  count instead of a table (`vp8/decoder/dboolhuff.h`), except on RISC-V
  cores without Zbb (the C6), where `__builtin_clz` is a libgcc call: the
  table there.
- Small per-row and per-block fills and copies (borders, entropy contexts,
  full-pixel predictions, coefficient clears) are inline word or byte
  stores instead of memset/memcpy calls (`vp8/common/mgx_mem.h`).
- With segmentation, the dequantisation tables are rebuilt only when a
  macroblock's Q index changes (`vp8/decoder/decodeframe.c`).
- On the ESP32-S3, bilinear prediction of 16x16 and 8x8 blocks runs on its
  SIMD (`src/vp8/common/xtensa/bilinear_pie.S`, `MGX_PIE_S3`): 16-bit lanes,
  the same arithmetic as the C; checked against the C on 4000 random blocks
  in Espressif's QEMU (esp32s3). About 2 ms a frame less at 232 px.
- Memory (`vp8/common/alloccommon.c`, `vp8/common/reconinter.c`,
  `vpx_scale/`): pictures are allocated when a frame first needs one, so a
  stream holds only as many as its references use (libvpx allocates four up
  front, plus a scratch picture the decoder never uses). Their border is 8
  pixels (16 on the S3 and the S31, whose SIMD code wants 16-byte aligned
  rows) instead of 32: a reference block that reaches past it is copied
  through an edge-emulation buffer with its coordinates clamped into the
  picture, which gives the pixels the wider border would have held. The
  buffer is in each decoder's state, as two decoders run at once (a
  picture's two bands, one per core). A stride that would be a multiple of
  256 bytes gets 32 more, so a block's rows don't share cache sets (480 px:
  544, as upstream).
- Low-memory decoding (`vp8dx_mgx_set_lowmem()`, `vpx/vp8dx.h`;
  `vp8/decoder/decodeframe.c`), opt-in: an inter frame that replaces the
  last frame, and leaves no reference on its old content, is decoded over it
  through a ring of a few macroblock rows (3 is enough for the phone's
  streams). A row goes from the ring to the picture once it is final and no
  row still to decode reads the old picture at or above it; VP8 parses a
  frame's modes and motion vectors before its residual, so the decoder knows
  that before the first row. The few macroblocks that read further up than
  the ring can wait for (at most 6 a frame on our clips) are predicted
  first into a stash of 8 (3 KB); a frame that needs more gets a picture of
  its own, as before. A stream whose golden frame is its last frame then
  needs one picture and the ring instead of two, otherwise two instead of
  three: at 180 px, 157 KB -> 111 KB and 222 KB -> 176 KB. The shown
  picture is valid only until the next decode starts, which the caller must
  allow for (muse_vdec does not turn it on).
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
per-macroblock code can run from IRAM (`linker.lf`, `CONFIG_MUSE_VP8_IRAM`):
full (every per-macroblock object with the unrolled filters, about 45 KB on
the S31), compact (the default with `CONFIG_MUSE_VIDEO_DECODER_IRAM`, about
33 KB: the same objects compiled without loop unrolling or the 6-tap
filter's per-width copies, `VP8_MGX_COMPACT`, bit-exact, and the low-memory
mode's code left in flash), hot (tokens, inverse transforms, inter
prediction and the bilinear filters, the bulk of the work for phone-style
streams; about 12 KB) or none.
All of the decoder's memory comes from PSRAM where the board has some, else
from DMA-capable internal RAM (`vpx_mem/vpx_mem.c`): plain malloc() gave
small blocks such as the entropy contexts internal RAM, and LP RAM once that
ran low, which is many times slower.
Memory: one picture per reference the stream holds (two or three), with
8-pixel borders (16 on the S3 and S31): 65 KB each at 180 px, 98 KB at 240,
369 KB at 480 (libvpx's four with 32-pixel borders took 450 KB at 180 px,
1.8 MB at 480).

On the Waveshare ESP32-S3-Touch-AMOLED-1.75C (232 px, two times upscaled,
350 kb/s, Muse Gadgets Test's live path on four xiph.org test clips, decoder in
IRAM): 13-15 ms a frame against tinyh264's 13-20 ms. Per stage
(city): tokens 3.0 ms, prediction 6.5 (mostly reference reads from PSRAM),
residual 3.1, border extension 0.9, modes 1.2.
