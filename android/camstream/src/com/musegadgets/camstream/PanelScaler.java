/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
package com.musegadgets.camstream;

/**
 * Downscaling that knows how the picture will be shown: the gadget's GPU
 * stretches it to the panel with a bilinear filter, which blurs. Instead of a
 * plain box (area) filter, each output pixel is the least-squares best
 * weighting of the source pixels near it for what the panel ends up showing:
 *
 *   D = argmin || U D x - A x ||^2 + lambda || D ||^2   (over D, per row)
 *     = (U'U + lambda I)^-1 U'A
 *
 * with A the area filter from the source to the panel (what a perfect
 * display of the source would show) and U the bilinear stretch from the
 * encoded size to the panel. D comes out as a sharpening filter (negative
 * side lobes), cut to +-6 source pixels and normalized. lambda keeps the
 * sharpening from amplifying what the encoder then has to code. Measured
 * (x264 at 245 kb/s, 4 clips, PSNR at the panel inside the circle): +0.2 to
 * +0.7 dB over the area filter, for the same bitrate.
 *
 * Separable: rows, then columns, in the camera's orientation; the small
 * result is then turned upright (the filter is symmetric, so the order does
 * not matter).
 */
final class PanelScaler {
    private final int nIn, nOut;
    private final int[] start;     /* first source tap of each output */
    private final int[] taps;      /* nOut x len weights, Q12 */
    private final int len;
    private int[] tmp, acc;

    PanelScaler(int nIn, int nOut, int panel, double lambda, int halfTaps) {
        this.nIn = nIn;
        this.nOut = nOut;
        double[][] a = area(panel, nIn), u = bilinear(panel, nOut);
        /* M = U'U + lambda I (nOut x nOut), R = U'A (nOut x nIn) */
        double[][] m = new double[nOut][nOut], r = new double[nOut][nIn];
        /* U and A are sparse (2 and ~3 taps a row): accumulate row by row */
        for (int k = 0; k < panel; k++) {
            for (int i = 0; i < nOut; i++) {
                double ui = u[k][i];
                if (ui == 0) {
                    continue;
                }
                for (int j = 0; j < nOut; j++) {
                    if (u[k][j] != 0) {
                        m[i][j] += ui * u[k][j];
                    }
                }
                for (int j = 0; j < nIn; j++) {
                    if (a[k][j] != 0) {
                        r[i][j] += ui * a[k][j];
                    }
                }
            }
        }
        for (int i = 0; i < nOut; i++) {
            m[i][i] += lambda;
        }
        solve(m, r);   /* r <- M^-1 R = D */
        len = 2 * halfTaps;
        start = new int[nOut];
        taps = new int[nOut * len];
        for (int o = 0; o < nOut; o++) {
            double c = (o + 0.5) * nIn / nOut;
            int lo = Math.max(0, (int) Math.floor(c - halfTaps)), hi = Math.min(nIn, lo + len);
            lo = Math.max(0, hi - len);
            double sum = 0;
            for (int i = lo; i < hi; i++) {
                sum += r[o][i];
            }
            start[o] = lo;
            int acc = 0, big = lo;
            for (int i = lo; i < hi; i++) {
                int w = (int) Math.round(r[o][i] / sum * 4096);
                taps[o * len + i - lo] = w;
                acc += w;
                if (r[o][i] > r[o][big]) {
                    big = i;
                }
            }
            taps[o * len + big - lo] += 4096 - acc;   /* exact unity gain */
        }
    }

    /**
     * src: the square's top-left at (x0, y0), rowStride / pixStride as in
     * the camera's planes; dst: nOut x nOut, in the source's orientation.
     */
    void scale(byte[] src, int rowStride, int pixStride, int x0, int y0, byte[] dst) {
        if (X264.available()) {
            X264.scale(src, rowStride, pixStride, x0, y0, nIn, nOut, start, taps, len, dst);
            return;
        }
        if (tmp == null) {
            tmp = new int[nIn * nOut];
            acc = new int[nOut];
        }
        int[] t = tmp, a = acc, w = taps;
        final int n = len;
        /* rows: nIn source rows -> nOut values each, Q6 */
        for (int y = 0; y < nIn; y++) {
            int rowBase = (y0 + y) * rowStride + x0 * pixStride;
            int to = y * nOut;
            if (pixStride == 1) {
                for (int o = 0, wo = 0; o < nOut; o++, wo += n) {
                    int s = rowBase + start[o], v = 0;
                    for (int k = 0; k < n; k++) {
                        v += (src[s + k] & 0xff) * w[wo + k];
                    }
                    t[to + o] = (v + 32) >> 6;
                }
            } else {
                for (int o = 0, wo = 0; o < nOut; o++, wo += n) {
                    int s = rowBase + start[o] * pixStride, v = 0;
                    for (int k = 0; k < n; k++, s += pixStride) {
                        v += (src[s] & 0xff) * w[wo + k];
                    }
                    t[to + o] = (v + 32) >> 6;
                }
            }
        }
        /* columns, a whole output row at a time (sequential reads), Q18 -> 8 bits */
        for (int o = 0, wo = 0; o < nOut; o++, wo += n) {
            int r0 = start[o] * nOut;
            int c0 = w[wo];
            for (int x = 0; x < nOut; x++) {
                a[x] = t[r0 + x] * c0;
            }
            for (int k = 1; k < n; k++) {
                int r = r0 + k * nOut, c = w[wo + k];
                for (int x = 0; x < nOut; x++) {
                    a[x] += t[r + x] * c;
                }
            }
            int d = o * nOut;
            for (int x = 0; x < nOut; x++) {
                int v = (a[x] + (1 << 17)) >> 18;
                dst[d + x] = (byte) (v < 0 ? 0 : v > 255 ? 255 : v);
            }
        }
    }

    private static double[][] area(int nOut, int nIn) {
        double[][] m = new double[nOut][nIn];
        double r = (double) nIn / nOut;
        for (int o = 0; o < nOut; o++) {
            double a = o * r, b = (o + 1) * r, s = 0;
            for (int i = (int) a; i < Math.min(nIn, (int) Math.ceil(b)); i++) {
                m[o][i] = Math.min(b, i + 1) - Math.max(a, i);
                s += m[o][i];
            }
            for (int i = 0; i < nIn; i++) {
                m[o][i] /= s;
            }
        }
        return m;
    }

    private static double[][] bilinear(int nOut, int nIn) {
        double[][] m = new double[nOut][nIn];
        for (int o = 0; o < nOut; o++) {
            double x = (o + 0.5) * nIn / nOut - 0.5;
            int i = (int) Math.floor(x);
            double f = x - i;
            m[o][Math.min(Math.max(i, 0), nIn - 1)] += 1 - f;
            m[o][Math.min(Math.max(i + 1, 0), nIn - 1)] += f;
        }
        return m;
    }

    /* Gaussian elimination with partial pivoting: r <- m^-1 r (m is destroyed). */
    private static void solve(double[][] m, double[][] r) {
        int n = m.length;
        for (int c = 0; c < n; c++) {
            int p = c;
            for (int i = c + 1; i < n; i++) {
                if (Math.abs(m[i][c]) > Math.abs(m[p][c])) {
                    p = i;
                }
            }
            double[] t = m[c];
            m[c] = m[p];
            m[p] = t;
            t = r[c];
            r[c] = r[p];
            r[p] = t;
            for (int i = c + 1; i < n; i++) {
                double f = m[i][c] / m[c][c];
                if (f == 0) {
                    continue;
                }
                for (int j = c; j < n; j++) {
                    m[i][j] -= f * m[c][j];
                }
                double[] ri = r[i], rc = r[c];
                for (int j = 0; j < ri.length; j++) {
                    ri[j] -= f * rc[j];
                }
            }
        }
        for (int c = n - 1; c >= 0; c--) {
            double[] rc = r[c];
            for (int j = 0; j < rc.length; j++) {
                rc[j] /= m[c][c];
            }
            for (int i = 0; i < c; i++) {
                double f = m[i][c];
                if (f != 0) {
                    double[] ri = r[i];
                    for (int j = 0; j < ri.length; j++) {
                        ri[j] -= f * rc[j];
                    }
                }
            }
        }
    }
}
