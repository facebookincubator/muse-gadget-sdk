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

/* What the gadget offers (its hello). */
final class Caps: CustomStringConvertible {
    var maxPlayoutMs = 0   /* the device paces frames up to this playout delay (0: no pacing) */
    var dispW = 0, dispH = 0, maxW = 240, maxH = 240, maxFps = 30, sugW = 232, sugH = 232, sugFps = 24, sugKbps = 450
    var round = false
    var buffer: Int64 = 65536
    var video = false
    var maxTiles = 1       /* bands it decodes in parallel */
    var tileSplitPct = 0   /* two bands: the top band's share of the height, %; 0: equal */
    var vp8 = false        /* decodes VP8 as well as H.264 (hello's codecs) */

    /* Keeps what a gadget claims in range: the app sizes encoders and buffers
     * from it, and anything on the network can answer as a gadget. */
    func clamp() {
        func c(_ v: Int, _ lo: Int, _ hi: Int) -> Int { max(lo, min(hi, v)) }
        maxW = c(maxW, 64, 720)
        maxH = c(maxH, 64, 720)
        sugW = c(sugW, 64, maxW)
        sugH = c(sugH, 64, maxH)
        dispW = c(dispW, 0, 2048)
        dispH = c(dispH, 0, 2048)
        maxFps = c(maxFps, 1, 60)
        sugFps = c(sugFps, 1, maxFps)
        sugKbps = c(sugKbps, 0, 20000)
        maxPlayoutMs = c(maxPlayoutMs, 0, 2000)
        maxTiles = c(maxTiles, 1, 2)
        tileSplitPct = c(tileSplitPct, 0, 90)
        buffer = max(16384, min(1 << 20, buffer))
    }

    var description: String {
        "display \(dispW)x\(dispH)\(round ? " round" : ""), max \(maxW)x\(maxH)@\(maxFps), buffer \(buffer), suggests \(sugW)x\(sugH)@\(sugFps) \(sugKbps) kb/s"
            + (maxTiles > 1 ? ", \(maxTiles) bands" : "") + (vp8 ? ", VP8" : "")
    }
}

protocol VideoLinkListener: AnyObject {
    func onStatus(_ status: String)
    func onReady(_ caps: Caps)
    func onRequestStreaming(_ want: Bool)
    func onKeyframeRequest()
    func onStreamStopped()
    /* The gadget refused a start (reason: busy, invalid, unsupported). */
    func onStartRefused(_ reason: String)
    func onBufferUpdate(avail: Int64, received: Int64, shown: Int, dropped: Int, decodeMs: Int)
    func onDisconnected()
    /* A gadget found (for the device picker); rssi 0 where the link has none. */
    func onSeen(name: String, address: String, rssi: Int)
}

/*
 * A connection to one gadget that shows video, whatever carries it: finding
 * gadgets for the picker, connecting, the gadget's capabilities, and a stream
 * of frames (VideoFrame) sent inside the gadget's credits. WifiLink is the
 * one over the local network (protocols/VIDEO.md).
 */
class VideoLink: NSObject {
    /* Live sources (the camera): at most this much video the device hasn't
     * decoded yet before the next frame starts; 0: sized for clips. */
    var liveAhead: Int64 = 0
    var meter: LatencyMeter?
    /* Stats: stream bytes sent, writes, time frames waited for the backlog
     * gate (statGateUs) and for credits (statStallUs). */
    var statBytes: Int64 = 0, statWrites: Int64 = 0, statStallUs: Int64 = 0, statGateUs: Int64 = 0

    func start() {}
    func close() {}
    func ready() -> Bool { false }
    func caps() -> Caps { Caps() }
    /* Before start(): what to connect to (address wins when given); neither: list only. */
    func setTarget(address: String?, name: String?) {}
    func describe() -> String { "" }
    /* What kind of link it is, for the screen ("Wi-Fi"), and the connected gadget's name. */
    func kind() -> String { "" }
    func gadgetName() -> String? { nil }
    /* bands: the picture as that many horizontal bands, each its own stream; vp8: VP8 frames, else H.264. */
    func startStreaming(w: Int, h: Int, fps: Int, kbps: Int, playoutMs: Int, bands: Int = 1, vp8: Bool = false) {}
    func stopStreaming() {}
    func streaming() -> Bool { false }
    /* Queues one frame (header included). */
    func sendFrame(_ frame: Data) {}
    /* Frame bytes waiting to go (not counting what's on its way). */
    func queuedBytes() -> Int64 { 0 }
    /* Bytes the device hasn't taken out of its ring yet, as far as we know. */
    func inFlight() -> Int64 { 0 }
    /* The device picker. */
    func scanForPicker() {}
    func pickerClosed() {}
    func select(address: String, name: String) {}
    func connectedAddress() -> String? { nil }
}
