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
"""Makes the VP8 test streams here (tests/test_libvpx_vp8.py) and md5.txt.

The picture is synthetic, so the streams carry no one's footage: a panning
texture of rings and stripes, a box crossing it at sub-pixel speed, and a
noisy patch, 240x240, 20 frames. Each stream is encoded with libvpx's
vpxenc in a way that exercises one part of the decoder; md5.txt is the
output of stock vpxdec (libvpx v1.16.0) for each.

  VPXENC=/path/to/vpxenc VPXDEC=/path/to/vpxdec python3 make_vectors.py
"""
import math
import os
import random
import subprocess
import tempfile

W = H = 240
FRAMES = 20
HERE = os.path.dirname(os.path.abspath(__file__))
ENC = os.environ.get("VPXENC", "vpxenc")
DEC = os.environ.get("VPXDEC", "vpxdec")

# name: vpxenc options (besides size, rate and frame count)
STREAMS = {
    "p0_altref": "--profile=0 --good --passes=2 --cpu-used=1 --lag-in-frames=16 --auto-alt-ref=1 --target-bitrate=300",
    "p0_rt_normallf": "--profile=0 --rt --cpu-used=6 --lag-in-frames=0 --end-usage=cbr --target-bitrate=250",
    "p0_keyframes": "--profile=0 --good --cpu-used=2 --kf-min-dist=4 --kf-max-dist=4 --target-bitrate=400",
    "p1_good": "--profile=1 --good --cpu-used=1 --lag-in-frames=0 --target-bitrate=300 --sharpness=3",
    "p1_static": "--profile=1 --good --cpu-used=2 --lag-in-frames=0 --static-thresh=500 --target-bitrate=150",
    "p2_4parts_er": "--profile=2 --good --cpu-used=2 --token-parts=2 --error-resilient=1 --target-bitrate=350",
    "p3_fullpel": "--profile=3 --good --cpu-used=2 --lag-in-frames=0 --target-bitrate=300",
}


def frame(t, rnd):
    y = bytearray(W * H)
    ox, oy = 1.7 * t, 0.6 * t                      # the pan, in pixels
    bx, by = 20 + 6.25 * t, 140 - 3.5 * t          # the box, sub-pixel steps
    for j in range(H):
        for i in range(W):
            x, yy = i + ox, j + oy
            r = math.hypot(x - 150, yy - 90)
            v = 128 + 60 * math.sin(r / 5.0) + 30 * math.sin(x / 3.1 + yy / 7.3)
            if bx <= i < bx + 48 and by <= j < by + 40:
                v = 40 + 4 * ((i - int(bx)) % 8) + 3 * ((j - int(by)) % 9)
            if 160 <= i < 220 and 160 <= j < 220:
                v = 128 + rnd.randint(-50, 50)
            y[j * W + i] = max(16, min(235, int(v)))
    u = bytearray(W // 2 * H // 2)
    v = bytearray(W // 2 * H // 2)
    for j in range(H // 2):
        for i in range(W // 2):
            u[j * (W // 2) + i] = 128 + int(40 * math.sin((i + ox / 2) / 9.0))
            v[j * (W // 2) + i] = 128 + int(40 * math.cos((j + oy / 2) / 11.0))
    return bytes(y) + bytes(u) + bytes(v)


def main():
    rnd = random.Random(7)
    with tempfile.TemporaryDirectory() as tmp:
        src = os.path.join(tmp, "src.yuv")
        with open(src, "wb") as f:
            for t in range(FRAMES):
                f.write(frame(t, rnd))
        lines = []
        for name, opts in STREAMS.items():
            out = os.path.join(HERE, name + ".ivf")
            subprocess.run([ENC, "--codec=vp8", "-q", f"--width={W}", f"--height={H}", "--i420", "--fps=30/1",
                            f"--limit={FRAMES}", "--threads=1", *opts.split(), "--ivf", "-o", out, src], check=True)
            md5 = subprocess.run([DEC, "--md5", "--i420", out], check=True, capture_output=True, text=True)
            lines.append(f"{name}.ivf {md5.stdout.split()[0]}")
        with open(os.path.join(HERE, "md5.txt"), "w") as f:
            f.write("\n".join(lines) + "\n")


if __name__ == "__main__":
    main()
