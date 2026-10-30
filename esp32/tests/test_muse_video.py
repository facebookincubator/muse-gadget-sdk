# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""Video streaming on the host: the receive ring every transport shares
(components/muse_vstream) and the Wi-Fi control messages
(components/muse/muse_video_wire.c), in tests/muse_video_harness.c; and the
firmware's wiring under CONFIG_MUSE_VIDEO."""

from __future__ import annotations

import os
import shlex
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MUSE = ROOT / "components/muse"
VSTREAM = ROOT / "components/muse_vstream"
# CI supplies pinned upstream sources; local IDF builds already have cJSON.
JSON = Path(os.environ.get("CJSON_SOURCE_DIR", ROOT / "managed_components/espressif__cjson/cJSON"))


class MuseVideoTest(unittest.TestCase):
    def test_ring_and_wire(self) -> None:
        if not (JSON / "cJSON.c").exists():
            self.skipTest("cJSON not found: run idf.py build once, or set CJSON_SOURCE_DIR")
        cc = shlex.split(os.environ.get("CC", "cc"))
        with tempfile.TemporaryDirectory() as tmp:
            exe = Path(tmp, "video")
            build = subprocess.run(
                [*cc, "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
                 f"-I{VSTREAM / 'include'}", f"-I{MUSE}", f"-I{JSON}", "-o", str(exe),
                 str(ROOT / "tests/muse_video_harness.c"), str(VSTREAM / "muse_vstream.c"),
                 str(MUSE / "muse_video_wire.c"), str(JSON / "cJSON.c"), "-lm"],
                capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=60)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertIn("PASS muse_video", run.stdout)

    def test_restart_while_the_consumer_pops(self) -> None:
        # The producer (a new start) can run while the consumer is between
        # checking a frame and consuming it. The ring source is built with its
        # barriers hooked, so the start lands exactly there.
        harness = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
static void race(void);
#define BARRIER() race()
#include "muse_vstream.c"

static muse_vstream_t s;
static int countdown;   /* barriers until the producer restarts the stream */

static void race(void)
{
    if (countdown > 0 && --countdown == 0) {
        muse_vstream_start(&s);
    }
}

static uint32_t frame(uint8_t *f, uint16_t number, uint32_t len)
{
    memset(f, 0x55, MUSE_VIDEO_FRAME_HEADER_LEN + len);
    f[0] = MUSE_VIDEO_FRAME_MAGIC;
    f[1] = 0;
    f[2] = (uint8_t)number;
    f[3] = (uint8_t)(number >> 8);
    f[4] = (uint8_t)len;
    f[5] = (uint8_t)(len >> 8);
    f[6] = (uint8_t)(len >> 16);
    f[7] = (uint8_t)(len >> 24);
    f[8] = f[9] = f[10] = 0;   /* an access unit's start code */
    f[11] = 1;
    return MUSE_VIDEO_FRAME_HEADER_LEN + len;
}

int main(void)
{
    static uint8_t ring[4096], f[600], out[600];
    muse_vframe_t fr;
    muse_vstream_init(&s, ring, sizeof(ring), 1024);
    muse_vstream_start(&s);
    assert(muse_vstream_pop(&s, out, sizeof(out), &fr) == 0);   /* nothing yet */
    muse_vstream_write(&s, f, frame(f, 1, 500));
    /* Its second barrier is the one before the consume. */
    countdown = 2;
    assert(muse_vstream_pop(&s, out, sizeof(out), &fr) == 500);
    /* The new stream starts clean all the same. */
    muse_vstream_write(&s, f, frame(f, 7, 300));
    assert(muse_vstream_pop(&s, out, sizeof(out), &fr) == 300 && fr.number == 7);
    assert(muse_vstream_available(&s) == sizeof(ring));
    /* A start with the old stream unread: the new one's credits are the
     * whole ring at once, before the consumer has popped (it may be waiting
     * for those very credits to bring a frame). */
    muse_vstream_write(&s, f, frame(f, 8, 500));
    muse_vstream_write(&s, f, frame(f, 9, 500));
    assert(muse_vstream_available(&s) < sizeof(ring));
    muse_vstream_start(&s);
    assert(muse_vstream_available(&s) == sizeof(ring));
    muse_vstream_write(&s, f, frame(f, 1, 200));
    assert(muse_vstream_pop(&s, out, sizeof(out), &fr) == 200 && fr.number == 1);
    assert(muse_vstream_available(&s) == sizeof(ring));
    puts("PASS restart");
    return 0;
}
'''
        cc = shlex.split(os.environ.get("CC", "cc"))
        with tempfile.TemporaryDirectory() as tmp:
            src, exe = Path(tmp, "restart.c"), Path(tmp, "restart")
            src.write_text(harness)
            build = subprocess.run(
                [*cc, "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
                 f"-I{VSTREAM / 'include'}", f"-I{VSTREAM}", "-o", str(exe), str(src)],
                capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=60)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertIn("PASS restart", run.stdout)

    def test_wired_under_the_option(self) -> None:
        cmake = (MUSE / "CMakeLists.txt").read_text()
        self.assertIn("if(CONFIG_MUSE_VIDEO)", cmake)
        self.assertIn('"muse_video_wifi.c" "muse_video_wire.c"', cmake)
        self.assertIn("muse_vstream tinyh264 libvpx_vp8", cmake)
        self.assertIn('"muse_video.c" "muse_vdec.c" "muse_yuv.c"', cmake)
        manifest = (MUSE / "idf_component.yml").read_text()
        # Keyed on a symbol every build defines: a rule on one a build lacks
        # (MUSE_VIDEO_WIFI without video) stops the component manager.
        self.assertIn('if: "$CONFIG{MUSE_VIDEO_MDNS} == \\"y\\""', manifest)
        kc = (MUSE / "Kconfig").read_text()
        mdns = kc[kc.index("config MUSE_VIDEO_MDNS"):]
        mdns = mdns[:mdns.index("\n\n")]
        self.assertNotIn("depends on", mdns)
        self.assertIn('default "n"', mdns)
        app = (MUSE / "muse_app.c").read_text()
        self.assertIn("#if CONFIG_MUSE_VIDEO_WIFI\n    muse_video_wifi_start();", app)
        # The touch boards' settings decide the Phone camera row as they're
        # built, so the transport has to be there before the UI.
        self.assertLess(app.index("muse_video_wifi_start();"), app.index("muse_ui_start()"))
        # Any button leaves the view; the console opens and closes it.
        inp = (MUSE / "muse_input.c").read_text()
        self.assertIn("muse_video_close();\n            ev &= ~(presses | MUSE_BTN_POWER_LONG);", inp)
        self.assertIn('!strcmp(line, "video=0")', inp)
        # The transports' clients hear the view open and close.
        video = (MUSE / "muse_video.c").read_text()
        self.assertIn("s_transports[i]->request(true);", video)
        self.assertIn("s_transports[i]->request(false);", video)
        tiny = (ROOT / "components/tinyh264/CMakeLists.txt").read_text()
        self.assertIn("if(CONFIG_MUSE_VIDEO)", tiny)
        vpx = (ROOT / "components/libvpx_vp8/CMakeLists.txt").read_text()
        self.assertIn("if(CONFIG_MUSE_VIDEO_VP8)", vpx)
        # VP8's IRAM placement is a choice that follows the decoders' IRAM option.
        self.assertIn("default MUSE_VP8_IRAM_COMPACT if MUSE_VIDEO_DECODER_IRAM",
                      (ROOT / "components/libvpx_vp8/Kconfig").read_text())
        self.assertIn("elif MUSE_VP8_IRAM_COMPACT = y:", (ROOT / "components/libvpx_vp8/linker.lf").read_text())
        # The ring resyncs by the stream's codec, whatever the transport.
        self.assertIn("t->ring->codec = vp->codec;", video)
        # A replaced stream waits for both pictures, as a stopped one does.
        swap = video[video.index("if (!dec || gen != s_gen) {"):]
        swap = swap[:swap.index("muse_vdec_close(dec);")]
        self.assertEqual(swap.count("xSemaphoreTake(s_slots, portMAX_DELAY);"), 2)

    def test_radio_held_for_a_stream(self) -> None:
        """A stream keeps Wi-Fi out of modem sleep and BLE quiet, and gives both back once."""
        wifi = (MUSE / "muse_video_wifi.c").read_text()
        self.assertIn("muse_wifi_keep_awake(on);\n    muse_link_ble_quiet(on);", wifi)
        self.assertEqual(wifi.count("radio_for_video(true);"), 1)
        self.assertEqual(wifi.count("radio_for_video(false);"), 2)   # the client's stop, the gadget's
        self.assertIn("__atomic_exchange_n(&s_streaming, false", wifi)
        ble = (ROOT / "main/ble_server.c").read_text()
        start = ble[ble.index("static void start_advertising(void) {"):]
        self.assertIn("if (s_quiet) return;", start[:400])
        self.assertIn(".ble_quiet = ble_server_set_quiet,", (ROOT / "main/muse_glue.c").read_text())


if __name__ == "__main__":
    unittest.main()
