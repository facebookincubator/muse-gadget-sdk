import Foundation

struct CalmPlan: Codable {
    let phrases: [String]
    var soundscapes: [CalmSoundRecipe]? = nil

    static func parse(_ text: String) throws -> CalmPlan {
        var text = text.trimmingCharacters(in: .whitespacesAndNewlines)
        if text.hasPrefix("```"), let newline = text.firstIndex(of: "\n"), text.hasSuffix("```") {
            text = String(text[text.index(after: newline)...].dropLast(3))
        }
        guard text.utf8.count <= 8000 else { throw CalmError.invalidGuide }
        let plan = try JSONDecoder().decode(CalmPlan.self, from: Data(text.utf8))
        guard (4...24).contains(plan.phrases.count) else { throw CalmError.invalidGuide }
        let phrases = plan.phrases.map { $0.trimmingCharacters(in: .whitespacesAndNewlines) }
        guard phrases.allSatisfy({ (8...200).contains($0.count) && !$0.contains("http") && !$0.contains("\n") }) else { throw CalmError.invalidGuide }
        return CalmPlan(phrases: phrases, soundscapes: plan.soundscapes?.filter { $0.isValid })
    }

    static let prompt = """
Create a gentle spoken meditation guide for someone listening to a changing, soft ASMR soundscape: brushing, rain, leaves, fabric, water and wood.
Return ONLY a JSON object with "phrases", containing 16 different short
sentences, each 8 to 18 words, and "soundscapes", described below. They will be spoken separately about every 15 seconds.
Use fresh phrases different from your earlier replies in this conversation.
Use simple, warm, unhurried language and optional invitations to notice gentle textures and the space around them,
relax the shoulders, or let thoughts pass. Invite natural comfortable breathing;
no breath holds, forced deep breathing, fixed breathing rates, or timed instructions.
Do not ask the listener to close their eyes. Do not diagnose, promise health
benefits, claim that you can sense their body, or make assumptions about their mood.
Also direct a soft soundscape. The "soundscapes" array must contain six objects,
one for each "scene": "brush", "fabric", "rain", "leaves", "wood", "water".
Each object has only "scene" and four numeric parameters from 0 to 1:
"brightness" (low is warm/dark), "activity" (low is sparse), "pace" (low is slow),
"space" (stereo spread). Keep brightness 0.2–0.5, activity 0.2–0.5, pace 0.2–0.5,
space 0.4–0.8, varying naturally between scenes and batches. These values direct
a bounded audio synthesizer; do not return executable code, audio URLs or files.
No introduction, numbering, markdown, URLs, stage directions, or questions.
"""

    // A clearly labeled offline guide, not presented as a live Muse response.
    static let offline = CalmPlan(phrases: [
        "Let the sound of the water meet you where you are.",
        "You can let your shoulders soften, if that feels comfortable.",
        "There is nothing you need to solve in this moment.",
        "Let your breathing find its own easy, natural rhythm.",
        "Notice a small ripple of sound, then let it drift away.",
        "A thought can pass through without needing your attention.",
        "Feel the support beneath you as the water continues flowing.",
        "You can return to this gentle sound whenever you choose.",
        "Allow this moment to be simple, just as it is.",
        "Take your time; the water has nowhere it needs to hurry."
    ])
}

enum CalmError: Error { case invalidGuide, missingSound }

struct CalmClock {
    let started: Date
    let duration: TimeInterval
    var nextPhrase: TimeInterval = 5
    func remaining(at now: Date) -> TimeInterval { max(0, duration - max(0, now.timeIntervalSince(started))) }
    mutating func phraseDue(at now: Date, interval: TimeInterval, occupied: Bool) -> Bool {
        let elapsed = now.timeIntervalSince(started)
        guard remaining(at: now) > 0, interval > 0, elapsed >= nextPhrase else { return false }
        nextPhrase = elapsed + interval
        return !occupied
    }
}


struct CalmSoundRecipe: Codable {
    let scene: String
    let brightness: Double
    let activity: Double
    let pace: Double
    let space: Double
    var isValid: Bool {
        ["brush", "fabric", "rain", "leaves", "wood", "water"].contains(scene) &&
        [brightness, activity, pace, space].allSatisfy { $0.isFinite && (0...1).contains($0) }
    }
    var parameters: [String: Double] {
        ["brightness": brightness, "activity": activity, "pace": pace, "space": space]
    }
}
