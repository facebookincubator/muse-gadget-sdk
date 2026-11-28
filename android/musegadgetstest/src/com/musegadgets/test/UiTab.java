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

import android.app.AlertDialog;
import android.bluetooth.BluetoothDevice;
import android.content.Intent;
import android.graphics.RectF;
import android.provider.Settings;
import android.text.InputType;
import android.widget.EditText;

import java.util.Locale;

/**
 * The UI tab (protocols/UI.md): stream a web page or a square of the screen
 * to the gadget at its panel's own size, with the fisheye, the gadget's
 * touch back, and its HID.
 */
final class UiTab {
    private UiTab() {}

    /** A phone with a window: the UI tab. */
    static final Icons.Glyph ICON = (c, p, s) -> {
        c.drawRoundRect(new RectF(s * 0.26f, s * 0.08f, s * 0.74f, s * 0.92f), s * 0.08f, s * 0.08f, p);
        c.drawRect(new RectF(s * 0.34f, s * 0.30f, s * 0.66f, s * 0.62f), p);
    };

    static String badge(MainActivity app, String what) {
        UiSession u = app.ui();
        return u != null && u.active() && what.equals(u.source()) ? "LIVE" : null;
    }

    static void sources(MainActivity app, Ui.SettingsList l) {
        l.header("Stream to the gadget");
        l.row(() -> "Web page", () -> "Rendered offscreen at the gadget's own size", null, null, true,
                () -> app.play(UiSession.WEB)).badge(() -> badge(app, UiSession.WEB));
        l.row(() -> "Screen mirror", () -> "A square of this phone's screen (asks for screen capture)", null, null, true,
                () -> app.play(UiSession.MIRROR)).badge(() -> badge(app, UiSession.MIRROR));
        l.row("Stop", () -> "Leave the gadget's last picture", null, () -> app.play("stop"))
                .showIf(() -> app.ui() != null && app.ui().active());
        l.footnote("Frames go only when the picture changes, at the gadget's panel size, then sharpen while it "
                + "holds still. Needs a gadget with UI streaming (it lists it on the Gadget tab).");
    }

    static void web(MainActivity app, Ui.SettingsList l) {
        UiSession u = app.ui();
        l.header("Web page");
        l.row("Address", () -> u.url, null, () -> askUrl(app));
        for (String[] d : UiSession.DEMOS) {
            l.row(d[0], () -> "Demo page", null, () -> {
                u.setUrl(d[1]);
                if (!u.active() || !UiSession.WEB.equals(u.source())) {
                    app.play(UiSession.WEB);
                }
            }).badge(() -> d[1].equals(u.url) ? "SHOWN" : null);
        }
        int[] css = {0, 240, 320, 360, 480, 640};
        l.choice("Page width", () -> "CSS pixels across the gadget (Gadget: one CSS pixel per panel pixel)",
                () -> new String[] {"Gadget", "240 px", "320 px", "360 px", "480 px", "640 px"},
                () -> {
                    for (int i = 0; i < css.length; i++) {
                        if (css[i] == u.cssPx) {
                            return i;
                        }
                    }
                    return 0;
                }, i -> u.setCss(css[i]));
    }

    static void mirror(MainActivity app, Ui.SettingsList l) {
        UiSession u = app.ui();
        l.header("Mirror window");
        l.slider("Left", () -> Math.round(u.winX * 100) + "%", 100, () -> (int) Math.round(u.winX * 100),
                v -> u.setWindow(v / 100.0, u.winY, u.winSize));
        l.slider("Top", () -> Math.round(u.winY * 100) + "%", 100, () -> (int) Math.round(u.winY * 100),
                v -> u.setWindow(u.winX, v / 100.0, u.winSize));
        l.slider("Size", () -> Math.round(u.winSize * 100) + "% of the width", 100,
                () -> (int) Math.round(u.winSize * 100), v -> u.setWindow(u.winX, u.winY, v / 100.0));
        l.footnote("The square of the screen the gadget shows: its left and top edges as a share of the screen, and "
                + "its side as a share of the screen's width.");
        l.row("Proof key", () -> {
            String g = app.proofGadget();
            if (g == null) {
                return "Connect to the gadget over BLE first";
            }
            String fp = ProofKeys.fingerprint(app, g);
            return fp != null ? "Key " + fp + " for " + g : "None for " + g + ": no screen mirror to it. Set it up with "
                    + "mg_ble_client, then paste its key";
        }, null, () -> askProofKey(app));
        l.info("Token proof", app::proofLine);
        l.footnote("The screen and touches go only to a gadget that proves, on each connection, that it holds this "
                + "phone's proof key for it (the token proof): a nearby device that only copies its name gets nothing. "
                + "The link isn't encrypted, so a device in range that relays the real gadget, or injects into the link, "
                + "can still see the mirror and send touches and Back/Home/Recents: mirror only where that's acceptable.");
    }

    private static void askProofKey(MainActivity app) {
        String g = app.proofGadget();
        if (g == null) {
            new AlertDialog.Builder(app).setTitle("Pick a gadget first")
                    .setMessage("Connect to the gadget over BLE, then set its key.").setPositiveButton("OK", null).show();
            return;
        }
        EditText e = new EditText(app);
        e.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_PASSWORD | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS);
        e.setHint("64 hex digits");
        new AlertDialog.Builder(app).setTitle("Proof key for " + g)
                .setMessage("After esp32/tools/mg_ble_client.py setup, paste proof_key from "
                        + "~/.mg_ble_client/proof-<address>.json. Kept in this phone's Keystore only. Empty removes it.")
                .setView(e)
                .setPositiveButton("Save", (d, w) -> {
                    if (!app.setProofKey(g, e.getText().toString())) {
                        new AlertDialog.Builder(app).setTitle("Not a proof key").setMessage("A proof key is 64 hex digits.")
                                .setPositiveButton("OK", null).show();
                    }
                })
                .setNegativeButton("Cancel", null).show();
    }

    static void picture(MainActivity app, Ui.SettingsList l) {
        UiSession u = app.ui();
        l.header("Picture");
        l.toggle("Fisheye", () -> "Fit the whole square onto the round panel (else its corners are cut off)",
                () -> u.fisheye, u::setFisheye).showIf(u::round);
        l.toggle("Sharpen when still", () -> "Resend the picture finer 0.3 s and 1 s after a change",
                () -> u.refine, u::setRefine);
        l.toggle("Sharp quality", () -> "A lossless pass 0.6 s after a change (or one more, finer refinement on gadgets without it)",
                () -> u.sharp, u::setSharp);
        l.toggle("Pan mode", () -> "Drags on the gadget move the mirror window (or drag from its edge ring any time)",
                u::pan, u::setPan).showIf(() -> UiSession.MIRROR.equals(u.source()));
        l.choice("Overscroll", () -> "How far the mirror window pans past the screen (by up to half its size): not at all, past the top and bottom, or past every edge",
                () -> UiSession.OVERSCROLL_NAMES, () -> u.overscroll, u::setOverscroll)
                .showIf(() -> UiSession.MIRROR.equals(u.source()));
        l.footnote("On the gadget, its menu button (the StopWatch's blue button) opens a menu with these settings.");
        l.info("Size", () -> u.active() ? u.outW() + " px" : "-");
        l.info("Frames", () -> u.streamer() != null && u.active() ? u.streamer().describe() : "-");
    }

    static void touch(MainActivity app, Ui.SettingsList l) {
        UiSession u = app.ui();
        l.header("Touch back");
        l.toggle("Gadget touch", () -> app.caps() != null && app.caps().touch
                        ? "Touches on the gadget drive the page (or, with the touch service, the screen)"
                        : "This gadget sends no touch events",
                () -> u.touchBack, u::setTouchBack);
        l.row("Touch service", () -> UiTouchService.enabled() ? "On: the mirror's touches reach any app"
                        : "Off: turn it on to drive the mirrored screen without HID",
                null, () -> app.startActivity(new Intent(Settings.ACTION_ACCESSIBILITY_SETTINGS)));
        l.info("Last touch", () -> u.touches == 0 ? "-" : String.format(Locale.US, "%d, %d · %s (%d)", u.lastTouchX,
                u.lastTouchY, u.lastTouchWhere, u.touches));
    }

    static void hid(MainActivity app, Ui.SettingsList l) {
        UiSession u = app.ui();
        l.header("HID (the gadget as a touchscreen)");
        l.choice("Mode", () -> "What the gadget's touches are to this phone, over Bluetooth HID (mirror only)",
                () -> new String[] {"Off", "Touchscreen", "Absolute pointer", "Relative pointer"},
                () -> u.hidMode, u::setHidMode);
        l.row("Pair", () -> {
            int b = app.bondState();
            return b == BluetoothDevice.BOND_BONDED ? "Paired with this phone"
                    : b == BluetoothDevice.BOND_BONDING ? "Pairing..." : "Not paired: HID needs pairing (tap)";
        }, null, app::bond);
        l.info("Gadget HID", () -> {
            if (app.caps() == null || !(app.caps().hidDigitizer || app.caps().hidMouseAbs || app.caps().hidMouseRel)) {
                return "not offered";
            }
            String m = new String[] {"off", "touchscreen", "absolute pointer", "relative pointer"}[Math.min(3, u.hidStatusMode)];
            return m + ((u.hidStatusFlags & Mg.HID_STATUS_BONDED) != 0 ? ", bonded" : "")
                    + ((u.hidStatusFlags & Mg.HID_STATUS_HOST) != 0 ? ", host connected" : ", no host");
        });
    }

    private static void askUrl(MainActivity app) {
        EditText e = new EditText(app);
        e.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_URI);
        e.setText(app.ui().url);
        new AlertDialog.Builder(app).setTitle("Web page").setView(e)
                .setPositiveButton("Show", (d, w) -> {
                    String s = e.getText().toString().trim();
                    if (!s.isEmpty()) {
                        if (!s.contains("://")) {
                            s = "https://" + s;
                        }
                        app.ui().setUrl(s);
                        app.refreshUi();
                    }
                })
                .setNegativeButton("Cancel", null).show();
    }
}
