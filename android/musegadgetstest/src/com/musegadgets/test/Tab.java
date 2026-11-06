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
 * One tab of the app: its name and icon in the tab bar, and the sections
 * of its list, in order. Tabs (the registration point) puts them together.
 */
final class Tab {
    /** Adds a section (a header and its rows) to a tab's list. */
    interface Section {
        void build(MainActivity app, Ui.SettingsList list);
    }

    final String title;
    final Icons.Glyph icon;
    final List<Section> sections = new ArrayList<>();
    Ui.SettingsList list;
    /* The tab owns something that's playing (a dot on its icon). */
    java.util.function.BooleanSupplier live = () -> false;

    Tab(String title, Icons.Glyph icon) {
        this.title = title;
        this.icon = icon;
    }

    Tab add(Section s) {
        sections.add(s);
        return this;
    }

    Tab live(java.util.function.BooleanSupplier f) {
        live = f;
        return this;
    }
}
