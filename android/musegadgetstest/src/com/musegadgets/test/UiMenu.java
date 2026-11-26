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

import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.RectF;
import android.graphics.Typeface;

import java.nio.ByteBuffer;
import java.util.ArrayList;
import java.util.List;

/**
 * The UI stream's menu (protocols/mgcommands.h, UI input controls): the
 * gadget's menu button opens it, the phone draws it into the streamed
 * picture (after the warp, at the panel's pixels, so it reads the same on
 * every gadget), and taps on the gadget pick an item.
 */
final class UiMenu {
    /** An item: its label and current value, and what a tap does. */
    static final class Item {
        final String label;
        final java.util.function.Supplier<String> value;
        final Runnable tap;
        RectF box;
        int row;   /* items with the same nonzero row share one line, side by side */

        Item(String label, java.util.function.Supplier<String> value, Runnable tap) {
            this.label = label;
            this.value = value;
            this.tap = tap;
        }

        Item row(int r) {
            row = r;
            return this;
        }
    }

    final List<Item> items = new java.util.concurrent.CopyOnWriteArrayList<>();
    private Bitmap bmp;
    private final Paint dim = new Paint(), pill = new Paint(Paint.ANTI_ALIAS_FLAG), text = new Paint(Paint.ANTI_ALIAS_FLAG),
            accent = new Paint(Paint.ANTI_ALIAS_FLAG), title = new Paint(Paint.ANTI_ALIAS_FLAG);
    private int pressed = -1;

    UiMenu() {
        dim.setColor(Color.argb(215, 0, 0, 0));
        pill.setColor(Color.rgb(40, 44, 52));
        text.setColor(Color.WHITE);
        text.setTypeface(Typeface.create(Typeface.DEFAULT, Typeface.BOLD));
        accent.setColor(Color.rgb(90, 200, 250));
        accent.setTypeface(Typeface.create(Typeface.DEFAULT, Typeface.BOLD));
        title.setColor(Color.rgb(170, 170, 180));
    }

    /** Draws the menu over an RGBA picture (row 0 at the top) in place. */
    void draw(ByteBuffer rgba, int w, int h) {
        if (bmp == null || bmp.getWidth() != w || bmp.getHeight() != h) {
            bmp = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888);
        }
        rgba.position(0);
        bmp.copyPixelsFromBuffer(rgba);
        Canvas c = new Canvas(bmp);
        c.drawRect(0, 0, w, h, dim);
        float sz = Math.min(w, h);
        /* Lines: each item alone, or a run of items with the same row side by side. */
        java.util.List<java.util.List<Item>> lines = new ArrayList<>();
        for (Item it : items) {
            java.util.List<Item> last = lines.isEmpty() ? null : lines.get(lines.size() - 1);
            if (it.row != 0 && last != null && last.get(0).row == it.row) {
                last.add(it);
            } else {
                java.util.List<Item> l = new ArrayList<>();
                l.add(it);
                lines.add(l);
            }
        }
        int n = lines.size();
        float rowH = sz * (n > 4 ? 0.105f : 0.12f), gap = sz * (n > 4 ? 0.02f : 0.025f), rowW = sz * 0.68f;
        float top = (h - n * rowH - (n - 1) * gap) / 2 + sz * 0.02f;
        text.setTextSize(sz * 0.05f);
        accent.setTextSize(sz * 0.05f);
        title.setTextSize(sz * 0.04f);
        title.setTextAlign(Paint.Align.CENTER);
        c.drawText("Menu", w / 2f, top - sz * 0.035f, title);
        for (int li = 0; li < n; li++) {
            java.util.List<Item> line = lines.get(li);
            float y = top + li * (rowH + gap), x0 = (w - rowW) / 2;
            float cellGap = sz * 0.02f, cellW = (rowW - (line.size() - 1) * cellGap) / line.size();
            for (int k = 0; k < line.size(); k++) {
                Item it = line.get(k);
                float x = x0 + k * (cellW + cellGap);
                it.box = new RectF(x, y, x + cellW, y + rowH);
                pill.setColor(items.indexOf(it) == pressed ? Color.rgb(70, 78, 92) : Color.rgb(40, 44, 52));
                c.drawRoundRect(it.box, rowH / 2, rowH / 2, pill);
                float base = y + rowH / 2 - (text.descent() + text.ascent()) / 2;
                String v = it.value != null ? it.value.get() : null;
                if (v != null) {
                    text.setTextAlign(Paint.Align.LEFT);
                    c.drawText(it.label, x + rowH * 0.45f, base, text);
                    accent.setTextAlign(Paint.Align.RIGHT);
                    c.drawText(v, x + cellW - rowH * 0.45f, base, accent);
                } else {
                    text.setTextAlign(Paint.Align.CENTER);
                    c.drawText(it.label, x + cellW / 2, base, text);
                }
            }
        }
        rgba.position(0);
        bmp.copyPixelsToBuffer(rgba);
        rgba.position(0);
    }

    /** The item under a panel pixel, or -1. */
    int hit(float x, float y) {
        for (int i = 0; i < items.size(); i++) {
            RectF b = items.get(i).box;
            if (b != null && b.contains(x, y)) {
                return i;
            }
        }
        return -1;
    }

    void setPressed(int i) {
        pressed = i;
    }
}
