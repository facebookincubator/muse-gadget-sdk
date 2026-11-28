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

# lc3fx: fixed-point LC3 for every Muse Gadget

LC3 on every Muse Gadget: a fixed-point port of LC3 that runs on all devices
with very efficient CPU and memory use. This is the copy this SDK vendors
(`VENDORED.md`).

- **What:** an integer-only LC3 encoder + decoder, same API as Google liblc3
  (`include/lc3.h`) but for three refusals: no float PCM (encode and decode
  return -1), no resampling (setup returns NULL unless the PCM rate is the
  codec rate) and no high-resolution mode (an HR setup returns NULL, and the
  HR size queries -1). Apache-2.0 derivative of liblc3 (clean room: no
  LC3plus/ETSI fixed-point code). Interoperates both ways with float liblc3;
  decoder within ~1 LSB of float (RMS -114..-120 dBFS, LC3 conformance
  thresholds pass); bit-exact across RISC-V (C6), Cortex-M33 and Xtensa.
- **Numbers (16 kHz, 10 ms, 32 kb/s voice):** ESP32-C6 enc 544 / dec 287 us
  per frame (13x/12x faster than float soft-float, codec RAM 49 KB with code
  in IRAM).
- **Source:** the Muse Gadgets fixed-point port of Google liblc3 (see
  VENDORED.md). In this SDK: `xplat/lc3fx`, which `esp32/components/liblc3`
  builds for chips without an FPU; the ESP32 and Zephyr gadgets use it.
- **Porting notes:** targets whose ROM keeps the stack 4-byte aligned need
  `-mpreferred-stack-boundary=2`; put the hot tables and
  tns/sns/spec/mdct in RAM when code runs from XIP flash; pointer tables are
  `* const`.
- **Use FPU float liblc3 instead** only where the chip has a fast FPU and the
  float build is already in place (`xplat/liblc3` in this repo); both decode each
  other's streams.
- Tests: `python3 xplat/tests/run_tests.py` (CI: `.github/workflows/xplat.yml`)
  decodes lc3fx's frames with liblc3 and liblc3's with lc3fx, holds lc3fx's
  decoder within 2 LSB of float liblc3's and its output to recorded hashes,
  and runs under ASan and UBSan (`--sanitize`); `--fuzz` fuzzes the decoder.
