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

"""Bench presses of the device's own screen and buttons over BLE.

The Nordic UART's !IN <word> reaches the console's bench words (press=,
tap=<control>, key=, page= and the diagnostics), and only them. They're open
in automation builds; in a release build a state-changing word needs a
token-proven connection (test_mg_unlock).
"""

from __future__ import annotations

import re
import tempfile
import unittest
from pathlib import Path

from test_mg_ble import MG, ROOT, compile_and_run

MUSE = ROOT / "components/muse"


def function(text: str, signature: str) -> str:
    i = text.index(signature)
    return text[i:text.index("\n}\n", i) + 3]


class NordicUartBenchTest(unittest.TestCase):
    def test_open_and_only_the_allowlist(self) -> None:
        ble = (MG / "mg_ble.c").read_text()
        nus = function(ble, "static void nus_command(char *line)\n{")
        word = nus[nus.index('if (!strncmp(line, "!IN ", 4)) {'):]
        word = word[:word.index("return;\n    }") + 14]
        self.assertIn("bool ok = s_platform.bench_input != NULL;", word)
        self.assertNotIn("unlock", re.sub(r"/\*.*?\*/", "", word, flags=re.S))   # no unlock in the code
        glue = (ROOT / "main/mg_glue.c").read_text()
        self.assertIn(".bench_input = ble_bench_input,", glue)
        # Queued to the input task: the NUS write runs on the NimBLE host, which
        # must never wait on the UI.
        self.assertIn("return muse_input_console_post(line) ? 1 : -1;", glue)
        self.assertIn("nus_command(line);", function(ble, "static int nus_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)\n{"))
        self.assertIn('ESP_LOGW(TAG, "a bench word: busy");', word)
        inp = (MUSE / "muse_input.c").read_text()
        loop = function(inp, "static void input_task(void *arg)\n{")
        self.assertIn("while (xQueueReceive(s_post, line, 0) == pdTRUE) {\n                s_line_wireless = true;", loop)
        self.assertIn("muse_input_console_line(line);   /* the Nordic UART's !IN words (muse_input_console_post) */\n"
                      "                s_line_wireless = false;", loop)
        post = function(inp, "bool muse_input_console_post(const char *line)\n{")
        self.assertIn("xQueueSend(s_post, buf, 0)", post)        # never blocks the caller
        self.assertIn("xTaskNotifyGive(s_input);", post)
        # SAFE mode's platform has no Muse UI, and no bench input.
        safe = glue[glue.index("static void start_safe(void)"):]
        safe = safe[:safe.index("\n}\n")]
        self.assertNotIn("bench_input", safe)
        allow = function(glue, "static int ble_bench_input(char *line) {")
        harness = r"""
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
static int forwarded;
static bool muse_input_console_post(const char *line) { (void)line; forwarded++; return true; }
@ALLOW@
int main(void) {
    static const char *ok[] = { "tap=call.minus", "tap=home.speaker.hold", "press=1,2", "key=speaker", "key=talk,400", "key=talk,400,30", "key=talk?", "page=settings/sound",
                                "call", "call=0", "call?", "wake", "wake?", "ptt=1", "ptt=0", "mic.gain?", "mic.gain=30",
                                "spk.dsp", "spk.dsp?", "spk.dsp=off", "spk.dsp=700,-30,4,20", "ptt.minrise?",
                                "ptt.minrise=15", "ptt.minrise=0", "vol?", "vol=0", "vol=100", "vol=saved" };
    static const char *no[] = { "setup.reset", "power.off=5", "restart", "restart=panic", "sdk_token=x", "crash",
                                "pair.confirm", "ble.forget", "calls", "ptt=2", "wake2", "chat=hi", "f2.arm", "mic.gain",
                                "mic_gain=30", "spk.dsp.x", "spk_dsp=on", "ptt.minrise", "ptt_minrise=15", "vol", "volume=5", "vol!", "" };
    int bad = 0;
    for (unsigned i = 0; i < sizeof(ok) / sizeof(ok[0]); i++) { char b[64]; strcpy(b, ok[i]); if (ble_bench_input(b) != 1) { printf("refused %s\n", ok[i]); bad++; } }
    for (unsigned i = 0; i < sizeof(no) / sizeof(no[0]); i++) { char b[64]; strcpy(b, no[i]); if (ble_bench_input(b) != 0) { printf("allowed %s\n", no[i]); bad++; } }
    printf("%d %d\n", bad, forwarded);
    return 0;
}
""".replace("@ALLOW@", allow)
        with tempfile.TemporaryDirectory(prefix="mg-in-") as tmp:
            h = Path(tmp) / "a.c"
            h.write_text(harness)
            out = compile_and_run(self, [h], [], []).strip()
        self.assertEqual(out, "0 28", out)


class SubtitleWordsTest(unittest.TestCase):
    """The subtitle words (sub=, sub.now=, sub.clear) only copy and queue on
    the console task: the work goes to the musegadgets worker, where a
    phone's subtitles run. sub.now's words, layout and band (~3 KB of
    stack) overflowed the console task's 3.5 KB on the Watcher."""

    def test_console_queues_and_the_worker_renders(self) -> None:
        inp = (MUSE / "muse_input.c").read_text()
        start = inp.index("#if CONFIG_MUSE_GADGET_BLE_LIVE_CALL\n/* The subtitle words' work")
        bench = inp[start:inp.index("\n#endif\n", start)].split("\n", 1)[1]
        start = inp.index('    if (!strncmp(line, "sub=", 4) || !strncmp(line, "sub.now=", 8)) {')
        words = inp[start:inp.index("\n#endif", start)]
        # The console's part calls no subtitle code itself.
        for call in ("muse_subs_bench(", "muse_subs_call_end(", "muse_subs_event("):
            self.assertNotIn(call, words)
        mg = (ROOT / "components/muse_gadget_ble/mg_ble.c").read_text()
        self.assertIn("            case EV_RUN:\n                ev.run(ev.data);\n                break;", mg)
        harness = r"""
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define MUSE_BIG_CAPS 0
static void *heap_caps_malloc(size_t n, int caps) { (void)caps; return malloc(n); }
static int bench_calls, clear_calls, got_ms;
static char got[256];
static void muse_subs_bench(const char *text, int ms) { bench_calls++; snprintf(got, sizeof(got), "%s", text); got_ms = ms; }
static void muse_subs_call_end(void) { clear_calls++; }
static void (*queued_fn)(void *);
static void *queued_arg;
static bool queue_full;
static bool mg_ble_run_on_worker(void (*fn)(void *arg), void *arg) {
    if (queue_full || queued_fn) return false;
    queued_fn = fn;
    queued_arg = arg;
    return true;
}
static void worker_runs(void) { void (*fn)(void *) = queued_fn; queued_fn = NULL; fn(queued_arg); }
""" + bench + r"""
static bool console(char *line) {
""" + words + r"""
    return false;
}
int main(void) {
    char line[128];
    strcpy(line, "sub.now=Hello 你好 world");
    console(line);
    printf("inline %d %d queued %d\n", bench_calls, clear_calls, queued_fn != NULL);
    memset(line, 'x', sizeof(line) - 1);   /* the console's buffer is gone by the time the worker runs */
    worker_runs();
    printf("worker %d [%s] %d\n", bench_calls, got, got_ms);
    strcpy(line, "sub=one two");
    console(line);
    worker_runs();
    printf("reveal [%s] %d\n", got, got_ms);
    strcpy(line, "sub.clear");
    console(line);
    printf("clear inline %d\n", clear_calls);
    worker_runs();
    printf("clear worker %d\n", clear_calls);
    queue_full = true;
    strcpy(line, "sub.now=dropped");
    console(line);
    printf("full %d\n", bench_calls);
    return 0;
}
"""
        with tempfile.TemporaryDirectory(prefix="mg-subwords-") as tmp:
            h = Path(tmp) / "t.c"
            h.write_text(harness)
            out = compile_and_run(self, [h], [], [], extra=["-fsanitize=address,undefined", "-g"])
        self.assertIn("inline 0 0 queued 1", out)
        self.assertIn("@sub ok", out)
        self.assertIn("worker 1 [Hello 你好 world] 0", out)
        self.assertIn("reveal [one two] 300", out)
        self.assertIn("clear inline 0", out)
        self.assertIn("@sub cleared", out)
        self.assertIn("clear worker 1", out)
        self.assertIn("@sub busy", out)
        self.assertIn("full 2", out)


class LvglPostTest(unittest.TestCase):
    def test_no_touch_coordinates_or_passcode_on_the_mirrored_tags(self) -> None:
        # mg.* is on the Nordic UART log mirror, which any central reads with no pairing (release too):
        # coordinates, passcode keys and the passcode's state go only to the cable console's tags.
        inp = (MUSE / "muse_input.c").read_text()
        self.assertNotRegex(inp, r'ESP_LOG[EWIDV]\("mg\.[a-z]+",[^;]*%d, ?%d')
        self.assertNotRegex(inp, r'ESP_LOG[EWIDV]\("mg\.[a-z]+", "pass %s"')
        self.assertNotRegex(inp, r'ESP_LOG[EWIDV]\("mg\.[a-z]+", "tap %s[^"]*",\s*what\b')
        self.assertIn('return strncmp(what, "pass.", 5) ? what : "a passcode key";', inp)
        self.assertIn('ESP_LOGI("muse_pass", "pass %s", out);', inp)
        self.assertIn('#define BENCH_XY_TAG "muse_touch"', inp)
        # Neither tag is one the mirror carries by default.
        kconfig = (MG / "Kconfig").read_text()
        tags = re.search(r'config MUSE_GADGET_BLE_NUS_LOG_TAGS\n.*?default "([^"]*)"', kconfig, re.S).group(1)
        for tag in ("muse_touch", "muse_pass"):
            self.assertFalse(any(tag.startswith(p) for p in tags.split(",")), tag)

    def test_an_injected_talk_press_never_opens_the_mic_on_release(self) -> None:
        # On a release image a wireless talk press (the Nordic UART's ptt=1 or key=talk, a bench press or tap on a
        # talk surface) answers "real press needed" and never opens the mic; the gadget's own press does, and so does
        # the wired console's line (the USB console is fully open: line_talk_ok). Test and automation builds take
        # every source. muse_input_talk_ok compiled from muse_input.c.
        inp = (MUSE / "muse_input.c").read_text()
        hdr = (MUSE / "muse_input.h").read_text()
        macro = hdr[hdr.index("#if (defined(CONFIG_MUSE_GADGET_TEST_BUILD)"):hdr.index("#define MUSE_NO_PERSON 0\n#endif\n") + len("#define MUSE_NO_PERSON 0\n#endif\n")]
        self.assertIn("#define MUSE_INJECTED_TALK_OK MUSE_NO_PERSON\n", inp)
        macro += "#define MUSE_INJECTED_TALK_OK MUSE_NO_PERSON\n"
        # One "no person in the loop" predicate: mg_ble.c defines the same expression (it can't include muse_input.h),
        # and every person gate uses it (the touch confirms, the start_mic press gate, the update's confirm press).
        ble = (ROOT / "components" / "muse_gadget_ble" / "mg_ble.c").read_text()
        theirs = ble[ble.index("#if (defined(CONFIG_MUSE_GADGET_TEST_BUILD)"):ble.index("#define MG_NO_PERSON 0\n#endif\n") + len("#define MG_NO_PERSON 0\n#endif\n")]
        self.assertEqual(theirs.replace("MG_NO_PERSON", "X"), macro[:macro.index("#define MUSE_INJECTED")].replace("MUSE_NO_PERSON", "X"))
        self.assertIn("#if !MG_NO_PERSON\n    cfg.press_gate = true;", ble)
        self.assertIn("#if MG_NO_PERSON   /* no person in the loop", ble)   # SMP_RELEASE
        self.assertIn("#if MUSE_NO_PERSON\n    return true;", (MUSE / "muse_ui.c").read_text())   # touch_confirms
        ok = function(inp, "bool muse_input_talk_ok(bool real)\n{")
        self.assertIn("return MUSE_INJECTED_TALK_OK || real;", ok)
        for defs, want in (("", "0 1"), ("#define CONFIG_MUSE_GADGET_TEST_BUILD 1\n", "1 1"),
                           ("#define CONFIG_MUSE_GADGET_AUTOMATION 1\n", "1 1")):
            src = ("#include <stdbool.h>\n#include <stdio.h>\n" + defs + macro + ok + "\n"
                   "int main(void) { printf(\"%d %d\\n\", muse_input_talk_ok(false), muse_input_talk_ok(true)); return 0; }\n")
            with tempfile.TemporaryDirectory(prefix="mg-talk-") as tmp:
                h = Path(tmp) / "a.c"
                h.write_text(src)
                self.assertEqual(compile_and_run(self, [h], [], []).strip(), want, defs)
        ptt = inp[inp.index('    if (!strcmp(line, "ptt=1") || !strcmp(line, "ptt=0")) {'):]
        self.assertLess(ptt.index("if (line[4] == '1' && !line_talk_ok()) {"), ptt.index("post("))
        self.assertIn('printf("@ptt real press needed\\n");', ptt)
        key = function(inp, "static void bench_key_talk(const char *k)\n{")
        self.assertIn('if (strcmp(k, "talk?") && strcmp(k, "talk,cancel") && !line_talk_ok()) {', key)
        self.assertIn("return !s_line_wireless || muse_input_talk_ok(false);", function(inp, "static bool line_talk_ok(void)\n{"))
        self.assertIn('printf("@key real press needed\\n");', key)
        for f in ("muse_home.c", "muse_ui.c"):
            self.assertIn("if (!muse_input_talk_ok(muse_touch_real())) {", (MUSE / f).read_text(), f)
        self.assertNotIn("press_window", (ROOT / "main/muse_glue.c").read_text())   # no window lets one through

    def test_an_injected_key_never_confirms_a_setup_prompt_on_release(self) -> None:
        # The Nordic UART's key=aux (or speaker, power) is the board's button pressed, and the second key confirms a
        # setup or re-setup prompt (aux_key). On a release image a wireless key= while that prompt is up answers
        # "real press needed" and presses nothing, as a bench touch on its Confirm confirms nothing; with no prompt
        # it presses, the wired console's presses in every build, and test and automation builds take both.
        # bench_key, line_talk_ok and muse_input_talk_ok compiled from muse_input.c.
        inp = (MUSE / "muse_input.c").read_text()
        hdr = (MUSE / "muse_input.h").read_text()
        macro = hdr[hdr.index("#if (defined(CONFIG_MUSE_GADGET_TEST_BUILD)"):hdr.index("#define MUSE_NO_PERSON 0\n#endif\n") + len("#define MUSE_NO_PERSON 0\n#endif\n")]
        macro += "#define MUSE_INJECTED_TALK_OK MUSE_NO_PERSON\n"
        code = (function(inp, "bool muse_input_talk_ok(bool real)\n{") + function(inp, "static bool line_talk_ok(void)\n{")
                + function(inp, "static void bench_key(const char *k)\n{"))
        self.assertIn("        bench_key(k);\n        return true;", inp)   # the console's key= words
        fakes = r"""
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#define MUSE_BTN_AUX_PRESS 4u
#define MUSE_BTN_POWER_PRESS 16u
#define MUSE_BTN_SPEAKER_PRESS 64u
#define ESP_LOGW(tag, ...) ((void)(tag))
#define ESP_LOGI(tag, ...) ((void)(tag))
typedef enum { MUSE_LINK_ONLINE, MUSE_LINK_CONFIRM } muse_link_state_t;
static muse_link_state_t link_state;
static muse_link_state_t muse_link_state(void) { return link_state; }
static struct { const char *aux_button, *power_button, *speaker_button; } board = { "aux", "power", "speaker" }, *muse_board = &board;
static atomic_uint s_bench_keys, s_bench_keys_wireless;
static bool s_line_wireless;
bool muse_input_talk_ok(bool real);
/* Pressed: the wired and the wireless presses. */
static unsigned taken(void) { return atomic_exchange(&s_bench_keys, 0) | atomic_exchange(&s_bench_keys_wireless, 0); }
"""
        main = r"""
int main(void) {
    /* wireless, prompt up: one row per key; then no prompt; then the wired console with the prompt up */
    const char *keys[] = { "aux", "power", "speaker" };
    for (int i = 0; i < 3; i++) {
        s_line_wireless = true; link_state = MUSE_LINK_CONFIRM; bench_key(keys[i]);
        printf("%u ", taken());
    }
    s_line_wireless = true; link_state = MUSE_LINK_ONLINE; bench_key("aux");
    unsigned wireless = atomic_load(&s_bench_keys_wireless);   /* marked as the Nordic UART's (aux_key) */
    printf("%u ", taken());
    s_line_wireless = false; link_state = MUSE_LINK_CONFIRM; bench_key("aux");
    unsigned wired = atomic_load(&s_bench_keys_wireless);
    printf("%u %u %u\n", taken(), wireless, wired);
    return 0;
}
"""
        for defs, want in (("", "0 0 0 4 4 4 0"), ("#define CONFIG_MUSE_GADGET_TEST_BUILD 1\n", "4 16 64 4 4 4 0"),
                           ("#define CONFIG_MUSE_GADGET_AUTOMATION 1\n", "4 16 64 4 4 4 0")):
            src = (fakes + defs + macro + "#define printf(...) ((void)0)\n#define fflush(f) ((void)(f))\n" + code
                   + "#undef printf\n#undef fflush\n" + main)
            with tempfile.TemporaryDirectory(prefix="mg-key-") as tmp:
                h = Path(tmp) / "a.c"
                h.write_text(src)
                out = compile_and_run(self, [h], [], []).strip()
            self.assertEqual(out, want, defs)

    def test_one_slot_any_task_runs_on_the_frame_tick(self) -> None:
        # muse_ui_run_in_lvgl: a press that reads LVGL's objects waits for the
        # LVGL task's next frame; one at a time (a second is refused, never
        # queued behind a paused LVGL), and its argument is copied.
        ui = (MUSE / "muse_ui.c").read_text()
        slot = ui[ui.index("static atomic_int s_run_state;"):ui.index("static void run_posted(void)")]
        slot += function(ui, "static void run_posted(void)\n{")
        harness = """
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
@SLOT@
static char got[64]; static int calls;
static void fn(const char *a) { strcpy(got, a); calls++; }
int main(void) {
    char arg[16] = "home.speaker";
    int a = muse_ui_run_in_lvgl(fn, arg);
    int b = muse_ui_run_in_lvgl(fn, "set.speaker");        /* the slot is taken */
    strcpy(arg, "changed");                                 /* copied when posted */
    int before = calls;                                     /* nothing runs before the tick */
    run_posted();
    char first[64]; strcpy(first, got); int after = calls;
    int c = muse_ui_run_in_lvgl(fn, "page=x");              /* free again */
    run_posted(); run_posted();                             /* nothing twice */
    char lng[60]; memset(lng, 'x', 59); lng[59] = 0;
    int d = muse_ui_run_in_lvgl(fn, lng);                   /* too long */
    int e = muse_ui_run_in_lvgl(NULL, "x");
    printf("%d %d %d %d %d %d %d %d %s %s\\n", a, b, c, d, e, before, after, calls, first, got);
    return 0;
}
""".replace("@SLOT@", slot)
        self.assertIn("run_posted();", function(ui, "static void frame_tick_body(void)\n{"))
        with tempfile.TemporaryDirectory(prefix="mg-lvpost-") as tmp:
            h = Path(tmp) / "p.c"
            h.write_text(harness)
            out = compile_and_run(self, [h], [], []).strip()
        self.assertEqual(out, "1 0 1 0 0 0 1 2 home.speaker page=x")


if __name__ == "__main__":
    unittest.main()
