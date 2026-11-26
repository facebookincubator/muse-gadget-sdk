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

import Compression
import CoreVideo
import Foundation

/* Where a UI stream goes: a gadget link (the app's, or the broadcast extension's own). */
protocol UiOutput: AnyObject {
    func uiStart(w: Int, h: Int, fps: Int, bands: Int)
    func uiStop()
    func uiSend(_ frame: Data)
    /* Frame bytes not yet written to the link: a new frame waits until it's 0. */
    func uiQueued() -> Int64
}

/*
 * The UI stream (protocols/UI.md), shared by the web page and the screen
 * mirror: pictures in (any size, any rate: a source may send the same picture
 * again), the square-to-disc warp and the panel's size on the GPU (UiWarp),
 * then only what changed goes out:
 * - each new picture is compared with the last one sent, 16x16 block by
 *   block; nothing changed, nothing sent (a still screen costs 0 kb/s);
 * - a change goes out at once at a coarse quantizer (QP 30; the first frame
 *   and a requested one as an IDR), at most 15 a second and only once the
 *   link has written the frame before (the gadget's credits pace it);
 * - after 300 ms and 1 s without change the same picture again as P frames
 *   at QP 24 and 18 (progressive refinement), each coding only the
 *   difference to what the gadget shows;
 * - a frame over the gadget's max frame bytes is dropped and the picture
 *   sent again as an IDR 4 QP coarser (a P frame can't be dropped: the
 *   encoder would reference a picture the gadget never got).
 * Two bands where the gadget decodes them in parallel (each its own encoder).
 *
 * Quality (the gadget's menu, mg_ui_control_quality): fast stops at QP 18;
 * sharp ends with lossless still patches where the gadget takes them
 * (mg_stream_video_capability_ui_still, RGB565 zlib; one band, x264): once
 * a picture has held still for stillMs after its first refinement, the
 * macroblocks not exactly the phone's RGB565 on the gadget go out as zlib
 * RGB565 rectangles in place of the QP 18 refinement. The phone knows what
 * the gadget's decoder has (x264's reconstruction): a macroblock is dirty
 * when that changed there (the gadget redraws it from the decoded picture)
 * or when the source changed since its last patch. Patches over
 * stillMaxBytes: the QP 18 refinement instead. Without the capability,
 * sharp adds a QP 12 refinement at 2 s.
 *
 * overlay draws over the warped picture (the gadget's menu), in BGRA at the
 * panel's size; rerender() renders the last source picture again (the menu
 * changed, the warp toggled).
 */
final class UiStreamer {
    struct Config {
        var n = 360                  /* picture size (square) */
        var bands = 1
        var band0Rows = 0            /* two bands: the top band's rows (a multiple of 16) */
        var maxFrame = 60000         /* bytes, header excluded */
        var maxFps = 15
        /* A change goes out at the finest of these whose frame (estimated from
         * the changed blocks) fits budgetMs of the link at linkKbps. */
        var changeQps = [20, 24, 28, 32]
        var linkKbps = 600
        var budgetMs = 300
        /* Then the same picture again at these, after this long without change
         * (a step no finer than what's on the gadget is skipped). */
        var refine: [(ms: Int, qp: Int)] = [(300, 24), (1000, 18)]
        var x264 = true
    }

    /* The stream a gadget takes (its ui capability; else video at its largest
     * picture, for gadgets without UI streams). */
    static func config(for c: Caps, twoBands: Bool = false) -> Config {
        var cfg = Config()
        let ui = c.uiW > 0 && c.uiH > 0
        cfg.n = max(Caps.minPanel, min(720, ui ? min(c.uiW, c.uiH) : min(c.maxW, c.maxH))) & ~1   /* the UI cap */
        cfg.maxFrame = min(ui && c.uiMaxFrame > 0 ? c.uiMaxFrame : 30000, c.maxFrame)
        cfg.maxFps = min(15, ui ? max(1, c.uiFps) : c.maxFps)
        cfg.linkKbps = c.sugKbps >= 300 ? c.sugKbps : 600
        if twoBands && ui && c.uiBands >= 2 && cfg.n >= 64 {   /* each band at least 16 rows */
            cfg.bands = 2
            let split = c.tileSplitPct > 0 ? c.tileSplitPct : 50
            cfg.band0Rows = max(16, min(cfg.n - 16, (cfg.n * split / 100 + 8) / 16 * 16))
        }
        return cfg
    }

    struct Stats {
        var frames = 0, changes = 0, refines = 0, keys = 0, oversize = 0, unchanged = 0
        var bytes: Int64 = 0
        var lastKind = "", lastQp = 0, lastBytes = 0, lastPsnr = 0.0, lastEncMs = 0.0, lastBlocks = 0
        var stills = 0, stillBytes: Int64 = 0, lastStillBytes = 0, lastStillPx = 0, lastStillMs = 0.0, stillTooBig = 0
        /* bytes and PSNR of the latest change and its refinements, by step (0: the change) */
        var stepBytes = [0, 0, 0], stepPsnr = [0.0, 0.0, 0.0]
        var encoder = ""
    }

    let cfg: Config
    /* Sharp quality (see above), and whether the gadget takes still patches. */
    var sharp = true { didSet { ladderChanged() } }
    var stillPatches = false { didSet { ladderChanged() } }
    var stillMs = 600, stillMaxBytes = 120 * 1024
    /* Over the warped picture, in place (BGRA n x n), when set; true: it drew. */
    var overlay: ((UnsafeMutablePointer<UInt8>, Int) -> Bool)?
    var overlayActive: (() -> Bool)?
    var warp = true
    var mask = true
    weak var output: UiOutput?
    var meter: LatencyMeter?
    /* The latest picture as the gadget shows it (BGRA, n x n), for the phone's preview. */
    var onPreview: ((Data, Int) -> Void)?
    var previewEveryMs: Int64 = 200
    var log = true

    private let gpu: UiWarp
    private let lock = NSCondition()
    private var latest: [UInt8], latestNs: Int64 = 0, latestSeq = 0, seenSeq = 0
    private var sent: [UInt8]
    private var hasSent = false
    private var keyWanted = true
    private var running = false
    private var codecs: [UiCodec] = []
    private var bandBuf: [[UInt8]] = []
    private var frameNo = 0
    private var stage = 0, changeAtNs: Int64 = 0, lastChangeSendNs: Int64 = 0
    /* Pacing (protocols/UI.md, Pacing): a change frame starts at most every
     * 1000/maxFps ms counted from the previous one's start, every 1000/fastFps
     * ms while the last change was small (smallBytes or less); it may be
     * encoded while the previous one still goes out (pipelineBytes or less of
     * it left), and starts only while at most aheadFrames frames sent aren't
     * shown yet (the gadget's echo, through the meter). legacyPacing: the old
     * rule (after the previous encode ended, the link idle), for comparisons. */
    var fastFps = 30
    var smallBytes = 2048, pipelineBytes = 2048, aheadFrames = 1
    var legacyPacing = false
    private var lastChangeStartNs: Int64 = 0
    private var lastChangeBytes = 0
    private var stats = Stats()
    private var lastPreviewMs: Int64 = 0
    private var previewBuf: [UInt8]
    private var thread: Thread?
    /* Bytes per changed 16x16 block at QP 32 (P frames, and IDRs apart),
     * learned from the frames sent; other QPs scale by 2^((32 - qp) / 6)
     * (H.264's step size doubles every 6 QP). Picks the change QP. */
    private var bpb32 = 12.0, bpb32Idr = 8.0
    /* Still patches: the RGB565 of the latest picture and of the one sent, the
     * gadget's decoded picture now and before, the macroblocks not exact there. */
    private var latest565: [UInt8]?, sent565: [UInt8]?
    private var recon: [UInt8] = [], reconPrev: [UInt8] = []
    private var haveReconPrev = false
    private var dirty: [Bool] = []
    private var stillTooBig = false
    /* The last source picture, for rerender(). */
    private var lastPB: CVPixelBuffer?, lastCrop = SIMD4<Float>(0, 0, 1, 1)
    private var bgraBuf: [UInt8]

    private var usePatches: Bool { stillPatches && sharp && cfg.bands == 1 && codecs.first is UiX264 }

    /* The refinements after a change, for the quality (under lock). */
    private var ladder: [(ms: Int, qp: Int)] {
        if usePatches && !stillTooBig {
            return [(cfg.refine.first?.ms ?? 300, cfg.refine.first?.qp ?? 24)]   /* then the patch at stillMs */
        }
        return sharp && !usePatches ? cfg.refine + [(2000, 12)] : cfg.refine
    }

    /* The quality changed: refine the still picture to the new last step now. */
    private func ladderChanged() {
        lock.lock()
        if hasSent {
            stage = 0
            while stage < ladder.count && ladder[stage].qp >= sentQp {
                stage += 1
            }
            changeAtNs = nowNs() - 60_000_000_000
        }
        lock.broadcast()
        lock.unlock()
    }
    private var sentQp = 51

    private static func scale(_ qp: Int) -> Double { pow(2, Double(32 - qp) / 6) }

    /* The finest change QP whose frame fits the link budget and the gadget's max frame. */
    private func changeQp(blocks: Int, idr: Bool) -> Int {
        let budget = min(Double(cfg.linkKbps) * 1000 / 8 * Double(cfg.budgetMs) / 1000, Double(cfg.maxFrame) * 0.85)
        let b = Double(idr ? (cfg.n + 15) / 16 * ((cfg.n + 15) / 16) : max(1, blocks))
        for qp in cfg.changeQps.sorted() where b * (idr ? bpb32Idr : bpb32) * Self.scale(qp) <= budget {
            return qp
        }
        return cfg.changeQps.max() ?? 32
    }

    private func learn(qp: Int, blocks: Int, bytes: Int, idr: Bool) {
        let b = Double(idr ? (cfg.n + 15) / 16 * ((cfg.n + 15) / 16) : blocks)
        guard b >= 4 else { return }   /* tiny changes say little */
        let v = Double(bytes) / b / Self.scale(qp)
        if idr {
            bpb32Idr = bpb32Idr * 0.6 + v * 0.4
        } else {
            bpb32 = bpb32 * 0.6 + v * 0.4
        }
    }

    init?(_ cfg: Config, gpu: UiWarp? = nil) {
        /* Buffers are sized from n: only a size within the protocol's UI cap. */
        guard cfg.n >= Caps.minPanel && cfg.n <= 720, let g = gpu ?? UiWarp() else { return nil }
        self.gpu = g
        self.cfg = cfg
        let size = cfg.n * cfg.n * 3 / 2
        latest = [UInt8](repeating: 0, count: size)
        sent = [UInt8](repeating: 0, count: size)
        previewBuf = [UInt8](repeating: 0, count: cfg.n * cfg.n * 4)
        bgraBuf = [UInt8](repeating: 0, count: cfg.n * cfg.n * 4)
    }

    private func bandRows() -> [Int] {
        if cfg.bands < 2 {
            return [cfg.n]
        }
        let b0 = cfg.band0Rows > 0 ? cfg.band0Rows : (cfg.n / 2 + 8) / 16 * 16
        return [b0, cfg.n - b0]
    }

    func start() {
        lock.lock()
        defer { lock.unlock() }
        if running {
            return
        }
        codecs = []
        for rows in bandRows() {
            let c: UiCodec? = (cfg.x264 && X264.available ? UiX264(w: cfg.n, h: rows, fps: cfg.maxFps) : nil)
                ?? UiVideoToolbox(w: cfg.n, h: rows, fps: cfg.maxFps)
            guard let codec = c else {
                mlog("ui: no encoder")
                return
            }
            codecs.append(codec)
            bandBuf.append([UInt8](repeating: 0, count: cfg.n * rows * 3 / 2))
        }
        stats = Stats()
        stats.encoder = codecs.first?.name ?? ""
        running = true
        hasSent = false
        keyWanted = true
        frameNo = 0
        stage = 99
        seenSeq = latestSeq > 0 ? latestSeq - 1 : 0   /* the picture already there goes out first; none yet: wait for one */
        haveReconPrev = false
        dirty = [Bool](repeating: false, count: ((cfg.n + 15) / 16) * ((cfg.n + 15) / 16))
        recon = [UInt8](repeating: 0, count: cfg.n * cfg.n * 3 / 2)
        reconPrev = recon
        output?.uiStart(w: cfg.n, h: cfg.n, fps: cfg.maxFps, bands: codecs.count)
        mlog("ui: streaming \(cfg.n)x\(cfg.n), \(codecs.count) band(s) \(bandRows()), \(stats.encoder), max frame \(cfg.maxFrame) B, warp \(warp)")
        let t = Thread { [weak self] in self?.loop() }
        t.name = "mg-ui"
        t.qualityOfService = .userInteractive
        thread = t
        t.start()
    }

    func stop() {
        lock.lock()
        let was = running
        running = false
        lock.broadcast()
        lock.unlock()
        if was {
            output?.uiStop()
        }
    }

    /* A frame to the gadget unless stop() came first: stop() takes the lock, so once it returns nothing more of
     * this stream goes out (a replacement stream may already be starting on the same link). */
    private func send(_ frame: Data) -> Bool {
        lock.lock()
        defer { lock.unlock() }
        guard running else { return false }
        output?.uiSend(frame)
        return true
    }

    var isRunning: Bool {
        lock.lock()
        defer { lock.unlock() }
        return running
    }

    func requestKeyframe() {
        lock.lock()
        keyWanted = true
        lock.broadcast()
        lock.unlock()
    }

    func snapshot() -> Stats {
        lock.lock()
        defer { lock.unlock() }
        return stats
    }

    /*
     * A source picture: the square crop (x, y, w, h in 0..1 of pb) to the
     * panel's picture on the GPU. Any thread; returns false if the GPU failed.
     * The newest picture wins: one that arrives while a frame is being sent
     * replaces the one waiting.
     */
    private let submitLock = NSLock()

    @discardableResult
    func submit(_ pb: CVPixelBuffer, crop: SIMD4<Float>, captureNs: Int64 = nowNs()) -> Bool {
        submitLock.lock()
        defer { submitLock.unlock() }
        lastPB = pb
        lastCrop = crop
        let n = cfg.n
        var pic = [UInt8](repeating: 0, count: n * n * 3 / 2)
        let ov = overlay, patches = stillPatches && sharp && cfg.bands == 1
        let wantPreview = onPreview != nil && nowMs() - lastPreviewMs >= previewEveryMs
        let needBgra = wantPreview || (ov != nil && overlayActive?() == true) || patches
        let ok = pic.withUnsafeMutableBufferPointer { p in
            bgraBuf.withUnsafeMutableBufferPointer { pv in
                gpu.render(pb, crop: crop, n: n, warp: warp, mask: mask, i420: p.baseAddress!,
                           preview: needBgra ? pv.baseAddress! : nil)
            }
        }
        guard ok else { return false }
        if let ov {
            bgraBuf.withUnsafeMutableBufferPointer { b in
                if ov(b.baseAddress!, n) {
                    pic.withUnsafeMutableBufferPointer { Self.bgraToI420(b.baseAddress!, n, $0.baseAddress!) }
                }
            }
        }
        var p565: [UInt8]?
        if patches {
            p565 = bgraBuf.withUnsafeBufferPointer { Self.bgraToRgb565($0.baseAddress!, n) }
        }
        if wantPreview {
            lastPreviewMs = nowMs()
            onPreview?(Data(bgraBuf), n)
        }
        lock.lock()
        latest = pic
        latest565 = p565
        latestNs = captureNs
        latestSeq += 1
        lock.broadcast()
        lock.unlock()
        return true
    }

    /* Per 16x16 block: does it differ between a and b? */
    private func changedMask(_ a: [UInt8], _ b: [UInt8]) -> [Bool] {
        let bw = (cfg.n + 15) / 16
        var m = [Bool](repeating: false, count: bw * bw)
        for by in 0..<bw {
            for bx in 0..<bw {
                m[by * bw + bx] = Self.blockDiffers(a, b, cfg.n, bx, by)
            }
        }
        return m
    }

    /* src where need, else ref, block by block (I420 n x n). */
    private static func compose(_ src: [UInt8], _ ref: [UInt8], _ need: [Bool], _ n: Int) -> [UInt8] {
        var out = ref
        let bw = (n + 15) / 16, h = n / 2
        src.withUnsafeBufferPointer { s in
            out.withUnsafeMutableBufferPointer { o in
                let S = s.baseAddress!, O = o.baseAddress!
                for by in 0..<bw {
                    for bx in 0..<bw where need[by * bw + bx] {
                        let x0 = bx * 16, w = min(16, n - x0)
                        for y in (by * 16)..<min(n, by * 16 + 16) {
                            memcpy(O + y * n + x0, S + y * n + x0, w)
                        }
                        let cx = bx * 8, cw = min(8, h - cx)
                        for plane in 0..<2 {
                            let off = n * n + plane * h * h
                            for y in (by * 8)..<min(h, by * 8 + 8) {
                                memcpy(O + off + y * h + cx, S + off + y * h + cx, cw)
                            }
                        }
                    }
                }
            }
        }
        return out
    }

    /* 16x16 blocks (8x8 in chroma) that differ between a and b. */
    private func changedBlocks(_ a: [UInt8], _ b: [UInt8]) -> Int {
        let n = cfg.n, mbs = (n + 15) / 16
        var count = 0
        a.withUnsafeBufferPointer { pa in
            b.withUnsafeBufferPointer { pb in
                let A = pa.baseAddress!, B = pb.baseAddress!
                let h = n / 2
                for my in 0..<mbs {
                    for mx in 0..<mbs {
                        let x0 = mx * 16, w = min(16, n - x0)
                        var diff = false
                        for y in (my * 16)..<min(n, my * 16 + 16) where memcmp(A + y * n + x0, B + y * n + x0, w) != 0 {
                            diff = true
                            break
                        }
                        if !diff {
                            let cx = mx * 8, cw = min(8, h - cx)
                            for plane in 0..<2 where !diff {
                                let off = n * n + plane * h * h
                                for y in (my * 8)..<min(h, my * 8 + 8) where memcmp(A + off + y * h + cx, B + off + y * h + cx, cw) != 0 {
                                    diff = true
                                    break
                                }
                            }
                        }
                        if diff {
                            count += 1
                        }
                    }
                }
            }
        }
        return count
    }

    /* The least time between change frames' starts: faster while changes are small. */
    private func changeGapNs() -> Int64 {
        var fps = max(1, min(cfg.maxFps, 15))
        if fastFps > fps && lastChangeBytes > 0 && lastChangeBytes <= smallBytes {
            fps = fastFps
        }
        return Int64(1_000_000_000 / fps)
    }

    /* At most aheadFrames frames sent and not yet shown (the gadget's echo);
     * without an echo for 300 ms (an older gadget, a lost update) nothing is held back. */
    private func gadgetKeepsUp(_ now: Int64) -> Bool {
        guard let m = meter, frameNo > 0 else {
            return true
        }
        let (shown, atNs) = m.lastShownFrame()
        if shown < 0 || now - atNs > 300_000_000 {
            return true
        }
        let ahead = ((frameNo - 1) - shown) & 0xffff
        return ahead <= aheadFrames || ahead > 0x8000
    }

    private func loop() {
        let minGapNs = Int64(1_000_000_000 / max(1, min(cfg.maxFps, 15)))
        while true {
            lock.lock()
            if !running {
                lock.unlock()
                break
            }
            let now = nowNs()
            let busy = (output?.uiQueued() ?? 0) > 0
            let fresh = latestSeq != seenSeq
            let lad = ladder
            let refineDue = hasSent && stage < lad.count && now >= changeAtNs + Int64(lad[stage].ms) * 1_000_000
            let stillDue = !busy && !fresh && !keyWanted && hasSent && usePatches && !stillTooBig && stage >= lad.count
                && sent565 != nil && now >= changeAtNs + Int64(stillMs) * 1_000_000 && dirty.contains(true)
            var job: (pic: [UInt8], qp: Int, kind: String, capNs: Int64, blocks: Int)?
            var pic565: [UInt8]?
            if stillDue, let s565 = sent565 {
                let mask = dirty
                for i in dirty.indices {
                    dirty[i] = false
                }
                lock.unlock()
                sendStill(s565, mask)
                continue
            }
            let queued = output?.uiQueued() ?? 0
            let changeBusy = legacyPacing ? busy : (queued > Int64(pipelineBytes) || !gadgetKeepsUp(now))
            let gapNs = legacyPacing ? minGapNs : changeGapNs()
            let since = legacyPacing ? lastChangeSendNs : lastChangeStartNs
            if !changeBusy && fresh && now - since >= gapNs {
                lastChangeStartNs = now
                seenSeq = latestSeq
                let pic = latest, capNs = latestNs
                pic565 = latest565
                lock.unlock()
                let blocks = hasSent ? changedBlocks(pic, sent) : cfg.n * cfg.n / 256
                lock.lock()
                if blocks > 0 || keyWanted {
                    job = (pic, changeQp(blocks: blocks, idr: keyWanted || !hasSent), "change", capNs, blocks)
                } else {
                    stats.unchanged += 1
                }
            } else if !busy && keyWanted && hasSent {
                job = (sent, changeQp(blocks: 0, idr: true), "change", now, 0)
                pic565 = sent565
            } else if !busy && refineDue {
                job = (sent, lad[stage].qp, "refine\(stage + 1)", 0, 0)
            }
            guard let j = job else {
                /* sleep until a picture, a request, the next refinement, or the link frees up */
                var wake = Date().addingTimeInterval(busy ? 0.005 : 0.5)
                if hasSent && stage < lad.count && !busy {
                    let ns = changeAtNs + Int64(lad[stage].ms) * 1_000_000 - nowNs()
                    wake = min(wake, Date().addingTimeInterval(Double(max(0, ns)) / 1e9))
                } else if hasSent && usePatches && !busy && dirty.contains(true) {
                    let ns = changeAtNs + Int64(stillMs) * 1_000_000 - nowNs()
                    wake = min(wake, Date().addingTimeInterval(Double(max(1_000_000, ns)) / 1e9))
                }
                if fresh && changeBusy {
                    wake = min(wake, Date().addingTimeInterval(0.005))
                } else if fresh {
                    let ns = since + gapNs - nowNs()
                    wake = min(wake, Date().addingTimeInterval(Double(max(1_000_000, ns)) / 1e9))
                }
                _ = lock.wait(until: wake)
                lock.unlock()
                continue
            }
            var idr = keyWanted || !hasSent
            keyWanted = false
            /* With patches, the macroblocks already exact on the gadget are given
             * to the encoder as it already has them (its reference): they're
             * skipped, the decoded picture doesn't change there, and their patches
             * stay. Only the changed and the not yet exact ones are coded. */
            var input = j.pic
            if usePatches && haveReconPrev && !idr {
                var need = dirty
                if j.kind == "change" {
                    let ch = changedMask(j.pic, sent)
                    for i in need.indices where ch[i] {
                        need[i] = true
                    }
                }
                input = Self.compose(j.pic, reconPrev, need, cfg.n)
            }
            lock.unlock()
            var qp = j.qp
            var result = encodeAll(input, qp: qp, idr: idr)
            while let r = result, r.payload.count > cfg.maxFrame && qp < 51 {
                /* Too big for the gadget: drop it and start over as an IDR, coarser. */
                mlog("ui: \(j.kind) frame \(r.payload.count) B > max \(cfg.maxFrame) at QP \(qp); IDR at QP \(min(51, qp + 4))")
                lock.lock()
                stats.oversize += 1
                lock.unlock()
                qp = min(51, qp + 4)
                idr = true
                result = encodeAll(j.pic, qp: qp, idr: true)
                input = j.pic
            }
            guard let r = result else {
                mlog("ui: the encoder produced nothing")
                lock.lock()
                keyWanted = true
                lock.unlock()
                continue
            }
            if r.payload.count > cfg.maxFrame {
                /* Still too big at QP 51: the gadget can't take it. Dropped; the encoder's references moved on with
                 * it, so the next frame is an IDR, tried no sooner than 100 ms from now. */
                mlog("ui: \(j.kind) frame \(r.payload.count) B > max \(cfg.maxFrame) at QP \(qp): dropped")
                lock.lock()
                stats.oversize += 1
                keyWanted = true
                _ = lock.wait(until: Date().addingTimeInterval(0.1))
                lock.unlock()
                continue
            }
            let num = frameNo & 0xffff
            frameNo += 1
            let encNs = nowNs()
            if j.kind == "change" {
                meter?.captured(num, captureNs: j.capNs, encodedNs: encNs)
            }
            guard send(VideoFrame.make(r.payload, number: num, key: idr)) else { break }
            lock.lock()
            trackDirty(key: idr, pic: j.kind == "change" ? j.pic : nil, prev: j.kind == "change" && hasSent ? sent : nil)
            if j.kind == "change" {
                if qp == j.qp {
                    learn(qp: qp, blocks: j.blocks, bytes: r.payload.count, idr: idr)
                }
                sent = j.pic
                sent565 = pic565
                stillTooBig = false
                hasSent = true
                sentQp = qp
                /* refinements finer than this change only */
                stage = 0
                let lad2 = ladder
                while stage < lad2.count && lad2[stage].qp >= qp {
                    stage += 1
                }
                changeAtNs = encNs
                lastChangeSendNs = encNs
                lastChangeBytes = r.payload.count
                stats.changes += 1
                stats.stepBytes = [r.payload.count, 0, 0]
                stats.stepPsnr = [r.psnr, 0, 0]
            } else {
                let s = stage + 1
                if s < 3 {
                    stats.stepBytes[s] = r.payload.count
                    stats.stepPsnr[s] = r.psnr
                }
                stage += 1
                sentQp = qp
                stats.refines += 1
            }
            stats.frames += 1
            if idr {
                stats.keys += 1
            }
            stats.bytes += Int64(r.payload.count)
            stats.lastKind = j.kind
            stats.lastQp = qp
            stats.lastBytes = r.payload.count
            stats.lastPsnr = r.psnr
            stats.lastEncMs = r.ms
            stats.lastBlocks = j.blocks
            lock.unlock()
            if log {
                mlog(String(format: "ui frame %d %@%@ qp %d %d B psnr %.2f dB enc %.1f ms blocks %d", num, j.kind, idr ? " IDR" : "",
                            qp, r.payload.count, r.psnr, r.ms, j.blocks))
            }
        }
        for c in codecs {
            c.close()
        }
    }

    /*
     * After an encode (under lock): the macroblocks whose decoded picture
     * changed (x264's reconstruction) and those whose source changed (pic vs
     * prev) are no longer exactly the phone's picture on the gadget.
     */
    private func trackDirty(key: Bool, pic: [UInt8]?, prev: [UInt8]?) {
        guard usePatches, let codec = codecs.first else { return }
        let n = cfg.n, bw = (n + 15) / 16
        let got = recon.withUnsafeMutableBufferPointer { codec.recon($0.baseAddress!) }
        for by in 0..<bw {
            for bx in 0..<bw where !dirty[by * bw + bx] {
                if key || !got || !haveReconPrev || Self.blockDiffers(recon, reconPrev, n, bx, by)
                    || (pic != nil && prev != nil && Self.blockDiffers(pic!, prev!, n, bx, by)) {
                    dirty[by * bw + bx] = true
                }
            }
        }
        if got {
            swap(&recon, &reconPrev)
            haveReconPrev = true
        }
    }

    private static func blockDiffers(_ a: [UInt8], _ b: [UInt8], _ n: Int, _ bx: Int, _ by: Int) -> Bool {
        a.withUnsafeBufferPointer { pa in
            b.withUnsafeBufferPointer { pb in
                let A = pa.baseAddress!, B = pb.baseAddress!
                let x0 = bx * 16, w = min(16, n - x0)
                for y in (by * 16)..<min(n, by * 16 + 16) where memcmp(A + y * n + x0, B + y * n + x0, w) != 0 {
                    return true
                }
                let h = n / 2, cx = bx * 8, cw = min(8, h - cx)
                for plane in 0..<2 {
                    let off = n * n + plane * h * h
                    for y in (by * 8)..<min(h, by * 8 + 8) where memcmp(A + off + y * h + cx, B + off + y * h + cx, cw) != 0 {
                        return true
                    }
                }
                return false
            }
        }
    }

    /* The dirty macroblocks as still patches: a rectangle per run of rows (merged as the gadget merges its redraws). */
    private func sendStill(_ rgb565: [UInt8], _ mask: [Bool]) {
        let t0 = nowNs()
        let n = cfg.n, bw = (n + 15) / 16
        var rects: [[Int]] = []
        var cur: Int? = nil
        for by in 0..<bw {
            var lo = bw, hi = -1
            for bx in 0..<bw where mask[by * bw + bx] {
                lo = min(lo, bx)
                hi = bx
            }
            if hi < 0 {
                cur = nil
                continue
            }
            let x0 = lo * 16, x1 = min(n, (hi + 1) * 16) & ~1, y0 = by * 16, y1 = min(n, y0 + 16) & ~1
            if let c = cur {
                let r = rects[c]
                let ux0 = min(r[0], x0), ux1 = max(r[2], x1)
                let union = (ux1 - ux0) * (y1 - r[1])
                let sep = (r[2] - r[0]) * (r[3] - r[1]) + (x1 - x0) * (y1 - y0)
                if union <= sep + sep / 4 {
                    rects[c] = [ux0, r[1], ux1, y1]
                    continue
                }
            }
            rects.append([x0, y0, x1, y1])
            cur = rects.count - 1
        }
        if rects.count > 6 {
            var b = [n, n, 0, 0]
            for r in rects {
                b = [min(b[0], r[0]), min(b[1], r[1]), max(b[2], r[2]), max(b[3], r[3])]
            }
            rects = [b]
        }
        var frames: [Data] = []
        var total = 0, px = 0
        for r in rects {
            let rw = r[2] - r[0], rh = r[3] - r[1]
            if rw <= 0 || rh <= 0 {
                continue
            }
            var raw = [UInt8](repeating: 0, count: rw * rh * 2)
            rgb565.withUnsafeBufferPointer { s in
                raw.withUnsafeMutableBufferPointer { d in
                    for y in 0..<rh {
                        memcpy(d.baseAddress! + y * rw * 2, s.baseAddress! + ((r[1] + y) * n + r[0]) * 2, rw * 2)
                    }
                }
            }
            guard let z = Zlib.compress(raw) else { continue }
            var payload = Data(capacity: 9 + z.count)
            for v in [r[0], r[1], rw, rh] {
                payload.append(contentsOf: [UInt8(v & 0xff), UInt8(v >> 8)])
            }
            payload.append(1)   /* mg_video_still_rgb565_zlib */
            payload.append(z)
            frames.append(payload)
            total += payload.count + VideoFrame.headerLen
            px += rw * rh
        }
        let ms = Double(nowNs() - t0) / 1e6
        let fits = total <= stillMaxBytes && frames.allSatisfy { $0.count <= cfg.maxFrame }
        lock.lock()
        if !fits {
            /* Too big (a photo): the H.264 refinement instead, and nothing more of this picture patched.
             * Those macroblocks are still not exact on the gadget: dirty again, so the encoder codes them. */
            stillTooBig = true
            stats.stillTooBig += 1
            if dirty.count == mask.count {
                for i in mask.indices where mask[i] {
                    dirty[i] = true
                }
            }
            lock.unlock()
            mlog(String(format: "ui: still patch %d B for %d px is too big: refining instead", total, px))
            return
        }
        var nums: [Int] = []
        for _ in frames {
            nums.append(frameNo & 0xffff)
            frameNo += 1
        }
        stats.stills += 1
        stats.stillBytes += Int64(total)
        stats.bytes += Int64(total)
        stats.lastStillBytes = total
        stats.lastStillPx = px
        stats.lastStillMs = ms
        stats.frames += frames.count
        lock.unlock()
        for (k, p) in frames.enumerated() {
            guard send(Self.stillFrame(p, number: nums[k])) else { return }
        }
        if log {
            mlog(String(format: "ui still patch: %d rect%@, %d px (%.0f%%), %d B, deflate %.1f ms", frames.count,
                        frames.count == 1 ? "" : "s", px, 100.0 * Double(px) / Double(n * n), total, ms))
        }
    }

    private static func stillFrame(_ payload: Data, number: Int) -> Data {
        var fr = VideoFrame.make(payload, number: number, key: false)
        fr[fr.startIndex + 1] = VideoFrame.still
        return fr
    }

    /* RGB565 big-endian, rounded: what a still patch carries (the panel's order). */
    static func bgraToRgb565(_ p: UnsafePointer<UInt8>, _ n: Int) -> [UInt8] {
        var out = [UInt8](repeating: 0, count: n * n * 2)
        out.withUnsafeMutableBufferPointer { o in
            let O = o.baseAddress!
            for i in 0..<(n * n) {
                let b = Int(p[4 * i]), g = Int(p[4 * i + 1]), r = Int(p[4 * i + 2])
                let v = ((r * 31 + 127) / 255) << 11 | ((g * 63 + 127) / 255) << 5 | (b * 31 + 127) / 255
                O[2 * i] = UInt8(v >> 8)
                O[2 * i + 1] = UInt8(v & 0xff)
            }
        }
        return out
    }

    /* BGRA to I420 (BT.601 video range), as UiWarp's shader does it: for a picture drawn over on the CPU. */
    static func bgraToI420(_ p: UnsafePointer<UInt8>, _ n: Int, _ o: UnsafeMutablePointer<UInt8>) {
        let h = n / 2
        let U = o + n * n, V = U + h * h
        for gy in 0..<h {
            for gx in 0..<h {
                var su = 0.0, sv = 0.0
                for k in 0..<4 {
                    let x = gx * 2 + (k & 1), y = gy * 2 + (k >> 1), i = 4 * (y * n + x)
                    let b = Double(p[i]) / 255, g = Double(p[i + 1]) / 255, r = Double(p[i + 2]) / 255
                    o[y * n + x] = UInt8(max(16, min(235, (16 + 65.481 * r + 128.553 * g + 24.966 * b).rounded())))
                    su += -37.797 * r - 74.203 * g + 112.0 * b
                    sv += 112.0 * r - 93.786 * g - 18.214 * b
                }
                U[gy * h + gx] = UInt8(max(16, min(240, (128 + su / 4).rounded())))
                V[gy * h + gx] = UInt8(max(16, min(240, (128 + sv / 4).rounded())))
            }
        }
    }

    /* The crop of a w x h source now (the mirror window moves), for rerender(); nil: the last one. */
    var cropProvider: ((Int, Int) -> SIMD4<Float>)?

    /* Renders the last source picture again (the overlay, the warp or the window changed). */
    func rerender() {
        submitLock.lock()   /* submit() sets them on the capture's queue */
        let pb0 = lastPB, crop0 = lastCrop
        submitLock.unlock()
        guard let pb = pb0 else { return }
        let crop = cropProvider?(CVPixelBufferGetWidth(pb), CVPixelBufferGetHeight(pb)) ?? crop0
        DispatchQueue.global(qos: .userInteractive).async { [weak self] in
            self?.submit(pb, crop: crop)
        }
    }

    /* Every band at qp; the frame's payload (bands as protocols/mgcommands.h lays them out). */
    private func encodeAll(_ pic: [UInt8], qp: Int, idr: Bool) -> (payload: Data, psnr: Double, ms: Double)? {
        let t0 = nowNs()
        let rows = bandRows(), n = cfg.n
        var out = Data()
        var mseSum = 0.0, psnrKnown = true
        var r0 = 0
        for (k, codec) in codecs.enumerated() {
            let h = rows[k]
            let res: (au: Data, psnr: Double)?
            if codecs.count == 1 {
                res = pic.withUnsafeBufferPointer { codec.encode($0.baseAddress!, qp: qp, idr: idr) }
            } else {
                /* the band's rows of each plane */
                pic.withUnsafeBufferPointer { src in
                    bandBuf[k].withUnsafeMutableBufferPointer { dst in
                        let S = src.baseAddress!, D = dst.baseAddress!
                        memcpy(D, S + r0 * n, h * n)
                        let ch = n / 2, srcC = S + n * n, dstC = D + h * n
                        for p in 0..<2 {
                            memcpy(dstC + p * (h / 2) * ch, srcC + p * ch * ch + (r0 / 2) * ch, (h / 2) * ch)
                        }
                    }
                }
                res = bandBuf[k].withUnsafeBufferPointer { codec.encode($0.baseAddress!, qp: qp, idr: idr) }
            }
            guard let r = res else { return nil }
            if k < codecs.count - 1 {
                let l = r.au.count
                out.append(contentsOf: [UInt8(l & 0xff), UInt8((l >> 8) & 0xff), UInt8((l >> 16) & 0xff), UInt8(l >> 24)])
            }
            out.append(r.au)
            if r.psnr > 0 {
                mseSum += pow(10, -r.psnr / 10) * Double(h)
            } else {
                psnrKnown = false
            }
            r0 += h
        }
        let psnr = psnrKnown && mseSum > 0 ? -10 * log10(mseSum / Double(n)) : (psnrKnown ? 99 : 0)
        return (out, psnr, Double(nowNs() - t0) / 1e6)
    }
}

/* zlib (RFC 1950) streams: the Compression framework's raw deflate in zlib's header and Adler-32 trailer. */
enum Zlib {
    static func compress(_ src: [UInt8]) -> Data? {
        let cap = src.count + src.count / 8 + 128
        var dst = [UInt8](repeating: 0, count: cap)
        let n = src.withUnsafeBufferPointer { s in
            dst.withUnsafeMutableBufferPointer { d in
                compression_encode_buffer(d.baseAddress!, cap, s.baseAddress!, s.count, nil, COMPRESSION_ZLIB)
            }
        }
        guard n > 0 else { return nil }
        var a: UInt32 = 1, b: UInt32 = 0
        var i = 0
        while i < src.count {
            let end = min(src.count, i + 5552)   /* sums stay under 2^32 between reductions */
            while i < end {
                a += UInt32(src[i])
                b += a
                i += 1
            }
            a %= 65521
            b %= 65521
        }
        let adler = b << 16 | a
        var out = Data([0x78, 0x9C])
        out.append(contentsOf: dst[0..<n])
        out.append(contentsOf: [UInt8(adler >> 24), UInt8((adler >> 16) & 0xff), UInt8((adler >> 8) & 0xff), UInt8(adler & 0xff)])
        return out
    }
}
