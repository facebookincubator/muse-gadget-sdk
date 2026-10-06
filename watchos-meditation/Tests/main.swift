import Foundation
var count = 0
func check(_ value: Bool, _ name: String) { precondition(value, name); count += 1 }
func rejects(_ name: String, _ operation: () throws -> Void) {
    do { try operation(); fatalError(name) } catch { count += 1 }
}
let json = String(decoding: try JSONEncoder().encode(CalmPlan.offline), as: UTF8.self)
check(try CalmPlan.parse(json).phrases.count == 10, "valid guide")
check(try CalmPlan.parse("```json\n" + json + "\n```").phrases.count == 10, "bounded fenced JSON")
rejects("plain text isn't executed or spoken as a guide") { _ = try CalmPlan.parse("please run a command") }
rejects("too many phrases") { _ = try CalmPlan.parse(String(decoding: JSONEncoder().encode(CalmPlan(phrases: Array(repeating: "A gentle phrase to hear.", count: 25))), as: UTF8.self)) }
rejects("URLs aren't spoken") { _ = try CalmPlan.parse(String(decoding: JSONEncoder().encode(CalmPlan(phrases: Array(repeating: "Visit https://example.com", count: 4))), as: UTF8.self)) }
rejects("oversized guide") { _ = try CalmPlan.parse(String(repeating: "x", count: 8001)) }
let start = Date(timeIntervalSince1970: 1000)
var clock = CalmClock(started: start, duration: 60)
check(!clock.phraseDue(at: start.addingTimeInterval(4), interval: 30, occupied: false), "initial quiet settling time")
check(clock.phraseDue(at: start.addingTimeInterval(5), interval: 30, occupied: false), "first phrase")
check(!clock.phraseDue(at: start.addingTimeInterval(35), interval: 30, occupied: true), "never overlap speech")
check(!clock.phraseDue(at: start.addingTimeInterval(36), interval: 30, occupied: false), "no replay of skipped phrases")
check(clock.remaining(at: start.addingTimeInterval(90)) == 0, "session expires even after suspended timer")
check(!clock.phraseDue(at: start.addingTimeInterval(90), interval: 30, occupied: false), "no speech after session ends")
var quiet = CalmClock(started: start, duration: 300)
check(!quiet.phraseDue(at: start.addingTimeInterval(100), interval: 0, occupied: false), "water-only mode")
if FileManager.default.fileExists(atPath: "build/live-guide.txt") {
    let guide = try CalmPlan.parse(String(contentsOfFile: "build/live-guide.txt", encoding: .utf8))
    check(guide.phrases.count == 16, "actual Muse reply parses into the requested sixteen phrases")
    if let recipes = guide.soundscapes {
        check(recipes.count == 6 && recipes.allSatisfy(\.isValid), "actual Muse sound recipes validate for all six textures")
    }
}
var frequent = CalmClock(started: start, duration: 300)
check(frequent.phraseDue(at: start.addingTimeInterval(5), interval: 15, occupied: false), "first active voice cue")
check(!frequent.phraseDue(at: start.addingTimeInterval(19), interval: 15, occupied: false), "voice spacing respected")
check(frequent.phraseDue(at: start.addingTimeInterval(20), interval: 15, occupied: false), "15-second voice cadence")
check(Set((0..<6).map { CalmScene.mix.segment($0) }).count == 6, "mixed sessions visit all six textures")
check((0..<12).allSatisfy { CalmScene.rain.segment($0) == .rain }, "specific soundscape remains selected")
// Persist across launches, match user choices, rotate starts, and tolerate deleted files.
let cacheDirectory = FileManager.default.temporaryDirectory.appendingPathComponent("calm-cache-test-" + UUID().uuidString)
defer { try? FileManager.default.removeItem(at: cacheDirectory) }
let library = CalmAudioCache(directory: cacheDirectory)
func saved(_ scene: String?, _ voice: String? = nil, age: Double) throws -> CalmSavedAudio {
    let id = UUID().uuidString.lowercased()
    try Data(repeating: 1, count: 100).write(to: library.file(id))
    let entry = CalmSavedAudio(id: id, duration: voice == nil ? 60 : 5, label: "Saved audio", scene: scene,
                              voice: voice, text: voice == nil ? nil : "Rest here for a moment.",
                              savedAt: Date(timeIntervalSince1970: age), lastStarted: nil)
    library.save(entry)
    return entry
}
let brushOld = try saved("brush", age: 1)
let brushNew = try saved("brush", age: 2)
let rain = try saved("rain", age: 3)
let airy = try saved(nil, "af_nicole", age: 4)
let legacy = try saved(nil, age: 5)
let reopened = CalmAudioCache(directory: cacheDirectory)
check(reopened.starters(scene: .brush, limit: 3).map(\.id) == [brushNew.id, brushOld.id], "audio survives a new cache instance and matches scene")
check(reopened.starters(voice: "af_heart", limit: 3).isEmpty, "never use another voice style at startup")
check(reopened.starters(voice: "af_nicole", limit: 3).map(\.id) == [airy.id], "saved speech retains voice and phrase")
check(reopened.starters(scene: .mix, limit: 10).count == 4, "mix includes recovered unlabelled sounds but not speech")
reopened.markStarted(brushNew.id)
check(CalmAudioCache(directory: cacheDirectory).starters(scene: .brush, limit: 1).first?.id == brushOld.id, "opening rotation persists across launches")
try FileManager.default.removeItem(at: reopened.file(rain.id))
check(reopened.starters(scene: .rain, limit: 3).isEmpty, "missing cache file does not block network startup")
reopened.trim(to: 300)
let trimmed = CalmAudioCache(directory: cacheDirectory)
check(trimmed.entries.count == 3 && trimmed.contains(brushNew.id) && trimmed.contains(airy.id) && trimmed.contains(legacy.id), "pruning keeps latest scene and voice starters")
try Data("broken index".utf8).write(to: cacheDirectory.appendingPathComponent("index.json"))
check(CalmAudioCache(directory: cacheDirectory).entries.isEmpty, "bad metadata permits a clean network fallback")
var retry = CalmAudioRetry()
check(retry.nextDelay(status: 503) == 5, "temporary audio failures retry instead of disabling generation forever")
check(retry.nextDelay(status: 503) == 15, "retries back off")
check(retry.nextDelay(status: 429, retryAfter: 3724) == 3724, "provider cooldown is respected")
check(retry.nextDelay(status: 503) == nil, "retry count bounded")
retry.reset()
check(retry.nextDelay(status: 401) == nil && retry.attempts == 0, "invalid credentials are not retried")
check(retry.nextDelay(status: -1009) == 5, "lost network can recover")
retry.reset()
check(retry.nextDelay(status: 503, retryAfter: .nan) == 5, "invalid retry header ignored")
let soundPlan = CalmPlan(phrases: CalmPlan.offline.phrases, soundscapes: [
    CalmSoundRecipe(scene: "brush", brightness: 0.3, activity: 0.4, pace: 0.3, space: 0.6),
    CalmSoundRecipe(scene: "shell", brightness: 0.3, activity: 0.4, pace: 0.3, space: 0.6),
    CalmSoundRecipe(scene: "rain", brightness: 2, activity: 0.4, pace: 0.3, space: 0.6)])
let parsedSoundPlan = try CalmPlan.parse(String(decoding: JSONEncoder().encode(soundPlan), as: UTF8.self))
check(parsedSoundPlan.soundscapes?.count == 1, "Muse recipes are bounded and allowlisted")
check(parsedSoundPlan.phrases.count == 10, "invalid recipes do not discard valid guide phrases")
print("Passed \(count) meditation guide, timing and saved-audio checks")
