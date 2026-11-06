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

import android.graphics.Insets;
import android.graphics.Matrix;
import android.graphics.drawable.GradientDrawable;
import android.text.TextUtils;
import android.util.DisplayMetrics;
import android.view.Gravity;
import android.view.TextureView;
import android.view.View;
import android.view.WindowInsets;
import android.widget.FrameLayout;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.TextView;
import java.util.ArrayList;
import java.util.List;

/**
 * The screen: the app bar (its name, the gadget it's connected to), the
 * display (the preview of what's sent, the stats over it, and what's
 * playing with its Stop), the selected tab's list, and the tab bar.
 */
final class AppScreen {
    final LinearLayout root;
    final TextureView preview;
    final StatsOverlay overlay;
    private final MainActivity app;
    private final List<Tab> tabs;
    private final List<View> navItems = new ArrayList<>();
    private final List<Icons.GlyphDrawable> navIcons = new ArrayList<>();
    private final List<View> navDots = new ArrayList<>();
    private final FrameLayout content;
    private final View chipDot, nowDot;
    private final TextView chipText, nowTitle, nowSub, nowButton, placeholder;
    private final LinearLayout header, nav;
    private final FrameLayout placeholderBox, box;
    private final LinearLayout now;
    private final int side;
    private int selected = -1;

    AppScreen(MainActivity a) {
        app = a;
        root = new LinearLayout(a);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setBackgroundColor(Ui.BG);

        /* App bar */
        header = new LinearLayout(a);
        header.setOrientation(LinearLayout.HORIZONTAL);
        header.setGravity(Gravity.CENTER_VERTICAL);
        header.setPadding(dp(20), dp(8), dp(12), dp(8));
        TextView title = Ui.text(a, 20, Ui.TEXT, true);
        title.setText("Muse Gadgets Test");
        title.setSingleLine(true);
        title.setEllipsize(TextUtils.TruncateAt.END);
        header.addView(title, new LinearLayout.LayoutParams(0, -2, 1));
        LinearLayout chip = new LinearLayout(a);
        chip.setOrientation(LinearLayout.HORIZONTAL);
        chip.setGravity(Gravity.CENTER_VERTICAL);
        chip.setPadding(dp(12), dp(7), dp(14), dp(7));
        chip.setBackground(Ui.ripple(Ui.rounded(Ui.SURFACE_HI, dp(16)), null));
        chipDot = Ui.dot(a, Ui.TEXT3, 8);
        chip.addView(chipDot);
        chipText = Ui.text(a, 13, Ui.TEXT, false);
        chipText.setPadding(dp(8), 0, 0, 0);
        chipText.setSingleLine(true);
        chipText.setEllipsize(TextUtils.TruncateAt.END);
        chipText.setMaxWidth(dp(170));
        chip.addView(chipText);
        header.addView(chip);
        root.addView(header, new LinearLayout.LayoutParams(-1, -2));

        /* The display: preview, stats, what's playing */
        DisplayMetrics dm = a.getResources().getDisplayMetrics();
        side = Math.min(dm.widthPixels, (int) (dm.heightPixels * 0.36f));
        FrameLayout box = new FrameLayout(a);
        box.setBackgroundColor(0xFF000000);
        preview = new TextureView(a);
        box.addView(preview, Ui.frame(side, side, Gravity.TOP | Gravity.CENTER_HORIZONTAL));
        placeholderBox = new FrameLayout(a);
        placeholderBox.setBackgroundColor(0xFF050607);
        placeholder = Ui.text(a, 14, Ui.TEXT2, false);
        placeholder.setGravity(Gravity.CENTER);
        Icons.GlyphDrawable big = (Icons.GlyphDrawable) Icons.of(Icons.VIDEO, Ui.TEXT3);
        big.setBounds(0, 0, dp(44), dp(44));
        placeholder.setCompoundDrawables(null, big, null, null);
        placeholder.setCompoundDrawablePadding(dp(10));
        FrameLayout.LayoutParams pl = Ui.frame(-2, -2, Gravity.BOTTOM | Gravity.CENTER_HORIZONTAL);
        pl.bottomMargin = dp(72);   /* above what's playing, below the stats */
        placeholderBox.addView(placeholder, pl);
        box.addView(placeholderBox, Ui.frame(-1, -1, Gravity.CENTER));
        overlay = new StatsOverlay(a);
        box.addView(overlay, Ui.frame(-1, -1, Gravity.TOP));
        box.setOnClickListener(v -> app.setOverlayMode(overlay.mode() == StatsOverlay.COMPACT
                ? StatsOverlay.DETAILED : StatsOverlay.COMPACT));

        LinearLayout now = new LinearLayout(a);
        now.setOrientation(LinearLayout.HORIZONTAL);
        now.setGravity(Gravity.CENTER_VERTICAL);
        now.setPadding(dp(16), dp(22), dp(12), dp(10));
        now.setBackground(new GradientDrawable(GradientDrawable.Orientation.BOTTOM_TOP,
                new int[] {0xE6000000, 0x99000000, 0x00000000}));
        nowDot = Ui.dot(a, Ui.TEXT3, 10);
        now.addView(nowDot);
        LinearLayout nowTexts = new LinearLayout(a);
        nowTexts.setOrientation(LinearLayout.VERTICAL);
        nowTexts.setPadding(dp(10), 0, dp(8), 0);
        nowTitle = Ui.text(a, 15, Ui.TEXT, true);
        nowTitle.setSingleLine(true);
        nowTitle.setEllipsize(TextUtils.TruncateAt.END);
        nowSub = Ui.text(a, 12, 0xFFB8BDC4, false);
        nowSub.setPadding(0, dp(3), 0, 0);
        nowSub.setSingleLine(true);
        nowSub.setEllipsize(TextUtils.TruncateAt.END);
        nowTexts.addView(nowTitle);
        nowTexts.addView(nowSub);
        now.addView(nowTexts, new LinearLayout.LayoutParams(0, -2, 1));
        nowButton = Ui.pill(a, "Stop", 0xFF1A0D0D, Ui.BAD);
        now.addView(nowButton);
        box.addView(now, Ui.frame(-1, -2, Gravity.BOTTOM));
        root.addView(box, new LinearLayout.LayoutParams(-1, side));
        this.box = box;
        this.now = now;

        /* The tabs' lists */
        content = new FrameLayout(a);
        root.addView(content, new LinearLayout.LayoutParams(-1, 0, 1));
        tabs = Tabs.create(a);
        for (Tab t : tabs) {
            t.list = new Ui.SettingsList(a);
            for (Tab.Section s : t.sections) {
                s.build(a, t.list);
            }
            t.list.scroll.setVisibility(View.GONE);
            content.addView(t.list.scroll, Ui.frame(-1, -1, Gravity.TOP));
        }

        /* Tab bar */
        nav = new LinearLayout(a);
        nav.setOrientation(LinearLayout.HORIZONTAL);
        nav.setBackgroundColor(Ui.NAV);
        nav.setPadding(0, dp(8), 0, dp(10));
        for (int i = 0; i < tabs.size(); i++) {
            Tab t = tabs.get(i);
            LinearLayout item = new LinearLayout(a);
            item.setOrientation(LinearLayout.VERTICAL);
            item.setGravity(Gravity.CENTER_HORIZONTAL);
            FrameLayout pillBox = new FrameLayout(a);
            ImageView icon = new ImageView(a);
            Icons.GlyphDrawable g = (Icons.GlyphDrawable) Icons.of(t.icon, Ui.TEXT2);
            icon.setImageDrawable(g);
            icon.setPadding(dp(20), dp(4), dp(20), dp(4));
            pillBox.addView(icon, Ui.frame(dp(64), dp(32), Gravity.CENTER));
            View liveDot = Ui.dot(a, Ui.GOOD, 8);
            FrameLayout.LayoutParams dl = Ui.frame(dp(8), dp(8), Gravity.TOP | Gravity.END);
            dl.topMargin = dp(2);
            dl.rightMargin = dp(14);
            pillBox.addView(liveDot, dl);
            item.addView(pillBox, new LinearLayout.LayoutParams(dp(64), dp(32)));
            TextView label = Ui.text(a, 12, Ui.TEXT2, true);
            label.setText(t.title);
            label.setPadding(0, dp(4), 0, 0);
            label.setGravity(Gravity.CENTER_HORIZONTAL);
            item.addView(label);
            final int k = i;
            item.setOnClickListener(v -> select(k));
            item.setBackground(Ui.ripple(null, null));
            nav.addView(item, new LinearLayout.LayoutParams(0, -2, 1));
            navItems.add(item);
            navIcons.add(g);
            navDots.add(liveDot);
        }
        root.addView(nav, new LinearLayout.LayoutParams(-1, -2));
        chip.setOnClickListener(v -> select(tabIndex("Gadget")));

        /* Edge to edge: the app bar under the status bar, the tab bar over the navigation bar. */
        root.setOnApplyWindowInsetsListener((v, in) -> {
            Insets sb = in.getInsets(WindowInsets.Type.systemBars() | WindowInsets.Type.displayCutout());
            header.setPadding(dp(20) + sb.left, sb.top + dp(8), dp(12) + sb.right, dp(8));
            nav.setPadding(sb.left, dp(8), sb.right, sb.bottom + dp(10));
            return WindowInsets.CONSUMED;
        });
        select(0);
    }

    private int dp(float v) {
        return Ui.dp(app, v);
    }

    int tabIndex(String title) {
        for (int i = 0; i < tabs.size(); i++) {
            if (tabs.get(i).title.equals(title)) {
                return i;
            }
        }
        return 0;
    }

    void select(int k) {
        if (k == selected) {
            tabs.get(k).list.scroll.smoothScrollTo(0, 0);
            return;
        }
        selected = k;
        for (int i = 0; i < tabs.size(); i++) {
            boolean on = i == k;
            tabs.get(i).list.scroll.setVisibility(on ? View.VISIBLE : View.GONE);
            View item = navItems.get(i);
            View pill = ((LinearLayout) item).getChildAt(0);
            pill.setBackground(on ? Ui.rounded(Ui.ACCENT_BG, dp(16)) : null);
            navIcons.get(i).setColor(on ? Ui.ACCENT : Ui.TEXT2);
            ((TextView) ((LinearLayout) item).getChildAt(1)).setTextColor(on ? Ui.TEXT : Ui.TEXT2);
        }
        refresh();
    }

    /** Scrolls the tab to the section titled header (adb's "pick rate", say). */
    void show(String tab, String header) {
        select(tabIndex(tab));
        Ui.SettingsList l = tabs.get(selected).list;
        for (int i = 0; i < l.column.getChildCount(); i++) {
            View v = l.column.getChildAt(i);
            if (v instanceof TextView && header.contentEquals(((TextView) v).getText())) {
                l.scroll.post(() -> l.scroll.smoothScrollTo(0, v.getTop()));
            }
        }
    }

    /** The preview's picture: the camera (center square of an upright 4:3 picture) or a decoded stream. */
    void cameraTransform(boolean camera) {
        Matrix m = new Matrix();
        if (camera) {
            m.setScale(1f, 4f / 3f, side / 2f, side / 2f);
        }
        preview.setTransform(m);
    }

    /** Brings everything on screen up to date. */
    void refresh() {
        boolean connected = app.connected();
        String name = app.gadgetName();
        Ui.tintDot(chipDot, connected ? Ui.GOOD : Ui.WARN);
        String shortName = name != null && name.startsWith("MuseGadget-") ? name.substring(11) : name;
        chipText.setText(connected ? shortName : name != null ? "Connecting…" : "No gadget");

        boolean playing = app.streamingOrPreparing();
        if (!connected) {
            Ui.tintDot(nowDot, Ui.TEXT3);
            nowTitle.setText(name != null ? "Connecting to " + name : "Looking for a gadget");
            nowSub.setText(app.connectionLine());
            setButton("Choose", Ui.TEXT, Ui.SURFACE_HI, app::showPicker);
        } else if (playing) {
            Ui.tintDot(nowDot, app.preparing() ? Ui.WARN : Ui.GOOD);
            nowTitle.setText(app.sourceLabel() + (app.preparing() ? " · preparing" : ""));
            nowSub.setText("Video tab → " + name);
            setButton("Stop", 0xFF1A0D0D, Ui.BAD, () -> app.play("stop"));
        } else {
            Ui.tintDot(nowDot, Ui.ACCENT);
            nowTitle.setText("Ready: " + app.sourceLabel());
            nowSub.setText("Tap Start, or a source below");
            setButton("Start", 0xFF0B1A33, Ui.ACCENT, () -> app.play(app.source()));
        }
        overlay.setVisibility(overlay.mode() != StatsOverlay.OFF && playing ? View.VISIBLE : View.GONE);
        /* Detailed stats: the display grows to hold them above what's playing. */
        int h = Math.max(side, overlay.mode() == StatsOverlay.DETAILED ? overlay.neededHeight() + now.getHeight() - dp(24) : 0);
        if (box.getLayoutParams().height != h) {
            box.getLayoutParams().height = h;
            box.requestLayout();
        }
        boolean picture = playing && !app.preparing() && (app.cameraPreview() || app.decoderShown());
        placeholderBox.setVisibility(picture ? View.GONE : View.VISIBLE);
        /* The bars in two bands can't be decoded into one preview: show a test card instead. */
        boolean card = playing && app.sourceIs("pattern") && !picture;
        if (card != (placeholderBox.getTag() != null)) {
            placeholderBox.setTag(card ? Boolean.TRUE : null);
            placeholderBox.setBackground(card ? Icons.bars() : new android.graphics.drawable.ColorDrawable(0xFF050607));
            placeholder.setVisibility(card ? View.GONE : View.VISIBLE);
        }
        placeholder.setText(!connected ? "No gadget yet" : playing ? (app.preparing() ? "Preparing the clip…"
                : "Sending " + app.sourceLabel()) : "Not streaming");
        for (int i = 0; i < tabs.size(); i++) {
            navDots.get(i).setVisibility(tabs.get(i).live.getAsBoolean() ? View.VISIBLE : View.INVISIBLE);
        }
        if (selected >= 0) {
            tabs.get(selected).list.refresh();
        }
    }

    private void setButton(String label, int fg, int bg, Runnable r) {
        if (!label.contentEquals(nowButton.getText())) {
            nowButton.setText(label);
            nowButton.setTextColor(fg);
            nowButton.setBackground(Ui.ripple(Ui.rounded(bg, dp(20)), null));
        }
        nowButton.setOnClickListener(v -> {
            r.run();
            refresh();
        });
    }
}
