#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
"""Host tests for the codecs in xplat/: liblc3, lc3fx and libsbc built as the
SDKs build them, and include/rv_xpulp.h.

    python3 xplat/tests/run_tests.py                 build and test
    python3 xplat/tests/run_tests.py --sanitize      the same under ASan + UBSan
    python3 xplat/tests/run_tests.py --fuzz 60       fuzz the decoders, 60 s per LC3 build

- lc3fx and liblc3 decode each other's streams, at 16 and 24 kHz.
- lc3fx and libsbc give bit-exact output: hashes of their encode and decode
  of integer test signals (tests/signal.h) against the values below. Integer
  codecs give the same bits on every host; a change that changes them changes
  the codec's output.
- Both LC3 builds refuse the rates and arguments they don't support, and a
  liblc3 mask beyond its trimmed tables doesn't compile.
- rv_xpulp.h's plain-C twins match a model of the Xpulp instructions' definitions
  across edge values (tests/test_rv_xpulp.c).
- SBC encodes and decodes back (16 and 15 blocks, mSBC), and the SDKs' mono
  8-subband build refuses other frames; the full build encodes and decodes a
  dual-channel frame and conceals by the last good frame's channel count.

--sanitize builds with -fsanitize=address,undefined and LC3_HOT empty, so the
lc3 hot functions are checked too. --fuzz runs tests/fuzz_decode.c, built
once with lc3fx and once with liblc3, with libFuzzer when the compiler has
it, else as a seeded random loop. CC picks
the compiler.
"""

import argparse
import array
import math
import os
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
XPLAT = HERE.parent

LC3_SRCS = ["attdet", "bits", "bwdet", "energy", "lc3", "ltpf", "mdct", "plc", "sns", "spec", "tables", "tns"]
# As esp32/components/liblc3 and libsbc build them.
LIBLC3 = ["-DLC3_ENC_LTPF=0", "-DLC3_DEC_LTPF=0", "-DLC3_PLUS=0", "-DLC3_PLUS_HR=0",
          "-DLC3_DT_MASK=8", "-DLC3_SR_MASK=6", "-ffast-math"]
LC3FX = ["-DLC3_DEC_LTPF=0", "-DLC3_DT_MASK=8", "-DLC3_SR_MASK=6"]
# The postfilter on (LC3_DEC_LTPF=1), with a float encoder that turns the LTPF on.
LC3FX_LTPF = [f.replace("LC3_DEC_LTPF=0", "LC3_DEC_LTPF=1") for f in LC3FX]
LIBLC3_LTPF = [f.replace("LC3_ENC_LTPF=0", "LC3_ENC_LTPF=1").replace("LC3_DEC_LTPF=0", "LC3_DEC_LTPF=1")
               for f in LIBLC3 if f != "-ffast-math"]
LIBSBC = ["-DSBC_MAX_CHANNELS=1", "-DSBC_WITH_4SB=0", "-DSBC_SMALL_CRC=1", "-DSBC_FAST_DCT=1"]

# lc3fx encode + decode of the test signals: "rate bytes kind" -> FNV-1a 64.
LC3FX_DIGEST = {
    "16000 20 0": "896f72af58e2c53d",
    "16000 20 1": "e65f073af6319563",
    "16000 20 2": "f93209d2f423a678",
    "16000 20 3": "17cb4f2bd647c79d",
    "16000 20 4": "6eeefb36e1756c73",
    "16000 20 5": "076e6cdce9f03ff8",
    "16000 20 6": "84814640c8ae34ac",
    "16000 20 7": "6d9e7aaf4d439fa5",
    "16000 20 random": "10069ecddcfa226e",
    "16000 40 0": "57476e4e0b2c521d",
    "16000 40 1": "4d103e679c969b14",
    "16000 40 2": "cefdb7f47affd7a0",
    "16000 40 3": "7438f9509eae378f",
    "16000 40 4": "94212ea3624137be",
    "16000 40 5": "c13cdef65afb5fd1",
    "16000 40 6": "002dab9fbe2bc179",
    "16000 40 7": "b0d2e82726eef202",
    "16000 40 random": "5ffa3100b0c1b09a",
    "16000 80 0": "058f0b7cecd039dd",
    "16000 80 1": "db4e6073c0f5ce51",
    "16000 80 2": "7c52954a51fe8e44",
    "16000 80 3": "277a983bed911977",
    "16000 80 4": "6bad88c34ae9d47e",
    "16000 80 5": "145eeab517261db9",
    "16000 80 6": "b398996ae95dd3c8",
    "16000 80 7": "bb54e9498ecc4d22",
    "16000 80 random": "88f0c3e8de2b63f5",
    "16000 155 0": "2233c86a522bcbb9",
    "16000 155 1": "4750ea9119ff25f5",
    "16000 155 2": "d47017b8e8bc429f",
    "16000 155 3": "4513b28bc0458ead",
    "16000 155 4": "a9e8fdbac2de948e",
    "16000 155 5": "9b97cf850b70dcc3",
    "16000 155 6": "c9d305506b0b7d68",
    "16000 155 7": "81bf1cfe4b89776e",
    "16000 155 random": "8325662f174063a6",
    "24000 20 0": "6c96848dd3ccb40d",
    "24000 20 1": "1deec3f00a824fed",
    "24000 20 2": "fe72df4bb5d73b5d",
    "24000 20 3": "5f07bd015533bc13",
    "24000 20 4": "85fa73a96692084f",
    "24000 20 5": "564131a35ea9d93b",
    "24000 20 6": "487454e98cf8484e",
    "24000 20 7": "372ffca8859af92c",
    "24000 20 random": "20745e49f3356f46",
    "24000 40 0": "fc1f5c7883b143ad",
    "24000 40 1": "c926a1bd80ec89bb",
    "24000 40 2": "599404fdd6e59d1c",
    "24000 40 3": "bca8063864f1ff60",
    "24000 40 4": "35b395bf8bfde401",
    "24000 40 5": "6368ced56f962ef2",
    "24000 40 6": "01f6abdf788d780d",
    "24000 40 7": "8620dabb9a7ee216",
    "24000 40 random": "01314a2556d96856",
    "24000 80 0": "b7358237ecf4806d",
    "24000 80 1": "64f8533ca2176b51",
    "24000 80 2": "96aaa83da48fc869",
    "24000 80 3": "eb2aab6742de48ae",
    "24000 80 4": "335a9ab3b0d7553b",
    "24000 80 5": "f9eec87e54a944d1",
    "24000 80 6": "fb08b71a074ddb6c",
    "24000 80 7": "0ce85ff62480ec06",
    "24000 80 random": "1bc6bdb1bf88d4c3",
    "24000 155 0": "bc89ef083a2f3ab9",
    "24000 155 1": "fa478e954b5f3af3",
    "24000 155 2": "27f132d47577b054",
    "24000 155 3": "b496aac384e762df",
    "24000 155 4": "d956c59a2fa7f5c1",
    "24000 155 5": "4a147d053db81791",
    "24000 155 6": "312d4e67990988c5",
    "24000 155 7": "f0a0af2f3dcf2fb9",
    "24000 155 random": "2d57486676cb108e",
}
# libsbc, the SDKs' build and the full one alike: "msbc blocks bitpool kind" -> FNV-1a 64.
SBC_DIGEST = {
    "0 16 26 0": "004e4cf2e24db53d",
    "0 16 26 1": "004e4cf2e24db53d",
    "0 16 26 2": "4c91134d7e850ae1",
    "0 16 26 3": "59f853cefc131ae0",
    "0 16 26 4": "839225ffafa37046",
    "0 16 26 5": "1df5213f6184eec6",
    "0 16 26 6": "bae1ef0709df8e01",
    "0 16 26 7": "448c91623aff52cb",
    "1 15 26 0": "29b8a6ceb6adad21",
    "1 15 26 1": "29b8a6ceb6adad21",
    "1 15 26 2": "0a77159820f6dd7f",
    "1 15 26 3": "1d4168d2e4f5a79b",
    "1 15 26 4": "3b8681a24f219fed",
    "1 15 26 5": "a6032fb0f416a081",
    "1 15 26 6": "18266ff5be46d225",
    "1 15 26 7": "4598e39ac8d89d5a",
    "0 16 2 0": "21f434eb8baa99a5",
    "0 16 2 1": "21f434eb8baa99a5",
    "0 16 2 2": "313ed1cb643a4cf2",
    "0 16 2 3": "cff184206d38b78d",
    "0 16 2 4": "9875045029632886",
    "0 16 2 5": "77fe35c770ea60ec",
    "0 16 2 6": "bc8844f900f98a6b",
    "0 16 2 7": "8a3f7caf428d59ce",
    "0 4 60 0": "052fe0829e67a585",
    "0 4 60 1": "052fe0829e67a585",
    "0 4 60 2": "a5de43106259b4bf",
    "0 4 60 3": "5c5316e25de4c5ee",
    "0 4 60 4": "9c2ad51edeb66b60",
    "0 4 60 5": "492ac2c77376f6aa",
    "0 4 60 6": "51fff5cebefcffe3",
    "0 4 60 7": "a9975b3db223118e",
    "0 16 124 0": "187e6888d9ef43a5",
    "0 16 124 1": "187e6888d9ef43a5",
    "0 16 124 2": "43c555b9bfe784e0",
    "0 16 124 3": "289edbdcffb7be5d",
    "0 16 124 4": "49ecacfb2f4c2a0a",
    "0 16 124 5": "1e40a42b5539cac1",
    "0 16 124 6": "37e51f82d96a4a9f",
    "0 16 124 7": "3f252b66834967bf",
}

failures = []


def check(ok, what):
    print(("PASS  " if ok else "FAIL  ") + what)
    if not ok:
        failures.append(what)


def cc():
    c = shlex.split(os.environ.get("CC", "cc"))
    if not c or not shutil.which(c[0]):
        sys.exit(f"no C compiler ({' '.join(c)})")
    return c


def build(out, srcs, flags, extra):
    cmd = [*cc(), "-std=c11", "-D_DEFAULT_SOURCE", "-Wall", "-Wno-unused-function",
           "-Wno-unused-const-variable", "-Wno-unknown-warning-option", *flags, *extra,
           *map(str, srcs), "-lm", "-o", str(out)]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        sys.exit(f"build of {out.name} failed:\n{' '.join(cmd)}\n{r.stdout}{r.stderr}")
    return out


def lc3_srcs(lib):
    return [XPLAT / lib / "src" / f"{f}.c" for f in LC3_SRCS], ["-I", str(XPLAT / lib / "include"), "-I", str(XPLAT / lib / "src")]


def sbc_srcs():
    return [XPLAT / "libsbc/src/sbc.c", XPLAT / "libsbc/src/bits.c"], ["-I", str(XPLAT / "libsbc/include")]


def run(*args):
    r = subprocess.run([str(a) for a in args], capture_output=True, text=True)
    if r.returncode:
        sys.exit(f"{' '.join(map(str, args))} failed ({r.returncode}):\n{r.stdout}{r.stderr}")
    return r.stdout


def snr(ref, out, delay, span=4):
    """Waveform SNR in dB of `out` against `ref` delayed by `delay` +- `span` samples."""
    best = -100.0
    for lag in range(max(0, delay - span), delay + span + 1):
        sig = err = 0.0
        for i in range(2000, min(len(ref), len(out) - lag)):
            d = out[i + lag] - ref[i]
            sig += ref[i] * ref[i]
            err += d * d
        best = max(best, 10 * math.log10(sig / (err + 1e-9)))
    return best


def voice(rate, seconds=2):
    """Two tones and a pitch glide at voice level, 1.5 Hz tremolo (+9.5 kHz at 24 kHz)."""
    pcm = array.array("h")
    for i in range(rate * seconds):
        t = i / rate
        v = 0.25 * math.sin(2 * math.pi * 220 * t) + 0.12 * math.sin(2 * math.pi * 1250 * t) \
            + 0.1 * math.sin(2 * math.pi * (300 + 400 * t) * t)
        if rate > 16000:
            v += 0.05 * math.sin(2 * math.pi * 9500 * t)
        pcm.append(int(v * 32767 * (0.5 + 0.5 * math.sin(2 * math.pi * 1.5 * t))))
    return pcm


def read_pcm(path):
    a = array.array("h")
    a.frombytes(Path(path).read_bytes())
    return a


def digests(text):
    return {" ".join(m[:-1]): m[-1] for m in (line.split()[1:] for line in text.splitlines() if line.startswith("digest "))}


def compare_digests(name, got, want):
    if not want:
        print(f"note  {name}: no reference digests; these are this build's:")
        for k, v in got.items():
            print(f'    "{k}": "{v}",')
        return
    bad = sorted(k for k in set(want) | set(got) if want.get(k) != got.get(k))
    check(not bad, f"{name}: {len(got)} encode/decode hashes bit-exact" + (f"; differ: {', '.join(bad[:8])}" if bad else ""))


def test_lc3(tmp, flags):
    srcs, inc = lc3_srcs("liblc3")
    flt = build(tmp / "lc3_liblc3", [HERE / "lc3_tool.c", *srcs], LIBLC3 + flags, inc)
    # Without -ffast-math: the exact float reference lc3fx's decoder is held to.
    exact = build(tmp / "lc3_liblc3_exact", [HERE / "lc3_tool.c", *srcs], [f for f in LIBLC3 if f != "-ffast-math"] + flags, inc)
    srcs, inc = lc3_srcs("lc3fx")
    fx = build(tmp / "lc3_lc3fx", [HERE / "lc3_tool.c", *srcs], LC3FX + flags, inc)

    for name, exe in (("liblc3", flt), ("lc3fx", fx)):
        out = run(exe, "refuse")
        setups = {m[0]: m[1] + m[2] for m in re.findall(r"setup (\w+) enc (\d) dec (\d)", out)}
        check(setups == {"8000": "00", "16000": "11", "24000": "11", "32000": "00", "48000": "00", "7500us": "00"},
              f"{name}: sets up 10 ms at 16 and 24 kHz only ({setups})")
        check("encode bytes 19 -1 401 -1" in out and "decode bytes 19 -1 401 -1" in out,
              f"{name}: refuses frames under 20 or over 400 bytes")
        check("format 4 enc -1 dec -1" in out, f"{name}: refuses an unknown PCM format")

    # lc3fx has no high-resolution mode: an HR request is refused, not served as
    # standard LC3, also where the masks let 48 kHz through.
    srcs, inc = lc3_srcs("lc3fx")
    fx_all = build(tmp / "lc3_lc3fx_all", [HERE / "lc3_tool.c", *srcs],
                   [f for f in LC3FX if "_MASK=" not in f] + flags, inc)
    for name, exe, std48 in (("lc3fx", fx, "00"), ("lc3fx, every rate", fx_all, "11")):
        out = run(exe, "refuse")
        hr = re.findall(r"hr (\d+) enc (\d) dec (\d) samples (-?\d+) bytes (-?\d+)", out)
        check(re.search(rf"setup 48000 enc {std48[0]} dec {std48[1]}", out) is not None and
              hr == [("16000", "0", "0", "-1", "-1"), ("48000", "0", "0", "-1", "-1")],
              f"{name}: refuses a high-resolution setup and its sizes ({hr})")

    for rate in (16000, 24000):
        pcm = voice(rate)
        raw = tmp / f"in{rate}.raw"
        raw.write_bytes(pcm.tobytes())
        delay = int(run(flt, "delay", rate))
        streams = {}
        for name, exe in (("liblc3", flt), ("lc3fx", fx)):
            streams[name] = tmp / f"{name}{rate}.lc3"
            run(exe, "enc", rate, 40, raw, streams[name])
        outs = {}
        for src in streams:
            for name, exe in (("liblc3", flt), ("lc3fx", fx)):
                o = tmp / f"{src}-{name}{rate}.raw"
                run(exe, "dec", rate, 40, streams[src], o)
                outs[src, name] = read_pcm(o)
                s = snr(pcm, outs[src, name], delay)
                check(s > 12.0, f"{rate} Hz, 32 kb/s: {src}'s stream decoded by {name}, SNR {s:.1f} dB")
        for src in streams:
            o = tmp / f"{src}-exact{rate}.raw"
            run(exact, "dec", rate, 40, streams[src], o)
            a, b = read_pcm(o), outs[src, "lc3fx"]
            diff = max(abs(x - y) for x, y in zip(a, b))
            check(diff <= 2, f"{rate} Hz: lc3fx decodes {src}'s stream within {diff} LSB of float liblc3")
        o = tmp / f"loss{rate}.raw"
        run(fx, "dec", rate, 40, streams["lc3fx"], o, 5)
        s = snr(pcm, read_pcm(o), delay)
        check(s > 3.0, f"{rate} Hz: lc3fx conceals every 5th frame lost, SNR {s:.1f} dB")

    compare_digests("lc3fx", digests(run(fx, "digest")), LC3FX_DIGEST)

    # The postfilter on: a stream the float encoder made with the LTPF on, decoded by lc3fx's
    # postfilter (under ASan/UBSan with --sanitize), clean and with every 5th frame lost.
    srcs, inc = lc3_srcs("liblc3")
    flt_ltpf = build(tmp / "lc3_liblc3_ltpf", [HERE / "lc3_tool.c", *srcs], LIBLC3_LTPF + flags, inc)
    srcs, inc = lc3_srcs("lc3fx")
    fx_ltpf = build(tmp / "lc3_lc3fx_ltpf", [HERE / "lc3_tool.c", *srcs], LC3FX_LTPF + flags, inc)
    for rate in (16000, 24000):
        pcm = voice(rate)
        raw = tmp / f"in{rate}.raw"
        raw.write_bytes(pcm.tobytes())
        delay = int(run(flt_ltpf, "delay", rate))
        stream = tmp / f"ltpf{rate}.lc3"
        run(flt_ltpf, "enc", rate, 40, raw, stream)
        for loss, floor in ((0, 12.0), (5, 3.0)):
            o = tmp / f"ltpf{rate}-{loss}.raw"
            run(fx_ltpf, "dec", rate, 40, stream, o, *([loss] if loss else []))
            s = snr(pcm, read_pcm(o), delay)
            check(s > floor, f"{rate} Hz: lc3fx with the postfilter on decodes an LTPF stream"
                  f"{f', every {loss}th frame lost' if loss else ''}, SNR {s:.1f} dB")
        # The postfilter ran: the same stream without it decodes to other samples.
        o = tmp / f"ltpf{rate}-off.raw"
        run(fx, "dec", rate, 40, stream, o)
        check(read_pcm(o) != read_pcm(tmp / f"ltpf{rate}-0.raw"),
              f"{rate} Hz: the stream's LTPF is on (lc3fx's postfilter changes the output)")

    # A mask beyond the trimmed tables stops the build (src/tables_mask.h).
    for mask in ("-DLC3_SR_MASK=0x16", "-DLC3_DT_MASK=0xc"):
        r = subprocess.run([*cc(), "-std=c11", "-c", mask, *inc_of("liblc3"), str(XPLAT / "liblc3/src/lc3.c"),
                            "-o", str(tmp / "mask.o")], capture_output=True, text=True)
        check(r.returncode != 0 and "trim_tables.py" in r.stderr, f"liblc3: {mask} doesn't compile")


def inc_of(lib):
    return ["-I", str(XPLAT / lib / "include"), "-I", str(XPLAT / lib / "src")]


def test_sbc(tmp, flags):
    srcs, inc = sbc_srcs()
    sdk = build(tmp / "sbc_sdk", [HERE / "sbc_tool.c", *srcs], LIBSBC + flags, inc)
    full = build(tmp / "sbc_full", [HERE / "sbc_tool.c", *srcs], flags, inc)

    pcm = voice(16000)
    raw = tmp / "sbc_in.raw"
    raw.write_bytes(pcm.tobytes())
    o = tmp / "sbc_out.raw"
    run(sdk, "roundtrip", 16, 26, raw, o)
    s = snr(pcm, read_pcm(o), 73)
    check(s > 20.0, f"SBC 16 blocks, bitpool 26: SNR {s:.1f} dB")
    o = tmp / "msbc_out.raw"
    run(sdk, "msbc", raw, o)
    s = snr(pcm, read_pcm(o), 73)
    check(s > 20.0, f"mSBC: SNR {s:.1f} dB")

    out = run(sdk, "refuse")
    for line in ("conceal before a good frame -1", "frequency out of range size 0 encode -1",
                 "mode out of range size 0", "allocation out of range size 0",
                 "joint stereo frame -1", "4-subband frame -1", "short frame -1"):
        check(line in out, f"SBC (mono, 8 subbands): {line}")
    out = run(full, "refuse")
    for line in ("joint stereo frame 0", "4-subband frame 0"):
        check(line in out, f"SBC (full build): {line}")
    out = run(full, "stereo")
    for line in ("dual channel encode 0 decode 0 channels 2", "stereo conceal without a right buffer -1",
                 "stereo conceal 0 right written 1", "mono after stereo 0 0 conceal 0"):
        check(line in out, f"SBC (full build): {line}")

    a, b = digests(run(sdk, "digest")), digests(run(full, "digest"))
    check(a == b, "SBC: the SDKs' trimmed build and the full build give the same bits")
    compare_digests("libsbc", a, SBC_DIGEST)


def test_rv_xpulp(tmp, flags):
    exe = build(tmp / "rv_xpulp", [HERE / "test_rv_xpulp.c"], [*flags, "-Wextra"],
                ["-I", str(XPLAT / "include")])
    r = subprocess.run([str(exe)], capture_output=True, text=True)
    check(r.returncode == 0, f"{r.stdout.strip().splitlines()[-1] if r.stdout.strip() else 'rv_xpulp: no output'}")
    if r.returncode:
        print(r.stdout[-4000:])


def fuzz(tmp, seconds):
    ssrcs, sinc = sbc_srcs()
    san = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all"]
    probe = subprocess.run([*cc(), "-fsanitize=fuzzer", "-x", "c", "-", "-o", str(tmp / "probe")],
                           input="#include <stdint.h>\n#include <stddef.h>\n"
                                 "int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n) { return 0; }\n",
                           capture_output=True, text=True)
    # The same target twice: with lc3fx, and with float liblc3 (chips with an FPU).
    for lib, lflags in (("lc3fx", LC3FX), ("lc3fx_ltpf", LC3FX_LTPF), ("liblc3", LIBLC3)):
        srcs, inc = lc3_srcs(lib.split("_")[0])
        common = ["-g", "-O1", "-fno-omit-frame-pointer", "-DLC3_HOT=", *lflags, *LIBSBC, *inc, *sinc]
        if probe.returncode == 0:
            exe = build(tmp / f"fuzz_{lib}", [HERE / "fuzz_decode.c", *srcs, *ssrcs],
                        [*common, "-fsanitize=fuzzer,address,undefined", "-fno-sanitize-recover=all"], [])
            corpus = tmp / f"corpus_{lib}"
            corpus.mkdir()
            r = subprocess.run([str(exe), f"-max_total_time={seconds}", "-max_len=1601", "-print_final_stats=1",
                                str(corpus)], capture_output=True, text=True)
            tail = [line for line in r.stderr.splitlines() if "stat::number_of_executed_units" in line]
            check(r.returncode == 0, f"libFuzzer {seconds} s on the {lib} and libsbc decoders"
                  + (f" ({tail[0].split(':')[-1].strip()} inputs)" if tail else ""))
        else:
            exe = build(tmp / f"fuzz_{lib}", [HERE / "fuzz_decode.c", *srcs, *ssrcs],
                        [*common, *san, "-DFUZZ_STANDALONE"], [])
            r = subprocess.run([str(exe), str(seconds)], capture_output=True, text=True)
            check(r.returncode == 0, f"random-frame loop {seconds} s on the {lib} and libsbc decoders "
                  f"(no libFuzzer here): {r.stdout.strip()}")
        if r.returncode:
            print(r.stderr[-4000:])


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--sanitize", action="store_true", help="build with ASan and UBSan")
    ap.add_argument("--fuzz", type=int, metavar="SECONDS", help="fuzz the decoders instead of the tests")
    a = ap.parse_args()
    # An empty LC3_HOT keeps the sanitizers on in the lc3 hot functions (lc3fx/src/common.h).
    flags = ["-g", "-O1", "-fno-omit-frame-pointer", "-fsanitize=address,undefined",
             "-fno-sanitize-recover=all", "-DLC3_HOT="] if a.sanitize else ["-O2"]
    with tempfile.TemporaryDirectory(prefix="xplat-tests-") as name:
        tmp = Path(name)
        if a.fuzz:
            fuzz(tmp, a.fuzz)
        else:
            test_lc3(tmp, flags)
            test_sbc(tmp, flags)
            test_rv_xpulp(tmp, flags)
    print(f"{len(failures)} failed" if failures else "all passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
