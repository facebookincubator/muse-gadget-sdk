import SwiftUI
import WatchKit
import AVFoundation

final class WatchModel: NSObject, ObservableObject, AVSpeechSynthesizerDelegate {
    let link = WatchLink()
    @Published var conversation = Conversation()
    @Published var draft = ""
    @Published var speaking = false
    @Published var voiceError: String?
    @Published var speakReplies = true
    let heartRate = HeartRateStore()
    let healthReview = HealthReviewStore()
    private let speaker = AVSpeechSynthesizer()
    private var sessionID: String
    private var timeout: Timer?
    private var speechGeneration = 0
    private var currentUtterance: AVSpeechUtterance?

    override init() {
        sessionID = UserDefaults.standard.string(forKey: "sessionID") ?? UUID().uuidString.lowercased()
        super.init()
        UserDefaults.standard.set(sessionID, forKey: "sessionID")
        speaker.delegate = self
        link.onEvent = { [weak self] event in
            guard let self else { return }
            if let reply = conversation.receive(event), speakReplies, !reply.isEmpty { speak(reply) }
            if !conversation.busy { timeout?.invalidate() }
        }
        link.onDisconnect = { [weak self] message in
            self?.conversation.disconnected(message); self?.timeout?.invalidate()
        }
    }
    func dictate() {
        stopSpeaking(); voiceError = nil
        guard let controller = WKApplication.shared().rootInterfaceController else {
            voiceError = "Use the message field and tap its microphone to dictate."; return
        }
        controller.presentTextInputController(withSuggestions: nil, allowedInputMode: .plain) { [weak self] result in
            DispatchQueue.main.async {
                if let text = result?.first as? String { self?.draft = text }
            }
        }
    }
    func send() {
        guard !conversation.busy else { return }
        let text = draft.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !text.isEmpty else { return }
        stopSpeaking()
        do {
            try link.send(["op": "chat", "message": text, "session_id": sessionID])
            conversation.begin(text); draft = ""
            timeout?.invalidate()
            timeout = Timer.scheduledTimer(withTimeInterval: 210, repeats: false) { [weak self] _ in
                guard let self else { return }
                try? link.send(["op": "cancel"])
                conversation.disconnected("Reply timed out. Reconnect before trying again.")
                link.disconnect()
            }
        } catch { conversation.error = error.localizedDescription }
    }
    func askAboutHeartRate() {
        guard let prompt = HeartRateSummary.prompt(readings: heartRate.readings, context: heartRate.context) else { return }
        draft = prompt; send()
    }
    func askAboutHealth() {
        guard !conversation.busy, let prompt = healthReview.prompt else { return }
        speakReplies = true
        draft = prompt; send()
    }
    func cancel() {
        try? link.send(["op": "cancel"])
        conversation.error = "Reply wait canceled."
        conversation.status = "Canceling…"
        stopSpeaking()
    }
    func newChat() {
        guard !conversation.busy else { return }
        stopSpeaking(); sessionID = UUID().uuidString.lowercased()
        UserDefaults.standard.set(sessionID, forKey: "sessionID")
        conversation = Conversation(); draft = ""
    }
    func speak(_ text: String) {
        stopSpeaking(); voiceError = nil
        let generation = speechGeneration
        do {
            let audio = AVAudioSession.sharedInstance()
            try audio.setCategory(.playback, mode: .spokenAudio, options: [.duckOthers])
            audio.activate(options: []) { [weak self] success, error in
                DispatchQueue.main.async {
                    guard let self else { return }
                    guard generation == self.speechGeneration else { return }
                    guard success else { self.voiceError = error?.localizedDescription ?? "Audio playback unavailable"; return }
                    let utterance = AVSpeechUtterance(string: text)
                    utterance.rate = AVSpeechUtteranceDefaultSpeechRate
                    self.currentUtterance = utterance
                    self.speaker.speak(utterance); self.speaking = true
                }
            }
        } catch { voiceError = error.localizedDescription }
    }
    func stopSpeaking() {
        speechGeneration += 1; currentUtterance = nil
        speaker.stopSpeaking(at: .immediate); speaking = false
        try? AVAudioSession.sharedInstance().setActive(false, options: .notifyOthersOnDeactivation)
    }
    func speechSynthesizer(_ synthesizer: AVSpeechSynthesizer, didFinish utterance: AVSpeechUtterance) {
        guard currentUtterance === utterance else { return }
        currentUtterance = nil
        speaking = false; try? AVAudioSession.sharedInstance().setActive(false, options: .notifyOthersOnDeactivation)
    }
    func speechSynthesizer(_ synthesizer: AVSpeechSynthesizer, didCancel utterance: AVSpeechUtterance) {
        guard currentUtterance === utterance else { return }
        currentUtterance = nil; speaking = false
    }
}

@main
struct MuseWatchApp: App {
    @StateObject private var model = WatchModel()
    var body: some Scene { WindowGroup { WatchView(model: model, link: model.link) } }
}

struct WatchView: View {
    @ObservedObject var model: WatchModel
    @ObservedObject var link: WatchLink
    @State private var settings = false
    @State private var showHeartRate = false
    @State private var showHealthReview = false
    var body: some View {
        NavigationStack {
            ScrollView {
                VStack(alignment: .leading, spacing: 12) {
                    Label(link.status, systemImage: link.online ? "checkmark.circle.fill" : "antenna.radiowaves.left.and.right")
                        .font(.caption).foregroundStyle(link.online ? .green : .secondary)
                    if !link.connected {
                        Button(link.scanning ? "Searching…" : "Connect", action: link.scan).disabled(link.scanning)
                        ForEach(link.relays) { relay in Button(relay.name) { link.connect(relay) } }
                    }
                    ForEach(model.conversation.lines) { line in
                        VStack(alignment: .leading, spacing: 3) {
                            Text(line.isUser ? "You" : "Muse").font(.caption2).foregroundStyle(.secondary)
                            Text(line.text.isEmpty ? "…" : line.text).font(.body)
                        }.padding(9).frame(maxWidth: .infinity, alignment: .leading)
                            .background(line.isUser ? Color.blue.opacity(0.22) : Color.gray.opacity(0.18), in: RoundedRectangle(cornerRadius: 12))
                    }
                    if model.conversation.busy {
                        ProgressView(model.conversation.status)
                        Button("Cancel reply", action: model.cancel)
                    }
                    if let error = model.conversation.error { Text(error).font(.caption).foregroundStyle(.orange) }
                    if let error = model.voiceError { Text(error).font(.caption).foregroundStyle(.orange) }
                    TextField("Message Muse", text: $model.draft).disabled(model.conversation.busy)
                    HStack {
                        Button(action: model.dictate) { Image(systemName: "mic.fill") }.accessibilityLabel("Dictate message")
                        Button("Send", action: model.send)
                    }.disabled(!link.online || !link.approved || model.conversation.busy)
                    if model.speaking { Button("Stop speaking", action: model.stopSpeaking) }
                    Button { showHealthReview = true } label: { Label("Health check-in", systemImage: "cross.case.fill") }
                    Button { showHeartRate = true } label: { Label("Heart rate", systemImage: "heart.fill") }
                }.padding(.horizontal, 2)
            }
            .navigationTitle("Muse")
            .toolbar { ToolbarItem(placement: .topBarTrailing) { Button { settings = true } label: { Image(systemName: "ellipsis") } } }
            .sheet(isPresented: $settings) {
                VStack {
                    Toggle("Speak replies", isOn: $model.speakReplies)
                        .onChange(of: model.speakReplies) { _, enabled in if !enabled { model.stopSpeaking() } }
                    Button("New chat") { model.newChat(); settings = false }.disabled(model.conversation.busy)
                    Button("Disconnect") { model.cancel(); link.disconnect(); settings = false }
                    Button("Done") { settings = false }
                }
            }
            .sheet(isPresented: $showHealthReview) {
                HealthReviewView(store: model.healthReview, canSend: link.online && link.approved && !model.conversation.busy) {
                    model.askAboutHealth(); showHealthReview = false
                }
            }
            .sheet(isPresented: $showHeartRate) {
                HeartRateView(store: model.heartRate, canSend: link.online && link.approved && !model.conversation.busy) {
                    model.askAboutHeartRate(); showHeartRate = false
                }
            }
        }
    }
}

struct HeartRateView: View {
    @ObservedObject var store: HeartRateStore
    let canSend: Bool
    let askMuse: () -> Void
    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 12) {
                Label("Heart rate", systemImage: "heart.fill").foregroundStyle(.pink)
                Text(store.status).font(.caption)
                Button(store.loading ? "Reading…" : "Read Apple Health", action: store.load).disabled(store.loading)
                ForEach(store.readings) { sample in
                    HStack {
                        Text("\(sample.bpm, specifier: "%.0f") bpm").bold()
                        Spacer()
                        Text(sample.date, style: .time).font(.caption2)
                    }
                }
                if !store.readings.isEmpty {
                    TextField("Context (rest, exercise…)", text: $store.context)
                    Text("Ask Muse sends these readings, timestamps, and your context through the Mac to your Muse account. This is not a live monitor or a diagnosis.").font(.caption2)
                    Button("Ask Muse to explain", action: askMuse).disabled(!canSend)
                    Button("Clear readings", action: store.clear)
                }
            }
        }.padding(.horizontal, 4)
    }
}


struct HealthReviewView: View {
    @ObservedObject var store: HealthReviewStore
    let canSend: Bool
    let askMuse: () -> Void
    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 12) {
                Label("Health check-in", systemImage: "cross.case.fill").foregroundStyle(.pink)
                Text("Review measurements, sleep, symptoms, health events and workouts. Select what Muse can see.").font(.caption)
                Picker("Review period", selection: $store.days) {
                    Text("7 days").tag(7)
                    Text("30 days").tag(30)
                    Text("90 days").tag(90)
                }.disabled(store.loading)
                Text("Up to 128 recent records per category. Includes only data accessible on this watch; excludes clinical documents, medication lists, ECG waveforms, routes and other specialized records.").font(.caption2)
                Button(store.loading ? "Reading…" : "Read Apple Health", action: store.load).disabled(store.loading)
                Text(store.status).font(.caption)
                if store.loading { Button("Cancel reading", action: store.clear) }
                if !store.loading && !store.rows.isEmpty {
                    Text("Loaded period: \(store.loadedDays) days").font(.caption2)
                    ForEach(store.rows) { row in
                        Toggle(row.title, isOn: Binding(get: { store.selected.contains(row.id) }, set: { enabled in
                            if enabled { store.selected.insert(row.id) } else { store.selected.remove(row.id) }
                        }))
                        Text(row.detail).font(.caption2)
                    }
                    TextField("Symptoms, concerns, medicines…", text: $store.context)
                    Text("Send shares selected summaries, dates and your context through the Mac with your Muse account. They become part of your Muse conversation. Muse gives health information, not a diagnosis, and reads its answer aloud.").font(.caption2)
                    Button("Send & hear check-in", action: askMuse).disabled(!canSend || store.selected.isEmpty)
                    if !canSend { Text("Connect to the Mac relay and wait for Muse to be ready.").font(.caption2) }
                    Button("Clear health summary", action: store.clear)
                }
            }
        }.padding(.horizontal, 4)
    }
}
