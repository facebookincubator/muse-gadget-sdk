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
import android.content.Context;
import android.content.res.ColorStateList;
import android.graphics.Typeface;
import android.graphics.drawable.Drawable;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.RippleDrawable;
import android.text.TextUtils;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.FrameLayout;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.SeekBar;
import android.widget.Switch;
import android.widget.TextView;
import java.util.ArrayList;
import java.util.List;
import java.util.function.BooleanSupplier;
import java.util.function.Consumer;
import java.util.function.IntConsumer;
import java.util.function.IntSupplier;
import java.util.function.Supplier;

/**
 * The app's look (a dark palette, type sizes) and its settings-style list:
 * section headers over rounded cards of rows, each row a title, a line of
 * description and its current value, a switch, a picker or a slider. Rows
 * read their text from suppliers, so refresh() brings the whole list up to
 * date (once a second, and after every change). Framework widgets only.
 */
final class Ui {
    private Ui() {}

    static final int BG = 0xFF0E1013, SURFACE = 0xFF1A1D22, SURFACE_HI = 0xFF242830, NAV = 0xFF15181C;
    static final int DIVIDER = 0x14FFFFFF;
    static final int TEXT = 0xFFE8EAED, TEXT2 = 0xFF9AA0A8, TEXT3 = 0xFF666C75;
    static final int ACCENT = 0xFF8AB4F8, ACCENT_BG = 0x338AB4F8;
    static final int GOOD = 0xFF5BD68A, WARN = 0xFFFFB454, BAD = 0xFFFF6B6B, SENDING = 0xFFFFD54F;
    static final int DIALOG_THEME = android.R.style.Theme_DeviceDefault_Dialog_Alert;

    static int dp(Context c, float v) {
        return Math.round(v * c.getResources().getDisplayMetrics().density);
    }

    static GradientDrawable rounded(int color, float radiusPx) {
        GradientDrawable d = new GradientDrawable();
        d.setColor(color);
        d.setCornerRadius(radiusPx);
        return d;
    }

    /** The touch ripple over content (null: a plain background), clipped to mask. */
    static Drawable ripple(Drawable content, Drawable mask) {
        return new RippleDrawable(ColorStateList.valueOf(0x22FFFFFF), content, mask);
    }

    static TextView text(Context c, float sp, int color, boolean bold) {
        TextView t = new TextView(c);
        t.setTextSize(sp);
        t.setTextColor(color);
        t.setIncludeFontPadding(false);
        if (bold) {
            t.setTypeface(Typeface.create("sans-serif-medium", Typeface.NORMAL));
        }
        return t;
    }

    /** A single-choice picker; pick gets the index chosen. */
    static void choose(Context c, String title, String[] labels, int selected, IntConsumer pick) {
        new AlertDialog.Builder(c, DIALOG_THEME)
                .setTitle(title)
                .setSingleChoiceItems(labels, selected, (d, which) -> {
                    d.dismiss();
                    pick.accept(which);
                })
                .setNegativeButton("Cancel", null)
                .show();
    }

    /** One row of a list; the suppliers it was made with are read on refresh(). */
    static final class Row {
        final View view;
        TextView title, desc, value;
        ImageView icon;
        Switch toggle;
        TextView badge;
        SeekBar slider;
        Supplier<String> titleFn, descFn, valueFn, badgeFn;
        Supplier<Drawable> iconFn;
        BooleanSupplier onFn, enabledFn, visibleFn;
        IntSupplier sliderFn;
        View divider;
        private Drawable shownIcon;

        Row(View view) {
            this.view = view;
        }

        /** Shown only while this holds. */
        Row showIf(BooleanSupplier f) {
            visibleFn = f;
            return this;
        }

        /** Grayed out and untouchable unless this holds. */
        Row enabledIf(BooleanSupplier f) {
            enabledFn = f;
            return this;
        }

        /** A small colored label on the right (e.g. "LIVE"), shown while f returns text ("!TEXT": a warning). */
        Row badge(Supplier<String> f) {
            badgeFn = f;
            return this;
        }

        void refresh() {
            boolean vis = visibleFn == null || visibleFn.getAsBoolean();
            view.setVisibility(vis ? View.VISIBLE : View.GONE);
            if (divider != null) {
                divider.setVisibility(vis ? View.VISIBLE : View.GONE);
            }
            if (!vis) {
                return;
            }
            boolean en = enabledFn == null || enabledFn.getAsBoolean();
            view.setEnabled(en);
            view.setAlpha(en ? 1f : 0.45f);
            if (toggle != null) {
                toggle.setEnabled(en);
            }
            if (slider != null) {
                slider.setEnabled(en);
            }
            set(title, titleFn);
            set(desc, descFn);
            set(value, valueFn);
            if (badge != null) {
                String b = badgeFn != null ? badgeFn.get() : null;
                badge.setVisibility(b == null ? View.GONE : View.VISIBLE);
                boolean warn = b != null && b.startsWith("!");   /* "!TEXT": in the warning color */
                if (warn) {
                    b = b.substring(1);
                }
                if (b != null && !b.contentEquals(badge.getText())) {
                    badge.setText(b);
                    ((GradientDrawable) badge.getBackground()).setColor(warn ? WARN : GOOD);
                }
            }
            if (toggle != null && onFn != null && toggle.isChecked() != onFn.getAsBoolean()) {
                toggle.setTag(Boolean.TRUE);   /* not the person: don't act on it */
                toggle.setChecked(onFn.getAsBoolean());
                toggle.setTag(null);
            }
            if (slider != null && sliderFn != null && !slider.isPressed()) {
                slider.setProgress(sliderFn.getAsInt());
            }
            if (icon != null && iconFn != null) {
                Drawable d = iconFn.get();
                if (d != shownIcon) {
                    shownIcon = d;
                    icon.setImageDrawable(d);
                }
            }
        }

        private static void set(TextView t, Supplier<String> f) {
            if (t == null || f == null) {
                return;
            }
            String s = f.get();
            t.setVisibility(s == null || s.isEmpty() ? View.GONE : View.VISIBLE);
            if (s != null && !s.contentEquals(t.getText())) {
                t.setText(s);
            }
        }
    }

    /**
     * A scrolling list of sections. header() starts a section (its title
     * over a new card); the row builders add to the current card.
     */
    static final class SettingsList {
        final Context c;
        final ScrollView scroll;
        final LinearLayout column;
        private LinearLayout card;
        private final List<Row> rows = new ArrayList<>();

        SettingsList(Context c) {
            this.c = c;
            scroll = new ScrollView(c);
            scroll.setFillViewport(true);
            scroll.setVerticalScrollBarEnabled(false);
            scroll.setOverScrollMode(View.OVER_SCROLL_IF_CONTENT_SCROLLS);
            column = new LinearLayout(c);
            column.setOrientation(LinearLayout.VERTICAL);
            column.setPadding(dp(c, 16), dp(c, 4), dp(c, 16), dp(c, 24));
            scroll.addView(column, new ViewGroup.LayoutParams(-1, -2));
        }

        /** A section: its title, then a card the next rows go into. */
        SettingsList header(String title) {
            TextView h = text(c, 13, ACCENT, true);
            h.setText(title);
            h.setLetterSpacing(0.04f);
            h.setPadding(dp(c, 16), dp(c, 18), dp(c, 16), dp(c, 8));
            column.addView(h);
            card = new LinearLayout(c);
            card.setOrientation(LinearLayout.VERTICAL);
            card.setBackground(rounded(SURFACE, dp(c, 20)));
            card.setClipToOutline(true);
            column.addView(card, new LinearLayout.LayoutParams(-1, -2));
            return this;
        }

        /** A line of small print under the current card. */
        SettingsList footnote(String s) {
            TextView t = text(c, 12, TEXT3, false);
            t.setText(s);
            t.setLineSpacing(0, 1.15f);
            t.setPadding(dp(c, 16), dp(c, 8), dp(c, 16), 0);
            column.addView(t);
            return this;
        }

        private Row add(LinearLayout v) {
            if (card == null) {
                header("");
            }
            Row r = new Row(v);
            if (card.getChildCount() > 0) {
                View d = new View(c);
                d.setBackgroundColor(DIVIDER);
                LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(-1, Math.max(1, dp(c, 0.7f)));
                lp.leftMargin = dp(c, 16);
                card.addView(d, lp);
                r.divider = d;
            }
            card.addView(v, new LinearLayout.LayoutParams(-1, -2));
            rows.add(r);
            return r;
        }

        /* Row layout: [icon] title / description ... [value | badge | switch]. */
        private LinearLayout shell(Row[] out, Supplier<Drawable> icon, boolean bigIcon) {
            LinearLayout v = new LinearLayout(c);
            v.setOrientation(LinearLayout.HORIZONTAL);
            v.setGravity(Gravity.CENTER_VERTICAL);
            v.setMinimumHeight(dp(c, 64));
            v.setPadding(dp(c, 16), dp(c, 12), dp(c, 16), dp(c, 12));
            Row r = add(v);
            out[0] = r;
            if (icon != null) {
                ImageView iv = new ImageView(c);
                iv.setScaleType(ImageView.ScaleType.CENTER_CROP);
                iv.setClipToOutline(true);
                iv.setBackground(rounded(SURFACE_HI, dp(c, bigIcon ? 10 : 12)));
                int w = dp(c, bigIcon ? 64 : 40), h = dp(c, 40);
                LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(w, h);
                lp.rightMargin = dp(c, 14);
                v.addView(iv, lp);
                r.icon = iv;
                r.iconFn = icon;
            }
            LinearLayout texts = new LinearLayout(c);
            texts.setOrientation(LinearLayout.VERTICAL);
            r.title = text(c, 16, TEXT, false);
            r.title.setSingleLine(true);
            r.title.setEllipsize(TextUtils.TruncateAt.END);
            r.desc = text(c, 13, TEXT2, false);
            r.desc.setPadding(0, dp(c, 4), 0, 0);
            r.desc.setLineSpacing(0, 1.1f);
            r.desc.setMaxLines(3);
            r.desc.setEllipsize(TextUtils.TruncateAt.END);
            texts.addView(r.title);
            texts.addView(r.desc);
            v.addView(texts, new LinearLayout.LayoutParams(0, -2, 1));
            r.badge = text(c, 11, BG, true);
            r.badge.setLetterSpacing(0.08f);
            r.badge.setPadding(dp(c, 8), dp(c, 4), dp(c, 8), dp(c, 4));
            r.badge.setBackground(rounded(GOOD, dp(c, 10)));
            r.badge.setVisibility(View.GONE);
            LinearLayout.LayoutParams blp = new LinearLayout.LayoutParams(-2, -2);
            blp.leftMargin = dp(c, 12);
            v.addView(r.badge, blp);
            return v;
        }

        private void clickable(LinearLayout v, Runnable onClick) {
            if (onClick != null) {
                v.setBackground(ripple(null, rounded(0xFFFFFFFF, 0)));
                v.setOnClickListener(x -> {
                    onClick.run();
                    refresh();
                });
            }
        }

        /** A row with a value on the right; onClick may be null (information only). */
        Row row(String title, Supplier<String> desc, Supplier<String> value, Runnable onClick) {
            return row(() -> title, desc, value, null, false, onClick);
        }

        Row row(Supplier<String> title, Supplier<String> desc, Supplier<String> value, Supplier<Drawable> icon,
                boolean bigIcon, Runnable onClick) {
            Row[] r = new Row[1];
            LinearLayout v = shell(r, icon, bigIcon);
            r[0].titleFn = title;
            r[0].descFn = desc;
            if (value != null) {
                r[0].value = text(c, 14, ACCENT, false);
                r[0].value.setGravity(Gravity.END);
                r[0].value.setMaxWidth(dp(c, 150));
                LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(-2, -2);
                lp.leftMargin = dp(c, 12);
                v.addView(r[0].value, lp);
                r[0].valueFn = value;
            }
            clickable(v, onClick);
            r[0].refresh();
            return r[0];
        }

        /** A row with a switch; set gets the new state from a tap on the row or the switch. */
        Row toggle(String title, Supplier<String> desc, BooleanSupplier on, Consumer<Boolean> set) {
            Row[] r = new Row[1];
            LinearLayout v = shell(r, null, false);
            r[0].titleFn = () -> title;
            r[0].descFn = desc;
            r[0].onFn = on;
            Switch s = new Switch(c);
            int[][] states = {{android.R.attr.state_checked}, {}};
            s.setThumbTintList(new ColorStateList(states, new int[] {ACCENT, 0xFFB0B4BA}));
            s.setTrackTintList(new ColorStateList(states, new int[] {ACCENT_BG | 0x55000000, 0x33FFFFFF}));
            s.setChecked(on.getAsBoolean());
            s.setOnCheckedChangeListener((b, checked) -> {
                if (s.getTag() == null) {
                    set.accept(checked);
                    refresh();
                }
            });
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(-2, -2);
            lp.leftMargin = dp(c, 12);
            v.addView(s, lp);
            r[0].toggle = s;
            clickable(v, () -> s.toggle());
            r[0].refresh();
            return r[0];
        }

        /** A row whose value is one of labels; a tap opens a picker. */
        Row choice(String title, Supplier<String> desc, Supplier<String[]> labels, IntSupplier selected,
                   IntConsumer pick) {
            return row(() -> title, desc, () -> {
                String[] l = labels.get();
                int i = selected.getAsInt();
                return i >= 0 && i < l.length ? l[i] : "";
            }, null, false, () -> choose(c, title, labels.get(), selected.getAsInt(), i -> {
                pick.accept(i);
                refresh();
            }));
        }

        /** A row with a slider under its title; set gets the value when the finger lifts. */
        Row slider(String title, Supplier<String> desc, int max, IntSupplier get, IntConsumer set) {
            LinearLayout v = new LinearLayout(c);
            v.setOrientation(LinearLayout.VERTICAL);
            v.setPadding(dp(c, 16), dp(c, 14), dp(c, 16), dp(c, 8));
            Row r = add(v);
            LinearLayout top = new LinearLayout(c);
            top.setOrientation(LinearLayout.HORIZONTAL);
            r.title = text(c, 16, TEXT, false);
            r.titleFn = () -> title;
            top.addView(r.title, new LinearLayout.LayoutParams(0, -2, 1));
            r.value = text(c, 14, ACCENT, false);
            r.valueFn = desc;
            top.addView(r.value);
            v.addView(top);
            SeekBar sb = new SeekBar(c);
            sb.setMax(max);
            sb.setProgressTintList(ColorStateList.valueOf(ACCENT));
            sb.setThumbTintList(ColorStateList.valueOf(ACCENT));
            sb.setProgressBackgroundTintList(ColorStateList.valueOf(0x33FFFFFF));
            sb.setPadding(dp(c, 4), dp(c, 12), dp(c, 4), dp(c, 8));
            sb.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
                @Override
                public void onProgressChanged(SeekBar s, int p, boolean user) {}

                @Override
                public void onStartTrackingTouch(SeekBar s) {}

                @Override
                public void onStopTrackingTouch(SeekBar s) {
                    set.accept(s.getProgress());
                    refresh();
                }
            });
            v.addView(sb, new LinearLayout.LayoutParams(-1, -2));
            r.slider = sb;
            r.sliderFn = get;
            r.refresh();
            return r;
        }

        /** A compact label / value line (the gadget's report, stats). */
        Row info(String label, Supplier<String> value) {
            LinearLayout v = new LinearLayout(c);
            v.setOrientation(LinearLayout.HORIZONTAL);
            v.setGravity(Gravity.CENTER_VERTICAL);
            v.setMinimumHeight(dp(c, 48));
            v.setPadding(dp(c, 16), dp(c, 10), dp(c, 16), dp(c, 10));
            Row r = add(v);
            r.title = text(c, 15, TEXT2, false);
            r.titleFn = () -> label;
            v.addView(r.title, new LinearLayout.LayoutParams(-2, -2));
            r.value = text(c, 14, TEXT, false);
            r.value.setTypeface(Typeface.MONOSPACE);
            r.value.setGravity(Gravity.END);
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(0, -2, 1);
            lp.leftMargin = dp(c, 16);
            v.addView(r.value, lp);
            r.valueFn = value;
            r.refresh();
            return r;
        }

        void refresh() {
            for (Row r : rows) {
                r.refresh();
            }
        }
    }

    /** A pill-shaped button. */
    static TextView pill(Context c, String label, int fg, int bg) {
        TextView b = text(c, 14, fg, true);
        b.setText(label);
        b.setGravity(Gravity.CENTER);
        b.setMinWidth(dp(c, 76));
        b.setPadding(dp(c, 16), dp(c, 9), dp(c, 16), dp(c, 9));
        b.setBackground(ripple(rounded(bg, dp(c, 20)), null));
        return b;
    }

    /** A filled circle, for status dots. */
    static View dot(Context c, int color, int sizeDp) {
        View d = new View(c);
        GradientDrawable g = new GradientDrawable();
        g.setShape(GradientDrawable.OVAL);
        g.setColor(color);
        d.setBackground(g);
        d.setLayoutParams(new LinearLayout.LayoutParams(dp(c, sizeDp), dp(c, sizeDp)));
        return d;
    }

    static void tintDot(View d, int color) {
        ((GradientDrawable) d.getBackground()).setColor(color);
    }

    static FrameLayout.LayoutParams frame(int w, int h, int gravity) {
        return new FrameLayout.LayoutParams(w, h, gravity);
    }
}
