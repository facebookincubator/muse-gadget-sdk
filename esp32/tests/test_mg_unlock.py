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
"""The Nordic UART console's bench words (no owner unlock any more):
!PAIR AUTO, !RESTART and the harmless answer to an old tool's ?UNLOCK /
!UNLOCK / !LOCK, from components/muse_gadget_ble/mg_unlock.c on the host, and
which words a release build takes only from a token-proven connection."""

from __future__ import annotations

import os
import shlex
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))   # its helpers, also when run from esp32/ as tests.<module>
from host_child import run_child  # noqa: E402 (macOS's pipe deadlock on a driver's input: host_child.py)

ROOT = Path(__file__).resolve().parents[1]
MG = ROOT / "components" / "muse_gadget_ble"


class MgConsoleWords(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.tmp = tempfile.TemporaryDirectory()
        cls.exe = Path(cls.tmp.name) / "unlock"
        cc = shlex.split(os.environ.get("CC", "cc"))
        subprocess.run(cc + ["-std=c11", "-Wall", "-Wextra", "-Werror", "-I", str(MG / "include"),
                             str(ROOT / "tests" / "mg_unlock_harness.c"), str(MG / "mg_unlock.c"), "-o",
                             str(cls.exe)], check=True)

    @classmethod
    def tearDownClass(cls) -> None:
        cls.tmp.cleanup()

    def run_lines(self, lines: list[str]) -> list[str]:
        r = run_child([str(self.exe)], input="\n".join(lines) + "\n", text=True, capture_output=True,
                      check=True)
        return r.stdout.splitlines()

    def test_pair_auto_open_once(self) -> None:
        # Open on the Nordic UART (benches stay automatable), and
        # the USB console's >pair.auto arms the same.
        out = self.run_lines(["> !PAIR AUTO", "auto", "take", "take", "auto", "> !PAIR AUTO 1", "> !PAIR AUTO 0",
                              "auto", "arm", "auto", "disarm", "auto"])
        self.assertEqual(out, ["PAIR AUTO on (one pairing, 10 min)", "auto 1", "take 1", "take 0", "auto 0",
                               "PAIR AUTO on (one pairing, 10 min)", "PAIR AUTO off", "auto 0", "auto 1", "auto 0"])

    def test_pair_auto_off_and_expiry(self) -> None:
        out = self.run_lines(["t 5", "arm", "disarm", "auto", "arm", "t 600000004", "auto", "t 600000006", "auto",
                              "> status"])
        self.assertEqual(out, ["auto 0", "auto 1", "auto 0", "(not mine)"])

    def test_ble_pause_argument(self) -> None:
        # >ble.pause=SECS[,ADDR]: SECS 1-600, ADDR AA:BB:CC:DD:EE:FF (either case), most significant first.
        cases = {"30": "pause 30", "1": "pause 1", "600": "pause 600", "0": "pause no", "601": "pause no",
                 "": "pause no", "30s": "pause no", "-5": "pause no", "12345": "pause no",
                 "30,AA:BB:CC:DD:EE:FF": "pause 30 aabbccddeeff", "600,0a:1b:2c:3d:4e:5f": "pause 600 0a1b2c3d4e5f",
                 "30,": "pause no", "30,AA:BB:CC:DD:EE": "pause no", "30,AA:BB:CC:DD:EE:FG": "pause no",
                 "30,AA-BB-CC-DD-EE-FF": "pause no", "30,AA:BB:CC:DD:EE:FF:00": "pause no",
                 "0,AA:BB:CC:DD:EE:FF": "pause no", "30 AA:BB:CC:DD:EE:FF": "pause no"}
        out = self.run_lines(["pause " + k for k in cases])
        self.assertEqual(out, list(cases.values()))

    def test_pair_auto_never_armed_over_the_air(self) -> None:
        # The bench auto-confirm presses the next setup confirmation for the user, with the talk key's semantics
        # (muse_link_bench_confirm). It's for benches: a release build never arms it from the Nordic UART, on a
        # proven connection either, in SAFE or out; "!PAIR AUTO 0" (disarming) still runs, and the USB console's
        # >pair.auto arms it in every build.
        for safe in "01":
            out = self.run_lines([f"remote 1 {safe} " + w for w in ("!PAIR AUTO", "!PAIR AUTO 1", "!PAIR AUTO 0")])
            self.assertEqual(out, ["locked", "locked", "runs"], safe)
        self.assertEqual(self.run_lines(["arm", "auto"]), ["auto 1"])   # >pair.auto
        ble = (MG / "mg_ble.c").read_text()
        self.assertIn('ESP_LOGW(TAG, "a bench-only word: refused on a release build");', ble)

    def test_remote_words_in_both_build_types(self) -> None:
        # CONFIG_MUSE_GADGET_AUTOMATION on (a bench build, MUSE_BENCH=1) keeps every Nordic UART word open.
        # Off (a release build, every board overlay), a word that changes state runs only on a connection
        # whose token proof matched and answers "locked" otherwise; in SAFE, safe.off / safe=0 (the way out of
        # SAFE) stay open; the reads stay open; the USB console stays open.
        words = ["!PAIR AUTO", "!PAIR AUTO 0", "!RESTART 5000", "!SAFEID", "!SILENCE", "!SETUP ADV", "!SETUP ADV 0",
                 "!LOCK", "!UNLOCK 00", "?UNLOCK", "!IN key=talk,400", "!IN key=talk,400,30", "!IN key=aux", "!IN ptt=1",
                 "!IN tap=home.speaker", "!IN press=1,2", "!IN page=settings", "!IN wake", "!IN call",
                 "!IN call=0", "!IN session.ping", "!IN vib=pulse,50", "!IN ble.pause=30",
                 "!IN ble.pause=30,AA:BB:CC:DD:EE:FF", "vib=stop", "safe.off", "safe=0", "ui.lat=5", "ui.md5=1",
                 "video.itvl=12,0", "!IN mic.gain=30", "!IN spk.dsp=off", "!IN spk.dsp=700,-30,4,20", "!IN ptt.minrise=15", "!IN ptt.minrise=0", "!IN vol=0", "!IN vol=50", "!IN vol=saved", "link.wake=0", "link.wake=1", "link.sleep=160,320,2",
                 "link.sleep=0", "bench.out=1", "bench.out=0"]
        reads = ["status", "version", "guard?", "ota?", "vib?", "!IN voice?", "!IN wake?", "!IN call?",
                 "!IN vib?", "!IN mic.gain?", "!IN spk.dsp", "!IN spk.dsp?", "!IN ptt.minrise?", "!IN vol?", "link.wake?",
                 "link.sleep?", "bench.out?"]
        out = self.run_lines(["auto? " + w for w in words + reads])
        self.assertEqual(out, ["automation"] * len(words) + ["diagnostic"] * len(reads))
        # Release build, unproven: every state-changing word locked, every read runs.
        out = self.run_lines(["remote 0 0 " + w for w in words + reads])
        self.assertEqual(out, ["locked"] * len(words) + ["runs"] * len(reads))
        # Proven: every word runs, but arming the bench auto-confirm (test_pair_auto_never_armed_over_the_air).
        armed = ("!PAIR AUTO", "!PAIR AUTO 1")
        proven = ["locked" if w in armed else "runs" for w in words + reads]
        self.assertEqual(self.run_lines(["remote 1 0 " + w for w in words + reads]), proven)
        # SAFE, unproven: only the way out of SAFE (and the reads); proven, every word.
        safe = {w: "runs" if w in ("safe.off", "safe=0") else "locked" for w in words}
        self.assertEqual(self.run_lines(["remote 0 1 " + w for w in words]), list(safe.values()))
        self.assertEqual(self.run_lines(["remote 0 1 " + w for w in reads]), ["runs"] * len(reads))
        self.assertEqual(self.run_lines(["remote 1 1 " + w for w in words]),
                         ["locked" if w in armed else "runs" for w in words])
        # The one dispatch point: a release build asks with the connection's proof, before any word runs; an
        # automation build doesn't ask.
        ble = (MG / "mg_ble.c").read_text()
        nus = ble[ble.index("static void nus_command(char *line)\n{"):]
        nus = nus[:nus.index("\n}\n")]
        gate = nus[:nus.index("if (line[0] == '?' || line[0] == '!') {")]
        self.assertIn("#if !CONFIG_MUSE_GADGET_AUTOMATION", gate)
        self.assertIn("bool proven = mg_proto_proof_matched(&s_proto);", gate)
        self.assertIn("if (!mg_unlock_remote_ok(line, proven, s_platform.safe_mode)) {", gate)
        self.assertIn('ESP_LOGW(TAG, "a state-changing word: locked (this connection\'s proof hasn\'t matched)");', gate)
        self.assertNotIn("automation off", ble)
        # The USB console is open in every build: no path of it refuses with the switch off (its talk press too:
        # line_talk_ok). A wireless line's talk press opens the mic on test and automation builds only
        # (MUSE_INJECTED_TALK_OK = muse_input.h's MUSE_NO_PERSON, the one place that names the build options).
        for src in ((ROOT / "components/muse/muse_input.c"), (ROOT / "main/muse_glue.c"),
                    (ROOT / "main/sdk_token_console.c")):
            text = src.read_text()
            self.assertNotIn("automation off", text, src)
            self.assertNotIn("CONFIG_MUSE_GADGET_AUTOMATION", text, src)
        # Nothing else gates them: no unlock, no guard macro.
        for src in (ble, (MG / "mg_unlock.c").read_text(), (ROOT / "main" / "mg_glue.c").read_text()):
            self.assertNotIn("bench_in_allowed", src)
            self.assertNotIn("s_unlock.unlocked", src)
            self.assertNotIn("MUSE_GADGET_UNLOCK", src)


    def test_restart_without_unlock_and_bounds(self) -> None:
        out = self.run_lines(["> !RESTART 500", "> !RESTART 5000", "restart", "restart"])
        self.assertEqual(out, ["RESTART refused: 1000..600000 ms", "RESTART in 5000 ms", "restart 5000 0 0",
                               "restart 0 0 0"])

    def test_restart_schedule_bounds_and_clear(self) -> None:
        out = self.run_lines(["> !RESTART 20000 5", "> !RESTART 40000 8", "> !RESTART 40000 0",
                              "> !RESTART 40000 5", "restart", "> !RESTART 0", "restart", "> !LOCK", "restart"])
        self.assertEqual(out[:3], ["RESTART refused: <30000..600000 ms> <1..7>"] * 3)
        self.assertEqual(out[3:], ["RESTART every 40000 ms, 5 times", "restart 40000 5 0",
                                   "RESTART schedule cleared", "restart 0 0 1",
                                   "UNLOCK not needed (restart schedule cleared)", "restart 0 0 1"])


if __name__ == "__main__":
    unittest.main()
