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
        self.assertIn("if MUSE_VIDEO_DECODER_IRAM = y:", (ROOT / "components/libvpx_vp8/linker.lf").read_text())
        # The ring resyncs by the stream's codec, whatever the transport.
        self.assertIn("t->ring->codec = vp->codec;", video)

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
