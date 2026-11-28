/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
import Foundation

/* report_capabilities parsing (GadgetLink.parseCaps): malformed reports are
 * rejected without trapping and leave the caps as they were. */
enum CapsTests {
    static func check(_ ok: Bool, _ what: String, line: Int = #line) {
        Tests.check(ok, "CapsTests: " + what, line: line)
    }

    static func parse(_ v: [UInt8]) -> (Caps, Int?) {
        let c = Caps()
        return (c, GadgetLink.parseCaps(v, into: c))
    }

    static func run() {
        /* Every fixed-width capability with its value cut short. */
        let cut: [[UInt8]] = [
            [0x84, 0x01, 0x01, 0x04],                    /* max_fps, no value */
            [0x84, 0x01, 0x01, 0x01],                    /* codecs, no count */
            [0x84, 0x01, 0x01, 0x01, 0x02, 0x18],        /* codecs, one of two */
            [0x84, 0x01, 0x01, 0x02, 0xe8, 0x01, 0xe8, 0x01],
            [0x84, 0x01, 0x01, 0x03, 0xf0, 0x00, 0xf0],
            [0x84, 0x01, 0x01, 0x05, 0x00, 0x00, 0x01],
            [0x84, 0x01, 0x01, 0x06, 0xe8, 0x00, 0xe8, 0x00, 0x18, 0xc2],
            [0x84, 0x01, 0x01, 0x07, 0x80],
            [0x84, 0x01, 0x01, 0x08, 0x10],
            [0x84, 0x01, 0x01, 0x09],
            [0x84, 0x01, 0x01, 0x0a],
            [0x84, 0x01, 0x01, 0x0b, 0xd0, 0x01, 0xd0, 0x01, 0x0f, 0x00, 0x40, 0x00, 0x00],
            [0x84, 0x01, 0x01, 0x0c],
        ]
        for v in cut {
            check(parse(v).1 == nil, "short report \(v) accepted")
        }

        /* A complete ui capability, then a max_fps without its value: the
         * link's caps keep their earlier values (huge sizes never land). */
        let link = Caps()
        link.uiW = 400
        link.uiH = 400
        link.maxFps = 20
        let bad: [UInt8] = [0x84, 0x01, 0x02, 0x0b, 0xff, 0xff, 0xff, 0xff, 0x0f, 0x00, 0x00, 0x01, 0x00, 0x01, 0x04]
        check(GadgetLink.applyCapsReport(bad, to: link, bufferOverride: 0) == nil, "truncated ui+max_fps accepted")
        check(link.uiW == 400 && link.uiH == 400 && link.maxFps == 20 && !link.video, "link caps changed by a rejected report")

        /* A good report is clamped before the link's caps see it, with the bench override. */
        let big: [UInt8] = [0x84, 0x01, 0x02, 0x0b, 0xff, 0xff, 0xff, 0xff, 0x0f, 0x00, 0x00, 0x01, 0x00, 0x01,
                            0x05, 0x00, 0x00, 0x01, 0x00]
        check(GadgetLink.applyCapsReport(big, to: link, bufferOverride: 8192) == GadgetLink.noPsm, "big report rejected")
        check(link.uiW == 720 && link.uiH == 720 && link.buffer == 8192 && link.uiMaxFrame == 4096 && link.video,
              "big report not clamped: \(link.uiW)x\(link.uiH) buffer \(link.buffer)")

        /* Fields a report doesn't carry keep their values, except codecs, ui and ui_still. */
        let kept = Caps()
        kept.vp8 = true
        kept.uiW = 400
        kept.uiH = 400
        kept.uiStill = 2
        kept.maxFps = 20
        kept.buffer = 8192
        check(GadgetLink.parseCaps([0x84, 0x01, 0x01, 0x03, 0x40, 0x01, 0x40, 0x01], into: kept) == GadgetLink.noPsm, "max_size only")
        check(!kept.vp8 && kept.uiW == 0 && kept.uiH == 0 && kept.uiStill == 0, "codecs/ui/ui_still not cleared")
        check(kept.maxFps == 20 && kept.buffer == 8192 && kept.maxW == 320 && kept.maxH == 320, "fields not kept")

        /* A well-formed report parses, and clamp keeps it in range. */
        let good: [UInt8] = [0x84, 0x01, 0x05,
                             0x01, 0x02, 0x18, 0x19,                         /* codecs: H.264, VP8 */
                             0x03, 0xf0, 0x00, 0xf0, 0x00,                   /* max 240x240 */
                             0x07, 0x80, 0x00,                               /* PSM 0x80 */
                             0x0b, 0xff, 0xff, 0xd0, 0x01, 0x0f, 0x00, 0x40, 0x00, 0x00, 0x02,  /* ui 65535x464 */
                             0x04, 0x1e]                                     /* 30 fps */
        let (c, psm) = parse(good)
        check(psm == 0x80, "psm \(String(describing: psm))")
        check(c.vp8, "vp8")
        check(c.maxW == 240 && c.maxH == 240, "max size")
        check(c.maxFps == 30, "max fps")
        c.clamp()
        check(c.uiW == 720 && c.uiH == 464 && c.uiBands == 2, "ui clamped: \(c.uiW)x\(c.uiH)")

        /* Fewer entries than counted, or no count: malformed. An unknown id up to 16 ends the report
         * there; from 17 on it carries a length and is skipped. */
        check(parse([0x84, 0x01, 0x03, 0x04, 0x1e]).1 == nil, "short count accepted")
        let (u, up) = parse([0x84, 0x01, 0x03, 0x04, 0x1e, 0x10, 0x01, 0x09, 0x02])
        check(up == GadgetLink.noPsm && u.maxFps == 30 && u.maxTiles == 1, "unknown id 16 didn't end the report")
        let (s17, sp) = parse([0x84, 0x01, 0x03, 0x04, 0x1e, 0x11, 0x02, 0xaa, 0xbb, 0x09, 0x02])
        check(sp == GadgetLink.noPsm && s17.maxFps == 30 && s17.maxTiles == 2, "id 17 not skipped by its length")
        check(parse([0x84, 0x01, 0x02, 0x04, 0x1e, 0x7f, 0x05, 0xaa]).1 == nil, "id 127 past the end accepted")
        check(parse([0x84, 0x01, 0x02, 0x04, 0x1e, 0x7f]).1 == nil, "id 127 without a length accepted")
        let (z17, zp17) = parse([0x84, 0x01, 0x03, 0x11, 0x00, 0x04, 0x1e, 0x09, 0x02])
        check(zp17 == GadgetLink.noPsm && z17.maxFps == 30 && z17.maxTiles == 2, "id 17 of length 0 not skipped")
        let long = [UInt8]([0x84, 0x01, 0x02, 0x04, 0x1e, 0x20, 0x90]) + [UInt8](repeating: 0xaa, count: 0x90)
        let (l17, lp) = parse(long)
        check(lp == GadgetLink.noPsm && l17.maxFps == 30, "id 32 of length 0x90, ending the report exactly, not skipped")
        check(parse(Array(long.dropLast())).1 == nil, "id 32 one byte short accepted")

        /* Error codes as mgcommands.h names them. */
        check(Mg.errorName(1) == "unsupported" && Mg.errorName(2) == "invalid length" && Mg.errorName(3) == "invalid value"
              && Mg.errorName(4) == "busy" && Mg.errorName(5) == "not found" && Mg.errorName(6) == "proof required"
              && Mg.errorName(7) == "insecure" && Mg.errorName(8) == "recovery" && Mg.errorName(0) == "code 0"
              && Mg.errorName(99) == "code 99", "error names")
        let (z, zp) = parse([0x84, 0x01, 0x02, 0x01, 0x00, 0x04, 0x0f])
        check(zp == GadgetLink.noPsm && !z.vp8 && z.maxFps == 15, "codecs with no entries")
        check(parse([0x84, 0x01]).1 == nil, "report without a count accepted")
        check(parse([0x84, 0x01, 0x00]).1 == GadgetLink.noPsm, "empty report")
        check(parse([0x84, 0x01, 0x01, 0x07, 0x00, 0x00]).1 == 0, "PSM 0 carried")
        let (two, _) = parse([0x84, 0x01, 0x02, 0x01, 0x01, 0x19, 0x01, 0x01, 0x18])
        check(two.vp8, "VP8 in the first of two codecs entries lost")

        /* A reader's snapshot is clamped, whatever the link's caps hold mid-update (a hello arriving). */
        let raw = Caps()
        raw.maxW = 1 << 40; raw.maxH = -5; raw.uiW = 1 << 40; raw.uiH = 1 << 40; raw.dispW = 15; raw.dispH = 1 << 30
        raw.sugW = 1 << 40; raw.buffer = 1 << 50; raw.uiMaxFrame = 1 << 50; raw.maxFps = 0
        let snap = raw.snapshot()
        check(snap.maxW == 720 && snap.maxH == 64 && snap.uiW == 720 && snap.uiH == 720 && snap.dispW == 0
              && snap.dispH == 2048 && snap.sugW == 720 && snap.buffer == 1 << 20 && snap.uiMaxFrame == 1 << 19
              && snap.maxFps == 1, "snapshot not clamped: \(snap)")
        check(raw.maxW == 1 << 40, "snapshot changed the caps it copied")

        /* A Wi-Fi hello is parsed and clamped on a copy: the link's caps see only clamped values. */
        let old = Caps()
        let hello: [String: Any] = ["display": ["width": 16, "height": 16], "max": ["width": 1 << 40, "height": 1 << 40, "fps": 30],
                                    "suggested": ["width": 1 << 40, "height": 240], "buffer": 1 << 50]
        let hc = WifiLink.helloCaps(hello, over: old)
        check(hc.maxW == 720 && hc.maxH == 720 && hc.sugW == 720 && hc.buffer == 1 << 20 && hc.dispW == 16 && hc.video,
              "hello caps not clamped: \(hc)")
        check(old.maxW == 240 && !old.video, "the hello wrote the old caps before the clamp")

        /* copy() carries every field. */
        let all = Caps()
        all.maxPlayoutMs = 1; all.dispW = 2; all.dispH = 3; all.maxW = 4; all.maxH = 5; all.maxFps = 6
        all.sugW = 7; all.sugH = 8; all.sugFps = 9; all.sugKbps = 10; all.round = true; all.buffer = 11
        all.video = true; all.maxTiles = 12; all.tileSplitPct = 13; all.vp8 = true
        all.uiW = 14; all.uiH = 15; all.uiFps = 16; all.uiMaxFrame = 17; all.uiBands = 18
        all.uiTouch = true; all.hidDigitizer = true; all.hidMouseAbs = true; all.hidMouseRel = true
        all.uiControls = true; all.uiStill = 19
        let fields = { (c: Caps) in Mirror(reflecting: c).children.map { "\($0.label ?? ""): \($0.value)" } }
        check(fields(all.copy()) == fields(all), "copy() misses a field")
        for (set, unset) in zip(fields(all), fields(Caps())) where set == unset {
            check(false, "copy() test leaves a field at its default: \(set)")
        }
    }
}
