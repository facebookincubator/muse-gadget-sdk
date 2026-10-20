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
import Foundation

/*
 * Downscaling that knows how the picture will be shown (Android's
 * PanelScaler, the same filter and integer arithmetic): the gadget stretches
 * the picture to its panel with a bilinear filter, which blurs. Each output
 * pixel is the least-squares best weighting of the source pixels near it
 * for what the panel ends up showing,
 *
 *   D = argmin || U D x - A x ||^2 + lambda || D ||^2 = (U'U + lambda I)^-1 U'A
 *
 * with A the area filter from the source to the panel and U the bilinear
 * stretch from the encoded size to the panel: a sharpening filter, cut to
 * +-halfTaps source pixels and normalized. +0.2 to +0.7 dB at the panel over
 * the area filter for the same bitrate. Separable; the loops are in C
 * (native/mgscale.c).
 */
final class PanelScaler {
    let nIn: Int, nOut: Int
    private var start: [Int32]
    private var taps: [Int32]
    private let len: Int
    private var tmp: [Int32]

    init(nIn: Int, nOut: Int, panel: Int, lambda: Double, halfTaps: Int) {
        self.nIn = nIn
        self.nOut = nOut
        let a = Self.area(panel, nIn), u = Self.bilinear(panel, nOut)
        var m = [[Double]](repeating: [Double](repeating: 0, count: nOut), count: nOut)
        var r = [[Double]](repeating: [Double](repeating: 0, count: nIn), count: nOut)
        for k in 0..<panel {
            let uk = u[k], ak = a[k]
            let ui = uk.indices.filter { uk[$0] != 0 }, ai = ak.indices.filter { ak[$0] != 0 }
            for i in ui {
                for j in ui {
                    m[i][j] += uk[i] * uk[j]
                }
                for j in ai {
                    r[i][j] += uk[i] * ak[j]
                }
            }
        }
        for i in 0..<nOut {
            m[i][i] += lambda
        }
        Self.solve(&m, &r)
        len = 2 * halfTaps
        start = [Int32](repeating: 0, count: nOut)
        taps = [Int32](repeating: 0, count: nOut * len)
        tmp = [Int32](repeating: 0, count: nIn * nOut)
        for o in 0..<nOut {
            let c = (Double(o) + 0.5) * Double(nIn) / Double(nOut)
            var lo = max(0, Int((c - Double(halfTaps)).rounded(.down)))
            let hi = min(nIn, lo + len)
            lo = max(0, hi - len)
            var sum = 0.0
            for i in lo..<hi {
                sum += r[o][i]
            }
            start[o] = Int32(lo)
            var acc: Int32 = 0
            var big = lo
            for i in lo..<hi {
                let w = Int32((r[o][i] / sum * 4096).rounded())
                taps[o * len + i - lo] = w
                acc += w
                if r[o][i] > r[o][big] {
                    big = i
                }
            }
            taps[o * len + big - lo] += 4096 - acc   /* exact unity gain */
        }
    }

    /* src: the square's top-left at (x0, y0), with its strides; dst: nOut x nOut. */
    func scale(_ src: UnsafePointer<UInt8>, rowStride: Int, pixStride: Int, x0: Int, y0: Int,
               _ dst: UnsafeMutablePointer<UInt8>) {
        start.withUnsafeBufferPointer { s in
            taps.withUnsafeBufferPointer { t in
                tmp.withUnsafeMutableBufferPointer { m in
                    mg_panel_scale(src, Int32(rowStride), Int32(pixStride), Int32(x0), Int32(y0), Int32(nIn),
                                   Int32(nOut), s.baseAddress, t.baseAddress, Int32(len), m.baseAddress, dst)
                }
            }
        }
    }

    private static func area(_ nOut: Int, _ nIn: Int) -> [[Double]] {
        var m = [[Double]](repeating: [Double](repeating: 0, count: nIn), count: nOut)
        let r = Double(nIn) / Double(nOut)
        for o in 0..<nOut {
            let a = Double(o) * r, b = Double(o + 1) * r
            var s = 0.0
            var i = Int(a)
            while i < min(nIn, Int(b.rounded(.up))) {
                m[o][i] = min(b, Double(i + 1)) - max(a, Double(i))
                s += m[o][i]
                i += 1
            }
            for i in 0..<nIn {
                m[o][i] /= s
            }
        }
        return m
    }

    private static func bilinear(_ nOut: Int, _ nIn: Int) -> [[Double]] {
        var m = [[Double]](repeating: [Double](repeating: 0, count: nIn), count: nOut)
        for o in 0..<nOut {
            let x = (Double(o) + 0.5) * Double(nIn) / Double(nOut) - 0.5
            let i = Int(x.rounded(.down))
            let f = x - Double(i)
            m[o][min(max(i, 0), nIn - 1)] += 1 - f
            m[o][min(max(i + 1, 0), nIn - 1)] += f
        }
        return m
    }

    /* Gaussian elimination with partial pivoting: r <- m^-1 r. */
    private static func solve(_ m: inout [[Double]], _ r: inout [[Double]]) {
        let n = m.count
        for c in 0..<n {
            var p = c
            for i in (c + 1)..<max(c + 1, n) where abs(m[i][c]) > abs(m[p][c]) {
                p = i
            }
            m.swapAt(c, p)
            r.swapAt(c, p)
            for i in (c + 1)..<max(c + 1, n) {
                let f = m[i][c] / m[c][c]
                if f == 0 {
                    continue
                }
                for j in c..<n {
                    m[i][j] -= f * m[c][j]
                }
                let rc = r[c]
                r[i].withUnsafeMutableBufferPointer { ri in
                    for j in 0..<ri.count {
                        ri[j] -= f * rc[j]
                    }
                }
            }
        }
        for c in stride(from: n - 1, through: 0, by: -1) {
            let d = m[c][c]
            r[c].withUnsafeMutableBufferPointer { rc in
                for j in 0..<rc.count {
                    rc[j] /= d
                }
            }
            let rc = r[c]
            for i in 0..<c {
                let f = m[i][c]
                if f != 0 {
                    r[i].withUnsafeMutableBufferPointer { ri in
                        for j in 0..<ri.count {
                            ri[j] -= f * rc[j]
                        }
                    }
                }
            }
        }
    }
}
