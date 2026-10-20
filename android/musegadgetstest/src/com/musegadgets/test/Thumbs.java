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
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.drawable.BitmapDrawable;
import android.graphics.drawable.Drawable;
import android.media.MediaMetadataRetriever;
import android.util.Log;
import java.io.File;
import java.io.FileOutputStream;
import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/**
 * Thumbnails for the clip rows: one frame of each clip, taken in the
 * background the first time it's asked for and kept in the app's cache
 * (a sample clip's costs a few range requests, once). get() returns null
 * until it's ready; the list's next refresh picks it up.
 */
final class Thumbs {
    private Thumbs() {}

    /** Opens the clip in a retriever. */
    interface Source {
        void open(MediaMetadataRetriever r) throws Exception;
    }

    private static final Map<String, Drawable> done = new ConcurrentHashMap<>();
    private static final Map<String, Boolean> asked = new ConcurrentHashMap<>();
    private static final ExecutorService pool = Executors.newSingleThreadExecutor(r -> {
        Thread t = new Thread(r, "thumbs");
        t.setPriority(Thread.MIN_PRIORITY);
        return t;
    });

    /** The thumbnail for key (a frame at atUs), or null while it's being made (or failed). */
    static Drawable get(Context c, String key, long atUs, Source src) {
        Drawable d = done.get(key);
        if (d != null || asked.putIfAbsent(key, true) != null) {
            return d;
        }
        Context app = c.getApplicationContext();
        pool.execute(() -> {
            File f = new File(app.getCacheDir(), "thumb-" + key + ".jpg");
            Bitmap b = f.exists() ? BitmapFactory.decodeFile(f.getPath()) : null;
            if (b == null) {
                try (MediaMetadataRetriever r = new MediaMetadataRetriever()) {
                    src.open(r);
                    b = r.getScaledFrameAtTime(atUs, MediaMetadataRetriever.OPTION_CLOSEST_SYNC, 256, 256);
                    if (b != null) {
                        try (FileOutputStream out = new FileOutputStream(f)) {
                            b.compress(Bitmap.CompressFormat.JPEG, 85, out);
                        }
                    }
                } catch (Exception e) {
                    Log.w(VideoLink.TAG, "thumbnail " + key + ": " + e);
                }
            }
            if (b != null) {
                done.put(key, new BitmapDrawable(app.getResources(), b));
            }
        });
        return null;
    }
}
