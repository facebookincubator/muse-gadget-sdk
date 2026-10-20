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
import Foundation

protocol CameraSink: AnyObject {
    func onFrame(_ frame: Data, key: Bool)
    /* true: skip this camera frame (the link is behind). */
    func behind() -> Bool
}

/*
 * Camera frames (or color bars), cropped square, upright, scaled down for
 * the panel (PanelScaler) and blacked out in the 4x4 blocks past the round
 * display's edge, into x264, VP8 or the phone's H.264 encoder; each encoded
 * frame goes out with its frame header (VideoFrame). Frames are paced to the
 * target rate and skipped while the link is behind, so the encoder's
 * references never break. Android's CameraStreamer, on AVFoundation.
 */
final class CameraStreamer: NSObject, AVCaptureVideoDataOutputSampleBufferDelegate {
    static let maskMargin = 3.0

    private weak var sink: CameraSink?
    let session = AVCaptureSession()
    private let q = DispatchQueue(label: "cam", qos: .userInteractive)
    private var dataOut: AVCaptureVideoDataOutput?
    private var input: AVCaptureDeviceInput?
    private var timer: DispatchSourceTimer?

    var front = false
    /* Color bars and a moving box instead of the camera. */
    var pattern = false
    /* Downscale for the panel (PanelScaler) instead of the 2x2 box; panelPx:
     * the gadget's display, which it stretches the picture to. */
    var panelFilter = true
    var panelPx = 360
    /* Lowest frame rate exposure may stretch to in dim light (30: fixed, the least latency). */
    var minAeFps = 30
    /* "x264" (X264), "vp8" (Vpx) or nil: the phone's encoder (VTH264). */
    var swEncoder: String?
    var swPreset = "fast", swParams: String?
    /* Two bands, each its own encoder; the top band's share of the height, %. */
    var bands = 1
    var bandSplitPct = 50
    var meter: LatencyMeter?

    private(set) var running = false
    private var outW = 232, outH = 232, fps = 30
    private(set) var kbps = 450
    private var round = true
    private var blkOut: [Bool] = []
    private var blkW = 0
    private var nextFeedNs: Int64 = 0
    private var frameNo = 0
    private let emitLock = NSLock()

    var statFed = 0, statSkipped = 0, statEncoded = 0, statKeys = 0
    var statCamToEncUs: Int64 = 0, statEncUs: Int64 = 0, statConvUs: Int64 = 0, statEncBytes: Int64 = 0
    var statQpSum = 0, statQpN = 0
    private(set) var encoderName = ""

    private var sw: SwEncoder?, sw1: SwEncoder?
    private var vt: VTH264?
    private var swKey = true
    private var band0Rows = 0
    private var inflight: [Int64: (Int64, Int64)] = [:]
    private let inflightLock = NSLock()

    /* I420 work buffers: the picture at the stream's size, and the two bands. */
    private var pic: [UInt8] = [], band0: [UInt8] = [], band1: [UInt8] = []
    private var scY: PanelScaler?, scC: PanelScaler?
    private var smY: [UInt8] = [], smU: [UInt8] = [], smV: [UInt8] = []
    private var geomKey = 0
    private var cropX = 0, cropY = 0, cropSide = 0
    private var patternFrame = 0

    init(sink: CameraSink) {
        self.sink = sink
    }

    func start(w: Int, h: Int, fps: Int, kbps: Int, round: Bool) throws {
        stop()
        try q.sync {
            outW = w
            outH = h
            self.fps = fps
            self.kbps = kbps
            self.round = round
            buildMask()
            pic = [UInt8](repeating: 0, count: w * h * 3 / 2)
            geomKey = 0
            frameNo = 0
            try startEncoder()
            running = true
        }
        if pattern {
            let t = DispatchSource.makeTimerSource(queue: q)
            t.schedule(deadline: .now(), repeating: .nanoseconds(1_000_000_000 / max(1, fps)))
            t.setEventHandler { [weak self] in self?.patternTick() }
            t.resume()
            timer = t
        } else {
            try openCamera()
        }
    }

    func stop() {
        timer?.cancel()
        timer = nil
        if session.isRunning {
            session.stopRunning()
        }
        q.sync {
            running = false
            sw?.close()
            sw1?.close()
            sw = nil
            sw1 = nil
            vt?.close()
            vt = nil
        }
        inflightLock.lock()
        inflight.removeAll()
        inflightLock.unlock()
    }

    func requestKeyframe() {
        q.async { self.swKey = true }
    }

    func setBitrate(_ k: Int) {
        q.async { [self] in
            kbps = k
            if let s = sw {
                let k0 = sw1 != nil ? k * band0Rows / max(1, outH) : k
                s.setBitrate(k0)
                sw1?.setBitrate(k - k0)
            }
            vt?.setBitrate(k)
        }
    }

    /* ---- encoder -------------------------------------------------------- */

    /*
     * Past 240 px, decoder-friendly motion vectors (x264 mv-cheap=2, from
     * android/musegadgetstest/jni/x264_mvcheap.py), trellis and the wider motion
     * search, as on Android. An explicit option in params wins.
     */
    static func withMvCheap(_ params: String?, width: Int) -> String? {
        if width <= 240 {
            return params
        }
        var p = params ?? ""
        if !p.contains("mv-cheap") {
            p += (p.isEmpty ? "" : ":") + "mv-cheap=2"
        }
        if !p.contains("trellis") {
            p += ":trellis=2"
        }
        if !p.contains("me=") {
            p += ":me=umh"
        }
        return p
    }

    /*
     * VP8 settings for a picture this wide, unless params names its own:
     * libvpx's good mode with no lag, version 1, no loop filter, one thread;
     * up to 240 px cpu-used 1 and every motion vector, past it cpu-used 2
     * and decoder-friendly motion vectors (bands encoded side by side).
     */
    static func vp8Params(_ params: String?, width: Int) -> String {
        if let p = params, p.contains("profile=") {
            return p
        }
        return width <= 240 ? "cpu=1:deadline=good:profile=1:lf=0:mvcheap=0:threads=1"
                            : "cpu=2:deadline=good:profile=1:lf=0:mvcheap=1:threads=1"
    }

    private func startEncoder() throws {
        let vp8 = swEncoder == "vp8"
        if swEncoder == "x264" || vp8 {
            if vp8 ? !Vpx.available : !X264.available {
                throw NSError(domain: "MuseCam", code: 1, userInfo: [NSLocalizedDescriptionKey:
                    vp8 ? "libmgvpx is not in this build (native/build_vpx.sh)" : "libmgx264 is not in this build (native/build_x264.sh)"])
            }
            let two = bands == 2 && outH % 32 == 0
            let h0 = two ? max(16, min(outH - 16, (outH * bandSplitPct / 100 + 8) / 16 * 16)) : outH
            band0Rows = h0
            let k0 = two ? kbps * h0 / outH : kbps
            let params = vp8 ? Self.vp8Params(swParams, width: outW) : Self.withMvCheap(swParams, width: outW)
            func open(_ h: Int, _ k: Int) -> SwEncoder? {
                vp8 ? Vpx(w: outW, h: h, fps: fps, kbps: k, params: params)
                    : X264(w: outW, h: h, fps: fps, kbps: k, preset: swPreset, params: params)
            }
            sw = open(h0, k0)
            sw1 = two ? open(outH - h0, kbps - k0) : nil
            if sw == nil || (two && sw1 == nil) {
                throw NSError(domain: "MuseCam", code: 2, userInfo: [NSLocalizedDescriptionKey: "\(swEncoder!) open failed"])
            }
            band0 = [UInt8](repeating: 0, count: outW * h0 * 3 / 2)
            band1 = two ? [UInt8](repeating: 0, count: outW * (outH - h0) * 3 / 2) : []
            swKey = true
            encoderName = (vp8 ? "VP8 " : "x264 \(swPreset)") + (params != nil ? " " + params! : "")
                + (two ? ", two bands of \(h0) and \(outH - h0) rows" : "")
            mlog("encoder \(encoderName) \(outW)x\(outH) \(kbps) kb/s")
            return
        }
        guard let v = VTH264(w: outW, h: outH, fps: fps, kbps: kbps, live: true, output: { [weak self] au, key, idx in
            guard let self else { return }
            self.inflightLock.lock()
            let ts = self.inflight.removeValue(forKey: idx)
            self.inflightLock.unlock()
            self.emit(au, key: key, capNs: ts?.0 ?? 0, convNs: ts?.1 ?? nowNs())
        }) else {
            throw NSError(domain: "MuseCam", code: 3, userInfo: [NSLocalizedDescriptionKey: "no H.264 encoder"])
        }
        vt = v
        swKey = true
        encoderName = v.name
        mlog("encoder \(encoderName) \(outW)x\(outH) \(kbps) kb/s")
    }

    /* An encoded frame: framed and handed to the sink. */
    private func emit(_ au: Data, key: Bool, capNs: Int64, convNs: Int64) {
        emitLock.lock()
        let now = nowNs()
        if capNs > 0 {
            statCamToEncUs += (now - capNs) / 1000
        }
        statEncUs += (now - convNs) / 1000
        meter?.captured(frameNo, captureNs: capNs > 0 ? capNs : convNs, encodedNs: now)
        let fr = VideoFrame.make(au, number: frameNo, key: key)
        frameNo += 1
        statEncoded += 1
        statEncBytes += Int64(au.count)
        if key {
            statKeys += 1
        }
        emitLock.unlock()
        sink?.onFrame(fr, key: key)
    }

    /* pic -> the encoder, on the camera queue. */
    private func encodePicture(capNs: Int64, convNs: Int64) {
        guard running else { return }
        let idr = swKey
        swKey = false
        if let v = vt {
            let idx = Int64(statFed)
            inflightLock.lock()
            inflight[idx] = (capNs, convNs)
            inflightLock.unlock()
            pic.withUnsafeBufferPointer { v.encode(i420: $0.baseAddress!, index: idx, fps: fps, key: idr) }
            statFed += 1
            return
        }
        guard let s0 = sw else { return }
        var out: (data: Data, key: Bool, qp: Int)?
        if let s1 = sw1 {
            let h0 = band0Rows, ys = outW * outH, cs = ys / 4
            for b in 0..<2 {
                let r0 = b == 0 ? 0 : h0, rows = b == 0 ? h0 : outH - h0
                let bys = outW * rows, bcs = bys / 4
                let fill = { (dst: UnsafeMutablePointer<UInt8>) in
                    self.pic.withUnsafeBufferPointer { p in
                        let src = p.baseAddress!
                        memcpy(dst, src + self.outW * r0, bys)
                        memcpy(dst + bys, src + ys + self.outW * r0 / 4, bcs)
                        memcpy(dst + bys + bcs, src + ys + cs + self.outW * r0 / 4, bcs)
                    }
                }
                if b == 0 {
                    band0.withUnsafeMutableBufferPointer { fill($0.baseAddress!) }
                } else {
                    band1.withUnsafeMutableBufferPointer { fill($0.baseAddress!) }
                }
            }
            var r0: (data: Data, key: Bool, qp: Int)?, r1: (data: Data, key: Bool, qp: Int)?
            let pts = Int64(statFed)
            let b0 = band0, b1 = band1
            if swEncoder == "vp8" {
                DispatchQueue.concurrentPerform(iterations: 2) { i in
                    if i == 0 {
                        r0 = b0.withUnsafeBufferPointer { s0.encode($0.baseAddress!, pts: pts, key: idr) }
                    } else {
                        r1 = b1.withUnsafeBufferPointer { s1.encode($0.baseAddress!, pts: pts, key: idr) }
                    }
                }
            } else {
                r0 = b0.withUnsafeBufferPointer { s0.encode($0.baseAddress!, pts: pts, key: idr) }
                r1 = b1.withUnsafeBufferPointer { s1.encode($0.baseAddress!, pts: pts, key: idr) }
            }
            guard let a0 = r0, let a1 = r1, !a0.data.isEmpty, !a1.data.isEmpty else {
                swKey = true   /* a band came out empty: start both over */
                statFed += 1
                return
            }
            var au = Data()
            let n = a0.data.count
            au.append(contentsOf: [UInt8(n & 0xff), UInt8((n >> 8) & 0xff), UInt8((n >> 16) & 0xff), UInt8((n >> 24) & 0xff)])
            au.append(a0.data)
            au.append(a1.data)
            out = (au, a0.key && a1.key, (a0.qp + a1.qp) / 2)
        } else {
            out = pic.withUnsafeBufferPointer { s0.encode($0.baseAddress!, pts: Int64(statFed), key: idr) }
        }
        statFed += 1
        guard let o = out, !o.data.isEmpty else { return }
        statQpSum += o.qp
        statQpN += 1
        emit(o.data, key: o.key, capNs: capNs, convNs: convNs)
    }

    /* ---- camera --------------------------------------------------------- */

    private func openCamera() throws {
        let pos: AVCaptureDevice.Position = front ? .front : .back
        guard let dev = AVCaptureDevice.default(.builtInWideAngleCamera, for: .video, position: pos) else {
            throw NSError(domain: "MuseCam", code: 4, userInfo: [NSLocalizedDescriptionKey: "no camera"])
        }
        session.beginConfiguration()
        /* At least twice the stream's size on the short side, for the scaler. */
        let want: AVCaptureSession.Preset = max(outW, outH) * 2 > 480 ? .hd1280x720 : .vga640x480
        session.sessionPreset = session.canSetSessionPreset(want) ? want : .high
        if let i = input {
            session.removeInput(i)
        }
        let inp = try AVCaptureDeviceInput(device: dev)
        if session.canAddInput(inp) {
            session.addInput(inp)
        }
        input = inp
        if dataOut == nil {
            let o = AVCaptureVideoDataOutput()
            o.videoSettings = [kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange]
            o.alwaysDiscardsLateVideoFrames = true
            o.setSampleBufferDelegate(self, queue: q)
            if session.canAddOutput(o) {
                session.addOutput(o)
            }
            dataOut = o
        }
        if let c = dataOut?.connection(with: .video) {
            /* Upright for a phone held in portrait, mirrored like a selfie camera at the front. */
            if c.isVideoRotationAngleSupported(90) {
                c.videoRotationAngle = 90
            }
            if c.isVideoMirroringSupported {
                c.automaticallyAdjustsVideoMirroring = false
                c.isVideoMirrored = front
            }
            /* No stabilization: it holds frames back (latency). */
            if c.isVideoStabilizationSupported {
                c.preferredVideoStabilizationMode = .off
            }
        }
        session.commitConfiguration()
        do {
            try dev.lockForConfiguration()
            let f = CMTime(value: 1, timescale: 30)
            let slow = CMTime(value: 1, timescale: CMTimeScale(max(1, min(30, minAeFps))))
            if dev.activeFormat.videoSupportedFrameRateRanges.contains(where: { $0.maxFrameRate >= 30 }) {
                dev.activeVideoMinFrameDuration = f
                dev.activeVideoMaxFrameDuration = slow
            }
            dev.unlockForConfiguration()
        } catch {
            mlog("camera frame rate: \(error)")
        }
        mlog("camera \(pos == .front ? "front" : "back") \(session.sessionPreset.rawValue)")
        DispatchQueue.global(qos: .userInitiated).async { self.session.startRunning() }
    }

    func captureOutput(_ output: AVCaptureOutput, didOutput sb: CMSampleBuffer, from connection: AVCaptureConnection) {
        guard running, let pb = CMSampleBufferGetImageBuffer(sb) else { return }
        let now = nowNs()
        if now < nextFeedNs {
            return   /* above the target rate */
        }
        let period = Int64(1_000_000_000 / max(1, fps))
        nextFeedNs = max(nextFeedNs + period, now - period / 2)
        if sink?.behind() ?? false {
            statSkipped += 1
            return
        }
        let c0 = nowNs()
        convert(pb)
        let c1 = nowNs()
        statConvUs += (c1 - c0) / 1000
        let sensor = Int64(CMTimeGetSeconds(CMSampleBufferGetPresentationTimeStamp(sb)) * 1e9)
        encodePicture(capNs: abs(c1 - sensor) < 2_000_000_000 ? sensor : 0, convNs: c1)
    }

    private func buildMask() {
        var spanLo = [Int](repeating: 0, count: outH), spanHi = [Int](repeating: outW, count: outH)
        let r = Double(min(outW, outH)) / 2, cx = Double(outW) / 2, cy = Double(outH) / 2
        for y in 0..<outH where round {
            let dy = Double(y) + 0.5 - cy
            let half = r * r - dy * dy
            if half <= 0 {
                spanLo[y] = Int(cx)
                spanHi[y] = Int(cx)
                continue
            }
            /* Past the circle by maskMargin: a gadget's bilinear upscale reads
             * a pixel beyond the edge. */
            let hw = half.squareRoot() + Self.maskMargin
            spanLo[y] = max(0, Int((cx - hw).rounded(.down)))
            spanHi[y] = min(outW, Int((cx + hw).rounded(.up)))
        }
        blkW = (outW + 3) / 4
        let blkH = (outH + 3) / 4
        blkOut = [Bool](repeating: true, count: blkW * blkH)
        for by in 0..<blkH {
            for bx in 0..<blkW {
                var out = true
                for y in (by * 4)..<min(outH, by * 4 + 4) where spanHi[y] > bx * 4 && spanLo[y] < min(outW, bx * 4 + 4) {
                    out = false
                }
                blkOut[by * blkW + bx] = out
            }
        }
    }

    /*
     * The center square of a camera frame (already upright) into pic at the
     * stream's size: PanelScaler on luma and on the interleaved chroma, then
     * the round mask. Without the panel filter, a 2x2 box.
     */
    private func convert(_ pb: CVPixelBuffer) {
        CVPixelBufferLockBaseAddress(pb, .readOnly)
        defer { CVPixelBufferUnlockBaseAddress(pb, .readOnly) }
        let w = CVPixelBufferGetWidthOfPlane(pb, 0), h = CVPixelBufferGetHeightOfPlane(pb, 0)
        let ys = CVPixelBufferGetBytesPerRowOfPlane(pb, 0), cs = CVPixelBufferGetBytesPerRowOfPlane(pb, 1)
        guard let yb = CVPixelBufferGetBaseAddressOfPlane(pb, 0)?.assumingMemoryBound(to: UInt8.self),
              let cb = CVPixelBufferGetBaseAddressOfPlane(pb, 1)?.assumingMemoryBound(to: UInt8.self) else { return }
        let key = ((w * 31 + h) * 31 + ys) * 31 + outW + (panelFilter ? 1 : 0)
        if key != geomKey {
            geomKey = key
            cropSide = min(w, h) & ~1
            cropX = ((w - cropSide) / 2) & ~1
            cropY = ((h - cropSide) / 2) & ~1
            if panelFilter {
                let t0 = nowMs()
                scY = PanelScaler(nIn: cropSide, nOut: outW, panel: panelPx, lambda: 0.2, halfTaps: 3)
                scC = PanelScaler(nIn: cropSide / 2, nOut: outW / 2, panel: panelPx / 2, lambda: 0.2, halfTaps: 3)
                smY = [UInt8](repeating: 0, count: outW * outW)
                smU = [UInt8](repeating: 0, count: outW * outW / 4)
                smV = [UInt8](repeating: 0, count: outW * outW / 4)
                mlog("panel filter \(cropSide) -> \(outW) (panel \(panelPx)): \(nowMs() - t0) ms to build")
            } else {
                scY = nil
            }
        }
        let n = outW, cn = outW / 2
        pic.withUnsafeMutableBufferPointer { pp in
            let oy = pp.baseAddress!, ou = oy + n * n, ov = ou + cn * cn
            if let sy = scY, let sc = scC {
                smY.withUnsafeMutableBufferPointer { sy.scale(yb, rowStride: ys, pixStride: 1, x0: cropX, y0: cropY, $0.baseAddress!) }
                smU.withUnsafeMutableBufferPointer { sc.scale(cb, rowStride: cs, pixStride: 2, x0: cropX / 2, y0: cropY / 2, $0.baseAddress!) }
                smV.withUnsafeMutableBufferPointer { sc.scale(cb + 1, rowStride: cs, pixStride: 2, x0: cropX / 2, y0: cropY / 2, $0.baseAddress!) }
                for y in 0..<n {
                    for x in 0..<n {
                        oy[y * n + x] = blkOut[(y >> 2) * blkW + (x >> 2)] ? 16 : smY[y * n + x]
                    }
                }
                for y in 0..<cn {
                    for x in 0..<cn {
                        let i = y * cn + x
                        if blkOut[(y >> 1) * blkW + (x >> 1)] {
                            ou[i] = 128
                            ov[i] = 128
                        } else {
                            ou[i] = smU[i]
                            ov[i] = smV[i]
                        }
                    }
                }
                return
            }
            let step = Double(cropSide) / Double(n)
            for y in 0..<n {
                let sy = min(cropSide - 2, max(0, Int((Double(y) + 0.5) * step - 0.5))) + cropY
                for x in 0..<n {
                    if blkOut[(y >> 2) * blkW + (x >> 2)] {
                        oy[y * n + x] = 16
                        continue
                    }
                    let sx = min(cropSide - 2, max(0, Int((Double(x) + 0.5) * step - 0.5))) + cropX
                    let s = yb + sy * ys + sx
                    oy[y * n + x] = UInt8((Int(s[0]) + Int(s[1]) + Int(s[ys]) + Int(s[ys + 1]) + 2) >> 2)
                }
            }
            for y in 0..<cn {
                let sy = (min(cropSide - 2, max(0, Int((Double(2 * y) + 0.5) * step - 0.5))) + cropY) / 2
                for x in 0..<cn {
                    let i = y * cn + x
                    if blkOut[(y >> 1) * blkW + (x >> 1)] {
                        ou[i] = 128
                        ov[i] = 128
                        continue
                    }
                    let sx = (min(cropSide - 2, max(0, Int((Double(2 * x) + 0.5) * step - 0.5))) + cropX) / 2
                    ou[i] = cb[sy * cs + 2 * sx]
                    ov[i] = cb[sy * cs + 2 * sx + 1]
                }
            }
        }
    }

    /* Color bars (BT.601 limited), a gray ramp on top and a moving box. */
    private func patternTick() {
        guard running else { return }
        if sink?.behind() ?? false {
            statSkipped += 1
            return
        }
        let bars: [[Int]] = [[235, 128, 128], [210, 16, 146], [170, 166, 16], [145, 54, 34],
                             [106, 202, 222], [81, 90, 240], [41, 240, 110], [16, 128, 128]]
        let t = patternFrame
        patternFrame += 1
        let bx = Int(Double(outW - 48) * (0.5 + 0.5 * sin(Double(t) * 2 * .pi / 90)))
        let by = Int(Double(outH - 48) * (0.5 + 0.5 * cos(Double(t) * 2 * .pi / 120)))
        let w = outW, h = outH, cw = w / 2
        pic.withUnsafeMutableBufferPointer { pp in
            let oy = pp.baseAddress!, ou = oy + w * h, ov = ou + cw * (h / 2)
            for y in 0..<h {
                for x in 0..<w {
                    var v = bars[x * 8 / w][0]
                    if y < h / 6 {
                        v = 16 + 219 * x / w
                    }
                    if x >= bx && x < bx + 48 && y >= by && y < by + 48 {
                        v = 235
                    }
                    if blkOut[(y >> 2) * blkW + (x >> 2)] {
                        v = 16
                    }
                    oy[y * w + x] = UInt8(v)
                }
            }
            for y in 0..<(h / 2) {
                for x in 0..<cw {
                    let c = bars[(2 * x) * 8 / w]
                    var u = c[1], v = c[2]
                    let X = 2 * x, Y = 2 * y
                    if Y < h / 6 || (X >= bx && X < bx + 48 && Y >= by && Y < by + 48) || blkOut[(Y >> 2) * blkW + (X >> 2)] {
                        u = 128
                        v = 128
                    }
                    ou[y * cw + x] = UInt8(u)
                    ov[y * cw + x] = UInt8(v)
                }
            }
        }
        let now = nowNs()
        encodePicture(capNs: now, convNs: now)
    }
}
