import SwiftUI

@main
struct CalmApp: App {
    @StateObject private var model = CalmModel()
    var body: some Scene {
        WindowGroup { NavigationStack { CalmView(model: model) }.preferredColorScheme(.dark) }
    }
}

struct CalmView: View {
    @ObservedObject var model: CalmModel
    private let aqua = Color(red: 0.55, green: 0.89, blue: 0.87)
    var body: some View {
        ScrollView {
            VStack(spacing: 12) {
                Text("MUSE CALM").font(.system(size: 10, weight: .medium, design: .rounded)).tracking(3).foregroundStyle(aqua)
                RippleView(active: model.active).frame(height: 76)
                Text(model.active ? model.soundTitle : "Come back to stillness.")
                    .font(.system(size: 21, weight: .medium, design: .serif)).multilineTextAlignment(.center)
                if model.active {
                    Text("\(Int(model.remaining) / 60):\(String(format: "%02d", Int(model.remaining) % 60))")
                        .font(.system(size: 26, weight: .light, design: .rounded)).monospacedDigit().foregroundStyle(aqua)
                    Text(model.phrase).font(.system(size: 15, design: .serif)).multilineTextAlignment(.center).lineSpacing(3)
                    Button("End meditation", role: .destructive) { model.stop() }
                } else {
                    Text(model.status).font(.caption).foregroundStyle(.secondary).multilineTextAlignment(.center)
                    CalmSelection(title: "Time", selection: $model.minutes, options: [
                        CalmOption(1, "1 minute · demo"), CalmOption(5, "5 minutes"),
                        CalmOption(10, "10 minutes"), CalmOption(20, "20 minutes")
                    ]).disabled(model.starting)
                    Button(model.starting ? "Preparing…" : "Begin meditation") { model.start() }
                        .buttonStyle(.borderedProminent).tint(aqua).foregroundStyle(Color.black).disabled(model.starting)
                    if model.starting { Button("Cancel") { model.stop() } }
                }
                VStack(alignment: .leading, spacing: 8) {
                    CalmSelection(title: "Soundscape", selection: $model.scene,
                                  options: CalmScene.allCases.map { CalmOption($0, $0.title) })
                        .disabled(model.active || model.starting)
                    CalmSelection(title: "Voice style", selection: $model.voice, options: [
                        CalmOption("af_heart", "Warm & gentle"), CalmOption("af_nicole", "Soft & airy")
                    ]).disabled(model.active || model.starting)
                    Label("Sound", systemImage: "waveform.path").font(.caption)
                    Slider(value: $model.soundVolume, in: 0...1).tint(aqua)
                    Label("Voice", systemImage: "waveform").font(.caption)
                    Slider(value: $model.voiceVolume, in: 0...1).tint(aqua)
                    CalmSelection(title: "Gentle phrases", selection: $model.phraseInterval, options: [
                        CalmOption(15.0, "Every 15 seconds"), CalmOption(30.0, "Every 30 seconds"),
                        CalmOption(60.0, "Every minute"), CalmOption(0.0, "Sounds only")
                    ])
                }
                if model.active { Text(model.guideStatus).font(.caption2).foregroundStyle(.secondary) }
                if !model.route.isEmpty && model.active { Label(model.route, systemImage: "speaker.wave.2").font(.caption2).foregroundStyle(.secondary) }
                if let error = model.error {
                    Text(error).font(.caption2).foregroundStyle(.orange)
                    if model.active || model.starting { Button("Retry audio") { model.retryAudio() } }
                }
                Text("Fresh ASMR sounds, created as you listen. Gentle words from your Muse.")
                    .font(.system(size: 10)).foregroundStyle(.secondary).multilineTextAlignment(.center)
            }.padding(.horizontal, 7).padding(.bottom, 14)
        }.background(LinearGradient(colors: [Color(red: 0.035, green: 0.12, blue: 0.15), .black], startPoint: .top, endPoint: .bottom).ignoresSafeArea())
    }
}

// Full-screen choices keep long labels readable on small watches and at larger text sizes.
struct CalmOption<Value: Hashable>: Identifiable {
    let id: Value
    let title: String
    init(_ value: Value, _ title: String) { self.id = value; self.title = title }
}

struct CalmSelection<Value: Hashable>: View {
    let title: String
    @Binding var selection: Value
    let options: [CalmOption<Value>]

    var body: some View {
        NavigationLink {
            CalmChoices(title: title, selection: $selection, options: options)
        } label: {
            HStack(spacing: 8) {
                VStack(alignment: .leading, spacing: 4) {
                    Text(title).font(.caption2).foregroundStyle(.secondary)
                    Text(options.first(where: { $0.id == selection })?.title ?? "Choose")
                        .font(.body).fixedSize(horizontal: false, vertical: true)
                }.frame(maxWidth: .infinity, alignment: .leading)
                Image(systemName: "chevron.right").font(.caption2).foregroundStyle(.secondary)
            }
            .multilineTextAlignment(.leading)
            .padding(10)
            .frame(maxWidth: .infinity, minHeight: 54, alignment: .leading)
            .background(.white.opacity(0.09), in: RoundedRectangle(cornerRadius: 14))
            .contentShape(RoundedRectangle(cornerRadius: 14))
        }
        .buttonStyle(.plain)
        .accessibilityLabel(title)
        .accessibilityValue(options.first(where: { $0.id == selection })?.title ?? "Choose")
    }
}

private struct CalmChoices<Value: Hashable>: View {
    let title: String
    @Binding var selection: Value
    let options: [CalmOption<Value>]
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        ScrollView {
            VStack(spacing: 8) {
                ForEach(options) { option in
                    Button {
                        selection = option.id
                        dismiss()
                    } label: {
                        HStack(spacing: 8) {
                            Text(option.title)
                                .font(.body)
                                .fixedSize(horizontal: false, vertical: true)
                                .frame(maxWidth: .infinity, alignment: .leading)
                            if option.id == selection {
                                Image(systemName: "checkmark").foregroundStyle(.cyan)
                            }
                        }
                        .multilineTextAlignment(.leading)
                        .padding(12)
                        .frame(maxWidth: .infinity, minHeight: 50, alignment: .leading)
                        .background(.white.opacity(option.id == selection ? 0.16 : 0.08), in: RoundedRectangle(cornerRadius: 14))
                        .contentShape(RoundedRectangle(cornerRadius: 14))
                    }
                    .buttonStyle(.plain)
                    .accessibilityAddTraits(option.id == selection ? .isSelected : [])
                }
            }.padding(.horizontal, 4).padding(.bottom, 12)
        }
        .navigationTitle(title)
    }
}

struct RippleView: View {
    let active: Bool
    @State private var expanded = false
    var body: some View {
        ZStack {
            ForEach(0..<4) { index in
                Ellipse().stroke(Color.cyan.opacity(0.4 - Double(index) * 0.07), lineWidth: 1)
                    .frame(width: CGFloat(35 + index * 30), height: CGFloat(15 + index * 13))
                    .scaleEffect(active && expanded ? 1.08 : 0.94)
            }
            Image(systemName: "drop.fill").font(.system(size: 25, weight: .ultraLight)).foregroundStyle(Color(red: 0.62, green: 0.93, blue: 0.9)).offset(y: -12)
        }
        .onAppear { expanded = true }
        .animation(active ? .easeInOut(duration: 4).repeatForever(autoreverses: true) : .default, value: expanded)
        .animation(.easeInOut(duration: 1), value: active)
    }
}
