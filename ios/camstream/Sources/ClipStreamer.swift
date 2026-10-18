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

protocol ClipSink: AnyObject {
    func onFrame(_ frame: Data, key: Bool)
    func queuedBytes() -> Int64
}

/* One clip, encoded for one size and bitrate. */
final class Clip {
    let name: String
    let size: Int, fps: Int, kbps: Int
    var keyed: [Data] = [], steady: [Data] = []
    /* A closed loop: every loop starts on its own keyframe (keyed and steady are the same frames). */
    var closed = false
    /* VP8 frames (Vpx), else H.264 access units. */
    var vp8 = false
    var info = ""
    var maxFrame = 0   /* the largest frame (a keyframe takes a while to go out: no hold for that) */

    init(name: String, size: Int, fps: Int, kbps: Int) {
        self.name = name
        self.size = size
        self.fps = fps
        self.kbps = kbps
    }

    func measure() {
        maxFrame = (keyed + steady).map { $0.count }.max() ?? 0
    }

    func loopBytes() -> Int {
        steady.reduce(0) { $0 + $1.count }
    }

    func steadyKbps() -> Int {
        steady.isEmpty ? 0 : loopBytes() * 8 * fps / steady.count / 1000
    }
}

/*
 * Pre-recorded clips (sample movies) as loops, as Android's ClipStreamer:
 * each is decoded once, area-averaged to the gadget's size, masked to its
 * circle and encoded ahead of time: twice through one encoder, a keyed loop
 * (a keyframe, then predicted frames) and a steady loop that follows on from
 * either, so a loop wraps without a keyframe; a keyed loop goes out again
 * every few loops, or when the gadget asks. Frames go out at the clip's
 * rate; when the link falls behind they wait (encoded frames can't be
 * skipped) and catch up on the clock.
 */
final class ClipStreamer {
    private static let refreshLoops = 6
    private static let maxQueued: Int64 = 6000, catchUpMs: Int64 = 250

    private weak var sink: ClipSink?
    private let q = DispatchQueue(label: "clips", qos: .userInteractive)
    private var running = false
    private var gen = 0
    private(set) var clip: Clip?
    private var lastSent: Clip?
    private var pos = 0, loop = 0, frameNo = 0
    private var keyedLoop = true
    private var nextNs: Int64 = 0
    var statSent = 0, statLoops = 0, statKeys = 0
    var statHeldMs: Int64 = 0

    init(sink: ClipSink) {
        self.sink = sink
    }

    /* ---- preparing ------------------------------------------------------ */

    /* Decoded, scaled pictures by source and size: a clip re-encoded at another bitrate needs no second fetch. */
    private static var pictures: [String: [Data]] = [:]
    private static let picLock = NSLock()

    /*
     * Fetches (if not held), decodes, downscales and encodes one sample clip;
     * blocks for the excerpt's length or so (it plays from the network at
     * normal speed while its frames are taken), then a second or two.
     */
    static func encode(_ s: Sample, size: Int, kbps: Int, round: Bool, vp8: Bool) throws -> Clip {
        try encode(name: s.name, fps: s.fps, size: size, kbps: kbps, vp8: vp8, pictures: "\(s.name)|\(size)|\(round)") {
            try decodeScaled(url: s.url, size: size, round: round, startS: Double(s.startS), maxFrames: s.seconds * s.fps)
        }
    }

    /* Any clip: frames() gives its pictures (I420 at size x size), kept under the key given. */
    static func encode(name: String, fps: Int, size: Int, kbps: Int, vp8: Bool, pictures key: String,
                       frames make: () throws -> [Data]) throws -> Clip {
        let t0 = nowMs()
        picLock.lock()
        var frames = pictures[key]
        picLock.unlock()
        if frames == nil {
            frames = try make()
            picLock.lock()
            pictures = [key: frames!]   /* the latest only: ~15 MB at 232 px */
            picLock.unlock()
        }
        let t1 = nowMs()
        let c = Clip(name: name, size: size, fps: fps, kbps: kbps)
        c.vp8 = vp8
        if vp8 {
            try encodeLoopsVp8(frames!, c)
        } else if size > 240 && X264.available {
            try encodeLoopsX264(frames!, c)
        } else {
            try encodeLoopsVT(frames!, c)
        }
        c.measure()
        c.info = String(format: "%@: %d frames @%d fps (%.1f s), keyframe %d B, loop %d kb/s; decode+scale %lld ms, encode %lld ms (%@)",
                        name, c.steady.count, c.fps, Double(c.steady.count) / Double(c.fps), c.keyed.first?.count ?? 0,
                        c.steadyKbps(), t1 - t0, nowMs() - t1, vp8 ? "VP8" : "H.264")
        mlog("clip: " + c.info)
        return c
    }

    /*
     * Every frame of the excerpt as I420 at size x size, centre-cropped,
     * area-averaged and circle-masked. The movie plays (muted, unseen) from
     * its URL and each new picture is taken as it comes: AVAssetReader
     * reads only local files, and the excerpt is a few MB of a large movie.
     */
    private static func decodeScaled(url: String, size: Int, round: Bool, startS: Double, maxFrames: Int) throws -> [Data] {
        guard let u = URL(string: url) else {
            throw NSError(domain: "MuseCam", code: 10, userInfo: [NSLocalizedDescriptionKey: "bad URL"])
        }
        let item = AVPlayerItem(url: u)
        let out = AVPlayerItemVideoOutput(pixelBufferAttributes: [
            kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange,
        ])
        item.add(out)
        item.preferredForwardBufferDuration = 4
        let player = AVPlayer(playerItem: item)
        player.isMuted = true
        player.automaticallyWaitsToMinimizeStalling = true
        defer {
            player.pause()
            player.replaceCurrentItem(with: nil)
        }
        let deadline = nowMs() + 30000
        while item.status != .readyToPlay {
            if item.status == .failed || nowMs() > deadline {
                throw item.error ?? NSError(domain: "MuseCam", code: 11, userInfo: [NSLocalizedDescriptionKey: "couldn't open the clip"])
            }
            Thread.sleep(forTimeInterval: 0.02)
        }
        let start = CMTime(seconds: startS, preferredTimescale: 600)
        let sought = DispatchSemaphore(value: 0)
        player.seek(to: start, toleranceBefore: .zero, toleranceAfter: .zero) { _ in sought.signal() }
        if sought.wait(timeout: .now() + 20) == .timedOut {
            throw NSError(domain: "MuseCam", code: 12, userInfo: [NSLocalizedDescriptionKey: "seek timed out"])
        }
        player.play()
        var frames: [Data] = []
        var scaler: AreaScaler?
        var last = CMTime.invalid
        let end = startS + Double(maxFrames) / 24.0 + 1
        let limit = nowMs() + 120000
        while frames.count < maxFrames && nowMs() < limit {
            let t = out.itemTime(forHostTime: CACurrentMediaTime())
            if out.hasNewPixelBuffer(forItemTime: t) {
                var shown = CMTime.zero
                if let pb = out.copyPixelBuffer(forItemTime: t, itemTimeForDisplay: &shown), shown != last {
                    last = shown
                    if shown.seconds >= startS - 0.001 {
                        if scaler == nil {
                            scaler = AreaScaler(w: CVPixelBufferGetWidth(pb), h: CVPixelBufferGetHeight(pb), size: size, round: round)
                        }
                        frames.append(scaler!.scale(pb))
                    }
                    if shown.seconds > end {
                        break
                    }
                }
            } else if item.status == .failed {
                throw item.error ?? NSError(domain: "MuseCam", code: 13, userInfo: [NSLocalizedDescriptionKey: "playback failed"])
            } else {
                Thread.sleep(forTimeInterval: 0.004)
            }
        }
        if frames.isEmpty {
            throw NSError(domain: "MuseCam", code: 14, userInfo: [NSLocalizedDescriptionKey: "decoded no frames"])
        }
        return frames
    }

    /* Frames 0..N-1 twice through the phone's encoder: the keyed loop, then the steady loop. */
    private static func encodeLoopsVT(_ frames: [Data], _ c: Clip) throws {
        let n = frames.count
        var aus = [Data?](repeating: nil, count: 2 * n)
        let lock = NSLock()
        guard let e = VTH264(w: c.size, h: c.size, fps: c.fps, kbps: c.kbps, live: false, output: { au, _, idx in
            lock.lock()
            if idx >= 0 && Int(idx) < aus.count {
                aus[Int(idx)] = au
            }
            lock.unlock()
        }) else {
            throw NSError(domain: "MuseCam", code: 15, userInfo: [NSLocalizedDescriptionKey: "no H.264 encoder"])
        }
        for i in 0..<(2 * n) {
            frames[i % n].withUnsafeBytes { e.encode(i420: $0.bindMemory(to: UInt8.self).baseAddress!, index: Int64(i), fps: c.fps, key: i == 0) }
        }
        e.finish()
        e.close()
        let got = aus.compactMap { $0 }
        if got.count != 2 * n {
            throw NSError(domain: "MuseCam", code: 16, userInfo: [NSLocalizedDescriptionKey: "encoder gave \(got.count) frames for \(2 * n)"])
        }
        c.keyed = Array(got[0..<n])
        c.steady = Array(got[n...])
        if c.steady.contains(where: VTH264.isIdr) {
            throw NSError(domain: "MuseCam", code: 17, userInfo: [NSLocalizedDescriptionKey: "encoder put a keyframe in the steady loop"])
        }
    }

    /*
     * x264 for pictures past 240 px: 40 frames of lookahead and
     * macroblock-tree rate control, peaks held to the clip's rate plus 2%
     * over any 300 ms, no deblocking, decoder-friendly motion vectors.
     */
    private static func encodeLoopsX264(_ frames: [Data], _ c: Clip) throws {
        let n = frames.count
        let maxK = c.kbps * 102 / 100, buf = maxK * 300 / 1000
        guard let e = X264(w: c.size, h: c.size, fps: c.fps, kbps: c.kbps, preset: "fast",
                           params: CameraStreamer.withMvCheap("no-deblock=1:intra-refresh=0:keyint=infinite:rc-lookahead=40:mbtree=1:vbv-maxrate=\(maxK):vbv-bufsize=\(buf)", width: c.size)) else {
            throw NSError(domain: "MuseCam", code: 18, userInfo: [NSLocalizedDescriptionKey: "x264 open failed"])
        }
        defer { e.close() }
        var aus: [Data] = []
        for i in 0..<(2 * n) {
            if let r = frames[i % n].withUnsafeBytes({ e.encode($0.bindMemory(to: UInt8.self).baseAddress!, pts: Int64(i), key: i == 0) }) {
                aus.append(r.data)
            }
        }
        while aus.count < 2 * n, let r = e.encode(nil, pts: 0, key: false) {
            aus.append(r.data)
        }
        if aus.count != 2 * n {
            throw NSError(domain: "MuseCam", code: 19, userInfo: [NSLocalizedDescriptionKey: "x264 gave \(aus.count) frames for \(2 * n)"])
        }
        c.keyed = Array(aus[0..<n])
        c.steady = Array(aus[n...])
        if c.steady.contains(where: VTH264.isIdr) {
            throw NSError(domain: "MuseCam", code: 17, userInfo: [NSLocalizedDescriptionKey: "encoder put a keyframe in the steady loop"])
        }
    }

    /* VP8: the camera's settings for the size, but VBR inside a 300 ms buffer; no lag, keyframes only when asked. */
    private static func encodeLoopsVp8(_ frames: [Data], _ c: Clip) throws {
        let n = frames.count
        guard let e = Vpx(w: c.size, h: c.size, fps: c.fps, kbps: c.kbps,
                          params: CameraStreamer.vp8Params(nil, width: c.size) + ":vbr=1:vbv-ms=300") else {
            throw NSError(domain: "MuseCam", code: 20, userInfo: [NSLocalizedDescriptionKey: "libvpx open failed"])
        }
        defer { e.close() }
        for i in 0..<(2 * n) {
            guard let r = frames[i % n].withUnsafeBytes({ e.encode($0.bindMemory(to: UInt8.self).baseAddress!, pts: Int64(i), key: i == 0) }),
                  !r.data.isEmpty else {
                throw NSError(domain: "MuseCam", code: 21, userInfo: [NSLocalizedDescriptionKey: "libvpx gave no frame for picture \(i)"])
            }
            if i >= n && r.data[r.data.startIndex] & 1 == 0 {
                throw NSError(domain: "MuseCam", code: 17, userInfo: [NSLocalizedDescriptionKey: "encoder put a keyframe in the steady loop"])
            }
            if i < n {
                c.keyed.append(r.data)
            } else {
                c.steady.append(r.data)
            }
        }
    }

    /* ---- playback ------------------------------------------------------- */

    /* Plays the clip from its keyed loop; while running, switches over at the next frame. */
    func play(_ c: Clip) {
        q.async { [self] in
            clip = c
            pos = 0
            loop = 0
            keyedLoop = true
            if !running {
                running = true
                gen += 1
                nextNs = nowNs()
                let g = gen
                q.async { self.tick(g) }
            }
        }
    }

    func stop() {
        q.sync {
            running = false
            gen += 1
        }
    }

    /* Start the next keyed loop now (the gadget lost a frame). */
    func requestKeyframe() {
        q.async { [self] in
            pos = 0
            loop = 0
            keyedLoop = true
        }
    }

    private func tick(_ g: Int) {
        guard running, g == gen, let c = clip, let s = sink else { return }
        let period = Int64(1_000_000_000 / c.fps)
        if s.queuedBytes() > max(Self.maxQueued, Int64(c.maxFrame + 2000)) {
            statHeldMs += 5
            q.asyncAfter(deadline: .now() + .milliseconds(5)) { self.tick(g) }
            return
        }
        let au = (keyedLoop ? c.keyed : c.steady)[pos]
        let key = (keyedLoop || c.closed) && pos == 0
        if c !== lastSent {
            lastSent = c
            mlog("clip: now sending \(c.name) @\(c.kbps) kb/s")
        }
        s.onFrame(VideoFrame.make(au, number: frameNo, key: key), key: key)
        frameNo += 1
        statSent += 1
        if key {
            statKeys += 1
        }
        pos += 1
        if pos == c.steady.count {
            pos = 0
            statLoops += 1
            loop += 1
            keyedLoop = loop % Self.refreshLoops == 0
        }
        nextNs += period
        var wait = (nextNs - nowNs()) / 1_000_000
        if wait < -Self.catchUpMs {
            nextNs = nowNs()
            wait = 0
        }
        q.asyncAfter(deadline: .now() + .milliseconds(Int(max(0, wait)))) { self.tick(g) }
    }
}

/*
 * Area-average downscaler for 4:2:0 biplanar pictures: each source pixel of
 * the centred square adds to the one destination pixel it falls in, then
 * each is divided by its count (keeps fur and edges crisp without aliasing).
 * Black only in the 4x4 blocks wholly outside the circle plus the margin.
 */
final class AreaScaler {
    let size: Int, side: Int, x0: Int, y0: Int
    private let xmapY: [Int], xmapC: [Int], cntY: [Int], cntC: [Int]
    private var accY: [Int], accU: [Int], accV: [Int]
    private var inY: [Bool], inC: [Bool]

    init(w: Int, h: Int, size: Int, round: Bool) {
        self.size = size & ~1
        side = min(w, h) & ~1
        x0 = ((w - side) / 2) & ~1
        y0 = ((h - side) / 2) & ~1
        let s = self.size, cs = s / 2
        func map(_ src: Int, _ dst: Int) -> [Int] { (0..<src).map { $0 * dst / src } }
        func counts(_ m: [Int], _ dst: Int) -> [Int] {
            var c = [Int](repeating: 0, count: dst)
            for v in m { c[v] += 1 }
            return c
        }
        xmapY = map(side, s)
        xmapC = map(side / 2, cs)
        cntY = counts(xmapY, s)
        cntC = counts(xmapC, cs)
        accY = [Int](repeating: 0, count: s * s)
        accU = [Int](repeating: 0, count: cs * cs)
        accV = [Int](repeating: 0, count: cs * cs)
        let c = Double(s) / 2, r2 = (c + CameraStreamer.maskMargin) * (c + CameraStreamer.maskMargin)
        let bw = (s + 3) / 4
        var blkIn = [Bool](repeating: false, count: bw * bw)
        for y in 0..<s {
            for x in 0..<s {
                let dx = Double(x) + 0.5 - c, dy = Double(y) + 0.5 - c
                if dx * dx + dy * dy <= r2 {
                    blkIn[(y >> 2) * bw + (x >> 2)] = true
                }
            }
        }
        inY = (0..<(s * s)).map { !round || blkIn[(($0 / s) >> 2) * bw + (($0 % s) >> 2)] }
        inC = (0..<(cs * cs)).map { !round || blkIn[(($0 / cs) >> 1) * bw + (($0 % cs) >> 1)] }
    }

    func scale(_ pb: CVPixelBuffer) -> Data {
        CVPixelBufferLockBaseAddress(pb, .readOnly)
        defer { CVPixelBufferUnlockBaseAddress(pb, .readOnly) }
        let s = size, cs = s / 2
        let yb = CVPixelBufferGetBaseAddressOfPlane(pb, 0)!.assumingMemoryBound(to: UInt8.self)
        let cb = CVPixelBufferGetBaseAddressOfPlane(pb, 1)!.assumingMemoryBound(to: UInt8.self)
        let ys = CVPixelBufferGetBytesPerRowOfPlane(pb, 0), crs = CVPixelBufferGetBytesPerRowOfPlane(pb, 1)
        for i in accY.indices { accY[i] = 0 }
        for i in accU.indices { accU[i] = 0; accV[i] = 0 }
        accY.withUnsafeMutableBufferPointer { a in
            for y in 0..<side {
                let row = yb + (y0 + y) * ys + x0, base = xmapY[y] * s
                for x in 0..<side {
                    a[base + xmapY[x]] += Int(row[x])
                }
            }
        }
        accU.withUnsafeMutableBufferPointer { au in
            accV.withUnsafeMutableBufferPointer { av in
                for y in 0..<(side / 2) {
                    let row = cb + (y0 / 2 + y) * crs + x0, base = xmapC[y] * cs
                    for x in 0..<(side / 2) {
                        au[base + xmapC[x]] += Int(row[2 * x])
                        av[base + xmapC[x]] += Int(row[2 * x + 1])
                    }
                }
            }
        }
        var out = Data(count: s * s * 3 / 2)
        out.withUnsafeMutableBytes { raw in
            let o = raw.bindMemory(to: UInt8.self).baseAddress!
            for y in 0..<s {
                for x in 0..<s {
                    let i = y * s + x, n = cntY[x] * cntY[y]
                    o[i] = inY[i] ? UInt8((accY[i] + n / 2) / n) : 16
                }
            }
            let uo = s * s, vo = uo + cs * cs
            for y in 0..<cs {
                for x in 0..<cs {
                    let i = y * cs + x, n = cntC[x] * cntC[y]
                    o[uo + i] = inC[i] ? UInt8((accU[i] + n / 2) / n) : 128
                    o[vo + i] = inC[i] ? UInt8((accV[i] + n / 2) / n) : 128
                }
            }
        }
        return out
    }
}
