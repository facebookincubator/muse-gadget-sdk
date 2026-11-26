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

/**
 * The square-to-disc warp of protocols/UI.md (the elliptical grid mapping),
 * on the CPU for touches: UiRenderer's shader does the same per pixel.
 */
final class UiWarp {
    private UiWarp() {}

    private static final double S2 = 2 * Math.sqrt(2);

    /** Disc (u, v in [-1, 1], moved onto the unit circle if outside) to square (x, y in [-1, 1]). */
    static void squareFromDisc(double u, double v, double[] out) {
        double r2 = u * u + v * v;
        if (r2 > 1) {
            double r = Math.sqrt(r2);
            u /= r;
            v /= r;
        }
        double a = 2 + u * u - v * v, b = 2 - u * u + v * v;
        double x = 0.5 * Math.sqrt(Math.max(0, a + S2 * u)) - 0.5 * Math.sqrt(Math.max(0, a - S2 * u));
        double y = 0.5 * Math.sqrt(Math.max(0, b + S2 * v)) - 0.5 * Math.sqrt(Math.max(0, b - S2 * v));
        out[0] = Math.max(-1, Math.min(1, x));
        out[1] = Math.max(-1, Math.min(1, y));
    }

    /** Square to disc (the forward map; for tests and the preview's grid). */
    static void discFromSquare(double x, double y, double[] out) {
        out[0] = x * Math.sqrt(1 - y * y / 2);
        out[1] = y * Math.sqrt(1 - x * x / 2);
    }

    /**
     * A panel pixel (of a w x h panel) to where in the streamed square it
     * came from: out = fractions of the square's width and height from its
     * top-left corner, [0, 1].
     */
    static void panelToSquare(int px, int py, int w, int h, boolean fisheye, double[] out) {
        double u = (2.0 * px + 1) / w - 1, v = (2.0 * py + 1) / h - 1;
        if (fisheye) {
            squareFromDisc(u, v, out);
        } else {
            out[0] = Math.max(-1, Math.min(1, u));
            out[1] = Math.max(-1, Math.min(1, v));
        }
        out[0] = (out[0] + 1) / 2;
        out[1] = (out[1] + 1) / 2;
    }
}
