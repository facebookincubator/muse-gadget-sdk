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
import UIKit

/*
 * The stats over the preview, as on Android (StatsOverlay.java): a
 * translucent rounded panel. Compact shows what's being sent and four key
 * numbers; detailed adds the Link, Encoder, Gadget and Latency sections,
 * labels muted, values monospaced so they don't jitter, problems in the
 * warning color. Tap the preview to switch. The "stats:" log line is
 * written by MainViewController as before.
 */
final class StatsOverlay: UIView {
    enum Mode: Int {
        case compact = 0, detailed = 1, off = 2
    }

    struct Item {
        let label: String, value: String, bad: Bool
    }

    /* One second's numbers (MainViewController.tick()). */
    struct Stats {
        var connected = false, streaming = false, preparing = false, clip = false
        var codec = "", source = "", encoder = "", linkName = ""
        var size = 0, fps = 0, bands = 1, targetKbps = 0
        var fixedRate = false
        var linkKind = "", linkDetail = ""
        var sendKbps = 0.0, gateMsPerS = 0.0
        var inFlight: Int64 = 0, queued: Int64 = 0
        var fedFps = 0.0, encFps = 0.0
        var skipped = 0, skippedNew = 0, keys = 0
        var shownFps = 0.0
        var dropped = 0, droppedNew = 0, decodeMs = 0
        var free: Int64 = 0, buffer: Int64 = 0
        var camToEncMs = 0.0, convertMs = 0.0, encodeMs = 0.0
        var note = ""
        /* More lines from a link or source, by section ("LINK", "LATENCY", or a new one). */
        var extra: [(String, Item)] = []

        mutating func add(_ section: String, _ label: String, _ value: String, bad: Bool = false) {
            extra.append((section, Item(label: label, value: value, bad: bad)))
        }
    }

    var stats = Stats() {
        didSet { setNeedsDisplay() }
    }
    var mode = Mode.compact {
        didSet { setNeedsDisplay() }
    }

    private let label = UIFont.systemFont(ofSize: 11)
    private let value = UIFont.monospacedSystemFont(ofSize: 11.5, weight: .regular)
    private let valueBold = UIFont.monospacedSystemFont(ofSize: 13, weight: .bold)
    private let head = UIFont.systemFont(ofSize: 13, weight: .semibold)
    private let section = UIFont.systemFont(ofSize: 10, weight: .bold)
    private let muted = UIColor(red: 0.56, green: 0.59, blue: 0.63, alpha: 1)
    private let sectionColor = UIColor(red: 0.49, green: 0.56, blue: 0.69, alpha: 1)

    override init(frame: CGRect) {
        super.init(frame: frame)
        isOpaque = false
        backgroundColor = .clear
        isUserInteractionEnabled = false
        contentMode = .redraw
    }

    required init?(coder: NSCoder) {
        fatalError()
    }

    private var s: Stats { stats }

    private func f0(_ v: Double) -> String { String(format: "%.0f", v) }
    private func f1(_ v: Double) -> String { String(format: "%.1f", v) }

    private var skipBad: Bool { s.skippedNew > 2 }
    private var shownBad: Bool { s.streaming && s.encFps > 5 && s.shownFps < s.encFps * 0.85 }
    private var decodeBad: Bool { s.decodeMs > 0 && s.fps > 0 && s.decodeMs * s.fps > 900 }
    private var gateBad: Bool { s.gateMsPerS > 150 }
    private var latencyBad: Bool { !s.clip && s.camToEncMs > 150 }

    private var title: String {
        if !s.connected { return "Not connected" }
        if s.preparing { return "Preparing " + s.source }
        if !s.streaming { return "Not sending" }
        return "\(s.codec) \(s.size)×\(s.size) · \(s.fps) fps" + (s.bands > 1 ? " · \(s.bands) bands" : "")
    }

    private var sub: String {
        if !s.streaming || s.preparing { return s.connected ? s.linkName : "" }
        return "\(s.targetKbps) kb/s \(s.fixedRate ? "fixed" : "auto") · link \(f0(s.sendKbps)) kb/s · "
            + (s.encoder.isEmpty ? s.source : s.source + " · " + s.encoder)
    }

    private var compactItems: [Item] {
        [Item(label: "LINK", value: f0(s.sendKbps) + " kb/s", bad: gateBad),
         Item(label: "GADGET", value: f1(s.shownFps) + " fps", bad: shownBad),
         Item(label: "DECODE", value: "\(s.decodeMs) ms", bad: decodeBad),
         s.clip ? Item(label: "DROPPED", value: "\(s.dropped)", bad: s.droppedNew > 0)
             : Item(label: "SKIPPED", value: s.skippedNew > 0 ? "+\(s.skippedNew)" : "\(s.skipped)", bad: skipBad)]
    }

    private var sections: [(String, [Item])] {
        var all: [(String, [Item])] = []
        all.append(("LINK", [Item(label: "link", value: s.linkKind, bad: false),
                             Item(label: "sent", value: f0(s.sendKbps) + " kb/s", bad: false),
                             Item(label: "in flight", value: "\(s.inFlight) B", bad: false),
                             Item(label: "queued", value: "\(s.queued) B", bad: s.queued > 16384),
                             Item(label: "gate", value: f0(s.gateMsPerS) + " ms/s", bad: gateBad)]))
        var enc: [Item] = []
        if s.clip {
            enc.append(Item(label: "frames", value: f1(s.encFps) + " fps", bad: false))
            enc.append(Item(label: "target", value: "\(s.targetKbps) kb/s", bad: false))
            enc.append(Item(label: "rate", value: s.fixedRate ? "fixed" : "auto", bad: false))
        } else {
            enc.append(Item(label: "camera", value: f1(s.fedFps) + " fps", bad: s.streaming && s.fedFps < Double(s.fps) * 0.8))
            enc.append(Item(label: "encoded", value: f1(s.encFps) + " fps", bad: false))
            enc.append(Item(label: "skipped", value: "\(s.skipped)" + (s.skippedNew > 0 ? " +\(s.skippedNew)" : ""), bad: skipBad))
            enc.append(Item(label: "keyframes", value: "\(s.keys)", bad: false))
        }
        enc.append(Item(label: "encoder", value: s.encoder.isEmpty ? "-" : s.encoder, bad: false))
        all.append((s.clip ? "CLIP" : "ENCODER", enc))
        all.append(("GADGET", [Item(label: "shown", value: f1(s.shownFps) + " fps", bad: shownBad),
                               Item(label: "dropped", value: "\(s.dropped)" + (s.droppedNew > 0 ? " +\(s.droppedNew)" : ""),
                                    bad: s.droppedNew > 0),
                               Item(label: "decode", value: "\(s.decodeMs) ms", bad: decodeBad),
                               Item(label: "free", value: s.free >= 1024 ? "\(s.free / 1024) KB" : "\(s.free) B",
                                    bad: s.buffer > 0 && s.streaming && s.free < s.buffer / 8)]))
        all.append(("LATENCY", s.clip ? [] : [Item(label: "cam→enc", value: f0(s.camToEncMs) + " ms", bad: latencyBad),
                                              Item(label: "convert", value: f1(s.convertMs) + " ms", bad: false),
                                              Item(label: "encode", value: f1(s.encodeMs) + " ms", bad: false)]))
        for (sec, item) in s.extra {
            if let i = all.firstIndex(where: { $0.0 == sec }) {
                all[i].1.append(item)
            } else {
                all.append((sec, [item]))
            }
        }
        return all.filter { !$0.1.isEmpty }
    }

    private var notes: [String] {
        [s.linkDetail, s.note].filter { !$0.isEmpty }
    }

    private let pad: CGFloat = 12, margin: CGFloat = 10
    private var lineH: CGFloat { 16 }
    private var headH: CGFloat { 18 }

    /* The panel's height in this mode. */
    private func panelHeight() -> CGFloat {
        let subH: CGFloat = sub.isEmpty ? 0 : lineH
        if mode == .compact {
            return pad * 2 + headH + subH + (s.streaming ? 8 + 2 * lineH + 2 : 0)
        }
        var body: CGFloat = 0
        let secs = sections
        var i = 0
        while i < secs.count {
            let n = max(secs[i].1.count, i + 1 < secs.count ? secs[i + 1].1.count : 0)
            body += CGFloat(n + 1) * lineH + 10
            i += 2
        }
        return pad * 2 + headH + subH + 6 + body + CGFloat(notes.count) * lineH
    }

    /* The height this view needs to show the whole panel (margins included). */
    var neededHeight: CGFloat {
        isHidden ? 0 : panelHeight() + 2 * margin
    }

    private func draw(_ t: String, _ font: UIFont, _ color: UIColor, at p: CGPoint, width: CGFloat, right: Bool = false) {
        let ps = NSMutableParagraphStyle()
        ps.lineBreakMode = .byTruncatingTail
        ps.alignment = right ? .right : .left
        (t as NSString).draw(in: CGRect(x: p.x, y: p.y, width: width, height: font.lineHeight + 2),
                             withAttributes: [.font: font, .foregroundColor: color, .paragraphStyle: ps])
    }

    override func draw(_ rect: CGRect) {
        let w = bounds.width - 2 * margin
        let height = min(panelHeight(), bounds.height - 2 * margin)
        let panel = UIBezierPath(roundedRect: CGRect(x: margin, y: margin, width: w, height: height), cornerRadius: 14)
        UIColor(red: 0.063, green: 0.071, blue: 0.086, alpha: 0.72).setFill()
        panel.fill()
        let x0 = margin + pad
        var y = margin + pad
        let status = !s.connected ? Theme.text3 : s.streaming && !s.preparing ? Theme.good : s.preparing ? Theme.warn : Theme.text2
        status.setFill()
        UIBezierPath(ovalIn: CGRect(x: x0, y: y + 5, width: 8, height: 8)).fill()
        let tx = x0 + 14, tw = w - 2 * pad - 14
        draw(title, head, s.streaming && !s.preparing ? Theme.sending : Theme.text, at: CGPoint(x: tx, y: y), width: tw)
        y += headH
        if !sub.isEmpty {
            draw(sub, label, UIColor(red: 0.79, green: 0.8, blue: 0.83, alpha: 1), at: CGPoint(x: tx, y: y), width: tw)
            y += lineH
        }
        if mode == .compact {
            guard s.streaming else { return }
            y += 8
            let items = compactItems
            let cw = (w - 2 * pad) / CGFloat(items.count)
            for (i, it) in items.enumerated() {
                let cx = x0 + CGFloat(i) * cw
                draw(it.label, section, muted, at: CGPoint(x: cx, y: y), width: cw)
                draw(it.value, valueBold, it.bad ? Theme.warn : Theme.text, at: CGPoint(x: cx, y: y + lineH - 2), width: cw - 4)
            }
            return
        }
        y += 6
        let secs = sections
        let colW = (w - 2 * pad - 16) / 2
        var i = 0
        while i < secs.count {
            var n = 0
            for col in 0..<2 where i + col < secs.count {
                let cx = x0 + CGFloat(col) * (colW + 16)
                var yy = y
                draw(secs[i + col].0, section, sectionColor, at: CGPoint(x: cx, y: yy), width: colW)
                yy += lineH
                for it in secs[i + col].1 {
                    draw(it.label, label, muted, at: CGPoint(x: cx, y: yy), width: colW)
                    draw(it.value, value, it.bad ? Theme.warn : Theme.text, at: CGPoint(x: cx + 60, y: yy), width: colW - 60,
                         right: true)
                    yy += lineH
                }
                n = max(n, secs[i + col].1.count)
            }
            y += CGFloat(n + 1) * lineH + 10
            i += 2
        }
        for t in notes {
            draw(t, label, UIColor(red: 0.46, green: 0.49, blue: 0.53, alpha: 1), at: CGPoint(x: x0, y: y - 4), width: w - 2 * pad)
            y += lineH
        }
    }
}
