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

"""Decodes the firmware's encoded streams with the upstream reference decoders.

The unit tests (tests/unit, test_codec.c) print what the vendored, optimized
encoders produce as MGSTREAM lines. This script builds small decoders from
upstream Google libsbc and liblc3, decodes those streams, and checks them
against the two-tone input the test encodes for each (PROFILES): an
independent check that the optimized encoders emit standard SBC and LC3.

    zephyr.exe | tee unit.log   # the unit test binary
    python3 xcheck.py unit.log --libsbc DIR --liblc3 DIR

DIR defaults: --liblc3 $ZEPHYR_BASE/../modules/lib/liblc3 (Zephyr's module is
upstream liblc3); --libsbc is cloned from github.com/google/libsbc if missing.
"""

import argparse
import math
import os
import subprocess
import sys
import tempfile

# xplat/libsbc/README.md: the upstream commit it is vendored from.
LIBSBC_COMMIT = "6e505650145c9973d08a0bdd5e5f5e1914305e40"

DEC_C = r"""
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#ifdef USE_SBC
#include <sbc.h>
#else
#include <lc3.h>
#endif
#include <stdlib.h>
int main(int argc, char **argv) {
  FILE *in = fopen(argv[1], "rb"), *out = fopen(argv[2], "wb");
  static uint8_t buf[1 << 20];
  size_t n = fread(buf, 1, sizeof(buf), in), off = 0;
  int16_t pcm[480];
#ifdef USE_SBC
  sbc_t sbc; struct sbc_frame f;
  sbc_reset(&sbc);
  while (off + 4 <= n) {
    if (sbc_probe(&buf[off], &f)) return 2;
    unsigned sz = sbc_get_frame_size(&f);
    if (sbc_decode(&sbc, &buf[off], sz, &f, pcm, 1, NULL, 0)) return 3;
    fwrite(pcm, 2, f.nblocks * f.nsubbands, out);
    off += sz;
  }
#else
  /* argv[3], argv[4]: the sample rate and the frame bytes (10 ms frames). */
  int rate = argc > 4 ? atoi(argv[3]) : 16000, fb = argc > 4 ? atoi(argv[4]) : 40;
  static uint8_t mem[65536]; /* > lc3_decoder_size(10000, 48000) */
  lc3_decoder_t d = lc3_setup_decoder(10000, rate, 0, mem);
  if (!d || fb <= 0) return 4;
  while (off + fb <= n) {
    if (lc3_decode(d, &buf[off], fb, LC3_PCM_FORMAT_S16, pcm, 1) < 0) return 5;
    fwrite(pcm, 2, rate / 100, out);
    off += fb;
  }
#endif
  fclose(out);
  return 0;
}
"""


# What test_codec.c encodes under each MGSTREAM name: the codec, the sample
# rate, the LC3 frame bytes, the second tone (Hz) next to 440 Hz at
# 6000 / 3000, the samples skipped before comparing, the largest lag tried
# and the SNR (dB) the upstream decoder must reach.
PROFILES = {
    "sbc": ("sbc", 16000, 0, 1500.0, 800, 200, 20.0),
    "lc3": ("lc3", 16000, 40, 1500.0, 800, 200, 12.0),
    "lc3-16k": ("lc3", 16000, 20, 1500.0, 800, 200, 6.0),
    "lc3-24k": ("lc3", 24000, 40, 9000.0, 1200, 300, 10.0),
}


def build(kind, src, work):
    c = os.path.join(work, "dec.c")
    exe = os.path.join(work, "dec_" + kind)
    open(c, "w").write(DEC_C)
    if kind == "sbc":
        srcs = [os.path.join(src, "src", f) for f in ("sbc.c", "bits.c")]
        cmd = ["cc", "-O2", "-DUSE_SBC", "-I" + os.path.join(src, "include"), c] + srcs
    else:
        sd = os.path.join(src, "src")
        srcs = [os.path.join(sd, f) for f in sorted(os.listdir(sd)) if f.endswith(".c")]
        cmd = ["cc", "-O2", "-std=c11", "-I" + os.path.join(src, "include"), c] + srcs + ["-lm"]
    subprocess.check_call(cmd + ["-o", exe])
    return exe


def signal(rate, tone2):
    return [int(6000.0 * math.sin(2 * math.pi * 440.0 * i / rate) +
                3000.0 * math.sin(2 * math.pi * tone2 * i / rate)) for i in range(rate)]


def best_snr(ref, out, skip, max_lag):
    best, lag_at = -100.0, 0
    for lag in range(max_lag + 1):
        sig = err = 0.0
        for i in range(skip, min(len(ref), len(out) - lag)):
            d = out[i + lag] - ref[i]
            sig += ref[i] * ref[i]
            err += d * d
        snr = 10 * math.log10(sig / (err + 1e-9))
        if snr > best:
            best, lag_at = snr, lag
    return best, lag_at


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log")
    ap.add_argument("--libsbc")
    ap.add_argument("--liblc3")
    a = ap.parse_args()

    streams = {}
    for line in open(a.log, errors="replace"):
        p = line.split()
        if len(p) == 3 and p[0] == "MGSTREAM":
            streams.setdefault(p[1], bytearray()).extend(bytes.fromhex(p[2]))
    if not streams:
        sys.exit("no MGSTREAM lines in " + a.log)

    work = tempfile.mkdtemp()
    libsbc = a.libsbc or os.path.join(work, "libsbc")
    if not a.libsbc:
        # Google's upstream at the commit xplat/libsbc is vendored from
        # (its README), never whatever HEAD is.
        subprocess.check_call(["git", "clone", "-q", "https://github.com/google/libsbc", libsbc])
        subprocess.check_call(["git", "-C", libsbc, "checkout", "-q", LIBSBC_COMMIT])
    liblc3 = a.liblc3 or os.path.join(os.environ.get("ZEPHYR_BASE", "."), "..", "modules", "lib",
                                      "liblc3")
    ok = True
    exes = {}
    for name, data in sorted(streams.items()):
        if name not in PROFILES:
            print("%s: unknown stream (known: %s): FAIL" % (name, ", ".join(sorted(PROFILES))))
            ok = False
            continue
        codec, rate, frame_bytes, tone2, skip, max_lag, limit = PROFILES[name]
        if codec not in exes:
            exes[codec] = build(codec, libsbc if codec == "sbc" else liblc3, work)
        bin_in, pcm_out = os.path.join(work, name + ".bin"), os.path.join(work, name + ".pcm")
        open(bin_in, "wb").write(data)
        subprocess.check_call([exes[codec], bin_in, pcm_out, str(rate), str(frame_bytes)])
        raw = open(pcm_out, "rb").read()
        out = [int.from_bytes(raw[i:i + 2], "little", signed=True) for i in range(0, len(raw), 2)]
        snr, lag = best_snr(signal(rate, tone2), out, skip, max_lag)
        good = snr > limit
        ok &= good
        print("%s: %d bytes, upstream decoder -> %d samples, SNR %.1f dB at lag %d: %s"
              % (name.upper(), len(data), len(out), snr, lag, "ok" if good else "FAIL"))
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
