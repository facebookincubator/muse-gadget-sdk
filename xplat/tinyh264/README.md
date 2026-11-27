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
deblocking off and 2 Mb/s VGA; the zero-motion skip copy and the parsing
and boundary strength changes give the same pictures as the decoder before
them on the 256-stream corpus of "Checking a change" below):

- The bit reader (`h264bsdShowBits32`, `h264bsdFlushBits`,
  `h264bsdGetBits`) is inlined into its callers.
- A macroblock's 1.6 KB of coefficients are cleared only for the blocks its
  coded block pattern says will be read.
- The start code search skips four bytes at a time while none can start one.
- A P_Skip macroblock with a zero motion vector is copied straight from the
  reference picture into the picture, instead of being predicted into a
  scratch block and written out from there.
- Parsing: nC and the intra 4x4 neighbours are found once per macroblock
  instead of per block; CAVLC places the runs and levels in one pass and
  keeps its 32-bit stream cache across a macroblock's blocks; with
  `H264BSD_CLZ`, leading zeros are counted with `__builtin_clz` and
  Exp-Golomb codes of up to 31 bits are decoded inline (on unless the
  target is known to lack an instruction for it: RISC-V without Zbb, Arm
  without CLZ; a port whose core has none sets it to 0, or `__builtin_clz`
  becomes a library call); the bit reader loads a byte-swapped word where
  unaligned loads are cheap on a little-endian core (`H264BSD_WORD_LOADS`,
  auto; a port can set it to 0, or to 1 on a little-endian core only: a
  big-endian one is refused at compile time).
- Deblocking: the boundary strengths test a 16-bit mask of the luma blocks
  with coefficients (`nzMask`), compare two single-vector macroblocks' motion
  once, and are stored as bytes.
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
- Off by default, for RISC-V cores with Xpulp (`TINYH264_XPULP=1`):
  `ABS`, `CLIP3`, `CLIP1` and the clip table lookups (`H264BSD_CLP`: the
  6-tap interpolation, deblocking, intra prediction and the residual add)
  become abs, min/max and clipu from `xplat/include/rv_xpulp.h` (build with
  `-DRV_XPULP=1` and `xplat/include` on the include path; without
  `RV_XPULP=1` that header's plain-C twins run). Same pictures, byte for
  byte. Experimental with `RV_XPULP=1`: the instructions have run on QEMU,
  not on hardware. Porting notes: `xplat/PORTING-PERF.md`.

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

A residual block whose total_zeros would put a coefficient past a
15-coefficient AC block is a decode error too; upstream wrote that
coefficient into the next block. No valid stream has one, so only damaged
streams decode differently.

On the S3 at 240 MHz a 232 px picture decodes in about 21 ms at 600 kb/s and
25 ms at 1000 (x264, no deblocking), with the code running from cache.
`CONFIG_MUSE_VIDEO_DECODER_IRAM` puts the per-macroblock code and its tables
in IRAM instead (the component's `linker.lf`, about 44 KB at -O2; -O3 would more than double
it): on the S3 that comes out of the heap, which a board running Wi-Fi, BLE
and the UI can't spare, so it's off by default.

## Checking a change

A change that claims to be bit-exact decodes a stream corpus to the same
pictures as the decoder before it. `../tests/h264_corpus.py` makes a corpus
with ffmpeg and libx264 (`gen`: 96 to 480 px, intra-only and I/P, 1 to 4
references, deblocking on, off and offset, several slices, low and high QP,
damaged copies), decodes it to one MD5 per picture with `../tests/h264_tool.c`
(`hash`, optionally under ASan and UBSan or with extra `-D` flags) and
compares two runs (`compare`):

```sh
mkdir -p ../h264-check/base && git archive BASE xplat/tinyh264/src | tar -x -C ../h264-check/base
python3 xplat/tests/h264_corpus.py gen ../h264-check/corpus
python3 xplat/tests/h264_corpus.py hash --src ../h264-check/base/xplat/tinyh264/src -o base.txt ../h264-check/corpus esp32/tests/vectors/h264
python3 xplat/tests/h264_corpus.py hash --sanitize -o new.txt ../h264-check/corpus esp32/tests/vectors/h264
python3 xplat/tests/h264_corpus.py compare base.txt new.txt
```

For a base from before the decoder moved to `xplat/`, its sources are in
`esp32/components/tinyh264/src`: archive that path and pass
`--src ../h264-check/base/esp32/components/tinyh264/src`.

`hash --crop` and `ffmpeg` give lines to compare with ffmpeg's own decode of
the undamaged streams.

## Knobs for other platforms

Compile-time switches for ports to other MCUs, each off by default: off, the
code is the same as without it. Define them for the decoder's sources and for
the code that calls their functions. `esp32/tests/test_tinyh264_knobs.py` builds
each one in and compares it with the decoder as it ships, picture for picture
(set `TINYH264_CORPUS` to a directory of `.264` streams to run more), and
`esp32/tests/test_tinyh264_knob_builds.py` builds the portable ones (the
packed operations on their C model, the DSP stand-ins, the cold and hot
switches) and compares the vectors' pictures, under ASan and UBSan where
the host has them. `TINYH264_MVE` and the DSP instructions are experimental:
they have run on QEMU's Cortex-M55 and M33, by hand and not on hardware; CI
only compiles them (Arm GCC, for a Cortex-M55 and a Cortex-M33) and runs the
DSP's plain-C stand-ins. The memory and API
switches from `TINYH264_CUSTOM_ALLOC` down are checked on the host only (those
tests and a corpus of 256 streams); no board here builds them in, so they are
experimental until a port runs them.

- `TINYH264_MVE=1` (experimental): Arm Helium (MVE), with the ACLE intrinsics
  of `arm_mve.h`, where the compiler targets it (`__ARM_FEATURE_MVE`:
  Cortex-M55, M85); elsewhere the C. All of motion compensation
  (`src/h264bsd_inter_mve.c`: every partition and position, predicted straight
  into the picture and the residual added there), the general inverse
  transform, deblocking a whole edge at a time, the intra residual add and the
  macroblock writes. The first call checks the Helium motion compensation
  against the C on random blocks and keeps the C if they differ
  (`h264bsdPieState()`; `TINYH264_LOG`, a printf-style macro, reports it); the
  transform, deblocking and residual paths have no such check. On QEMU's
  Cortex-M55, 40 % fewer instructions per picture; the same pictures, byte for
  byte. Where it builds Helium, not with `H264BSD_LOWMEM` or `H264BSD_STRIP`
  yet.
- `TINYH264_DSP_MC=1` (experimental): the luma 6-tap and chroma interpolation
  on the Armv7E-M / Armv8-M DSP extension (`src/h264bsd_interp_dsp.c`, under
  `__ARM_FEATURE_DSP`; plain-C stand-ins with the same results elsewhere), two
  pixels to a register, behind the same hooks and first-use check as the S31's
  SIMD. `TINYH264_TLS=__thread` where several threads decode.
- `TINYH264_PACKED4_MC=1`: every interpolation function in packed 4-lane
  operations (`src/h264bsd_interp_packed4.c`): 4 x 8-bit multiplies into
  4 x 16-bit lanes, a rounding shift and saturating pack, funnel shifts. The
  operations come from `src/h264bsd_packed4_ops.h`, their plain-C model, or
  from the port's own header with the same names and results
  (`TINYH264_PACKED4_OPS_H='"ops.h"'`); `h264bsdPacked4Check()`
  (`src/h264bsd_reconstruct.h`) runs every function against the C on random
  blocks, for checking the port's operations on the target: there is no
  first-use check. Little-endian targets only (a big-endian one is refused at
  compile time). One motion-compensation switch at a time (0 is off).
- `H264BSD_COLD_LEVEL=1` or `2`: small or rare functions (`H264BSD_COLD_FN`,
  some of which run per macroblock: the DC transforms, motion vector
  prediction from the neighbours; at 2 also the once-per-slice and
  once-per-picture ones, `H264BSD_COLD_FN2`) compiled for size with GCC's
  `optimize("Os")`, the rest at the build's level: on a Cortex-M33 at -O2,
  78668 B of code at 0, 78480 at 1, 77288 at 2. GCC only (other compilers
  ignore it), and GCC documents the attribute as a debugging aid: check that
  the port's own flags (stack protection, say) hold in those functions, or
  compile `src/h264bsd_slice_header.c` and `src/h264bsd_dpb.c` at -Os per file
  instead.
- `H264BSD_HOT_SECTION='".itcm.h264"'`, `H264BSD_HOT_DATA_SECTION='".dtcm.h264"'`:
  the per-macroblock code (`H264BSD_HOT`) and the tables it reads
  (`H264BSD_HOT_DATA`) in the port's own sections, for its linker script to
  place in fast RAM (about 36-41 KB of code and 5 KB of tables on a
  Cortex-M); `xplat/PORTING-PERF.md`. The ESP-IDF component uses `linker.lf`.
- `TINYH264_CUSTOM_ALLOC=1`: every block comes from the port's allocator,
  `tinyh264_alloc`, `tinyh264_alloc_hot` (the blocks read for every
  macroblock, where faster memory pays) and `tinyh264_free`, which the port
  defines; a fixed arena, say, so the decoder never touches a system heap.
  Blocks are aligned as `malloc`'s are.
- `TINYH264_PIC_ALIGN=N`: picture buffers aligned to N bytes (a power of two,
  at least 16, checked at compile time; 16 by default), for a GPU or DMA
  engine that reads the planes in place.
- `H264BSD_LIMITS=1`: `h264bsdSetLimits(dec, maxMbs, maxRefFrames)` refuses a
  sequence parameter set for more macroblocks a picture or more reference
  frames (with output reordering on, more frames in the DPB too) before
  anything is allocated for it, so what the decoder allocates for a sequence
  is bounded: at most maxRefFrames + 1 pictures of maxMbs macroblocks and
  their macroblock entries. A refused one is an error from `h264bsdDecode`
  (the rest of that access unit isn't decoded) and drops the set stored with
  its id, deactivated if active, so the stream needs a new sequence parameter
  set and an IDR picture; one the kept parameter sets refuse anyway
  (`h264bsdKeepParamSets`) is only refused, as any other, and the kept ones
  decode on. The parameter sets it holds aren't bounded by it
  (`src/h264bsd_decoder.h`).
- `H264BSD_BUFFER_GATE=1`: `h264bsdSetBufferGate(dec, fn, ctx)` calls
  `fn(ctx, luma)` with the buffer each new picture is about to be decoded
  into, before any pixel of it is written, and with every picture buffer
  before it is freed (another sequence parameter set, `h264bsdShutdown`);
  `fn` returns once a display that reads pictures in place is done with it.
  Refused (an error, nothing set) in strip mode, which has no picture
  buffers.
- `H264BSD_LEAN_VUI=1`: a VUI parser of under 1 KB instead of about 5 KB. It
  walks every field with the full parser's checks, so it accepts and refuses
  the same parameter sets and ends on the same bit, and keeps only what the
  decoder reads; the SEI parser, which nothing calls, is left out with it.
  `tests/tinyh264_vui_lean_harness.c` runs both parsers on every sequence
  parameter set of the streams, 200 mutations of each and 20,000 crafted
  ones cut at every byte.
- `H264BSD_LOWMEM=1`: `h264bsdSetLowMem(dec, flags)`, right after
  `h264bsdInit`, for boards without external RAM (`src/h264bsd_lowmem.c`).
  Macroblock entries shrink from 216 to 160 bytes (32-bit targets), and
  `H264BSD_LOWMEM_MBRING` keeps them for three macroblock rows instead of
  the whole picture. `H264BSD_LOWMEM_INPLACE` (with one reference frame and
  no output reordering) decodes each picture over its reference in one
  buffer D luma rows taller than a picture: D from the stream's motion
  vector limit, or bits 8-15 of the flags times 8. A picture whose motion
  reaches further up than D is an error, never a wrong picture; the picture
  returned stays valid until the next call, and `h264bsdPicturePlanes`
  gives its chroma planes. This mode conceals nothing: a damaged picture is
  dropped, and in place, once one (or a reference picture that reference
  marking can't store) has overwritten part of the reference, the decoder
  waits for an IDR or all-intra reference picture (an all-intra
  non-reference one is still shown). With the macroblock ring alone it
  doesn't wait: the P pictures after a dropped reference picture are decoded
  against the reference before it, wrong but not errors, until the next IDR
  or intra picture, as the shipped decoder drifts after a picture it
  conceals or its caller abandons. Not with `TINYH264_MVE` where it builds
  Helium, yet (refused at compile time). Peak
  memory (32-bit build, one reference frame), shipped / ring / in place /
  in place D = 64: 180 x 180 150.8 / 125.6 / 112.4 / 88.7 KB, 240 x 240
  230.8 / 189.5 / 155.7 / 126.2 KB, 480 x 480 897.7 / 717.8 / 569.6 /
  418.4 KB.
- `H264BSD_STRIP=1`: `h264bsdSetStrip(dec, k, fn, ctx)`, for intra-only
  streams on boards that can't hold a picture (`src/h264bsd_strip.c`): the
  decoder works in a ring of k + 2 macroblock rows and hands each row to
  `fn` once it's final, in order (k from 1 to 255); `h264bsdDecode` returns
  a NULL picture pointer. P slices, redundant slices and macroblocks out of
  raster order are refused; nothing is concealed. The call itself is
  refused (an error) for k = 0, with output reordering on
  (`h264bsdInit(dec, 0)`) or a buffer gate set. Not with `TINYH264_MVE`
  where it builds Helium, yet (refused at compile time). It assumes a flat
  address space, as on the cores it's for: the picture's row addresses are
  re-based to point before the ring and are dereferenced only once a row
  offset brings them into it. Peak
  memory (32-bit build, intra-only), shipped / k = 1 / k = 4: 180 x 180
  150.8 / 30.8 / 52.4 KB, 240 x 240 230.8 / 36.5 / 63.5 KB, 480 x 480
  897.7 / 66.2 / 120.2 KB (with `H264BSD_LOWMEM`'s smaller macroblock
  entries, k = 1: 28.9, 34.1, 61.3 KB).
- `H264BSD_ONEREF=1` (with `H264BSD_LOWMEM`): in place, a sequence parameter
  set that allows several reference frames decodes keeping one, for encoders
  that declare several and predict from the last picture only. A slice that
  needs another (more than one active, reordering, long-term or adaptive
  marking) is refused and the decoder waits for an IDR picture; a
  non-reference picture is skipped, and `h264bsdRefInfo` counts those so a
  caller can tell such a skip from an error. Peak memory with a sequence
  parameter set allowing 4 reference frames, shipped / one kept in place:
  180 x 180 316.9 / 112.4 KB, 240 x 240 490.2 / 155.7 KB, 480 x 480
  1934.7 / 569.6 KB.
- `H264BSD_CHANGED_MAP=1`: `h264bsdChangedMap(dec, &clean)` says which
  macroblocks of the picture just returned carry the previous output
  picture's pixels before the loop filter (inter predicted from that very
  picture with zero motion and no residual, and no luma coefficients where it
  deblocks), one bit each, when its result
  has bit 0 set. With bit 1 also set a slice deblocked, and a clean
  macroblock beside a changed one may differ in the 3 luma (1 chroma) pixels
  along their shared edge: a display that redraws only the changed
  macroblocks redraws those edges too, or the whole picture. The contract is
  in `src/h264bsd_decoder.h`. `h264bsdChangedMapSkip` pauses it. No map (bit
  0 clear; bit 2 then marks an IDR picture) after an IDR, an error, a gap,
  reordered or long-term references, output reordering on, a concealed
  picture, or one larger
  than `H264BSD_CHANGED_MAP_MBS` (1024) macroblocks. `muse_vdec_same.c`
  works out a similar map after the fact from the macroblock entries; this
  one is built while decoding, so it works with the macroblock ring too.
