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

# libsbc

Google's SBC (Bluetooth subband codec) encoder and decoder, the default codec
of the musegadgets BLE protocol: 16 kHz mono, 16 blocks, 8 subbands, loudness
allocation. One copy serves both SDKs: the ESP32 component
[`esp32/components/libsbc`](../../esp32/components/libsbc) compiles these
sources with the options below, and the Zephyr gadgets use the same ones.

| | |
|---|---|
| Upstream | https://github.com/google/libsbc |
| Version | commit `6e505650145c9973d08a0bdd5e5f5e1914305e40` (2022-09-13, "fix: Limit bitpool to positive compression ratio") |
| License | Apache-2.0, Copyright 2022 Google LLC. See [`LICENSE`](LICENSE). The optimizations are by Meta Platforms, Inc. and affiliates under the same license. |

This is not the BlueZ SBC library, which is LGPL. Don't replace it with that.

## What's upstream and what's ours

The sources are an optimized build of upstream. The changes are compile-time
options; with all of them
off the code is upstream, and with them on the output stays bit-exact with
upstream (776 encoder and decoder comparisons over 196 stream configurations:
16 to 48 kHz, 4 to 16 blocks, 4 and 8 subbands, bitpools 19 to 53). Each
changed file says so in its header. Upstream's standalone tools and Makefiles
are left out.

| File | From |
|---|---|
| `include/sbc.h` | upstream, modified: trimming options (`SBC_MAX_CHANNELS`, `SBC_PLC`) |
| `src/sbc.c` | upstream, modified: symmetric DCT (`SBC_FAST_DCT`), 16-entry CRC table (`SBC_SMALL_CRC`), Arm dual-MAC windowing (`SBC_DSP`), mergeable constant tables (`SBC_MERGE_TABLES`), mono and 8-subband trimming; dual-channel encoding no longer computes the second channel's scale factors twice (upstream writes past the array); concealment takes the channel count from the last good frame and refuses before one; a frame description with an out-of-range frequency, mode or allocation method is rejected (upstream indexes its tables with them); no left shift of a negative value |
| `src/bits.c`, `src/bits.h` | upstream, modified: optional single-unit inlining (`SBC_UNITY`) |
| `LICENSE` | upstream, unmodified |
| `README.md` | Meta |

## Build options

| Option | Effect | ESP32 | Zephyr |
|---|---|---|---|
| `SBC_MAX_CHANNELS=1` | mono-only state (halves the codec RAM); changes `struct sbc`, so every user of `sbc.h` needs it | on | on |
| `SBC_WITH_4SB=0` | drops 4-subband support | on | on |
| `SBC_SMALL_CRC=1` | a 16-entry CRC table instead of 256 | on | on |
| `SBC_FAST_DCT=1` | symmetric analysis matrixing | on | on |
| `SBC_DSP=1` | Armv7E-M/Armv8-M dual-MAC windowing | off (Xtensa, RISC-V) | on with a DSP extension (Cortex-M33) |
| `SBC_UNITY=1` | `bits.c` compiled into `sbc.c` so the bit writer inlines | off | with `SBC_DSP` |
| `SBC_MERGE_TABLES=1` | identical constant tables (the header maps, the frequency and cos(i*pi/8) tables) in mergeable sections, so the linker keeps one copy; GCC on ELF targets with a 4-byte `int`, a no-op elsewhere. 8 B of 924 on RISC-V, 24 of 940 on Xtensa (the SDKs' build) | off | off |
| `SBC_PLC` | packet loss concealment: a lost frame replays the last good one, 6 dB quieter each time; changes `struct sbc` | off | off |

`SBC_MAX_CHANNELS` and `SBC_PLC` change the size and layout
of `sbc_t`, so they are public definitions: build every file that includes
`sbc.h` with the same values (the ESP32 component exports them `PUBLIC`), or
a caller allocates a context of the wrong size. The others only change
`sbc.c` and stay private.

Concealment (`sbc_decode` with `data` NULL) doesn't fill the caller's frame
description, as upstream: a caller that needs the layout keeps the last good
frame's.

## Numbers

16 kHz speech at 60.8 kb/s, PESQ-WB 4.35 for upstream and this build alike:

| | Upstream | This build |
|---|---|---|
| Encoder code + data, ESP32-S3 | 5.8 KB | 3.7 KB |
| Encoder, Xtensa (ESP32 in QEMU) | 2.8 MIPS | 2.6 MIPS |
| Encoder, RV32IMC (ESP32-C3 in QEMU, stands in for the C6 and C5) | 2.3 MIPS | 2.0 MIPS |
| Encoder stack | 880 B | 704 B |
| Encoder, Cortex-M33 (emulator, `-O2`, `SBC_DSP`) | | 14.8k instructions per frame (1.9 MIPS), 3.2 KB flash, 372 B static RAM |

## Updating

Take the new upstream release's `include/` and `src/`, apply the options
listed above again and keep the modification notes in the file headers. Then run
`python3 xplat/tests/run_tests.py` and `python3 xplat/tests/run_tests.py
--sanitize` from the repository root: they encode speech and decode it back,
check the trimmed and full builds give the same bits as the recorded hashes,
and check the refusals. CI runs them and a decoder fuzz
(`.github/workflows/xplat.yml`).
