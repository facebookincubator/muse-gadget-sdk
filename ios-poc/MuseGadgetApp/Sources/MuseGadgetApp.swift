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
