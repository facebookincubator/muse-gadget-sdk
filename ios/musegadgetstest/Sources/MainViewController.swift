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
import AVFoundation
import ReplayKit
import Security
import UIKit

/*
 * Muse Gadgets Test: streams the phone's camera (or color bars, or a
 * sample clip) to a gadget's display over Wi-Fi (protocols/VIDEO.md), as the
 * Android app does: the gadget's suggested size and bitrate, an adaptive
 * bitrate for the camera, a bitrate ladder for clips, H.264 or VP8, and the
 * same screen (AppScreen: the preview with the stats over it, above tabs of
 * settings-style lists, Tabs). Launch arguments as Android's extras (Util.swift Extras):
 * -target A1B2C3 -host 192.168.1.20 -fps 24 -kbps 1000 -auto YES -front YES
 * -codec vp8 -clip bunny -source pattern ...
 *
 * Over musegadgets BLE instead (GadgetLink, mgcommands.h): -link ble, or the
 * Gadget tab's Connect over; the choice is kept. BLE bench options as Android's: -device
 * <identifier>, -tone YES (an SBC loop beside the video; -tonecodec lc3),
 * -mic sbc|lc3 (capture the gadget's mic too, to Documents/mic.<codec>),
 * -bench SECS (raw throughput), -l2cap YES, -air BYTES, -wmax BYTES.
 */
final class MainViewController: UIViewController, VideoLinkListener, CameraSink, ClipSink {
    /* Background parking of the gadget's link (appBackground, below). */
    var inBackground = false, parked = false
    var parkWork: DispatchWorkItem?

    private var screen: AppScreen!
    private var previewLayer: AVCaptureVideoPreviewLayer?
    private var link: VideoLink!
    private var cam: CameraStreamer!
    private var target: String?, deviceAddr: String?
    /* BLE (GadgetLink) instead of Wi-Fi: -link ble|wifi, or the Gadget tab's Connect over. */
    private var ble = false
    private var prefAddr: String { ble ? "device_addr" : "wifi_addr" }
    private var prefName: String { ble ? "device_name" : "wifi_name" }
    private var prefOff: String { ble ? "gadget_off" : "wifi_gadget_off" }
    /* Disconnect (the app bar's button, the Gadget tab): the user let the gadget go. No link,
     * no scanning and no reconnecting (after a drop, coming back to the foreground or a
     * relaunch) until they pick a gadget or press Connect. The last gadget stays remembered,
     * for Connect. Remembered across launches. */
    private(set) var gadgetOff = false
    private var linkStarted = false
    private var lastAddr: String?, lastName: String?
    private var explicitTarget = false   /* the launch named a gadget (-target, -host, -device) */
    private var bleLink: GadgetLink? { link as? GadgetLink }
    private var air = 3072, benchSecs = 0, wmax = 495
    private var airSet = false, l2cap = false, tone = false
    private var mic: String?, toneCodec: String?
    private var lastDataIn: Int64 = 0, lastAudio: Int64 = 0
    private var micStarted = false
    /* Sent once this connection's token proof ends (a set-up gadget refuses them before). */
    private var micAfterProof = false, volumeAfterProof: Int?
    private var uiAudioKbps = 0.0, uiMicKbps = 0.0
    private var deviceLine = ""
    private var fps = 30, kbps = 450, size = 0
    private var noBands = false
    /* The Codec button (or -codec h264|vp8): VP8 for the camera and the
     * clips where the gadget lists it and the app has libmgvpx, else H.264. Remembered. */
    private var wantVp8 = false, wantVp8Set = false
    private var baseSw: String?   /* the H.264 encoder: x264, or nil for the phone's */
    private var camVp8 = false    /* the camera stream that's on is VP8 */
    private var auto = false, front = false, adaptive = true
    /* The user's own start (a pick, a UI source, a codec change, Start automatically turned on), for
     * one connection; a stream that starts on its own on another waits for the token proof. Main queue. */
    private var grant = StartGrant()
    /* A start the gadget refused until the proof (the connection's epoch, -1: none): asked again on a match. */
    private var startAfterProof = -1
    private var userStart: Bool { grant.held }

    /* The user picked a UI source (UiController). */
    func userStarted() {
        grant.grant()
    }

    /* The user turned the UI source off (UiController): the start went with it. */
    func userStopped() {
        grant.revoke()
    }
    /* What streams: "camera", "pattern" or "clip:<name>". */
    private var source = "camera"
    static let clipPrefix = "clip:"
    private var player: ClipStreamer?
    private let clipsLock = NSLock()
    private var clips: [String: Clip] = [:]
    private var failedClips: Set<String> = []
    private var failReasons: [String: String] = [:]   /* why a clip failed, by name, for its row */
    private let clipWake = NSCondition()
    private var clipThread: Thread?
    private var clipSize = 232, clipRound = true
    private var clipTopKbps = 450, clipKbpsSet = false
    private var clipKbps = 450
    private var lastHeldMs: Int64 = 0
    private var heldSeconds = 0, clipCalmSeconds = 0
    private var lastSize = 232
    private var wanted = false
    private var linkStatus = ""
    /* The link's state in a line, for the Gadget tab and the app bar. */
    private var connStatus = ""
    private var uiSkipped = 0, uiDropped = 0
    private var devAvail: Int64 = 0, devRecv: Int64 = 0
    private var devShown = 0, devDropped = 0, devMs = 0
    private let meter = LatencyMeter()
    private var live = true
    private var aheadPct = 250, behindPct = 100
    private var lastStatMs: Int64 = 0, lastBytes: Int64 = 0, lastCamEnc: Int64 = 0, lastEncUs: Int64 = 0
    private var lastConv: Int64 = 0, lastGate: Int64 = 0
    private var lastFed = 0, lastEnc = 0, lastShown = 0
    private var curFps = 30
    private var playoutMs = 200, playoutSet = false
    private var maxKbps = 650, minKbps = 200
    private var kbpsSet = false, maxKbpsSet = false
    /* The Rate button: fixed bitrates for clips and camera (0: automatic). Remembered. */
    private var fixedClipKbps = 0, fixedCamKbps = 0
    static let camRates = [150, 200, 250, 300, 400, 500, 650, 800, 1000, 1200]
    static let sizes = [240, 320, 480]
    private var lastSkipped = 0
    private var lastGateUs: Int64 = 0
    private var calmSeconds = 0, gadgetBehindSeconds = 0
    private var seen: [String: (name: String, rssi: Int, at: Int64)] = [:]
    /* The picker's rows in a fixed order while it's open: the connected (or last
     * used) gadget first, then those already seen by name, then each new one at
     * the end. Signal and name update a row in place; a gadget that stops
     * advertising stays, greyed. Only Rescan rebuilds the order. */
    private var pickerOrder: [String] = []
    private var pickerTimer: Timer?
    private static let pickerStaleMs: Int64 = 15000
    private var picker: PickerController?
    private var linkReleased = false
    /* The UI tab: web pages and the screen at the gadget's size (protocols/UI.md). */
    private(set) lazy var ui: UiController = {
        let u = UiController(app: self) { [weak self] in self?.link }
        u.meter = meter
        return u
    }()

    private func setLiveBudget() {
        let frame = Int64(kbps) * 1000 / 8 / Int64(max(1, curFps))
        link.liveAhead = live && !isClip() ? max(1600, frame * Int64(aheadPct) / 100) : 0
    }

    override var prefersStatusBarHidden: Bool { false }
    override var preferredStatusBarStyle: UIStatusBarStyle { .lightContent }

    override func loadView() {
        screen = AppScreen(app: self)
        view = screen
    }

    override func viewDidLoad() {
        super.viewDidLoad()
        overrideUserInterfaceStyle = .dark
        readExtras()
        if !Extras.has("front") {
            front = Prefs.bool("front")
        }
        screen.overlay.mode = StatsOverlay.Mode(rawValue: Prefs.int("overlay")) ?? .compact
        cam = CameraStreamer(sink: self)
        let pl = AVCaptureVideoPreviewLayer(session: cam.session)
        pl.videoGravity = .resizeAspectFill
        screen.previewHost.layer.addSublayer(pl)
        previewLayer = pl
        if let t = Extras.string("tab") {   /* -tab gadget: open on that tab (for screenshots) */
            screen.select(screen.tabIndex(t.capitalized))
        }
        begin()
        Timer.scheduledTimer(withTimeInterval: 1, repeats: true) { [weak self] _ in self?.tick() }
        if ui.offline > 0 {   /* -uioffline: no link, no gadget to start for */
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) { [weak self] in self?.ui.startIfReady() }
        }
        if Extras.bool("uibroadcast", false) {   /* opens the broadcast picker (scripts press its Start) */
            DispatchQueue.main.asyncAfter(deadline: .now() + 2) { [weak self] in self?.showBroadcastPicker() }
        }
    }

    override func viewDidLayoutSubviews() {
        super.viewDidLayoutSubviews()
        previewLayer?.frame = screen.previewHost.bounds
        if let c = previewLayer?.connection, c.isVideoRotationAngleSupported(90) {
            c.videoRotationAngle = 90
        }
    }

    private func readExtras() {
        if let t = Extras.string("target") {
            target = t
            deviceAddr = nil
            explicitTarget = true
        }
        if let k = Extras.string("link") {
            ble = k == "ble"
            Prefs.setString("link", ble ? "ble" : "wifi")
        }
        if let h = Extras.string("host") {
            deviceAddr = h
        }
        if let d = Extras.string("device") {
            deviceAddr = d
        }
        explicitTarget = explicitTarget || Extras.has("host") || Extras.has("device")
        mic = Extras.string("mic")
        airSet = Extras.has("air")
        air = Extras.int("air", air)
        benchSecs = Extras.int("bench", 0)
        l2cap = Extras.bool("l2cap", false)
        tone = Extras.bool("tone", false)
        toneCodec = Extras.string("tonecodec")
        wmax = Extras.int("wmax", wmax)
        fps = Extras.int("fps", fps)
        kbpsSet = Extras.has("kbps")
        kbps = Extras.int("kbps", kbps)
        size = Extras.int("size", size)
        noBands = Extras.bool("nobands", false)
        if Extras.has("codec") || Extras.string("sw") == "vp8" {
            wantVp8 = Extras.string("codec")?.lowercased() == "vp8" || Extras.string("sw") == "vp8"
            wantVp8Set = true
        }
        auto = Extras.bool("auto", auto)
        front = Extras.bool("front", front)
        if Extras.bool("pattern", false) { source = "pattern" }
        if let s = Extras.string("source") { source = s }
        if let c = Extras.string("clip") { source = Self.clipPrefix + c }
        clipTopKbps = Extras.int("clipkbps", clipTopKbps)
        clipKbpsSet = Extras.has("clipkbps")
        clipKbps = clipTopKbps
        playoutSet = Extras.has("playout")
        playoutMs = Extras.int("playout", playoutMs)
        maxKbpsSet = Extras.has("maxkbps")
        maxKbps = Extras.int("maxkbps", maxKbps)
        adaptive = Extras.bool("adaptive", true)
        meter.perFrame = Extras.bool("latlog", false)
        live = Extras.int("live", 1) != 0
        aheadPct = Extras.int("ahead", aheadPct)
        behindPct = Extras.int("behind", behindPct)
    }

    private func begin() {
        fixedClipKbps = Prefs.int("clip_kbps")
        fixedCamKbps = Prefs.int("cam_kbps")
        if !Extras.has("size") { size = Prefs.int("size") }
        if !Extras.has("fps") { fps = Prefs.int("fps", fps) }
        if Extras.has("rateclip") { setClipRate(Extras.int("rateclip", 0)) }
        if Extras.has("ratec") { setCamRate(Extras.int("ratec", 0)) }
        if !wantVp8Set { wantVp8 = Prefs.bool("codec_vp8") }
        if !Extras.has("link") {
            ble = Prefs.string("link") == "ble"
        }
        if target == nil && deviceAddr == nil {
            deviceAddr = Prefs.string(prefAddr)
            target = Prefs.string(prefName)
        }
        lastAddr = deviceAddr
        lastName = target
        gadgetOff = !explicitTarget && Prefs.bool(prefOff)
        if explicitTarget {
            Prefs.setBool(prefOff, false)
        }
        if gadgetOff {
            grant.close()   /* nothing starts until a gadget is picked */
            mlog("disconnected by the user last time: not connecting to \(lastName ?? lastAddr ?? "?") until a gadget is picked")
            deviceAddr = nil
            target = nil
            connStatus = "Not connected"
        }
        makeLink()
        if Extras.bool("auto", false) {
            grant.grant()   /* a bench's -auto YES: the first connection */
        }
        cam.front = front
        cam.meter = meter
        cam.panelFilter = Extras.int("pf", 1) != 0
        /* The software encoder when this build has it (native/build_x264.sh;
         * -sw hw: the phone's encoder regardless). */
        let sw = Extras.string("sw")
        cam.swEncoder = sw != nil && sw != "vp8" ? (sw == "hw" ? nil : sw) : (X264.available ? "x264" : nil)
        baseSw = cam.swEncoder
        if let p = Extras.string("swpreset") { cam.swPreset = p }
        cam.swParams = Extras.string("swparams") ?? "no-deblock=1"
        cam.minAeFps = Extras.int("aefps", 30)
        mlog("Muse Gadgets Test: x264 \(X264.available ? "built in" : "not built in"), VP8 \(Vpx.available ? "built in" : "not built in")")
        if !gadgetOff {
            startLink()
        }
    }

    private func makeLink() {
        grant.linkRemade()   /* closing a link reports no disconnect */
        wanted = false
        micAfterProof = false
        askAfterProof = false
        volumeAfterProof = nil
        if ble {
            let g = GadgetLink(listener: self)
            g.airCap = Int64(air)
            g.bufferOverride = Int64(Extras.int("vbuf", 0))   /* bench: a small gadget's ring */
            g.writeMax = wmax
            g.useL2cap = l2cap
            /* The token proof with the key pasted for the gadget (UI tab), every connection; only the
             * screen mirror needs its match (UiController), and the gadget's own gates (mic, settings). */
            g.proofRequired = false
            g.proofKeyFor = { UiShared.proofKey(for: $0) }
            g.onProof = { [weak self, weak g] o in DispatchQueue.main.async { self?.onProof(o, on: g) } }
            g.onCommandError = { [weak self, weak g] forCmd, code in
                DispatchQueue.main.async { self?.onCommandError(forCmd, code, on: g) }
            }
            g.onUiInput = { [weak self, weak g] v in self?.ui.onUiInput(v, from: g) }
            g.onDeviceLog = { [weak self] line in
                if line.contains("video") || line.contains("mg.ble") {
                    mlog("gadget: " + line.trimmingCharacters(in: .whitespaces))
                    if line.contains("fps decoded") {
                        DispatchQueue.main.async { self?.deviceLine = line.trimmingCharacters(in: .whitespaces) }
                    }
                }
            }
            link = g
        } else {
            link = WifiLink(listener: self)
        }
        link.setTarget(address: deviceAddr, name: target)
        link.meter = meter
    }

    /* Wi-Fi <-> BLE (the Gadget tab's Connect over), each with the gadget last picked on it. */
    private func switchLink() {
        if link.streaming() {
            stopStream()
        }
        link.close()
        ble = !ble
        Prefs.setString("link", ble ? "ble" : "wifi")
        deviceAddr = Prefs.string(prefAddr)
        target = Prefs.string(prefName)
        micStarted = false
        makeLink()
        linkStatus = ble ? "BLE" : "Wi-Fi"
        lastAddr = deviceAddr
        lastName = target
        gadgetOff = Prefs.bool(prefOff)
        if gadgetOff {
            grant.close()
            deviceAddr = nil
            target = nil
            link.setTarget(address: nil, name: nil)
            linkStarted = false
        } else {
            grant.reopen()
            startLink()
        }
    }

    /* -mic sbc|lc3 (BLE): also capture the gadget's mic (start_mic), saved as Documents/mic.<codec>. */
    private func startMic() {
        guard let g = bleLink, let m = mic, !micStarted else { return }
        if g.proofPending {
            micAfterProof = true   /* a set-up gadget takes start_mic only on a proven connection */
            return
        }
        micStarted = true
        let url = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0].appendingPathComponent("mic." + m)
        FileManager.default.createFile(atPath: url.path, contents: nil)
        g.micOut = try? FileHandle(forWritingTo: url)
        g.sendControl(Data([Mg.cmdStartMic, m == "lc3" ? Mg.typeLc3 : Mg.typeSbc]))
        mlog("mic capture on, codec \(m)")
    }

    private func flushMic() {
        bleLink?.syncMic()
    }

    /* An SBC (or LC3) tone loop played on the gadget beside the video, over BLE. */
    private func toggleTone() {
        guard let g = bleLink else { return }
        if g.audioOn() {
            g.stopAudio()
            return
        }
        let lc3 = toneCodec == "lc3"
        guard let url = Bundle.main.url(forResource: "tone", withExtension: lc3 ? "lc3" : "sbc"),
              let d = try? Data(contentsOf: url) else {
            mlog("no tone asset")
            return
        }
        g.startAudio(d, frameBytes: lc3 ? 40 : 60, lc3: lc3)
    }

    /* ---- what the lists and a pick URL do ------------------------------- */

    func pick(_ what: String) {
        guard link != nil else { return }
        mlog("pick " + what)
        if what == "device" {
            showPicker()
            return
        }
        if what == "disconnect" {
            disconnectGadget()
            return
        }
        if what == "connect" {
            connectLast()
            return
        }
        if what == "rate" {
            screen.show("Video", "Encoding")
            return
        }
        if what == "codec" || what == "h264" || what == "vp8" {
            setCodec(what == "codec" ? !wantVp8 : what == "vp8")
            return
        }
        if what == "tone" {
            toggleTone()
            return
        }
        if what == "flushmic" {
            flushMic()
            return
        }
        if what == "link" {
            switchLink()
            return
        }
        if what.hasPrefix(Self.clipPrefix) && isClip() && link.streaming() && !ui.active {
            source = what   /* one clip to another: the stream goes on, the clip switches */
            grant.grant()
            startClip(lastSize, link.caps())
            return
        }
        if link.streaming() {
            stopStream()
        }
        if what == "stop" {
            auto = false
            grant.revoke()
            return
        }
        if ui.active {
            ui.setSource(.off)   /* now, so a connection that comes up starts this, not the UI */
        }
        source = what
        auto = true
        grant.grant()
        if link.ready() {
            startStreamSoon()
        }
    }

    /* ---- streaming ------------------------------------------------------ */

    private func startStream() {
        guard !grant.closed else { return }   /* Disconnect: nothing starts until a gadget is picked */
        if ui.active {
            let mine = userStart
            ui.setSource(.off)
            if mine {
                grant.grant()   /* the camera or clip replacing it is the same tap's start */
            }
        }
        let c = link.caps().snapshot()
        var s = size > 0 ? size : c.sugW
        s = min(s, min(c.maxW, c.maxH)) & ~7
        lastSize = s
        if isClip() {
            startClip(s, c)
            return
        }
        if source == "camera" && bleLink != nil && grant.needsCameraAsk(benchAuto: auto && Extras.benchMarker) {
            askCamera()   /* the gadget's ask starts this phone's camera only on the user's yes */
            return
        }
        let f = min(fps, c.maxFps)
        curFps = f
        /* The gadget's suggested bitrate, unless given; a gadget suggesting
         * less than 450 kb/s also caps the adaptive bitrate near it. */
        if fixedCamKbps > 0 {
            kbps = fixedCamKbps
        } else if !kbpsSet && c.sugKbps > 0 {
            kbps = c.sugKbps
        }
        if !maxKbpsSet {
            maxKbps = c.sugKbps > 0 && c.sugKbps < 450 ? max(kbps, c.sugKbps * 11 / 10)
                : c.sugKbps >= 600 ? c.sugKbps * 5 / 4 : 650
        }
        /* VP8 where it was picked, the gadget decodes it and the app has libvpx. */
        camVp8 = wantVp8 && c.vp8 && Vpx.available
        if wantVp8 && !camVp8 {
            mlog("VP8 picked, but " + (c.vp8 ? "this build has no libmgvpx" : "the gadget doesn't decode it") + ": H.264")
        }
        cam.swEncoder = camVp8 ? "vp8" : baseSw
        /* On BLE, every frame within half the gadget's ring. Under 16 KB (a tiny-RAM gadget) the
         * phone's encoder's keyframes (5-8 times a delta frame) don't fit: x264 holds them. */
        cam.maxFrameBytes = bleLink != nil ? max(0, c.maxFrame - VideoFrame.headerLen) : 0
        cam.vbvCeilKbps = maxKbps
        if cam.swEncoder == nil && bleLink != nil && c.maxFrame < 16384 && X264.available && Extras.string("sw") != "hw" {
            cam.swEncoder = "x264"
            mlog("a \(c.buffer) B ring: x264, frames up to \(cam.maxFrameBytes) B")
        }
        if let g = bleLink {
            /* Bytes in the air before the gadget's credit comes back: 3 KB holds a
             * 15 ms link to ~800 kb/s, 6 KB lets it carry ~870. */
            g.airCap = Int64(airSet ? air : c.sugKbps >= 600 ? 6144 : air)
        }
        /* Past 240 px on a gadget that decodes bands in parallel, send two
         * (software encoders only: one per band). */
        let bands = c.maxTiles >= 2 && s > 240 && s % 32 == 0 && cam.swEncoder != nil && !noBands ? 2 : 1
        cam.bands = bands
        cam.bandSplitPct = c.tileSplitPct > 0 ? c.tileSplitPct : 50
        mlog("starting \(s)x\(s) @\(f) \(kbps) kb/s (adaptive up to \(maxKbps))" + (bands > 1 ? ", two bands" : ""))
        link.startStreaming(w: s, h: s, fps: f, kbps: kbps, playoutMs: 0, bands: bands, vp8: camVp8)
        startMic()
        setLiveBudget()
        cam.pattern = source == "pattern"
        if c.dispW > 0 && c.dispH > 0 {
            cam.panelPx = min(c.dispW, c.dispH)   /* what the gadget scales the picture to */
        }
        let round = c.round, kb = kbps
        if !cam.pattern && AVCaptureDevice.authorizationStatus(for: .video) != .authorized {
            let ep = grant.epoch
            AVCaptureDevice.requestAccess(for: .video) { ok in
                DispatchQueue.main.async {
                    if ok {
                        /* The answer can come long after: only for the stream this connection still has. */
                        if ep == self.grant.epoch && self.link.streaming() {
                            self.startCamera(s, f, kb, round)
                        }
                    } else {
                        self.linkStatus = "Needs camera permission"
                        self.link.stopStreaming()
                    }
                }
            }
            return
        }
        startCamera(s, f, kb, round)
    }

    private func startCamera(_ s: Int, _ f: Int, _ k: Int, _ round: Bool) {
        do {
            try cam.start(w: s, h: s, fps: f, kbps: k, round: round)
        } catch {
            mlog("camera start failed: \(error.localizedDescription)")
            linkStatus = "Camera: \(error.localizedDescription)"
            link.stopStreaming()
        }
    }

    private func isClip() -> Bool {
        source.hasPrefix(Self.clipPrefix)
    }

    private func clipKey(_ name: String, _ size: Int, _ kbps: Int, _ vp8: Bool) -> String {
        /* A small gadget's frame cap and frame rate make other encodes of the same clip. */
        "\(name)|\(size)|\(kbps)" + (vp8 ? "|vp8" : "")
            + (ClipStreamer.maxFrameBytes > 0 ? "|max\(ClipStreamer.maxFrameBytes)" : "")
            + (ClipStreamer.maxFps > 0 ? "|fps\(ClipStreamer.maxFps)" : "")
    }

    /* Clips as VP8: picked, the gadget decodes it, and the app has libvpx. */
    private func clipVp8() -> Bool {
        wantVp8 && link != nil && link.caps().vp8 && Vpx.available
    }

    /* The Codec button: H.264 or VP8 from now on; a camera or clip stream that runs starts over in it. */
    func setCodec(_ vp8: Bool) {
        wantVp8 = vp8
        Prefs.setBool("codec_vp8", vp8)
        refreshUi()
        mlog("codec: " + (vp8 ? "VP8" : "H.264"))
        restartRunning(for: [.camera, .clip])
    }

    func refreshUi() {
        screen?.refresh()
    }

    /*
     * The bitrates clips are encoded at: clipTopKbps, then three steps down
     * (none below 180 kb/s); past 320 px the steps are small.
     */
    func clipLevels() -> [Int] {
        let t = clipTopKbps
        let l = clipSize > 320 ? [t, t * 90 / 100, t * 80 / 100, t * 70 / 100] : [t, t * 78 / 100, t * 60 / 100, t * 45 / 100]
        let floor = bleLink != nil ? min(180, t * 45 / 100) : 180   /* 180, or less for a BLE gadget suggesting under 400 */
        return l.map { max(floor, $0) }
    }

    /* Shows the clip now (in the best version already encoded) and has the encoder make the right one. */
    private func startClip(_ s: Int, _ c: Caps) {
        if player == nil {
            player = ClipStreamer(sink: self)
        }
        clipSize = s
        clipRound = c.round
        playWanted()
        wakeClipWorker()
    }

    private func wakeClipWorker() {
        clipWake.lock()
        if clipThread == nil {
            let t = Thread { [weak self] in self?.clipLoop() }
            t.name = "clips"
            t.qualityOfService = .utility
            clipThread = t
            t.start()
        }
        clipWake.broadcast()
        clipWake.unlock()
    }

    /*
     * One clip at a time, always the wanted clip at the current bitrate
     * first; then the other clips already played, at that bitrate, so a step
     * in bitrate finds them ready.
     */
    private func clipLoop() {
        while true {
            var snap: (String?, Int, Int, Bool, Bool, [String]) = (nil, 0, 0, true, false, [])
            DispatchQueue.main.sync {
                snap = (isClip() ? String(source.dropFirst(Self.clipPrefix.count)) : nil, clipSize, clipKbps, clipRound,
                        clipVp8(), clipsToKeepReady())
            }
            let (want, s, k, round, vp8, ready) = snap
            var next: String?
            if let w = want, needsClip(w, s, k, vp8) {
                next = w
            }
            for n in ready where next == nil && needsClip(n, s, k, vp8) {
                next = n
            }
            guard let name = next else {
                clipWake.lock()
                _ = clipWake.wait(until: Date().addingTimeInterval(1))
                clipWake.unlock()
                continue
            }
            if encodeClip(name, s, round, k, vp8) != nil {
                DispatchQueue.main.async { self.playWanted() }
            }
        }
    }

    private func clipsToKeepReady() -> [String] {
        clipsLock.lock()
        defer { clipsLock.unlock() }
        var names: [String] = []
        for c in clips.values where c.size == clipSize && !names.contains(c.name) {
            names.append(c.name)
        }
        return names
    }

    private func needsClip(_ name: String, _ s: Int, _ k: Int, _ vp8: Bool) -> Bool {
        let key = clipKey(name, s, k, vp8)
        clipsLock.lock()
        defer { clipsLock.unlock() }
        /* none yet, or one made for another gadget's limits */
        return (clips[key].map { !ClipStreamer.fits($0) } ?? true) && !failedClips.contains(key)
    }

    /* The wanted clip's best version: at clipKbps, else the nearest lower bitrate, else the lowest higher one. */
    private func bestClip(_ name: String, _ s: Int) -> Clip? {
        let vp8 = clipVp8()
        clipsLock.lock()
        defer { clipsLock.unlock() }
        if let exact = clips[clipKey(name, s, clipKbps, vp8)], ClipStreamer.fits(exact) {
            return exact
        }
        var below: Clip?, above: Clip?
        for c in clips.values where c.name == name && c.size == s && c.vp8 == vp8 && ClipStreamer.fits(c) {
            if c.kbps < clipKbps && (below == nil || c.kbps > below!.kbps) {
                below = c
            } else if c.kbps > clipKbps && (above == nil || c.kbps < above!.kbps) {
                above = c
            }
        }
        return below ?? above
    }

    private func playWanted() {
        guard isClip(), player != nil, link.ready() else { return }
        /* A late encode starts only what this connection still allows (a running clip just switches),
         * and never over a UI stream. */
        guard !ui.active, link.streaming() || grant.allows(selfStart: auto || wanted, proven: askOk()) else { return }
        let name = String(source.dropFirst(Self.clipPrefix.count))
        guard let best = bestClip(name, clipSize) else {
            clipsLock.lock()
            let failed = failedClips.contains(clipKey(name, clipSize, clipKbps, clipVp8()))
            clipsLock.unlock()
            if !failed {
                linkStatus = "Preparing \(clipLabel(name))..."
            }
            return
        }
        if best === player?.clip && link.streaming() {
            return
        }
        playClip(best)
    }

    private func encodeClip(_ name: String, _ s: Int, _ round: Bool, _ k: Int, _ vp8: Bool) -> Clip? {
        let key = clipKey(name, s, k, vp8)
        guard let sample = SampleClips.get(name) else {
            clipsLock.lock()
            failedClips.insert(key)
            clipsLock.unlock()
            return nil
        }
        do {
            let c = try ClipStreamer.encode(sample, size: s, kbps: k, round: round, vp8: vp8)
            clipsLock.lock()
            clips[key] = c
            clipsLock.unlock()
            return c
        } catch {
            mlog("clip \(name) failed: \(error.localizedDescription)")
            clipsLock.lock()
            failedClips.insert(key)
            failReasons[name] = (error as? URLError) != nil ? "it needs the Internet the first time" : error.localizedDescription
            clipsLock.unlock()
            DispatchQueue.main.async {
                self.linkStatus = "\(self.clipLabel(name)) failed: \(error.localizedDescription) (needs the Internet)"
            }
            return nil
        }
    }

    private func playClip(_ clip: Clip) {
        guard let p = player else { return }
        let cur = p.clip
        let same = link.streaming() && cur != nil && cur!.size == clip.size && cur!.fps == clip.fps && curFps == clip.fps
            && cur!.vp8 == clip.vp8
        if !same {
            if link.streaming() {
                p.stop()
                link.stopStreaming()
            }
            curFps = clip.fps
            link.startStreaming(w: clip.size, h: clip.size, fps: clip.fps, kbps: max(clip.kbps, clip.steadyKbps()),
                                playoutMs: playoutMs, bands: 1, vp8: clip.vp8)
        }
        p.play(clip)
        let sample = SampleClips.get(clip.name)
        linkStatus = (sample != nil ? sample!.credit + "\n" : "") + clip.info
    }

    /* A UI stream takes over the link: the camera and the clip stop feeding it. */
    func stopVideoForUi() {
        cam.stop()
        player?.stop()
    }

    private func stopStream() {
        bleLink?.stopAudio()
        flushMic()
        cam.stop()
        player?.stop()
        link.stopStreaming()
    }

    /* ---- sinks ------------------------------------------------------------ */

    func queuedBytes() -> Int64 {
        link.queuedBytes()
    }

    func onFrame(_ frame: Data, key: Bool) {
        link.sendFrame(frame)
    }

    func behind() -> Bool {
        /* More than about a frame's worth waiting on the phone: skip this camera frame. */
        let frame = Int64(kbps) * 1000 / 8 / Int64(max(1, curFps))
        let budget = live ? max(1200, frame * Int64(behindPct) / 100) : max(4000, frame * 2)
        return link.queuedBytes() > budget
    }

    /* ---- device picker ------------------------------------------------- */

    /* Lists the gadgets found on the network; tapping one connects to it. The choice is remembered. */
    private func showPicker() {
        if picker != nil {
            return
        }
        let p = PickerController { [weak self] addr in
            guard let self else { return }
            if let addr {
                let name = self.seen[addr]?.name ?? addr
                Prefs.setString(self.prefAddr, addr)
                Prefs.setBool(self.prefOff, false)
                self.gadgetOff = false
                self.grant.reopen()
                self.lastAddr = addr
                self.lastName = name
                if self.ble {
                    self.ui.gadgetChosen(address: addr, name: name)
                }
                Prefs.setString(self.prefName, name)
                self.deviceAddr = addr
                self.target = name
                mlog("picked \(name) \(addr)")
                if self.link.streaming() {
                    self.stopStream()
                }
                self.link.select(address: addr, name: name)
            } else {
                self.link.pickerClosed()
                if self.gadgetOff {
                    /* Closed without a choice after a Disconnect: stop the picker's scan, stay off. */
                    self.link.close()
                    self.makeLink()
                    self.linkStarted = false
                    self.connStatus = "Not connected"
                }
            }
            self.picker = nil
            self.refreshUi()
        }
        p.onRescan = { [weak self] in
            /* The one way to reorder: forget what was seen, start the list over. */
            guard let self else { return }
            let keep = self.pinnedPickerAddr()
            self.seen = self.seen.filter { $0.key == keep }
            self.buildPickerOrder()
            self.refreshPicker()
            self.link.scanForPicker()
        }
        picker = p
        present(UINavigationController(rootViewController: p), animated: true)
        buildPickerOrder()
        refreshPicker()
        pickerTimer?.invalidate()
        pickerTimer = Timer.scheduledTimer(withTimeInterval: 2, repeats: true) { [weak self] t in
            guard let self, self.picker != nil else {
                t.invalidate()
                return
            }
            self.refreshPicker()
        }
        if !linkStarted {
            startLink()   /* after a Disconnect: a link with no gadget, for the picker's scan */
        }
        link.scanForPicker()
    }

    /* The connected gadget, else the last one used: pinned at the top from the start. */
    private func pinnedPickerAddr() -> String? {
        if let cur = link.connectedAddress(), !cur.isEmpty {
            return cur
        }
        return lastAddr.flatMap { $0.isEmpty ? nil : $0 }
    }

    /* The order at opening (or Rescan): the pinned gadget, then those seen lately by name. */
    private func buildPickerOrder() {
        let now = nowMs(), pin = pinnedPickerAddr()
        pickerOrder = pin.map { [$0] } ?? []
        pickerOrder += seen.filter { now - $0.value.at < Self.pickerStaleMs && $0.key != pin }
            .sorted { ($0.value.name.lowercased(), $0.key) < ($1.value.name.lowercased(), $1.key) }
            .map { $0.key }
    }

    /* Rows in pickerOrder; a gadget seen for the first time goes at the end. Never reorders. */
    private func refreshPicker() {
        guard let p = picker else { return }
        let now = nowMs(), cur = link.connectedAddress(), pin = pinnedPickerAddr()
        for (addr, v) in seen.sorted(by: { $0.value.at < $1.value.at })
        where now - v.at < Self.pickerStaleMs && !pickerOrder.contains(addr) {
            pickerOrder.append(addr)
        }
        p.rows = pickerOrder.map { addr in
            guard let v = seen[addr] else {
                /* the pinned gadget, not heard from yet */
                let name = addr == lastAddr ? (lastName ?? addr) : addr
                return (addr, name + "  (last used)", "looking…", true)
            }
            let on = addr == cur
            let stale = !on && now - v.at >= Self.pickerStaleMs
            let tag = on ? "  (connected)" : addr == pin ? "  (last used)" : ""
            let detail = stale ? "not seen for \((now - v.at) / 1000) s"
                : self.ble ? "rssi \(v.rssi)" : addr.hasPrefix("svc:") ? "Bonjour" : addr
            return (addr, v.name + tag, detail, stale)
        }
    }

    func onSeen(name: String, address: String, rssi: Int) {
        DispatchQueue.main.async {
            let old = self.seen[address]
            self.seen[address] = (name, rssi, nowMs())
            if old == nil || old!.name != name || abs(old!.rssi - rssi) >= 4 {
                self.refreshPicker()   /* in place: the order doesn't change */
            }
        }
    }

    /* ---- VideoLinkListener ---------------------------------------------- */

    func onStatus(_ s: String) {
        mlog(s)
        DispatchQueue.main.async {
            self.linkStatus = s
            self.connStatus = s
        }
    }

    func onReady(_ caps: Caps) {
        DispatchQueue.main.async { [self] in
            linkStatus = "Connected: \(caps)"
            connStatus = "Connected over " + link.kind()
            clipSize = min(size > 0 ? size : caps.sugW, min(caps.maxW, caps.maxH)) & ~7
            clipRound = caps.round
            /* A small BLE gadget's limits; Wi-Fi as before. */
            let small = bleLink != nil
            ClipStreamer.maxFrameBytes = small ? max(0, caps.maxFrame - VideoFrame.headerLen) : 0
            ClipStreamer.maxFps = small ? caps.maxFps : 0
            /* A gadget suggesting a high bitrate gets clips aimed at 92% of it
             * and a 300 ms playout delay for the peaks' queue; a BLE one suggesting
             * under 450 (a small gadget's link or decoder), its own suggestion. */
            if !clipKbpsSet {
                clipTopKbps = caps.sugKbps >= 600 ? caps.sugKbps * 92 / 100
                    : small && caps.sugKbps > 0 && caps.sugKbps < 450 ? caps.sugKbps : 450
            }
            if !playoutSet {
                playoutMs = caps.sugKbps >= 600 && caps.maxPlayoutMs >= 300 ? 300 : 200
            }
            if fixedClipKbps > 0 && clipLevels().contains(fixedClipKbps) {
                clipKbps = fixedClipKbps
            } else if !clipLevels().contains(clipKbps) {
                clipKbps = clipLevels()[0]
            }
            wakeClipWorker()
            bleLink?.askSpeakerVolume()
            if let g = bleLink, tone, !g.audioOn() {
                DispatchQueue.main.asyncAfter(deadline: .now() + 1.5) {
                    self.startMic()
                    self.toggleTone()
                }
            }
            if let g = bleLink, benchSecs > 0 {
                g.benchReceive(benchSecs)
                return
            }
            if link is GadgetLink {
                ui.gadgetChosen(address: link.connectedAddress(), name: link.gadgetName())
            }
            if ui.active {
                if userStart || askAllowed() {   /* a UI source the user didn't start here waits for the proof */
                    ui.startIfReady()
                }
                return
            }
            if (userStart || (auto || wanted) && askAllowed()) && !link.streaming() {
                startStream()
            }
        }
    }

    /* The gadget's ask for the camera over BLE (or Start automatically on a new connection): the user's
     * yes holds for the rest of this connection; a no answers the gadget as a camera that can't start does
     * (stop_streaming, buffer_drop). Main queue. */
    private var cameraAlert: UIAlertController?

    private func askCamera() {
        guard cameraAlert == nil else { return }
        let ep = grant.epoch, name = gadgetName() ?? "The gadget"
        mlog("camera: \(name) asks to start the camera; asking the user")
        let a = UIAlertController(title: "Start the camera?",
            message: "\(name) asks to show this iPhone's camera. Allow it until it disconnects?", preferredStyle: .alert)
        a.addAction(UIAlertAction(title: "Don't Allow", style: .cancel) { [weak self] _ in self?.cameraAnswered(false, ep) })
        a.addAction(UIAlertAction(title: "Allow", style: .default) { [weak self] _ in self?.cameraAnswered(true, ep) })
        cameraAlert = a
        (presentedViewController ?? self).present(a, animated: true)
    }

    private func cameraAnswered(_ yes: Bool, _ ep: Int) {
        cameraAlert = nil
        guard ep == grant.epoch else { return }   /* another connection by now: it asks again */
        mlog("camera: the user \(yes ? "allowed" : "declined") the gadget's ask")
        if yes {
            grant.allowCamera()
            if link.ready() && !link.streaming() && !ui.active && (wanted || auto) {
                startStream()
            }
            return
        }
        wanted = false
        bleLink?.sendControl(Data([Mg.cmdStreamVideo, Mg.vStopStreaming, Mg.bufferDrop]))
        linkStatus = "Camera not started: you declined the gadget's ask"
        refreshUi()
    }

    func onRequestStreaming(_ want: Bool) {
        mlog("gadget asks for streaming: \(want)")
        DispatchQueue.main.async { [self] in
            wanted = want
            if !want {
                askAfterProof = false
            }
            if ui.active {
                return   /* the UI stream is on */
            }
            if want && link.ready() && !link.streaming() && (userStart || askAllowed()) {
                startStream()
            } else if !want && link.streaming() {
                stopStream()
            }
        }
    }

    /* The gadget's own ask to stream, before the token proof finished: it starts on a match. */
    private var askAfterProof = false

    /*
     * Whether the gadget's ask, or -auto on a new connection, may start a
     * stream (the user's own start on this connection needs none): only after
     * this connection's token proof matched; a gadget without a proof key, or
     * any gadget over Wi-Fi (nothing proves it there), streams only on a tap.
     * A proof still running holds the ask until it ends. Main queue.
     */
    private func askOk() -> Bool {
        link?.proven ?? false
    }

    private func askAllowed() -> Bool {
        if askOk() {
            askAfterProof = false
            return true
        }
        guard let g = bleLink else {
            askAfterProof = false
            linkStatus = "Tap a source to stream: over Wi-Fi the gadget proves nothing, so only your own start streams"
            mlog("gadget's ask held: Wi-Fi")
            refreshUi()
            return false
        }
        askAfterProof = g.proofPending
        linkStatus = askAfterProof ? "The gadget's stream waits for its token proof"
            : g.hasProofKey ? "Not streaming on the gadget's ask: it didn't prove this phone's key"
            : "Tap a source to stream: no proof key for this gadget"
        mlog("gadget's ask held: " + linkStatus)
        refreshUi()
        return false
    }

    /* What waited for the token proof (main queue): the screen mirror, the mic, the volume. */
    private func onProof(_ o: TokenProof.Outcome?, on g: GadgetLink?) {
        guard let g, g === bleLink else { return }
        ui.proofDone(o)
        if startAfterProof >= 0 {
            let ep = startAfterProof
            startAfterProof = -1
            if g.proven {
                askAgain(ep)
            } else if ep == grant.epoch {
                refused("The gadget refused the stream: it didn't prove this phone's key")
            }
        }
        if askAfterProof {
            askAfterProof = false
            if g.proven && ui.active {
                ui.startIfReady()
            } else if g.proven && (wanted || auto) && !ui.active && !link.streaming() {
                startStream()
            } else if !g.proven {
                linkStatus = "Not streaming on the gadget's ask: it didn't prove this phone's key"
            }
        }
        if micAfterProof {
            micAfterProof = false
            startMic()
        }
        if let v = volumeAfterProof {
            volumeAfterProof = nil
            g.setSpeakerVolume(v)
        }
        refreshUi()
    }

    private func onCommandError(_ forCmd: Int, _ code: Int, on g: GadgetLink?) {
        guard let g, g === bleLink else { return }
        if forCmd == Int(Mg.cmdStartMic) && micStarted {
            micStarted = false
            g.closeMic()
            linkStatus = "The gadget refused the mic (" + (code == 6 ? "it needs the token proof: paste its key on the UI tab" : Mg.errorName(code)) + ")"
        }
    }

    func onKeyframeRequest() {
        mlog("keyframe requested")
        ui.keyframe()
        cam.requestKeyframe()
        player?.requestKeyframe()
    }

    func onStreamStopped() {
        mlog("gadget stopped the stream")
        DispatchQueue.main.async {
            self.ui.linkLost()
            self.cam.stop()
            self.player?.stop()
        }
    }

    func onStartRefused(_ reason: String) {
        if reason == "closed" {
            /* Phone camera isn't open on the gadget: its "want" starts the stream. */
            mlog("gadget refused the stream: open Phone camera on it")
            DispatchQueue.main.async { self.linkStatus = "Open Phone camera on the gadget" }
            return
        }
        if reason == "proof required", bleLink?.proofPending == true {
            /* A set-up gadget takes a stream only after this connection's proof: ask again on a match. */
            mlog("gadget refused the stream until the token proof matches")
            DispatchQueue.main.async { [self] in
                ui.linkLost(.retry)
                startAfterProof = grant.epoch
                linkStatus = "The stream waits for the gadget's token proof"
            }
            return
        }
        guard reason == "busy" else {
            /* Only busy passes (the gadget booting, or its screen waking): anything else stays refused. */
            mlog("gadget refused the stream (\(reason))")
            DispatchQueue.main.async { [self] in refused("The gadget refused the stream (\(reason))") }
            return
        }
        mlog("gadget refused the stream (\(reason)); trying again in 1 s")
        DispatchQueue.main.async { [self] in
            ui.linkLost(.retry)
            let ep = grant.epoch
            DispatchQueue.main.asyncAfter(deadline: .now() + 1) { [self] in askAgain(ep) }
        }
    }

    /* A start that stays refused (main queue): what fed the stream stops, nothing restarts it on
     * its own, a tap starts it again. */
    private func refused(_ why: String) {
        if ui.active {
            ui.setSource(.off)
        } else {
            ui.linkLost()
        }
        cam.stop()
        player?.stop()
        auto = false
        wanted = false   /* the gadget's ask went with the refusal */
        grant.revoke()
        startAfterProof = -1
        linkStatus = why
        connStatus = linkStatus
        refreshUi()
    }

    /* startStream() in 0.3 s, only if it is still the same connection and ready: a stream meant for one
     * connection never goes to the next (main queue). */
    private func startStreamSoon() {
        let ep = grant.epoch
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.3) { [self] in
            guard ep == grant.epoch, link.ready(), !link.streaming() else { return }
            if grant.allows(selfStart: auto || wanted, proven: askOk()) {
                startStream()
            } else if !linkStatus.hasPrefix("The gadget refused") {   /* a refusal keeps its own reason */
                linkStatus = "Stopped: tap a source to start again"   /* a Stop or Start automatically off meanwhile */
                connStatus = linkStatus
                refreshUi()
            }
        }
    }

    /* A refused start (busy, or waiting for the proof) asked for again on the same connection (main queue). */
    private func askAgain(_ ep: Int) {
        guard ep == grant.epoch, link.ready(), !link.streaming() else { return }
        /* A saved UI source, too, needs the user's start or the proof. */
        guard grant.allows(selfStart: ui.active || auto || wanted, proven: askOk()) else { return }
        if ui.active {
            ui.startIfReady()
            return
        }
        if isClip(), let p = player, let clip = p.clip {
            link.startStreaming(w: clip.size, h: clip.size, fps: clip.fps, kbps: max(clip.kbps, clip.steadyKbps()),
                                playoutMs: playoutMs, bands: 1, vp8: clip.vp8)
            p.requestKeyframe()
        } else if !isClip() && cam.running {
            link.startStreaming(w: lastSize, h: lastSize, fps: curFps, kbps: kbps, playoutMs: 0, bands: cam.bands, vp8: camVp8)
            cam.requestKeyframe()
        } else {
            startStream()
        }
    }

    func onBufferUpdate(avail: Int64, received: Int64, shown: Int, dropped: Int, decodeMs: Int) {
        DispatchQueue.main.async { [self] in
            devAvail = avail
            devRecv = received
            devShown = shown
            devDropped = dropped
            devMs = decodeMs
        }
    }

    func onDisconnected(wasReady: Bool) {
        mlog("disconnected")
        DispatchQueue.main.async {
            self.grant.connectionEnded(wasReady: wasReady)   /* the next connection proves its token first, capabilities or not */
            self.wanted = false   /* the gadget's ask was this connection's */
            if let a = self.cameraAlert {
                self.cameraAlert = nil   /* the next connection asks again */
                a.dismiss(animated: true)
            }
            if self.micStarted {
                self.micStarted = false   /* the next connection starts it again */
                self.bleLink?.closeMic()
            }
            self.ui.linkLost(.disconnected)
            self.cam.stop()
            self.player?.stop()
        }
    }

    /* ---- bitrates and sizes (Video tab) ---------------------------------- */

    func setClipRate(_ k: Int) {
        fixedClipKbps = k
        Prefs.setInt("clip_kbps", k)
        mlog("clip bitrate " + (k > 0 ? "\(k) kb/s (fixed)" : "auto"))
        let t = k > 0 ? k : clipLevels()[0]
        if t != clipKbps {
            clipKbps = t
            heldSeconds = 0
            clipCalmSeconds = 0
            if isClip() && link != nil && link.streaming() {
                startClip(lastSize, link.caps())
            }
        }
        refreshUi()
    }

    func setCamRate(_ k: Int) {
        fixedCamKbps = k
        Prefs.setInt("cam_kbps", k)
        mlog("camera bitrate " + (k > 0 ? "\(k) kb/s (fixed)" : "auto"))
        if k > 0 {
            kbps = k
            if cam != nil && cam.running {
                cam.setBitrate(k)
                setLiveBudget()
            }
        }
        refreshUi()
    }

    func setFps(_ f: Int) {
        fps = f
        Prefs.setInt("fps", f)
        mlog("camera frame rate \(f) fps")
        restartRunning(for: [.camera])
        refreshUi()
    }

    /* A new picture size restarts the camera or clip that streams (0: the gadget's suggestion). */
    func setSize(_ k: Int) {
        size = k
        Prefs.setInt("size", k)
        mlog("picture size " + (k > 0 ? "\(k) px" : "auto"))
        restartRunning(for: [.camera, .clip])
        refreshUi()
    }

    /* What streams now: a UI stream, the camera (or the bars), a clip, or nothing. */
    private func running() -> RunningStream {
        guard link != nil, link.streaming() else { return .nothing }
        return ui.active ? .ui : isClip() ? .clip : .camera
    }

    /* A setting's restart: only the stream of those kinds that runs now, as it was started (the start it
     * already had: no new grant), and nothing when it isn't running; a UI stream keeps going. */
    private func restartRunning(for kinds: Set<RunningStream>) {
        guard running().restarts(for: kinds) else { return }
        stopStream()
        startStreamSoon()
    }

    private func clipLabel(_ name: String) -> String {
        SampleClips.get(name)?.label ?? name
    }

    /* ---- adaptive bitrate ------------------------------------------------- */

    /*
     * Clips can't skip frames: when frames waited on the link for more than
     * 40 ms of the last second, two seconds running, re-encode a step down;
     * after 8 calm seconds, step back up.
     */
    private func adaptClip() {
        guard isClip(), let p = player, link.streaming(), fixedClipKbps == 0 else {
            lastHeldMs = player?.statHeldMs ?? 0
            return
        }
        let held = p.statHeldMs - lastHeldMs
        lastHeldMs = p.statHeldMs
        heldSeconds = held > 40 ? heldSeconds + 1 : 0
        clipCalmSeconds = held > 20 ? 0 : clipCalmSeconds + 1
        let levels = clipLevels()
        var at = 0
        while at < levels.count - 1 && levels[at] > clipKbps {
            at += 1
        }
        var next = clipKbps
        if heldSeconds >= 2 && at < levels.count - 1 {
            next = levels[at + 1]
        } else if clipCalmSeconds >= 8 && at > 0 {
            next = levels[at - 1]
        }
        if next != clipKbps {
            mlog("clip bitrate \(clipKbps) -> \(next) kb/s (held \(held) ms in the last second)")
            clipKbps = next
            heldSeconds = 0
            clipCalmSeconds = 0
            startClip(lastSize, link.caps())
        }
    }

    /*
     * The camera's bitrate, once a second: when the link falls behind (three
     * or more frames skipped, or the send gate holding them 150 ms a second),
     * to what it carried less 5% (at least 10% down) in one step; when the
     * gadget falls behind, 10% down; after two calm seconds, 10% up, to
     * maxKbps. Never below camFloorKbps().
     */
    private func adaptBitrate(_ dtMs: Int64, _ sentBytes: Int64, _ encFps: Double, _ shownFps: Double) {
        guard link.streaming(), !isClip(), adaptive, fixedCamKbps == 0 else { return }
        let skipped = cam.statSkipped
        let gate = link.statGateUs
        let newSkips = skipped - lastSkipped
        let gateMsPerS = (gate - lastGateUs) / 1000 * 1000 / max(1, dtMs)
        lastSkipped = skipped
        lastGateUs = gate
        let f = max(1, curFps)
        let sentKbps = Int(sentBytes * 8 / max(1, dtMs))
        let linkBehind = newSkips > 2 || gateMsPerS > 150
        let linkTight = newSkips > 0
        let gadgetBehind = devMs > 0 && devMs * f > 900 && encFps > 5 && shownFps < encFps * 0.85
        gadgetBehindSeconds = gadgetBehind ? gadgetBehindSeconds + 1 : 0
        var floor = lastSize > 320 ? max(minKbps, maxKbps * 2 / 3) : minKbps
        /* A BLE gadget suggesting less than twice the floor (a tiny one, ~80 kb/s): half its suggestion. */
        let sug = link.caps().sugKbps
        if bleLink != nil && sug > 0 && sug < 2 * floor {
            floor = max(24, sug / 2)
        }
        var k = cam.kbps
        let was = k
        let why: String
        if linkBehind {
            k = max(floor, min(k * 9 / 10, sentKbps * 95 / 100))
            calmSeconds = 0
            why = "link behind"
        } else if gadgetBehindSeconds >= 2 {
            k = max(floor, k * 9 / 10)
            gadgetBehindSeconds = 0
            calmSeconds = 0
            why = "gadget behind"
        } else if linkTight {
            calmSeconds = 0
            return
        } else {
            calmSeconds += 1
            if calmSeconds >= 2 && k < maxKbps {
                k = min(maxKbps, k * 110 / 100 + 1)
                why = "calm"
            } else {
                return
            }
        }
        if k != was {
            cam.setBitrate(k)
            kbps = k
            setLiveBudget()
            mlog("bitrate \(was) -> \(k) kb/s, \(why) (sent \(sentKbps) kb/s, skips \(newSkips), gate \(gateMsPerS) ms/s, gadget "
                 + String(format: "%.1f/%.1f", shownFps, encFps) + " fps, decode \(devMs) ms)")
        }
    }

    /* ---- stats ---------------------------------------------------------- */

    private func tick() {
        guard link != nil else { return }
        ui.tick()
        let now = nowMs()
        let dt = max(1, now - lastStatMs)
        let bytes = link.statBytes
        let av = isClip() && player != nil
        let fed = av ? player!.statSent : cam.statFed, enc = av ? player!.statSent : cam.statEncoded, shown = devShown
        let dEnc = Double(max(1, enc - lastEnc))
        let camEncMs = Double(cam.statCamToEncUs - lastCamEnc) / 1000 / dEnc
        let convMs = Double(cam.statConvUs - lastConv) / 1000 / Double(max(1, fed - lastFed))
        let encMs = Double(cam.statEncUs - lastEncUs) / 1000 / dEnc
        let gateMs = Double(link.statGateUs - lastGate) / 1000 * 1000 / Double(dt)
        let lat = String(format: "cam->enc %.0f ms (convert %.1f, encoder %.1f)  gate %.0f ms/s", camEncMs, convMs, encMs, gateMs)
        lastCamEnc = cam.statCamToEncUs
        lastEncUs = cam.statEncUs
        lastConv = cam.statConvUs
        lastGate = link.statGateUs
        var lat2 = lat
        if let g = bleLink {
            lat2 += String(format: "  audio out %.0f kb/s, %lld type switches", Double(g.statAudioBytes - lastAudio) * 8 / Double(dt),
                           g.statTypeSwitches)
            lat2 += String(format: "  mic in %.0f kb/s", Double(g.statDataIn - lastDataIn) * 8 / Double(dt))
            uiAudioKbps = Double(g.statAudioBytes - lastAudio) * 8 / Double(dt)
            uiMicKbps = Double(g.statDataIn - lastDataIn) * 8 / Double(dt)
            lastAudio = g.statAudioBytes
            lastDataIn = g.statDataIn
        } else {
            uiAudioKbps = 0
            uiMicKbps = 0
        }
        let mode = (link.streaming() ? "STREAMING" : "idle") + ((isClip() ? fixedClipKbps : fixedCamKbps) > 0 ? " (fixed rate)" : " (auto rate)")
        let s = String(format: "%@\n%@  %d kb/s target  %@\nsend %.0f kb/s  in flight %lld  queued %lld\ncamera->enc %.1f fps  encoded %.1f fps  skipped %d  keys %d\n%@\ngadget shown %.1f fps  dropped %d  decode %d ms  free %lld",
                       linkStatus, link.describe(), isClip() ? clipKbps : kbps, mode,
                       Double(bytes - lastBytes) * 8 / Double(dt), link.inFlight(), link.queuedBytes(),
                       Double(fed - lastFed) * 1000 / Double(dt), Double(enc - lastEnc) * 1000 / Double(dt), cam.statSkipped,
                       cam.statKeys, lat2, Double(shown - lastShown) * 1000 / Double(dt), devDropped, devMs, devAvail)
            + (deviceLine.isEmpty ? "" : "\n" + deviceLine)
        if link.streaming() && !isClip() {
            mlog(meter.take())
        }
        if link.streaming() {
            mlog("stats: " + s.replacingOccurrences(of: "\n", with: "|"))
        }
        adaptBitrate(dt, bytes - lastBytes, Double(enc - lastEnc) * 1000 / Double(dt), Double(shown - lastShown) * 1000 / Double(dt))
        adaptClip()
        var st = StatsOverlay.Stats()
        st.sendKbps = Double(bytes - lastBytes) * 8 / Double(dt)
        st.gateMsPerS = gateMs
        st.fedFps = Double(fed - lastFed) * 1000 / Double(dt)
        st.encFps = Double(enc - lastEnc) * 1000 / Double(dt)
        st.shownFps = Double(shown - lastShown) * 1000 / Double(dt)
        st.camToEncMs = camEncMs
        st.convertMs = convMs
        st.encodeMs = encMs
        if let g = bleLink, g.audioOn() || micStarted {
            st.add("AUDIO", "tone out", String(format: "%.0f kb/s", uiAudioKbps))
            st.add("AUDIO", "mic in", String(format: "%.0f kb/s", uiMicKbps))
        }
        fillStats(&st)
        screen.overlay.stats = st
        refreshUi()
        lastStatMs = now
        lastBytes = bytes
        lastFed = fed
        lastEnc = enc
        lastShown = shown
    }
}

/* ---- for the screen (AppScreen, the tabs) ---------------------------------- */

extension MainViewController {
    /* The rest of the overlay's numbers: what's being sent, the link, the gadget. */
    func fillStats(_ st: inout StatsOverlay.Stats) {
        let pc = isClip() ? player?.clip : nil
        st.connected = connected()
        st.streaming = link.streaming()
        st.preparing = preparing()
        st.clip = isClip()
        st.codec = (isClip() ? pc?.vp8 == true : camVp8) ? "VP8" : "H.264"
        st.size = pc?.size ?? lastSize
        st.fps = curFps
        st.bands = isClip() ? 1 : cam.bands
        st.targetKbps = isClip() ? (pc?.kbps ?? clipKbps) : kbps
        st.fixedRate = (isClip() ? fixedClipKbps : fixedCamKbps) > 0
        st.source = sourceLabel()
        st.encoder = isClip() ? "" : cam.encoderName.isEmpty ? "starting" : String(cam.encoderName.split(separator: " ").first ?? "")
        st.linkName = gadgetName() ?? ""
        st.linkKind = link.kind()
        st.linkDetail = link.describe()
        st.inFlight = link.inFlight()
        st.queued = link.queuedBytes()
        st.skipped = isClip() ? 0 : cam.statSkipped
        st.skippedNew = max(0, cam.statSkipped - uiSkipped)
        uiSkipped = cam.statSkipped
        st.keys = isClip() ? (player?.statKeys ?? 0) : cam.statKeys
        st.dropped = devDropped
        st.droppedNew = max(0, devDropped - uiDropped)
        uiDropped = devDropped
        st.decodeMs = devMs
        st.free = devAvail
        st.buffer = link.ready() ? link.caps().buffer : 0
        st.note = pc.flatMap { SampleClips.get($0.name)?.credit } ?? ""
    }

    /* The system's broadcast picker (it has no API to open: its button is tapped for it). */
    /* The broadcast extension streams on its own connection: the app lets the
     * gadget go meanwhile (so neither starts a stream over the other), and
     * takes it back after. */
    func releaseLinkForBroadcast() {
        guard ble, !linkReleased else { return }
        linkReleased = true
        if link.streaming() {
            stopStream()
        }
        link.close()
        grant.linkRemade()   /* let go: the link it resumes with is remade too */
        mlog("broadcast: the app lets go of the gadget")
    }

    func resumeLinkAfterBroadcast() {
        guard linkReleased else { return }
        linkReleased = false
        guard !gadgetOff else { return }
        if inBackground {
            parked = true   /* appForeground opens it */
            return
        }
        link.close()
        makeLink()
        startLink()
        mlog("broadcast: the app reconnects")
    }

    func showBroadcastPicker() {
        let p = RPSystemBroadcastPickerView(frame: CGRect(x: 0, y: 0, width: 50, height: 50))
        p.preferredExtension = (Bundle.main.bundleIdentifier ?? "") + ".broadcast"
        p.showsMicrophoneButton = false
        view.addSubview(p)
        p.isHidden = true
        for case let b as UIButton in p.subviews {
            b.sendActions(for: .touchUpInside)
        }
        DispatchQueue.main.asyncAfter(deadline: .now() + 30) { p.removeFromSuperview() }
    }

    func showCalibration() {
        present(CalibrationController(ui: ui), animated: true)
    }

    private func startLink() {
        linkStarted = true
        link.start()
    }

    /** The gadget Connect goes back to after a Disconnect (nil: none remembered). */
    func lastGadget() -> String? {
        lastName ?? lastAddr
    }

    /*
     * Disconnect: ends what's running on the gadget cleanly (a UI or video stream, the
     * screen broadcast, the mic), then closes the link and stays off
     * (gadgetOff). Doesn't forget the gadget: Connect, or picking it again, comes back to it.
     */
    func disconnectGadget() {
        guard !gadgetOff, link != nil else { return }
        /* First: nothing from before the Disconnect (the gadget's ask during the close's grace, a retry,
         * the camera's yes, a proof ending) starts anything again. */
        grant.close()
        wanted = false
        startAfterProof = -1
        askAfterProof = false
        if let a = cameraAlert {
            cameraAlert = nil
            a.dismiss(animated: true)
        }
        let name = gadgetName() ?? "the gadget"
        mlog("disconnect \(name) (the user's choice): no reconnecting until a gadget is picked")
        if ui.broadcasting || UIScreen.main.isCaptured {
            UiShared.post(UiShared.notifyStop)   /* the extension's own link to the gadget */
        }
        if ui.active {
            ui.setSource(.off)   /* the UI stream's stop */
        }
        if link.streaming() {
            stopStream()   /* stop_streaming */
        }
        if micStarted, let g = bleLink {
            g.sendControl(Data([Mg.cmdStopMic]))
            micStarted = false
            g.closeMic()
        }
        gadgetOff = true
        Prefs.setBool(prefOff, true)
        parkWork?.cancel()
        parked = false
        lastAddr = link.connectedAddress() ?? deviceAddr
        lastName = lastAddr.flatMap { seen[$0]?.name } ?? gadgetName()
        if lastAddr != nil || lastName != nil {
            Prefs.setString(prefAddr, lastAddr)   /* Connect comes back to this one, after a relaunch too */
            Prefs.setString(prefName, lastName)
        }
        deviceAddr = nil
        target = nil
        connStatus = "Disconnecting from \(name)…"
        refreshUi()
        let old = link!
        /* A moment for the stop writes to go out before the link closes. */
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.6) { [weak self] in
            guard let self else { return }
            old.close()
            if self.link === old && self.gadgetOff {
                self.makeLink()   /* no gadget, not started: nothing scans or connects */
                self.linkStarted = false
                self.connStatus = "Not connected"
                if self.picker != nil {
                    /* The picker opened meanwhile, on the old link: its scan goes on on this one. */
                    self.startLink()
                    self.link.scanForPicker()
                }
            }
            mlog("disconnected from \(name) (stays off until a gadget is picked)")
            self.refreshUi()
        }
    }

    /** Connect after a Disconnect: the last gadget, or the picker when there's none. */
    func connectLast() {
        guard gadgetOff else { return }
        guard lastAddr != nil || lastName != nil else {
            showPicker()
            return
        }
        gadgetOff = false
        grant.reopen()
        Prefs.setBool(prefOff, false)
        deviceAddr = lastAddr
        target = lastName
        mlog("connect to \(lastGadget() ?? "?") again")
        link.close()
        makeLink()
        startLink()
        refreshUi()
    }

    /** Forget: Disconnect and forget it here. iOS has no API to unpair: the alert says where. */
    func forgetGadget() {
        guard let name = gadgetOff ? lastGadget() : gadgetName() else { return }
        let addr = gadgetOff ? lastAddr : link.connectedAddress()
        let a = UIAlertController(title: "Forget \(name)?",
            message: "The app disconnects from it and forgets it, its proof key included. To use it again, choose it "
                + "from the list and paste its key again. "
                + "If it's paired with this iPhone (for touch input), also tap Forget This Device for it in "
                + "Settings > Bluetooth: apps can't remove a pairing.",
            preferredStyle: .alert)
        a.addAction(UIAlertAction(title: "Cancel", style: .cancel))
        a.addAction(UIAlertAction(title: "Forget", style: .destructive) { [weak self] _ in
            guard let self else { return }
            self.disconnectGadget()
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.8) {
                self.lastAddr = nil
                self.lastName = nil
                Prefs.setString(self.prefAddr, nil)
                Prefs.setString(self.prefName, nil)
                UiShared.setProofKey(nil, for: name)   /* a gadget set up again under its name proves itself anew */
                let saved = self.ui.mirror.gadgetName   /* the name its key went under, if it reads differently */
                if let addr, addr == self.ui.mirror.gadget, !saved.isEmpty, saved != name {
                    UiShared.setProofKey(nil, for: saved)
                }
                mlog("forgot \(name)")
                self.connStatus = "Not connected"
                self.refreshUi()
            }
        })
        present(a, animated: true)
    }

    func connected() -> Bool {
        link != nil && link.ready()
    }

    /* The gadget connected to, else the one being looked for (nil: any). */
    func gadgetName() -> String? {
        (link != nil ? link.gadgetName() : nil) ?? target
    }

    func connectionLine() -> String {
        connStatus.isEmpty ? (link?.describe() ?? "") : connStatus
    }

    func linkDetail() -> String {
        link?.describe() ?? ""
    }

    func decodeLine() -> String {
        connected() ? "\(devDropped) dropped, \(devMs) ms" : "-"
    }

    func caps() -> Caps? {
        connected() ? link.caps() : nil
    }

    func currentSource() -> String {
        source
    }

    func sourceIs(_ what: String) -> Bool {
        source == what
    }

    /* The clip that's the source (its name), or nil. */
    func clipName() -> String? {
        isClip() ? String(source.dropFirst(Self.clipPrefix.count)) : nil
    }

    func sourceLabel() -> String {
        if let n = clipName() {
            return clipLabel(n)
        }
        return source == "pattern" ? "Color bars" : (front ? "Front camera" : "Camera")
    }

    /* A clip's still for the preview: its thumbnail once the row has made it. */
    func clipStill(_ name: String) -> UIImage? {
        Thumbs.cached(name)
    }

    /* Streaming, or a clip being encoded to stream. */
    func streamingOrPreparing() -> Bool {
        link != nil && (link.streaming() || preparing())
    }

    /* The wanted clip isn't encoded yet. */
    func preparing() -> Bool {
        guard let n = clipName(), link != nil, link.ready(), !ui.active,
              link.streaming() || grant.allows(selfStart: auto || wanted, proven: askOk()) else { return false }
        clipsLock.lock()
        let failed = failedClips.contains(clipKey(n, clipSize, clipKbps, clipVp8()))
        clipsLock.unlock()
        return bestClip(n, clipSize) == nil && !failed
    }

    /* How a clip stands, after its description: "", "Ready · ...", "Preparing…", "Failed". */
    func clipState(_ name: String) -> String {
        guard link != nil else { return "" }
        clipsLock.lock()
        let failed = failedClips.contains(clipKey(name, clipSize, clipKbps, clipVp8()))
        clipsLock.unlock()
        if failed {
            clipsLock.lock()
            let why = failReasons[name]
            clipsLock.unlock()
            return why.map { "\nFailed: " + $0 } ?? "\nFailed"
        }
        if let c = bestClip(name, clipSize) {
            return "\nReady · \(c.size) px · \(c.kbps) kb/s · \(c.loopBytes() / 1024) KB loop"
        }
        return sourceIs(Self.clipPrefix + name) && preparing() ? "\nPreparing…" : ""
    }

    /* A source row's tap: stream what (or switch to it). */
    func play(_ what: String) {
        pick(what)
        refreshUi()
    }

    func frontCamera() -> Bool {
        front
    }

    /* Front or back camera; remembered, and a camera stream starts over on it. */
    func setFront(_ f: Bool) {
        guard f != front else { return }
        front = f
        Prefs.setBool("front", f)
        cam?.front = f
        mlog("camera: " + (f ? "front" : "back"))
        if source == "camera" {
            restartRunning(for: [.camera])
        }
        refreshUi()
    }

    func fpsSetting() -> Int { fps }
    func sizeSetting() -> Int { size }
    func vp8Wanted() -> Bool { wantVp8 }
    func camKbpsFixed() -> Int { fixedCamKbps }
    func clipKbpsFixed() -> Int { fixedClipKbps }
    func kbpsCeiling() -> Int { maxKbps }
    func twoBands() -> Bool { !noBands }
    func useX264() -> Bool { baseSw != nil }
    func panelFilter() -> Bool { cam?.panelFilter ?? true }
    func autoStart() -> Bool { auto }

    /* The Codec row's description: whether VP8 can be had. */
    func codecNote() -> String {
        if !Vpx.available { return "This build has no VP8 encoder (native/build_vpx.sh)" }
        guard let c = caps() else { return "VP8 where the gadget decodes it" }
        return c.vp8 ? "The gadget decodes both" : "This gadget decodes H.264 only"
    }

    /* Two bands where the gadget takes them (session only, like -nobands); a camera stream starts over. */
    func setTwoBands(_ on: Bool) {
        noBands = !on
        restartLive()
    }

    /* x264 or the phone's encoder for H.264 (session only, like -sw). */
    func setUseX264(_ on: Bool) {
        baseSw = on && X264.available ? "x264" : nil
        restartLive()
    }

    func setPanelFilter(_ on: Bool) {
        cam?.panelFilter = on
        restartLive()
    }

    /* A live stream (the camera, the bars) starts over with new settings. */
    private func restartLive() {
        restartRunning(for: [.camera])
        refreshUi()
    }

    func setAutoStart(_ on: Bool) {
        auto = on
        if on {
            grant.grant()   /* this connection's, or the one coming up */
        } else {
            grant.revoke()
        }
        if on && link != nil && link.ready() && !link.streaming() {
            startStream()
        }
    }

    func overlayMode() -> StatsOverlay.Mode {
        screen?.overlay.mode ?? .compact
    }

    func setOverlayMode(_ m: StatsOverlay.Mode) {
        screen.overlay.mode = m
        Prefs.setInt("overlay", m.rawValue)
        refreshUi()
    }
}

/* The Device picker: gadgets found, the connected one marked; nil on Cancel. */
final class PickerController: UITableViewController {
    /* Fixed order (MainViewController.refreshPicker); reloaded without animation, so no row moves. */
    var rows: [(addr: String, title: String, detail: String, grey: Bool)] = [] {
        didSet { UIView.performWithoutAnimation { tableView.reloadData() } }
    }
    var onRescan: (() -> Void)?
    private let done: (String?) -> Void
    private var finished = false

    init(done: @escaping (String?) -> Void) {
        self.done = done
        super.init(style: .insetGrouped)
        title = "Choose a gadget"
        navigationItem.leftBarButtonItem = UIBarButtonItem(systemItem: .cancel, primaryAction: UIAction { [weak self] _ in
            self?.finish(nil)
        })
        navigationItem.rightBarButtonItem = UIBarButtonItem(title: "Rescan", primaryAction: UIAction { [weak self] _ in
            self?.onRescan?()
        })
    }

    required init?(coder: NSCoder) {
        fatalError()
    }

    override func tableView(_ tableView: UITableView, numberOfRowsInSection section: Int) -> Int {
        max(1, rows.count)
    }

    override func tableView(_ tableView: UITableView, cellForRowAt indexPath: IndexPath) -> UITableViewCell {
        let c = UITableViewCell(style: .subtitle, reuseIdentifier: nil)
        if rows.isEmpty {
            c.textLabel?.text = "Looking for gadgets…"
        } else {
            c.textLabel?.text = rows[indexPath.row].title
            c.detailTextLabel?.text = rows[indexPath.row].detail
            if rows[indexPath.row].grey {
                c.textLabel?.textColor = .secondaryLabel
                c.detailTextLabel?.textColor = .tertiaryLabel
            }
        }
        return c
    }

    override func tableView(_ tableView: UITableView, didSelectRowAt indexPath: IndexPath) {
        if indexPath.row < rows.count {
            finish(rows[indexPath.row].addr)
        }
    }

    override func viewDidDisappear(_ animated: Bool) {
        super.viewDidDisappear(animated)
        finish(nil)
    }

    private func finish(_ addr: String?) {
        if finished {
            return
        }
        finished = true
        done(addr)
        dismiss(animated: true)
    }
}

/* ---- musegadgets BLE, for the screen (BleTab) --------------------------------- */

extension MainViewController {
    func isBle() -> Bool { ble }

    /* Wi-Fi or BLE, each with the gadget last picked on it. */
    func setBle(_ on: Bool) {
        if on != ble && link != nil {
            switchLink()
        }
        refreshUi()
    }

    func bleReady() -> Bool {
        bleLink != nil && link.ready()
    }

    func toneOn() -> Bool {
        bleLink?.audioOn() ?? false
    }

    func setTone(_ on: Bool) {
        if on != toneOn() {
            toggleTone()
        }
    }

    func toneLc3() -> Bool { toneCodec == "lc3" }

    /* SBC or LC3 for the tone; a tone that's playing starts over in it. */
    func setToneLc3(_ lc3: Bool) {
        toneCodec = lc3 ? "lc3" : "sbc"
        if toneOn() {
            toggleTone()
            toggleTone()
        }
    }

    func speakerVolume() -> Int { bleLink?.speakerVolume ?? -1 }

    /* The gadget's speaker_volume setting (0 turns its speaker off). */
    func setSpeakerVolume(_ v: Int) {
        guard let g = bleLink else { return }
        if g.proofPending {
            volumeAfterProof = v
            return
        }
        g.setSpeakerVolume(v)
        mlog("speaker volume \(v)")
    }

    func micCodec() -> String? { micStarted ? mic : nil }

    /* Capture the gadget's mic as sbc or lc3 into Documents/mic.<codec>, or stop (nil). */
    func setMic(_ codec: String?) {
        guard let g = bleLink else { return }
        if micStarted {
            g.sendControl(Data([Mg.cmdStopMic]))
            flushMic()
            g.closeMic()
            micStarted = false
            mlog("mic capture off")
        }
        mic = codec
        micAfterProof = false
        if codec != nil {
            startMic()
        }
    }

    /*
     * The gadget's link only while the app is in the foreground: iOS keeps a
     * pending connect after the app is suspended and completes it whenever the gadget
     * advertises (after its reboot, say), which then holds the gadget so it stops advertising
     * to anything else. When the app leaves the foreground, the link closes at once, pending connect included (a timer would
     * not run: iOS suspends the app within seconds); it opens again in the foreground.
     */
    func appBackground() {
        inBackground = true
        parkLater()
    }

    func appForeground() {
        inBackground = false
        parkWork?.cancel()
        /* While a broadcast holds the gadget, resumeLinkAfterBroadcast opens the link after it. */
        if parked && !gadgetOff && !linkReleased {
            parked = false
            mlog("foreground: opening the link to the gadget again")
            link.close()
            makeLink()
            startLink()
        }
    }

    func parkLater() {
        parkWork?.cancel()
        guard inBackground, !parked, !linkReleased else { return }
        let w = DispatchWorkItem { [weak self] in
            guard let self, self.inBackground, !self.parked, !self.gadgetOff, !self.linkReleased, self.link != nil else { return }
            mlog("background: closing the link to the gadget")
            if self.link.streaming() { self.stopStream() }
            self.link.close()
            self.parked = true
        }
        parkWork = w
        DispatchQueue.main.async(execute: w)
    }

    func audioKbps() -> Double { uiAudioKbps }
    func micKbps() -> Double { uiMicKbps }
}
