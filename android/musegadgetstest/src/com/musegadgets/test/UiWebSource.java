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

import android.app.Presentation;
import android.content.Context;
import android.hardware.display.DisplayManager;
import android.hardware.display.VirtualDisplay;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.util.Log;
import android.view.MotionEvent;
import android.view.Surface;
import android.webkit.WebSettings;
import android.webkit.WebView;
import android.webkit.WebViewClient;

/**
 * A web page rendered offscreen for the gadget (protocols/UI.md): a WebView
 * in a Presentation on a private VirtualDisplay whose Surface is
 * UiRenderer's, so the page draws straight into the stream at the size we
 * pick, and only when it changes (the display composes a frame only on a
 * redraw). The display is square: cssPx CSS pixels across, rendered at px
 * device pixels (the gadget's size, or twice it with the fisheye on, which
 * the renderer then supersamples). Touches from the gadget come back as
 * MotionEvents injected into the WebView.
 */
final class UiWebSource {
    private static final String TAG = "MuseCam";

    private final Context ctx;
    private final Handler main = new Handler(Looper.getMainLooper());
    private VirtualDisplay vd;
    private Presentation pres;
    private WebView web;
    private int px;
    private long downTime;
    private String url;

    UiWebSource(Context ctx) {
        this.ctx = ctx;
    }

    /** Shows url on a px x px display, cssPx CSS pixels across, drawing into surface. Main thread. */
    void start(Surface surface, int px, int cssPx, String url) {
        stop();
        this.px = px;
        this.url = url;
        int dpi = Math.max(60, Math.round(160f * px / Math.max(1, cssPx)));
        DisplayManager dm = ctx.getSystemService(DisplayManager.class);
        vd = dm.createVirtualDisplay("mg-ui-web", px, px, dpi, surface,
                DisplayManager.VIRTUAL_DISPLAY_FLAG_PRESENTATION | DisplayManager.VIRTUAL_DISPLAY_FLAG_OWN_CONTENT_ONLY);
        if (vd == null) {
            Log.e(TAG, "ui: no virtual display");
            return;
        }
        pres = new Presentation(ctx, vd.getDisplay());
        web = new WebView(pres.getContext());
        WebSettings s = web.getSettings();
        s.setJavaScriptEnabled(true);
        s.setDomStorageEnabled(true);
        s.setAllowFileAccess(false);
        s.setLoadWithOverviewMode(false);
        s.setUseWideViewPort(true);
        web.setVerticalScrollBarEnabled(false);
        web.setHorizontalScrollBarEnabled(false);
        web.setBackgroundColor(0xff000000);
        web.setWebViewClient(new WebViewClient());
        pres.setContentView(web);
        pres.show();
        web.loadUrl(url);
        Log.i(TAG, "ui: web " + url + " on a " + px + " px display, " + cssPx + " CSS px across (" + dpi + " dpi)");
    }

    void load(String url) {
        this.url = url;
        main.post(() -> {
            if (web != null) {
                web.loadUrl(url);
            }
        });
    }

    String url() {
        return url;
    }

    /** Resizes the display (the fisheye changed the render size). */
    void resize(int px, int cssPx) {
        main.post(() -> {
            if (vd != null) {
                this.px = px;
                vd.resize(px, px, Math.max(60, Math.round(160f * px / Math.max(1, cssPx))));
            }
        });
    }

    boolean running() {
        return web != null;
    }

    void stop() {
        if (pres != null) {
            pres.dismiss();
            pres = null;
        }
        if (web != null) {
            web.destroy();
            web = null;
        }
        if (vd != null) {
            vd.release();
            vd = null;
        }
    }

    /** Bench: the page as the WebView draws it into a bitmap at its display's size (ground truth for the GL path). */
    void snapshot(java.io.File f) {
        main.post(() -> {
            if (web == null || web.getWidth() <= 0) {
                return;
            }
            android.graphics.Bitmap b = android.graphics.Bitmap.createBitmap(web.getWidth(), web.getHeight(),
                    android.graphics.Bitmap.Config.ARGB_8888);
            web.draw(new android.graphics.Canvas(b));
            try (java.io.FileOutputStream o = new java.io.FileOutputStream(f)) {
                b.compress(android.graphics.Bitmap.CompressFormat.PNG, 100, o);
                Log.i(TAG, "ui: saved " + f + " (" + b.getWidth() + "x" + b.getHeight() + ")");
            } catch (java.io.IOException e) {
                Log.w(TAG, "ui: web snapshot failed", e);
            }
        });
    }

    /** A touch at fractions (fx, fy) of the page's square: Mg.TOUCH_* actions. Any thread. */
    void touch(int action, double fx, double fy) {
        main.post(() -> {
            if (web == null) {
                return;
            }
            float x = (float) (fx * px), y = (float) (fy * px);
            long now = SystemClock.uptimeMillis();
            int a;
            if (action == Mg.TOUCH_DOWN) {
                downTime = now;
                a = MotionEvent.ACTION_DOWN;
            } else if (action == Mg.TOUCH_MOVE) {
                a = MotionEvent.ACTION_MOVE;
            } else {
                a = MotionEvent.ACTION_UP;
            }
            MotionEvent e = MotionEvent.obtain(downTime, now, a, x, y, 0);
            e.setSource(android.view.InputDevice.SOURCE_TOUCHSCREEN);
            web.dispatchTouchEvent(e);
            e.recycle();
        });
    }
}
