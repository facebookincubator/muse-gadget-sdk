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

/* StartGrant: a user's start lasts one connection, however that connection went. */
enum StartGrantTests {
    static func check(_ ok: Bool, _ what: String, line: Int = #line) {
        Tests.check(ok, "StartGrantTests: " + what, line: line)
    }

    static func run() {
        var g = StartGrant()
        check(!g.held, "held before any start")

        /* A tap while disconnected: kept through failed connect attempts, good for the first usable link. */
        g.grant()
        g.connectionEnded(wasReady: false)
        g.connectionEnded(wasReady: false)
        check(g.held, "a connect attempt that never got a link dropped the start")

        /* That connection streams, never sends capabilities, and ends: the start goes with it. */
        let ep = g.epoch
        g.connectionEnded(wasReady: true)
        check(!g.held, "the start outlived a connection that withheld its capabilities")
        check(ep != g.epoch, "a retry from the ended connection still matches")

        /* A remade link (park, switch, picker) ends it too. */
        g.grant()
        g.linkRemade()
        check(!g.held, "the start outlived a remade link")

        /* Turned off, or refused: gone on the same connection. */
        g.grant()
        g.revoke()
        check(!g.held, "a revoked start still held")

        /* A start made on its own needs this connection's proof; Wi-Fi (nothing proved) only the user's. */
        var w = StartGrant()
        check(!w.allows(selfStart: true, proven: false), "the gadget's ask or auto started over a link that proved nothing")
        check(w.allows(selfStart: true, proven: true), "the gadget's ask after a matched proof refused")
        check(!w.allows(selfStart: false, proven: true), "a start nobody asked for allowed")
        w.grant()
        check(w.allows(selfStart: false, proven: false), "the user's own start refused over Wi-Fi")
        w.connectionEnded(wasReady: true)
        check(!w.allows(selfStart: false, proven: false) && !w.allows(selfStart: true, proven: false),
              "the user's start carried to the next Wi-Fi connection")

        /* The gadget's ask for the camera: the user's yes once per connection, none for the user's own start. */
        var cg = StartGrant()
        check(cg.needsCameraAsk(benchAuto: false), "the camera started on the gadget's ask without the user's yes")
        check(!cg.needsCameraAsk(benchAuto: true), "Start automatically with the bench marker asked")
        cg.allowCamera()
        check(!cg.needsCameraAsk(benchAuto: false), "asked again on the connection the user said yes on")
        cg.revoke()
        check(!cg.needsCameraAsk(benchAuto: false), "a stop on the same connection took the yes back")
        cg.connectionEnded(wasReady: false)
        check(!cg.needsCameraAsk(benchAuto: false), "a connect attempt that never got a link took the yes")
        cg.connectionEnded(wasReady: true)
        check(cg.needsCameraAsk(benchAuto: false), "the yes carried to the next connection (Start automatically, no marker)")
        check(!cg.needsCameraAsk(benchAuto: true), "Start automatically with the bench marker asked on a new connection")
        cg.allowCamera()
        cg.linkRemade()
        check(cg.needsCameraAsk(benchAuto: false), "the yes outlived a remade link")
        cg.grant()
        check(!cg.needsCameraAsk(benchAuto: false), "the user's own start asked")

        /* Disconnect: nothing from before it (the gadget's want in the close's grace, a retry, the camera's yes)
         * starts anything, the user's start included, until a gadget is picked again. */
        var d = StartGrant()
        d.grant()
        d.allowCamera()
        let before = d.epoch
        d.close()
        check(d.epoch != before, "a retry from before the Disconnect still matches")
        check(!d.held, "the user's start outlived the Disconnect")
        check(!d.allows(selfStart: true, proven: true), "the gadget's want after the Disconnect restarted the camera")
        check(!d.allows(selfStart: false, proven: false), "a start allowed after the Disconnect")
        d.grant()
        check(!d.held && !d.allows(selfStart: true, proven: true), "a start held while disconnected")
        d.reopen()
        check(d.held, "the user's start for the gadget picked again lost")
        check(d.needsCameraAsk(benchAuto: false) == false, "the user's own start asked")
        d.revoke()
        check(d.needsCameraAsk(benchAuto: false), "the camera's yes from before the Disconnect carried over")

        /* A setting change restarts only what runs, of its kinds, and never a UI stream (a web page, the screen). */
        check(!RunningStream.ui.restarts(for: [.camera, .clip]), "a setting change restarted a UI stream")
        check(!RunningStream.nothing.restarts(for: [.camera, .clip]), "a setting change started a stream")
        check(!RunningStream.clip.restarts(for: [.camera]), "a camera setting restarted a clip")
        check(RunningStream.camera.restarts(for: [.camera]) && RunningStream.clip.restarts(for: [.camera, .clip]),
              "a setting change didn't restart the stream it applies to")

        let wl = WifiLink(listener: GadgetLinkProofTests.Listener())
        check(!wl.proven, "a Wi-Fi link counted as proved")
        wl.startStreaming(w: 240, h: 240, fps: 15, kbps: 400, playoutMs: 0)
        check(!wl.streaming(), "a Wi-Fi start with no gadget connected was kept for whoever connects next")
        check(!GadgetLink(listener: GadgetLinkProofTests.Listener()).proven, "an unproved BLE link counted as proved")

        /* A connect attempt that never got a usable link is reported as such (it keeps the start). */
        let l = GadgetLinkProofTests.Listener()
        let link = GadgetLink(listener: l)
        link.disconnected()
        link.close()   /* before its rescan runs */
        check(l.disconnects == [false], "a link that never got ready reported a ready connection")
    }
}
