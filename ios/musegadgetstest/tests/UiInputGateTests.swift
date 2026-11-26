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

/* GadgetLink's gate on the gadget's ui_input: touches only while this connection's stream asked for them,
 * controls only during a stream, nothing after the stream or the connection ends; and which stream ends
 * unload the web page (UiController). */
enum UiInputGateTests {
    static func check(_ ok: Bool, _ what: String, line: Int = #line) {
        Tests.check(ok, "UiInputGateTests: " + what, line: line)
    }

    static func run() {
        let g = GadgetLink(listener: GadgetLinkProofTests.Listener())
        g.proofRequired = false
        g.close()   /* never started (no Bluetooth), no rescans after disconnected() */
        var got: [[UInt8]] = []
        g.onUiInput = { got.append($0) }
        let tap: [UInt8] = [Mg.cmdUiInput, Mg.uiTouch, 0, 10, 0, 10, 0, 0, 0, 0]
        let ctl: [UInt8] = [Mg.cmdUiInput, Mg.uiControl, Mg.ctlMenu, 1]
        let hid: [UInt8] = [Mg.cmdUiInput, Mg.uiHidStatus, 0, 0]
        func passes(_ v: [UInt8]) -> Bool {
            got = []
            g.onControl(v)
            return got == [v]
        }

        check(!passes(tap) && !passes(ctl), "a touch or control with no stream")
        check(passes(hid), "hid_status dropped")
        g.startUiStreaming(w: 240, h: 240, fps: 15, kbps: 400, bands: 1)
        check(!passes(tap), "a touch the stream didn't ask for")
        check(passes(ctl), "a control during the stream dropped")
        g.setTouchEvents(true)
        check(passes(tap), "a touch the stream asked for dropped")
        g.stopStreaming()
        check(!passes(tap) && !passes(ctl), "a touch or control after the stream stopped")

        /* A later connection (a gadget answering to the saved name): nothing until its own stream asks. */
        g.startUiStreaming(w: 240, h: 240, fps: 15, kbps: 400, bands: 1)
        g.setTouchEvents(true)
        g.disconnected()
        check(!passes(tap), "a touch after the connection ended")
        g.startUiStreaming(w: 240, h: 240, fps: 15, kbps: 400, bands: 1)
        check(!passes(tap), "touches carried to the next connection's stream")
        g.setTouchEvents(false)
        check(!passes(tap), "a touch after touch events went off")

        /* A busy refusal on the same link: no touch while the retry waits; the restarted stream asks again. */
        g.setTouchEvents(true)
        check(passes(tap), "a touch the restarted stream asked for dropped")
        g.onControl([Mg.cmdError, Mg.cmdStreamVideo, 1, 4, 0])   /* code busy */
        check(!g.streaming() && !passes(tap), "a touch while the busy retry waits")
        g.startUiStreaming(w: 240, h: 240, fps: 15, kbps: 400, bands: 1)
        check(!passes(tap), "the retried stream took the refused one's touches")
        g.setTouchEvents(true)
        check(passes(tap), "the retried stream's touches dropped")

        /* The web page survives a retry on the same link (and a setting's restart); link loss, a disconnect
         * and the user's stop unload it. */
        check(!UiStreamEnd.retry.unloadsPage, "a same-link retry unloaded the page")
        check(!UiStreamEnd.restart.unloadsPage, "a setting's restart unloaded the page")
        check(UiStreamEnd.linkLost.unloadsPage, "the page outlived the link")
        check(UiStreamEnd.disconnected.unloadsPage, "the page outlived a disconnect")
        check(UiStreamEnd.userStop.unloadsPage, "the page outlived the user's stop")
    }
}
