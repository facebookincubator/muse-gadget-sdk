import Foundation

var count = 0
func check(_ condition: Bool, _ name: String) {
    precondition(condition, name); count += 1
}
func rejects(_ name: String, _ action: () throws -> Void) {
    do { try action(); fatalError("Accepted invalid input: \(name)") }
    catch { count += 1 }
}

let message = ["op": "chat", "message": "Hello 🌍 你好", "session_id": "12345678-1234-1234-1234-123456789abc"]
let bytes = try MuseWire.encode(message)
// Exercise every split point, including every byte inside a multibyte character.
for split in 0...bytes.count {
    var decoder = FrameDecoder()
    let output = try decoder.feed(Data(bytes.prefix(split))) + decoder.feed(Data(bytes.dropFirst(split)))
    check(output.count == 1 && output[0]["message"] as? String == message["message"], "UTF-8 chunk boundary \(split)")
    check(!decoder.hasPartialFrame, "complete frame")
}
var decoder = FrameDecoder()
check(try decoder.feed(bytes + bytes).count == 2, "batched frames")
rejects("oversize frame") { _ = try decoder.feed(Data(repeating: 65, count: MuseWire.maximumFrame + 1)) }
check(!decoder.hasPartialFrame, "oversize clears frame")
rejects("non-object JSON") { _ = try decoder.feed(Data("[]\n".utf8)) }
rejects("malformed JSON") { _ = try decoder.feed(Data("{bad}\n".utf8)) }
decoder.reset()
check(try MuseWire.command(message)["message"] as? String == message["message"], "valid chat")
rejects("watch cannot change SDK token") { _ = try MuseWire.command(["op": "pair", "sdk_token": "secret"]) }
rejects("watch cannot stop host") { _ = try MuseWire.command(["op": "stop"]) }
rejects("invalid conversation") { _ = try MuseWire.command(["op": "chat", "message": "hi", "session_id": "bad"]) }
rejects("empty message") { _ = try MuseWire.command(["op": "chat", "message": "  ", "session_id": message["session_id"]!]) }
rejects("oversize message") { _ = try MuseWire.command(["op": "chat", "message": String(repeating: "x", count: 32001), "session_id": message["session_id"]!]) }
check(try MuseWire.command(["op": "check", "sdk_token": "do not forward"]).count == 1, "unknown fields removed")
var chat = Conversation()
chat.begin("Hello")
check(chat.receive(["type": "reply", "message_id": "one", "text": "Hel"]) == nil, "do not speak partial")
check(chat.receive(["type": "reply", "message_id": "one", "text": "Hello there", "complete": true]) == nil, "wait until turn finishes")
check(chat.lines.count == 2 && chat.lines[1].text == "Hello there", "snapshot replaces text")
check(chat.receive(["type": "turn_finished"]) == "Hello there", "speak completed turn")
check(chat.receive(["type": "turn_finished"]) == nil, "never speak duplicate completion")
chat.begin("Next")
_ = chat.receive(["type": "reply", "message_id": "two", "text": "New answer"])
check(chat.receive(["type": "turn_finished"]) == "New answer", "do not speak previous turn")
chat.begin("Fails")
_ = chat.receive(["type": "error", "error": "offline"])
check(chat.receive(["type": "turn_finished"]) == nil, "no speech after failure")
chat.begin("Disconnect")
chat.disconnected("Gone")
check(!chat.busy && chat.error?.contains("not resent") == true, "no automatic retry on disconnect")
let now = Date(timeIntervalSince1970: 1700000000)
check(HeartRateSummary.prompt(readings: [], context: "", now: now) == nil, "no fabricated health readings")
let invalid = [Double.nan, Double.infinity, -10, 0].map { HeartRateReading(id: UUID(), date: now, bpm: $0) }
check(HeartRateSummary.prompt(readings: invalid, context: "", now: now) == nil, "reject invalid measurements")
let health = HeartRateSummary.prompt(readings: [HeartRateReading(id: UUID(), date: now, bpm: 72)], context: "after walking", now: now)!
check(health.contains("72 bpm") && health.contains("2023-11-14T22:13:20Z"), "correct heart-rate units and timestamp")
check(health.contains("after walking") && health.contains("not a continuous recording"), "include context and sampling limits")
check(health.contains("Do not assume these are resting"), "avoid inferring unmeasured state")
check(HealthReview.quantity(values: [.nan, .infinity], unit: "count/min", cumulative: false) == nil, "reject nonfinite health values")
check(HealthReview.quantity(values: [0.97], unit: "%", cumulative: false)?.contains("97 %") == true, "HealthKit fractional percent conversion")
check(HealthReview.quantity(values: [100, 25], unit: "count", cumulative: true)?.contains("NOT daily totals") == true, "avoid double-counting cumulative source samples")
check(HealthReview.prompt(rows: [], days: 30, generatedAt: now, context: "") == nil, "no review without selected health data")
let reviewRow = HealthReviewRow(id: "heart", title: "Heart rate", detail: "latest 72 count/min")
let review = HealthReview.prompt(rows: [reviewRow], days: 7, generatedAt: now, context: "after exercise")!
check(review.contains("previous 7 days") && review.contains("2023-11-14T22:13:20Z"), "stable health review date and window")
check(review.contains("after exercise") && review.contains("72 count/min"), "review includes selected observations and context")
check(review.contains("not my clinician") && review.contains("Do not diagnose") && review.contains("one focused follow-up"), "health check-in role and conversational boundaries")
check(review.contains("not normal results") && review.contains("NOT my complete medical record"), "missing data never implies normality or completeness")
let longRows = (0..<200).map { HealthReviewRow(id: "\($0)", title: "Metric \($0)", detail: String(repeating: "測", count: 500)) }
let boundedReview = HealthReview.prompt(rows: longRows, days: 90, generatedAt: now, context: String(repeating: "🙂", count: 5000))!
check(boundedReview.utf8.count <= 32000 && boundedReview.contains("omitted"), "UTF-8 health message respects relay limit and discloses omissions")
check(try MuseWire.command(["op": "chat", "message": boundedReview, "session_id": message["session_id"]!])["message"] as? String == boundedReview, "health review accepted by actual relay contract")
let sleepIntervals = [DateInterval(start: now, duration: 3600), DateInterval(start: now.addingTimeInterval(1800), duration: 3600), DateInterval(start: now, duration: 3600)]
check(HealthReview.coveredSeconds(sleepIntervals) == 5400, "merge duplicated and overlapping sleep intervals")
check(HealthReview.coveredSeconds([]) == 0, "empty sleep data remains empty")
check(HealthReview.coveredSeconds([DateInterval(start: now, duration: 600), DateInterval(start: now.addingTimeInterval(1200), duration: 600)]) == 1200, "do not count gaps as sleep")
print("Passed \(count) protocol, conversation, and health checks")
