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

# Porting the codecs for speed

How to build the SDK's codecs (`lc3fx`, `libsbc`, `tinyh264`, `libvpx_vp8`)
on a new MCU so they run as fast as the ESP32 builds: their own -O2
archives, the hot code and tables in RAM, and the optimization switches.
Every switch below is off by default, but tinyh264's `H264BSD_CLZ` and
`H264BSD_WORD_LOADS`, which follow the target, and lc3fx's
`LC3FX_TNS_AC_BLOCK`, which is on. With every switch at its default the code
is what the ESP32 SDK builds, but for two libvpx_vp8 switches the SDK sets:
`VP8_MGX_BORDER=16` on the ESP32-S3 and ESP32-S31, and `VP8_MGX_COMPACT=1`
with `CONFIG_MUSE_VP8_IRAM_COMPACT`.

## Build the codecs as their own -O2 archives

MCU SDKs often build everything at -Os or -Oz, where the codecs' inner
loops run slower. Build each codec as its own static library at -O2, with
the same options the SDK uses, and link it into the image; measure it against
the SDK's level on your core:

```sh
CFLAGS="-O2 -ffunction-sections -fdata-sections $TARGET_FLAGS"
for f in xplat/lc3fx/src/*.c; do
  $CC $CFLAGS -DLC3_DEC_LTPF=0 -DLC3_DT_MASK=8 -DLC3_SR_MASK=6 \
      -Ixplat/lc3fx/include -Ixplat/lc3fx/src -c "$f" -o "build/lc3fx/$(basename "$f" .c).o"
done
# xplat/tools/hot_place.py (below), if you use it, runs here, before $AR
$AR rcs build/liblc3fx.a build/lc3fx/*.o
```

- -O2, not -O3: -O3's inlining makes tinyh264's hot code 2.3 times bigger
  (110 KB), more than RAM can hold.
- `-ffunction-sections -fdata-sections`: one section per function and
  table, so the linker script (or `xplat/tools/hot_place.py`) can place
  them one by one and `--gc-sections` drops the rest.
- Keep the defines the ESP32 components use (each codec's README and
  `esp32/components/*/CMakeLists.txt`): the rate masks for lc3fx,
  `SBC_MAX_CHANNELS=1 SBC_WITH_4SB=0 SBC_SMALL_CRC=1 SBC_FAST_DCT=1` for
  libsbc. `SBC_MAX_CHANNELS`, `LC3_ENC_LTPF` and `LC3_DEC_LTPF` change
  structure layouts: build every file that includes the codec's header with
  the same values.

## Hot code and tables in RAM

Where code runs from XIP flash through a small cache, the per-frame code
and its tables usually run faster from RAM.

| Codec | Hot code, most important first | Hot tables |
|---|---|---|
| lc3fx | `tns`, `sns`, `spec`, `mdct` (whole objects); then `bits`, `energy`, `attdet`, `bwdet`; in `lc3`: `lc3_encode`, `analyze`, `encode`, `load_s16`, `lc3_decode`, `decode`, `synthesize`, `store_s16` | `tables`: the MDCT window, band limits, MDCT rotation and FFT twiddles of the rate you use (`mdct_win_10m_16k`, `band_lim_10m_16k`, `mdct_rot_*_w`, `fft_twiddles_*_t`), the spectrum, TNS and SNS tables; the LTPF and PLC tables are cold |
| libsbc | `sbc`, `bits` (small: 5-8 KB of code) | the window and cos tables in `sbc` |
| tinyh264 | smallest set (about 13 KB): `h264bsd_reconstruct`, `h264bsd_cavlc`, `h264bsd_transform`, `h264bsd_vlc`, `h264bsd_image`, `h264bsd_util`, `h264bsd_neighbour`; the full set (about 44 KB): `esp32/components/tinyh264/linker.lf`. With a motion-compensation switch also its object (`h264bsd_interp_dsp`, `h264bsd_interp_packed4` or `h264bsd_inter_mve`; with `TINYH264_MVE` also `h264bsd_deblocking`), or use `H264BSD_HOT_SECTION`, which marks them | the clip table `h264bsdClip` (in `h264bsd_intra_prediction`), the CAVLC tables |
| libvpx_vp8 | `esp32/components/libvpx_vp8/linker.lf`: hot (tokens, inverse transforms, inter prediction, bilinear; about 12 KB), compact (33 KB) or full (45 KB) | the same objects' tables |

The ESP32 SDK does this with `linker.lf` in each component. Elsewhere, two
ways:

- **Section names in the source.** The port defines the section names:
  - lc3fx: `-DLC3_HOT_SECTION='".ram_text.lc3"'` puts the `LC3_HOT`
    functions there, `-DLC3_TABLES_IN_RAM='".ram_rodata.lc3"'` the data
    tables of `src/tables.c` (the descriptors and pointer tables that index
    them stay in `.rodata`);
  - tinyh264: `H264BSD_HOT_SECTION` and `H264BSD_HOT_DATA_SECTION` (its
    README).
- **After the compile, per function or table:** `xplat/tools/hot_place.py`
  renames the listed objects' `.text.<name>` and `.rodata.<name>` sections
  to `<hot-text>.<object>.<name>` and `<hot-rodata>.<object>.<name>`. It
  edits the objects in place, so run it before `$AR rcs` builds the archive:

  ```sh
  cat > lc3.hot <<'EOF'
  tns
  sns
  spec
  mdct
  lc3:lc3_encode
  tables:mdct_win_10m_16k
  tables:band_lim_10m_16k
  EOF
  OBJCOPY=$PREFIX-objcopy OBJDUMP=$PREFIX-objdump \
      python3 xplat/tools/hot_place.py --hot-list lc3.hot \
      --hot-text .ram_text --hot-rodata .ram_rodata build/lc3fx/*.o
  $AR rcs build/liblc3fx.a build/lc3fx/*.o
  ```

Either way, the port's linker script gives those sections a RAM address and
a flash load address, and its startup code copies them to RAM before the
first codec call, as it does `.data`. The linker line alone copies nothing:
add the section to the startup's copy table, or copy it by its symbols:

```
.ram_text : {
  _ram_text_start = .;
  *(.ram_text .ram_text.*) *(.ram_rodata .ram_rodata.*)
  _ram_text_end = .;
} > RAM AT > FLASH
_ram_text_load = LOADADDR(.ram_text);
```

```c
extern char _ram_text_start[], _ram_text_end[], _ram_text_load[];
memcpy(_ram_text_start, _ram_text_load, _ram_text_end - _ram_text_start);
```

The RAM must be executable, and on a core with an instruction cache or
separate instruction and data paths the copy needs the target's
synchronization before the code runs: `fence.i` on RISC-V; `dsb` and `isb` on
Arm, and on a core with a write-back data cache (Cortex-M7, for example) first
a clean of the copied range from the D-cache and an I-cache invalidate (CMSIS
`SCB_CleanDCache_by_Addr()` and `SCB_InvalidateICache()`). This code parses
untrusted streams: where the core has an MPU or PMP, make the copied region
read-only (and executable) once the copy is done.

Measure before keeping it: on a core with a fast flash path or a large
cache, code in RAM can be slower than from flash (tables in RAM rarely are).

## Optimization switches

All off by default (but tinyh264's `H264BSD_CLZ` and `H264BSD_WORD_LOADS`,
which follow the target, and `LC3FX_TNS_AC_BLOCK`, which is on). The Xpulp
ones need `xplat/include` on the include path and `-DRV_XPULP=1` to emit the
instructions (`xplat/include/rv_xpulp.h`; without it, its plain-C twins run,
with the same results). The instruction path is experimental: it has run on
QEMU, which executes the instructions per the manual, not on hardware. Before
turning a codec's Xpulp switch on, build `xplat/tests/test_rv_xpulp.c` with
`-DRV_XPULP=1` and run it on the target: a wrong clip convention
(`RV_XPULP_CLIP_BIAS`) fails it.

| Switch | Codec | What it does |
|---|---|---|
| `RV_XPULP=1` | all | Xpulp instructions in `rv_xpulp.h` (RISC-V cores with the CV32E40P 1.0 / RI5CY Xpulp extensions); never set from a target macro; experimental (QEMU only) |
| `RV_XPULP_CLIP_BIAS=0` | all | for a core whose clip bound is 2^Is2 instead of the manual's 2^(Is2-1) (default 1) |
| `LC3FX_XPULP=1` | lc3fx | leading-zero count by fl1, 16-bit saturation by clip |
| `LC3FX_SWCLZ=1` | lc3fx | branchless leading-zero count where the core has none (libgcc's is a call) |
| `LC3FX_TNS_RECIP=1` | lc3fx | the TNS analysis's 64-bit divisions by reciprocals (`src/tns.c`): encoder only, not bit-exact (the encoded frames can differ) |
| `LC3FX_TNS_AC_BLOCK=0` | lc3fx | the TNS autocorrelation by pairs of lags instead of in blocks of 5 (`src/tns.c`; on by default), bit-exact |
| `LC3FX_MERGE_TABLES=1` | lc3fx | the inline helpers' identical tables kept once by the linker (GCC, ELF) |
| `LC3_HOT_SECTION`, `LC3_TABLES_IN_RAM` | lc3fx | section names for the hot functions and the data tables (above) |
| `LC3_HOT` | lc3fx | the hot functions' attributes, if the build defines it (then `LC3_HOT_SECTION` doesn't apply) |
| `SBC_DSP=1` | libsbc | Armv7E-M / Armv8-M DSP extension dual-MAC windowing |
| `SBC_UNITY=1` | libsbc | `bits.c` compiled into `sbc.c`, so the bit writer inlines |
| `SBC_MERGE_TABLES=1` | libsbc | identical small tables kept once by the linker (GCC, ELF) |
| `TINYH264_XPULP=1` | tinyh264 | clamps, abs and min/max on Xpulp |
| `TINYH264_MVE=1` | tinyh264 | experimental (run on QEMU, not on hardware): Arm Helium (MVE) where the compiler targets it (`__ARM_FEATURE_MVE`): motion compensation, the inverse transform, deblocking, the residual adds and the macroblock writes; elsewhere the C. Where it builds Helium, not with `H264BSD_LOWMEM` or `H264BSD_STRIP` yet |
| `TINYH264_DSP_MC=1` | tinyh264 | experimental (run on QEMU, not on hardware): motion compensation on the Armv7E-M / Armv8-M DSP extension (`__ARM_FEATURE_DSP`; plain-C stand-ins elsewhere), checked against the C on first use; `TINYH264_LOG` (printf-style) reports the check, `TINYH264_TLS=__thread` when several threads decode |
| `TINYH264_PACKED4_MC=1`, `TINYH264_PACKED4_OPS_H='"ops.h"'` | tinyh264 | motion compensation in packed 4-lane operations from the port's header (default `src/h264bsd_packed4_ops.h`, the plain-C model); `h264bsdPacked4Check()` checks a port's operations on the target (no first-use check); little-endian targets. One motion-compensation switch at a time, each off at 0 |
| `TINYH264_CUSTOM_ALLOC=1` | tinyh264 | every block from the port's allocator (`tinyh264_alloc`, `tinyh264_alloc_hot`, `tinyh264_free`); experimental, host-tested |
| `TINYH264_PIC_ALIGN=N` | tinyh264 | picture buffers aligned to N bytes (a power of two, at least 16; default 16); experimental, host-tested |
| `H264BSD_LIMITS=1` | tinyh264 | `h264bsdSetLimits()`: a sequence parameter set for more macroblocks or reference frames (with output reordering, more DPB frames) refused before anything is allocated: an error that drops the set stored with its id (one the kept parameter sets refuse anyway is only refused); experimental, host-tested |
| `H264BSD_BUFFER_GATE=1` | tinyh264 | `h264bsdSetBufferGate()`: a callback before each picture's buffer is written or freed; experimental, host-tested |
| `H264BSD_LEAN_VUI=1` | tinyh264 | a VUI parser of under 1 KB, accepting and refusing what the full one does; experimental, host-tested |
| `H264BSD_LOWMEM=1` | tinyh264 | `h264bsdSetLowMem()`: smaller macroblock entries, a three-row ring (`H264BSD_LOWMEM_MBRING`), one picture in place over its reference (`H264BSD_LOWMEM_INPLACE`); not with `TINYH264_MVE` where it builds Helium; experimental, host-tested |
| `H264BSD_STRIP=1` | tinyh264 | `h264bsdSetStrip()`: intra-only streams in a ring of macroblock rows; not with `TINYH264_MVE` where it builds Helium; needs a flat address space (its re-based row addresses point before the ring and are dereferenced only once a row offset brings them into it; `src/h264bsd_strip.c`); experimental, host-tested |
| `H264BSD_ONEREF=1` | tinyh264 | with `H264BSD_LOWMEM`: in place keeping one reference when the stream declares more; `h264bsdRefInfo()`; experimental, host-tested |
| `H264BSD_CHANGED_MAP=1` | tinyh264 | `h264bsdChangedMap()`: the macroblocks unchanged from the last picture before the loop filter (with result bit 1 set, a clean macroblock may differ along its edge with a changed one; `src/h264bsd_decoder.h`), for pictures up to `H264BSD_CHANGED_MAP_MBS` macroblocks (default 1024); experimental, host-tested |
| `H264BSD_COLD_LEVEL=1` or `2` | tinyh264 | small or rare functions, some run per macroblock, compiled -Os with GCC's `optimize` attribute (GCC only; check the port's flags hold there, or -Os per file), default 0 |
| `H264BSD_HOT_SECTION`, `H264BSD_HOT_DATA_SECTION` | tinyh264 | section names for the per-macroblock code and its tables (above) |
| `H264BSD_CLZ`, `H264BSD_WORD_LOADS` | tinyh264 | `__builtin_clz` and word-at-a-time stream loads, `H264BSD_CLZ` on unless the target is known to lack the instruction (RISC-V without Zbb, Arm without CLZ; set 0 on any other core without one), `H264BSD_WORD_LOADS` on where unaligned little-endian loads are cheap; a port can set either to 0, and to 1 (`H264BSD_WORD_LOADS=1` only on a little-endian core: a big-endian one is refused at compile time) |
| `VPX_XPULP=1` | libvpx_vp8 | clamps, abs and the loop filter's edge mask on Xpulp |
| `VP8_MGX_BORDER=N` | libvpx_vp8 | the pictures' border, 8 by default (16 for SIMD code that wants 16-byte aligned rows; the ESP32-S3 and ESP32-S31 builds set 16); edge emulation covers the rest |
| `VP8_MGX_COMPACT=1` | libvpx_vp8 | no loop unrolling and no per-width filter copies: smaller hot code, bit-exact (the ESP32 build sets it with `CONFIG_MUSE_VP8_IRAM_COMPACT`) |

Each codec's README has the details and its tests. CI runs
`xplat/tests/run_tests.py` once more with `LC3FX_XPULP`,
`LC3FX_MERGE_TABLES`, `SBC_MERGE_TABLES` and the lc3fx section names set (the
Xpulp switch on the plain-C twins); the other lc3fx and libsbc switches are not
built there. `esp32/tests/test_libvpx_vp8.py` decodes with `VPX_XPULP` too,
and `esp32/tests/test_tinyh264_knob_builds.py` and `test_tinyh264_knobs.py`
check tinyh264 with its portable switches. A compile-only CI job builds the
paths only other cores take: tinyh264 with `TINYH264_MVE` for a Cortex-M55 and
`TINYH264_DSP_MC` for a Cortex-M33 (Arm GCC), and the Xpulp users with
`RV_XPULP=1` for RV32 (clang).
