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
 * Capture -> shown latency, from the gadget's echo of the last frame it drew
 * (its buffer message: frame number and how long ago). Each frame's capture
 * time, when the encoder put it out and when its last byte went to the link
 * are kept by frame number; when the gadget reports a frame drawn, the
 * latency is its draw time minus its capture time.
 */
final class LatencyMeter {
    private static let n = 4096
    private var capNs = [Int64](repeating: 0, count: n), encNs = [Int64](repeating: 0, count: n)
    private var sentNs = [Int64](repeating: 0, count: n)
    private var gen = [Int](repeating: -1, count: n)
    private var lastShown = -1
    private var lat: [Int64] = [], enc: [Int64] = [], queue: [Int64] = [], air: [Int64] = []
    private let lock = NSLock()
    var perFrame = false

    func captured(_ frameNo: Int, captureNs: Int64, encodedNs: Int64) {
        lock.lock()
        defer { lock.unlock() }
        let i = frameNo & (Self.n - 1)
        capNs[i] = captureNs
        encNs[i] = encodedNs
        sentNs[i] = 0
        gen[i] = frameNo & 0xffff
    }

    func sent(_ frameNo: Int, ns: Int64) {
        lock.lock()
        defer { lock.unlock() }
        let i = frameNo & (Self.n - 1)
        if gen[i] == frameNo & 0xffff {
            sentNs[i] = ns
        }
    }

    func shown(_ num: Int, ageMs: Int, rxNs: Int64) {
        lock.lock()
        defer { lock.unlock() }
        if ageMs == 0xffff || num == lastShown {
            return
        }
        lastShown = num
        let i = num & (Self.n - 1)
        if gen[i] != num || capNs[i] == 0 || lat.count >= 512 {
            return
        }
        let drawn = rxNs - Int64(ageMs) * 1_000_000
        lat.append((drawn - capNs[i]) / 1000)
        enc.append((encNs[i] - capNs[i]) / 1000)
        queue.append(sentNs[i] > 0 ? (sentNs[i] - encNs[i]) / 1000 : -1)
        air.append(sentNs[i] > 0 ? (drawn - sentNs[i]) / 1000 : -1)
        if perFrame {
            mlog("lf \(num) \(lat.last! / 1000) \(enc.last! / 1000) \(queue.last! / 1000) \(air.last! / 1000)")
        }
    }

    private static func pct(_ a: [Int64], _ p: Int) -> Int64 {
        let c = a.filter { $0 >= 0 }.sorted()
        if c.isEmpty {
            return -1
        }
        return c[(c.count - 1) * p / 100]
    }

    /* One line of the last interval's numbers (ms), and starts the next. */
    func take() -> String {
        lock.lock()
        defer { lock.unlock() }
        if lat.isEmpty {
            return "latency: no frames reported"
        }
        let s = "latency capture->shown med \(Self.pct(lat, 50) / 1000) p90 \(Self.pct(lat, 90) / 1000) max \(Self.pct(lat, 100) / 1000) ms (n \(lat.count)) | capture->encoded \(Self.pct(enc, 50) / 1000), encoded->sent \(Self.pct(queue, 50) / 1000), sent->shown \(Self.pct(air, 50) / 1000)"
        lat.removeAll()
        enc.removeAll()
        queue.removeAll()
        air.removeAll()
        return s
    }
}
