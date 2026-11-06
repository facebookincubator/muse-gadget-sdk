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
 * The Video tab: what streams (the camera, color bars, the sample clips;
 * a tap starts it, or switches to it), then the camera, the encoding
 * (codec, picture size, bitrates, bands) and the advanced options.
 */
enum VideoTab {
    /* The badge on the source that's streaming: LIVE / PLAYING, or "!PREPARING" while a clip is encoded. */
    static func badge(_ app: MainViewController, _ what: String) -> String? {
        guard app.sourceIs(what), app.streamingOrPreparing() else { return nil }
        if app.preparing() { return "!PREPARING" }
        return what.hasPrefix(MainViewController.clipPrefix) ? "PLAYING" : "LIVE"
    }

    static let placeholder = UIImage(systemName: "film")

    /* A clip row: thumbnail, name, what it is and how big, and its state. */
    static func clipRow(_ app: MainViewController, _ l: SettingsList, name: String, title: String, about: String,
                        thumbAt: Double, asset: @escaping () -> AVAsset?) {
        let what = MainViewController.clipPrefix + name
        l.row({ title }, detail: { [weak app] in about + (app?.clipState(name) ?? "") }, value: nil,
              image: { Thumbs.get(name, at: thumbAt, asset: asset) ?? placeholder }, bigImage: true,
              action: { [weak app] in app?.play(what) })
            .withBadge { [weak app] in app.flatMap { badge($0, what) } }
    }

    static func sources(_ app: MainViewController, _ l: SettingsList) {
        l.header("Source")
        let cam = UIImage(systemName: "camera")
        l.row({ "Camera" }, detail: { [weak app] in "Live from the phone's " + (app?.frontCamera() == true ? "front" : "back") + " camera" },
              image: { cam }, bigImage: true, action: { [weak app] in app?.play("camera") })
            .withBadge { [weak app] in app.flatMap { badge($0, "camera") } }
        l.row({ "Color bars" }, detail: { "A test pattern, encoded live like the camera" }, image: { Thumbs.bars },
              bigImage: true, action: { [weak app] in app?.play("pattern") })
            .withBadge { [weak app] in app.flatMap { badge($0, "pattern") } }
        for s in SampleClips.all {
            let credit = s.credit.components(separatedBy: ", ")
            let about = "\(credit.first ?? s.label) · \(credit.last ?? "") · \(s.seconds) s at \(s.fps) fps"
            clipRow(app, l, name: s.name, title: s.label, about: about, thumbAt: Double(s.startS + 2)) {
                URL(string: s.url).map { AVURLAsset(url: $0) }
            }
        }
        l.footnote("Tap a source to stream it. Sample clips download a few seconds of the film the first time they play, "
                   + "then encode for the gadget.")
    }

    static func camera(_ app: MainViewController, _ l: SettingsList) {
        l.header("Camera")
        l.choice("Lens", detail: { "Which camera streams" }, labels: { ["Back", "Front"] },
                 selected: { [weak app] in app?.frontCamera() == true ? 1 : 0 }, pick: { [weak app] i in app?.setFront(i == 1) })
        l.choice("Frame rate", detail: { [weak app] in app?.fpsSetting() == 24 ? "24 fps puts a quarter more bits in each frame"
                     : "30 fps moves most smoothly" },
                 labels: { ["30 fps", "24 fps"] }, selected: { [weak app] in app?.fpsSetting() == 24 ? 1 : 0 },
                 pick: { [weak app] i in app?.setFps(i == 1 ? 24 : 30) })
    }

    /* Sizes the gadget takes (0: the gadget's suggestion), with their labels. */
    static func sizes(_ app: MainViewController) -> [(Int, String)] {
        let c = app.caps()
        let maxSize = c.map { min($0.maxW, $0.maxH) } ?? 240
        var v: [(Int, String)] = [(0, c.map { "Auto (\($0.sugW) px)" } ?? "Auto")]
        for k in MainViewController.sizes where k <= maxSize {
            v.append((k, "\(k) px"))
        }
        return v
    }

    static func encoding(_ app: MainViewController, _ l: SettingsList) {
        l.header("Encoding")
        l.choice("Codec", detail: { [weak app] in app?.codecNote() ?? "" }, labels: { ["H.264", "VP8"] },
                 selected: { [weak app] in app?.vp8Wanted() == true ? 1 : 0 }, pick: { [weak app] i in app?.setCodec(i == 1) })
        l.choice("Picture size", detail: { "What the gadget gets; it scales to its panel" },
                 labels: { [weak app] in app.map { sizes($0).map { $0.1 } } ?? [] },
                 selected: { [weak app] in app.flatMap { a in sizes(a).firstIndex { $0.0 == a.sizeSetting() } } ?? 0 },
                 pick: { [weak app] i in if let a = app { a.setSize(sizes(a)[i].0) } })
        l.choice("Camera bitrate", detail: { [weak app] in
                     guard let a = app else { return "" }
                     return a.camKbpsFixed() > 0 ? "Fixed: no adaptation to the link" : "Adapts to the link, up to \(a.kbpsCeiling()) kb/s"
                 },
                 labels: { ["Auto"] + MainViewController.camRates.map { "\($0) kb/s" } },
                 selected: { [weak app] in app.flatMap { a in MainViewController.camRates.firstIndex(of: a.camKbpsFixed()).map { $0 + 1 } } ?? 0 },
                 pick: { [weak app] i in app?.setCamRate(i == 0 ? 0 : MainViewController.camRates[i - 1]) })
        l.choice("Clip bitrate", detail: { [weak app] in app?.clipKbpsFixed() ?? 0 > 0 ? "Fixed" : "Steps down when the link falls behind" },
                 labels: { [weak app] in ["Auto"] + (app?.clipLevels() ?? []).map { "\($0) kb/s" } },
                 selected: { [weak app] in app.flatMap { a in a.clipLevels().firstIndex(of: a.clipKbpsFixed()).map { $0 + 1 } } ?? 0 },
                 pick: { [weak app] i in if let a = app { a.setClipRate(i == 0 ? 0 : a.clipLevels()[i - 1]) } })
        l.toggle("Two bands", detail: { "Past 240 px, send the picture as two streams the gadget decodes in parallel (x264 or VP8)" },
                 isOn: { [weak app] in app?.twoBands() ?? true }, set: { [weak app] on in app?.setTwoBands(on) })
            .showIf { [weak app] in app?.caps().map { $0.maxTiles >= 2 } ?? true }
    }

    static func advanced(_ app: MainViewController, _ l: SettingsList) {
        l.header("Advanced")
        l.choice("H.264 encoder", detail: { X264.available ? "x264 gives 0.4-2 dB more at the same bitrate"
                     : "This build has no x264 (native/build_x264.sh)" },
                 labels: { X264.available ? ["x264", "Phone's hardware"] : ["Phone's hardware"] },
                 selected: { [weak app] in X264.available && app?.useX264() == false ? 1 : 0 },
                 pick: { [weak app] i in app?.setUseX264(X264.available && i == 0) })
            .enabledIf { X264.available }
        l.toggle("Panel filter", detail: { "Sharpen for the gadget's upscaling" },
                 isOn: { [weak app] in app?.panelFilter() ?? true }, set: { [weak app] on in app?.setPanelFilter(on) })
        l.choice("Stats overlay", detail: { "Or tap the preview" }, labels: { ["Compact", "Detailed", "Off"] },
                 selected: { [weak app] in app?.overlayMode().rawValue ?? 0 },
                 pick: { [weak app] i in app?.setOverlayMode(StatsOverlay.Mode(rawValue: i) ?? .compact) })
    }
}
