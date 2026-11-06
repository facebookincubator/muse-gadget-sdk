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

import android.graphics.drawable.Drawable;
import java.util.Locale;

/**
 * The Video tab: what streams (the camera, color bars, the sample clips;
 * a tap starts it, or switches to it), then the camera, the encoding
 * (codec, picture size, bitrates, bands) and the advanced options.
 */
final class VideoTab {
    private VideoTab() {}

    /** The badge on the source that's streaming: LIVE / PLAYING, or "!PREPARING" while a clip is encoded. */
    static String badge(MainActivity app, String what) {
        if (!app.sourceIs(what) || !app.streamingOrPreparing()) {
            return null;
        }
        if (app.preparing()) {
            return "!PREPARING";
        }
        return what.startsWith(MainActivity.CLIP) ? "PLAYING" : "LIVE";
    }

    /** A clip row: thumbnail, name, what it is and how big, and its state. */
    static void clipRow(MainActivity app, Ui.SettingsList l, String name, String title, String about,
                        Thumbs.Source thumb, long thumbAtUs) {
        String what = MainActivity.CLIP + name;
        Drawable placeholder = Icons.of(Icons.VIDEO, Ui.TEXT3);
        ((Icons.GlyphDrawable) placeholder).pad(0.25f);
        l.row(() -> title, () -> about + app.clipState(name), null, () -> {
            Drawable d = Thumbs.get(app, name, thumbAtUs, thumb);
            return d != null ? d : placeholder;
        }, true, () -> app.play(what)).badge(() -> badge(app, what));
    }

    static void sources(MainActivity app, Ui.SettingsList l) {
        l.header("Source");
        Drawable cam = Icons.of(Icons.CAMERA, Ui.ACCENT);
        ((Icons.GlyphDrawable) cam).pad(0.2f);
        l.row(() -> "Camera", () -> "Live from the phone's " + (app.frontCamera() ? "front" : "back") + " camera",
                null, () -> cam, true, () -> app.play("camera")).badge(() -> badge(app, "camera"));
        Drawable bars = Icons.bars();
        l.row(() -> "Color bars", () -> "A test pattern, encoded live like the camera", null, () -> bars, true,
                () -> app.play("pattern")).badge(() -> badge(app, "pattern"));
        for (SampleClips.Sample s : SampleClips.ALL) {
            String[] credit = s.credit.split(", ");
            String about = String.format(Locale.US, "%s · %s · %d s at %d fps", credit[0], credit[credit.length - 1],
                    s.seconds, s.fps);
            clipRow(app, l, s.name, s.label, about, r -> r.setDataSource(s.url,
                    java.util.Collections.singletonMap("User-Agent", "MuseGadgetsTest")), (s.startS + 2) * 1_000_000L);
        }
        l.footnote("Tap a source to stream it. Sample clips download a few seconds of the film the first time "
                + "they play, then encode for the gadget.");
    }

    static void camera(MainActivity app, Ui.SettingsList l) {
        l.header("Camera");
        l.choice("Lens", () -> "Which camera streams", () -> new String[] {"Back", "Front"},
                () -> app.frontCamera() ? 1 : 0, i -> app.setFront(i == 1));
        l.choice("Frame rate", () -> app.fps() == 24 ? "24 fps puts a quarter more bits in each frame"
                        : "30 fps moves most smoothly",
                () -> new String[] {"30 fps", "24 fps"}, () -> app.fps() == 24 ? 1 : 0, i -> app.setFps(i == 1 ? 24 : 30));
    }

    static void encoding(MainActivity app, Ui.SettingsList l) {
        l.header("Encoding");
        l.choice("Codec", app::codecNote, () -> new String[] {"H.264", "VP8"}, () -> app.wantVp8() ? 1 : 0,
                i -> app.setCodec(i == 1));
        l.choice("Picture size", () -> "What the gadget gets; it scales to its panel", () -> sizes(app, true),
                () -> {
                    String[] v = sizes(app, false);
                    for (int i = 0; i < v.length; i++) {
                        if (Integer.parseInt(v[i]) == app.size()) {
                            return i;
                        }
                    }
                    return 0;
                }, i -> app.setSize(Integer.parseInt(sizes(app, false)[i])));
        l.choice("Camera bitrate", () -> app.fixedCamKbps() > 0 ? "Fixed: no adaptation to the link"
                        : "Adapts to the link, up to " + app.maxKbps() + " kb/s",
                () -> {
                    String[] v = new String[MainActivity.CAM_RATES.length + 1];
                    v[0] = "Auto";
                    for (int i = 0; i < MainActivity.CAM_RATES.length; i++) {
                        v[i + 1] = MainActivity.CAM_RATES[i] + " kb/s";
                    }
                    return v;
                }, () -> {
                    for (int i = 0; i < MainActivity.CAM_RATES.length; i++) {
                        if (MainActivity.CAM_RATES[i] == app.fixedCamKbps()) {
                            return i + 1;
                        }
                    }
                    return 0;
                }, i -> app.setCamRate(i == 0 ? 0 : MainActivity.CAM_RATES[i - 1]));
        l.choice("Clip bitrate", () -> app.fixedClipKbps() > 0 ? "Fixed" : "Steps down when the link falls behind",
                () -> {
                    int[] lv = app.clipLevels();
                    String[] v = new String[lv.length + 1];
                    v[0] = "Auto";
                    for (int i = 0; i < lv.length; i++) {
                        v[i + 1] = lv[i] + " kb/s";
                    }
                    return v;
                }, () -> {
                    int[] lv = app.clipLevels();
                    for (int i = 0; i < lv.length; i++) {
                        if (lv[i] == app.fixedClipKbps()) {
                            return i + 1;
                        }
                    }
                    return 0;
                }, i -> app.setClipRate(i == 0 ? 0 : app.clipLevels()[i - 1]));
        l.toggle("Two bands", () -> "Past 240 px, send the picture as two streams the gadget decodes in parallel"
                        + " (x264 or VP8)", app::twoBands, app::setTwoBands)
                .showIf(() -> app.caps() == null || app.caps().maxTiles >= 2);
    }

    /* Sizes the gadget takes: labels, or the values (0: the gadget's suggestion). */
    static String[] sizes(MainActivity app, boolean labels) {
        VideoLink.Caps c = app.caps();
        int max = c != null ? Math.min(c.maxW, c.maxH) : 240;
        java.util.List<String> v = new java.util.ArrayList<>();
        v.add(labels ? (c != null ? "Auto (" + c.sugW + " px)" : "Auto") : "0");
        for (int k : MainActivity.SIZES) {
            if (k <= max) {
                v.add(labels ? k + " px" : Integer.toString(k));
            }
        }
        return v.toArray(new String[0]);
    }

    static void advanced(MainActivity app, Ui.SettingsList l) {
        l.header("Advanced");
        l.choice("H.264 encoder", () -> app.x264Available() ? "x264 gives 0.4-2 dB more at the same bitrate"
                        : "This build has no x264 (jni/build.sh)",
                () -> app.x264Available() ? new String[] {"x264", "Phone's hardware"} : new String[] {"Phone's hardware"},
                () -> app.x264Available() && !app.useX264() ? 1 : 0,
                i -> app.setUseX264(app.x264Available() && i == 0)).enabledIf(app::x264Available);
        l.toggle("Intra refresh", () -> "Refresh the picture in stripes instead of periodic keyframes",
                app::intraRefresh, app::setIntraRefresh);
        l.toggle("Panel filter", () -> "Sharpen for the gadget's upscaling", app::panelFilter, app::setPanelFilter);
        l.choice("Stats overlay", () -> "Or tap the preview", () -> new String[] {"Compact", "Detailed", "Off"},
                app::overlayMode, app::setOverlayMode);
    }
}
