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

import java.util.Locale;

/**
 * The Gadget tab: which gadget the app talks to (the device picker), how it
 * starts, what the gadget reports about itself and how it's decoding.
 */
final class GadgetTab {
    private GadgetTab() {}

    static void connection(MainActivity app, Ui.SettingsList l) {
        l.header("Connection");
        l.row(() -> "Gadget", app::connectionLine, () -> app.gadgetOff() ? "Not connected"
                : app.gadgetName() != null ? app.gadgetName() : "Choose", null, false, app::showPicker);
        l.row(() -> "Disconnect " + (app.gadgetName() != null ? app.gadgetName() : "the gadget"),
                () -> "Ends any stream, then stays disconnected until you connect or choose a gadget",
                null, null, false, app::disconnectGadget).showIf(() -> !app.gadgetOff());
        l.row(() -> "Connect " + app.lastGadget(), () -> "Connect to it again", null, null, false, app::connectLast)
                .showIf(() -> app.gadgetOff() && app.lastGadget() != null);
        l.row(() -> "Choose a gadget", () -> "The gadgets nearby, to connect to one", null, null, false, app::showPicker)
                .showIf(app::gadgetOff);
        l.row(() -> "Forget " + (app.gadgetOff() ? app.lastGadget() : app.gadgetName()) + "…",
                () -> "Disconnect, unpair it from this phone and forget it", null, null, false, app::forgetGadget)
                .showIf(() -> app.gadgetOff() ? app.lastGadget() != null : app.gadgetName() != null);
        l.toggle("Start automatically", () -> "Stream as soon as the gadget connects (over BLE, once it proves its key; over Wi-Fi, only the connection it is turned on for), without waiting for its "
                + "Phone camera view", app::autoStart, app::setAutoStart);
    }

    static void reports(MainActivity app, Ui.SettingsList l) {
        l.header("What the gadget reports");
        l.info("Display", () -> {
            VideoLink.Caps c = app.caps();
            return c == null ? "-" : c.dispW + "×" + c.dispH + (c.round ? " round" : "");
        });
        l.info("Pictures up to", () -> {
            VideoLink.Caps c = app.caps();
            return c == null ? "-" : String.format(Locale.US, "%d×%d, %d fps", c.maxW, c.maxH, c.maxFps);
        });
        l.info("Suggests", () -> {
            VideoLink.Caps c = app.caps();
            return c == null ? "-" : String.format(Locale.US, "%d×%d, %d fps, %d kb/s", c.sugW, c.sugH, c.sugFps,
                    c.sugKbps);
        });
        l.info("Decodes", () -> {
            VideoLink.Caps c = app.caps();
            return c == null ? "-" : (c.vp8 ? "H.264, VP8" : "H.264") + (c.maxTiles > 1 ? ", " + c.maxTiles + " bands" : "");
        });
        l.info("Buffer", () -> {
            VideoLink.Caps c = app.caps();
            return c == null ? "-" : (c.buffer / 1024) + " KB" + (c.maxPlayoutMs > 0 ? ", playout " + c.maxPlayoutMs
                    + " ms" : "");
        });
        l.info("Decoding now", app::decodeLine);
        l.row(() -> "Gadget log", app::deviceLine, null, null, false, null).showIf(() -> !app.deviceLine().isEmpty());
        l.row(() -> "Link", app::linkDetail, null, null, false, null);
    }

    static void about(MainActivity app, Ui.SettingsList l) {
        l.header("About");
        l.info("App", () -> "Muse Gadgets Test " + app.versionName());
        l.info("Package", app::getPackageName);
        l.footnote("Streams to Muse gadgets for testing: protocols/VIDEO.md. Logs on the MuseCam tag "
                + "(adb logcat -s MuseCam).");
    }
}
