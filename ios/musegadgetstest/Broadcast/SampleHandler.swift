// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

import CoreBluetooth
import ReplayKit

/*
 * The broadcast extension: the whole screen on the gadget while the user
 * is in other apps (protocols/UI.md). ReplayKit runs it, as its own process
 * with a 50 MB memory limit, for as long as the broadcast lasts, and hands it
 * a picture of the screen each time the screen changes. It needs nothing
 * from the app while it runs: it opens its own musegadgets connection
 * (CoreBluetooth; the link the app already has is reused by the system) to
 * the gadget the app picked, and runs the same UI stream as the app's UI tab
 * (UiStreamer: the window, the warp, only what changed). The app's settings
 * (the window, the warp, the gadget) come through the shared keychain group,
 * its own report goes back the same way, and Darwin notifications say
 * started, stopped, and "settings changed".
 */
final class SampleHandler: RPBroadcastSampleHandler, VideoLinkListener {
    private var link: GadgetLink?
    private var streamer: UiStreamer?
    /* The streamer holds its output weakly: this keeps the link's alive while it streams. */
    private var output: LinkOutput?
    private var settings = MirrorSettings()
    private let lock = NSLock()
    private var timer: DispatchSourceTimer?
    private var lastCpu = 0.0, lastAt = 0.0, lastBytes: Int64 = 0
    private var samples = 0, lastSampleNs: Int64 = 0
    private var state = "Starting"
    private var btState = ""
    private let meter = LatencyMeter()
    private var gpu: UiWarp?
    private var controls: UiControls?
    private var lastHid = ""
    /* The gadget's panel is round (its caps): the warp and the mask apply only there. */
    private var round = true
    private var finished = false, endedWithReason = false
    private var refusals = 0
    /* When the gadget was last streaming (or the broadcast started): with no
     * stream for this long, the broadcast ends with the reason. */
    private var lastGoodAt = Date()
    private static let connectTimeout = 10.0
    /* Caps that arrived before the token proof matched: the stream starts once it does. */
    private var pendingCaps: Caps?
    /* The key, and whether the gadget is proving a changed key (no stream until it has). */
    private var keyBytes: [UInt8]? = nil, reproving = false
    /* A refusal since the gadget last took the stream: its next buffer_update clears the count. */
    private let refLock = NSLock()
    private var refusedSince = false
    private static let fixHint = "Set it up with mg_ble_client (esp32/tools/mg_ble_client.py setup), "
        + "then import its key in Muse Gadgets Test (UI tab, Screen mirror, Proof key)."

    override func broadcastStarted(withSetupInfo setupInfo: [String: NSObject]?) {
        settings = UiShared.load("settings", MirrorSettings.self) ?? MirrorSettings()
        mlog("broadcast: started; gadget \(settings.gadgetName) \(settings.gadget), window \(settings.crop(w: 1000, h: 2000)), warp \(settings.warp), memory \(String(format: "%.1f", ProcStats.footprintMB())) MB")
        UiShared.post(UiShared.notifyStarted)
        UiShared.observe(UiShared.notifyStop) { [weak self] in
            self?.finishBroadcastWithError(NSError(domain: "MuseGadgets", code: 1,
                                                   userInfo: [NSLocalizedDescriptionKey: "Stopped from Muse Gadgets Test"]))
        }
        UiShared.observe(UiShared.notifySettings) { [weak self] in self?.reloadSettings() }
        UiShared.observe(UiShared.notifyProofKey) { [weak self] in self?.proofKeyChanged() }
        gpu = UiWarp()
        guard !settings.gadgetName.isEmpty else {
            finish("Pick a gadget in Muse Gadgets Test (Gadget tab), then start the broadcast again.")
            return
        }
        /* Pixels go only to a gadget that proves it holds this phone's key for
         * it (the token proof). The name only finds it: a peripheral's
         * identifier differs between the app and its extension. */
        guard let startKey = UiShared.proofKey(for: settings.gadgetName) else {
            finish("No proof key for \(gadgetLabel), so the broadcast can't tell it from another device using its name. "
                + Self.fixHint)
            return
        }
        let g = GadgetLink(listener: self)
        g.setTarget(address: nil, name: settings.gadgetName)
        g.exactName = true
        keyBytes = startKey
        g.proofKeyFor = { UiShared.proofKey(for: $0) }   /* read at every connection */
        g.onProof = { [weak self] o in DispatchQueue.main.async { self?.onProof(o) } }
        state = "Connecting to \(gadgetLabel)"
        lastGoodAt = Date()
        g.meter = meter
        link = g
        g.start()
        let t = DispatchSource.makeTimerSource(queue: .main)   /* with onReady, onProof and onDisconnected */
        t.schedule(deadline: .now() + 1, repeating: 1)
        t.setEventHandler { [weak self] in
            self?.report()
            self?.watchdog()
        }
        t.resume()
        timer = t
        lastCpu = ProcStats.cpuSeconds()
        lastAt = Date().timeIntervalSince1970
    }

    override func broadcastPaused() {
        mlog("broadcast: paused")
    }

    override func broadcastResumed() {
        mlog("broadcast: resumed")
        streamer?.requestKeyframe()
    }

    private var gadgetLabel: String { settings.gadgetName.isEmpty ? "the gadget" : settings.gadgetName }

    /* processSampleBuffer reads it under the lock, on ReplayKit's queue. */
    private func setStreamer(_ s: UiStreamer?) {
        lock.lock()
        streamer = s
        lock.unlock()
    }

    /* The app changed or removed a proof key (main queue): the stream stops, and goes on
     * only once the gadget proves the key now saved for it. */
    private func proofKeyChanged() {
        guard !finished, let g = link else { return }
        guard let k = UiShared.proofKey(for: settings.gadgetName) else {
            finish("The proof key for \(gadgetLabel) was removed. " + Self.fixHint)
            return
        }
        guard k != keyBytes else { return }   /* another gadget's, or the same key again */
        keyBytes = k
        mlog("broadcast: \(gadgetLabel)'s proof key changed: proving it again")
        reproving = true
        state = "Checking \(gadgetLabel)'s new key"
        pointerOff()   /* while the old key still holds; back once the new one matches */
        lastHid = ""
        controls = nil
        g.onUiInput = nil
        streamer?.stop()
        setStreamer(nil)
        if g.ready() {
            pendingCaps = g.caps()
        }
        lastGoodAt = Date()
        g.reprove()
    }

    /* The token proof's outcome (main queue): only a match lets the stream start. */
    private func onProof(_ o: TokenProof.Outcome?) {
        guard !finished else { return }
        reproving = false
        switch o {
        case .match:
            mlog("broadcast: \(gadgetLabel) proved its key")
            lastGoodAt = Date()
            if pendingCaps != nil, let g = link, g.ready() {
                pendingCaps = nil
                onReady(g.caps())   /* this connection's caps */
            }
        case .mismatch:
            finish("\(gadgetLabel) didn't prove it holds this phone's key: another device may be using its name, "
                + "or it was set up again since. " + Self.fixHint)
        case .notFound:
            finish("\(gadgetLabel) isn't set up (it holds no token). " + Self.fixHint)
        case .refused(let code):
            finish("\(gadgetLabel) refused the token proof (error \(code)): it may be in recovery mode, "
                + "or another device is using its name. " + Self.fixHint)
        case .recovery:
            finish("\(gadgetLabel) is in a recovery mode, so the broadcast can't trust it. "
                + "Restart it, or update it from Muse Gadgets Test.")
        case .timeout:
            finish("\(gadgetLabel) didn't answer the token proof. Start the broadcast again; if it keeps failing, "
                + Self.fixHint.prefix(1).lowercased() + Self.fixHint.dropFirst())
        case nil:
            finish("\(gadgetLabel) doesn't offer the token proof, so the broadcast can't trust it: another device may be "
                + "using its name, or it's in recovery mode or needs a firmware update.")
        }
    }

    /* No stream for connectTimeout seconds: end the broadcast and say why. */
    private func watchdog() {
        guard !finished, streamer == nil, Date().timeIntervalSince(lastGoodAt) > Self.connectTimeout,
              link?.proofPending != true else { return }
        let connected = link?.ready() == true
        finish(connected
            ? "\(gadgetLabel) didn't take the stream. Open Muse Gadgets Test and try again."
            : "Couldn't connect to \(gadgetLabel): it may be connected to another phone, off, or out of range. "
            + "Check it in Muse Gadgets Test (Gadget tab), then start the broadcast again.")
    }

    /* Ends the broadcast with a reason the user sees (ReplayKit shows it) and the app shows. */
    private func finish(_ why: String) {
        guard !finished else { return }
        finished = true
        endedWithReason = true
        mlog("broadcast: ending: " + why)
        state = why
        timer?.cancel()
        streamer?.stop()
        setStreamer(nil)
        pointerOff()
        link?.close()
        report(ended: true)
        finishBroadcastWithError(NSError(domain: "MuseGadgets", code: 2, userInfo: [NSLocalizedDescriptionKey: why]))
    }

    override func broadcastFinished() {
        mlog("broadcast: finished")
        finished = true
        timer?.cancel()
        streamer?.stop()
        setStreamer(nil)
        pointerOff()
        link?.close()
        link = nil
        if !endedWithReason {
            state = "Stopped"
        }
        report(ended: true)
        UiShared.post(UiShared.notifyStopped)
    }

    override func processSampleBuffer(_ sb: CMSampleBuffer, with type: RPSampleBufferType) {
        guard type == .video, let pb = CMSampleBufferGetImageBuffer(sb) else { return }
        samples += 1
        lastSampleNs = nowNs()
        lock.lock()
        let s = streamer, crop = settings.crop(w: CVPixelBufferGetWidth(pb), h: CVPixelBufferGetHeight(pb))
        lock.unlock()
        s?.submit(pb, crop: crop)
    }

    private func reloadSettings() {
        guard let n = UiShared.load("settings", MirrorSettings.self) else { return }
        lock.lock()
        let old = settings
        settings = n
        lock.unlock()
        streamer?.warp = n.warp && round
        if let c = controls, c.overscroll != (n.overscroll ?? 1) {
            c.setOverscroll(n.overscroll ?? 1)   /* the app's UI tab changed it: tell the gadget */
        }
        streamer?.rerender()
        if n.gadgetName != old.gadgetName {
            finish("The broadcast's gadget changed to \(n.gadgetName.isEmpty ? "none" : n.gadgetName). Start the broadcast again.")
            return
        }
        sendHidMap()
        mlog("broadcast: settings \(n.crop(w: 1000, h: 2000)) warp \(n.warp)")
    }

    /* Screen control ends: the gadget keeps its HID map in flash, so its pointer goes off before the link closes. */
    private func pointerOff() {
        guard let g = link, g.ready(), g.proven, g.caps().hidMouseRel else { return }
        g.sendHidMap(mode: 0, x: 0, y: 0, w: 0, h: 0, fisheye: false)
        g.flushControl(maxMs: 300)
    }

    private func sendHidMap() {
        guard let g = link, g.ready(), g.proven, g.caps().hidMouseRel else { return }
        let s = settings
        let key = "\(s.hid) \(s.cx) \(s.cy) \(s.size) \(s.warp) \(s.hidCountsW) \(s.hidCountsH) \(s.overscroll ?? 1)"
        if key == lastHid {
            return
        }
        lastHid = key
        if !s.hid {
            g.sendHidMap(mode: 0, x: 0, y: 0, w: 0, h: 0, fisheye: false)
            return
        }
        /* The window in 0..1 of the screen, from the screen's shape in points. */
        let b = UIScreen.main.nativeBounds
        let c = s.crop(w: Int(b.width), h: Int(b.height))
        let cw = s.hidCountsW > 0 ? s.hidCountsW : Int(UIScreen.main.bounds.width)
        let ch = s.hidCountsH > 0 ? s.hidCountsH : Int(UIScreen.main.bounds.height)
        let q = GadgetLink.hidSquare(c)
        g.sendHidMap(mode: 3, x: q.x, y: q.y, w: q.w, h: q.h, fisheye: s.warp && round, countsW: cw, countsH: ch, offset: q.offset)
    }

    private func report(ended: Bool = false) {
        var st = MirrorStatus()
        st.ended = ended
        let now = Date().timeIntervalSince1970, cpu = ProcStats.cpuSeconds()
        st.at = now
        st.state = state
        st.memMB = ProcStats.footprintMB()
        st.cpuPct = (cpu - lastCpu) / max(0.001, now - lastAt) * 100
        st.bluetooth = btState
        if let s = streamer {
            let x = s.snapshot()
            st.frames = x.frames
            st.changes = x.changes
            st.refines = x.refines
            st.kbps = Double(x.bytes - lastBytes) * 8 / 1000 / max(0.001, now - lastAt)
            lastBytes = x.bytes
        }
        st.latency = meter.take()
        lastCpu = cpu
        lastAt = now
        UiShared.save("status", st)
        mlog(String(format: "broadcast stats: %@, %d frames (%d changes, %d refines), %.1f kb/s, %d screen samples, memory %.1f MB, CPU %.0f%%; %@",
                    state, st.frames, st.changes, st.refines, st.kbps, samples, st.memMB, st.cpuPct, st.latency))
    }

    /* ---- VideoLinkListener ------------------------------------------------ */

    func onStatus(_ status: String) {
        mlog("broadcast: " + status)
        if streamer == nil {
            state = status
        }
        btState = status
    }

    func onReady(_ caps: Caps) {
        mlog("broadcast: gadget ready: \(caps)")
        DispatchQueue.main.async { [self] in
            guard !finished, streamer == nil, let g = link, let gpu else { return }
            guard g.proven && !reproving else {
                pendingCaps = caps   /* starts when the token proof matches */
                return
            }
            let cfg = UiStreamer.config(for: caps)
            guard let s = UiStreamer(cfg, gpu: gpu) else { return }
            round = caps.round
            s.warp = settings.warp && round
            s.mask = round
            s.meter = meter
            let out = LinkOutput(g, ui: caps.uiW > 0)
            output = out
            s.output = out
            s.log = true
            s.sharp = settings.sharp ?? true
            s.fastFps = settings.fastFps ?? 30
            s.legacyPacing = settings.legacyPacing ?? false
            s.stillPatches = (caps.uiStill & (1 << Int(Mg.stillRgb565Zlib))) != 0
            let ctl = UiControls(n: cfg.n, mirror: true, fisheye: settings.warp && round, sharp: s.sharp, round: round,
                                 overscroll: settings.overscroll ?? 1)
            ctl.send = { [weak g] id, v in g?.sendUiControl(id, v) }
            ctl.onFisheye = { [weak self] on in
                guard let self else { return }
                self.lock.lock()
                self.settings.warp = on
                self.lock.unlock()
                self.streamer?.warp = on && self.round
                self.streamer?.rerender()
                UiShared.save("settings", self.settings)
                self.sendHidMap()
            }
            ctl.onSharp = { [weak self] on in
                guard let self, let st = self.streamer else { return }
                let was = st.sharp
                st.sharp = on
                if on && !was && st.stillPatches {
                    st.requestKeyframe()
                }
                self.settings.sharp = on
                UiShared.save("settings", self.settings)
            }
            ctl.onStop = { [weak self] in self?.finish("Stopped from the gadget") }
            ctl.onPan = { [weak self] dx, dy in self?.pan(dx, dy) }
            ctl.onRecenter = { [weak self] in
                guard let self else { return }
                self.lock.lock()
                self.settings.recenter()
                let s = self.settings
                self.lock.unlock()
                UiShared.save("settings", s)
                self.streamer?.rerender()
                self.sendHidMap()
            }
            ctl.onOverscroll = { [weak self] v in
                guard let self else { return }
                let b = UIScreen.main.bounds
                self.lock.lock()
                self.settings.overscroll = v
                self.settings.clampCenter(w: b.width, h: b.height)
                let s = self.settings
                self.lock.unlock()
                UiShared.save("settings", s)
                self.streamer?.rerender()
                self.sendHidMap()
            }
            ctl.navAvailable = { [weak self, weak g] in (self?.settings.hid ?? false) && (g?.caps().hidMouseRel ?? false) }
            ctl.onNav = { [weak g] which in
                g?.sendHidKey(which == 0 ? 0x224 : 0x223)
                mlog("broadcast: \(which == 0 ? "Back" : "Home") via the gadget's HID key")
            }
            ctl.onRedraw = { [weak self] in self?.streamer?.rerender() }
            controls = ctl
            s.overlay = { [weak ctl] p, size in ctl?.draw(p, size) ?? false }
            s.overlayActive = { [weak ctl] in ctl?.drawing ?? false }
            s.cropProvider = { [weak self] w, h in
                guard let self else { return SIMD4<Float>(0, 0, 1, 1) }
                self.lock.lock()
                defer { self.lock.unlock() }
                return self.settings.crop(w: w, h: h)
            }
            g.onUiInput = { [weak self] v in DispatchQueue.main.async { self?.onUiInput(v) } }
            setStreamer(s)
            s.start()
            if caps.uiTouch {
                g.setTouchEvents(true)   /* the menu and panning need them */
            }
            ctl.sendAll()
            mlog("broadcast: controls \(caps.uiControls ? "on" : "not offered"), still patches \(s.stillPatches ? "on" : "off")")
            state = "Streaming to \(gadgetLabel)"
            lastGoodAt = Date()
            sendHidMap()
        }
    }

    /* [ui_input, ...] from the gadget, on the main queue: Stop runs finish(), whose link.close() waits on the BLE
     * queue the input arrives on. */
    private func onUiInput(_ v: [UInt8]) {
        guard !finished, v.count >= 4, link?.proven == true, let c = controls else { return }
        if v[1] == Mg.uiTouch && v.count >= 8 {
            c.touch(action: Int(v[2]), x: Mg.u16(v, 3), y: Mg.u16(v, 5), id: v.count >= 10 ? Int(v[9]) : 0)
        } else if v[1] == Mg.uiControl {
            c.gadgetControl(v[2], Int(v[3]))
        }
    }

    /* The window follows the finger the other way (dx, dy in panel pixels). */
    private func pan(_ dx: Double, _ dy: Double) {
        let b = UIScreen.main.bounds
        lock.lock()
        let n = Double(max(1, controls?.n ?? 360))
        let perPx = settings.size * b.width / n
        settings.cx -= dx * perPx / b.width
        settings.cy -= dy * perPx / b.height
        settings.clampCenter(w: b.width, h: b.height)   /* within the overscroll setting's reach */
        let s = settings
        lock.unlock()
        UiShared.save("settings", s)
        streamer?.rerender()
        sendHidMap()
    }

    func onRequestStreaming(_ want: Bool) {}

    func onKeyframeRequest() {
        DispatchQueue.main.async { [self] in streamer?.requestKeyframe() }
    }

    func onStreamStopped() {
        mlog("broadcast: the gadget stopped the stream")
        DispatchQueue.main.async { [self] in
            streamer?.stop()
            setStreamer(nil)
            state = "Stopped by the gadget"
        }
    }

    /* Main queue: finish() closes the link, which waits on its BLE queue (where this is called). */
    func onStartRefused(_ reason: String) {
        DispatchQueue.main.async { [self] in startRefused(reason) }
    }

    private func startRefused(_ reason: String) {
        refusals += 1
        refLock.lock()
        refusedSince = true
        refLock.unlock()
        if refusals >= 3 || reason != "busy" {   /* only busy passes */
            mlog("broadcast: the gadget refused the stream (\(reason))")
            finish("\(gadgetLabel) refused the stream (\(reason))." + (reason == "busy" ? " Is another stream on?" : ""))
            return
        }
        mlog("broadcast: the gadget refused the stream (\(reason)); again in 1 s")
        let caps = link?.caps()
        streamer?.stop()
        setStreamer(nil)
        DispatchQueue.main.asyncAfter(deadline: .now() + 1) { [weak self] in
            if let c = caps, self?.link?.ready() == true {
                self?.onReady(c)
            }
        }
    }

    func onBufferUpdate(avail: Int64, received: Int64, shown: Int, dropped: Int, decodeMs: Int) {
        refLock.lock()
        let reset = refusedSince
        refusedSince = false
        refLock.unlock()
        if reset {
            DispatchQueue.main.async { [self] in refusals = 0 }   /* the gadget took the stream */
        }
    }

    func onDisconnected(wasReady: Bool) {
        DispatchQueue.main.async { [self] in   /* with onReady and onProof, which run there */
            streamer?.stop()
            setStreamer(nil)
            pendingCaps = nil
            controls = nil        /* the next connection proves itself before it drives anything */
            link?.onUiInput = nil
            lastHid = ""
            lastGoodAt = Date()   /* connectTimeout to come back */
            state = "Reconnecting to \(gadgetLabel)"
        }
    }

    func onSeen(name: String, address: String, rssi: Int) {}
}

/* The UI stream onto the extension's own link. */
private final class LinkOutput: UiOutput {
    private let g: GadgetLink
    private let ui: Bool

    init(_ g: GadgetLink, ui: Bool) {
        self.g = g
        self.ui = ui
    }

    func uiStart(w: Int, h: Int, fps: Int, bands: Int) {
        guard g.proven else { return }
        if ui {
            g.startUiStreaming(w: w, h: h, fps: fps, kbps: 1000, bands: bands)
        } else {
            g.startStreaming(w: w, h: h, fps: fps, kbps: 600, playoutMs: 0, bands: bands)
        }
    }

    func uiStop() {
        g.stopStreaming()
    }

    func uiSend(_ frame: Data) {
        guard g.proven else { return }   /* never a pixel to a gadget that hasn't proved its key */
        g.sendFrame(frame)
    }

    func uiQueued() -> Int64 {
        g.queuedBytes()
    }
}
