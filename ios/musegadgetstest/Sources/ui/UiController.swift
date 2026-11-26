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

import ReplayKit
import UIKit

/*
 * The UI tab's engine room (protocols/UI.md): which source feeds the UI
 * stream (a web page, the app's own screen, or the whole screen through the
 * broadcast extension, which streams on its own Bluetooth connection), the
 * stream itself (UiStreamer) on the app's gadget link, and the gadget's
 * touches back: into the web page through the inverse warp, and HID for the
 * mirror (the gadget does that mapping itself; the app sends hid_map).
 */
final class UiController: UiOutput {
    enum Source: Int {
        case off = 0, web, screenApp, broadcast
    }

    private weak var app: MainViewController?
    private let linkOf: () -> VideoLink?
    private(set) var source = Source(rawValue: Prefs.int("ui_source")) ?? .off
    var warp = Prefs.int("ui_warp", 1) != 0
    /* The gadget's panel is round (its caps): the warp and the round mask only
     * apply there; a square panel shows the square as it is. */
    private(set) var round = true
    var warpOn: Bool { warp && round }
    var touchBack = Prefs.int("ui_touch", 1) != 0
    /* Quality: sharp (still patches where the gadget takes them, else a QP 12 step) or fast. */
    var sharp = Prefs.int("ui_sharp", 1) != 0 { didSet { mirror.sharp = sharp } }
    /* The stream's controls and the gadget's menu (while a stream is on). */
    private(set) var controls: UiControls?
    var zoom = Double(Prefs.int("ui_zoom", 100)) / 100   /* CSS viewport: the panel's size times this */
    var url = Prefs.string("ui_url") ?? "demo:clock"
    var twoBands = Prefs.bool("ui_bands")
    var mirror = UiShared.load("settings", MirrorSettings.self) ?? MirrorSettings()
    private(set) var streamer: UiStreamer?
    private(set) var web: UiWebSource?
    private(set) var preview: UIImage?
    private lazy var gpu = UiWarp()
    private(set) var n = 0
    /* The link the running stream goes to: the gadget's touches count only from it, only while it streams. */
    private var streamLink: VideoLink?
    private(set) var uiContent = false
    private(set) var lastTouch = ""
    private(set) var hidStatus = ""
    private(set) var broadcasting = false
    /* Why this app's screen isn't streaming ("": nothing holds it back), and the last proof's outcome. */
    private(set) var mirrorNote = ""
    private var lastProof = ""
    /* Why the last broadcast ended (the extension's last report). */
    private(set) var broadcastNote = ""
    private var broadcastStartedAt = 0.0
    private(set) var extStatus: MirrorStatus?
    private var pageTitle = ""
    private var recording = false
    /* Which capture start a ReplayKit picture belongs to (it arrives on ReplayKit's queue). */
    private let recLock = NSLock()
    private var recGen = 0
    private var lastStatBytes: Int64 = 0, lastStatMs: Int64 = 0
    private(set) var kbps = 0.0
    var meter: LatencyMeter?
    /* -uioffline N: no gadget; an N px UI stream into Documents/ui.h264 (frames
     * as the gadget would get them), paced like a link of -uikbps (testing in
     * the Simulator, and the stream's numbers without a gadget). */
    let offline = Extras.int("uioffline", 0)
    private var dump: FileHandle?, stillDump: FileHandle?
    private var dumpBusyUntilNs: Int64 = 0
    /* Calibration of the HID pointer (see calibrate()). */
    private var calib: (expect: CGPoint, start: Int64)?
    var onCalibrated: ((String) -> Void)?

    init(app: MainViewController, link: @escaping () -> VideoLink?) {
        self.app = app
        linkOf = link
        /* -uisource off|web|app|broadcast, -uiurl, -uiwarp 0|1 (for scripts) */
        if let src = Extras.string("uisource") {
            source = ["web": .web, "app": .screenApp, "broadcast": .broadcast][src] ?? .off
        }
        if let u = Extras.string("uiurl") {
            url = u
        }
        if let g = Extras.string("uimirrorgadget") {   /* the broadcast's gadget by name (scripts) */
            mirror.gadget = ""
            mirror.gadgetName = g
            UiShared.save("settings", mirror)
        }
        /* -proofkeyfile <name>: the broadcast gadget's token proof key from a file in Documents (scripts copy
         * mg_ble_client's proof_key there), read once and deleted, so the key is never on a command line. */
        if let f = Extras.string("proofkeyfile"),
           let dir = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask).first {
            let u = dir.appendingPathComponent((f as NSString).lastPathComponent)
            if let s = try? String(contentsOf: u, encoding: .utf8), !setProofKey(s) {
                mlog("ui: -proofkeyfile \(u.lastPathComponent): no gadget picked, or not a key; nothing saved")
            }
            try? FileManager.default.removeItem(at: u)
        }
        if let w = Extras.string("uiwin") {   /* -uiwin cx,cy,size: the mirror window (scripts; 0..1, size of the width) */
            let v = w.split(separator: ",").compactMap { Double($0) }
            if v.count == 3 {
                mirror.cx = v[0]
                mirror.cy = v[1]
                mirror.size = v[2]
                UiShared.save("settings", mirror)
            }
        }
        if Extras.has("uifast") || Extras.has("uilegacy") {   /* pacing, for the app's streams and the broadcast (scripts) */
            if Extras.has("uifast") {
                mirror.fastFps = Extras.int("uifast", 30)
            }
            if Extras.has("uilegacy") {
                mirror.legacyPacing = Extras.bool("uilegacy", false)
            }
            UiShared.save("settings", mirror)
        }
        if Extras.has("uihid") {   /* -uihid YES: the gadget as an AssistiveTouch pointer (hid_map mouse_rel) */
            mirror.hid = Extras.bool("uihid", false)
            UiShared.save("settings", mirror)
        }
        if Extras.has("uibands") {
            twoBands = Extras.bool("uibands", false)
        }
        if Extras.has("uiwarp") {
            warp = Extras.bool("uiwarp", true)
        }
        UiShared.observe(UiShared.notifyStarted) { [weak self] in
            mlog("ui: the broadcast extension started")
            guard let self else { return }
            self.broadcasting = true
            self.broadcastStartedAt = Date().timeIntervalSince1970
            self.broadcastNote = ""
            self.stopStream(.linkLost)
            /* It streams on its own connection: the app lets go of the gadget meanwhile. */
            self.app?.releaseLinkForBroadcast()
        }
        UiShared.observe(UiShared.notifyStopped) { [weak self] in
            guard let self else { return }
            self.broadcasting = false
            self.extStatus = UiShared.load("status", MirrorStatus.self)
            self.broadcastNote = self.extStatus?.state ?? ""
            mlog("ui: the broadcast extension stopped: \(self.broadcastNote)")
            self.app?.resumeLinkAfterBroadcast()
        }
    }

    var active: Bool { source != .off && source != .broadcast }

    func setSource(_ s: Source) {
        if s == source {
            if active && streamer == nil {
                app?.userStarted()   /* a source held for the proof: tapping it is the user's start */
                startIfReady()
            }
            return
        }
        if active && (s == .off || s == .broadcast) {
            app?.userStopped()   /* Off (or the broadcast): a UI start is no camera start */
        }
        stopStream()
        if source == .broadcast && broadcasting {
            UiShared.post(UiShared.notifyStop)
        }
        source = s
        Prefs.setInt("ui_source", s.rawValue)
        if active {
            app?.userStarted()
        }
        if s == .broadcast {
            saveMirror()
            if !broadcasting {
                app?.showBroadcastPicker()   /* picking it is starting it */
            }
        }
        startIfReady()
    }

    func setWarp(_ on: Bool) {
        if let c = controls, c.fisheye != on {
            c.setFisheye(on)   /* tells the gadget, and comes back here */
            return
        }
        applyWarp(on)
    }

    private func applyWarp(_ on: Bool) {
        warp = on
        Prefs.setInt("ui_warp", on ? 1 : 0)
        streamer?.warp = on && round
        streamer?.rerender()
        web?.markDirty()
        mirror.warp = on
        saveMirror()
        sendHidMap()
    }

    /* Gates injecting the gadget's touches; the touch events stay on during a
     * stream (the menu and panning need them). */
    func setTouch(_ on: Bool) {
        touchBack = on
        Prefs.setInt("ui_touch", on ? 1 : 0)
    }

    func setSharp(_ on: Bool) {
        if let c = controls, c.sharp != on {
            c.setSharp(on)
            return
        }
        applySharp(on)
    }

    private func applySharp(_ on: Bool) {
        let was = streamer?.sharp
        sharp = on
        Prefs.setInt("ui_sharp", on ? 1 : 0)
        guard let s = streamer else { return }
        s.sharp = on
        if on && was == false && s.stillPatches {
            s.requestKeyframe()   /* the patches track the gadget's picture from its first frame */
        }
    }

    /* None, Vertical or All (the UI tab, or the gadget's menu through the controls). */
    func setOverscroll(_ v: Int) {
        if let c = controls, c.mirror, c.overscroll != v {
            c.setOverscroll(v)   /* tells the gadget, and comes back here */
            return
        }
        applyOverscroll(v)
    }

    private func applyOverscroll(_ v: Int) {
        let b = UIScreen.main.bounds
        mirror.overscroll = max(0, min(2, v))
        mirror.clampCenter(w: b.width, h: b.height)
        saveMirror()
        sendHidMap()
        streamer?.rerender()
        mlog("ui: overscroll \(MirrorSettings.overscrollNames[mirror.overscroll ?? 1])")
    }

    /* The mirror window back to the screen's middle (its size kept). */
    func recenter() {
        mirror.recenter()
        UiShared.save("settings", mirror)
        UiShared.post(UiShared.notifySettings)
        sendHidMap()
        streamer?.rerender()
        mlog("ui: mirror window recentered")
    }

    /* The mirror window followed the finger (dx, dy in panel pixels): the other way. */
    private func panWindow(_ dx: Double, _ dy: Double) {
        let b = UIScreen.main.bounds
        let perPx = mirror.size * b.width / Double(max(1, n))   /* screen points per panel pixel */
        /* up to half the window past the screen's edges (MirrorSettings.crop) */
        mirror.cx -= dx * perPx / b.width
        mirror.cy -= dy * perPx / b.height
        mirror.clampCenter(w: b.width, h: b.height)
        UiShared.save("settings", mirror)
        sendHidMap()
        streamer?.rerender()
        if nowMs() - lastPanLogMs > 250 {
            lastPanLogMs = nowMs()
            mlog(String(format: "ui: window center %.3f, %.3f, crop %@", mirror.cx, mirror.cy,
                        "\(mirror.crop(w: Int(b.width), h: Int(b.height)))"))
        }
    }
    private var lastPanLogMs: Int64 = 0

    func setZoom(_ z: Double) {
        zoom = z
        Prefs.setInt("ui_zoom", Int(z * 100))
        if n > 0 {
            web?.setViewport(CGFloat(Double(n) * z))
        }
    }

    func setTwoBands(_ on: Bool) {
        twoBands = on
        Prefs.setBool("ui_bands", on)
        restart()
    }

    func load(_ u: String) {
        url = u
        Prefs.setString("ui_url", u)
        web?.load(u)
        if source != .web {
            setSource(.web)
        }
    }

    func saveMirror() {
        UiShared.save("settings", mirror)
        UiShared.post(UiShared.notifySettings)
    }

    /* The gadget the app uses (connected, or just picked) is the broadcast's gadget. */
    func gadgetChosen(address: String?, name: String?) {
        guard !Extras.has("uimirrorgadget") else { return }
        let a = address ?? "", nm = name ?? ""
        if (a.isEmpty && nm.isEmpty) || (a == mirror.gadget && nm == mirror.gadgetName) {
            return
        }
        mirror.gadget = a
        mirror.gadgetName = nm
        mlog("ui: the broadcast's gadget is now \(nm) \(a)")
        saveMirror()
    }

    /* The gadget's token proof key (from mg_ble_client's proof_key), "" to remove it. False: not a key. */
    @discardableResult
    func setProofKey(_ hex: String) -> Bool {
        let name = mirror.gadgetName
        guard !name.isEmpty else { return false }
        if hex.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty {
            UiShared.setProofKey(nil, for: name)
            mlog("ui: removed the proof key for \(name)")
        } else {
            guard let k = TokenProof.parseKey(hex) else {
                mlog("ui: not a proof key (64 hex digits) for \(name)")
                return false
            }
            UiShared.setProofKey(k, for: name)
            mlog("ui: proof key \(TokenProof.fingerprint(k)) for \(name)")
        }
        if let g = linkOf() as? GadgetLink, g.gadgetName() == name {
            pointerOff(g)   /* back on once the new key's proof matches */
            g.reprove()   /* the app's own link, with the new key */
        }
        return true
    }

    func proofKeyLine() -> String {
        let name = mirror.gadgetName
        guard !name.isEmpty else { return "Pick a gadget first" }
        guard let k = UiShared.proofKey(for: name) else {
            return "None for \(name): the screen mirror won't stream to it. Set it up with mg_ble_client, then paste its key"
        }
        return "Key \(TokenProof.fingerprint(k)) for \(name)"
    }

    /* This app's screen, its HID pointer and keys go only to a gadget that proved this phone's key on
     * this connection; nil when it has. */
    private func mirrorRefusal(_ l: VideoLink?) -> String? {
        guard let g = l as? GadgetLink else { return "No screen mirror: it needs the BLE link" }
        if g.proven {
            return nil
        }
        if !g.hasProofKey {
            return "No screen mirror: no proof key for \(g.gadgetName() ?? "the gadget"). Set it up with mg_ble_client, "
                + "then paste its key (Screen mirror, Proof key)"
        }
        return g.proofPending ? "Screen mirror waits for the gadget's token proof"
            : "No screen mirror: the gadget didn't prove this phone's key (\(lastProof))"
    }

    /* The app's link finished its token proof (main queue): what waited for it goes on. */
    func proofDone(_ o: TokenProof.Outcome?) {
        switch o {
        case .match: lastProof = "matched"
        case .mismatch: lastProof = "a different key"
        case .notFound: lastProof = "it isn't set up"
        case .refused(let code): lastProof = "error \(code)"
        case .recovery: lastProof = "it's in a recovery mode"
        case .timeout: lastProof = "no answer"
        case nil: lastProof = "it doesn't offer the token proof"
        }
        if source == .screenApp && streamer == nil {
            startIfReady()
        } else if o == .match {
            streamer?.requestKeyframe()   /* a re-prove may have cut a frame short */
        }
        sendHidMap()
    }

    /* The UI tab's and the now bar's broadcast button. */
    func startOrStopBroadcast() {
        if broadcasting || UIScreen.main.isCaptured {
            UiShared.post(UiShared.notifyStop)
        } else {
            if source != .broadcast {
                setSource(.broadcast)   /* opens the picker */
            } else {
                saveMirror()
                app?.showBroadcastPicker()
            }
        }
    }

    func setMirror(_ f: (inout MirrorSettings) -> Void) {
        f(&mirror)
        saveMirror()
        sendHidMap()
    }

    /* ---- the stream ---------------------------------------------------- */

    func startIfReady() {
        guard active, streamer == nil else { return }
        if offline > 0 {
            startStream()
            return
        }
        guard let l = linkOf(), l.ready() else { return }
        startStream()
    }

    func restart() {
        if streamer != nil {
            stopStream(.restart)
            startIfReady()
        }
    }

    private func startStream() {
        guard let l = offline > 0 ? VideoLink() : linkOf() else { return }
        mirrorNote = ""
        if source == .screenApp && offline == 0, let why = mirrorRefusal(l) {
            mirrorNote = why
            mlog("ui: " + why)
            return
        }
        (l as? GadgetLink)?.proofRequired = source == .screenApp
        app?.stopVideoForUi()
        let c = l.caps().snapshot()   /* this stream's own, clamped: a hello arriving changes nothing under it */
        if offline > 0 {
            c.uiW = offline
            c.uiH = offline
            c.uiFps = 15
            c.uiMaxFrame = Extras.int("uimaxframe", offline > 400 ? 48 * 1024 : 32 * 1024)
            c.uiBands = 2
            c.sugKbps = Extras.int("uikbps", 600)
            c.uiTouch = true
            c.uiControls = true
            c.uiStill = Extras.int("uistillcap", 2)
        }
        uiContent = c.uiW > 0 && c.uiH > 0
        round = offline > 0 || c.round
        var cfg = UiStreamer.config(for: c, twoBands: twoBands)
        n = cfg.n
        cfg.linkKbps = Extras.int("uikbps", cfg.linkKbps)
        if let q = Extras.string("uiqps") {   /* -uiqps 30,24,18: change, then the refinements */
            let v = q.split(separator: ",").compactMap { Int($0) }
            if let first = v.first {
                cfg.changeQps = [first]
                cfg.refine = zip([300, 1000], v.dropFirst()).map { ($0, $1) }
            }
        }
        cfg.x264 = Extras.string("uienc") != "vt"
        guard let s = UiStreamer(cfg, gpu: gpu) else {
            mlog("ui: no GPU warp")
            return
        }
        s.warp = warpOn
        s.mask = round
        s.output = self
        s.meter = meter
        s.sharp = sharp
        s.fastFps = mirror.fastFps ?? 30
        s.legacyPacing = mirror.legacyPacing ?? false
        s.stillPatches = (c.uiStill & (1 << Int(Mg.stillRgb565Zlib))) != 0 && Extras.int("uistill", 1) != 0
        let ctl = UiControls(n: n, mirror: source == .screenApp, fisheye: warpOn, sharp: sharp, round: round,
                             overscroll: mirror.overscroll ?? 1)
        let g0 = l as? GadgetLink
        ctl.send = { id, v in g0?.sendUiControl(id, v) }
        ctl.onFisheye = { [weak self] on in DispatchQueue.main.async { self?.applyWarp(on) } }
        ctl.onSharp = { [weak self] on in DispatchQueue.main.async { self?.applySharp(on) } }
        ctl.onStop = { [weak self] in DispatchQueue.main.async { self?.setSource(.off) } }
        ctl.onPan = { [weak self] dx, dy in DispatchQueue.main.async { self?.panWindow(dx, dy) } }
        ctl.onRedraw = { [weak self] in
            self?.streamer?.rerender()
            self?.web?.markDirty()
        }
        ctl.onTouch = { [weak self] a, x, y in DispatchQueue.main.async { self?.touch(action: a, px: x, py: y) } }
        ctl.onRecenter = { [weak self] in DispatchQueue.main.async { self?.recenter() } }
        ctl.onOverscroll = { [weak self] v in DispatchQueue.main.async { self?.applyOverscroll(v) } }
        ctl.navAvailable = { [weak self] in
            guard let self, let g = self.linkOf() as? GadgetLink else { return false }
            return self.mirror.hid && g.caps().hidMouseRel && g.proven
        }
        ctl.onNav = { [weak self] which in
            let usage = which == 0 ? 0x224 : 0x223
            guard let g = self?.linkOf() as? GadgetLink, g.proven else {
                mlog("ui: no HID key: the gadget hasn't proved this phone's key")
                return
            }
            g.sendHidKey(usage)
            mlog("ui: \(which == 0 ? "Back" : "Home") via the gadget's HID key 0x\(String(usage, radix: 16))")
        }
        controls = ctl
        s.overlay = { [weak ctl] p, size in ctl?.draw(p, size) ?? false }
        s.overlayActive = { [weak ctl] in ctl?.drawing ?? false }
        if source == .screenApp {
            s.cropProvider = { [weak self] w, h in self?.mirror.crop(w: w, h: h) ?? SIMD4<Float>(0, 0, 1, 1) }
        }
        s.onPreview = { [weak self] bgra, n in
            let img = Self.image(bgra, n)
            DispatchQueue.main.async { self?.preview = img }
        }
        streamer = s
        streamLink = l
        mlog("ui: \(uiContent ? "UI stream" : "no UI capability: as video at the camera's size") \(n)x\(n) from \(source)")
        s.start()
        switch source {
        case .web:
            let w = web ?? UiWebSource(viewport: 1)
            web = w
            if let win = app?.view.window {
                w.attach(to: win)
            }
            w.onLoaded = { [weak self] t in self?.pageTitle = t }
            w.setViewport(CGFloat(Double(n) * zoom))
            if w.web.url == nil {
                w.load(url)
            }
            w.start(s)
        case .screenApp:
            startRecorder()
        default:
            break
        }
        if let g = l as? GadgetLink {
            g.setTouchEvents(c.uiTouch)   /* always: the menu and panning need them; touchBack gates injection */
        }
        ctl.sendAll()
        mlog("ui: controls \(c.uiControls ? "on" : "not offered"), still patches \(s.stillPatches ? "on" : "off") (gadget formats \(c.uiStill)), quality \(sharp ? "sharp" : "fast")")
        sendHidMap()
        if Extras.bool("uitouchtest", false) {
            touchTest()
        }
        if Extras.bool("uimenutest", false) {
            menuTest()
        }
        if Extras.bool("uipantest", false) {
            panTest()
        }
    }

    /* -uimenutest YES: the gadget's menu button at 5 s, a tap on its last row (Close) at 7 s,
     * as notified, through the same path. */
    private func menuTest() {
        let n = self.n
        func at(_ t: Double, _ v: [UInt8]) {
            DispatchQueue.main.asyncAfter(deadline: .now() + t) { [weak self] in self?.onUiInput(v, from: self?.streamLink) }
        }
        at(5, [Mg.cmdUiInput, Mg.uiControl, Mg.ctlMenu, 1])
        let x = n / 2, y = n * 72 / 100   /* the last row on a 4- or 5-row menu */
        at(7, [Mg.cmdUiInput, Mg.uiTouch, 0, UInt8(x & 0xff), UInt8(x >> 8), UInt8(y & 0xff), UInt8(y >> 8), 0, 0, 0])
        at(7.1, [Mg.cmdUiInput, Mg.uiTouch, 2, UInt8(x & 0xff), UInt8(x >> 8), UInt8(y & 0xff), UInt8(y >> 8), 0, 0, 0])
    }

    /* -uipantest YES (the mirror): a tap in the edge ring (id 0: the gadget said tap), a pan
     * gesture (id 0x80) dragging right and down (overscroll as set), the gadget setting
     * overscroll All and the same drag again, then its menu's Recenter, all as notified. */
    private func panTest() {
        let n = self.n
        func ev(_ t: Double, _ a: Int, _ x: Int, _ y: Int, _ id: UInt8) {
            let v: [UInt8] = [Mg.cmdUiInput, Mg.uiTouch, UInt8(a), UInt8(x & 0xff), UInt8(x >> 8), UInt8(y & 0xff), UInt8(y >> 8), 0, 0, id]
            DispatchQueue.main.asyncAfter(deadline: .now() + t) { [weak self] in self?.onUiInput(v, from: self?.streamLink) }
        }
        func drag(_ t: Double) {
            for k in 0...20 {
                ev(t + Double(k) * 0.03, k == 0 ? 0 : 1, n / 4 + k * n / 40, n / 4 + k * n / 40, Mg.touchIdPan)
            }
            ev(t + 0.7, 2, n / 4 + n / 2, n / 4 + n / 2, Mg.touchIdPan)
        }
        func report(_ t: Double, _ what: String) {
            DispatchQueue.main.asyncAfter(deadline: .now() + t) { [weak self] in
                guard let self else { return }
                let b = UIScreen.main.bounds
                mlog("ui: pan test: \(what): overscroll \(self.mirror.overscroll ?? 1), center \(String(format: "%.3f, %.3f", self.mirror.cx, self.mirror.cy)), crop \(self.mirror.crop(w: Int(b.width), h: Int(b.height)))")
            }
        }
        ev(4.0, 0, n / 2, 6, 0)
        ev(4.1, 2, n / 2, 6, 0)
        report(4.5, "start")
        drag(5.0)
        report(7.0, "after a drag")
        DispatchQueue.main.asyncAfter(deadline: .now() + 7.5) { [weak self] in
            self?.onUiInput([Mg.cmdUiInput, Mg.uiControl, Mg.ctlOverscroll, 2], from: self?.streamLink)
        }
        drag(8.0)
        report(10.0, "after a drag with All")
        DispatchQueue.main.asyncAfter(deadline: .now() + 11) { [weak self] in
            self?.onUiInput([Mg.cmdUiInput, Mg.uiControl, Mg.ctlMenu, 1], from: self?.streamLink)
        }
        /* the Pan | Recenter line: 5 lines without the nav row (Fisheye, Pan|Recenter, Overscroll, Quality, Stop|Close) */
        let rowH = Double(n) * 0.105, gap = Double(n) * 0.02, top = (Double(n) - 5 * rowH - 4 * gap) / 2 + Double(n) * 0.02
        let y = Int(top + 1 * (rowH + gap) + rowH / 2), x = Int(Double(n) * 0.66)
        ev(13.0, 0, x, y, 0)
        ev(13.1, 2, x, y, 0)
        report(14.0, "after Recenter")
        DispatchQueue.main.asyncAfter(deadline: .now() + 14.5) { [weak self] in
            self?.onUiInput([Mg.cmdUiInput, Mg.uiControl, Mg.ctlOverscroll, 1], from: self?.streamLink)
        }
    }

    /* -uitouchtest YES: gadget touches made up on the phone (as notified, through
     * the same path): taps at the center and upper left, a drag up the middle. */
    private func touchTest() {
        let n = self.n
        func ev(_ at: Double, _ a: Int, _ x: Int, _ y: Int) {
            DispatchQueue.main.asyncAfter(deadline: .now() + at) { [weak self] in
                let v: [UInt8] = [Mg.cmdUiInput, Mg.uiTouch, UInt8(a), UInt8(x & 0xff), UInt8(x >> 8), UInt8(y & 0xff), UInt8(y >> 8), 0, 0, 0]
                self?.onUiInput(v, from: self?.streamLink)
            }
        }
        ev(4.0, 0, n / 2, n / 2)
        ev(4.1, 2, n / 2, n / 2)
        for k in 0...10 {
            ev(6.0 + Double(k) * 0.03, k == 0 ? 0 : 1, n / 2, n * 3 / 4 - k * n / 20)
        }
        ev(6.4, 2, n / 2, n / 4)
        ev(8.0, 0, n * 3 / 10, n * 3 / 10)
        ev(8.1, 2, n * 3 / 10, n * 3 / 10)
    }

    func stopStream(_ why: UiStreamEnd = .userStop) {
        endTouches(why)
        stopRecorder()
        mirrorNote = ""
        if let g = linkOf() as? GadgetLink {
            pointerOff(g)
            if g.ready() {
                g.setTouchEvents(false)
            }
        }
        streamer?.stop()
        streamer = nil
        (linkOf() as? GadgetLink)?.proofRequired = false   /* after the producer stops */
        controls = nil
        preview = nil
    }

    /* The gadget's link went away or stopped the stream, or a start waits for a retry on it (why). */
    func linkLost(_ why: UiStreamEnd = .linkLost) {
        endTouches(why)
        stopRecorder()
        streamer?.stop()
        streamer = nil
        (linkOf() as? GadgetLink)?.proofRequired = false   /* after the producer stops */
        controls = nil
    }

    /* The stream ends: no touch reaches anything after this (no link, no panel size, no gesture). On link
     * loss, a disconnect or the user's stop the web page is unloaded too, not kept in the window with the
     * user's session behind it; a retry on the same link keeps it. */
    private func endTouches(_ why: UiStreamEnd) {
        streamLink = nil
        n = 0
        controls = nil   /* the gesture and the menu */
        if why.unloadsPage {
            web?.detach()
            web = nil
        } else {
            web?.stop()
        }
    }

    func keyframe() {
        streamer?.requestKeyframe()
    }

    /* UiOutput: the app's gadget link. */
    func uiStart(w: Int, h: Int, fps: Int, bands: Int) {
        if offline > 0 {
            let url = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0].appendingPathComponent("ui.h264")
            FileManager.default.createFile(atPath: url.path, contents: nil)
            dump = try? FileHandle(forWritingTo: url)
            mlog("ui: offline \(w)x\(h), \(bands) band(s) into \(url.path)")
            return
        }
        guard let l = linkOf() else { return }
        if uiContent {
            l.startUiStreaming(w: w, h: h, fps: fps, kbps: 1000, bands: bands)
        } else {
            l.startStreaming(w: w, h: h, fps: fps, kbps: 600, playoutMs: 0, bands: bands)
        }
    }

    func uiStop() {
        if offline > 0 {
            try? dump?.close()
            dump = nil
            return
        }
        linkOf()?.stopStreaming()
    }

    func uiSend(_ frame: Data) {
        if offline > 0 {
            if frame[frame.startIndex + 1] & VideoFrame.still != 0 {
                /* still patches beside the stream, whole frames (tools/uistill_check.py) */
                let url = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0].appendingPathComponent("ui_still.bin")
                if stillDump == nil {
                    FileManager.default.createFile(atPath: url.path, contents: nil)
                    stillDump = try? FileHandle(forWritingTo: url)
                }
                stillDump?.write(frame)
                dumpBusyUntilNs = max(nowNs(), dumpBusyUntilNs) + Int64(frame.count) * 8 * 1_000_000 / Int64(max(100, Extras.int("uikbps", 600)))
                return
            }
            /* the first band only (ffmpeg plays one stream); the link's time for the whole frame */
            let p = frame.dropFirst(VideoFrame.headerLen)
            if twoBands, p.count > 4 {
                let b = p.startIndex
                let l0 = Int(p[b]) | Int(p[b + 1]) << 8 | Int(p[b + 2]) << 16 | Int(p[b + 3]) << 24
                dump?.write(p.subdata(in: (b + 4)..<(b + 4 + l0)))
            } else {
                dump?.write(Data(p))
            }
            let kb = Int64(max(100, Extras.int("uikbps", 600)))
            dumpBusyUntilNs = max(nowNs(), dumpBusyUntilNs) + Int64(frame.count) * 8 * 1_000_000 / kb
            return
        }
        linkOf()?.sendFrame(frame)
    }

    func uiQueued() -> Int64 {
        if offline > 0 {
            return dumpBusyUntilNs > nowNs() ? 1 : 0
        }
        return linkOf()?.queuedBytes() ?? 0
    }

    static func image(_ bgra: Data, _ n: Int) -> UIImage? {
        guard let prov = CGDataProvider(data: bgra as CFData),
              let cg = CGImage(width: n, height: n, bitsPerComponent: 8, bitsPerPixel: 32, bytesPerRow: n * 4,
                               space: CGColorSpaceCreateDeviceRGB(),
                               bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo.noneSkipFirst.rawValue | CGBitmapInfo.byteOrder32Little.rawValue),
                               provider: prov, decode: nil, shouldInterpolate: true, intent: .defaultIntent) else { return nil }
        return UIImage(cgImage: cg)
    }

    /* ---- the app's own screen (RPScreenRecorder) ------------------------ */

    private func startRecorder() {
        let r = RPScreenRecorder.shared()
        r.isMicrophoneEnabled = false
        recording = true
        recLock.lock()
        recGen += 1
        let gen = recGen
        recLock.unlock()
        weak var g = linkOf() as? GadgetLink
        r.startCapture(handler: { [weak self, weak s = streamer] sb, type, err in
            /* Only to this start's streamer, while its gadget holds this connection's proof. */
            guard let self, type == .video, let pb = CMSampleBufferGetImageBuffer(sb), let s, g?.proven == true else { return }
            self.recLock.lock()
            let current = gen == self.recGen
            self.recLock.unlock()
            guard current else { return }
            s.submit(pb, crop: self.mirror.crop(w: CVPixelBufferGetWidth(pb), h: CVPixelBufferGetHeight(pb)))
        }, completionHandler: { err in
            if let e = err {
                mlog("ui: screen capture failed: \(e.localizedDescription)")
                DispatchQueue.main.async { [weak self] in
                    guard let self, self.source == .screenApp else { return }
                    self.recLock.lock()
                    let current = gen == self.recGen
                    self.recLock.unlock()
                    guard current else { return }   /* an earlier start's answer */
                    self.recording = false
                    self.stopStream()   /* the pointer goes off too */
                    self.mirrorNote = (e as NSError).code == RPRecordingErrorCode.userDeclined.rawValue
                        ? "Screen capture not allowed" : "Screen capture failed: \(e.localizedDescription)"
                }
            } else {
                mlog("ui: capturing the app's screen")
            }
        })
    }

    private func stopRecorder() {
        recLock.lock()
        recGen += 1   /* pictures from a capture still starting go nowhere */
        recLock.unlock()
        if recording {
            recording = false
            RPScreenRecorder.shared().stopCapture { _ in }
        }
    }

    /* ---- the gadget's touches ------------------------------------------- */

    /* [ui_input, ...] from the gadget (BLE queue), from link l: touches and controls count only from the
     * running stream's link (Android's UiSession does the same). */
    func onUiInput(_ v: [UInt8], from l: VideoLink?) {
        guard v.count >= 2 else { return }
        if v[1] == Mg.uiTouch && v.count >= 8 {
            let action = Int(v[2]), x = Mg.u16(v, 3), y = Mg.u16(v, 5)
            DispatchQueue.main.async {
                guard self.streamer != nil, let sl = self.streamLink, sl === l else { return }
                if self.source == .screenApp && self.offline == 0 && (l as? GadgetLink)?.proven != true {
                    self.lastTouch = "the gadget hasn't proved its key (ignored)"
                    return
                }
                if let c = self.controls {
                    c.touch(action: action, x: x, y: y, id: v.count >= 10 ? Int(v[9]) : 0)
                } else {
                    self.touch(action: action, px: x, py: y)
                }
            }
        } else if v[1] == Mg.uiControl && v.count >= 4 {
            let id = v[2], value = Int(v[3])
            DispatchQueue.main.async {
                guard self.streamer != nil, let sl = self.streamLink, sl === l else { return }
                self.controls?.gadgetControl(id, value)
            }
        } else if v[1] == Mg.uiHidStatus && v.count >= 4 {
            let mode = Int(v[2]), f = Int(v[3])
            let s = "mode \(["off", "digitizer", "mouse abs", "mouse rel"][min(3, mode)])"
                + (f & 1 != 0 ? ", bonded" : ", not bonded") + (f & 2 != 0 ? ", host on" : ", no host")
            mlog("ui: hid_status " + s)
            DispatchQueue.main.async { self.hidStatus = s }
        }
    }

    private func touch(action: Int, px: Int, py: Int) {
        guard n > 0, streamer != nil, streamLink != nil else { return }
        let (sx, sy) = UiGeometry.squareFromPanel(px: Double(px), py: Double(py), n: n, warp: warpOn)
        lastTouch = String(format: "%@ at %d,%d -> %.3f, %.3f", ["down", "move", "up"][min(2, action)], px, py, sx, sy)
        if action != 1 {
            mlog("ui: touch " + lastTouch)
        }
        if source == .web && touchBack {
            web?.touch(action: action, sx: sx, sy: sy)
        }
        if let c = calib, action == 0 {
            /* where the finger was, in screen points, for the calibration */
            /* during calibration the panel maps to the whole screen */
            let b = UIScreen.main.bounds
            calib = (CGPoint(x: sx * b.width, y: sy * b.height), c.start)
        }
    }

    /* ---- HID ------------------------------------------------------------- */

    /* hid_map for the mirror window (relative mouse, iOS AssistiveTouch). Only while the
     * screen itself streams (the web page takes the gadget's touches directly): HID needs a
     * pairing, and a gadget with HID on shows its HID service, so iOS asks to pair on every
     * connection (on the Mosaico, a saved HID pointer setting did that for web page
     * streams). Android's UiSession does the same. */
    /* Screen control ends: the gadget keeps its HID map in flash, so its pointer goes off. */
    private func pointerOff(_ g: GadgetLink) {
        guard source == .screenApp || calib != nil, g.ready(), g.proven, g.caps().hidMouseRel else { return }
        g.sendHidMap(mode: 0, x: 0, y: 0, w: 0, h: 0, fisheye: false)
    }

    func sendHidMap() {
        guard let g = linkOf() as? GadgetLink, g.ready(), g.caps().hidMouseRel else { return }
        /* The phone's pointer only for a gadget that proved this phone's key on this connection. */
        if !mirror.hid || (source != .screenApp && calib == nil) || !g.proven {
            g.sendHidMap(mode: 0, x: 0, y: 0, w: 0, h: 0, fisheye: false)
            return
        }
        let b = UIScreen.main.bounds
        let c = calib != nil ? SIMD4<Float>(0, 0, 1, 1) : mirror.crop(w: Int(b.width), h: Int(b.height))
        let cw = mirror.hidCountsW > 0 ? mirror.hidCountsW : Int(b.width), ch = mirror.hidCountsH > 0 ? mirror.hidCountsH : Int(b.height)
        let q = GadgetLink.hidSquare(c)
        g.sendHidMap(mode: 3, x: q.x, y: q.y, w: q.w, h: q.h, fisheye: warpOn, countsW: cw, countsH: ch, offset: q.offset)
        mlog("ui: hid_map mouse_rel window \(c) warp \(warpOn) counts \(cw)x\(ch)" + (q.offset ? " (past the screen's edges)" : ""))
    }

    /*
     * Calibration (the counts the pointer moves across the screen at the
     * host's tracking speed): with HID on and the window the whole screen,
     * the user taps the gadget; the gadget parks the pointer top-left and moves
     * it to where the finger points using the current counts; AssistiveTouch
     * clicks there, which lands as a tap in this app (calibrationTap). The
     * ratio of where it should have landed (the finger, through the inverse
     * warp) to where it did scales the counts. Two or three taps converge.
     */
    func startCalibration() {
        calib = (CGPoint(x: -1, y: -1), nowMs())
        if !mirror.hid {
            mirror.hid = true
        }
        sendHidMap()
    }

    var calibrating: Bool { calib != nil }
    var calibPending: Bool { (calib?.expect.x ?? -1) >= 0 }

    func calibrationTap(_ p: CGPoint) {
        guard let c = calib, c.expect.x >= 0 else { return }
        let b = UIScreen.main.bounds
        let cw0 = Double(mirror.hidCountsW > 0 ? mirror.hidCountsW : Int(b.width))
        let ch0 = Double(mirror.hidCountsH > 0 ? mirror.hidCountsH : Int(b.height))
        /* counts needed = counts used * expected / landed (both from the top-left corner) */
        let kx = Double(c.expect.x) / max(4, Double(p.x)), ky = Double(c.expect.y) / max(4, Double(p.y))
        mirror.hidCountsW = Int(cw0 * kx)
        mirror.hidCountsH = Int(ch0 * ky)
        let msg = String(format: "expected %.0f,%.0f, landed %.0f,%.0f: counts %d x %d", c.expect.x, c.expect.y, p.x, p.y,
                         mirror.hidCountsW, mirror.hidCountsH)
        mlog("ui: calibration " + msg)
        calib = (CGPoint(x: -1, y: -1), c.start)
        saveMirror()
        sendHidMap()
        onCalibrated?(msg)
    }

    func endCalibration() {
        calib = nil
        sendHidMap()
    }

    /* ---- for the screen -------------------------------------------------- */

    func tick() {
        if let s = streamer {
            let st = s.snapshot(), now = nowMs()
            if lastStatMs > 0 {
                kbps = Double(st.bytes - lastStatBytes) * 8 / Double(max(1, now - lastStatMs))
            }
            lastStatBytes = st.bytes
            lastStatMs = now
            mlog(String(format: "ui stats: %.1f kb/s, frames %d (changes %d, refines %d, keys %d, oversize %d), last %@ qp %d %d B %.2f dB enc %.1f ms; steps %@ B, %@ dB; snapshots %d (%.1f ms)",
                        kbps, st.frames, st.changes, st.refines, st.keys, st.oversize, st.lastKind, st.lastQp, st.lastBytes,
                        st.lastPsnr, st.lastEncMs, st.stepBytes.map { String($0) }.joined(separator: "/"),
                        st.stepPsnr.map { String(format: "%.1f", $0) }.joined(separator: "/"),
                        web?.statSnapshots ?? 0, (web?.statSnapshotMs ?? 0) / Double(max(1, web?.statSnapshots ?? 1))))
        }
        if source == .broadcast || broadcasting {
            extStatus = UiShared.load("status", MirrorStatus.self)
            let now = Date().timeIntervalSince1970
            if broadcasting, now - broadcastStartedAt > 6, let s = extStatus,
               (s.ended && s.at >= broadcastStartedAt) || now - s.at > 6 {
                /* it ended without telling us (killed, crashed) */
                broadcasting = false
                broadcastNote = s.ended ? s.state : "The broadcast stopped"
                app?.resumeLinkAfterBroadcast()
            }
        }
    }

    func statsLine() -> String {
        guard let s = streamer else {
            return source == .off ? "Off" : !mirrorNote.isEmpty ? mirrorNote : "Waiting for the gadget"
        }
        let st = s.snapshot()
        return String(format: "%.1f kb/s · %d frames · %d B at QP %d · %.1f dB", kbps, st.frames, st.lastBytes, st.lastQp, st.lastPsnr)
    }

    func stepsLine() -> String {
        guard let s = streamer else { return "-" }
        let st = s.snapshot()
        return zip(st.stepBytes, st.stepPsnr).map { String(format: "%d B %.1f dB", $0, $1) }.joined(separator: " → ")
    }

    func label() -> String {
        switch source {
        case .web: return "Web page" + (pageTitle.isEmpty ? "" : ": " + pageTitle)
        case .screenApp: return "This app's screen"
        case .broadcast: return "Screen (broadcast)"
        case .off: return "Off"
        }
    }

    func extensionLine() -> String {
        if broadcasting, let s = extStatus, Date().timeIntervalSince1970 - s.at < 6 {
            return String(format: "Broadcasting: %@ · %.1f kb/s · %.0f MB · CPU %.0f%%", s.state, s.kbps, s.memMB, s.cpuPct)
        }
        if broadcasting {
            return "Broadcasting: starting"
        }
        let to = mirror.gadgetName.isEmpty ? "no gadget picked" : "to " + mirror.gadgetName
        return "Not broadcasting (\(to))" + (broadcastNote.isEmpty ? "" : ". Last time: " + broadcastNote)
    }
}

private extension Float {
    var d: Double { Double(self) }
}
