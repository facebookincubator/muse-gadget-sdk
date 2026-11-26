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
 * musegadgets BLE on the screen (Tabs.swift): the Gadget tab's link choice
 * (Wi-Fi or Bluetooth LE), and the Audio tab: a tone loop on the gadget's
 * speaker beside the video, its speaker volume, and capturing its mic.
 */
enum BleTab {
    static func link(_ app: MainViewController, _ l: SettingsList) {
        l.header("Link")
        l.choice("Connect over", detail: { [weak app] in app?.isBle() == true ? "musegadgets BLE: video, audio and the mic"
                     : "The local network: the phone and the gadget on the same Wi-Fi" },
                 labels: { ["Wi-Fi", "Bluetooth LE"] }, selected: { [weak app] in app?.isBle() == true ? 1 : 0 },
                 pick: { [weak app] i in app?.setBle(i == 1) })
    }

    private static func needsBle(_ app: MainViewController?, _ desc: String) -> String {
        guard let app else { return desc }
        return !app.isBle() ? "Bluetooth LE only: pick it on the Gadget tab" : !app.connected() ? "Not connected" : desc
    }

    static func playback(_ app: MainViewController, _ l: SettingsList) {
        l.header("Playback")
        l.toggle("Tone", detail: { [weak app] in needsBle(app, "A loop on the gadget's speaker, beside any video") },
                 isOn: { [weak app] in app?.toneOn() ?? false }, set: { [weak app] on in app?.setTone(on) })
            .enabledIf { [weak app] in app?.bleReady() ?? false }
        l.choice("Tone codec", detail: { "What the tone is sent as" }, labels: { ["SBC", "LC3"] },
                 selected: { [weak app] in app?.toneLc3() == true ? 1 : 0 }, pick: { [weak app] i in app?.setToneLc3(i == 1) })
            .enabledIf { [weak app] in app?.bleReady() ?? false }
        l.slider("Speaker volume", value: { [weak app] in
                     let v = app?.speakerVolume() ?? -1
                     return v < 0 ? "Unknown" : v == 0 ? "Off" : "\(v)%"
                 }, get: { [weak app] in Float(max(0, app?.speakerVolume() ?? 0)) },
                 set: { [weak app] v in app?.setSpeakerVolume(Int(v)) })
            .enabledIf { [weak app] in app?.bleReady() ?? false }
    }

    static func mic(_ app: MainViewController, _ l: SettingsList) {
        l.header("Microphone")
        l.choice("Capture", detail: { [weak app] in
                     needsBle(app, app?.micCodec() == nil ? "Record the gadget's mic on the phone" : "Saving to Documents/mic." + (app?.micCodec() ?? ""))
                 },
                 labels: { ["Off", "SBC", "LC3"] },
                 selected: { [weak app] in app?.micCodec() == nil ? 0 : app?.micCodec() == "lc3" ? 2 : 1 },
                 pick: { [weak app] i in app?.setMic(i == 0 ? nil : i == 1 ? "sbc" : "lc3") })
            .enabledIf { [weak app] in app?.bleReady() ?? false }
        l.info("Audio out") { [weak app] in String(format: "%.0f kb/s", app?.audioKbps() ?? 0) }
        l.info("Mic in") { [weak app] in String(format: "%.0f kb/s", app?.micKbps() ?? 0) }
    }
}
