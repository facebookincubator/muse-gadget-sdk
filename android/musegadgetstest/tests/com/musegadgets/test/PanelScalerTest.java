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
package com.musegadgets.test;

/** PanelScaler from the smallest panel a report can give (16x16: the chroma's 8): unity gain, no lone tap. */
public final class PanelScalerTest {
    private static int failed;

    private static void check(boolean ok, String what) {
        if (!ok) {
            failed++;
            System.out.println("FAIL: " + what);
        }
    }

    public static void main(String[] args) {
        int[][] cases = {{720, 240, 16}, {360, 120, 8}, {480, 232, 16}, {240, 120, 8}, {720, 240, 360}};
        for (int[] k : cases) {
            int nIn = k[0], nOut = k[1], panel = k[2];
            String what = nIn + " -> " + nOut + " (panel " + panel + ")";
            PanelScaler p = new PanelScaler(nIn, nOut, panel, 0.2, 3);
            boolean sums = true, spread = true;
            for (int o = 0; o < nOut; o++) {
                int s = 0, top = 0;
                for (int i = 0; i < p.len; i++) {
                    s += p.taps[o * p.len + i];
                    top = Math.max(top, Math.abs(p.taps[o * p.len + i]));
                }
                sums &= s == 4096;
                spread &= top < 4096;   /* a lone 4096 tap: the weights were 0/0 */
            }
            check(sums, what + ": a row's taps don't sum to 4096");
            check(spread, what + ": an output took one source pixel (no weights)");
            /* A flat picture stays flat. */
            byte[] src = new byte[nIn * nIn], dst = new byte[nOut * nOut];
            java.util.Arrays.fill(src, (byte) 100);
            p.scale(src, nIn, 1, 0, 0, dst);
            boolean flat = true;
            for (byte b : dst) {
                flat &= Math.abs((b & 0xff) - 100) <= 1;
            }
            check(flat, what + ": a flat picture changed");
        }
        if (failed > 0) {
            System.out.println("PanelScalerTest: " + failed + " failed");
            System.exit(1);
        }
        System.out.println("PanelScalerTest: ok");
    }
}
