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

/* PanelScaler from the smallest panel a report can give (16x16: the chroma's 8): no trap, unity gain. */
enum PanelScalerTests {
    static func check(_ ok: Bool, _ what: String, line: Int = #line) {
        Tests.check(ok, "PanelScalerTests: " + what, line: line)
    }

    static func run() {
        for (nIn, nOut, panel) in [(720, 240, 16), (360, 120, 8), (480, 232, 16), (240, 120, 8), (720, 240, 360)] {
            let p = PanelScaler(nIn: nIn, nOut: nOut, panel: panel, lambda: 0.2, halfTaps: 3)
            var rowsOk = true
            for o in 0..<nOut where p.taps[o * p.len..<(o + 1) * p.len].reduce(0, +) != 4096 {
                rowsOk = false
            }
            check(rowsOk, "\(nIn) -> \(nOut) (panel \(panel)): a row's taps don't sum to 4096")
            /* A flat picture stays flat. */
            let src = [UInt8](repeating: 100, count: nIn * nIn)
            var dst = [UInt8](repeating: 0, count: nOut * nOut)
            src.withUnsafeBufferPointer { s in
                dst.withUnsafeMutableBufferPointer { d in
                    p.scale(s.baseAddress!, rowStride: nIn, pixStride: 1, x0: 0, y0: 0, d.baseAddress!)
                }
            }
            check(dst.allSatisfy { abs(Int($0) - 100) <= 1 }, "\(nIn) -> \(nOut) (panel \(panel)): a flat picture changed")
        }
    }
}
