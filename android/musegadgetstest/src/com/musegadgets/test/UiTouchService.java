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

import android.accessibilityservice.AccessibilityService;
import android.accessibilityservice.GestureDescription;
import android.graphics.Path;
import android.os.SystemClock;
import android.util.Log;
import android.view.accessibility.AccessibilityEvent;

import java.util.ArrayDeque;

/**
 * Touches from the gadget into whatever app is on the phone's screen, for
 * the mirrored window (protocols/UI.md, Touch events): an accessibility
 * service (the user turns it on once, Settings > Accessibility > Muse Gadgets
 * Test touch) that replays each touch as a continued gesture stroke. Points
 * queue while a segment plays; each completed segment starts the next from
 * where it ended. No HID pairing needed, Android only.
 */
public final class UiTouchService extends AccessibilityService {
    private static final String TAG = "MuseCam";
    private static volatile UiTouchService instance;

    /* A queued touch in screen pixels, with the generation it was injected under. */
    private static final class Pt {
        final int action, gen;
        final float x, y;

        Pt(int action, float x, float y, int gen) {
            this.action = action;
            this.x = x;
            this.y = y;
            this.gen = gen;
        }
    }

    private final ArrayDeque<Pt> q = new ArrayDeque<>();
    private GestureDescription.StrokeDescription stroke;
    private float lastX, lastY;
    private long downMs;
    private boolean busy;

    static boolean enabled() {
        return instance != null;
    }

    /** A global action (GLOBAL_ACTION_BACK, _HOME, _RECENTS). False: the service isn't on. */
    static boolean globalAction(int action) {
        UiTouchService s = instance;
        if (s == null) {
            return false;
        }
        int g = gen;
        s.getMainExecutor().execute(() -> {
            if (g == gen && UiCaptureService.running()) {   /* only for the mirror, while the screen is shared */
                s.performGlobalAction(action);
            }
        });
        return true;
    }

    /** A touch at screen pixels (x, y), Mg.TOUCH_*, while the screen is shared and no cancel() came after generation g. False: the service isn't on. */
    static boolean inject(int g, int action, float x, float y) {
        UiTouchService s = instance;
        if (s == null) {
            return false;
        }
        s.getMainExecutor().execute(() -> {
            if (g == gen && UiCaptureService.running()) {   /* again: a cancel() may have run since the caller looked */
                s.add(action, x, y, g);
            }
        });
        return true;
    }

    /* Moves on at every cancel(): a touch or action posted before it doesn't play. */
    private static volatile int gen;

    /** The generation a touch is checked under: pass it to inject(). */
    static int generation() {
        return gen;
    }

    /** Drops queued touches and lifts a finger still down. */
    static void cancel() {
        gen++;
        UiTouchService s = instance;
        if (s == null) {
            return;
        }
        s.getMainExecutor().execute(() -> {
            s.q.clear();
            if (s.stroke != null) {
                s.add(Mg.TOUCH_UP, s.lastX, s.lastY, gen);   /* the lift itself belongs to the new generation */
            }
        });
    }

    @Override
    protected void onServiceConnected() {
        instance = this;
        Log.i(TAG, "ui: touch service on");
    }

    @Override
    public boolean onUnbind(android.content.Intent intent) {
        instance = null;
        return super.onUnbind(intent);
    }

    @Override
    public void onAccessibilityEvent(AccessibilityEvent e) {}

    @Override
    public void onInterrupt() {}

    private void add(int action, float x, float y, int g) {
        if (action == Mg.TOUCH_MOVE && !q.isEmpty() && q.peekLast().action == Mg.TOUCH_MOVE) {
            q.pollLast();   /* only the newest move matters while a segment plays */
        }
        q.add(new Pt(action, x, y, g));
        next();
    }

    private void next() {
        /* A touch queued under an older generation (touch revoked since: cancel()) never plays, even if a
         * segment completes before cancel()'s own clear runs. */
        while (!q.isEmpty() && q.peek().gen != gen) {
            q.poll();
        }
        if (busy || q.isEmpty()) {
            return;
        }
        Pt p = q.poll();
        int action = p.action;
        Path path = new Path();
        GestureDescription.StrokeDescription s;
        if (action == Mg.TOUCH_DOWN) {
            Pt up = !q.isEmpty() && q.peek().action == Mg.TOUCH_UP && q.peek().gen == p.gen ? q.poll() : null;
            path.moveTo(p.x, p.y);
            if (up != null) {
                /* A tap whose up already arrived: one short stroke. */
                path.lineTo(up.x, up.y);
                s = new GestureDescription.StrokeDescription(path, 0, 50, false);
                stroke = null;
            } else {
                downMs = SystemClock.uptimeMillis();
                s = new GestureDescription.StrokeDescription(path, 0, 16, true);
                stroke = s;
            }
        } else if (stroke == null) {
            /* A move or up without a stroke (its down was lost): drop it, go on. */
            getMainExecutor().execute(this::next);
            return;
        } else {
            path.moveTo(lastX, lastY);
            path.lineTo(p.x, p.y);
            boolean more = action != Mg.TOUCH_UP;
            s = stroke.continueStroke(path, 0, 16, more);
            stroke = more ? s : null;
        }
        lastX = p.x;
        lastY = p.y;
        busy = true;
        boolean ok = dispatchGesture(new GestureDescription.Builder().addStroke(s).build(), new GestureResultCallback() {
            @Override
            public void onCompleted(GestureDescription g) {
                busy = false;
                next();
            }

            @Override
            public void onCancelled(GestureDescription g) {
                Log.w(TAG, "ui: touch gesture cancelled");
                busy = false;
                stroke = null;
                next();
            }
        }, null);
        if (!ok) {
            Log.w(TAG, "ui: touch gesture refused");
            busy = false;
            stroke = null;
        }
    }
}
