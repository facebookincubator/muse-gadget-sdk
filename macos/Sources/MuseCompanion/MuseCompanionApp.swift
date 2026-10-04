// Copyright (c) Meta Platforms, Inc. and affiliates.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
import SwiftUI
import AppKit

@main
struct MuseCompanionApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) var delegate
    @StateObject private var model = ChatModel()
    var body: some Scene {
        WindowGroup("Muse Companion") {
            CompanionView(model: model)
                .preferredColorScheme(.dark)
                .onAppear { model.checkConnection(); model.runDebugProbe() }
                .onReceive(NotificationCenter.default.publisher(for: NSApplication.willTerminateNotification)) { _ in model.stop() }
        }.defaultSize(width: 1120, height: 760)
        .commands { CommandGroup(replacing: .newItem) {
            Button("New Chat") { model.newChat() }.keyboardShortcut("n").disabled(model.busy)
        } }
        Settings {
            Form {
                Text(model.paired ? "This Mac is paired with Muse." : "Pair this Mac directly with the Muse phone app.")
                Text("Bluetooth pairs the device; your Mac's internet connection carries the conversation.")
                    .font(.caption).foregroundStyle(.secondary)
                Button("Reconnect") { model.checkConnection() }
                if !model.paired { Button("Pair this Mac") { model.showPairing = true } }
            }.padding(24).frame(width: 420)
        }
    }
}

final class AppDelegate: NSObject, NSApplicationDelegate {
    func applicationDidFinishLaunching(_ notification: Notification) {
        NSApp.setActivationPolicy(.regular)
        NSApp.activate(ignoringOtherApps: true)
    }
    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { true }
}

private let violet = Color(red: 0.69, green: 0.53, blue: 1)

struct CompanionView: View {
    @ObservedObject var model: ChatModel
    @State private var spinning = DebugProbe.spinning
    @State private var motion = MascotMotion.hover
    var body: some View {
        HStack(spacing: 0) {
            mascotPanel.frame(minWidth: 330, idealWidth: 400, maxWidth: 430)
            Rectangle().fill(.white.opacity(0.07)).frame(width: 1)
            chatPanel.frame(minWidth: 480, maxWidth: .infinity)
        }
        .background(Color(red: 0.055, green: 0.048, blue: 0.09))
        .frame(minWidth: 850, minHeight: 640)
        .sheet(isPresented: $model.showPairing) { pairingView }
    }
    private var mascotPanel: some View {
        VStack(spacing: 0) {
            HStack {
                Image(systemName: "sparkles").foregroundStyle(violet)
                Text("MUSE").font(.system(size: 14, weight: .bold, design: .rounded)).tracking(4)
                Spacer()
                Text("COMPANION").font(.system(size: 9, weight: .medium)).tracking(2).foregroundStyle(.secondary)
            }.padding(28)
            Spacer(minLength: 0)
            ZStack {
                Circle().fill(RadialGradient(colors: [violet.opacity(0.18), .clear], center: .center, startRadius: 30, endRadius: 160)).frame(width: 320, height: 320)
                MascotView(spinning: spinning, thinking: model.busy, speaking: model.answering, motion: motion).frame(height: 360)
            }
            VStack(spacing: 12) {
                Text("A little presence.\nA world of possibility.")
                    .font(.system(size: 28, weight: .semibold, design: .rounded)).multilineTextAlignment(.center)
                Text("Your Muse, right here on your Mac.")
                    .font(.system(size: 13)).foregroundStyle(.secondary)
                Button { spinning.toggle() } label: {
                    Label(spinning ? "Pause rotation" : "Resume rotation", systemImage: spinning ? "pause" : "arrow.triangle.2.circlepath")
                        .font(.system(size: 11, weight: .medium)).padding(.horizontal, 13).padding(.vertical, 8)
                }.buttonStyle(.plain).background(.white.opacity(0.055), in: Capsule())
                HStack(spacing: 12) {
                    Picker("Motion", selection: $motion) {
                        ForEach(MascotMotion.allCases) { Text($0.rawValue).tag($0) }
                    }.pickerStyle(.menu).labelsHidden().frame(width: 135)
                    Button { motion = MascotMotion.allCases.filter { $0 != motion }.randomElement() ?? .hover } label: {
                        Label("Shuffle", systemImage: "dice")
                    }.buttonStyle(.plain).font(.system(size: 11)).help("Pick a random quirky motion")
                }
                Text("Drag to orbit · Scroll to zoom").font(.system(size: 10)).foregroundStyle(.tertiary)
            }
            Spacer(minLength: 24)
            HStack(spacing: 8) {
                Circle().fill(model.online ? Color.green.opacity(0.8) : Color.orange).frame(width: 6, height: 6)
                Text(model.status).font(.system(size: 11)).foregroundStyle(.secondary).lineLimit(2)
                Spacer()
                Button { model.checkConnection() } label: { Image(systemName: "arrow.clockwise") }.buttonStyle(.plain).disabled(model.busy)
            }.padding(24)
        }
        .background(LinearGradient(colors: [Color(red: 0.13, green: 0.10, blue: 0.22), Color(red: 0.07, green: 0.055, blue: 0.12)], startPoint: .topLeading, endPoint: .bottomTrailing))
    }
    private var chatPanel: some View {
        VStack(spacing: 0) {
            HStack {
                VStack(alignment: .leading, spacing: 5) {
                    Text("Talk to your Muse").font(.system(size: 23, weight: .semibold))
                    Text("Ask a question. Follow your curiosity.").font(.system(size: 12)).foregroundStyle(.secondary)
                }
                Spacer()
                Button { model.newChat() } label: { Image(systemName: "square.and.pencil").font(.system(size: 17)).padding(10) }
                    .buttonStyle(.plain).help("New chat").disabled(model.busy)
                SettingsLink { Image(systemName: "slider.horizontal.3").padding(10) }.buttonStyle(.plain)
                if !model.paired { Button("Pair this Mac") { model.showPairing = true } }
            }.padding(28)
            ScrollViewReader { proxy in
                ScrollView {
                    VStack(alignment: .leading, spacing: 24) {
                        if model.messages.isEmpty { welcome }
                        ForEach(model.messages) { message in bubble(message).id(message.id) }
                        if model.busy {
                            HStack(spacing: 10) { ProgressView().controlSize(.small); Text(model.status).font(.system(size: 12)).foregroundStyle(.secondary) }
                        }
                        Color.clear.frame(height: 1).id("bottom")
                    }.padding(.horizontal, 28).padding(.vertical, 16).frame(maxWidth: .infinity, alignment: .leading)
                }
                .onChange(of: model.messages.map(\.text)) { _, _ in proxy.scrollTo("bottom", anchor: .bottom) }
                .onChange(of: model.busy) { _, _ in proxy.scrollTo("bottom", anchor: .bottom) }
            }
            if let error = model.error {
                HStack(alignment: .top) {
                    Image(systemName: "exclamationmark.circle")
                    Text(error).textSelection(.enabled)
                    Spacer()
                    Button { model.error = nil } label: { Image(systemName: "xmark") }.buttonStyle(.plain)
                }.font(.system(size: 12)).foregroundStyle(Color.orange).padding(14)
                    .background(Color.orange.opacity(0.06), in: RoundedRectangle(cornerRadius: 12)).padding(.horizontal, 28)
            }
            VStack(spacing: 12) {
                ZStack(alignment: .topLeading) {
                    if model.prompt.isEmpty { Text("Ask Muse anything…").foregroundStyle(.tertiary).padding(.horizontal, 6).padding(.vertical, 9).allowsHitTesting(false) }
                    TextEditor(text: $model.prompt).scrollContentBackground(.hidden).font(.system(size: 14)).frame(minHeight: 58, maxHeight: 100)
                }
                HStack {
                    Text("Replies from your connected Muse").font(.system(size: 10)).foregroundStyle(.tertiary)
                    Spacer()
                    if model.busy { Button("Cancel") { model.cancel() }.buttonStyle(.plain).font(.system(size: 12)) }
                    Button { model.send() } label: {
                        HStack(spacing: 8) { Text("Send"); Image(systemName: "arrow.up") }.font(.system(size: 12, weight: .semibold)).padding(.horizontal, 15).padding(.vertical, 9)
                    }.buttonStyle(.plain).background(model.canSend ? violet : .white.opacity(0.08), in: Capsule())
                        .foregroundStyle(model.canSend ? Color.black : Color.gray).disabled(!model.canSend)
                        .keyboardShortcut(.return, modifiers: .command)
                }
            }.padding(16).background(.white.opacity(0.035), in: RoundedRectangle(cornerRadius: 18))
                .overlay(RoundedRectangle(cornerRadius: 18).stroke(.white.opacity(0.09), lineWidth: 1)).padding(28)
        }
    }
    private var pairingView: some View {
        VStack(alignment: .leading, spacing: 18) {
            Text("Pair this Mac").font(.title2.bold())
            Text("Use the Muse app on your phone to add this Mac as a device. Allow Bluetooth when macOS asks.")
            Text(model.deviceName).font(.headline).textSelection(.enabled)
            if !model.pairing {
                if model.sdkTokenSaved { Text("Your SDK token is saved on this Mac.").font(.callout).foregroundStyle(.secondary) }
                else {
                    SecureField("Muse SDK token", text: $model.sdkToken).textFieldStyle(.roundedBorder)
                    Link("Get your SDK token", destination: URL(string: "https://gadgets.muse.ai/settings/sdk-tokens")!)
                }
                Button("Start pairing") { model.pair() }.disabled(!model.sdkTokenSaved && model.sdkToken.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty)
            } else {
                ProgressView().controlSize(.small)
                Text(model.pairingStatus).textSelection(.enabled)
                Button("Stop pairing") { model.cancelPairing() }
            }
            if let error = model.error { Text(error).foregroundStyle(.orange).textSelection(.enabled) }
            Text("In Muse: Settings → Devices → Developer mode → Add Device. Choose the name shown above and approve pairing.")
                .font(.callout).foregroundStyle(.secondary)
            Button("Close") { model.showPairing = false }
        }.padding(28).frame(width: 490)
    }
    private var welcome: some View {
        VStack(alignment: .leading, spacing: 16) {
            Image(systemName: "sparkle").font(.system(size: 29)).foregroundStyle(violet).padding(.top, 25)
            Text("What’s on your mind?").font(.system(size: 21, weight: .medium))
            Text("Get a quick answer, explore an idea, or catch up on the world. Your conversation continues in your Muse account.")
                .font(.system(size: 13)).foregroundStyle(.secondary).lineSpacing(5)
            ForEach(["What are today's biggest news stories? Include sources.", "Explain something interesting about AI this week.", "Help me plan a creative project for this weekend."], id: \.self) { suggestion in
                Button { model.prompt = suggestion } label: {
                    HStack { Text(suggestion).multilineTextAlignment(.leading); Spacer(); Image(systemName: "arrow.up.left") }
                        .font(.system(size: 12)).padding(14).frame(maxWidth: .infinity, alignment: .leading)
                }.buttonStyle(.plain).background(.white.opacity(0.035), in: RoundedRectangle(cornerRadius: 11))
            }
        }.padding(.bottom, 20)
    }
    private func bubble(_ message: ChatMessage) -> some View {
        VStack(alignment: .leading, spacing: 9) {
            HStack(spacing: 7) {
                Image(systemName: message.user ? "person.crop.circle" : "sparkles")
                Text(message.user ? "YOU" : "MUSE").tracking(1.5)
            }.font(.system(size: 10, weight: .semibold)).foregroundStyle(message.user ? .secondary : violet)
            if message.text.isEmpty {
                Text("…").foregroundStyle(.secondary)
            } else {
                Text(LocalizedStringKey(message.text)).font(.system(size: 14)).lineSpacing(5).textSelection(.enabled)
                    .frame(maxWidth: .infinity, alignment: .leading)
            }
        }.padding(message.user ? 15 : 0)
            .background(message.user ? Color.white.opacity(0.035) : Color.clear, in: RoundedRectangle(cornerRadius: 13))
    }
}
