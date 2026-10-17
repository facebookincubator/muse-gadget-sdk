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
package com.musegadgets.camstream;

/**
 * Open sample videos for the clip buttons: short excerpts of the Blender
 * Foundation's open movies (CC BY 3.0), which the phone streams from the
 * Internet Archive when one is first played, decodes and re-encodes for the
 * gadget (ClipStreamer). None of them is in this repository or the app; only
 * the excerpt's few megabytes are fetched. Their credits show while they play.
 */
final class SampleClips {
    static final class Sample {
        final String name, label, url, credit;
        final int startS, seconds, fps;

        Sample(String name, String label, String url, int startS, int seconds, int fps, String credit) {
            this.name = name;
            this.label = label;
            this.url = url;
            this.startS = startS;
            this.seconds = seconds;
            this.fps = fps;
            this.credit = credit;
        }

        /** The movie on the network, read by the extractor with range requests (only the excerpt's part). */
        ClipStreamer.Source source() {
            return ex -> ex.setDataSource(url, java.util.Collections.singletonMap("User-Agent", "MuseCamera"));
        }
    }

    static final Sample[] ALL = {
        new Sample("bunny", "Bunny",
                "https://archive.org/download/BigBuckBunny_124/Content/big_buck_bunny_720p_surround.mp4", 104, 8, 24,
                "Big Buck Bunny, (c) copyright 2008 Blender Foundation, www.bigbuckbunny.org, CC BY 3.0"),
        new Sample("sintel", "Sintel", "https://archive.org/download/Sintel/sintel-2048-stereo.mp4", 266, 8, 24,
                "Sintel, (c) copyright Blender Foundation, durian.blender.org, CC BY 3.0"),
    };

    static Sample get(String name) {
        for (Sample s : ALL) {
            if (s.name.equals(name)) {
                return s;
            }
        }
        return null;
    }

    private SampleClips() {}
}
