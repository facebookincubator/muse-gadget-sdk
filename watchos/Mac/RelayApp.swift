import SwiftUI
import AppKit

final class AppDelegate: NSObject, NSApplicationDelegate {
    var model: RelayModel?
    func applicationWillTerminate(_ notification: Notification) { model?.stop() }
}

@main
struct RelayApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) private var delegate
    @StateObject private var model = RelayModel()
    var body: some Scene {
        WindowGroup("Muse Watch Relay") {
            VStack(alignment: .leading, spacing: 18) {
                Label("Muse on your wrist", systemImage: "applewatch.radiowaves.left.and.right")
                    .font(.largeTitle.bold())
                Text("Keep this Mac awake and this app running while you talk to Muse from your watch.")
                    .foregroundStyle(.secondary)
                GroupBox("1 · Connect Muse") {
                    VStack(alignment: .leading, spacing: 10) {
                        Label(model.sdkStatus, systemImage: model.online ? "checkmark.circle.fill" : "network")
                        if !model.paired {
                            SecureField("Muse gadget SDK token", text: $model.sdkToken)
                            Link("Get an SDK token", destination: URL(string: "https://gadgets.muse.ai/settings/sdk-tokens")!)
                            Text("In the Muse iPhone app, enable Settings → Devices → Developer mode, then add \(model.deviceName). Choose the current internet connection.")
                                .font(.callout)
                            HStack {
                                Button(model.pairing ? "Pairing…" : "Start Muse Bluetooth pairing", action: model.pair)
                                    .disabled(model.pairing || (!model.tokenSaved && model.sdkToken.isEmpty))
                                if model.pairing { Button("Cancel", action: model.cancelPair) }
                            }
                        }
                    }.frame(maxWidth: .infinity, alignment: .leading).padding(8)
                }
                GroupBox("2 · Connect your watch") {
                    VStack(alignment: .leading, spacing: 10) {
                        Label(model.bluetoothStatus, systemImage: model.watchConnected ? "checkmark.circle.fill" : "applewatch")
                        Text("Open Muse on your watch, tap Connect, and choose this relay. Approve the Bluetooth pairing prompt if one appears.")
                        if model.needsApproval {
                            Text("Only allow access if you just connected your watch.").font(.callout.bold())
                            Button("Allow my watch", action: model.approveWatch).buttonStyle(.borderedProminent)
                        }
                        Button("Forget approved watch", action: model.forgetWatch)
                    }.frame(maxWidth: .infinity, alignment: .leading).padding(8)
                }
                if let error = model.error { Text(error).foregroundStyle(.red).textSelection(.enabled) }
                Text("Voice input and speech playback happen on the watch. Only text passes through this relay.")
                    .font(.caption).foregroundStyle(.secondary)
            }
            .padding(24).frame(width: 580)
            .onAppear { delegate.model = model }
        }
    }
}
