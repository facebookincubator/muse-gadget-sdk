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
import SwiftUI

@main
struct MuseGadgetApp: App {
    @StateObject private var model = GadgetModel()
    @Environment(\.scenePhase) private var scenePhase

    var body: some Scene {
        WindowGroup {
            ContentView(model: model)
                .onChange(of: scenePhase) { _, phase in
                    // iOS suspends the app, and its WebSocket, soon after it leaves the
                    // foreground. Keep the screen on and reconnect on return.
                    UIApplication.shared.isIdleTimerDisabled = phase == .active
                    if phase == .active { model.resume() }
                }
        }
    }
}
