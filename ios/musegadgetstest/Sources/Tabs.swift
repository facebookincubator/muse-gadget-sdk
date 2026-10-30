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
import UIKit

/* One tab: its name and icon in the tab bar, and the sections of its list, in order. */
final class Tab {
    typealias Section = (MainViewController, SettingsList) -> Void
    let title: String, symbol: String
    var sections: [Section] = []
    let list = SettingsList()
    /* The tab owns something that's playing (a dot on its icon). */
    var live: () -> Bool = { false }

    init(_ title: String, symbol: String) {
        self.title = title
        self.symbol = symbol
    }

    @discardableResult func add(_ s: @escaping Section) -> Tab {
        sections.append(s)
        return self
    }

    @discardableResult func live(_ f: @escaping () -> Bool) -> Tab {
        live = f
        return self
    }
}

/*
 * The app's tabs, one per system, and the sections in each: the one place
 * to add a tab, or a section to a tab (Android: Tabs.java). Video comes first.
 */
enum Tabs {
    static func create(_ app: MainViewController) -> [Tab] {
        var tabs: [Tab] = []
        tabs.append(Tab("Video", symbol: "video").live { [weak app] in app?.streamingOrPreparing() ?? false }
            .add(VideoTab.sources)       /* the camera, color bars, the sample clips */
            .add(VideoTab.camera)
            .add(VideoTab.encoding)
            .add(VideoTab.advanced))
        tabs.append(Tab("Gadget", symbol: "applewatch")
            .add(GadgetTab.connection)
            .add(GadgetTab.reports)
            .add(GadgetTab.about))
        return tabs
    }
}
