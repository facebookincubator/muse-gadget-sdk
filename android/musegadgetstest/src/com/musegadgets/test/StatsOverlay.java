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

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.RectF;
import android.graphics.Typeface;
import android.view.View;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;

/**
 * The stats over the preview: a translucent rounded panel. Compact shows
 * what's being sent and four key numbers; detailed adds the Link, Encoder,
 * Gadget and Latency sections, labels muted, values monospaced so they
 * don't jitter, problems (skipped or dropped frames, a held link, a slow
 * decoder, high latency) in the warning color. Tap the preview to switch.
 * MainActivity fills a Stats once a second; the logcat "stats:" line is
 * written there as before.
 */
final class StatsOverlay extends View {
    static final int COMPACT = 0, DETAILED = 1, OFF = 2;

    /** One second's numbers (MainActivity.tick()). */
    static final class Stats {
        boolean connected, streaming, preparing, clip;
        /* The "sending" line: what goes to the gadget. */
        String codec = "", source = "", encoder = "", linkName = "";
        int size, fps, bands = 1, targetKbps;
        boolean fixedRate;
        /* Link: its kind (VideoLink.kind()) and one line about it (VideoLink.describe()) */
        String linkKind = "", linkDetail = "";
        double sendKbps, gateMsPerS;
        long inFlight, queued;
        /* Encoder (the camera), or the clip player */
        double fedFps, encFps;
        int skipped, skippedNew, keys;
        /* Gadget */
        double shownFps;
        int dropped, droppedNew, decodeMs;
        long free, buffer;
        /* Latency */
        double camToEncMs, convertMs, encodeMs;
        String note = "";
        /* More lines from a link or source, by section ("LINK", "LATENCY", or a new one). */
        final java.util.LinkedHashMap<String, List<Item>> extra = new java.util.LinkedHashMap<>();

        void extra(String section, String label, String value, boolean bad) {
            extra.computeIfAbsent(section, k -> new ArrayList<>()).add(new Item(label, value, bad));
        }
    }

    private Stats s = new Stats();
    private int mode = COMPACT;
    private final float d;
    private final Paint panel = new Paint(Paint.ANTI_ALIAS_FLAG), label = new Paint(Paint.ANTI_ALIAS_FLAG),
            value = new Paint(Paint.ANTI_ALIAS_FLAG), head = new Paint(Paint.ANTI_ALIAS_FLAG),
            section = new Paint(Paint.ANTI_ALIAS_FLAG), dotP = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final RectF r = new RectF();

    StatsOverlay(Context c) {
        super(c);
        d = c.getResources().getDisplayMetrics().density;
        float sp = c.getResources().getDisplayMetrics().scaledDensity;
        panel.setColor(0xB8101216);
        label.setColor(0xFF8F96A0);
        label.setTextSize(11 * sp);
        label.setTypeface(Typeface.create("sans-serif", Typeface.NORMAL));
        value.setColor(Ui.TEXT);
        value.setTextSize(11.5f * sp);
        value.setTypeface(Typeface.create(Typeface.MONOSPACE, Typeface.NORMAL));
        head.setColor(Ui.SENDING);
        head.setTextSize(13 * sp);
        head.setTypeface(Typeface.create("sans-serif-medium", Typeface.NORMAL));
        section.setColor(0xFF7C8FB0);
        section.setTextSize(10 * sp);
        section.setLetterSpacing(0.12f);
        section.setTypeface(Typeface.create("sans-serif-medium", Typeface.BOLD));
    }

    void setMode(int m) {
        mode = m;
        invalidate();
    }

    int mode() {
        return mode;
    }

    void update(Stats st) {
        s = st;
        invalidate();
    }

    /* ---- what's shown --------------------------------------------------- */

    static final class Item {
        final String label, value;
        final boolean bad;

        Item(String label, String value, boolean bad) {
            this.label = label;
            this.value = value;
            this.bad = bad;
        }
    }

    private static String f1(double v) {
        return String.format(Locale.US, "%.1f", v);
    }

    private static String f0(double v) {
        return String.format(Locale.US, "%.0f", v);
    }

    /** Frames skipped or dropped in the last second, a slow gadget, a held link... */
    private boolean skipBad() {
        return s.skippedNew > 2;
    }

    private boolean shownBad() {
        return s.streaming && s.encFps > 5 && s.shownFps < s.encFps * 0.85;
    }

    private boolean decodeBad() {
        return s.decodeMs > 0 && s.fps > 0 && s.decodeMs * s.fps > 900;
    }

    /* The send gate holding camera frames (clips wait on it by design). */
    private boolean gateBad() {
        return !s.clip && s.gateMsPerS > 150;
    }

    private boolean latencyBad() {
        return !s.clip && s.camToEncMs > 150;
    }

    private String sendingTitle() {
        if (!s.connected) {
            return "Not connected";
        }
        if (s.preparing) {
            return "Preparing " + s.source;
        }
        if (!s.streaming) {
            return "Not sending";
        }
        return String.format(Locale.US, "%s %d×%d · %d fps%s", s.codec, s.size, s.size, s.fps,
                s.bands > 1 ? " · " + s.bands + " bands" : "");
    }

    private String sendingSub() {
        if (!s.streaming || s.preparing) {
            return s.connected ? s.linkName : "";
        }
        return String.format(Locale.US, "%d kb/s %s · link %s kb/s · %s", s.targetKbps, s.fixedRate ? "fixed" : "auto",
                f0(s.sendKbps), s.encoder.isEmpty() ? s.source : s.source + " · " + s.encoder);
    }

    private List<Item> compact() {
        List<Item> l = new ArrayList<>();
        l.add(new Item("LINK", f0(s.sendKbps) + " kb/s", gateBad()));
        l.add(new Item("GADGET", f1(s.shownFps) + " fps", shownBad()));
        l.add(new Item("DECODE", s.decodeMs + " ms", decodeBad()));
        if (s.clip) {
            l.add(new Item("DROPPED", Integer.toString(s.dropped), s.droppedNew > 0));
        } else {
            l.add(new Item("SKIPPED", s.skippedNew > 0 ? "+" + s.skippedNew : Integer.toString(s.skipped), skipBad()));
        }
        return l;
    }

    private List<List<Item>> sections(List<String> titles) {
        List<Item> link = new ArrayList<>();
        link.add(new Item("link", s.linkKind, false));
        link.add(new Item("sent", f0(s.sendKbps) + " kb/s", false));
        link.add(new Item("in flight", s.inFlight + " B", false));
        link.add(new Item("queued", s.queued + " B", s.queued > 16384));
        link.add(new Item("gate", f0(s.gateMsPerS) + " ms/s", gateBad()));
        List<Item> enc = new ArrayList<>();
        if (s.clip) {
            enc.add(new Item("frames", f1(s.encFps) + " fps", false));
            enc.add(new Item("target", s.targetKbps + " kb/s", false));
            enc.add(new Item("rate", s.fixedRate ? "fixed" : "auto", false));
        } else {
            enc.add(new Item("camera", f1(s.fedFps) + " fps", s.streaming && s.fedFps < s.fps * 0.8));
            enc.add(new Item("encoded", f1(s.encFps) + " fps", false));
            enc.add(new Item("skipped", s.skipped + (s.skippedNew > 0 ? " +" + s.skippedNew : ""), skipBad()));
            enc.add(new Item("keyframes", Integer.toString(s.keys), false));
        }
        enc.add(new Item("encoder", s.encoder.isEmpty() ? "-" : s.encoder, false));
        List<Item> gad = new ArrayList<>();
        gad.add(new Item("shown", f1(s.shownFps) + " fps", shownBad()));
        gad.add(new Item("dropped", s.dropped + (s.droppedNew > 0 ? " +" + s.droppedNew : ""), s.droppedNew > 0));
        gad.add(new Item("decode", s.decodeMs + " ms", decodeBad()));
        gad.add(new Item("free", s.free >= 1024 ? (s.free / 1024) + " KB" : s.free + " B",
                s.buffer > 0 && s.streaming && s.free < s.buffer / 8));
        List<Item> lat = new ArrayList<>();
        if (!s.clip) {
            lat.add(new Item("cam→enc", f0(s.camToEncMs) + " ms", latencyBad()));
            lat.add(new Item("convert", f1(s.convertMs) + " ms", false));
            lat.add(new Item("encode", f1(s.encodeMs) + " ms", false));
        }
        java.util.LinkedHashMap<String, List<Item>> all = new java.util.LinkedHashMap<>();
        all.put("LINK", link);
        all.put(s.clip ? "CLIP" : "ENCODER", enc);
        all.put("GADGET", gad);
        all.put("LATENCY", lat);
        for (java.util.Map.Entry<String, List<Item>> e : s.extra.entrySet()) {
            all.computeIfAbsent(e.getKey(), k -> new ArrayList<>()).addAll(e.getValue());
        }
        List<List<Item>> out = new ArrayList<>();
        for (java.util.Map.Entry<String, List<Item>> e : all.entrySet()) {
            if (!e.getValue().isEmpty()) {
                titles.add(e.getKey());
                out.add(e.getValue());
            }
        }
        return out;
    }

    /* ---- drawing -------------------------------------------------------- */

    private String[] notes() {
        String n = (s.linkDetail.isEmpty() ? "" : s.linkDetail) + (s.note.isEmpty() ? "" : "\n" + s.note);
        return n.trim().isEmpty() ? new String[0] : n.trim().split("\n");
    }

    /** The panel's height in this mode, unclipped. */
    private float panelHeight() {
        float pad = 12 * d, headH = head.getTextSize() * 1.25f, lineH = value.getTextSize() * 1.4f;
        float sub = sendingSub().isEmpty() ? 0 : lineH;
        if (mode == COMPACT) {
            return pad * 2 + headH + sub + (s.streaming ? 8 * d + 2 * lineH : 0);
        }
        List<String> titles = new ArrayList<>();
        List<List<Item>> secs = sections(titles);
        float body = 0;
        for (int i = 0; i < secs.size(); i += 2) {
            int n = Math.max(secs.get(i).size(), i + 1 < secs.size() ? secs.get(i + 1).size() : 0);
            body += (n + 1) * lineH + 10 * d;
        }
        return pad * 2 + headH + sub + 6 * d + body + notes().length * lineH;
    }

    /** The height this view needs to show the whole panel (its margins included). */
    int neededHeight() {
        return getVisibility() == VISIBLE ? (int) (panelHeight() + 20 * d) : 0;
    }

    @Override
    protected void onDraw(Canvas c) {
        float pad = 12 * d, margin = 10 * d;
        float w = getWidth() - 2 * margin;
        float x0 = margin + pad, y = margin + pad;
        float headH = head.getTextSize() * 1.25f, lineH = value.getTextSize() * 1.4f;
        String title = sendingTitle(), sub = sendingSub();
        int status = !s.connected ? Ui.TEXT3 : s.streaming && !s.preparing ? Ui.GOOD : s.preparing ? Ui.WARN : Ui.TEXT2;
        float height = Math.min(panelHeight(), getHeight() - 2 * margin);
        r.set(margin, margin, margin + w, margin + height);
        c.drawRoundRect(r, 14 * d, 14 * d, panel);
        /* status dot and the sending line */
        dotP.setColor(status);
        c.drawCircle(x0 + 4 * d, y + headH * 0.42f, 4 * d, dotP);
        head.setColor(s.streaming && !s.preparing ? Ui.SENDING : Ui.TEXT);
        float tx = x0 + 14 * d;
        c.drawText(ellipsize(title, head, w - 2 * pad - 14 * d), tx, y + head.getTextSize() * 0.85f, head);
        y += headH;
        if (!sub.isEmpty()) {
            label.setColor(0xFFC9CDD3);
            c.drawText(ellipsize(sub, label, w - 2 * pad - 14 * d), tx, y + label.getTextSize() * 0.95f, label);
            label.setColor(0xFF8F96A0);
            y += lineH;
        }
        if (mode == COMPACT) {
            if (s.streaming) {
                y += 8 * d;
                List<Item> items = compact();
                float cw = (w - 2 * pad) / items.size();
                for (int i = 0; i < items.size(); i++) {
                    Item it = items.get(i);
                    float cx = x0 + i * cw;
                    section.setColor(0xFF8F96A0);
                    c.drawText(it.label, cx, y + section.getTextSize(), section);
                    value.setColor(it.bad ? Ui.WARN : Ui.TEXT);
                    value.setTypeface(Typeface.create(Typeface.MONOSPACE, Typeface.BOLD));
                    c.drawText(ellipsize(it.value, value, cw - 4 * d), cx, y + lineH + value.getTextSize() * 0.9f, value);
                    value.setTypeface(Typeface.create(Typeface.MONOSPACE, Typeface.NORMAL));
                }
            }
            return;
        }
        y += 6 * d;
        List<String> titles = new ArrayList<>();
        List<List<Item>> secs = sections(titles);
        float colW = (w - 2 * pad - 16 * d) / 2;
        for (int row = 0; row < secs.size(); row += 2) {
            int n = 0;
            for (int col = 0; col < 2 && row + col < secs.size(); col++) {
                float cx = x0 + col * (colW + 16 * d);
                float yy = y;
                section.setColor(0xFF7C8FB0);
                c.drawText(titles.get(row + col), cx, yy + section.getTextSize(), section);
                yy += lineH;
                for (Item it : secs.get(row + col)) {
                    c.drawText(it.label, cx, yy + label.getTextSize(), label);
                    value.setColor(it.bad ? Ui.WARN : Ui.TEXT);
                    String v = ellipsize(it.value, value, colW - label.measureText(it.label) - 8 * d);
                    c.drawText(v, cx + colW - value.measureText(v), yy + label.getTextSize(), value);
                    yy += lineH;
                }
                n = Math.max(n, secs.get(row + col).size());
            }
            y += (n + 1) * lineH + 10 * d;
        }
        label.setColor(0xFF767D87);
        for (String n : notes()) {
            c.drawText(ellipsize(n, label, w - 2 * pad), x0, y + label.getTextSize() - 4 * d, label);
            y += lineH;
        }
        label.setColor(0xFF8F96A0);
    }

    private static String ellipsize(String s, Paint p, float max) {
        if (p.measureText(s) <= max) {
            return s;
        }
        int n = p.breakText(s, true, max - p.measureText("…"), null);
        return s.substring(0, Math.max(0, n)) + "…";
    }
}
