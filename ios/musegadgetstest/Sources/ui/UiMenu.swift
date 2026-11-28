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
 * A UI stream's controls and menu (protocols/mgcommands.h, UI input,
 * controls), shared by the app and the broadcast extension: fisheye, pan,
 * quality, stop, menu and edge pan kept in sync with the gadget; the menu its
 * button opens, drawn into the picture after the warp at the panel's pixels
 * (so it reads the same on every gadget) and hit-tested with its touches;
 * and panning the mirror window by the gestures the gadget marks as pans
 * (MG_TOUCH_ID_PAN in the touch's id: an edge-ring or pan-mode touch that
 * moved; the gadget decides, and keeps them from its HID). Touches that are
 * none of these go to onTouch (the web page, when touch is on).
 */
final class UiControls {
    let n: Int
    let mirror: Bool                    /* pan and edge pan: the screen mirror only */
    let round: Bool                     /* a round panel: the fisheye item, and an edge ring (else a band) */
    private(set) var fisheye: Bool
    private(set) var sharp: Bool
    private(set) var pan = false
    private(set) var menuOpen = false
    private(set) var overscroll = 1   /* the mirror window past the screen's edges: 0 none, 1 vertical, 2 all */

    /* To the gadget: [ui_input, control, id, value]. */
    var send: (UInt8, Int) -> Void = { _, _ in }
    var onFisheye: (Bool) -> Void = { _ in }
    var onSharp: (Bool) -> Void = { _ in }
    var onStop: () -> Void = {}
    /* The window follows the finger the other way: dx, dy in panel pixels. */
    var onPan: (Double, Double) -> Void = { _, _ in }
    /* The menu's look changed: render the picture again. */
    var onRedraw: () -> Void = {}
    /* A touch that's not for the menu or panning (action, panel x, y). */
    var onTouch: (Int, Int, Int) -> Void = { _, _, _ in }
    /* The mirror window back to the screen's middle. */
    var onRecenter: () -> Void = {}
    /* The phone's Back (0) or Home (1): the gadget's HID consumer keys. */
    var onNav: (Int) -> Void = { _ in }
    /* The gadget's HID drives the phone (pointer mode): the nav keys work. */
    var navAvailable: () -> Bool = { false }
    var onOverscroll: (Int) -> Void = { _ in }

    private struct Item {
        let label: String
        let value: (() -> String)?
        var row = 0   /* items with the same nonzero row share one line, side by side */
        let tap: () -> Void

        init(label: String, value: (() -> String)?, row: Int = 0, tap: @escaping () -> Void) {
            self.label = label
            self.value = value
            self.row = row
            self.tap = tap
        }
    }

    private var items: [Item] = []
    private var pressed = -1
    private var gestureMenu = false, gesturePan = false
    private var lastX = 0, lastY = 0
    private let lock = NSLock()

    init(n: Int, mirror: Bool, fisheye: Bool, sharp: Bool, round: Bool = true, overscroll: Int = 1) {
        self.overscroll = overscroll
        self.n = n
        self.mirror = mirror
        self.round = round
        self.fisheye = fisheye
        self.sharp = sharp
        items = buildItems()
    }

    private func buildItems() -> [Item] {
        var l: [Item] = []
        if mirror && navAvailable() {
            /* The phone's own navigation through the gadget's HID (iOS has no
             * app-level way to press Back or Home for other apps, nor Recents). */
            l.append(Item(label: "Back", value: nil, row: 1) { [unowned self] in
                self.setMenu(false)
                self.onNav(0)
            })
            l.append(Item(label: "Home", value: nil, row: 1) { [unowned self] in
                self.setMenu(false)
                self.onNav(1)
            })
        }
        if round {   /* a square panel has no corners to save */
            l.append(Item(label: "Fisheye", value: { [unowned self] in self.fisheye ? "On" : "Off" }) { [unowned self] in
                self.setFisheye(!self.fisheye)
            })
        }
        if mirror {
            l.append(Item(label: "Pan", value: { [unowned self] in self.pan ? "On" : "Off" }, row: 3) { [unowned self] in
                self.setPan(!self.pan)
                if self.pan {
                    self.setMenu(false)   /* straight to panning */
                }
            })
            l.append(Item(label: "Recenter", value: nil, row: 3) { [unowned self] in
                self.setMenu(false)
                self.onRecenter()
            })
            l.append(Item(label: "Overscroll", value: { [unowned self] in MirrorSettings.overscrollNames[self.overscroll] }) { [unowned self] in
                self.setOverscroll((self.overscroll + 1) % 3)
            })
        }
        l.append(Item(label: "Quality", value: { [unowned self] in self.sharp ? "Sharp" : "Fast" }) { [unowned self] in
            self.setSharp(!self.sharp)
        })
        l.append(Item(label: "Stop", value: nil, row: 2) { [unowned self] in
            self.setMenu(false)
            self.onStop()
        })
        l.append(Item(label: "Close", value: nil, row: 2) { [unowned self] in self.setMenu(false) })
        return l
    }

    /* What each control is, at the start of a UI stream. */
    func sendAll() {
        send(Mg.ctlFisheye, fisheye ? 1 : 0)
        send(Mg.ctlQuality, sharp ? 1 : 0)
        send(Mg.ctlPan, pan ? 1 : 0)
        send(Mg.ctlMenu, menuOpen ? 1 : 0)
        send(Mg.ctlEdgePan, mirror ? 1 : 0)
        if mirror {
            send(Mg.ctlOverscroll, overscroll)
        }
    }

    func setOverscroll(_ v: Int) {
        overscroll = max(0, min(2, v))
        send(Mg.ctlOverscroll, overscroll)
        onOverscroll(overscroll)
        onRedraw()
    }

    func setFisheye(_ on: Bool, echo: Bool = true) {
        fisheye = on
        send(Mg.ctlFisheye, on ? 1 : 0)
        onFisheye(on)
        onRedraw()
    }

    func setSharp(_ on: Bool) {
        sharp = on
        send(Mg.ctlQuality, on ? 1 : 0)
        onSharp(on)
        onRedraw()
    }

    func setPan(_ on: Bool) {
        pan = on && mirror
        send(Mg.ctlPan, pan ? 1 : 0)
        onRedraw()
    }

    func setMenu(_ open: Bool) {
        lock.lock()
        if open {
            items = buildItems()   /* the nav row depends on the HID mode now */
        }
        menuOpen = open
        pressed = -1
        lock.unlock()
        send(Mg.ctlMenu, open ? 1 : 0)
        mlog("ui: menu " + (open ? "open" : "closed"))
        onRedraw()
    }

    /* A control from the gadget (its menu button, or a setting changed there): apply and echo. */
    func gadgetControl(_ id: UInt8, _ value: Int) {
        mlog("ui: gadget control \(id) = \(value)")
        switch id {
        case Mg.ctlMenu: setMenu(value != 0)
        case Mg.ctlFisheye: setFisheye(value != 0)
        case Mg.ctlPan: setPan(value != 0)
        case Mg.ctlQuality: setSharp(value != 0)
        case Mg.ctlOverscroll where mirror: setOverscroll(value)
        case Mg.ctlStop where value != 0: onStop()
        default: break
        }
    }

    /* A touch from the gadget (panel pixels; id: its pointer id byte). */
    func touch(action: Int, x: Int, y: Int, id: Int = 0) {
        if action == 0 {
            gestureMenu = menuOpen
            /* The gadget decides tap or pan (protocols/mgcommands.h, MG_TOUCH_ID_PAN). */
            gesturePan = !gestureMenu && mirror && (id & Int(Mg.touchIdPan)) != 0
            lastX = x
            lastY = y
        }
        if gestureMenu {
            lock.lock()
            let its = items
            lock.unlock()
            let i = hit(x, y, its)
            if action == 2 {
                lock.lock()
                pressed = -1
                lock.unlock()
                if i >= 0 {
                    mlog("ui: menu " + its[i].label)
                    its[i].tap()
                }
                onRedraw()
            } else if action == 0 {
                lock.lock()
                pressed = i
                lock.unlock()
                onRedraw()
            }
            return
        }
        if gesturePan {
            if action == 2 {
                mlog("ui: pan gesture ended at \(x),\(y) (the gadget marked it a pan)")
            }
            let dx = Double(x - lastX), dy = Double(y - lastY)
            lastX = x
            lastY = y
            if action != 0 && (dx != 0 || dy != 0) {
                onPan(dx, dy)
            }
            return
        }
        onTouch(action, x, y)
    }

    /* Each item's box, in panel pixels (top-left origin): a line per item, or
     * per run of items with the same row, side by side. */
    private func layout(_ items: [Item]) -> [CGRect] {
        var lines: [[Int]] = []
        for (i, it) in items.enumerated() {
            if it.row != 0, let last = lines.last, items[last[0]].row == it.row {
                lines[lines.count - 1].append(i)
            } else {
                lines.append([i])
            }
        }
        let s = CGFloat(n), count = CGFloat(lines.count)
        let rowH = s * (lines.count > 4 ? 0.105 : 0.12), gap = s * (lines.count > 4 ? 0.02 : 0.025), rowW = s * 0.68
        let top = (s - count * rowH - (count - 1) * gap) / 2 + s * 0.02
        var boxes = [CGRect](repeating: .zero, count: items.count)
        for (li, line) in lines.enumerated() {
            let y = top + CGFloat(li) * (rowH + gap), x0 = (s - rowW) / 2
            let cellGap = s * 0.02, cellW = (rowW - CGFloat(line.count - 1) * cellGap) / CGFloat(line.count)
            for (k, i) in line.enumerated() {
                boxes[i] = CGRect(x: x0 + CGFloat(k) * (cellW + cellGap), y: y, width: cellW, height: rowH)
            }
        }
        return boxes
    }

    private func hit(_ x: Int, _ y: Int, _ items: [Item]) -> Int {
        let p = CGPoint(x: CGFloat(x), y: CGFloat(y))
        return layout(items).firstIndex { $0.contains(p) } ?? -1
    }

    var drawing: Bool {
        lock.lock()
        defer { lock.unlock() }
        return menuOpen
    }

    /* The menu over a BGRA picture (n x n), in place, when it's open; true: it drew. */
    func draw(_ bgra: UnsafeMutablePointer<UInt8>, _ size: Int) -> Bool {
        lock.lock()
        let open = menuOpen, pr = pressed, items = self.items   /* setMenu rebuilds them on another thread */
        lock.unlock()
        guard open, size == n,
              let ctx = CGContext(data: bgra, width: n, height: n, bitsPerComponent: 8, bytesPerRow: n * 4,
                                  space: CGColorSpaceCreateDeviceRGB(),
                                  bitmapInfo: CGImageAlphaInfo.premultipliedFirst.rawValue | CGBitmapInfo.byteOrder32Little.rawValue)
        else { return false }
        ctx.translateBy(x: 0, y: CGFloat(n))
        ctx.scaleBy(x: 1, y: -1)
        UIGraphicsPushContext(ctx)
        defer { UIGraphicsPopContext() }
        let s = CGFloat(n)
        UIColor(white: 0, alpha: 170.0 / 255).setFill()
        ctx.fill(CGRect(x: 0, y: 0, width: s, height: s))
        let boxes = layout(items)
        let font = UIFont.boldSystemFont(ofSize: s * 0.05)
        let white: [NSAttributedString.Key: Any] = [.font: font, .foregroundColor: UIColor.white]
        let accent: [NSAttributedString.Key: Any] = [.font: font, .foregroundColor: UIColor(red: 90 / 255, green: 200 / 255, blue: 250 / 255, alpha: 1)]
        let titleA: [NSAttributedString.Key: Any] = [.font: UIFont.systemFont(ofSize: s * 0.04),
                                                     .foregroundColor: UIColor(red: 170 / 255, green: 170 / 255, blue: 180 / 255, alpha: 1)]
        let title = NSAttributedString(string: "Menu", attributes: titleA)
        let ts = title.size()
        title.draw(at: CGPoint(x: (s - ts.width) / 2, y: (boxes.first?.minY ?? s / 3) - s * 0.04 - ts.height * 0.8))
        for (i, it) in items.enumerated() {
            let b = boxes[i]
            (i == pr ? UIColor(red: 70 / 255, green: 78 / 255, blue: 92 / 255, alpha: 1)
                : UIColor(red: 40 / 255, green: 44 / 255, blue: 52 / 255, alpha: 1)).setFill()
            UIBezierPath(roundedRect: b, cornerRadius: b.height / 2).fill()
            let label = NSAttributedString(string: it.label, attributes: white)
            let ls = label.size()
            let y = b.midY - ls.height / 2
            if let v = it.value?() {
                label.draw(at: CGPoint(x: b.minX + b.height * 0.45, y: y))
                let val = NSAttributedString(string: v, attributes: accent)
                val.draw(at: CGPoint(x: b.maxX - b.height * 0.45 - val.size().width, y: y))
            } else {
                label.draw(at: CGPoint(x: b.midX - ls.width / 2, y: y))
            }
        }
        return true
    }
}
