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

import android.graphics.SurfaceTexture;
import android.opengl.EGL14;
import android.opengl.EGLConfig;
import android.opengl.EGLContext;
import android.opengl.EGLDisplay;
import android.opengl.EGLSurface;
import android.opengl.GLES11Ext;
import android.opengl.GLES20;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.SystemClock;
import android.util.Log;
import android.view.Surface;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.FloatBuffer;

/**
 * The GPU half of a UI stream (protocols/UI.md): a Surface any source draws
 * into (a VirtualDisplay of the web view, or MediaProjection's mirror of the
 * screen), and on every new frame a pass that takes the chosen square of it,
 * warps it onto the disc when the fisheye is on, masks the round panel's
 * corners, and renders it at the gadget's own size; read back and turned into
 * I420 for the encoder (Listener.onPicture). The source only produces frames
 * when what it shows changes, so a still screen costs nothing here either.
 */
final class UiRenderer {
    private static final String TAG = "MuseCam";

    interface Listener {
        /** A new picture, I420 at the output size; tNs: when the source's frame arrived (elapsedRealtimeNanos). */
        void onPicture(byte[] i420, long tNs);

        /** The same with the picture as RGB565 big-endian (when wantsRgb565). */
        default void onPicture(byte[] i420, byte[] rgb565, long tNs) {
            onPicture(i420, tNs);
        }

        default boolean wantsRgb565() {
            return false;
        }
    }

    private final HandlerThread thread = new HandlerThread("ui-gl");
    private final Handler h;
    private final Listener listener;
    private volatile boolean released;
    private EGLDisplay dpy = EGL14.EGL_NO_DISPLAY;
    private EGLContext ctx = EGL14.EGL_NO_CONTEXT;
    private EGLConfig cfg;
    private EGLSurface pbuf = EGL14.EGL_NO_SURFACE, previewEgl = EGL14.EGL_NO_SURFACE;
    private Surface previewSurface;
    private int previewW, previewH;
    private int oesTex, fbo, fboTex, prog, progShow;
    private SurfaceTexture st;
    private Surface surface;
    private final float[] stm = new float[16];
    private int outW, outH;
    private ByteBuffer rgba;
    private boolean haveFrame;
    private long frameNs;
    /* What to show (set from any thread; applied on the next render). */
    private volatile float cropX, cropY, cropW = 1, cropH = 1;   /* the square, fractions of the source (y down) */
    private volatile boolean fisheye = true, round = true;
    private volatile int taps = 4;                                /* 1: one-to-one sources, 4: supersampled */
    volatile long statRenders, statRenderUs;
    /* Drawn over the picture after the warp (the gadget's menu), or null. */
    volatile UiMenu overlay;
    /* Bench: the next picture (before and after the overlay) saved as PNGs here. */
    private volatile java.io.File snapBase;

    /** Saves the next picture rendered as BASE_out.png (what's encoded), and renders now. */
    void snapshot(java.io.File base) {
        snapBase = base;
        rerender();
    }

    private static final String VS =
            "attribute vec2 a;\n"
            + "varying vec2 p;\n"
            + "void main() { p = a; gl_Position = vec4(a, 0.0, 1.0); }\n";

    /* p: the output picture, x right, y DOWN, [-1, 1] (memory row 0 is the top). */
    private static final String FS =
            "#extension GL_OES_EGL_image_external : require\n"
            + "precision highp float;\n"
            + "uniform samplerExternalOES tex;\n"
            + "uniform mat4 stm;\n"
            + "uniform vec4 crop;\n"      // x, y, w, h of the square in the source, fractions, y down
            + "uniform vec2 px;\n"        // one output pixel in p units
            + "uniform float warp, roundMask, rlim, edge;\n"
            + "uniform int taps;\n"
            + "varying vec2 p;\n"
            + "vec2 sq(vec2 d) {\n"       // square from disc (protocols/UI.md)
            + "  float r = length(d); if (r > 1.0) d /= r;\n"
            + "  float s2 = 2.0 * sqrt(2.0);\n"
            + "  float a = 2.0 + d.x * d.x - d.y * d.y, b = 2.0 - d.x * d.x + d.y * d.y;\n"
            + "  return vec2(0.5 * sqrt(max(0.0, a + s2 * d.x)) - 0.5 * sqrt(max(0.0, a - s2 * d.x)),\n"
            + "              0.5 * sqrt(max(0.0, b + s2 * d.y)) - 0.5 * sqrt(max(0.0, b - s2 * d.y)));\n"
            + "}\n"
            + "vec3 at(vec2 o) {\n"
            + "  vec2 q = o;\n"   // memory row 0 (gl_FragCoord y 0) is the picture's top
            + "  vec2 s = warp > 0.5 ? sq(q) : clamp(q, -1.0, 1.0);\n"
            + "  vec2 f = (s + 1.0) * 0.5;\n"   // fractions of the square, y down
            + "  vec2 src = crop.xy + f * crop.zw;\n"
            // past the source's edges (a mirror window panned past the screen): black, with a faint line at the edge
            + "  vec2 out2 = max(-src, src - 1.0);\n"
            + "  float past = max(out2.x, out2.y);\n"
            + "  if (past > 0.0) return past < edge ? vec3(0.28) : vec3(0.0);\n"
            + "  vec4 t = stm * vec4(src.x, 1.0 - src.y, 0.0, 1.0);\n"
            + "  return texture2D(tex, t.xy).rgb;\n"
            + "}\n"
            + "void main() {\n"
            + "  if (roundMask > 0.5 && length(p) > rlim) { gl_FragColor = vec4(0.0, 0.0, 0.0, 1.0); return; }\n"
            + "  vec3 c;\n"
            + "  if (taps == 1) c = at(p);\n"
            + "  else c = 0.25 * (at(p + px * vec2(-0.25, -0.25)) + at(p + px * vec2(0.25, -0.25))\n"
            + "                 + at(p + px * vec2(-0.25, 0.25)) + at(p + px * vec2(0.25, 0.25)));\n"
            + "  gl_FragColor = vec4(c, 1.0);\n"
            + "}\n";

    private static final String FS_SHOW =
            "precision mediump float;\n"
            + "uniform sampler2D tex;\n"
            + "varying vec2 p;\n"
            + "void main() { gl_FragColor = texture2D(tex, vec2(p.x * 0.5 + 0.5, 0.5 - p.y * 0.5)); }\n";

    private final FloatBuffer quad = ByteBuffer.allocateDirect(8 * 4).order(ByteOrder.nativeOrder()).asFloatBuffer()
            .put(new float[] {-1, -1, 1, -1, -1, 1, 1, 1});

    UiRenderer(Listener l) {
        listener = l;
        thread.start();
        h = new Handler(thread.getLooper());
        quad.position(0);
        h.post(this::setUp);
    }

    /** The output (the gadget's picture) size; the source's buffer size. Returns the Surface sources draw into. */
    Surface configure(int outW, int outH, int srcW, int srcH) {
        final Surface[] r = new Surface[1];
        runSync(() -> {
            if (st == null) {
                return;   /* released */
            }
            this.outW = outW;
            this.outH = outH;
            rgba = ByteBuffer.allocateDirect(outW * outH * 4).order(ByteOrder.nativeOrder());
            GLES20.glBindTexture(GLES20.GL_TEXTURE_2D, fboTex);
            GLES20.glTexImage2D(GLES20.GL_TEXTURE_2D, 0, GLES20.GL_RGBA, outW, outH, 0, GLES20.GL_RGBA,
                    GLES20.GL_UNSIGNED_BYTE, null);
            GLES20.glBindFramebuffer(GLES20.GL_FRAMEBUFFER, fbo);
            GLES20.glFramebufferTexture2D(GLES20.GL_FRAMEBUFFER, GLES20.GL_COLOR_ATTACHMENT0, GLES20.GL_TEXTURE_2D,
                    fboTex, 0);
            st.setDefaultBufferSize(srcW, srcH);
            haveFrame = false;
            r[0] = surface;
        });
        return r[0];
    }

    /** The source's buffer size changed (it keeps drawing into the same Surface). */
    void setSourceSize(int srcW, int srcH) {
        h.post(() -> {
            if (st != null) {
                st.setDefaultBufferSize(srcW, srcH);
            }
        });
    }

    /** The square of the source to stream (fractions of its width and height, y down); fisheye; taps 1 or 4. */
    void setView(double x, double y, double w, double h, boolean fisheye, boolean round, int taps) {
        cropX = (float) x;
        cropY = (float) y;
        cropW = (float) w;
        cropH = (float) h;
        this.fisheye = fisheye;
        this.round = round;
        this.taps = taps;
        rerender();
    }

    /** Draws the last frame again (the view changed). */
    void rerender() {
        h.post(() -> {
            if (haveFrame && st != null) {
                render(SystemClock.elapsedRealtimeNanos());
            }
        });
    }

    /** Where to show what's sent (the app's preview), or null. */
    void setPreview(Surface s, int w, int hgt) {
        h.post(() -> {
            if (st == null) {
                return;   /* released */
            }
            if (previewEgl != EGL14.EGL_NO_SURFACE) {
                EGL14.eglDestroySurface(dpy, previewEgl);
                previewEgl = EGL14.EGL_NO_SURFACE;
            }
            previewSurface = s;
            previewW = w;
            previewH = hgt;
            if (s != null && s.isValid()) {
                previewEgl = EGL14.eglCreateWindowSurface(dpy, cfg, s, new int[] {EGL14.EGL_NONE}, 0);
            }
            EGL14.eglMakeCurrent(dpy, pbuf, pbuf, ctx);
        });
    }

    void release() {
        released = true;   /* at once: a frame being drawn now goes nowhere */
        h.post(() -> {
            /* Nulled here, on this thread: a frame callback or a draw already
             * queued behind this one then finds no SurfaceTexture (and does
             * nothing) instead of an abandoned one and a destroyed context. */
            haveFrame = false;
            if (st != null) {
                st.release();
                st = null;
            }
            if (surface != null) {
                surface.release();
                surface = null;
            }
            if (previewEgl != EGL14.EGL_NO_SURFACE) {
                EGL14.eglDestroySurface(dpy, previewEgl);
                previewEgl = EGL14.EGL_NO_SURFACE;
            }
            EGL14.eglMakeCurrent(dpy, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_CONTEXT);
            EGL14.eglDestroySurface(dpy, pbuf);
            EGL14.eglDestroyContext(dpy, ctx);
            EGL14.eglTerminate(dpy);
            thread.quitSafely();
        });
        /* Bounded: lets a frame in progress finish before the next session's streamer and renderer exist. */
        if (Thread.currentThread() != thread) {
            try {
                thread.join(500);
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
            }
        }
    }

    private void runSync(Runnable r) {
        java.util.concurrent.CountDownLatch done = new java.util.concurrent.CountDownLatch(1);
        if (!h.post(() -> {
            try {
                r.run();
            } finally {
                done.countDown();
            }
        })) {
            return;   /* released */
        }
        try {
            done.await();
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        }
    }

    private void setUp() {
        dpy = EGL14.eglGetDisplay(EGL14.EGL_DEFAULT_DISPLAY);
        int[] ver = new int[2];
        EGL14.eglInitialize(dpy, ver, 0, ver, 1);
        int[] attr = {EGL14.EGL_RED_SIZE, 8, EGL14.EGL_GREEN_SIZE, 8, EGL14.EGL_BLUE_SIZE, 8, EGL14.EGL_ALPHA_SIZE, 8,
                      EGL14.EGL_RENDERABLE_TYPE, EGL14.EGL_OPENGL_ES2_BIT,
                      EGL14.EGL_SURFACE_TYPE, EGL14.EGL_PBUFFER_BIT | EGL14.EGL_WINDOW_BIT, EGL14.EGL_NONE};
        EGLConfig[] cfgs = new EGLConfig[1];
        int[] n = new int[1];
        EGL14.eglChooseConfig(dpy, attr, 0, cfgs, 0, 1, n, 0);
        cfg = cfgs[0];
        ctx = EGL14.eglCreateContext(dpy, cfg, EGL14.EGL_NO_CONTEXT,
                new int[] {EGL14.EGL_CONTEXT_CLIENT_VERSION, 2, EGL14.EGL_NONE}, 0);
        pbuf = EGL14.eglCreatePbufferSurface(dpy, cfg, new int[] {EGL14.EGL_WIDTH, 1, EGL14.EGL_HEIGHT, 1, EGL14.EGL_NONE}, 0);
        EGL14.eglMakeCurrent(dpy, pbuf, pbuf, ctx);
        int[] t = new int[2];
        GLES20.glGenTextures(2, t, 0);
        oesTex = t[0];
        fboTex = t[1];
        GLES20.glBindTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, oesTex);
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_MIN_FILTER, GLES20.GL_LINEAR);
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_MAG_FILTER, GLES20.GL_LINEAR);
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_WRAP_S, GLES20.GL_CLAMP_TO_EDGE);
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_WRAP_T, GLES20.GL_CLAMP_TO_EDGE);
        GLES20.glBindTexture(GLES20.GL_TEXTURE_2D, fboTex);
        GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_MIN_FILTER, GLES20.GL_LINEAR);
        GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_MAG_FILTER, GLES20.GL_LINEAR);
        GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_WRAP_S, GLES20.GL_CLAMP_TO_EDGE);
        GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_WRAP_T, GLES20.GL_CLAMP_TO_EDGE);
        int[] f = new int[1];
        GLES20.glGenFramebuffers(1, f, 0);
        fbo = f[0];
        prog = program(VS, FS);
        progShow = program(VS, FS_SHOW);
        st = new SurfaceTexture(oesTex);
        st.setOnFrameAvailableListener(s -> {   /* on this thread already: drawn now, never queued behind release */
            frameNs = SystemClock.elapsedRealtimeNanos();
            onFrame();
        }, h);
        surface = new Surface(st);
    }

    private void onFrame() {
        if (st == null) {
            return;
        }
        st.updateTexImage();
        st.getTransformMatrix(stm);
        haveFrame = true;
        if (outW > 0) {
            render(frameNs);
        }
    }

    private void render(long tNs) {
        long t0 = SystemClock.elapsedRealtimeNanos();
        GLES20.glBindFramebuffer(GLES20.GL_FRAMEBUFFER, fbo);
        GLES20.glViewport(0, 0, outW, outH);
        GLES20.glUseProgram(prog);
        GLES20.glActiveTexture(GLES20.GL_TEXTURE0);
        GLES20.glBindTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, oesTex);
        GLES20.glUniform1i(GLES20.glGetUniformLocation(prog, "tex"), 0);
        GLES20.glUniformMatrix4fv(GLES20.glGetUniformLocation(prog, "stm"), 1, false, stm, 0);
        GLES20.glUniform4f(GLES20.glGetUniformLocation(prog, "crop"), cropX, cropY, cropW, cropH);
        GLES20.glUniform2f(GLES20.glGetUniformLocation(prog, "px"), 2f / outW, 2f / outH);
        GLES20.glUniform1f(GLES20.glGetUniformLocation(prog, "warp"), fisheye ? 1f : 0f);
        GLES20.glUniform1f(GLES20.glGetUniformLocation(prog, "roundMask"), round ? 1f : 0f);
        /* 3 px past the inscribed circle: a panel shows a little past it. */
        GLES20.glUniform1f(GLES20.glGetUniformLocation(prog, "rlim"), 1f + 6f / Math.min(outW, outH));
        GLES20.glUniform1i(GLES20.glGetUniformLocation(prog, "taps"), taps);
        /* the edge line: about 1.5 output pixels, in source fractions */
        GLES20.glUniform1f(GLES20.glGetUniformLocation(prog, "edge"), 1.5f * Math.max(cropW, cropH) / Math.max(1, outW));
        drawQuad(prog);
        rgba.clear();
        GLES20.glReadPixels(0, 0, outW, outH, GLES20.GL_RGBA, GLES20.GL_UNSIGNED_BYTE, rgba);
        if (previewEgl != EGL14.EGL_NO_SURFACE) {
            showPreview();
        }
        UiMenu m = overlay;
        if (m != null) {
            m.draw(rgba, outW, outH);
        }
        java.io.File snap = snapBase;
        if (snap != null) {
            snapBase = null;
            savePng(rgba, outW, outH, new java.io.File(snap.getPath() + "_out.png"));
        }
        boolean want565 = listener.wantsRgb565();
        byte[] i420 = new byte[outW * outH * 3 / 2];
        byte[] rgb565 = want565 ? new byte[outW * outH * 2] : null;
        /* In C when the library has it (about 10x faster than the Java loops, which stay as the fallback). */
        boolean fast = nativeConvert && safeConvert(i420, rgb565);
        if (!fast) {
            i420 = toI420(rgba, outW, outH);
            rgb565 = want565 ? toRgb565(rgba, outW, outH) : null;
        }
        statRenders++;
        long us = (SystemClock.elapsedRealtimeNanos() - t0) / 1000;
        statRenderUs += us;
        if (statRenders % 50 == 1) {
            Log.i(TAG, "ui: render + convert " + us / 1000 + " ms (" + (fast ? "native" : "Java") + ")");
        }
        if (released) {
            return;
        }
        if (want565) {
            listener.onPicture(i420, rgb565, tNs);
        } else {
            listener.onPicture(i420, tNs);
        }
    }

    /* RGB565 big-endian, rounded: what a still patch carries (the gadget's panel order). */
    static byte[] toRgb565(ByteBuffer rgba, int w, int h) {
        byte[] out = new byte[w * h * 2];
        rgba.position(0);
        for (int i = 0, o = 0; i < w * h; i++, o += 2) {
            int r = rgba.get() & 0xff, g = rgba.get() & 0xff, b = rgba.get() & 0xff;
            rgba.get();
            int v = ((r * 31 + 127) / 255) << 11 | ((g * 63 + 127) / 255) << 5 | (b * 31 + 127) / 255;
            out[o] = (byte) (v >> 8);
            out[o + 1] = (byte) v;
        }
        rgba.position(0);
        return out;
    }

    private boolean nativeConvert = true;

    private boolean safeConvert(byte[] i420, byte[] rgb565) {
        try {
            rgba.position(0);
            return X264.rgbaConvert(rgba, outW, outH, i420, rgb565);
        } catch (UnsatisfiedLinkError e) {
            nativeConvert = false;   /* an older library: the Java loops */
            return false;
        }
    }

    static void savePng(ByteBuffer rgba, int w, int h, java.io.File f) {
        android.graphics.Bitmap b = android.graphics.Bitmap.createBitmap(w, h, android.graphics.Bitmap.Config.ARGB_8888);
        rgba.position(0);
        b.copyPixelsFromBuffer(rgba);
        rgba.position(0);
        try (java.io.FileOutputStream o = new java.io.FileOutputStream(f)) {
            b.compress(android.graphics.Bitmap.CompressFormat.PNG, 100, o);
            Log.i(TAG, "ui: saved " + f);
        } catch (java.io.IOException e) {
            Log.w(TAG, "ui: snapshot failed", e);
        }
    }

    private void showPreview() {
        if (!EGL14.eglMakeCurrent(dpy, previewEgl, previewEgl, ctx)) {
            return;
        }
        GLES20.glBindFramebuffer(GLES20.GL_FRAMEBUFFER, 0);
        /* Centered square in the preview, black around it. */
        int s = Math.min(previewW, previewH);
        GLES20.glViewport(0, 0, previewW, previewH);
        GLES20.glClearColor(0, 0, 0, 1);
        GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT);
        GLES20.glViewport((previewW - s) / 2, (previewH - s) / 2, s, s);
        GLES20.glUseProgram(progShow);
        GLES20.glActiveTexture(GLES20.GL_TEXTURE0);
        GLES20.glBindTexture(GLES20.GL_TEXTURE_2D, fboTex);
        GLES20.glUniform1i(GLES20.glGetUniformLocation(progShow, "tex"), 0);
        drawQuad(progShow);   /* FS_SHOW flips: the FBO's row 0 is the picture's top */
        EGL14.eglSwapBuffers(dpy, previewEgl);
        EGL14.eglMakeCurrent(dpy, pbuf, pbuf, ctx);
    }

    private void drawQuad(int p) {
        int a = GLES20.glGetAttribLocation(p, "a");
        quad.position(0);
        GLES20.glEnableVertexAttribArray(a);
        GLES20.glVertexAttribPointer(a, 2, GLES20.GL_FLOAT, false, 0, quad);
        GLES20.glDrawArrays(GLES20.GL_TRIANGLE_STRIP, 0, 4);
    }

    private static int program(String vs, String fs) {
        int p = GLES20.glCreateProgram();
        GLES20.glAttachShader(p, shader(GLES20.GL_VERTEX_SHADER, vs));
        GLES20.glAttachShader(p, shader(GLES20.GL_FRAGMENT_SHADER, fs));
        GLES20.glLinkProgram(p);
        int[] ok = new int[1];
        GLES20.glGetProgramiv(p, GLES20.GL_LINK_STATUS, ok, 0);
        if (ok[0] == 0) {
            Log.e(TAG, "ui: link failed: " + GLES20.glGetProgramInfoLog(p));
        }
        return p;
    }

    private static int shader(int type, String src) {
        int s = GLES20.glCreateShader(type);
        GLES20.glShaderSource(s, src);
        GLES20.glCompileShader(s);
        int[] ok = new int[1];
        GLES20.glGetShaderiv(s, GLES20.GL_COMPILE_STATUS, ok, 0);
        if (ok[0] == 0) {
            Log.e(TAG, "ui: shader failed: " + GLES20.glGetShaderInfoLog(s));
        }
        return s;
    }

    /*
     * RGBA (row 0 = the picture's top) to I420, BT.601 limited range as the
     * gadgets' decoders convert back. Chroma from each 2x2 block's mean.
     */
    static byte[] toI420(ByteBuffer rgba, int w, int h) {
        byte[] out = new byte[w * h * 3 / 2];
        byte[] px = new byte[w * 4 * 2];
        int uo = w * h, vo = uo + w * h / 4, cw = w / 2;
        rgba.position(0);
        for (int y = 0; y < h; y += 2) {
            rgba.get(px, 0, Math.min(2, h - y) * w * 4);
            for (int dy = 0; dy < 2 && y + dy < h; dy++) {
                int ro = dy * w * 4, yo = (y + dy) * w;
                for (int x = 0; x < w; x++) {
                    int r = px[ro + 4 * x] & 0xff, g = px[ro + 4 * x + 1] & 0xff, b = px[ro + 4 * x + 2] & 0xff;
                    out[yo + x] = (byte) (((66 * r + 129 * g + 25 * b + 128) >> 8) + 16);
                }
            }
            int co = (y / 2) * cw;
            for (int x = 0; x < w; x += 2) {
                int i0 = 4 * x, i1 = i0 + 4, i2 = w * 4 + i0, i3 = i2 + 4;
                int r = (px[i0] & 0xff) + (px[i1] & 0xff) + (px[i2] & 0xff) + (px[i3] & 0xff);
                int g = (px[i0 + 1] & 0xff) + (px[i1 + 1] & 0xff) + (px[i2 + 1] & 0xff) + (px[i3 + 1] & 0xff);
                int b = (px[i0 + 2] & 0xff) + (px[i1 + 2] & 0xff) + (px[i2 + 2] & 0xff) + (px[i3 + 2] & 0xff);
                out[uo + co + x / 2] = (byte) (((-38 * r - 74 * g + 112 * b + 512) >> 10) + 128);
                out[vo + co + x / 2] = (byte) (((112 * r - 94 * g - 18 * b + 512) >> 10) + 128);
            }
        }
        return out;
    }
}
