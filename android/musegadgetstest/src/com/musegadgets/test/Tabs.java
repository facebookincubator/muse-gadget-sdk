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

import java.util.ArrayList;
import java.util.List;

/**
 * The app's tabs, one per system, and the sections in each: the one place
 * to add a tab, or a section to a tab. Video comes first.
 */
final class Tabs {
    private Tabs() {}

    static List<Tab> create(MainActivity app) {
        List<Tab> tabs = new ArrayList<>();
        tabs.add(new Tab("Video", Icons.VIDEO).live(app::streamingOrPreparing)
                .add(VideoTab::sources)       /* the camera, color bars, the sample clips */
                .add(VideoTab::camera)
                .add(VideoTab::encoding)
                .add(VideoTab::advanced));
        tabs.add(new Tab("Gadget", Icons.GADGET)
                .add(GadgetTab::connection)
                .add(GadgetTab::reports)
                .add(GadgetTab::about));
        return tabs;
    }
}
