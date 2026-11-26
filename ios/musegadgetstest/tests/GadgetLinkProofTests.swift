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

/* GadgetLink's gate: no stream until this connection's token proof matched. */
enum GadgetLinkProofTests {
    final class Listener: VideoLinkListener {
        func onStatus(_ status: String) {}
        func onReady(_ caps: Caps) {}
        func onRequestStreaming(_ want: Bool) {}
        func onKeyframeRequest() {}
        func onStreamStopped() {}
        func onStartRefused(_ reason: String) {}
        func onBufferUpdate(avail: Int64, received: Int64, shown: Int, dropped: Int, decodeMs: Int) {}
        var disconnects: [Bool] = []
        func onDisconnected(wasReady: Bool) { disconnects.append(wasReady) }
        func onSeen(name: String, address: String, rssi: Int) {}
    }

    static func check(_ ok: Bool, _ what: String, line: Int = #line) {
        Tests.check(ok, "GadgetLinkProofTests: " + what, line: line)
    }

    /* A link with key k, never started (no Bluetooth), its outcomes collected. */
    static func make(_ k: [UInt8]) -> (GadgetLink, Listener, () -> [TokenProof.Outcome?]) {
        let l = Listener()
        let g = GadgetLink(listener: l)
        g.proofKey = k
        var got: [TokenProof.Outcome?] = []
        g.onProof = { got.append($0) }
        g.close()   /* no rescans after disconnected() */
        return (g, l, { got })
    }

    static func run() {
        let k = [UInt8](repeating: 7, count: 32), key = SymmetricKey(data: k)

        /* Commands without token_proof: nil, never proven, no stream. */
        var (g, l, got) = make(k)
        g.onControl([16, 10, 0x84, 0x86])
        check(got().count == 1 && got()[0] == nil && !g.proven, "no token_proof listed")
        g.startUiStreaming(w: 240, h: 240, fps: 15, kbps: 400, bands: 1)
        check(!g.streaming() && !g.takeControl().contains { $0.first == 0x84 }, "a stream started unproven")

        /* With token_proof: challenge, a device that answers right, result 1: proven, and only then a stream. */
        (g, l, got) = make(k)
        g.onControl([16, 10, 34, 0x84])
        let ch = g.takeControl().last.map { [UInt8]($0) } ?? []
        check(ch.count == 18 && ch[0] == 34 && ch[1] == 0, "no challenge")
        let cn = Array(ch.dropFirst(2)), dn = [UInt8](repeating: 9, count: 16)
        g.startUiStreaming(w: 240, h: 240, fps: 15, kbps: 400, bands: 1)
        check(!g.streaming(), "a stream started during the proof")
        g.onControl([34, 1] + dn + TokenProof.mac(key, device: true, cn, dn))
        let cf = g.takeControl().last.map { [UInt8]($0) } ?? []
        check(cf == [34, 2] + TokenProof.mac(key, device: false, cn, dn), "no confirm")
        check(!g.proven, "proven before the result")
        g.onControl([34, 3, 1])
        check(g.proven && got() == [.match], "not proven after result 1")
        g.startUiStreaming(w: 240, h: 240, fps: 15, kbps: 400, bands: 1)
        check(g.streaming(), "no stream after the match")

        /* A second command list doesn't start another proof; a disconnect forgets the match. */
        g.onControl([16, 10, 34, 0x84])
        check(g.takeControl().allSatisfy { $0.first != 34 }, "a second proof")
        g.disconnected()
        check(!g.proven && !g.streaming(), "the match outlived the connection")

        /* A wrong device MAC: mismatch, no confirm. */
        (g, l, got) = make(k)
        g.onControl([16, 10, 34])
        _ = g.takeControl()
        g.onControl([34, 1] + dn + [UInt8](repeating: 0, count: 32))
        check(got() == [.mismatch] && g.takeControl().isEmpty && !g.proven, "a wrong MAC")

        /* A recovery declaration: no proof, even when the command list has token_proof. */
        (g, l, got) = make(k)
        g.onControl([16, 0xFE, 10, 1, 1, 1])
        g.onControl([16, 10, 34])
        check(got() == [.recovery] && g.takeControl().isEmpty && !g.proven, "recovery: \(got())")

        /* A recovery declaration during a proof: that outcome stands, a later result 1 doesn't count. */
        (g, l, got) = make(k)
        g.onControl([16, 10, 34])
        let c2 = Array((g.takeControl().last.map { [UInt8]($0) } ?? []).dropFirst(2))
        g.onControl([16, 0xFE, 10, 1, 1, 1])
        g.onControl([34, 1] + dn + TokenProof.mac(key, device: true, c2, dn))
        g.onControl([34, 3, 1])
        check(got() == [.recovery] && !g.proven && g.takeControl().isEmpty, "recovery during a proof: \(got())")

        /* The app's link: no proof asked (camera, clips, web page): no key, no proof, streams as before. */
        let g2 = GadgetLink(listener: l)
        g2.proofRequired = false
        g2.close()
        g2.onControl([16, 10, 34, 0x84])
        check(g2.takeControl().isEmpty, "a proof without a key")
        g2.startUiStreaming(w: 240, h: 240, fps: 15, kbps: 400, bands: 1)
        check(g2.streaming(), "no stream without a key")

        /* Its screen mirror asks the proof: with no key, no stream and no frames. */
        g2.proofRequired = true
        g2.sendFrame(Data(count: 10))
        check(g2.queuedBytes() == 0, "a mirror frame went to a gadget with no key")
        g2.stopStreaming()
        g2.startUiStreaming(w: 240, h: 240, fps: 15, kbps: 400, bands: 1)
        check(!g2.streaming(), "a mirror started with no key")

        /* With a key, a stream that's already on carries no mirror frame until the proof matched. */
        (g, l, got) = make(k)
        g.proofRequired = false
        g.startStreaming(w: 240, h: 240, fps: 24, kbps: 400, playoutMs: 0)
        check(g.streaming(), "the camera waited for the proof")
        g.proofRequired = true
        g.sendFrame(Data(count: 10))
        check(g.queuedBytes() == 0, "a mirror frame before the proof")
        g.onControl([16, 10, 34, 0x84])
        let c3 = Array((g.takeControl().last.map { [UInt8]($0) } ?? []).dropFirst(2))
        g.onControl([34, 1] + dn + TokenProof.mac(key, device: true, c3, dn))
        g.onControl([34, 3, 1])
        g.sendFrame(Data(count: 10))
        check(g.proven && g.queuedBytes() == 10, "no mirror frame after the match")
        _ = l
    }
}
