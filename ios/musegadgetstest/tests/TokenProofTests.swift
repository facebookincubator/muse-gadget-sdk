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
import CryptoKit
import Foundation

/* The token proof (TokenProof) against protocols/test-vectors/mg-token-proof-v1.json, and its outcomes. */
enum TokenProofTests {
    static func check(_ ok: Bool, _ what: String, line: Int = #line) {
        Tests.check(ok, "TokenProofTests: " + what, line: line)
    }

    static func hex(_ s: String) -> [UInt8] {
        var out: [UInt8] = []
        var i = s.startIndex
        while i < s.endIndex {
            let j = s.index(i, offsetBy: 2)
            out.append(UInt8(s[i..<j], radix: 16)!)
            i = j
        }
        return out
    }

    /* A proof with a fixed client nonce, its sent messages collected. */
    static func make(_ k: [UInt8], _ cn: [UInt8]) -> (TokenProof, () -> [[UInt8]]) {
        var sent: [[UInt8]] = []
        let tp = TokenProof(key: k, send: { sent.append($0) }, nonce: { cn })
        return (tp, { sent })
    }

    static func run(vectors path: String) {
        guard let d = FileManager.default.contents(atPath: path),
              let j = try? JSONSerialization.jsonObject(with: d) as? [String: Any],
              let vs = j["vectors"] as? [[String: Any]], !vs.isEmpty else {
            check(false, "no vectors at \(path)")
            return
        }
        for v in vs {
            let name = v["name"] as? String ?? "?"
            let k = hex(v["K"] as! String), cn = hex(v["client_nonce"] as! String), dn = hex(v["device_nonce"] as! String)
            let f = v["frames"] as! [String: String]
            check(TokenProof.mac(SymmetricKey(data: k), device: true, cn, dn) == hex(v["device_mac"] as! String), "\(name): device MAC")
            check(TokenProof.mac(SymmetricKey(data: k), device: false, cn, dn) == hex(v["client_mac"] as! String), "\(name): client MAC")

            /* The exchange, frame for frame. */
            let (tp, sent) = make(k, cn)
            tp.start(nowMs: 1000)
            check(sent().last == hex(f["challenge"]!), "\(name): challenge frame")
            tp.onControl(hex(f["response"]!), nowMs: 1100)
            check(sent().last == hex(f["confirm"]!), "\(name): confirm frame")
            check(tp.pending, "\(name): done before the result")
            tp.onControl(hex(f["result"]!), nowMs: 1200)
            check(tp.outcome == .match, "\(name): \(String(describing: tp.outcome))")

            /* Another key: the device's MAC doesn't check, and nothing is confirmed. */
            var other = k
            other[0] ^= 1
            let (t2, s2) = make(other, cn)
            t2.start(nowMs: 1000)
            t2.onControl(hex(f["response"]!), nowMs: 1100)
            check(t2.outcome == .mismatch && s2().count == 1, "\(name): another key")
        }

        let v = vs[0], k = hex(v["K"] as! String), cn = hex(v["client_nonce"] as! String)
        let f = v["frames"] as! [String: String]

        /* The device says 0. */
        var (tp, sent) = make(k, cn)
        tp.start(nowMs: 0)
        tp.onControl(hex(f["response"]!), nowMs: 10)
        tp.onControl([0x22, 0x03, 0x00], nowMs: 20)
        check(tp.outcome == .mismatch, "result 0")

        /* A short response. */
        (tp, sent) = make(k, cn)
        tp.start(nowMs: 0)
        tp.onControl(Array(hex(f["response"]!).prefix(20)), nowMs: 10)
        check(tp.outcome == .mismatch, "short response")

        /* No token: error not_found for token_proof. */
        (tp, sent) = make(k, cn)
        tp.start(nowMs: 0)
        tp.onControl([25, 34, 1, 5, 0], nowMs: 10)
        check(tp.outcome == .notFound, "not_found")

        /* Other errors: refused, with the code found past a log entry. */
        (tp, sent) = make(k, cn)
        tp.start(nowMs: 0)
        tp.onControl([25, 34, 2, 3, 0, 0x61, 0x62, 0x63, 1, 8, 0], nowMs: 10)
        check(tp.outcome == .refused(8), "recovery after a log entry: \(String(describing: tp.outcome))")
        (tp, sent) = make(k, cn)
        tp.start(nowMs: 0)
        tp.onControl([25, 34, 2, 1, 0, 0x61, 1, 4, 0], nowMs: 10)
        check(tp.pending, "busy after a log entry")
        check(TokenProof.errorCode([25, 34]) == -1 && TokenProof.errorCode([25, 34, 1, 5]) == -1, "errorCode, short")

        /* busy: a new challenge 200 ms later, then a match. */
        (tp, sent) = make(k, cn)
        tp.start(nowMs: 0)
        tp.onControl([25, 34, 1, 4, 0], nowMs: 100)
        tp.tick(nowMs: 250)
        check(sent().count == 1 && tp.pending, "retried too soon")
        tp.tick(nowMs: 310)
        check(sent().count == 2 && sent()[1] == hex(f["challenge"]!), "no new challenge after busy")
        tp.onControl(hex(f["response"]!), nowMs: 400)
        tp.onControl(hex(f["result"]!), nowMs: 500)
        check(tp.outcome == .match, "busy, then match")

        /* busy answers keep it alive, but not past 20 s. */
        (tp, sent) = make(k, cn)
        tp.start(nowMs: 0)
        var t: Int64 = 0
        while tp.pending && t < 30000 {
            t += 100
            tp.onControl([25, 34, 1, 4, 0], nowMs: t)
            tp.tick(nowMs: t + 250)
            t += 250
        }
        check(tp.outcome == .timeout && t > 20000 && t < 21000, "busy forever: \(String(describing: tp.outcome)) at \(t)")

        /* Silence: a timeout after 5 s. */
        (tp, sent) = make(k, cn)
        tp.start(nowMs: 0)
        tp.tick(nowMs: 4900)
        check(tp.pending, "timed out early")
        tp.tick(nowMs: 5100)
        check(tp.outcome == .timeout, "silence")

        /* Other commands aren't the proof's; a result before the confirm is ignored. */
        (tp, sent) = make(k, cn)
        tp.start(nowMs: 0)
        check(!tp.onControl([0x84, 0x01, 0x00], nowMs: 10), "took a stream_video message")
        check(!tp.onControl([25, 0x84, 1, 4, 0], nowMs: 10), "took another command's error")
        tp.onControl([0x22, 0x03, 0x01], nowMs: 20)
        check(tp.pending, "a result before the confirm counted")

        /* Keys as mg_ble_client keeps them. */
        check(TokenProof.parseKey(v["K"] as! String) == k, "parseKey")
        check(TokenProof.parseKey(" " + (v["K"] as! String).uppercased() + "\n") == k, "parseKey, upper case and spaces")
        check(TokenProof.parseKey("abc") == nil && TokenProof.parseKey(String(repeating: "g", count: 64)) == nil
              && TokenProof.parseKey("+a" + String(repeating: "0", count: 62)) == nil, "parseKey, bad")
    }
}
