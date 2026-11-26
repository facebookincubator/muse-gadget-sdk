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

# lc3-kotlin

An LC3 (Low Complexity Communication Codec, Bluetooth LE Audio) encoder and decoder written in plain
Kotlin for the JVM and Android. It needs no JNI, no native library and no runtime dependency: the jar
uses only the JDK, so Java apps can use it without the Kotlin standard library.

It's a translation of Google's [liblc3](https://github.com/google/liblc3) v1.1.3+5 (8e1e722c), limited to 16 kHz,
the protocol's default LC3 rate: for 24 kHz, ask the gadget for 16 kHz or use liblc3. Muse gadget
builds use 10 ms frames. Its output is **bit-identical to liblc3** (see [Conformance](#conformance)).

| | |
|---|---|
| Sample rate | 16 kHz, mono, 16-bit PCM |
| Frame duration | 10 ms (160 samples) or 7.5 ms (120 samples) |
| Frame size | 20 to 400 bytes (16 to 320 kb/s at 10 ms). The gadget setting is 40 bytes at 10 ms = 32 kb/s |
| Encoder | Complete except the long-term postfilter (LTPF) analysis: every frame signals "no pitch", a valid frame for any LC3 decoder (the same as liblc3 with `lc3_encoder_disable_ltpf`) |
| Decoder | Complete: noise filling, TNS, SNS, LTPF postfilter, packet-loss concealment |
| Memory | A few KB of state per encoder or decoder, about 50 KB of tables shared by all; no allocation per frame |
| Size | 60 KB of dex (38 KB compressed) after R8 |
| License | Apache-2.0, derived from liblc3 (Copyright 2022 Google LLC). See [LICENSE](LICENSE) and [NOTICE](NOTICE) |

## Usage

```kotlin
import com.musegadgets.lc3.Lc3
import com.musegadgets.lc3.Lc3Decoder
import com.musegadgets.lc3.Lc3Encoder

val encoder = Lc3Encoder()                  // 16 kHz, 10 ms; Lc3Encoder(Lc3.FRAME_7_5MS) for 7.5 ms
val decoder = Lc3Decoder()
val pcm = ShortArray(encoder.frameSamples)  // 160
val frame = ByteArray(Lc3.DEFAULT_FRAME_BYTES)

encoder.encode(pcm, 0, frame, 0, frame.size)        // one frame in, 40 bytes out
val ok = decoder.decode(frame, 0, frame.size, pcm, 0) // false: the frame was corrupt and concealed
decoder.conceal(pcm, 0)                             // a lost frame (packet-loss concealment)
```

Java uses the same API: `new Lc3Encoder()`, `encoder.encode(pcm, 0, frame, 0, 40)`,
`Lc3.frameBytes(Lc3.FRAME_10MS, 32000)`.

- **Frame size:** `Lc3.frameBytes(durationUs, bitrate)` and `Lc3.bitrate(durationUs, bytes)` convert
  between frame size and bitrate, as liblc3 does; a negative bitrate or size throws
  `IllegalArgumentException`. The size may change from frame to frame.
- **Delay:** `Lc3.delaySamples(durationUs)` is the codec delay: 40 samples (2.5 ms) at 10 ms, 64 at
  7.5 ms.
- **State and threads:** each instance holds one stream's state and isn't thread-safe. Use one
  encoder or decoder per stream; `reset()` starts a new stream.
- **Invalid arguments:** a frame size outside 20 to 400 bytes, or arrays too short for a frame, throw
  `IllegalArgumentException` or `IndexOutOfBoundsException` before any state changes. Corrupt frames
  never throw: they're concealed and `decode` returns false, the same frames liblc3 rejects.

### On Android

- **License:** the jar carries `META-INF/LICENSE` and `META-INF/NOTICE`; keep them when you
  repackage it.
- **Keep rule:** the jar carries an R8 rule (`META-INF/proguard/lc3-kotlin.pro`) that keeps the
  codec's stages as separate methods. Without it, R8 inlines them into one huge method that ART
  compiles badly: the encoder got up to 2× slower on the Cortex-A510. R8 picks the rule up
  automatically. If your build doesn't read rules from jars, copy it into your own rules.
- **Baseline profile:** `META-INF/baseline-prof.txt` lists the codec. AGP 8 merges it, so the codec
  is compiled ahead of time at install. Otherwise the first frames of the first call run in the
  interpreter or JIT, at up to a few milliseconds a frame on a little core.
- **Warm-up:** if you can't ship a baseline profile, call `Lc3.warmUp()` once, off the audio thread,
  when a call starts.

## Conformance

The reference is upstream liblc3 v1.1.3+5 (commit `8e1e722c`, the commit
[`xplat/liblc3`](../../xplat/liblc3) is trimmed from), built in full without NEON intrinsics and without FMA contraction (`-U__ARM_NEON -ffp-contract=off`). The
Kotlin code performs the same floating-point operations in the same order, so:

- Encoder bitstreams are byte-identical to liblc3's (LTPF analysis off).
- Decoded PCM is byte-identical to liblc3's. That holds for any LC3 stream at these settings,
  including liblc3's own LTPF streams, lost frames (concealment) and corrupt or random frames.
- Against a default liblc3 build, which uses NEON and FMA instructions, decoded PCM is within 1 LSB
  (RMS difference −125 dBFS; the LC3 conformance bound is −89 dB).

How it was checked:

- **Unit tests** (`./gradlew test` or `./build.sh test`) run on a deterministic synthetic signal
  ([`TestSignal`](src/test/kotlin/com/musegadgets/lc3/TestSignal.kt): voiced speech-like sound,
  noise bursts, silence, narrowband content, a full-scale square wave and a chirp). They compare
  the SHA-256 of the encoder output for 14 configurations, the decoder output (with and without
  lost frames), two liblc3 LTPF streams and a corrupted stream with liblc3's. The vectors come from
  [`tools/make_vectors.sh`](tools/make_vectors.sh); the only binary files are two 12 KB LC3 streams.
- **Real speech:** [`tools/compare_liblc3.sh`](tools/compare_liblc3.sh) on 126 s of speech (4
  speech clips and 38 voice prompts) found every output identical to liblc3 at 16 configurations:
  7.5 and 10 ms × 20, 30, 40, 60, 80, 100, 160 and 400 bytes. The encoder was compared on about
  235,000 frames; the decoder on streams from both encoders, with and without lost frames.
- **On the phone:** on a Galaxy S22 under ART (JIT and AOT), bitstreams and PCM were byte-identical
  to the host JVM and to scalar liblc3 running on the same phone.
- **Quality:** PESQ-WB over 42 speech clips at 32 kb/s is 3.986, the same as liblc3 with LTPF
  analysis off (liblc3's default encoder, with LTPF analysis, scores 4.071).

## Performance

Measured on a Galaxy S22 (Snapdragon 8 Gen 1) at 16 kHz, 10 ms, 40 bytes, on 126 s of speech. The
Kotlin code was AOT-compiled with the R8 rule above. It ran in the same process as liblc3, with each
block of 32 frames alternating between the two so both saw the same clocks. Times are per 10 ms
frame; % is the share of one core used for real-time audio.

| Core | Kotlin encode | liblc3 encode | Kotlin decode | liblc3 decode | Kotlin ÷ liblc3 (enc / dec) |
|---|---|---|---|---|---|
| Cortex-X2 | 8.0 µs (0.08 %) | 5.8 µs | 4.8 µs (0.05 %) | 3.3 µs | 1.36 / 1.48 |
| Cortex-A710 | 18.2 µs (0.18 %) | 12.6 µs | 10.1 µs (0.10 %) | 6.7 µs | 1.44 / 1.51 |
| Cortex-A510 | 73 µs (0.73 %) | 50 µs | 30 µs (0.30 %) | 19 µs | 1.46 / 1.56 |

The absolute times move with the phone's clocks from run to run; the ratios hold to about ±5 %. The liblc3
here is a default `-O3` arm64 build: NEON intrinsics, FMA, auto-vectorization. Against the scalar build
without FMA (the bit-exact twin; the outputs compared identical on the phone), Kotlin is 1.33× / 1.34× on
the X2, 1.32× / 1.34× on the A710 and 1.29× / 1.28× on the A510. Against an optimized C build of liblc3
(trimmed to these configurations, conformant flags), it's 1.44-1.58×.

A full-duplex call (encode and decode every 10 ms) costs about 0.2 % of a big core, or 1.1 % of a
little core; paced in real time and left to the scheduler, about 2.5-3 % of one core. Steady state
allocates nothing and triggers no garbage collection.

The first frames depend on how the code is compiled. First 100 frames of a fresh process, median and
worst, encode / decode:

| | X2 | A510 |
|---|---|---|
| JIT only (no baseline profile) | 207 / 100 µs, worst 0.6 / 2.8 ms | 943 / 347 µs, worst 2.4 / 1.2 ms |
| JIT, after `Lc3.warmUp()` (took 133 ms on the X2, 619 ms on the A510) | 66 / 35 µs, worst 0.13 / 0.10 ms | 398 / 140 µs, worst 4.7 / 4.3 ms |
| AOT (baseline profile or `speed`) | 12 / 7.8 µs, worst 0.13 / 0.06 ms | 52-72 / 30-37 µs, worst 0.4-6 ms (page faults) |

So ship the baseline profile. Warm-up helps a JIT-only build, but it runs the code in the interpreter
first, which is slow.

### How it got there

The first port ran at 1.6-1.75× liblc3, measured the same way. Each step below kept the output bit-identical. The table
gives Kotlin ÷ liblc3 (NEON), encode / decode.

| Step | X2 | A510 |
|---|---|---|
| First port | 1.76 / 1.61 | 1.74 / 1.76 |
| TNS filters in registers; arithmetic-coder calls without object arguments | 1.69 / 1.55 | 1.74 / 1.69 |
| All 9 TNS autocorrelation lags in one pass; quantized magnitudes computed once; byte/char tables; spectrum coding loops with the coder state in locals | 1.37 / 1.66 | 1.54 / 1.63 |
| MDCT/FFT on split real/imaginary arrays with pre-split twiddles (loops ART can check once) | 1.41 / 1.61 | 1.53 / 1.59 |
| SNS codebook search, DCT and gain search reshaped (vectorizable or four independent chains); log2 four bands at a time | 1.42 / 1.63 | 1.69 / 1.55 |
| R8 keep rule (stops R8 merging the stages into one huge method); tables initialized in the constructors | **1.36 / 1.48** | **1.46 / 1.56** |

Run-to-run variation is about ±5 %. Two of the steps beat C outright: TNS (0.9× liblc3) and the SNS
codebook search (0.9×), because the restructured loops have more independent work than the C code.

What keeps the rest at about 1.4×:

- ART vectorizes only simple loops that accumulate through memory. It can't keep SIMD accumulators
  in registers, so the FFT, DCT and gain-search kernels that clang vectorizes stay scalar.
- On the in-order Cortex-A510, ART's instruction scheduling and remaining bounds checks cost more
  than on the big cores.
- FMA contraction isn't available. Using it would also break bit-exactness.

## Building

- **Gradle:** `./gradlew test jar`. Other Gradle builds can use the library through
  `includeBuild("../lc3-kotlin")` and `implementation("com.musegadgets:lc3-kotlin")`.
- **Without Gradle:** `./build.sh` writes `build/lc3-kotlin.jar`; `./build.sh test` also runs the
  checks; `./build.sh bench` also builds `build/lc3-bench.jar`, the file to file encoder and decoder
  that `tools/compare_liblc3.sh` runs.
- **Generated code and vectors:** `tools/gen_tables.c` and `tools/gen_consts.py` regenerate
  `Lc3Tables.kt` and `Lc3Consts.kt` from liblc3's sources (`gen_consts.py` takes the liblc3 v1.1.3+5 (8e1e722c)
  checkout in `LIBLC3` and needs Python 3 with numpy); `tools/make_vectors.sh` regenerates the
  test vectors.

| Path | |
|---|---|
| `src/main/kotlin/com/musegadgets/lc3/` | the library: `Lc3Encoder`, `Lc3Decoder`, `Lc3` (public); the rest internal |
| `src/main/resources/META-INF/` | R8 rule and baseline profile, packed in the jar |
| `src/test/` | conformance checks, synthetic signal, reference vectors |
| `bench/` | file to file encoder and decoder (`BenchMain`: enc, dec) for `tools/compare_liblc3.sh` |
| `tools/` | table generators, liblc3 reference CLI, vector and corpus comparison scripts |
