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
 * The UI tab (Tabs.swift): a picture at the gadget's own size, only when it
 * changes (protocols/UI.md): a web page rendered offscreen, or a square
 * window of the phone's screen, with the square-to-disc warp, and the
 * gadget's touches back.
 */
enum UiTab {
    static let demos = [("Clock", "demo:clock"), ("Dashboard", "demo:dashboard"), ("List", "demo:list")]

    static func stream(_ app: MainViewController, _ l: SettingsList) {
        l.header("UI stream")
        l.choice("Source", detail: { [weak app] in
                     guard let app else { return "" }
                     return !app.isBle() ? "Bluetooth LE only: pick it on the Gadget tab" : app.ui.label()
                 },
                 labels: { ["Off", "Web page", "This app's screen", "Whole screen (broadcast)"] },
                 selected: { [weak app] in app?.ui.source.rawValue ?? 0 },
                 pick: { [weak app] i in app?.ui.setSource(UiController.Source(rawValue: i) ?? .off) })
        l.toggle("Fisheye", detail: { "The whole square on the round panel (square to disc), else its corners are cut off" },
                 isOn: { [weak app] in app?.ui.warp ?? true }, set: { [weak app] on in app?.ui.setWarp(on) })
        l.toggle("Sharp", detail: { [weak app] in
                     guard let c = app?.caps() else { return "Still pictures end lossless where the gadget takes it" }
                     return c.uiStill & (1 << Int(Mg.stillRgb565Zlib)) != 0 ? "Still pictures end as lossless patches (the gadget's menu has it too)"
                         : "Still pictures get one more, finer refinement"
                 },
                 isOn: { [weak app] in app?.ui.sharp ?? true }, set: { [weak app] on in app?.ui.setSharp(on) })
        l.toggle("Touch", detail: { [weak app] in
                     guard let c = app?.caps() else { return "The gadget's touches back into the page" }
                     return c.uiTouch ? "The gadget's touches back into the page" : "This gadget sends no touch events"
                 },
                 isOn: { [weak app] in app?.ui.touchBack ?? true }, set: { [weak app] on in app?.ui.setTouch(on) })
        l.toggle("Two bands", detail: { [weak app] in
                     (app?.caps()?.uiBands ?? 1) >= 2 ? "Decoded on both of the gadget's cores" : "This gadget decodes one"
                 },
                 isOn: { [weak app] in app?.ui.twoBands ?? false }, set: { [weak app] on in app?.ui.setTwoBands(on) })
        l.info("Gadget") { [weak app] in
            guard let c = app?.caps() else { return "Not connected" }
            return c.uiW > 0 ? "UI \(c.uiW)x\(c.uiH), \(c.uiFps) fps, frames to \(c.uiMaxFrame / 1024) KB" + (c.uiBands > 1 ? ", 2 bands" : "")
                : "No UI streams: sent as video at \(min(c.maxW, c.maxH)) px"
        }
        l.info("Sending") { [weak app] in app?.ui.statsLine() ?? "" }
        l.info("Last change") { [weak app] in app?.ui.stepsLine() ?? "" }
        l.info("Touch") { [weak app] in app?.ui.lastTouch.isEmpty == false ? app!.ui.lastTouch : "-" }
    }

    static func webPage(_ app: MainViewController, _ l: SettingsList) {
        l.header("Web page")
        l.row({ "Address" }, detail: { [weak app] in app?.ui.url ?? "" }, action: { [weak app] in
            guard let app else { return }
            let a = UIAlertController(title: "Web page", message: "A URL, shown at the gadget's size", preferredStyle: .alert)
            a.addTextField { t in
                t.text = app.ui.url.hasPrefix("demo:") ? "https://" : app.ui.url
                t.keyboardType = .URL
                t.autocapitalizationType = .none
                t.autocorrectionType = .no
            }
            a.addAction(UIAlertAction(title: "Cancel", style: .cancel))
            a.addAction(UIAlertAction(title: "Show", style: .default) { [weak app] _ in
                if let u = a.textFields?.first?.text, !u.isEmpty {
                    app?.ui.load(u)
                }
            })
            app.present(a, animated: true)
        })
        for (name, u) in demos {
            l.row({ name }, detail: { "Demo page" }, value: { [weak app] in app?.ui.url == u ? "\u{2713}" : "" },
                  action: { [weak app] in app?.ui.load(u) })
        }
        l.choice("Page width", detail: { [weak app] in
                     guard let app, app.ui.n > 0 else { return "CSS pixels across the panel" }
                     return "\(Int(Double(app.ui.n) * app.ui.zoom)) CSS pixels across the panel"
                 },
                 labels: { ["Half the panel's pixels", "Three quarters", "The panel's pixels", "One and a half"] },
                 selected: { [weak app] in [0.5, 0.75, 1.0, 1.5].firstIndex(of: app?.ui.zoom ?? 1) ?? 2 },
                 pick: { [weak app] i in app?.ui.setZoom([0.5, 0.75, 1.0, 1.5][i]) })
    }

    static func mirror(_ app: MainViewController, _ l: SettingsList) {
        l.header("Screen mirror")
        l.row({ [weak app] in app?.ui.broadcasting == true ? "Stop broadcast" : "Start broadcast" },
              detail: { [weak app] in app?.ui.extensionLine() ?? "" },
              action: { [weak app] in app?.ui.startOrStopBroadcast() })
        l.row({ "Proof key" }, detail: { [weak app] in app?.ui.proofKeyLine() ?? "" }, action: { [weak app] in
            guard let app else { return }
            let a = UIAlertController(title: "Proof key",
                                      message: "The screen mirror (this app's screen, the broadcast) streams only to a gadget "
                                          + "that proves it holds this key. "
                                          + "After esp32/tools/mg_ble_client.py setup, paste proof_key from "
                                          + "~/.mg_ble_client/proof-<address>.json (64 hex digits). Empty removes it.",
                                      preferredStyle: .alert)
            a.addTextField { t in
                t.placeholder = "64 hex digits"
                t.isSecureTextEntry = true
                t.autocapitalizationType = .none
                t.autocorrectionType = .no
            }
            a.addAction(UIAlertAction(title: "Cancel", style: .cancel))
            a.addAction(UIAlertAction(title: "Save", style: .default) { [weak app] _ in
                if app?.ui.setProofKey(a.textFields?.first?.text ?? "") == false {
                    let none = app?.ui.mirror.gadgetName.isEmpty ?? true
                    let e = UIAlertController(title: none ? "Pick a gadget first" : "Not a proof key",
                                              message: none ? "The key belongs to the gadget picked on the Gadget tab."
                                                  : "A proof key is 64 hex digits.",
                                              preferredStyle: .alert)
                    e.addAction(UIAlertAction(title: "OK", style: .default))
                    app?.present(e, animated: true)
                }
            })
            app.present(a, animated: true)
        })
        l.choice("Overscroll", detail: { "How far the window may go past the screen's edges (black beyond)" },
                 labels: { MirrorSettings.overscrollNames }, selected: { [weak app] in app?.ui.mirror.overscroll ?? 1 },
                 pick: { [weak app] i in app?.ui.setOverscroll(i) })
        l.slider("Window size", value: { [weak app] in "\(Int((app?.ui.mirror.size ?? 0.9) * 100))% of the width" },
                 get: { [weak app] in Float(app?.ui.mirror.size ?? 0.9) },
                 set: { [weak app] v in app?.ui.setMirror { $0.size = Double(max(0.2, v)) } })
        l.slider("Window across", value: { [weak app] in "center at \(Int((app?.ui.mirror.cx ?? 0.5) * 100))%" },
                 get: { [weak app] in Float(app?.ui.mirror.cx ?? 0.5) },
                 set: { [weak app] v in app?.ui.setMirror { $0.cx = Double(v) } })
        l.slider("Window down", value: { [weak app] in "center at \(Int((app?.ui.mirror.cy ?? 0.5) * 100))%" },
                 get: { [weak app] in Float(app?.ui.mirror.cy ?? 0.5) },
                 set: { [weak app] v in app?.ui.setMirror { $0.cy = Double(v) } })
        l.footnote("The broadcast streams from its own Bluetooth connection to the gadget picked on the Gadget tab, "
            + "with this window and fisheye setting, while you use other apps, once the gadget proves its key. "
            + "This app's screen streams from here, only while the app is open.")
    }

    static func hid(_ app: MainViewController, _ l: SettingsList) {
        l.header("Gadget as a pointer (HID)")
        l.toggle("Pointer", detail: { [weak app] in
                     guard let c = app?.caps() else { return "Touching the gadget moves and clicks a pointer on the mirror window" }
                     return c.hidMouseRel ? "Touching the gadget moves and clicks a pointer on the mirror window" : "This gadget isn't a HID pointer"
                 },
                 isOn: { [weak app] in app?.ui.mirror.hid ?? false },
                 set: { [weak app] on in app?.ui.setMirror { $0.hid = on } })
        l.info("HID") { [weak app] in app?.ui.hidStatus.isEmpty == false ? app!.ui.hidStatus : "-" }
        l.info("Calibration") { [weak app] in
            guard let m = app?.ui.mirror else { return "" }
            return m.hidCountsW > 0 ? "\(m.hidCountsW) x \(m.hidCountsH) counts across the screen" : "Not calibrated (1 count a point)"
        }
        l.row({ "Calibrate" }, detail: { "Tap the gadget a few times while the pointer is on; see the steps" }, action: { [weak app] in
            app?.showCalibration()
        })
        l.footnote("Pair the gadget in Settings > Bluetooth, then turn on Settings > Accessibility > Touch > AssistiveTouch; "
            + "the gadget shows up under AssistiveTouch > Devices. iOS takes a relative pointer only: the gadget parks it "
            + "in the top-left corner at each touch and moves it to where you touch, using the calibration.")
    }
}

/* A full-screen target for the pointer calibration: AssistiveTouch's clicks land here as taps. */
final class CalibrationController: UIViewController {
    private weak var ui: UiController?
    private let label = UILabel()

    init(ui: UiController) {
        self.ui = ui
        super.init(nibName: nil, bundle: nil)
        modalPresentationStyle = .fullScreen
    }

    required init?(coder: NSCoder) {
        fatalError()
    }

    override func viewDidLoad() {
        super.viewDidLoad()
        view.backgroundColor = .black
        label.numberOfLines = 0
        label.textColor = .white
        label.textAlignment = .center
        label.text = "Pointer calibration\n\nThe gadget's whole panel maps to this whole screen. Tap the gadget near its "
            + "edges (top-left, bottom-right, ...); each click that lands here corrects the counts.\n\nTap here with your finger to finish."
        label.translatesAutoresizingMaskIntoConstraints = false
        view.addSubview(label)
        NSLayoutConstraint.activate([
            label.centerXAnchor.constraint(equalTo: view.centerXAnchor),
            label.centerYAnchor.constraint(equalTo: view.centerYAnchor),
            label.widthAnchor.constraint(equalTo: view.widthAnchor, multiplier: 0.8),
        ])
        ui?.onCalibrated = { [weak self] msg in self?.label.text = "Calibrated: " + msg + "\n\nTap the gadget again to refine, or here with your finger to finish." }
        ui?.startCalibration()
    }

    override func touchesEnded(_ touches: Set<UITouch>, with event: UIEvent?) {
        guard let t = touches.first, let ui else { return }
        let p = t.location(in: view)
        /* A finger is .direct; AssistiveTouch's pointer clicks come as .indirectPointer or synthesized taps. */
        if t.type == .direct && !ui.calibratingExpectsTap {
            ui.endCalibration()
            dismiss(animated: true)
            return
        }
        ui.calibrationTap(p)
    }
}

extension UiController {
    /* A gadget touch is waiting for its click to land. */
    var calibratingExpectsTap: Bool { calibrating && calibPending }
}
