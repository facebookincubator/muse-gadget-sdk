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

import android.graphics.Canvas;
import android.graphics.ColorFilter;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.PixelFormat;
import android.graphics.RectF;
import android.graphics.drawable.Drawable;

/** Line icons drawn in code (the app has no resources): the tab bar's and the list's. */
final class Icons {
    private Icons() {}

    /** Draws an icon in a box of side s (stroke paint set up, color included). */
    interface Glyph {
        void draw(Canvas c, Paint p, float s);
    }

    /** The glyph as a drawable, in one color. */
    static Drawable of(Glyph g, int color) {
        return new GlyphDrawable(g, color);
    }

    static final class GlyphDrawable extends Drawable {
        private final Glyph g;
        private final Paint p = new Paint(Paint.ANTI_ALIAS_FLAG);
        private float pad = 0;

        GlyphDrawable(Glyph g, int color) {
            this.g = g;
            p.setColor(color);
            p.setStyle(Paint.Style.STROKE);
            p.setStrokeCap(Paint.Cap.ROUND);
            p.setStrokeJoin(Paint.Join.ROUND);
        }

        /** Inset as a fraction of the side (for icons inside a tile). */
        GlyphDrawable pad(float f) {
            pad = f;
            return this;
        }

        void setColor(int color) {
            p.setColor(color);
            invalidateSelf();
        }

        @Override
        public void draw(Canvas c) {
            float w = getBounds().width(), h = getBounds().height();
            float s = Math.min(w, h) * (1 - 2 * pad);
            c.save();
            c.translate(getBounds().left + (w - s) / 2, getBounds().top + (h - s) / 2);
            p.setStrokeWidth(s * 0.075f);
            g.draw(c, p, s);
            c.restore();
        }

        @Override
        public void setAlpha(int a) {
            p.setAlpha(a);
        }

        @Override
        public void setColorFilter(ColorFilter f) {
            p.setColorFilter(f);
        }

        @Override
        public int getOpacity() {
            return PixelFormat.TRANSLUCENT;
        }
    }

    /** A video camera: the Video tab. */
    static final Glyph VIDEO = (c, p, s) -> {
        c.drawRoundRect(new RectF(s * 0.08f, s * 0.26f, s * 0.66f, s * 0.74f), s * 0.1f, s * 0.1f, p);
        Path t = new Path();
        t.moveTo(s * 0.66f, s * 0.44f);
        t.lineTo(s * 0.92f, s * 0.30f);
        t.lineTo(s * 0.92f, s * 0.70f);
        t.lineTo(s * 0.66f, s * 0.56f);
        c.drawPath(t, p);
    };

    /** A round gadget on a strap: the Gadget tab. */
    static final Glyph GADGET = (c, p, s) -> {
        c.drawCircle(s * 0.5f, s * 0.5f, s * 0.24f, p);
        c.drawRoundRect(new RectF(s * 0.38f, s * 0.08f, s * 0.62f, s * 0.26f), s * 0.05f, s * 0.05f, p);
        c.drawRoundRect(new RectF(s * 0.38f, s * 0.74f, s * 0.62f, s * 0.92f), s * 0.05f, s * 0.05f, p);
        c.drawLine(s * 0.5f, s * 0.5f, s * 0.5f, s * 0.38f, p);
        c.drawLine(s * 0.5f, s * 0.5f, s * 0.6f, s * 0.55f, p);
    };

    /** A phone camera: the Camera source. */
    static final Glyph CAMERA = (c, p, s) -> {
        Path b = new Path();
        b.moveTo(s * 0.12f, s * 0.34f);
        b.lineTo(s * 0.30f, s * 0.34f);
        b.lineTo(s * 0.38f, s * 0.22f);
        b.lineTo(s * 0.62f, s * 0.22f);
        b.lineTo(s * 0.70f, s * 0.34f);
        b.lineTo(s * 0.88f, s * 0.34f);
        b.lineTo(s * 0.88f, s * 0.78f);
        b.lineTo(s * 0.12f, s * 0.78f);
        b.close();
        c.drawPath(b, p);
        c.drawCircle(s * 0.5f, s * 0.55f, s * 0.14f, p);
    };

    /** A tile of color bars: the test pattern source. */
    static Drawable bars() {
        return new Drawable() {
            private final Paint p = new Paint();
            private final int[] colors = {0xFFC0C0C0, 0xFFC0C000, 0xFF00C0C0, 0xFF00C000, 0xFFC000C0, 0xFFC00000,
                                          0xFF0000C0};

            @Override
            public void draw(Canvas c) {
                float w = getBounds().width() / (float) colors.length;
                for (int i = 0; i < colors.length; i++) {
                    p.setColor(colors[i]);
                    c.drawRect(getBounds().left + i * w, getBounds().top, getBounds().left + (i + 1) * w + 1,
                            getBounds().bottom, p);
                }
            }

            @Override
            public void setAlpha(int a) {}

            @Override
            public void setColorFilter(ColorFilter f) {}

            @Override
            public int getOpacity() {
                return PixelFormat.OPAQUE;
            }
        };
    }
}
