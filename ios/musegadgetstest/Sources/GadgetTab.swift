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
 * The Gadget tab: which gadget the app talks to (the device picker), how it
 * starts, what the gadget reports about itself and how it's decoding.
 */
enum GadgetTab {
    static func connection(_ app: MainViewController, _ l: SettingsList) {
        l.header("Connection")
        l.row({ "Gadget" }, detail: { [weak app] in app?.connectionLine() ?? "" },
              value: { [weak app] in app?.gadgetOff == true ? "Not connected" : app?.gadgetName() ?? "Choose" },
              action: { [weak app] in app?.pick("device") })
        l.row({ [weak app] in "Disconnect " + (app?.gadgetName() ?? "the gadget") },
              detail: { "Ends any stream, then stays disconnected until you connect or choose a gadget" },
              action: { [weak app] in app?.disconnectGadget() })
            .showIf { [weak app] in app?.gadgetOff == false }
        l.row({ [weak app] in "Connect " + (app?.lastGadget() ?? "") }, detail: { "Connect to it again" },
              action: { [weak app] in app?.connectLast() })
            .showIf { [weak app] in app?.gadgetOff == true && app?.lastGadget() != nil }
        l.row({ "Choose a gadget" }, detail: { "The gadgets nearby, to connect to one" },
              action: { [weak app] in app?.pick("device") })
            .showIf { [weak app] in app?.gadgetOff == true }
        l.row({ [weak app] in "Forget " + ((app?.gadgetOff == true ? app?.lastGadget() : app?.gadgetName()) ?? "") + "…" },
              detail: { "Disconnect and forget it" }, action: { [weak app] in app?.forgetGadget() })
            .showIf { [weak app] in (app?.gadgetOff == true ? app?.lastGadget() : app?.gadgetName()) != nil }
        l.toggle("Start automatically", detail: { "Stream as soon as the gadget connects (over BLE, once it proves its key; over Wi-Fi, only the connection it is turned on for), without waiting for its Phone camera view" },
                 isOn: { [weak app] in app?.autoStart() ?? false }, set: { [weak app] on in app?.setAutoStart(on) })
    }

    static func reports(_ app: MainViewController, _ l: SettingsList) {
        l.header("What the gadget reports")
        l.info("Display") { [weak app] in app?.caps().map { "\($0.dispW)×\($0.dispH)" + ($0.round ? " round" : "") } ?? "-" }
        l.info("Pictures up to") { [weak app] in app?.caps().map { "\($0.maxW)×\($0.maxH), \($0.maxFps) fps" } ?? "-" }
        l.info("Suggests") { [weak app] in app?.caps().map { "\($0.sugW)×\($0.sugH), \($0.sugFps) fps, \($0.sugKbps) kb/s" } ?? "-" }
        l.info("Decodes") { [weak app] in
            app?.caps().map { ($0.vp8 ? "H.264, VP8" : "H.264") + ($0.maxTiles > 1 ? ", \($0.maxTiles) bands" : "") } ?? "-"
        }
        l.info("Buffer") { [weak app] in
            app?.caps().map { "\($0.buffer / 1024) KB" + ($0.maxPlayoutMs > 0 ? ", playout \($0.maxPlayoutMs) ms" : "") } ?? "-"
        }
        l.info("Decoding now") { [weak app] in app?.decodeLine() ?? "-" }
        l.row({ "Link" }, detail: { [weak app] in app?.linkDetail() ?? "" })
    }

    static func about(_ app: MainViewController, _ l: SettingsList) {
        l.header("About")
        l.info("App") { "Muse Gadgets Test " + (Bundle.main.infoDictionary?["CFBundleShortVersionString"] as? String ?? "") }
        l.info("Bundle") { Bundle.main.bundleIdentifier ?? "" }
        l.footnote("Streams to Muse gadgets for testing: protocols/VIDEO.md. Logs on the MuseCam tag (Xcode's console).")
    }
}
