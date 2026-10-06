import SwiftUI
import AVFoundation
import WatchKit
import MediaPlayer

final class CalmModel: NSObject, ObservableObject, AVAudioPlayerDelegate {
    @Published var active = false
    @Published var starting = false
    @Published var remaining: TimeInterval = 0
    @Published var minutes = 5
    @Published var scene: CalmScene = .mix
    @Published var voice = "af_heart"
    @Published var phraseInterval = 15.0 {
        didSet {
            if phraseInterval == 0 { silenceVoice() }
            else if active || starting { requestGuide(); fillVoice() }
        }
    }
    @Published var soundVolume = 0.55 { didSet { adjustVolumes() } }
    @Published var voiceVolume = 0.8 {
        didSet {
            voicePlayer?.volume = Float(voiceVolume)
            if voiceVolume == 0 { silenceVoice() }
        }
    }
    @Published var status = "A little space to breathe."
    @Published var phrase = "Let the world wait a moment."
    @Published var guideStatus = "Fresh sounds and a gentle voice"
    @Published var soundTitle = "ASMR mix"
    @Published var readySounds = 0
    @Published var route = ""
    @Published var error: String?
    @Published var speaking = false
    private let cloud = CloudLink()
    private let audio = CalmAudioService()
    private var soundQueue: [GeneratedClip] = []
    private var voiceQueue: [GeneratedClip] = []
    private var savedSoundIDs = Set<String>()
    private var savedVoiceIDs = Set<String>()
    private var phrases: [String] = []
    private var soundRecipes: [String: CalmSoundRecipe] = [:]
    private var usedPhrases = Set<String>()
    private var soundPlayer: AVAudioPlayer?
    private var fadingPlayer: AVAudioPlayer?
    private var voicePlayer: AVAudioPlayer?
    private var fadeDuration = CalmBufferPolicy.crossfadeSeconds
    private var fadeStarted: Date?
    private var duckGain: Float = 1
    private var soundPending = false
    private var voicePending = false
    private var soundFailed = false
    private var voiceFailed = false
    private var soundRetry = CalmAudioRetry()
    private var voiceRetry = CalmAudioRetry()
    private var soundRetryTask: DispatchWorkItem?
    private var voiceRetryTask: DispatchWorkItem?
    private var soundError: String?
    private var voiceError: String?
    private var sceneIndex = 0
    private var clock: CalmClock?
    private var timer: Timer?
    private var generation = UUID()
    private var guideSession = UUID().uuidString.lowercased()
    private var replies: [String: String] = [:]
    private var replyOrder: [String] = []
    private var failedGuide = false
    private var guidePending = false
    private var nextGuideAttempt = Date.distantPast
    private var observers: [NSObjectProtocol] = []
    private var remoteStop: Any?

    override init() {
        super.init()
        cloud.onEvent = { [weak self] event in self?.receive(event) }
        cloud.onFailure = { [weak self] _ in self?.guideUnavailable() }
        observers.append(NotificationCenter.default.addObserver(forName: AVAudioSession.interruptionNotification, object: nil, queue: .main) { [weak self] note in
            guard let value = note.userInfo?[AVAudioSessionInterruptionTypeKey] as? UInt,
                  AVAudioSession.InterruptionType(rawValue: value) == .began else { return }
            self?.stop(message: "Session interrupted. Start again when you're ready.")
        })
        observers.append(NotificationCenter.default.addObserver(forName: AVAudioSession.routeChangeNotification, object: nil, queue: .main) { [weak self] note in
            guard let self else { return }
            self.updateRoute()
            if let value = note.userInfo?[AVAudioSessionRouteChangeReasonKey] as? UInt,
               AVAudioSession.RouteChangeReason(rawValue: value) == .oldDeviceUnavailable, self.active || self.starting {
                self.stop(message: "Audio disconnected. Start again when you're ready.")
            }
        })
        let commands = MPRemoteCommandCenter.shared()
        commands.pauseCommand.isEnabled = true
        remoteStop = commands.pauseCommand.addTarget { [weak self] _ in
            DispatchQueue.main.async { self?.stop() }
            return .success
        }
    }

    func start() {
        guard !active, !starting else { return }
        generation = UUID(); let request = generation
        starting = true; error = nil; status = "Creating your first fresh sound…"
        soundTitle = scene.title; phrase = "Let the world wait a moment."
        guideSession = UUID().uuidString.lowercased()
        sceneIndex = 0; soundFailed = false; voiceFailed = false
        soundRetry.reset(); voiceRetry.reset(); soundError = nil; voiceError = nil
        phrases = []; soundRecipes = [:]; usedPhrases = []; replies = [:]; replyOrder = []
        failedGuide = false; guidePending = false; nextGuideAttempt = .distantPast
        guideStatus = "Muse is preparing your guide…"
        audio.trimCache()
        soundQueue = audio.savedSounds(for: scene)
        voiceQueue = phraseInterval > 0 ? audio.savedVoice(for: voice) : []
        savedSoundIDs = Set(soundQueue.map(\.id)); savedVoiceIDs = Set(voiceQueue.map(\.id))
        usedPhrases = Set(voiceQueue.compactMap { $0.text?.lowercased() })
        readySounds = soundQueue.count
        if !soundQueue.isEmpty { status = "Starting your soundscape…" }
        do {
            let session = AVAudioSession.sharedInstance()
            try session.setCategory(.playback, mode: .default, policy: .longFormAudio, options: [])
            session.activate(options: []) { [weak self] success, activationError in
                DispatchQueue.main.async {
                    guard let self, self.generation == request, self.starting else { return }
                    guard success else {
                        self.stop(message: "Choose an audio output, then try again.")
                        self.error = activationError?.localizedDescription ?? "Audio couldn't start."
                        return
                    }
                    self.updateRoute()
                    if !self.soundQueue.isEmpty { self.beginNextSound() }
                    self.fillSounds()
                    if self.phraseInterval > 0 { self.requestGuide() }
                    else { self.guideStatus = "Just the sounds" }
                }
            }
        } catch {
            stop(message: "This session couldn't start.")
            self.error = error.localizedDescription
        }
    }

    private func fillSounds() {
        guard active || starting, !soundPending, !soundFailed,
              soundQueue.count < CalmBufferPolicy.maximumSoundClips else { return }
        soundPending = true
        let request = generation
        let next = scene.segment(sceneIndex); sceneIndex += 1
        audio.generate(kind: "sound", scene: next, recipe: soundRecipes[next.rawValue]) { [weak self] result in
            guard let self, self.generation == request, self.active || self.starting else { return }
            self.soundPending = false
            switch result {
            case .success(let clip):
                self.soundRetry.reset(); self.soundError = nil; self.updateError()
                // Saved clips bridge startup only; fresh audio takes the next slot.
                self.soundQueue.removeAll { self.savedSoundIDs.contains($0.id) }
                self.soundQueue.append(clip); self.readySounds = self.soundQueue.count
                if self.soundPlayer == nil { self.beginNextSound() }
                self.fillSounds()
            case .failure(let error):
                self.soundFailed = true
                self.recoverAudio(error, sound: true)
            }
        }
    }

    private func player(for clip: GeneratedClip) throws -> AVAudioPlayer {
        let player = try AVAudioPlayer(contentsOf: clip.url)
        player.delegate = self
        player.numberOfLoops = 0
        player.prepareToPlay()
        return player
    }

    private func beginNextSound() {
        guard active || starting, !soundQueue.isEmpty else { return }
        let clip = soundQueue.removeFirst(); readySounds = soundQueue.count
        do {
            let next = try player(for: clip)
            next.volume = 0
            guard next.play() else { throw CalmError.missingSound }
            audio.markPlayed(clip)
            fadingPlayer?.stop()
            fadingPlayer = soundPlayer
            fadeDuration = soundPlayer == nil ? 0.6 : CalmBufferPolicy.crossfadeSeconds
            soundPlayer = next; fadeStarted = Date(); soundTitle = clip.label
            if starting {
                starting = false; active = true
                clock = CalmClock(started: Date(), duration: Double(minutes * 60))
                remaining = Double(minutes * 60)
                timer = Timer.scheduledTimer(withTimeInterval: 0.2, repeats: true) { [weak self] _ in self?.tick() }
            }
            status = "Just be here."
            updateNowPlaying(); fillSounds()
        } catch {
            soundFailed = true; self.error = "This sound couldn't play. Tap Retry audio."
        }
    }

    private func requestGuide() {
        guard active || starting, phraseInterval > 0, !guidePending,
              phrases.count < 4, Date() >= nextGuideAttempt else { return }
        guidePending = true; failedGuide = false; replies = [:]; replyOrder = []
        do {
            try cloud.send(["op": "chat", "message": CalmPlan.prompt, "session_id": guideSession])
        } catch { guideUnavailable() }
    }

    private func guideUnavailable() {
        guard active || starting else { return }
        guidePending = false; failedGuide = true
        nextGuideAttempt = Date().addingTimeInterval(30)
        guideStatus = "Muse guide unavailable · sounds continue"
    }

    private func receive(_ event: [String: Any]) {
        guard active || starting, guidePending else { return }
        switch event["type"] as? String {
        case "reply":
            guard let id = event["message_id"] as? String, let text = event["text"] as? String else { return }
            if !replyOrder.contains(id) { replyOrder.append(id) }
            replies[id] = text
        case "error": failedGuide = true
        case "turn_finished":
            guidePending = false
            let text = replyOrder.compactMap { replies[$0] }.joined(separator: "\n")
            if !failedGuide, let plan = try? CalmPlan.parse(text) {
                let fresh = plan.phrases.filter { usedPhrases.insert($0.lowercased()).inserted }
                phrases.append(contentsOf: fresh)
                for recipe in plan.soundscapes ?? [] { soundRecipes[recipe.scene] = recipe }
                guideStatus = "Gentle words from your Muse"
                nextGuideAttempt = Date().addingTimeInterval(30)
                fillVoice()
            } else { guideUnavailable() }
            replies = [:]; replyOrder = []
        default: break
        }
    }

    private func fillVoice() {
        guard active || starting, phraseInterval > 0, !voicePending, !voiceFailed,
              voiceQueue.filter({ !savedVoiceIDs.contains($0.id) }).count < CalmBufferPolicy.maximumVoiceClips else { return }
        guard !phrases.isEmpty else { requestGuide(); return }
        let text = phrases.removeFirst(); let request = generation
        voicePending = true
        audio.generate(kind: "voice", text: text, voice: voice) { [weak self] result in
            guard let self, self.generation == request, self.active || self.starting else { return }
            self.voicePending = false
            switch result {
            case .success(let clip):
                self.voiceRetry.reset(); self.voiceError = nil; self.updateError()
                self.voiceQueue.removeAll { self.savedVoiceIDs.contains($0.id) }
                self.voiceQueue.append(clip); self.fillVoice()
            case .failure(let error):
                self.voiceFailed = true
                self.recoverAudio(error, sound: false)
            }
        }
        requestGuide()
    }

    private func tick() {
        guard active, var clock else { return }
        remaining = clock.remaining(at: Date())
        guard remaining > 0 else { stop(message: "Session complete. Take your time."); return }
        if let player = soundPlayer, fadingPlayer == nil,
           player.duration - player.currentTime <= CalmBufferPolicy.crossfadeSeconds, !soundQueue.isEmpty {
            beginNextSound()
        }
        // Keep the pending cue until speech is ready, rather than missing a whole interval.
        if !voiceQueue.isEmpty && !speaking && voiceVolume > 0 && remaining > 10,
           clock.phraseDue(at: Date(), interval: phraseInterval, occupied: false) { playVoice() }
        self.clock = clock
        adjustVolumes()
        if phraseInterval > 0 { requestGuide(); fillVoice() }
    }

    private func playVoice() {
        guard !voiceQueue.isEmpty else { return }
        let clip = voiceQueue.removeFirst()
        do {
            let player = try player(for: clip)
            player.volume = Float(voiceVolume)
            guard player.play() else { throw CalmError.missingSound }
            audio.markPlayed(clip)
            voicePlayer = player; speaking = true; phrase = clip.text ?? ""
            fillVoice()
        } catch { voiceFailed = true; self.error = "The voice couldn't play. Tap Retry audio." }
    }

    private func updateError() {
        error = [soundError, voiceError].compactMap { $0 }.joined(separator: "\n")
        if error == "" { error = nil }
    }

    private func recoverAudio(_ failure: Error, sound: Bool) {
        let failure = failure as NSError
        let requestedDelay = failure.userInfo["retryAfter"] as? Double
        let delay = sound ? soundRetry.nextDelay(status: failure.code, retryAfter: requestedDelay)
                          : voiceRetry.nextDelay(status: failure.code, retryAfter: requestedDelay)
        let kind = sound ? "Sound" : "Voice"
        let message: String
        if let delay, delay < 120 {
            message = "\(kind) connection paused. Retrying in \(Int(ceil(delay))) seconds."
        } else if let delay {
            message = "\(kind) service reached a usage limit. Retrying in \(Int(ceil(delay / 60))) minutes."
        } else { message = failure.localizedDescription + " Tap Retry audio." }
        if sound { soundError = message } else { voiceError = message }
        updateError()
        if starting && sound { status = "Waiting for the sound service…" }
        guard let delay else { return }
        let request = generation
        let task = DispatchWorkItem { [weak self] in
            guard let self, self.generation == request, self.active || self.starting else { return }
            if sound {
                self.soundRetryTask = nil; self.soundFailed = false; self.fillSounds()
            } else {
                self.voiceRetryTask = nil; self.voiceFailed = false; self.fillVoice()
            }
        }
        if sound { soundRetryTask?.cancel(); soundRetryTask = task }
        else { voiceRetryTask?.cancel(); voiceRetryTask = task }
        DispatchQueue.main.asyncAfter(deadline: .now() + delay, execute: task)
    }

    func retryAudio() {
        guard active || starting else { return }
        // An advertised provider cooldown must expire even after a manual tap.
        if soundRetryTask == nil { soundFailed = false; soundRetry.reset(); soundError = nil }
        if voiceRetryTask == nil { voiceFailed = false; voiceRetry.reset(); voiceError = nil }
        updateError()
        nextGuideAttempt = .distantPast
        if starting { status = "Creating a fresh sound…" }
        if soundPlayer == nil { beginNextSound() }
        fillSounds(); requestGuide(); fillVoice()
    }

    private func silenceVoice() {
        voicePlayer?.stop(); voicePlayer = nil; speaking = false
    }

    func stop(message: String = "A quiet moment, just for you.") {
        guard active || starting else { return }
        generation = UUID(); active = false; starting = false
        soundRetryTask?.cancel(); soundRetryTask = nil
        voiceRetryTask?.cancel(); voiceRetryTask = nil
        timer?.invalidate(); timer = nil; clock = nil
        cloud.disconnect(); audio.cancelAll()
        replies = [:]; replyOrder = []; guidePending = false
        silenceVoice()
        soundPlayer?.stop(); fadingPlayer?.stop()
        soundPlayer = nil; fadingPlayer = nil; fadeStarted = nil; duckGain = 1
        soundQueue = []; voiceQueue = []; savedSoundIDs = []; savedVoiceIDs = []; phrases = []; readySounds = 0
        soundPending = false; voicePending = false
        remaining = 0; status = message
        audio.trimCache()
        MPNowPlayingInfoCenter.default().nowPlayingInfo = nil
        try? AVAudioSession.sharedInstance().setActive(false, options: .notifyOthersOnDeactivation)
    }

    private func adjustVolumes() {
        let target: Float = speaking ? 0.3 : 1
        duckGain += (target - duckGain) * 0.3
        let progress = min(1, max(0, (fadeStarted.map { Date().timeIntervalSince($0) } ?? fadeDuration) / fadeDuration))
        let gain = Float(soundVolume) * duckGain
        soundPlayer?.volume = gain * Float(progress)
        fadingPlayer?.volume = gain * Float(1 - progress)
        if progress >= 1 { fadingPlayer?.stop(); fadingPlayer = nil }
    }
    private func updateRoute() {
        route = AVAudioSession.sharedInstance().currentRoute.outputs.map(\.portName).joined(separator: ", ")
    }
    private func updateNowPlaying() {
        MPNowPlayingInfoCenter.default().nowPlayingInfo = [
            MPMediaItemPropertyTitle: soundTitle, MPMediaItemPropertyArtist: "Muse Calm",
            MPMediaItemPropertyPlaybackDuration: Double(minutes * 60),
            MPNowPlayingInfoPropertyElapsedPlaybackTime: Double(minutes * 60) - remaining,
            MPNowPlayingInfoPropertyPlaybackRate: 1.0]
    }
    func audioPlayerDidFinishPlaying(_ player: AVAudioPlayer, successfully flag: Bool) {
        if player === voicePlayer {
            voicePlayer = nil; speaking = false; fillVoice()
        } else if player === soundPlayer {
            soundPlayer = nil
            if !soundQueue.isEmpty { beginNextSound() }
            else { status = "Creating the next fresh sound…"; soundTitle = "A quiet pause"; fillSounds() }
        } else if player === fadingPlayer { fadingPlayer = nil }
    }
    func audioPlayerDecodeErrorDidOccur(_ player: AVAudioPlayer, error: Error?) {
        if player === soundPlayer || player === fadingPlayer || player === voicePlayer {
            stop(message: "Audio couldn't continue. Start again when you're ready.")
        }
    }
}
