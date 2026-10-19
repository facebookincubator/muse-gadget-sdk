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

"""score.py BENCHDIR CLIP.yuv W : what the gadget shows, scored at its panel.

The reference is the source clip itself (640x480 I420): its center 480x480,
area-scaled to the panel's size (PANEL, default 2 W: a picture shown two
times upscaled), inside the panel's circle. The test is each decoded frame
(the gadget's tinyh264, host build: tinyhost, built by build.sh)
bilinear-scaled from W to the panel, shown from its source frame until the
next decoded frame (skipped frames repeat the last one). Prints luma PSNR
(from the mean MSE), SSIM (Gaussian 11x11, sigma 1.5, mean over the circle),
the bitrate, and the frames encoded and skipped. Both are compared at the
panel, so runs at different encode sizes can be compared."""
import os, struct, subprocess, sys
import numpy as np

T = os.path.dirname(os.path.abspath(__file__))   # tinyhost (build.sh)
d, clip, W = sys.argv[1], sys.argv[2], int(sys.argv[3])
FW, FH, fps = 640, 480, 30.0
S = int(os.environ.get('PANEL', 2 * W))


def area(n_out, n_in):
    m = np.zeros((n_out, n_in))
    r = n_in / n_out
    for o in range(n_out):
        a, b = o * r, (o + 1) * r
        for i in range(int(a), min(n_in, int(np.ceil(b)))):
            m[o, i] = min(b, i + 1) - max(a, i)
    return m / m.sum(1, keepdims=True)


def bilinear(n_out, n_in):
    m = np.zeros((n_out, n_in))
    for o in range(n_out):
        x = (o + 0.5) * n_in / n_out - 0.5
        i = int(np.floor(x)); f = x - i
        m[o, min(max(i, 0), n_in - 1)] += 1 - f
        m[o, min(max(i + 1, 0), n_in - 1)] += f
    return m


def gauss(n, sigma=1.5, r=5):
    k = np.exp(-np.arange(-r, r + 1) ** 2 / (2 * sigma * sigma)); k /= k.sum()
    m = np.zeros((n, n))
    for i in range(n):
        for j, w in zip(range(i - r, i + r + 1), k):
            m[i, min(max(j, 0), n - 1)] += w
    return m


enc = open(os.path.join(d, 'enc.bin'), 'rb').read()
src, aus, p, nbytes = [], [], 0, 0
while p + 8 <= len(enc):
    n, i = struct.unpack_from('<II', enc, p); p += 8
    aus.append(enc[p:p + n]); src.append(i); p += n; nbytes += n + 8
with open('/tmp/_s.au', 'wb') as f:
    for a in aus:
        f.write(struct.pack('<I', len(a))); f.write(a)
subprocess.run([T + '/tinyhost', '/tmp/_s.au', '/tmp/_s_dec.yuv', '1', str(W), str(W)], check=True, capture_output=True)
fs = W * W * 3 // 2
dec = np.fromfile('/tmp/_s_dec.yuv', np.uint8)
nd = dec.size // fs
assert nd == len(src), (nd, len(src))
decY = [dec[k * fs:k * fs + W * W].reshape(W, W).astype(np.float64) for k in range(nd)]

raw = np.memmap(clip, np.uint8, 'r')
nsrc = raw.size // (FW * FH * 3 // 2)
nruns = max(src) + 1
A = area(S, 480); U = bilinear(S, W); G = gauss(S)
yy, xx = np.mgrid[0:S, 0:S]
circ = (xx + 0.5 - S / 2) ** 2 + (yy + 0.5 - S / 2) ** 2 <= (S / 2 - 1) ** 2
C1, C2 = (0.01 * 255) ** 2, (0.03 * 255) ** 2


def ref_panel(i):
    o = (i % nsrc) * (FW * FH * 3 // 2)
    y = np.asarray(raw[o:o + FW * FH]).reshape(FH, FW)[:, 80:560].astype(np.float64)
    return A @ y @ A.T


def ssim(a, b):
    ma, mb = G @ a @ G.T, G @ b @ G.T
    va = G @ (a * a) @ G.T - ma * ma; vb = G @ (b * b) @ G.T - mb * mb; cv = G @ (a * b) @ G.T - ma * mb
    s = ((2 * ma * mb + C1) * (2 * cv + C2)) / ((ma * ma + mb * mb + C1) * (va + vb + C2))
    return s[circ].mean()


up = {}
se, ss, n, j = 0.0, 0.0, 0, 0
per = []
per_mse = []
for i in range(src[0], nruns):
    while j + 1 < nd and src[j + 1] <= i:
        j += 1
    if j not in up:
        up[j] = U @ decY[j] @ U.T
    r = ref_panel(i); t = up[j]
    mse = ((r - t)[circ] ** 2).mean()
    se += mse; s1 = ssim(r, t); ss += s1; n += 1
    per_mse.append(mse)
    per.append(10 * np.log10(255 * 255 / mse))
    if len(up) > 4:
        for k in list(up):
            if k < j - 2:
                del up[k]
psnr = 10 * np.log10(255 * 255 / (se / n))
# steady state: from 1 s in (the first keyframe's transfer is a startup transient)
per_mse = np.array(per_mse)
ss_from = int(fps)
ss_mse = per_mse[ss_from:].mean() if len(per_mse) > ss_from else float('nan')
# coded: each decoded frame against its own source frame, at the panel (no repeats)
cse = 0.0
for k in range(nd):
    r = ref_panel(src[k]); t = U @ decY[k] @ U.T
    cse += ((r - t)[circ] ** 2).mean()
coded_psnr = 10 * np.log10(255 * 255 / (cse / nd))
kbps = nbytes * 8 / (nruns / fps) / 1000
per = np.array(per)
print('{"frames": %d, "encoded": %d, "skipped": %d, "kbps": %.1f, "psnr_panel": %.3f, "ssim_panel": %.4f, "mse": %.2f, "mse_ss": %.2f, "coded_panel": %.3f}'
      % (n, nd, n - nd, kbps, psnr, ss / n, se / n, ss_mse, coded_psnr))
