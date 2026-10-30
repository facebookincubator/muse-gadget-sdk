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

"""Hint captions: production muse_state.c against host fakes, and which captions are hints."""
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
MUSE = ROOT / "components/muse"
CC = shlex.split(os.environ.get("CC", "cc"))

FAKES = {
    "esp_timer.h": "#include <stdint.h>\nint64_t esp_timer_get_time(void);\n",
    "freertos/FreeRTOS.h": r'''
#include <stdint.h>
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(m) ((void)(m))
#define portEXIT_CRITICAL(m) ((void)(m))
#define portMAX_DELAY 0xffffffffu
#define pdFALSE 0
#define pdMS_TO_TICKS(n) (n)
#define BIT0 1u
#define BIT1 2u
typedef void *SemaphoreHandle_t;
typedef void *EventGroupHandle_t;
typedef int StaticSemaphore_t;
typedef int StaticEventGroup_t;
SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *s);
int xSemaphoreTake(SemaphoreHandle_t s, uint32_t t);
int xSemaphoreGive(SemaphoreHandle_t s);
EventGroupHandle_t xEventGroupCreateStatic(StaticEventGroup_t *g);
unsigned xEventGroupSetBits(EventGroupHandle_t g, unsigned b);
unsigned xEventGroupClearBits(EventGroupHandle_t g, unsigned b);
unsigned xEventGroupWaitBits(EventGroupHandle_t g, unsigned b, int c, int a, uint32_t t);
''',
    "freertos/event_groups.h": "",
    "freertos/semphr.h": "",
    "muse_text.h": "#include <stddef.h>\nvoid muse_text_to_ascii(char *s, size_t n);\n",
}

HARNESS = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "muse_state.h"

static int64_t now_us = 1000000;
int64_t esp_timer_get_time(void) { return now_us; }
void muse_text_to_ascii(char *s, size_t n) { (void)s; (void)n; }
static int dummy;
SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *s) { (void)s; return &dummy; }
int xSemaphoreTake(SemaphoreHandle_t s, uint32_t t) { (void)s; (void)t; return 1; }
/* Another task's caption, landing the moment the format lock is free. */
static const char *preempt;
int xSemaphoreGive(SemaphoreHandle_t s) {
    (void)s;
    if (preempt) {
        const char *p = preempt;
        preempt = NULL;
        muse_state_set_caption("%s", p);
    }
    return 1;
}
EventGroupHandle_t xEventGroupCreateStatic(StaticEventGroup_t *g) { (void)g; return &dummy; }
unsigned xEventGroupSetBits(EventGroupHandle_t g, unsigned b) { (void)g; return b; }
unsigned xEventGroupClearBits(EventGroupHandle_t g, unsigned b) { (void)g; return b; }
unsigned xEventGroupWaitBits(EventGroupHandle_t g, unsigned b, int c, int a, uint32_t t) {
    (void)g; (void)c; (void)a; (void)t; return b;
}

static uint32_t version;
static char shown[MUSE_CAPTION_MAX];

/* What the UI shows now; true if it changed since the last look. */
static int look(void) { return muse_state_caption(shown, sizeof(shown), &version); }
static void later(int ms) { now_us += (int64_t)ms * 1000; }

int main(int argc, char **argv) {
    muse_state_init();
    int test = argc > 1 ? argv[1][0] - '0' : -1;
    if (test == 0) {   /* a hint clears itself, once */
        muse_state_set_hint("HOLD LONGER TO TALK");
        assert(look() && strcmp(shown, "HOLD LONGER TO TALK") == 0);
        later(MUSE_HINT_MS - 1);
        assert(!look());
        later(2);
        assert(look() && shown[0] == 0);
        later(MUSE_HINT_MS * 2);
        assert(!look());   /* no further redraws */
    } else if (test == 1) {   /* a later plain caption keeps its place */
        muse_state_set_hint("SPEAKER ON");
        later(1000);
        muse_state_set_caption("REPLY INCOMPLETE");
        later(MUSE_HINT_MS * 3);
        assert(look() && strcmp(shown, "REPLY INCOMPLETE") == 0);
        assert(!look());
    } else if (test == 2) {   /* a plain caption set while the hint is being set wins */
        preempt = "NO REPLY FROM MUSE";
        muse_state_set_hint("HOLD LONGER TO TALK");
        assert(preempt == NULL);
        later(MUSE_HINT_MS * 3);
        assert(look() && strcmp(shown, "NO REPLY FROM MUSE") == 0);
    } else if (test == 3) {   /* a new hint restarts the clock */
        muse_state_set_hint("SPEAKER ON");
        later(MUSE_HINT_MS - 1000);
        muse_state_set_hint("SPEAKER OFF");
        later(2000);
        assert(look() && strcmp(shown, "SPEAKER OFF") == 0);
        later(MUSE_HINT_MS);
        assert(look() && shown[0] == 0);
    } else if (test == 4) {   /* expiry while nothing looks (asleep) shows up at the next look */
        muse_state_set_hint("PHONE SETUP OFF");
        assert(look());
        muse_state_set_asleep(true);
        later(MUSE_HINT_MS * 10);
        muse_state_set_asleep(false);
        assert(look() && shown[0] == 0);
    } else if (test == 5) {   /* whether a hint is still up */
        assert(!muse_state_hint_pending());
        muse_state_set_hint("HOLD TO MUTE");
        assert(muse_state_hint_pending());
        later(MUSE_HINT_MS + 1);
        assert(!muse_state_hint_pending());
        muse_state_set_hint("HOLD TO MUTE");
        muse_state_set_caption("HOLD TO POWER OFF");
        assert(!muse_state_hint_pending());
    } else {
        return 2;
    }
    puts("ok");
    return 0;
}
'''


def function(source, name):
    match = re.search(r"static void " + name + r"\([^)]*\)\n\{", source)
    end = source.index("\n}\n", match.start())
    return source[match.start():end + 2]


class CaptionHint(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix="muse-caption-hint-")
        root = Path(cls.tmp.name)
        for name, text in FAKES.items():
            path = root / "inc" / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text)
        (root / "harness.c").write_text(HARNESS)
        cls.exe = root / "hint"
        subprocess.run(
            [*CC, "-std=gnu11", "-Wall", "-Werror", "-include", str(ROOT / "tests/host_compat.h"),
             "-I", str(root / "inc"), "-I", str(MUSE),
             str(root / "harness.c"), str(MUSE / "muse_state.c"), "-o", str(cls.exe), "-lm"],
            check=True, capture_output=True, text=True, timeout=120,
        )

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def run_case(self, n):
        out = subprocess.run([str(self.exe), str(n)], capture_output=True, text=True, timeout=30)
        self.assertEqual(out.returncode, 0, out.stderr)

    def test_hint_expires_once(self):
        self.run_case(0)

    def test_plain_caption_cancels_expiry(self):
        self.run_case(1)

    def test_caption_racing_the_hint_wins(self):
        self.run_case(2)

    def test_new_hint_restarts_clock(self):
        self.run_case(3)

    def test_expiry_while_asleep_seen_on_wake(self):
        self.run_case(4)

    def test_hint_pending(self):
        self.run_case(5)

    def test_only_transient_notices_are_hints(self):
        voice = (MUSE / "muse_voice.c").read_text()
        # Replies, errors from Muse and status stay up: go_idle() keeps a plain caption.
        self.assertNotIn("muse_state_set_hint", function(voice, "go_idle"))
        self.assertEqual(re.findall(r'muse_state_set_hint\("([^"]*)"\)', voice), ["HOLD LONGER TO TALK"])
        self.assertNotIn('go_idle("HOLD LONGER TO TALK")', voice)
        inp = (MUSE / "muse_input.c").read_text()
        hints = re.findall(r"muse_state_set_hint\(([^;]*)\);", inp)
        self.assertEqual(hints, ['"PHONE SETUP OFF"', 'saved_caption', 'on ? "SPEAKER ON" : "SPEAKER OFF"'])
        self.assertIn('muse_state_set_caption("PHONE SETUP: %s", b.name)', inp)
        # Cancelling the power-off hold puts a hint back as a hint, anything else as it was.
        aux = function(inp, "aux_button")
        self.assertIn("saved_hint = muse_state_hint_pending();", aux)
        self.assertRegex(aux, r"if \(saved_hint\) \{\s*muse_state_set_hint\(saved_caption\);")
        ui = (MUSE / "muse_ui.c").read_text()
        on_speaker = function(ui, "on_speaker_event")
        self.assertNotIn("muse_state_set_caption", on_speaker)
        self.assertEqual(re.findall(r"muse_state_set_hint\(([^;]*)\);", on_speaker),
                         ['on ? "SPEAKER OFF" : "SPEAKER ON"', 'on ? "HOLD TO MUTE" : "HOLD TO UNMUTE"'])


if __name__ == "__main__":
    unittest.main()
