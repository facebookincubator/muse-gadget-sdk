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
import UIKit

/*
 * Muse Gadgets Camera: streams the phone's camera (or colour bars, or a
 * sample clip) to a gadget's display over Wi-Fi (protocols/VIDEO.md), as the
 * Android app does: the gadget's suggested size and bitrate, an adaptive
 * bitrate for the camera, a bitrate ladder for clips, H.264 or VP8, and the
 * same buttons. Launch arguments as Android's extras (Util.swift Extras):
 * -target A1B2C3 -host 192.168.1.20 -fps 24 -kbps 1000 -auto YES -front YES
 * -codec vp8 -clip bunny -source pattern ...
 */
final class MainViewController: UIViewController, VideoLinkListener, CameraSink, ClipSink {
    private let status = UILabel(), sending = UILabel()
    private var previewLayer: AVCaptureVideoPreviewLayer?
    private var link: VideoLink!
    private var cam: CameraStreamer!
    private var target: String?, deviceAddr: String?
    private var fps = 30, kbps = 450, size = 0
    private var noBands = false
    /* The Codec button (or -codec h264|vp8): VP8 for the camera and the
     * clips where the gadget lists it and the app has libmgvpx, else H.264. Remembered. */
    private var wantVp8 = false, wantVp8Set = false
    private var codecButton: UIButton?, rateButton: UIButton?
    private var baseSw: String?   /* the H.264 encoder: x264, or nil for the phone's */
    private var camVp8 = false    /* the camera stream that's on is VP8 */
    private var auto = false, front = false, adaptive = true
    /* What streams: "camera", "pattern" or "clip:<name>". */
    private var source = "camera"
    private static let clipPrefix = "clip:"
    private var player: ClipStreamer?
    private let clipsLock = NSLock()
    private var clips: [String: Clip] = [:]
    private var failedClips: Set<String> = []
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
    private static let camRates = [150, 200, 250, 300, 400, 500, 650, 800, 1000, 1200]
    private static let sizes = [240, 320, 480]
    private var lastSkipped = 0
    private var lastGateUs: Int64 = 0
    private var calmSeconds = 0, gadgetBehindSeconds = 0
    private var seen: [String: (name: String, rssi: Int, at: Int64)] = [:]
    private var picker: PickerController?

    private func setLiveBudget() {
        let frame = Int64(kbps) * 1000 / 8 / Int64(max(1, curFps))
        link.liveAhead = live && !isClip() ? max(1600, frame * Int64(aheadPct) / 100) : 0
    }

    override var prefersStatusBarHidden: Bool { true }

    override func viewDidLoad() {
        super.viewDidLoad()
        view.backgroundColor = .black
        overrideUserInterfaceStyle = .dark
        readExtras()
        cam = CameraStreamer(sink: self)
        let pl = AVCaptureVideoPreviewLayer(session: cam.session)
        pl.videoGravity = .resizeAspectFill
        view.layer.addSublayer(pl)
        previewLayer = pl
        status.textColor = .white
        status.font = .monospacedSystemFont(ofSize: 10, weight: .regular)
        status.numberOfLines = 0
        status.translatesAutoresizingMaskIntoConstraints = false
        view.addSubview(status)
        sending.textColor = UIColor(red: 1, green: 0.835, blue: 0.31, alpha: 1)
        sending.font = .boldSystemFont(ofSize: 18)
        sending.numberOfLines = 0
        sending.textAlignment = .center
        sending.shadowColor = .black
        sending.text = "Not sending"
        var row1: [(String, String)] = [("Camera", "camera"), ("Bars", "pattern")]
        for c in SampleClips.all {
            row1.append((c.label, Self.clipPrefix + c.name))
        }
        let rows = [row1, [("Device", "device"), ("Rate", "rate"), ("H.264", "codec"), ("Stop", "stop")]]
        let stack = UIStackView()
        stack.axis = .vertical
        stack.spacing = 6
        stack.translatesAutoresizingMaskIntoConstraints = false
        stack.addArrangedSubview(sending)
        for r in rows {
            let bar = UIStackView()
            bar.axis = .horizontal
            bar.distribution = .fillEqually
            bar.spacing = 6
            for (label, what) in r {
                var cfg = UIButton.Configuration.filled()
                cfg.baseBackgroundColor = UIColor(white: 0.22, alpha: 0.9)
                cfg.baseForegroundColor = .white
                cfg.title = label
                cfg.titleLineBreakMode = what == "rate" ? .byWordWrapping : .byTruncatingTail
                cfg.contentInsets = NSDirectionalEdgeInsets(top: 10, leading: 2, bottom: 10, trailing: 2)
                cfg.titleTextAttributesTransformer = UIConfigurationTextAttributesTransformer { a in
                    var a = a
                    a.font = UIFont.systemFont(ofSize: r.count > 4 ? 14 : 16, weight: .medium)
                    return a
                }
                let b = UIButton(configuration: cfg, primaryAction: UIAction { [weak self] _ in self?.pick(what) })
                if what == "rate" { rateButton = b }
                if what == "codec" { codecButton = b }
                bar.addArrangedSubview(b)
            }
            stack.addArrangedSubview(bar)
        }
        view.addSubview(stack)
        let g = view.safeAreaLayoutGuide
        NSLayoutConstraint.activate([
            status.topAnchor.constraint(equalTo: g.topAnchor, constant: 4),
            status.leadingAnchor.constraint(equalTo: g.leadingAnchor, constant: 8),
            status.trailingAnchor.constraint(equalTo: g.trailingAnchor, constant: -8),
            stack.leadingAnchor.constraint(equalTo: g.leadingAnchor, constant: 8),
            stack.trailingAnchor.constraint(equalTo: g.trailingAnchor, constant: -8),
            stack.bottomAnchor.constraint(equalTo: g.bottomAnchor, constant: -8),
        ])
        begin()
        Timer.scheduledTimer(withTimeInterval: 1, repeats: true) { [weak self] _ in self?.tick() }
    }

    override func viewDidLayoutSubviews() {
        super.viewDidLayoutSubviews()
        let side = view.bounds.width
        previewLayer?.frame = CGRect(x: 0, y: (view.bounds.height - side) / 2, width: side, height: side)
        previewLayer?.cornerRadius = side / 2
        if let c = previewLayer?.connection, c.isVideoRotationAngleSupported(90) {
            c.videoRotationAngle = 90
        }
    }

    private func readExtras() {
        if let t = Extras.string("target") {
            target = t
            deviceAddr = nil
        }
        if let h = Extras.string("host") {
            deviceAddr = h
        }
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
        updateRateButton()
        if !wantVp8Set { wantVp8 = Prefs.bool("codec_vp8") }
        updateCodecButton()
        if target == nil && deviceAddr == nil {
            deviceAddr = Prefs.string("wifi_addr")
            target = Prefs.string("wifi_name")
        }
        link = WifiLink(listener: self)
        link.setTarget(address: deviceAddr, name: target)
        cam.front = front
        cam.meter = meter
        link.meter = meter
        cam.panelFilter = Extras.int("pf", 1) != 0
        /* The software encoder when this build has it (native/build_x264.sh;
         * -sw hw: the phone's encoder regardless). */
        let sw = Extras.string("sw")
        cam.swEncoder = sw != nil && sw != "vp8" ? (sw == "hw" ? nil : sw) : (X264.available ? "x264" : nil)
        baseSw = cam.swEncoder
        if let p = Extras.string("swpreset") { cam.swPreset = p }
        cam.swParams = Extras.string("swparams") ?? "no-deblock=1"
        cam.minAeFps = Extras.int("aefps", 30)
        mlog("Muse Gadgets Camera: x264 \(X264.available ? "built in" : "not built in"), VP8 \(Vpx.available ? "built in" : "not built in")")
        link.start()
    }

    /* ---- buttons -------------------------------------------------------- */

    func pick(_ what: String) {
        guard link != nil else { return }
        mlog("pick " + what)
        if what == "device" {
            showPicker()
            return
        }
        if what == "rate" {
            showRatePicker()
            return
        }
        if what == "codec" || what == "h264" || what == "vp8" {
            setCodec(what == "codec" ? !wantVp8 : what == "vp8")
            return
        }
        if what.hasPrefix(Self.clipPrefix) && isClip() && link.streaming() {
            source = what   /* one clip to another: the stream goes on, the clip switches */
            startClip(lastSize, link.caps())
            return
        }
        if link.streaming() {
            stopStream()
        }
        if what == "stop" {
            auto = false
            return
        }
        source = what
        auto = true
        if link.ready() {
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.3) { self.startStream() }
        }
    }

    /* ---- streaming ------------------------------------------------------ */

    private func startStream() {
        let c = link.caps()
        var s = size > 0 ? size : c.sugW
        s = min(s, min(c.maxW, c.maxH)) & ~7
        lastSize = s
        if isClip() {
            startClip(s, c)
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
        /* Past 240 px on a gadget that decodes bands in parallel, send two
         * (software encoders only: one per band). */
        let bands = c.maxTiles >= 2 && s > 240 && s % 32 == 0 && cam.swEncoder != nil && !noBands ? 2 : 1
        cam.bands = bands
        cam.bandSplitPct = c.tileSplitPct > 0 ? c.tileSplitPct : 50
        mlog("starting \(s)x\(s) @\(f) \(kbps) kb/s (adaptive up to \(maxKbps))" + (bands > 1 ? ", two bands" : ""))
        link.startStreaming(w: s, h: s, fps: f, kbps: kbps, playoutMs: 0, bands: bands, vp8: camVp8)
        setLiveBudget()
        cam.pattern = source == "pattern"
        if c.dispW > 0 && c.dispH > 0 {
            cam.panelPx = min(c.dispW, c.dispH)   /* what the gadget scales the picture to */
        }
        let round = c.round, kb = kbps
        if !cam.pattern && AVCaptureDevice.authorizationStatus(for: .video) != .authorized {
            AVCaptureDevice.requestAccess(for: .video) { ok in
                DispatchQueue.main.async {
                    if ok {
                        self.startCamera(s, f, kb, round)
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
        "\(name)|\(size)|\(kbps)" + (vp8 ? "|vp8" : "")
    }

    /* Clips as VP8: picked, the gadget decodes it, and the app has libvpx. */
    private func clipVp8() -> Bool {
        wantVp8 && link != nil && link.caps().vp8 && Vpx.available
    }

    /* The Codec button: H.264 or VP8 from now on, for whatever streams; the stream starts over in it. */
    private func setCodec(_ vp8: Bool) {
        wantVp8 = vp8
        Prefs.setBool("codec_vp8", vp8)
        updateCodecButton()
        mlog("codec: " + (vp8 ? "VP8" : "H.264"))
        if link != nil && link.streaming() {
            let src = source
            stopStream()
            source = src
            auto = true
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.3) { self.startStream() }
        }
    }

    private func updateCodecButton() {
        codecButton?.configuration?.title = wantVp8 ? "VP8" : "H.264"
    }

    /*
     * The bitrates clips are encoded at: clipTopKbps, then three steps down
     * (none below 180 kb/s); past 320 px the steps are small.
     */
    private func clipLevels() -> [Int] {
        let t = clipTopKbps
        let l = clipSize > 320 ? [t, t * 90 / 100, t * 80 / 100, t * 70 / 100] : [t, t * 78 / 100, t * 60 / 100, t * 45 / 100]
        return l.map { max(180, $0) }
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
        return clips[key] == nil && !failedClips.contains(key)
    }

    /* The wanted clip's best version: at clipKbps, else the nearest lower bitrate, else the lowest higher one. */
    private func bestClip(_ name: String, _ s: Int) -> Clip? {
        let vp8 = clipVp8()
        clipsLock.lock()
        defer { clipsLock.unlock() }
        if let exact = clips[clipKey(name, s, clipKbps, vp8)] {
            return exact
        }
        var below: Clip?, above: Clip?
        for c in clips.values where c.name == name && c.size == s && c.vp8 == vp8 {
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

    private func stopStream() {
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
                Prefs.setString("wifi_addr", addr)
                Prefs.setString("wifi_name", name)
                self.deviceAddr = addr
                self.target = name
                mlog("picked \(name) \(addr)")
                if self.link.streaming() {
                    self.stopStream()
                }
                self.link.select(address: addr, name: name)
            } else {
                self.link.pickerClosed()
            }
            self.picker = nil
        }
        picker = p
        present(UINavigationController(rootViewController: p), animated: true)
        refreshPicker()
        link.scanForPicker()
    }

    private func refreshPicker() {
        guard let p = picker else { return }
        let now = nowMs(), cur = link.connectedAddress()
        p.rows = seen.filter { now - $0.value.at < 15000 || $0.key == cur }
            .sorted { $0.value.rssi > $1.value.rssi }
            .map { ($0.key, $0.value.name + ($0.key == cur ? "  (connected)" : ""), $0.key.hasPrefix("svc:") ? "Bonjour" : $0.key) }
    }

    func onSeen(name: String, address: String, rssi: Int) {
        DispatchQueue.main.async {
            let old = self.seen[address]
            self.seen[address] = (name, rssi, nowMs())
            if old == nil || old!.name != name {
                self.refreshPicker()
            }
        }
    }

    /* ---- VideoLinkListener ---------------------------------------------- */

    func onStatus(_ s: String) {
        mlog(s)
        DispatchQueue.main.async { self.linkStatus = s }
    }

    func onReady(_ caps: Caps) {
        DispatchQueue.main.async { [self] in
            linkStatus = "Connected: \(caps)"
            clipSize = min(size > 0 ? size : caps.sugW, min(caps.maxW, caps.maxH)) & ~7
            clipRound = caps.round
            /* A gadget suggesting a high bitrate gets clips aimed at 92% of it
             * and a 300 ms playout delay for the peaks' queue. */
            if !clipKbpsSet {
                clipTopKbps = caps.sugKbps >= 600 ? caps.sugKbps * 92 / 100 : 450
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
            if (auto || wanted) && !link.streaming() {
                startStream()
            }
        }
    }

    func onRequestStreaming(_ want: Bool) {
        mlog("gadget asks for streaming: \(want)")
        DispatchQueue.main.async { [self] in
            wanted = want
            if want && !link.streaming() {
                startStream()
            } else if !want && link.streaming() {
                stopStream()
            }
        }
    }

    func onKeyframeRequest() {
        mlog("keyframe requested")
        cam.requestKeyframe()
        player?.requestKeyframe()
    }

    func onStreamStopped() {
        mlog("gadget stopped the stream")
        DispatchQueue.main.async {
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
        mlog("gadget refused the stream (\(reason)); trying again in 1 s")
        DispatchQueue.main.asyncAfter(deadline: .now() + 1) { [self] in
            if !(auto || wanted) || !link.ready() || link.streaming() {
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

    func onDisconnected() {
        mlog("disconnected")
        DispatchQueue.main.async {
            self.cam.stop()
            self.player?.stop()
        }
    }

    /* ---- Rate ----------------------------------------------------------- */

    private func showRatePicker() {
        var items: [(String, () -> Void)] = []
        func check(_ on: Bool) -> String { on ? "\u{2713} " : "" }
        items.append((check(fixedClipKbps == 0) + "Clips: auto (step down when the link falls behind)", { self.setClipRate(0) }))
        for k in clipLevels() {
            items.append((check(fixedClipKbps == k) + "Clips: \(k) kb/s", { self.setClipRate(k) }))
        }
        items.append((check(fixedCamKbps == 0) + "Camera: auto (adaptive, up to \(maxKbps))", { self.setCamRate(0) }))
        for k in Self.camRates {
            items.append((check(fixedCamKbps == k) + "Camera: \(k) kb/s", { self.setCamRate(k) }))
        }
        let caps = link?.caps()
        let maxSize = caps != nil ? min(caps!.maxW, caps!.maxH) : 240
        if maxSize > 240 {
            items.append((check(size == 0) + "Size: auto (the gadget's \(caps?.sugW ?? 0) px)", { self.setSize(0) }))
            for k in Self.sizes where k <= maxSize {
                items.append((check(size == k) + "Size: \(k) px", { self.setSize(k) }))
            }
        }
        items.append((check(fps == 30) + "Camera frame rate: 30 fps (smoothest)", { self.setFps(30) }))
        items.append((check(fps == 24) + "Camera frame rate: 24 fps (sharper at the same bitrate)", { self.setFps(24) }))
        let a = UIAlertController(title: "Bitrate", message: nil, preferredStyle: .actionSheet)
        for (label, act) in items {
            a.addAction(UIAlertAction(title: label, style: .default) { _ in act() })
        }
        a.addAction(UIAlertAction(title: "Cancel", style: .cancel))
        a.popoverPresentationController?.sourceView = rateButton
        present(a, animated: true)
    }

    private func setClipRate(_ k: Int) {
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
        updateRateButton()
    }

    private func setCamRate(_ k: Int) {
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
        updateRateButton()
    }

    private func setFps(_ f: Int) {
        fps = f
        Prefs.setInt("fps", f)
        mlog("camera frame rate \(f) fps")
        if link != nil && link.streaming() && !isClip() {
            stopStream()
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.3) { self.startStream() }
        }
        updateRateButton()
    }

    /* A new picture size restarts what streams (0: the gadget's suggestion). */
    private func setSize(_ k: Int) {
        size = k
        Prefs.setInt("size", k)
        mlog("picture size " + (k > 0 ? "\(k) px" : "auto"))
        if link != nil && link.streaming() {
            stopStream()
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.3) { self.startStream() }
        }
        updateRateButton()
    }

    private func updateRateButton() {
        guard let b = rateButton else { return }
        if fixedClipKbps == 0 && fixedCamKbps == 0 && size == 0 {
            b.configuration?.title = "Rate"
        } else {
            b.configuration?.title = (fixedClipKbps > 0 ? "Clip \(fixedClipKbps)" : "Clip -") + "\n"
                + (fixedCamKbps > 0 ? "Cam \(fixedCamKbps)" : "Cam -") + (size > 0 ? "\n\(size)px" : "")
        }
    }

    /* The picture going to the gadget: codec, size, frame rate, bands, the bitrate and what the link carried, its source. */
    private func sendingLine(_ sentKbps: Double) -> String {
        if link == nil || !link.streaming() {
            return "Not sending"
        }
        if isClip() {
            guard let c = player?.clip else { return "Preparing clip" }
            return String(format: "Sending %@ %d×%d · %d fps\n%d kb/s (%@) · link %.0f kb/s\n%@", c.vp8 ? "VP8" : "H.264",
                          c.size, c.size, c.fps, c.kbps, fixedClipKbps > 0 ? "fixed" : "auto", sentKbps, clipLabel(c.name))
        }
        let src = source == "pattern" ? "Bars" : "Camera"
        let enc = cam.encoderName.isEmpty ? "encoder starting" : String(cam.encoderName.split(separator: " ").first ?? "")
        return String(format: "Sending %@ %d×%d · %d fps%@\n%d kb/s (%@) · link %.0f kb/s\n%@ · %@", camVp8 ? "VP8" : "H.264",
                      lastSize, lastSize, curFps, cam.bands == 2 ? " · 2 bands" : "", kbps, fixedCamKbps > 0 ? "fixed" : "auto",
                      sentKbps, src, enc)
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
        let floor = lastSize > 320 ? max(minKbps, maxKbps * 2 / 3) : minKbps
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
        let now = nowMs()
        let dt = max(1, now - lastStatMs)
        let bytes = link.statBytes
        let av = isClip() && player != nil
        let fed = av ? player!.statSent : cam.statFed, enc = av ? player!.statSent : cam.statEncoded, shown = devShown
        let dEnc = Double(max(1, enc - lastEnc))
        let lat = String(format: "cam->enc %.0f ms (convert %.1f, encoder %.1f)  gate %.0f ms/s",
                         Double(cam.statCamToEncUs - lastCamEnc) / 1000 / dEnc,
                         Double(cam.statConvUs - lastConv) / 1000 / Double(max(1, fed - lastFed)),
                         Double(cam.statEncUs - lastEncUs) / 1000 / dEnc, Double(link.statGateUs - lastGate) / 1000 * 1000 / Double(dt))
        lastCamEnc = cam.statCamToEncUs
        lastEncUs = cam.statEncUs
        lastConv = cam.statConvUs
        lastGate = link.statGateUs
        let mode = (link.streaming() ? "STREAMING" : "idle") + ((isClip() ? fixedClipKbps : fixedCamKbps) > 0 ? " (fixed rate)" : " (auto rate)")
        let s = String(format: "%@\n%@  %d kb/s target  %@\nsend %.0f kb/s  in flight %lld  queued %lld\ncamera->enc %.1f fps  encoded %.1f fps  skipped %d  keys %d\n%@\ngadget shown %.1f fps  dropped %d  decode %d ms  free %lld",
                       linkStatus, link.describe(), isClip() ? clipKbps : kbps, mode,
                       Double(bytes - lastBytes) * 8 / Double(dt), link.inFlight(), link.queuedBytes(),
                       Double(fed - lastFed) * 1000 / Double(dt), Double(enc - lastEnc) * 1000 / Double(dt), cam.statSkipped,
                       cam.statKeys, lat, Double(shown - lastShown) * 1000 / Double(dt), devDropped, devMs, devAvail)
        status.text = s
        if link.streaming() && !isClip() {
            mlog(meter.take())
        }
        if link.streaming() {
            mlog("stats: " + s.replacingOccurrences(of: "\n", with: "|"))
        }
        adaptBitrate(dt, bytes - lastBytes, Double(enc - lastEnc) * 1000 / Double(dt), Double(shown - lastShown) * 1000 / Double(dt))
        sending.text = sendingLine(Double(bytes - lastBytes) * 8 / Double(dt))
        adaptClip()
        lastStatMs = now
        lastBytes = bytes
        lastFed = fed
        lastEnc = enc
        lastShown = shown
    }
}

/* The Device picker: gadgets found, the connected one marked; nil on Cancel. */
final class PickerController: UITableViewController {
    var rows: [(addr: String, title: String, detail: String)] = [] {
        didSet { tableView.reloadData() }
    }
    private let done: (String?) -> Void
    private var finished = false

    init(done: @escaping (String?) -> Void) {
        self.done = done
        super.init(style: .insetGrouped)
        title = "Connect to"
        navigationItem.leftBarButtonItem = UIBarButtonItem(systemItem: .cancel, primaryAction: UIAction { [weak self] _ in
            self?.finish(nil)
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
            c.textLabel?.text = "Looking on this Wi-Fi network..."
        } else {
            c.textLabel?.text = rows[indexPath.row].title
            c.detailTextLabel?.text = rows[indexPath.row].detail
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
