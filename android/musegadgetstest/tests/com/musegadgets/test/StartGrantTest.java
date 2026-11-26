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
package com.musegadgets.test;

/** VideoLink.StartGrant: a user's start lasts one connection, however that connection went. */
public final class StartGrantTest {
    private static int failed;

    private static void check(boolean ok, String what) {
        if (!ok) {
            failed++;
            System.out.println("FAIL: " + what);
        }
    }

    public static void main(String[] args) {
        VideoLink.StartGrant g = new VideoLink.StartGrant();
        check(!g.held(), "held before any start");

        /* A tap while disconnected: kept through failed connect attempts, good for the first usable link. */
        g.grant();
        g.connectionEnded(false);
        g.connectionEnded(false);
        check(g.held(), "a connect attempt that never got a link dropped the start");

        /* That connection streams, never sends capabilities, and ends: the start goes with it. */
        int ep = g.epoch();
        g.connectionEnded(true);
        check(!g.held(), "the start outlived a connection that withheld its capabilities");
        check(ep != g.epoch(), "a retry from the ended connection still matches");

        /* A remade link (park, switch, picker) ends it too. */
        g.grant();
        g.linkRemade();
        check(!g.held(), "the start outlived a remade link");

        /* Turned off, or refused: gone on the same connection. */
        g.grant();
        g.revoke();
        check(!g.held(), "a revoked start still held");

        /* A start made on its own needs this connection's proof; Wi-Fi (nothing proved) only the user's. */
        VideoLink.StartGrant w = new VideoLink.StartGrant();
        check(!w.allows(true, false), "the gadget's ask or auto started over a link that proved nothing");
        check(w.allows(true, true), "the gadget's ask after a matched proof refused");
        check(!w.allows(false, true), "a start nobody asked for allowed");
        w.grant();
        check(w.allows(false, false), "the user's own start refused over Wi-Fi");
        w.connectionEnded(true);
        check(!w.allows(false, false) && !w.allows(true, false), "the user's start carried to the next Wi-Fi connection");

        /* The gadget's ask for the camera: the user's yes once per connection, none for the user's own start. */
        VideoLink.StartGrant cg = new VideoLink.StartGrant();
        check(cg.needsCameraAsk(false), "the camera started on the gadget's ask without the user's yes");
        check(!cg.needsCameraAsk(true), "Start automatically with the bench marker asked");
        cg.allowCamera();
        check(!cg.needsCameraAsk(false), "asked again on the connection the user said yes on");
        cg.revoke();
        check(!cg.needsCameraAsk(false), "a stop on the same connection took the yes back");
        cg.connectionEnded(false);
        check(!cg.needsCameraAsk(false), "a connect attempt that never got a link took the yes");
        cg.connectionEnded(true);
        check(cg.needsCameraAsk(false), "the yes carried to the next connection (Start automatically, no marker)");
        check(!cg.needsCameraAsk(true), "Start automatically with the bench marker asked on a new connection");
        cg.allowCamera();
        cg.linkRemade();
        check(cg.needsCameraAsk(false), "the yes outlived a remade link");
        cg.grant();
        check(!cg.needsCameraAsk(false), "the user's own start asked");

        /* Disconnect: nothing from before it (the gadget's want in the close's grace, a retry, the camera's yes)
         * starts anything, the user's start included, until a gadget is picked again. */
        VideoLink.StartGrant d = new VideoLink.StartGrant();
        d.grant();
        d.allowCamera();
        int before = d.epoch();
        d.close();
        check(d.epoch() != before, "a retry from before the Disconnect still matches");
        check(!d.held(), "the user's start outlived the Disconnect");
        check(!d.allows(true, true), "the gadget's want after the Disconnect restarted the camera");
        check(!d.allows(false, false), "a start allowed after the Disconnect");
        d.grant();
        check(!d.held() && !d.allows(true, true), "a start held while disconnected");
        d.reopen();
        check(d.held(), "the user's start for the gadget picked again lost");
        d.revoke();
        check(d.needsCameraAsk(false), "the camera's yes from before the Disconnect carried over");

        /* A setting change restarts only what runs, of its kinds, and never a UI stream (a web page, the screen). */
        VideoLink.Running cam = VideoLink.Running.CAMERA, clip = VideoLink.Running.CLIP;
        check(!VideoLink.Running.UI.restarts(cam, clip), "a setting change restarted a UI stream");
        check(!VideoLink.Running.NOTHING.restarts(cam, clip), "a setting change started a stream");
        check(!clip.restarts(cam), "a camera setting restarted a clip");
        check(cam.restarts(cam) && clip.restarts(cam, clip), "a setting change didn't restart the stream it applies to");

        if (failed > 0) {
            System.out.println("StartGrantTest: " + failed + " failed");
            System.exit(1);
        }
        System.out.println("StartGrantTest: ok");
    }
}
