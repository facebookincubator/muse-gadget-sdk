#!/usr/bin/env python3
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

"""speaker_volume reports (mgcommands.h, Settings reports).

Every way the device's user mutes or changes the level reports speaker_volume
unsolicited (0 when muted, the new level when unmuted), and the client's own
set_settings is never reported back. The firmware's own code runs here: the
glue's set_speaker_volume and speaker_observer (main/mg_glue.c) over
mg_spk_report.h, the live call's volume_set_quiet (muse_call.c), the home
slider's release (muse_home.c), against settings that notify the way
muse_settings.c does (each write notifies once, on the writer's task).
"""

from __future__ import annotations

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))   # its helpers, also when run from esp32/ as tests.<module>
from test_mg_ble import MG, ROOT, compile_and_run

MUSE = ROOT / "components/muse"


def function(text: str, signature: str) -> str:
    """A function's definition, from its signature to the closing brace at column 0."""
    i = text.index(signature)
    j = text.index("\n}\n", i)
    return text[i:j + 3]


def branch(text: str, start: str, opener: str) -> str:
    """The statements inside one `if` of a function (four-space indented), without its braces."""
    body = function(text, start)
    i = body.index(opener) + len(opener)
    j = body.index("\n    }", i)
    return body[i:j]


HARNESS = r"""
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "mg_spk_report.h"

#define ESP_LOGI(tag, ...) ((void)(tag))
#define portENTER_CRITICAL(m) ((void)(m))
#define portEXIT_CRITICAL(m) ((void)(m))
#define portMUX_INITIALIZER_UNLOCKED 0
typedef int portMUX_TYPE;
typedef enum { MUSE_SETTING_VOLUME, MUSE_SETTING_SPEAKER, MUSE_SETTING_OTHER } muse_setting_t;
enum { mg_setting_parameter_speaker_volume = 10 };

/* muse_settings.c's shape: a write stores and notifies (listener, then observer). */
static int vol = 40;
static bool spk = true;
static void (*observer)(muse_setting_t);
static void (*interloper)(void);   /* another task's change, landing inside a write */
static void notify(muse_setting_t w)
{
    if (interloper) {
        void (*f)(void) = interloper;
        interloper = NULL;
        f();
    }
    if (observer) observer(w);
}
static int muse_settings_volume(void) { return vol; }
static bool muse_settings_speaker_on(void) { return spk; }
static int writes;   /* saved writes (each setter saves in muse_settings.c) */
static void muse_settings_set_volume(int v) { vol = v < 0 ? 0 : v > 100 ? 100 : v; writes++; notify(MUSE_SETTING_VOLUME); }
static void muse_settings_set_speaker_on(bool on) { spk = on; writes++; notify(MUSE_SETTING_SPEAKER); }
/* Who made a change (the gadget shows the phone's with "Set by phone"). */
typedef enum { MUSE_VOL_BY_DEVICE, MUSE_VOL_BY_PHONE, MUSE_VOL_BY_BENCH } muse_vol_by_t;
static muse_vol_by_t last_by;
static void muse_settings_set_volume_by(int v, muse_vol_by_t by) { muse_settings_set_volume(v); last_by = by; }
static void muse_settings_set_speaker_on_by(bool on, muse_vol_by_t by) { muse_settings_set_speaker_on(on); last_by = by; }

/* The reports the client gets: mg_proto reads the level as it sends. */
static int reports[64], nreports;
static int speaker_volume(void);
static void mg_ble_report_setting(uint8_t param)
{
    if (param == mg_setting_parameter_speaker_volume && nreports < 64) reports[nreports++] = speaker_volume();
}

/* ---- main/mg_glue.c ---- */
@GLUE@

/* ---- muse_call.c ---- */
static long long s_controls_until_us;
#define CONTROLS_SHOW_US 3000000LL
static long long now_us(void) { return 0; }
static const char *TAG = "call";
@CALL@

/* ---- muse_home.c: the slider's release ---- */
static void home_slider_release(int v)
{
@SLIDER@
}

static void expect(const char *what, int n, const int *want)
{
    bool ok = n == nreports;
    for (int i = 0; ok && i < n; i++) ok = reports[i] == want[i];
    printf("%s %s:", ok ? "ok" : "FAIL", what);
    for (int i = 0; i < nreports; i++) printf(" %d", reports[i]);
    printf("\n");
    nreports = 0;
}

static void from(int v, bool on) { vol = v; spk = on; nreports = 0; s_spk_report.reported = on ? v : 0; }
static void blue_or_long_press(void) { muse_settings_set_speaker_on(!muse_settings_speaker_on()); }
static void louder_meanwhile(void) { muse_settings_set_volume(90); }
/* The console's vol=N and vol=saved (RAM only), as muse_settings.c does them: the level and the
 * switch in RAM, then one MUSE_SETTING_VOLUME notice (test_muse_ptt_ui's settings harness holds that). */
static int saved_vol = -1; static bool saved_spk;
static void bench_vol(int pct) { if (saved_vol < 0) { saved_vol = vol; saved_spk = spk; } if (pct > 0) vol = pct; spk = pct > 0; notify(MUSE_SETTING_VOLUME); }
static void bench_restore(void) { vol = saved_vol; spk = saved_spk; saved_vol = -1; notify(MUSE_SETTING_VOLUME); }

int main(void)
{
    observer = speaker_observer;
    /* The StopWatch's blue key outside a call, the home Speaker's long press,
     * the Sound page's switch: a toggle reports 0, then the level. */
    from(40, true);
    blue_or_long_press(); expect("toggle mutes", 1, (int[]){0});
    blue_or_long_press(); expect("toggle unmutes", 1, (int[]){40});
    /* The home slider: to 0 mutes (0 once), from 0 to 50 unmutes (50 once). */
    from(40, true);
    home_slider_release(0); expect("home slider to 0", 1, (int[]){0});
    home_slider_release(50); expect("home slider to 50", 1, (int[]){50});
    /* The Sound page's volume slider: the level alone (0 is muted too). */
    from(40, true);
    muse_settings_set_volume(0); expect("sound slider to 0", 1, (int[]){0});
    muse_settings_set_volume(55); expect("sound slider to 55", 1, (int[]){55});
    /* The live call: volume − (the panel, the StopWatch's red key) and a swipe
     * down to 0, then + up again: each step reports, 0 once, then the level. */
    from(13, true);
    volume_set_quiet(6, "-", true); volume_set_quiet(0, "-", true);
    expect("call volume - to 0", 2, (int[]){6, 0});
    volume_set_quiet(6, "+", true); expect("call volume + from 0", 1, (int[]){6});
    from(100, true);
    volume_set_quiet(0, "swipe", true); expect("call swipe to 0", 1, (int[]){0});
    volume_set_quiet(19, "swipe", true); expect("call swipe up", 1, (int[]){19});
    /* A step that changes nothing reports nothing. */
    from(0, false);
    volume_set_quiet(0, "-", true); expect("call - at 0", 0, NULL);
    /* The bench's vol=0 and vol=saved: changes made on the device, reported like a key's. */
    from(40, true);
    bench_vol(0); expect("bench vol=0", 1, (int[]){0});
    bench_vol(25); expect("bench vol=25", 1, (int[]){25});
    bench_restore(); expect("bench vol=saved", 1, (int[]){40});
    /* The client's own set_settings: never reported back, whatever the
     * speaker was (muted at 40 and asked for 70 is two writes). */
    from(40, false);
    set_speaker_volume(70); expect("client 70 while muted", 0, NULL);
    set_speaker_volume(0); expect("client 0", 0, NULL);
    set_speaker_volume(0); expect("client 0 again", 0, NULL);
    set_speaker_volume(30); expect("client 30 from 0", 0, NULL);
    set_speaker_volume(31); expect("client 31", 0, NULL);
    printf("%s the client's set is the phone's: %d\n", last_by == MUSE_VOL_BY_PHONE ? "ok" : "FAIL", (int)last_by);
    /* The level it already plays at (an app's sync, a bench tool's round trip): nothing written, so a bench
     * volume (vol=, RAM only) stays and the saved volume isn't touched. */
    writes = 0;
    set_speaker_volume(31);
    printf("%s client 31 again: %d writes\n", writes == 0 ? "ok" : "FAIL", writes);
    from(40, false);
    writes = 0;
    set_speaker_volume(0);
    printf("%s client 0 while off: %d writes\n", writes == 0 && !spk && vol == 40 ? "ok" : "FAIL", writes);
    from(40, true);
    /* ...and a device change after it is reported against what the client set. */
    blue_or_long_press(); expect("device mute after the client's set", 1, (int[]){0});
    /* A device change landing inside the client's set (another task) is
     * reported once the client's is done, with the level then. */
    from(40, false);
    interloper = louder_meanwhile;
    set_speaker_volume(70); expect("device change during the client's", 1, (int[]){90});
    return 0;
}
"""


class SpeakerReportsTest(unittest.TestCase):
    def test_every_mute_path_reports_and_the_client_set_does_not(self) -> None:
        glue = (ROOT / "main/mg_glue.c").read_text()
        call = (MUSE / "muse_call.c").read_text()
        home = (MUSE / "muse_home.c").read_text()
        glue_code = "\n".join([
            function(glue, "static int speaker_volume(void) {"),
            glue[glue.index("static mg_spk_report_t s_spk_report"):glue.index("static void report_speaker_volume(")],
            function(glue, "static void report_speaker_volume("),
            function(glue, "static void set_speaker_volume(uint8_t volume) {"),
            function(glue, "static void speaker_observer(muse_setting_t what) {"),
        ])
        call_code = "\n".join([
            function(call, "static int volume_now(void)\n{"),
            function(call, "static void volume_set_quiet(int pct, const char *why, bool show)\n{"),
        ])
        slider = branch(home, "static void on_slider(lv_event_t *e)\n{",
                        "if (lv_event_get_code(e) == LV_EVENT_RELEASED) {")
        src = HARNESS.replace("@GLUE@", glue_code).replace("@CALL@", call_code).replace("@SLIDER@", slider)
        import tempfile
        with tempfile.TemporaryDirectory(prefix="mg-spk-") as tmp:
            h = Path(tmp) / "spk.c"
            h.write_text(src)
            out = compile_and_run(self, [h], [], [MG / "include"], extra=["-Wno-unused-function"])
        bad = [line for line in out.splitlines() if not line.startswith("ok")]
        self.assertEqual(bad, [], out)
        self.assertEqual(len(out.splitlines()), 24, out)


if __name__ == "__main__":
    unittest.main()
