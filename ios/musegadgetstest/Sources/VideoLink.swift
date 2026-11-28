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
    /* UI streams (mg_stream_video_capability_ui): the picture's size, 0 when
     * the gadget takes none; its frame rate cap, largest frame and bands. */
    var uiW = 0, uiH = 0, uiFps = 15, uiMaxFrame = 0, uiBands = 1
    /* UI input (supported_features' typed list): touch events, HID modes. */
    var uiTouch = false, hidDigitizer = false, hidMouseAbs = false, hidMouseRel = false
    /* Controls and a menu button (mg_ui_input_feature_controls); still patch formats (1 << format). */
    var uiControls = false, uiStill = 0

    /* Keeps what a gadget claims in range: the app sizes encoders and buffers
     * from it, and anything on the network can answer as a gadget. */
    /* minBuffer: the smallest ring taken as reported (wifiMinBuffer on the Wi-Fi link). */
    func clamp(minBuffer: Int64 = Caps.minBuffer) {
        func c(_ v: Int, _ lo: Int, _ hi: Int) -> Int { max(lo, min(hi, v)) }
        maxW = c(maxW, 64, 720)
        maxH = c(maxH, 64, 720)
        sugW = c(sugW, 64, maxW)
        sugH = c(sugH, 64, maxH)
        /* A panel or UI picture under 16 px is no size at all (0: unknown): the scalers need a macroblock. */
        dispW = dispW < Caps.minPanel ? 0 : min(dispW, 2048)
        dispH = dispH < Caps.minPanel ? 0 : min(dispH, 2048)
        maxFps = c(maxFps, 1, 60)
        sugFps = c(sugFps, 1, maxFps)
        sugKbps = c(sugKbps, 0, 20000)
        maxPlayoutMs = c(maxPlayoutMs, 0, 2000)
        maxTiles = c(maxTiles, 1, 2)
        tileSplitPct = c(tileSplitPct, 0, 90)
        uiW = uiW < Caps.minPanel ? 0 : min(uiW, 720) & ~1
        uiH = uiH < Caps.minPanel ? 0 : min(uiH, 720) & ~1
        uiFps = c(uiFps, 1, 60)
        uiBands = c(uiBands, 1, 2)
        /* Tiny-RAM BLE gadgets (e.g. 2 x 3 KB) report small rings: keep theirs. */
        buffer = max(minBuffer, min(1 << 20, buffer))
        uiMaxFrame = c(uiMaxFrame, 0, Int(buffer / 2))   /* maxFrame: the ring holds one while the next arrives */
    }

    static let minBuffer: Int64 = 2048, wifiMinBuffer: Int64 = 16384
    static let minPanel = 16

    func copy() -> Caps {
        let c = Caps()
        c.set(self)
        return c
    }

    /* A private copy, clamped, to size anything from: a link's caps can change under the reader (a
     * hello or a capability report arriving), and no unclamped value reaches the arithmetic. */
    func snapshot() -> Caps {
        let c = copy()
        c.clamp()
        return c
    }

    func set(_ o: Caps) {
        maxPlayoutMs = o.maxPlayoutMs
        dispW = o.dispW; dispH = o.dispH; maxW = o.maxW; maxH = o.maxH; maxFps = o.maxFps
        sugW = o.sugW; sugH = o.sugH; sugFps = o.sugFps; sugKbps = o.sugKbps
        round = o.round
        buffer = o.buffer
        video = o.video
        maxTiles = o.maxTiles
        tileSplitPct = o.tileSplitPct
        vp8 = o.vp8
        uiW = o.uiW; uiH = o.uiH; uiFps = o.uiFps; uiMaxFrame = o.uiMaxFrame; uiBands = o.uiBands
        uiTouch = o.uiTouch; hidDigitizer = o.hidDigitizer; hidMouseAbs = o.hidMouseAbs; hidMouseRel = o.hidMouseRel
        uiControls = o.uiControls; uiStill = o.uiStill
    }

    /* The largest frame the gadget's ring takes whole: half of it (it holds one frame while the next arrives). */
    var maxFrame: Int { Int(buffer / 2) }

    var description: String {
        "display \(dispW)x\(dispH)\(round ? " round" : ""), max \(maxW)x\(maxH)@\(maxFps), buffer \(buffer), suggests \(sugW)x\(sugH)@\(sugFps) \(sugKbps) kb/s"
            + (maxTiles > 1 ? ", \(maxTiles) bands" : "") + (vp8 ? ", VP8" : "")
            + (uiW > 0 ? ", UI \(uiW)x\(uiH)@\(uiFps) max frame \(uiMaxFrame) B, \(uiBands) band(s)" : "")
    }
}

/*
 * The user's own start, held for one connection: the one it was made on, or the next one to get a
 * usable link. A connection that got one and ends, however far its gadget went (capabilities or
 * not), or a link that is remade, lets it go; a connect attempt that never got one keeps it.
 */
struct StartGrant {
    private(set) var epoch = 0   /* the connection a retry or a held start belongs to */
    private var granted = -1
    private var cameraYes = -1   /* the connection the user let the gadget start the camera on */
    /* The user's Disconnect: nothing starts until a gadget is picked again (reopen). */
    private(set) var closed = false

    var held: Bool { !closed && granted == epoch }

    /* A camera start that isn't the user's own (the gadget's ask over BLE, Start automatically) needs
     * the user's yes, once per connection. */
    /* benchAuto: Start automatically (or -auto YES) with the app's private bench marker present; the user's
     * standing start on a bench phone. */
    func needsCameraAsk(benchAuto: Bool) -> Bool { !held && cameraYes != epoch && !benchAuto }

    mutating func allowCamera() { cameraYes = epoch }

    mutating func grant() { granted = epoch }

    mutating func revoke() { granted = -1 }

    /* Moves on when a connection ends; one that never got a usable link doesn't count. */
    mutating func connectionEnded(wasReady: Bool) {
        if wasReady {
            epoch += 1
        }
    }

    mutating func linkRemade() { epoch += 1 }

    /* Disconnect, first thing: the connection it ends moves on at once, so nothing from before it (the
     * gadget's ask in the close's grace, a retry, the camera's yes) starts anything again. */
    mutating func close() {
        epoch += 1
        closed = true
    }

    /* A gadget picked again, or Connect. */
    mutating func reopen() { closed = false }

    /* Whether a start may go ahead: the user's own on this connection, or one made on its own
     * (selfStart: Start automatically, the gadget's ask) once this connection proved the gadget. */
    func allows(selfStart: Bool, proven: Bool) -> Bool {
        !closed && (held || selfStart && proven)
    }
}

/* What streams now, as a setting change sees it. */
enum RunningStream {
    case nothing, ui, camera, clip

    /* A setting (frame rate, picture size, camera, codec, bands, encoder) applies to what runs: it
     * restarts the stream it changes, of the kinds it names, and starts nothing; a UI stream (the
     * screen, a web page) keeps its own size and rate. */
    func restarts(for kinds: Set<RunningStream>) -> Bool {
        self != .nothing && self != .ui && kinds.contains(self)
    }
}

/* Why a UI stream ends, and whether its web page goes with it: a busy or proof-wait retry on the same
 * link, or a setting's restart, keeps the page (no touch reaches it while nothing streams); losing the
 * link, a disconnect or the user's stop unloads it. */
enum UiStreamEnd {
    case retry, restart, linkLost, disconnected, userStop

    var unloadsPage: Bool { self != .retry && self != .restart }
}

protocol VideoLinkListener: AnyObject {
    func onStatus(_ status: String)
    func onReady(_ caps: Caps)
    func onRequestStreaming(_ want: Bool)
    func onKeyframeRequest()
    func onStreamStopped()
    /* The gadget refused a start (reason: Mg.errorName of its code; over Wi-Fi closed, busy, invalid, unsupported). */
    func onStartRefused(_ reason: String)
    func onBufferUpdate(avail: Int64, received: Int64, shown: Int, dropped: Int, decodeMs: Int)
    /* wasReady: the connection got a usable link (a failed connect attempt: false). */
    func onDisconnected(wasReady: Bool)
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
    /* A UI stream (mg_video_content_ui, protocols/UI.md): H.264 at up to the
     * ui capability's size, frames only on change. Links without it ignore it. */
    func startUiStreaming(w: Int, h: Int, fps: Int, kbps: Int, bands: Int) {}
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

    /* This connection proved the gadget (its token proof matched). A link that proves nothing (Wi-Fi): never. */
    var proven: Bool { false }
}
