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

# lc3fx: vendored copy

**The Muse Gadgets fixed-point LC3 codec**, the integer-only LC3 that every
Muse Gadget can run: SBC and LC3 on every gadget, and lc3fx on chips without a
fast FPU. `src/fxmath.h` has portable C only (`lc3fx_clz()`, `LC3FX_SAT16`).

Integer overflow hardening in `src/lc3.c`, `src/fxmath.h`, `src/bits.c`,
`src/tns.c` and `src/common.h`: the global gain's Q27 log2 stays in 64 bits
until its exponent is taken out (past gain index 134 it would wrap, and
near-silent frames of 50 bytes or more at 16 kHz would decode as full-scale
clicks); packed 24-bit PCM rounds on its 24-bit value; the bit reader doesn't
shift by 32 on an overrun frame; TNS sums its autocorrelation energy in 64
bits (an isolated impulse would make a loud frame), and its synthesis
saturates on extreme frames from another encoder.

No high-resolution mode: an HR request is refused (setup returns NULL,
the size queries -1) rather than served as standard LC3 at that rate.
`src/common.h` sets `LC3_PLUS` and `LC3_PLUS_HR` to 0, so `src/tables.c`
and `src/bwdet.c` leave out liblc3's LC3-Plus (2.5 and 5 ms) and
high-resolution tables, which nothing could reach.

No undefined arithmetic: left shifts of signed values go through the
unsigned type (`LC3FX_SHL`, `LC3FX_SHL64` in `src/fxmath.h`), and spectral
shaping in `src/sns.c` zeroes a band whose shift would be 32 or more (only a
band of zeros has that much headroom). The output is bit-exact with the
code before the change. The postfilter (`LC3_DEC_LTPF=1`, `src/ltpf.c`)
does the same; `run_tests.py` builds and fuzzes it too.

The encoder's stack scratch buffer in `src/lc3.c` is 8-byte aligned by
hand, with a word of slack, instead of `alignas(8)`: compilers that realign
over-aligned stack arrays without slack on a 4-byte-aligned stack wrote past
it. The output is unchanged elsewhere.

`LC3_ENC_LTPF` and `LC3_DEC_LTPF` are layout definitions
(`include/lc3_private.h`): the library and every file that includes
`lc3.h` must be built with the same values.

`xplat/tests/run_tests.py` checks lc3fx and liblc3 against each other and
lc3fx's output against recorded hashes, also under ASan and UBSan, and
fuzzes the decoder (CI: `.github/workflows/xplat.yml`). To change the codec,
edit the files here, say what changed in this note, update the hashes below
(every file here but `README.md`) and, if the output changes on purpose, the
recorded ones in `run_tests.py`; the maintainers keep their other copies in
step.

```
cfc7749b96f63bd31c3c42b5c471bf756814053e847c10f3eb003417bc523d30  LICENSE
95f7684efdc4fbd183cbf6692dcc8fed18e1d8bad98855c929f4272640b76098  include/lc3.h
e6bd81f7ac3726bdfc2f10138ddbb5990a21d593e3d1b545acc8196e5229614b  include/lc3_private.h
4d6cf8c0448517dc0d9e6b47dc057884edb42fdb2dccdde035b4524b4dc90db3  src/attdet.c
e6cba23dea78d694405a26448ef1dfba48e433db6ea631ecf344f10c42aeb2d3  src/attdet.h
78ecaf9d2dc69aa98fd14eafbc27a66532daf2d8784caa6ee2339c3abbe630c0  src/bits.c
ce1529a758cbde44cc5a8b8c8f5676b3e6f92e59161c7c6f9867224e4b8eb98a  src/bits.h
b4e732b32e45b39c505238acd986303e63791587afb9567ae81f47a8eb3ca7bf  src/bwdet.c
7eac3dd6476b78f8bd8f1e2f275ee0f86d4099ce6077ca3fc01e992935a76db2  src/bwdet.h
14e32176de8045a2dfa65f1e54dbecd4a3ae22cc74ce2426275e18a541cb9f01  src/common.h
a0425e18de54fc3ecc7eec899b0ff80ef6b076929545e61357c24e8416b34299  src/energy.c
4be279f7455a5194542000a66b1c1987c9ee306f7fe4528363abe64ca5f1e076  src/energy.h
fde134dd8e855eabcbf30d81ddedfc3b6f52d2df8db663069bdac8d5ef0648b3  src/fxmath.h
ae3e38d4b50190ca05e136d024538e38ad9697c23569f7797cf6af405746c110  src/lc3.c
355d5e40dd40f123b904f90182124d8bdaf87f0a1743ba1074277f290c121c38  src/ltpf.c
5bb2f262b280eb6a26168466308a000c37d99b0f5642f67fec10180168e06f39  src/ltpf.h
cce018f41fa447d10427691c75ee75331f019e3cc8efb3d2b59af3b040193209  src/mdct.c
5c6aedf1de729bd053a395c7b8112c829c7877669268589bc881c6fb75157462  src/mdct.h
333c39bd66052581c1735b775afe8b18554b6e1c7d32b6874b280d5d2ce78c12  src/plc.c
eb2a0a494faeadf94b828ffc0a1c7a2e428218bf5fe467b0d893d209102a3ef5  src/plc.h
7fa18d1db805fa1ae85186546a90d8eaacd6d396642a3c7cfb628ca9cac29220  src/sns.c
d8a10163d4652c357d8e5202dfce5bb68b935fabcfc5f66450b4484da36f7826  src/sns.h
04bac9c240c6d3c233d8a1ba93f4e018b300f1d6ff35e4090162b20017cc2ccd  src/spec.c
ba8018b6b9289f2df62df515070df817a7adfa528cc5c0ce4266dde394d63bb7  src/spec.h
4400d445ba7fb63fdd91813e88489831bbccb3b3bc0613a043f45a2aea61d3e7  src/tables.c
3187d445c6f539b1377d54c4835fbc39d8842bef6e3e13b2bc2e65436458ba49  src/tables.h
b4b8c75dc959177effe95c0889c7835ecfcd664f09d33843f623dea111bc0f67  src/tns.c
200371db6060b564b038448772911975195c66baa6e0a37c4bf5094238eab234  src/tns.h
```

Integer-only LC3 encoder and decoder with Google liblc3's API (`include/lc3.h`)
and the standard bitstream: it decodes liblc3's frames and liblc3 decodes its
own (`xplat/tests/run_tests.py` checks both ways). Apache-2.0, a derivative
of liblc3 (`LICENSE`, and `README.md` for its numbers and porting notes).

How the ESP32 SDK builds it (for the BLE gadgets): `esp32/components/liblc3`, on chips without an FPU, 10 ms at 16 and 24 kHz (`LC3_SR_MASK=6 LC3_DT_MASK=8`, the rates the
SDKs use; the tables carry every rate, and the mask leaves the others' FFT
stages out), no decoder LTPF, and `linker.lf` for where its code and tables
live.
