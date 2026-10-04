// Copyright (c) Meta Platforms, Inc. and affiliates.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
import Testing
import Foundation
@testable import MuseChat

struct EventDecoderTests {
    @Test func testDirectPairingAndConnectionEvents() throws {
        var decoder = EventDecoder()
        let events = try decoder.feed(Data("{\"type\":\"connection\",\"paired\":true,\"online\":false,\"pairing\":false,\"ble_name\":\"MuseGadget123456\"}\n{\"type\":\"turn_finished\"}\n".utf8))
        #expect(events[0].paired == true)
        #expect(events[0].online == false)
        #expect(events[0].bleName == "MuseGadget123456")
        #expect(events[1].type == "turn_finished")
    }
    @Test func testEveryUTF8SplitAndMultipleEvents() throws {
        let bytes = Data("{\"type\":\"reply\",\"message_id\":\"r\",\"text\":\"Hi 🌟 你好\",\"complete\":false}\n{\"type\":\"done\"}\n".utf8)
        for split in 0...bytes.count {
            var decoder = EventDecoder()
            let events = try decoder.feed(bytes.prefix(split)) + decoder.feed(bytes.dropFirst(split))
            #expect(events.count == 2)
            #expect(events[0].text == "Hi 🌟 你好")
            #expect(events[0].messageID == "r")
            #expect(events[1].type == "done")
            try decoder.finish()
        }
    }
    @Test func testErrorsAndTruncatedOutput() throws {
        var decoder = EventDecoder()
        let events = try decoder.feed(Data("{\"ok\":false,\"error\":\"not connected\"}\n".utf8))
        #expect(events.first?.ok == false)
        #expect(events.first?.error == "not connected")
        _ = try decoder.feed(Data("{\"type\":".utf8))
        #expect(throws: EventDecoder.DecodeError.self) { try decoder.finish() }
    }
    @Test func testInvalidAndOversizedOutputFails() {
        var decoder = EventDecoder()
        #expect(throws: DecodingError.self) { try decoder.feed(Data("not json\n".utf8)) }
        var bounded = EventDecoder()
        #expect(throws: EventDecoder.DecodeError.self) { try bounded.feed(Data(repeating: 65, count: 8 * 1024 * 1024 + 1)) }
    }
}
