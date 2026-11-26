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

import re
import os
import shlex
import subprocess
import tempfile
import unittest
from pathlib import Path

from host_cjson import cjson_source
import tsan_run

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
                 str(MUSE / "muse_video_wire.c"), *cjson_source(JSON), "-lm"],
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

    def test_a_loss_never_splices_onto_a_frames_prefix(self) -> None:
        # A write refused past the credit (muse_vstream_drop) or cut short for room left the lost
        # frame's prefix in the ring, and the next bytes spliced onto it into a frame of the right length. The loss
        # boundary makes the consumer drop that prefix: the next whole frame comes out, marked after a gap.
        harness = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "muse_vstream.c"

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
    f[8] = f[9] = f[10] = 0;
    f[11] = 1;
    return MUSE_VIDEO_FRAME_HEADER_LEN + len;
}

int main(void)
{
    static muse_vstream_t s;
    static uint8_t ring[4096], a[1200], b[1200], out[1200];
    muse_vframe_t fr;
    muse_vstream_init(&s, ring, sizeof(ring), 1024);
    muse_vstream_start(&s);
    /* Frame 1 (500 bytes): its first 100 written, the rest refused (past the credit). Then frames 2 and 3, whose
     * bytes would complete frame 1's length if they spliced onto it. */
    uint32_t na = frame(a, 1, 500);
    muse_vstream_write(&s, a, 100);
    muse_vstream_drop(&s, na - 100);
    muse_vstream_write(&s, b, frame(b, 2, 300));
    muse_vstream_write(&s, b, frame(b, 3, 300));
    assert(muse_vstream_pop(&s, out, sizeof(out), &fr) == 300 && fr.number == 2 && fr.after_gap);
    assert(muse_vstream_pop(&s, out, sizeof(out), &fr) == 300 && fr.number == 3 && !fr.after_gap);
    /* A whole frame before the boundary is taken first; the cut one after it is dropped. */
    muse_vstream_write(&s, b, frame(b, 4, 200));
    muse_vstream_write(&s, a, 100);
    muse_vstream_drop(&s, na - 100);
    muse_vstream_write(&s, b, frame(b, 5, 300));
    muse_vstream_write(&s, b, frame(b, 6, 300));
    assert(muse_vstream_pop(&s, out, sizeof(out), &fr) == 200 && fr.number == 4 && !fr.after_gap);
    assert(muse_vstream_pop(&s, out, sizeof(out), &fr) == 300 && fr.number == 5 && fr.after_gap);
    assert(muse_vstream_pop(&s, out, sizeof(out), &fr) == 300 && fr.number == 6);
    /* A write cut short for room: the same. */
    muse_vstream_start(&s);
    for (int i = 0; i < 4; i++) muse_vstream_write(&s, b, frame(b, (uint16_t)(10 + i), 1000));   /* 4 x 1012 */
    muse_vstream_write(&s, a, na);   /* 512 bytes, 48 left: only its first 48 fit */
    for (int i = 0; i < 4; i++) {
        assert(muse_vstream_pop(&s, out, sizeof(out), &fr) == 1000 && fr.number == 10 + i);   /* whole, before it */
    }
    muse_vstream_write(&s, b, frame(b, 20, 300));
    muse_vstream_write(&s, b, frame(b, 21, 300));
    int32_t r;
    do {
        r = muse_vstream_pop(&s, out, sizeof(out), &fr);
    } while (r < 0);
    assert(r == 300 && fr.number == 20);
    puts("PASS loss boundary");
    return 0;
}
'''
        cc = shlex.split(os.environ.get("CC", "cc"))
        with tempfile.TemporaryDirectory() as tmp:
            src, exe = Path(tmp, "loss.c"), Path(tmp, "loss")
            src.write_text(harness)
            build = subprocess.run(
                [*cc, "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
                 f"-I{VSTREAM / 'include'}", f"-I{VSTREAM}", "-o", str(exe), str(src)],
                capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=60)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertIn("PASS loss boundary", run.stdout)


    def test_wired_under_the_option(self) -> None:
        cmake = (MUSE / "CMakeLists.txt").read_text()
        self.assertIn("if(CONFIG_MUSE_VIDEO)", cmake)
        self.assertIn('"muse_video_wifi.c" "muse_video_wire.c"', cmake)
        self.assertIn("muse_vstream tinyh264 libvpx_vp8", cmake)
        self.assertIn('list(APPEND srcs "muse_video.c")', cmake)
        self.assertIn('list(APPEND srcs "muse_vdec.c")', cmake)
        self.assertIn('list(APPEND srcs "muse_vdec_same.c")', cmake)
        self.assertIn('list(APPEND srcs "muse_yuv.c")', cmake)
        self.assertIn("if(CONFIG_MUSE_VIDEO OR CONFIG_MUSE_AVATAR_ANIMATIONS)", cmake)
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
        self.assertIn("if(CONFIG_MUSE_VIDEO OR CONFIG_MUSE_AVATAR_ANIMATIONS)", tiny)
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
        self.assertIn("take_both_slots();", swap)
        both = video[video.index("static void take_both_slots(void)"):]
        both = both[:both.index("\n}\n")]
        self.assertIn("for (int i = 0; i < 2; i++)", both)
        # Pictures stopping gives the screen back; a lost slot is given back, loudly.
        self.assertIn("esp_timer_get_time() - shown > STALL_US", video)
        self.assertIn("pictures lost (show task idle, none queued", video)

    def test_last_shot_is_only_a_whole_frame(self) -> None:
        # muse_video_shot_last serves the buffer only once a capture completed: a first shot that timed out (the
        # buffer allocated, never filled) or one cut mid-frame leaves nothing to serve.
        video = (MUSE / "muse_video.c").read_text()
        last = video[video.index("bool muse_video_shot_last("):]
        last = last[:last.index("\n}\n")]
        self.assertIn("!s_shot_whole", last)
        self.assertIn("s_shot_whole = false;   /* overwritten from here", video)
        self.assertLess(video.index("s_shot_whole = false;   /* overwritten from here"), video.index("s_shot_state = 2;"))
        done = video.index("s_shot_whole = true;\n                s_shot_state = 3;")
        self.assertGreater(done, 0)

    def test_a_still_after_a_missing_frame_asks_for_a_keyframe(self) -> None:
        # muse_vgate_still: a UI still patch after a frame number skipped (or bytes lost) puts the gate on a keyframe
        # and asks; one in sequence changes nothing; the next picture after the gap then waits for the keyframe.
        prog = r"""#include "muse_vgate.h"
#include <stdio.h>
int main(void)
{
    muse_vgate_t g;
    muse_vgate_start(&g);
    bool ask;
    int ok = muse_vgate_take(&g, 100, true, false, 1, 0, &ask);       /* keyframe 1 */
    ok &= !muse_vgate_still(&g, false, 2, 1000) && !g.need_key;         /* still 2: in sequence */
    ok &= muse_vgate_take(&g, 50, false, false, 3, 2000, &ask);         /* P 3 */
    int gap_ask = muse_vgate_still(&g, false, 5, 3000);                 /* still 5: frame 4 missed */
    ok &= g.need_key;
    ok &= !muse_vgate_take(&g, 50, false, false, 6, 4000, &ask);        /* P 6: waits for the keyframe */
    ok &= muse_vgate_take(&g, 100, true, false, 7, 5000, &ask);         /* keyframe 7: on again */
    int lost = muse_vgate_still(&g, true, 8, 600000000) && g.need_key;  /* bytes lost before still 8 */
    printf("still gate: %d %d %d\n", ok, gap_ask, lost);
    return 0;
}
"""
        import os, shlex, subprocess, tempfile
        cc = shlex.split(os.environ.get("CC", "cc"))
        with tempfile.TemporaryDirectory() as tmp:
            c = Path(tmp) / "g.c"
            c.write_text(prog)
            exe = Path(tmp) / "g"
            b = subprocess.run(cc + ["-std=gnu11", "-Wall", "-Werror", "-Wno-unused-function", "-I", str(MUSE), str(c),
                                     "-o", str(exe)],
                               capture_output=True, text=True)
            self.assertEqual(b.returncode, 0, b.stderr)
            out = subprocess.run([str(exe)], capture_output=True, text=True).stdout
        self.assertIn("still gate: 1 1 1", out)

    def test_unlocking_repaints_a_quiet_ui_stream(self) -> None:
        # The passcode keypad drew over the UI picture; a quiet change-only stream sends nothing new after the
        # unlock, so the show task asks for a keyframe once the hold ends (as it does when the screen wakes).
        video = (MUSE / "muse_video.c").read_text()
        i = video.index("bool covered = held();")
        block = video[i:video.index("was_held = covered;", i)]
        self.assertIn("if (was_held && !covered && !asleep) {", block)
        self.assertIn("ask_keyframe();", block)

    def test_the_show_task_reads_its_slot_before_giving_it_back(self) -> None:
        # Once the slot goes back the decode task may refill it (number, timings): everything read after the
        # give comes from a copy taken before it.
        video = (MUSE / "muse_video.c").read_text()
        i = video.index("const slot_t done_sl = *sl;")
        give = video.index("xSemaphoreGive(s_slots);", i)
        self.assertLess(i, give)
        after = video[give:video.index("if (now - poked > 1000000) {", give)]
        self.assertNotIn("sl->", after.replace("done_sl.", ""))
        self.assertIn("s_last_number = done_sl.number;", after)
        self.assertIn("s_shown32 = done_sl.number32;", after)

    def test_wifi_start_needs_the_users_view(self) -> None:
        # A BLE client's start opens the view too (s_open, so the picture shows); that must not admit a Wi-Fi
        # start without the user's opt-in once the BLE stream ends: the gate reads the user's view (s_view).
        video = (MUSE / "muse_video.c").read_text()
        wifi = (MUSE / "muse_video_wifi.c").read_text()
        self.assertIn("if (!s_want && !muse_video_view_open()) {", wifi)
        self.assertNotIn("muse_video_is_open()", wifi)
        self.assertEqual(video.count("s_view = true;"), 1)
        opened = video[video.index("void muse_video_open(void)"):]
        self.assertIn("s_view = true;", opened[:opened.index("\n}\n")])
        closed = video[video.index("void muse_video_close(void)"):]
        self.assertIn("s_view = false;", closed[:closed.index("\n}\n")])
        begin = video[video.index("static bool begin_locked(const muse_video_transport_t *t, const muse_video_params_t *vp)\n{"):]
        begin = begin[:begin.index("\n}\n")]
        self.assertIn("s_open = true;", begin)
        self.assertNotIn("s_view", begin)

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


class PictureNumbers(unittest.TestCase):
    """muse_vdec numbers pictures across every decoder (the band decoder runs beside the main one, on the other
    core): two threads taking numbers never get the same one, and none is 0."""

    def test_unique_across_two_decoders(self):
        import os, shlex, subprocess, tempfile
        src = (ROOT / "components/muse/muse_vdec.c").read_text()
        decl = src[src.index("static atomic_uint_fast32_t s_seq;"):]
        decl = decl[:decl.index("\n}\n") + 3]
        self.assertEqual(src.count("++s_seq"), 0)
        self.assertGreaterEqual(src.count("next_seq()"), 2)
        prog = "#include <pthread.h>\n#include <stdatomic.h>\n#include <stdint.h>\n#include <stdio.h>\n#include <stdlib.h>\n" + decl + r"""
#define N 200000
static uint32_t got[2][N];
static void *take(void *arg) { uint32_t *g = arg; for (int i = 0; i < N; i++) g[i] = next_seq(); return NULL; }
static int cmp(const void *a, const void *b) { uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b; return x < y ? -1 : x > y; }
int main(void)
{
    pthread_t t[2];
    for (int k = 0; k < 2; k++) pthread_create(&t[k], NULL, take, got[k]);
    for (int k = 0; k < 2; k++) pthread_join(t[k], NULL);
    static uint32_t all[2 * N];
    for (int i = 0; i < N; i++) { all[i] = got[0][i]; all[N + i] = got[1][i]; }
    qsort(all, 2 * N, sizeof(all[0]), cmp);
    int dup = 0, zero = all[0] == 0;
    for (int i = 1; i < 2 * N; i++) dup += all[i] == all[i - 1];
    printf("numbers: %d taken, %d duplicates, zero %d, last %u\n", 2 * N, dup, zero, all[2 * N - 1]);
    return 0;
}
"""
        cc = shlex.split(os.environ.get("CC", "cc"))
        with tempfile.TemporaryDirectory() as tmp:
            c = Path(tmp) / "seq.c"
            c.write_text(prog)
            exe = Path(tmp) / "seq"
            b = subprocess.run(cc + ["-std=gnu11", "-O2", "-Wall", "-Werror", "-fsanitize=thread", str(c), "-o", str(exe),
                                     "-lpthread"], capture_output=True, text=True)
            self.assertEqual(b.returncode, 0, b.stderr)
            r = tsan_run.run(self, [str(exe)], timeout=120)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("numbers: 400000 taken, 0 duplicates, zero 0, last 400000", r.stdout)


if __name__ == "__main__":
    unittest.main()
