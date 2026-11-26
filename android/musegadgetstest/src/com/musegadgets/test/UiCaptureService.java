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

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.Service;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.hardware.display.DisplayManager;
import android.hardware.display.VirtualDisplay;
import android.media.projection.MediaProjection;
import android.media.projection.MediaProjectionManager;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.util.DisplayMetrics;
import android.util.Log;
import android.view.Surface;

/**
 * Screen mirroring for a UI stream: a foreground service (Android requires
 * one of type mediaProjection) that holds the MediaProjection the user
 * granted and a VirtualDisplay mirroring the whole screen into UiRenderer's
 * Surface; the renderer crops the chosen square. The screen composes a new
 * frame only when something on it changes.
 */
public final class UiCaptureService extends Service {
    private static final String TAG = "MuseCam";
    static final String EXTRA_CODE = "code", EXTRA_DATA = "data", EXTRA_GEN = "gen";
    private static final String CHANNEL = "mirror";

    /* The one running service's state (same process as the activity). */
    private static volatile UiCaptureService instance;
    private static Surface target;
    private static Runnable onStarted, onStopped;
    private static volatile int targetGen;      /* the session start a grant must belong to */
    private static volatile boolean capturing;  /* the screen is being mirrored now */
    private Runnable stopped;   /* the onStopped of the start that made this service */
    private MediaProjection projection;
    private VirtualDisplay vd;
    private int screenW, screenH;

    /** Where the mirror draws, and for which session start (set before starting the service). */
    static void setTarget(Surface s, Runnable started, Runnable stopped, int gen) {
        target = s;
        onStarted = started;
        onStopped = stopped;
        targetGen = gen;
    }

    static boolean running() {
        return capturing;
    }

    /** The mirrored screen's size in pixels (0 before it starts). */
    static int[] screenSize() {
        UiCaptureService s = instance;
        return s != null ? new int[] {s.screenW, s.screenH} : new int[] {0, 0};
    }

    /** Ends the mirror, and refuses a grant still on its way for the same start. */
    static void stopMirror() {
        capturing = false;
        target = null;
        onStarted = onStopped = null;
        targetGen = 0;
        UiCaptureService s = instance;
        if (s != null) {
            s.stopSelf();
        }
    }

    @Override
    public IBinder onBind(Intent i) {
        return null;
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        NotificationManager nm = getSystemService(NotificationManager.class);
        nm.createNotificationChannel(new NotificationChannel(CHANNEL, "Screen mirroring", NotificationManager.IMPORTANCE_LOW));
        Notification n = new Notification.Builder(this, CHANNEL)
                .setContentTitle("Mirroring to your gadget")
                .setContentText("Muse Gadgets Test is streaming part of your screen")
                .setSmallIcon(android.R.drawable.ic_menu_view)
                .setOngoing(true)
                .build();
        startForeground(7, n, ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PROJECTION);
        instance = this;
        int gen = intent != null ? intent.getIntExtra(EXTRA_GEN, -1) : -1;
        if (gen != targetGen || projection != null) {
            Log.w(TAG, "ui: a screen grant for a mirror that has ended or already runs");
            stopSelf();
            return START_NOT_STICKY;
        }
        stopped = onStopped;
        int code = intent != null ? intent.getIntExtra(EXTRA_CODE, 0) : 0;
        Intent data = intent != null ? intent.getParcelableExtra(EXTRA_DATA, Intent.class) : null;
        if (data == null || target == null) {
            Log.w(TAG, "ui: mirror started without a grant or a target");
            stopSelf();
            return START_NOT_STICKY;
        }
        MediaProjectionManager mpm = getSystemService(MediaProjectionManager.class);
        projection = mpm.getMediaProjection(code, data);
        if (projection == null) {
            stopSelf();
            return START_NOT_STICKY;
        }
        projection.registerCallback(new MediaProjection.Callback() {
            @Override
            public void onStop() {
                Log.i(TAG, "ui: mirror stopped by the system");
                if (instance == UiCaptureService.this) {
                    capturing = false;
                }
                stopSelf();
            }
        }, new Handler(Looper.getMainLooper()));
        DisplayMetrics dm = getResources().getDisplayMetrics();
        android.graphics.Rect b = getSystemService(android.view.WindowManager.class).getMaximumWindowMetrics().getBounds();
        screenW = b.width();
        screenH = b.height();
        vd = projection.createVirtualDisplay("mg-ui-mirror", screenW, screenH, dm.densityDpi,
                DisplayManager.VIRTUAL_DISPLAY_FLAG_AUTO_MIRROR, target, null, null);
        if (vd == null) {
            stopSelf();
            return START_NOT_STICKY;
        }
        capturing = true;
        if (onStarted != null) {
            onStarted.run();
        }
        Log.i(TAG, "ui: mirroring the screen, " + screenW + "x" + screenH);
        return START_NOT_STICKY;
    }

    @Override
    public void onDestroy() {
        if (instance == this) {
            capturing = false;
        }
        if (vd != null) {
            vd.release();
            vd = null;
        }
        if (projection != null) {
            projection.stop();
            projection = null;
        }
        instance = null;
        Runnable r = stopped;
        if (r != null) {
            r.run();
        }
        super.onDestroy();
    }
}
