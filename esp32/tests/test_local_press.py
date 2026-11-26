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

"""The update's confirm press (mgcommands.h, Token proof; mg-update-v2.json): components/muse_restart/muse_local_press*.

The pure core compiled and run. An update's start asks: the prompt starts and the change waits (rc 10); a press during
the prompt confirms it, bound to its upload for 600 s across link drops (or covering that one erase, reset or state
write on its link); an upload naming no SHA-256 and a chunk with no upload started on its link are refused (rc 11, no
prompt); a write past the bound length, or a last chunk or state write while the slot isn't the bound image (the
caller's hash result), is refused and ends the binding; Forget ends it too; a press before the prompt or after its
timeout never counts; a timeout or a cancel refuses once (rc 11); the wired console's arm needs no press; a passcode
lock asks first. Every exchange of mg-update-v2.json replayed through it behind mg_smp_gate.h. And the wiring: only the
gadget's own key on the prompt (the board's keys, the GPIO button, a touch board's Confirm) confirms, never injected
input; update.arm comes from the wired console's readers only.
"""

import hashlib
import json
import os
import shlex
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
COMP = ROOT / "components" / "muse_restart"
UPDATE_V2 = ROOT.parent / "protocols" / "test-vectors" / "mg-update-v2.json"
sys.path.insert(0, str(ROOT / "components" / "muse_gadget_ble" / "smp" / "conformance"))
import smplink as sl  # noqa: E402

TEST_VECTORS = ROOT.parent / "protocols" / "test-vectors"

DRIVER = r"""
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mg_smp_gate.h"
#include "muse_local_press.h"
static void hex(const char *h, uint8_t *out, int n) {
    for (int i = 0; i < n; i++) {
        unsigned b = 0;
        sscanf(h + 2 * i, "%2x", &b);
        out[i] = (uint8_t)b;
    }
}
int main(void) {
    muse_local_press_t w;
    muse_local_press_core_init(&w);
    char line[256];
    long long now = 0;
    int locked = 0;
    /* mg_smp_gate.h's inputs (a release build, unproven: the press), as mg_ble.c's smp_gate_now gives them */
    int release = 1, proof = 0, button = 0, self = 0;
    while (fgets(line, sizeof line, stdin)) {
        muse_update_req_t r;
        memset(&r, 0, sizeof(r));
        int have = 0;
        if (!strncmp(line, "time ", 5)) now += atoll(line + 5);
        else if (!strncmp(line, "press", 5)) printf("p%d\n", muse_local_press_core_note(&w, now, locked));
        else if (!strncmp(line, "link", 4)) { muse_local_press_core_link_new(&w); proof = 0; }
        else if (!strncmp(line, "cancel", 6)) muse_local_press_core_cancel(&w, now);
        else if (!strncmp(line, "arm", 3)) muse_local_press_core_arm(&w, now);
        else if (!strncmp(line, "unlock", 6)) locked = 0;
        else if (!strncmp(line, "lock", 4)) locked = 1;
        else if (!strncmp(line, "forget", 6)) { muse_local_press_core_forget(&w, now); proof = 0; }
        else if (!strncmp(line, "setup", 5)) { }   /* set up again (a new device token): the gate asks either way */
        else if (!strncmp(line, "gate ", 5)) sscanf(line + 5, "%d %d %d %d", &release, &proof, &button, &self);
        else if (!strncmp(line, "ask", 3)) { r.kind = MUSE_UPDATE_CHANGE; have = 1; }   /* a change: key 0 */
        else if (!strncmp(line, "up ", 3)) {
            /* "up <len> <sha hex or -> [<n> <slot ok>]": an upload's first chunk naming that image */
            char sha[80] = "";
            unsigned long len = 0, n = 32;
            int ok = 0;
            sscanf(line + 3, "%lu %79s %lu %d", &len, sha, &n, &ok);
            r.kind = MUSE_UPDATE_UPLOAD0;
            r.id.len = (uint32_t)len;
            r.id.has_sha = strcmp(sha, "-") != 0;
            if (r.id.has_sha) hex(sha, r.id.sha, 32);
            r.n = (uint32_t)n;
            r.slot_ok = ok;
            have = 1;
        } else if (!strncmp(line, "chunk ", 6)) {
            /* "chunk <off> <n> <slot ok>" */
            unsigned long off = 0, n = 0;
            int ok = 0;
            sscanf(line + 6, "%lu %lu %d", &off, &n, &ok);
            r.kind = MUSE_UPDATE_CHUNK;
            r.off = (uint32_t)off;
            r.n = (uint32_t)n;
            r.slot_ok = ok;
            have = 1;
        } else if (!strncmp(line, "state ", 6) || !strncmp(line, "change ", 7)) {
            /* "state <key hex> <slot ok>", "change <key hex>": an image state write, an erase or the reset */
            char key[80] = "";
            int ok = 0;
            r.kind = line[0] == 's' ? MUSE_UPDATE_STATE : MUSE_UPDATE_CHANGE;
            sscanf(strchr(line, ' ') + 1, "%79s %d", key, &ok);
            hex(key, r.key, 32);
            r.slot_ok = ok;
            have = 1;
        }
        else if (!strncmp(line, "prompt?", 7))
            printf("s%s\n", !muse_local_press_core_prompting(&w, now) ? "none" : locked ? "keypad" : "confirm");
        if (have) {
            /* The slot's hash result was taken against the binding as it is now (muse_local_press_bound). */
            muse_local_press_core_bound(&w, &r.slot_for);
            mg_smp_gate_t g = mg_smp_gate(button || self, button, proof, release);
            printf("%d\n", g == MG_SMP_GATE_OPEN ? 0 : g == MG_SMP_GATE_CLOSED ? 2
                           : (int)muse_local_press_core_ask(&w, now, locked, &r));
        }
    }
    return 0;
}
"""


class LocalPressWindow(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.tmp = tempfile.TemporaryDirectory()
        d = Path(cls.tmp.name)
        (d / "driver.c").write_text(DRIVER)
        cls.exe = d / "driver"
        cc = shlex.split(os.environ.get("CC", "cc"))
        r = subprocess.run([*cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-I", str(COMP / "include"),
                            "-I", str(ROOT / "components" / "muse_gadget_ble"), str(d / "driver.c"),
                            str(COMP / "muse_local_press_core.c"), "-o", str(cls.exe)],
                           capture_output=True, text=True)
        if r.returncode:
            raise AssertionError(r.stdout + r.stderr)

    @classmethod
    def tearDownClass(cls) -> None:
        cls.tmp.cleanup()

    def run_(self, *cmds: str) -> list[str]:
        r = subprocess.run([str(self.exe)], input="".join(c + "\n" for c in cmds), text=True, capture_output=True,
                           check=True)
        return r.stdout.split()

    # The core's answers: 0 open (rc 0), 1 asking (rc 10), 2 refused (rc 11).
    def test_the_start_asks_and_a_press_during_the_prompt_confirms(self) -> None:
        self.assertEqual(self.run_("ask", "prompt?", "time 3000", "ask", "press", "prompt?", "ask", "time 599999", "ask",
                                   "time 1", "ask"), ["1", "sconfirm", "1", "p1", "snone", "0", "0", "1"])
        h = (COMP / "include" / "muse_local_press.h").read_text()
        self.assertIn("#define MUSE_LOCAL_PRESS_MS 60000", h)
        self.assertIn("#define MUSE_UPDATE_PROMPT_MS 30000", h)
        self.assertIn("#define MUSE_UPDATE_BOUND_MS 600000", h)
        self.assertIn("#define MUSE_UPDATE_REARM_MS 10000", h)

    def test_a_new_prompt_shows_at_most_once_per_10_s_per_link(self) -> None:
        # As the re-setup prompt: after a cancel (refused once), or another upload right after a press, a change that
        # would show the prompt under 10 s after it last showed waits (1, nothing shown); at 10 s it shows; a new link
        # may show it at once.
        self.assertEqual(self.run_("ask", "cancel", "ask", "ask", "prompt?", "time 9999", "ask", "prompt?", "time 1",
                                   "ask", "prompt?"),
                         ["1", "2", "1", "snone", "1", "snone", "1", "sconfirm"])
        a, b = "01" * 32, "02" * 32
        self.assertEqual(self.run_(f"up 100 {a}", "press", f"up 100 {b}", "prompt?", "time 10000", f"up 100 {b}",
                                   "prompt?", f"up 100 {a}"),
                         ["1", "p1", "1", "snone", "1", "sconfirm", "0"])
        self.assertEqual(self.run_("ask", "cancel", "ask", "link", "ask", "prompt?"), ["1", "2", "1", "sconfirm"])

    def test_a_cancel_binds_nothing(self) -> None:
        # A cancel answers rc 11 once and ends the prompt, binding nothing: after it the same upload asks again
        # (once the 10 s since the last prompt have passed), on this link and on a new one.
        a = "01" * 32
        self.assertEqual(self.run_(f"up 100 {a}", "cancel", f"up 100 {a}", f"up 100 {a}", "prompt?", "time 10000",
                                   f"up 100 {a}", "prompt?"),
                         ["1", "2", "1", "snone", "1", "sconfirm"])
        self.assertEqual(self.run_(f"up 100 {a}", "cancel", "link", f"up 100 {a}", "prompt?"), ["1", "1", "sconfirm"])

    def test_a_press_at_any_other_time_never_counts(self) -> None:
        # Before any prompt, and after the prompt's end: nothing (nobody in range waits for the owner's touch).
        self.assertEqual(self.run_("press", "ask", "time 30000", "prompt?", "press", "ask", "ask"),
                         ["p0", "1", "snone", "p0", "2", "1"])

    def test_cancel_the_wired_arm_the_lock_and_a_new_link(self) -> None:
        self.assertEqual(self.run_("ask", "cancel", "ask", "ask"), ["1", "2", "1"])
        self.assertEqual(self.run_("arm", "ask"), ["0"])
        self.assertEqual(self.run_("arm", "link", "ask", "link", "ask"), ["0", "1"])   # the arm's link only
        self.assertEqual(self.run_("lock", "ask", "prompt?", "press", "ask", "unlock", "prompt?", "press", "ask"),
                         ["1", "skeypad", "p0", "1", "sconfirm", "p1", "0"])
        # A lock doesn't stop the clock: the 30 s run from the wake to the keypad, the unlock and the press within them.
        self.assertEqual(self.run_("lock", "ask", "time 29999", "unlock", "press", "ask"), ["1", "p1", "0"])
        self.assertEqual(self.run_("lock", "ask", "time 30000", "unlock", "press", "ask"), ["1", "p0", "2"])
        # A press on a change other than an upload covers that change on its link only.
        self.assertEqual(self.run_("ask", "press", "ask", "link", "ask"), ["1", "p1", "0", "1"])
        self.assertEqual(self.run_(f"change {'01' * 32}", "press", f"change {'02' * 32}", "time 10000",
                                   f"change {'01' * 32}"), ["1", "p1", "1", "0"])
        # The press binds to its upload for 600 s, across link drops; another image asks again.
        a, b = "01" * 32, "02" * 32
        self.assertEqual(self.run_(f"up 100 {a}", "press", f"up 100 {a}", "link", "time 120000", f"up 100 {a}",
                                   f"up 100 {b}", "up 100 -", "link", "time 480000", f"up 100 {a}"),
                         ["1", "p1", "0", "0", "1", "2", "1"])
        # An upload without its SHA-256 can't be asked: rc 11 and no prompt, so no press binds to it; a chunk with no
        # upload started on its link likewise.
        self.assertEqual(self.run_("up 100 -", "prompt?", "press", "up 100 -", "link", "chunk 32 68 1", f"up 100 {a}"),
                         ["2", "snone", "p0", "2", "2", "1"])
        # The bound length and the slot: a write past it, or a last chunk or state write the caller's hash says isn't
        # the bound image, is refused and ends the binding; within it, the transfer under way goes on after 600 s.
        self.assertEqual(self.run_(f"up 100 {a}", "press", f"up 100 {a}", "chunk 32 76 1", "chunk 32 68 1"),
                         ["1", "p1", "0", "2", "2"])
        self.assertEqual(self.run_(f"up 100 {a}", "press", f"up 100 {a}", "chunk 32 68 0", f"state {b} 1"),
                         ["1", "p1", "0", "2", "1"])
        self.assertEqual(self.run_(f"up 100 {a}", "press", f"up 100 {a}", f"state {b} 0", "chunk 32 68 1"),
                         ["1", "p1", "0", "2", "2"])
        self.assertEqual(self.run_(f"up 100 {a}", "press", f"up 100 {a}", "time 600000", "chunk 32 68 1",
                                   f"state {b} 1", f"up 100 {a}"), ["1", "p1", "0", "0", "0", "1"])
        # Forget ends the binding and what a press covered; the wired arm stays.
        self.assertEqual(self.run_(f"up 100 {a}", "press", "forget", "time 10000", f"up 100 {a}"), ["1", "p1", "1"])
        self.assertEqual(self.run_("ask", "press", "forget", "time 10000", "ask"), ["1", "p1", "1"])
        self.assertEqual(self.run_("arm", "forget", f"up 100 {a}"), ["0"])

    def test_mg_update_v2_through_the_core(self) -> None:
        # Every exchange of mg-update-v2.json through the core behind mg_smp_gate.h (smp writes: rc 0 / 10 / 11 as
        # 0 / 1 / 2; reads never ask), the prompt checked after every step. The slot's hash result each write
        # carries (slot_ok: mg_smp_front.c's in the gadget, test_mg_smp's replay) is the reference's own slot,
        # stepped beside it; the lengths the core checks itself.
        sys.path.insert(0, str(TEST_VECTORS))
        import mg_update_ref as ref  # noqa: E402
        data = json.loads(UPDATE_V2.read_text())
        self.assertEqual(data["prompt_timeout_ms"], 30000)
        self.assertEqual(data["confirm_window_ms"], 600000)
        rc = {0: "0", 10: "1", 11: "2"}
        seen = 0
        for ex in data["exchanges"]:
            dev = ex.get("device", {})
            model = ref.Device(**dev)
            cmds = [f"gate {int(dev.get('release', True))} {int(dev.get('proof', False))} "
                    f"{int(dev.get('button_recovery', False))} {int(dev.get('self_recovery_unprompted', False))}"]
            cmds += (["arm"] if dev.get("console_arm") else []) + (["lock"] if dev.get("locked") else [])
            want = []
            for st in ex["steps"]:
                ev = st.get("event")
                if ev == "injected_press":
                    pass   # never a confirm: the core never hears it (the wiring: test_the_muse_boards_keys)
                elif ev in ("time", "press", "cancel", "unlock", "reconnect", "forget", "set_up"):
                    cmds.append({"time": f"time {st.get('ms', 0)}", "press": "press", "cancel": "cancel",
                                 "unlock": "unlock", "reconnect": "link", "forget": "forget",
                                 "set_up": "setup"}[ev])
                    if ev == "press":
                        want.append("p" + str(int(model.prompt == "confirm")))
                elif ev:
                    self.fail(f"mg-update-v2.json: an event this replay doesn't know: {ev!r}")
                else:
                    frame = bytes.fromhex(st["smp"])
                    group, cmd = frame[4] << 8 | frame[5], frame[7]
                    body = ref.cbor_map(frame[8:])
                    if frame[0] & 7 != ref.SMP_WRITE or (group, cmd) not in ref.UPDATE_CHANGES:
                        self.assertEqual(st["rc"], 0)   # a read: never asked
                    else:
                        key = hashlib.sha256(frame[4:6] + frame[7:8] + frame[8:]).hexdigest()
                        ok = int(self.slot_ok(ref, model, frame, body))
                        if (group, cmd) == (1, 1):
                            data_n = len(body.get("data", b""))
                            if body.get("off") == 0 and "len" in body:
                                sha = body.get("sha")
                                cmds.append(f"up {body['len']} {sha.hex() if sha else '-'} {data_n} {ok}")
                            else:
                                cmds.append(f"chunk {body.get('off', 0)} {data_n} {ok}")
                        elif (group, cmd) == (1, 0):
                            cmds.append(f"state {key} {ok}")
                        else:
                            cmds.append(f"change {key}")
                        want.append(rc[st["rc"]])
                if ev:
                    model.event(ev, **({"ms": st["ms"]} if "ms" in st else {}))
                else:
                    self.assertEqual(model.smp(bytes.fromhex(st["smp"])), st["rc"])   # the reference agrees
                cmds.append("prompt?")
                want.append("s" + st["prompt"])
            self.assertEqual(self.run_(*cmds), want, ex["name"])
            seen += 1
        self.assertEqual(seen, 26)

    @staticmethod
    def slot_ok(ref, model, frame: bytes, body: dict) -> bool:
        """The slot's hash result for this write against the binding, as the gadget's front works it out: an upload
        write that ends at the bound length, with it written; a state write, the slot as it is."""
        if not model.bound:
            return False
        if frame[7] == 0:
            return model.slot_matches()
        keep = bytearray(model.slot)
        model.write_slot(frame)
        ok = model.slot_matches()
        model.slot = keep
        return ok

    def test_the_wired_arm(self) -> None:
        dev = (COMP / "muse_local_press.c").read_text()
        word = dev[dev.index("bool muse_local_press_console(const char *line)"):]
        self.assertIn("muse_local_press_core_arm(&s_w, now_ms());", word)   # the wire is the owner's: no press


class APressCountsOnlyWhileThePromptIsOnShow(unittest.TestCase):
    """While a view covers the update card (the video view writes the panel past LVGL) its exit key
    must not confirm the update: with the Muse UI's set_shown, muse_local_press_note counts a press only while the card
    is on show. muse_local_press.c itself, on host fakes."""

    FAKES = {
        "esp_log.h": "#pragma once\n#include <stdio.h>\n#define ESP_LOGI(t, ...) ((void)(t), (void)fprintf(stderr, __VA_ARGS__))\n"
                     "#define ESP_LOGW(t, ...) ((void)(t), (void)fprintf(stderr, __VA_ARGS__))\n",
        "esp_timer.h": "#pragma once\n#include <stdint.h>\nextern int64_t g_now_us;\n"
                       "static inline int64_t esp_timer_get_time(void) { return g_now_us; }\n",
        "freertos/FreeRTOS.h": "#pragma once\ntypedef int portMUX_TYPE;\n#define portMUX_INITIALIZER_UNLOCKED 0\n"
                               "#define portENTER_CRITICAL(m) ((void)(m))\n#define portEXIT_CRITICAL(m) ((void)(m))\n",
        # A BLE gadget's build: the only one that asks (the others link the stubs).
        "sdkconfig.h": "#pragma once\n#define CONFIG_MUSE_GADGET_BLE_AUDIO 1\n",
    }
    DRIVER = r"""
#include <stdio.h>
#include "muse_local_press.h"
int64_t g_now_us = 1000000;
static bool shown;
static bool shown_fn(void) { return shown; }
int main(void) {
    /* No Muse UI (no set_shown): a press during the prompt counts, as before. */
    static const muse_update_req_t chg = { .kind = MUSE_UPDATE_CHANGE };   /* an erase */
    int a0 = muse_local_press_ask(&chg);
    int p0 = muse_local_press_note("key");
    muse_local_press_link_new();
    muse_local_press_set_shown(shown_fn);
    /* The video view over the card: its exit key isn't the update's; then the card on show: the press counts. */
    int a1 = muse_local_press_ask(&chg);
    shown = false;
    int p1 = muse_local_press_note("the view's exit key");
    int a2 = muse_local_press_ask(&chg);
    shown = true;
    int p2 = muse_local_press_note("a press on the card");
    int a3 = muse_local_press_ask(&chg);
    printf("%d %d %d %d %d %d %d\n", a0, p0, a1, p1, a2, p2, a3);
    return 0;
}
"""

    def test_the_view_over_the_card(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            for name, text in self.FAKES.items():
                f = Path(d) / "fakes" / name
                f.parent.mkdir(parents=True, exist_ok=True)
                f.write_text(text)
            (Path(d) / "drv.c").write_text(self.DRIVER)
            exe = Path(d) / "drv"
            cc = shlex.split(os.environ.get("CC", "cc"))
            r = subprocess.run([*cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-I", str(Path(d) / "fakes"),
                                "-I", str(COMP / "include"), str(Path(d) / "drv.c"), str(COMP / "muse_local_press.c"),
                                str(COMP / "muse_local_press_core.c"), "-o", str(exe)], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            out = subprocess.run([str(exe)], capture_output=True, text=True).stdout.split()
        # no UI: asking, the press counts; with the UI: asking, the hidden press doesn't count, still asking; the
        # shown press counts; open (0)
        self.assertEqual(out, ["1", "1", "1", "0", "1", "1", "0"], out)

    def test_a_covered_card_is_not_on_show(self) -> None:
        # The video view returns before the chrome is drawn (the card's on-show state was left as it last was, so
        # its exit key counted as the update's confirm): the frame that video covers, and the dark screen's, clear it.
        ui = (ROOT / "components" / "muse" / "muse_ui.c").read_text()
        tick = ui[ui.index("static void frame_tick_body(void)"):]
        video = tick[tick.index("if (muse_video_tick()) {"):]
        video = video[:video.index("return;")]
        self.assertIn("atomic_store(&s_ask_shown, false);", video)
        dark = tick[tick.index("if (update_sleep()) {"):]
        dark = dark[:dark.index("return;")]
        self.assertIn("atomic_store(&s_ask_shown, false);", dark)

    def test_each_cards_confirm_acts_on_its_own_card(self) -> None:
        # One tap on the update's Confirm confirmed a re-setup waiting under it too: LVGL sends a press to the input
        # device first (on_any_press: the update's) and then to the control under it (on_pair_ok: a re-setup's).
        # The Confirm says whose it is; the update's tap goes no further; a tap while the update holds the card
        # acts on no re-setup.
        ui = (ROOT / "components" / "muse" / "muse_ui.c").read_text()
        ok = ui[ui.index("static void on_pair_ok(lv_event_t *e)"):]
        ok = ok[:ok.index("\n}\n")]
        upd_ok = ok[ok.index("if (s_pair_ok == PAIR_OK_UPDATE) {"):]
        upd_ok = upd_ok[:upd_ok.index("        return;\n    }")]
        self.assertIn('(void)muse_local_press_note("a tap on the update card\'s Confirm");', upd_ok)
        self.assertNotIn("muse_link_confirm_key_press(", upd_ok)   # the update's tap confirms no re-setup
        self.assertLess(ok.index("if (muse_local_press_prompting()) {\n        return;"), ok.index("muse_link_confirm_key_press("))
        self.assertIn("if (s_pair_ok == PAIR_OK_SETUP && s_ok_seq == muse_link_prompt_seq()) {\n"
                      "        (void)muse_link_confirm_key_press();", ok)
        press = ui[ui.index("static void on_any_press(lv_event_t *e)"):]
        press = press[:press.index("\n}\n")]
        upd = press[press.index("if (s_pair_ok == PAIR_OK_UPDATE"):]
        upd = upd[:upd.index("        return;\n    }")]
        self.assertIn("lv_indev_stop_processing(indev);", upd)   # not to the control under the finger
        self.assertIn("lv_indev_wait_release(indev);", upd)      # nor this touch's CLICKED
        # A tap elsewhere on a re-setup's card does nothing: no dismiss, no talk press.
        rest = press[press.index("#endif"):]
        self.assertNotIn("muse_link_confirm_dismiss", rest)
        self.assertNotIn("muse_input_touch_talk", rest)
        self.assertIn("set_pair_ok(ok ? PAIR_OK_SETUP : PAIR_OK_NONE);", ui)

    def test_the_surfaces(self) -> None:
        ui = (ROOT / "components" / "muse" / "muse_ui.c").read_text()
        self.assertIn("covered |= muse_video_is_open();", ui)
        self.assertIn("atomic_store(&s_ask_shown, ask && !covered && !lv_obj_has_flag(s_pair, LV_OBJ_FLAG_HIDDEN));", ui)
        self.assertIn("if (s_pair_ok == PAIR_OK_UPDATE && muse_local_press_prompting() && atomic_load(&s_ask_shown)) {", ui)
        glue = (ROOT / "main" / "muse_glue.c").read_text()
        self.assertIn("muse_local_press_set_shown(muse_ui_update_prompt_shown);", glue)
        dev = (COMP / "muse_local_press.c").read_text()
        note = dev[dev.index("bool muse_local_press_note(const char *what)"):]
        self.assertLess(note.index("if (shown && !shown()) {"), note.index("muse_local_press_core_note("))


class OnlyTheGadgetsOwnPressOnThePrompt(unittest.TestCase):
    """No pre-armed opener is left (the setup-screen hold, the 2 s BOOT hold, update.arm's window); a press of the
    gadget's own key while the prompt shows confirms (muse_input.c, main/button.c), or a touch board's on-screen
    Confirm (muse_ui.c); never injected input, never anything over Bluetooth."""

    def test_the_muse_boards_keys(self) -> None:
        inp = (ROOT / "components" / "muse" / "muse_input.c").read_text()
        self.assertNotIn("setup_hold", inp)
        self.assertIn("own = update_prompt_press(own);", inp)   # the board's own keys (poll_buttons) only
        self.assertLess(inp.index("unsigned own = muse_board->poll_buttons();"),
                        inp.index("own = update_prompt_press(own);"))
        self.assertLess(inp.index("own = update_prompt_press(own);"),
                        inp.index("muse_input_press(MUSE_PRESS_KEY);"))   # the confirming press is nothing else
        self.assertLess(inp.index("own = update_prompt_press(own);"), inp.index("unsigned keys = own | bench_talk();"))
        fn = inp[inp.index("static unsigned update_prompt_press(unsigned phys)"):]
        fn = fn[:fn.index("\n}\n")]
        self.assertIn("muse_local_press_prompting()", fn)
        self.assertEqual(inp.count("muse_local_press_note("), 1)
        touch = (ROOT / "components" / "muse" / "muse_touch.c").read_text()
        self.assertNotIn("muse_local_press", touch)   # a plain touch never confirms

    def test_a_press_down_before_the_prompt_showed_never_confirms(self) -> None:
        # Ordering, not time: the update's prompt takes a key's own press at its edge while the prompt is on show.
        # A key already down when it shows makes no edge on it, and a finger held on the face (MUSE_BTN_TALK_HELD,
        # reported long after it went down) never answers it.
        inp = (ROOT / "components" / "muse" / "muse_input.c").read_text()
        fn = inp[inp.index("static unsigned update_prompt_press(unsigned phys)"):]
        fn = fn[:fn.index("\n}\n") + 3]
        code = r"""
#include <assert.h>
#include <stdbool.h>
#define MUSE_BTN_TALK_PRESS 1u
#define MUSE_BTN_TALK_RELEASE 2u
#define MUSE_BTN_AUX_PRESS 4u
#define MUSE_BTN_AUX_RELEASE 8u
#define MUSE_BTN_ENTER 16u
#define MUSE_BTN_TALK_HELD (1u << 17)
static bool prompting, shown;
static int notes;
static bool muse_local_press_prompting(void) { return prompting; }
static bool muse_local_press_note(const char *what) {   /* muse_local_press.c: only while the card is on show */
    (void)what;
    if (!shown) return false;
    notes++;
    prompting = false;
    return true;
}
""" + fn + r"""
int main(void) {
    prompting = true;
    /* Down before the card showed: the press is the view's; held on into the card it's no edge, and confirms nothing. */
    assert(update_prompt_press(MUSE_BTN_TALK_PRESS) == MUSE_BTN_TALK_PRESS && !notes);
    shown = true;
    assert(update_prompt_press(0) == 0 && !notes);
    assert(update_prompt_press(MUSE_BTN_TALK_RELEASE) == MUSE_BTN_TALK_RELEASE && !notes);
    /* A finger held on the face, reported once its hold passes: never the update's. */
    unsigned face = MUSE_BTN_TALK_PRESS | MUSE_BTN_TALK_HELD;
    assert(update_prompt_press(face) == face && !notes);
    /* A key pressed once the card is on show: it confirms, and its release is the prompt's too. */
    assert(update_prompt_press(MUSE_BTN_AUX_PRESS) == 0 && notes == 1);
    assert(update_prompt_press(MUSE_BTN_AUX_RELEASE) == 0);
    return 0;
}
"""
        cc = shlex.split(os.environ.get("CC", "cc"))
        with tempfile.TemporaryDirectory(prefix="update-press-") as d:
            src = Path(d) / "h.c"
            src.write_text(code)
            r = subprocess.run([*cc, "-std=c11", "-Wall", "-Wextra", "-Werror", str(src), "-o", str(Path(d) / "h")],
                               capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            r = subprocess.run([str(Path(d) / "h")], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)

    def test_a_touch_boards_confirm(self) -> None:
        ui = (ROOT / "components" / "muse" / "muse_ui.c").read_text()
        press = ui[ui.index("static void on_any_press(lv_event_t *e)"):]
        press = press[:press.index("\n}\n")]
        self.assertIn("if (s_pair_ok == PAIR_OK_UPDATE && muse_local_press_prompting() && atomic_load(&s_ask_shown)) {", press)
        self.assertIn('} else if (on) {\n            muse_state_poke();\n            return;   /* to Confirm: its tap confirms (on_pair_ok) */', press)
        ok = ui[ui.index("static void on_pair_ok(lv_event_t *e)"):]
        self.assertIn("if (muse_local_press_prompting() && atomic_load(&s_ask_shown)) {\n"
                      '            (void)muse_local_press_note("a tap on the update card\'s Confirm");', ok[:ok.index("\n}\n")])
        self.assertIn('muse_local_press_cancel("a touch elsewhere on the update card");', press)
        self.assertLess(press.index("if (!touch_confirms()) {"), press.index("} else if (on) {"))   # real only
        self.assertIn('const char *title = ask ? "Confirm update"', ui)
        self.assertIn("set_pair_ok(ask && muse_board->touch ? PAIR_OK_UPDATE : PAIR_OK_NONE);", ui)

    def test_the_link_only_button(self) -> None:
        btn = (ROOT / "main" / "button.c").read_text()
        self.assertNotIn("MUSE_LOCAL_PRESS_HOLD_MS", btn)
        self.assertIn("if (pressed && !was_pressed && muse_local_press_prompting()\n"
                      "            && muse_local_press_note(", btn)
        self.assertEqual(btn.count("muse_local_press_note("), 1)
        self.assertIn("*pressed = gpio_get_level(BTN_GPIO) == 0;", btn)   # the GPIO itself (or a board's own reader)
        self.assertNotIn("bench", btn)
        led = (ROOT / "main" / "led_status.c").read_text()
        self.assertIn("bool prompting = muse_local_press_prompting();\n        if (prompting) state = LED_STATE_PAIRING_CONFIRM_REQUIRED;", led)
        self.assertIn("s_prompt_shown = false;   // ...and over the update's confirm pattern", led)
        self.assertIn("muse_local_press_set_shown(led_update_prompt_shown);", led)

    def test_the_wired_console_only(self) -> None:
        inp = (ROOT / "components" / "muse" / "muse_input.c").read_text()
        serial = inp[inp.index("static void serial_task(void *arg)"):]
        serial = serial[:serial.index("\n}\n")]
        self.assertIn("if (line && whole && muse_local_press_console(line)) {", serial)
        safe = inp[inp.index("static void safe_serial_task(void *arg)"):]
        self.assertIn("!muse_local_press_console(line)) {", safe[:600])
        self.assertEqual(inp.count("muse_local_press_console("), 2)   # the USB readers only: not console_command
        link = (ROOT / "main" / "sdk_token_console.c").read_text()
        feed = link[link.index("static void feed(line_reader_t *r, char c) {"):]
        feed = feed[:feed.index("\n}\n")]
        self.assertIn("!muse_local_press_console(r->buf + 1)", feed)
        self.assertEqual(link.count("muse_local_press_console("), 1)

    def test_a_screenless_boards_key_opens_start_mic(self) -> None:
        # A screenless musegadgets board (a voice board, a status light) never called mg_ble_press(), so a proven
        # start_mic answered press_required forever on a release image. Its button's real edge is now that press, as
        # muse_glue's press hook is on Muse boards.
        btn = (ROOT / "main" / "button.c").read_text()
        edge = btn[btn.index("        if (pressed && !was_pressed) {\n            press_start = esp_timer_get_time();"):]
        edge = edge[:edge.index("boot_press = muse_bootguard_entry_open();")]
        self.assertIn("#if CONFIG_MUSE_GADGET_BLE_AUDIO && !CONFIG_MUSE_ENABLED\n", edge)
        self.assertIn("mg_ble_press(MG_PRESS_SRC_KEY);", edge)
        self.assertIn("*pressed = gpio_get_level(BTN_GPIO) == 0;", btn)   # the GPIO itself (or a board's own reader)
        self.assertNotIn("bench", btn)
        glue = (ROOT / "main" / "muse_glue.c").read_text()
        self.assertIn("mg_ble_press(src == MUSE_PRESS_PANEL ? MG_PRESS_SRC_PANEL : MG_PRESS_SRC_KEY);", glue)

    def test_the_ask_wakes_the_screen_and_a_lock_shows_the_keypad(self) -> None:
        dev = (COMP / "muse_local_press.c").read_text()
        ask = dev[dev.index("muse_update_answer_t muse_local_press_ask(const muse_update_req_t *req)"):]
        ask = ask[:ask.index("\n}\n")]
        self.assertIn("wake(lock);", ask[ask.index("if (a == MUSE_UPDATE_ASKING && !was && on) {"):])
        inp = (ROOT / "components" / "muse" / "muse_input.c").read_text()
        take = inp[inp.index("static void update_wake_take(void)"):]
        take = take[:take.index("\n}\n")]
        self.assertIn('set_asleep(false, "an update asks");', take)
        self.assertIn("muse_passcode_request_keypad(MUSE_PASS_WHY_UPDATE);", take)
        self.assertLess(inp.index("update_wake_take();"), inp.index("own = update_prompt_press(own);"))
        ui = (ROOT / "components" / "muse" / "muse_passcode_ui.c").read_text()
        self.assertRegex(ui, r's_why == MUSE_PASS_WHY_UPDATE\s+\? "Unlock to update"')
        glue = (ROOT / "main" / "muse_glue.c").read_text()
        self.assertIn("muse_local_press_set_ui(muse_passcode_locked, muse_input_update_wake);", glue)

    def test_no_safe_prompt_and_nothing_over_bluetooth(self) -> None:
        guard = (COMP / "muse_bootguard.c").read_text()
        self.assertNotIn("muse_local_press", guard)   # a self-entered SAFE can't prompt (rc 11, mg_smp_gate.h)
        for f in (ROOT / "components" / "muse_gadget_ble").glob("*.c"):
            src = f.read_text()
            self.assertNotIn("muse_local_press_note(", src, f.name)   # nothing over Bluetooth
            self.assertNotIn("muse_local_press_console(", src, f.name)
        ble = (ROOT / "main" / "ble_server.c").read_text()
        conn = ble[ble.index("case BLE_GAP_EVENT_CONNECT:"):]
        self.assertIn("muse_local_press_link_new();", conn[:600])   # a new link ends it


if __name__ == "__main__":
    unittest.main()
