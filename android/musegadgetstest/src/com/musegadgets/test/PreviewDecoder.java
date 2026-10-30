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

import android.media.MediaCodec;
import android.media.MediaFormat;
import android.os.Handler;
import android.os.HandlerThread;
import android.util.Log;
import android.view.Surface;
import java.nio.ByteBuffer;
import java.util.ArrayDeque;

/**
 * The preview of a stream that doesn't come from the camera (a clip, the
 * color bars): the frames sent to the gadget, decoded by the phone onto
 * the preview, so the screen shows what the gadget is given. Its own
 * thread; frames it can't keep up with are dropped here, never on the way
 * to the gadget.
 */
final class PreviewDecoder {
    private static final String TAG = VideoLink.TAG;
    private static final int MAX_PENDING = 8;

    private HandlerThread thread;
    private Handler h;
    private MediaCodec codec;
    private final ArrayDeque<byte[]> pending = new ArrayDeque<>();
    private final ArrayDeque<Integer> freeInputs = new ArrayDeque<>();
    private boolean keyed;
    private long frames;
    private int size;
    private boolean vp8;
    private volatile boolean shown;

    /** Starts decoding size×size H.264 or VP8 onto s (stops what ran before). */
    void start(Surface s, int size, boolean vp8) {
        stop();
        if (s != null) {
            synchronized (this) {
                open(s, size, vp8);
            }
        }
    }

    private void open(Surface s, int size, boolean vp8) {
        this.size = size;
        this.vp8 = vp8;
        thread = new HandlerThread("preview");
        thread.start();
        h = new Handler(thread.getLooper());
        try {
            String mime = vp8 ? MediaFormat.MIMETYPE_VIDEO_VP8 : MediaFormat.MIMETYPE_VIDEO_AVC;
            MediaFormat f = MediaFormat.createVideoFormat(mime, size, size);
            f.setInteger(MediaFormat.KEY_LOW_LATENCY, 1);
            MediaCodec c = MediaCodec.createDecoderByType(mime);
            c.setCallback(new MediaCodec.Callback() {
                @Override
                public void onInputBufferAvailable(MediaCodec mc, int index) {
                    synchronized (PreviewDecoder.this) {
                        if (codec != mc) {
                            return;
                        }
                        freeInputs.add(index);
                        pump();
                    }
                }

                @Override
                public void onOutputBufferAvailable(MediaCodec mc, int index, MediaCodec.BufferInfo info) {
                    synchronized (PreviewDecoder.this) {
                        if (codec != mc) {
                            return;
                        }
                        try {
                            mc.releaseOutputBuffer(index, true);
                            shown = true;
                        } catch (IllegalStateException e) {
                            // stopping
                        }
                    }
                }

                @Override
                public void onError(MediaCodec mc, MediaCodec.CodecException e) {
                    Log.w(TAG, "preview decoder: " + e);
                }

                @Override
                public void onOutputFormatChanged(MediaCodec mc, MediaFormat format) {}
            }, h);
            c.configure(f, s, null, 0);
            codec = c;
            c.start();
        } catch (Exception e) {
            Log.w(TAG, "preview decoder", e);
            codec = null;
            thread.quitSafely();
            thread = null;
            h = null;
        }
    }

    /** Running for this picture? */
    synchronized boolean running(int size, boolean vp8) {
        return codec != null && this.size == size && this.vp8 == vp8;
    }

    /** Something has been drawn since start(). */
    boolean shown() {
        return codec != null && shown;
    }

    /** One frame as sent (VideoFrame header included); waits for a keyframe after start(). */
    synchronized void feed(byte[] frame, boolean key) {
        if (codec == null || frame.length <= VideoFrame.HEADER_LEN) {
            return;
        }
        if (!keyed && !key) {
            return;
        }
        keyed = true;
        if (pending.size() >= MAX_PENDING) {
            pending.clear();   /* fell behind: wait for the next keyframe rather than show garbage */
            keyed = false;
            return;
        }
        pending.add(frame);
        pump();
    }

    /* Under the lock: frames into free input buffers. */
    private void pump() {
        while (codec != null && !pending.isEmpty() && !freeInputs.isEmpty()) {
            byte[] fr = pending.poll();
            int idx = freeInputs.poll();
            try {
                ByteBuffer b = codec.getInputBuffer(idx);
                int n = fr.length - VideoFrame.HEADER_LEN;
                if (b == null || b.capacity() < n) {
                    freeInputs.addFirst(idx);
                    continue;
                }
                b.clear();
                b.put(fr, VideoFrame.HEADER_LEN, n);
                codec.queueInputBuffer(idx, 0, n, frames++ * 33_333L, 0);
            } catch (IllegalStateException e) {
                return;
            }
        }
    }

    /* The codec is released outside the lock: its callbacks take the lock, and stop() waits for them. */
    void stop() {
        MediaCodec c;
        HandlerThread t;
        synchronized (this) {
            c = codec;
            t = thread;
            codec = null;
            thread = null;
            h = null;
            pending.clear();
            freeInputs.clear();
            keyed = false;
            shown = false;
            frames = 0;
        }
        if (c != null) {
            try {
                c.stop();
            } catch (Exception e) {
                // already stopped
            }
            c.release();
        }
        if (t != null) {
            t.quitSafely();
        }
    }
}
