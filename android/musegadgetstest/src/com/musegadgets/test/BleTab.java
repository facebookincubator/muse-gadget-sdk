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
package com.musegadgets.test;

import android.graphics.Path;
import android.graphics.RectF;

/**
 * musegadgets BLE on the screen (Tabs): the Gadget tab's link choice
 * (Wi-Fi or Bluetooth LE), and the Audio tab: a tone loop on the gadget's
 * speaker beside the video, its speaker volume, and capturing its mic.
 */
final class BleTab {
    private BleTab() {}

    /** A speaker: the Audio tab. */
    static final Icons.Glyph AUDIO = (c, p, s) -> {
        Path b = new Path();
        b.moveTo(s * 0.12f, s * 0.38f);
        b.lineTo(s * 0.28f, s * 0.38f);
        b.lineTo(s * 0.48f, s * 0.20f);
        b.lineTo(s * 0.48f, s * 0.80f);
        b.lineTo(s * 0.28f, s * 0.62f);
        b.lineTo(s * 0.12f, s * 0.62f);
        b.close();
        c.drawPath(b, p);
        c.drawArc(new RectF(s * 0.46f, s * 0.34f, s * 0.70f, s * 0.66f), -50, 100, false, p);
        c.drawArc(new RectF(s * 0.50f, s * 0.20f, s * 0.90f, s * 0.80f), -50, 100, false, p);
    };

    static void link(MainActivity app, Ui.SettingsList l) {
        l.header("Link");
        l.choice("Connect over", () -> app.ble() ? "musegadgets BLE: video, audio and the mic"
                        : "The local network: the phone and the gadget on the same Wi-Fi",
                () -> new String[] {"Wi-Fi", "Bluetooth LE"}, () -> app.ble() ? 1 : 0, i -> app.setBle(i == 1));
    }

    private static String needsBle(MainActivity app, String desc) {
        return !app.ble() ? "Bluetooth LE only: pick it on the Gadget tab" : !app.connected() ? "Not connected" : desc;
    }

    static void playback(MainActivity app, Ui.SettingsList l) {
        l.header("Playback");
        l.toggle("Tone", () -> needsBle(app, "A loop on the gadget's speaker, beside any video"), app::toneOn,
                app::setTone).enabledIf(app::bleReady);
        l.choice("Tone codec", () -> "What the tone is sent as", () -> new String[] {"SBC", "LC3"},
                () -> app.toneLc3() ? 1 : 0, i -> app.setToneLc3(i == 1)).enabledIf(app::bleReady);
        l.slider("Speaker volume", () -> app.speakerVolume() < 0 ? "Unknown" : app.speakerVolume() == 0 ? "Off" : app.speakerVolume() + "%", 100,
                () -> Math.max(0, app.speakerVolume()), app::setSpeakerVolume).enabledIf(app::bleReady);
    }

    static void mic(MainActivity app, Ui.SettingsList l) {
        l.header("Microphone");
        l.choice("Capture", () -> needsBle(app, app.micCodec() == null ? "Record the gadget's mic on the phone"
                        : "Saving to files/mic." + app.micCodec()),
                () -> new String[] {"Off", "SBC", "LC3"},
                () -> app.micCodec() == null ? 0 : "lc3".equals(app.micCodec()) ? 2 : 1,
                i -> app.setMic(i == 0 ? null : i == 1 ? "sbc" : "lc3")).enabledIf(app::bleReady);
        l.info("Audio out", () -> String.format(java.util.Locale.US, "%.0f kb/s", app.audioKbps()));
        l.info("Mic in", () -> String.format(java.util.Locale.US, "%.0f kb/s", app.micKbps()));
    }
}
