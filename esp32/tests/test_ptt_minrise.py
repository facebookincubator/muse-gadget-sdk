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

"""The bench guard's console word, ptt.minrise (after
five push-to-talk turns the gadget's mic didn't hear were sent and came back
as "Help."): its grammar and range (mg_minrise.h). What the held turn sends
(and doesn't) is test_mg_ble's mg_proto_harness test_minrise /
test_minrise_reply; the UI after a dropped turn is test_muse_ptt_ui's
test_bench_guard_held_turn_ui."""

import tempfile
import unittest
from pathlib import Path

from test_mg_ble import MG, compile_and_run


class PttMinrise(unittest.TestCase):
    def test_word_grammar_and_range(self) -> None:
        cases = {
            "ptt.minrise?": "show",
            "ptt.minrise=15": "set 15", "ptt.minrise=1": "set 1", "ptt.minrise=40": "set 40",
            "ptt.minrise=0": "set 0", "ptt.minrise=00": "set 0", "ptt.minrise=040": "set 40",
            "ptt.minrise=41": "bad", "ptt.minrise=100": "bad", "ptt.minrise=-1": "bad", "ptt.minrise=": "bad",
            "ptt.minrise=1.5": "bad", "ptt.minrise=15 ": "bad", "ptt.minrise= 15": "bad", "ptt.minrise=0040": "bad",
            "ptt.minrise": "bad", "ptt.minrise??": "bad", "ptt.minrisex=1": "bad",
            "ptt=1": "none", "ptt.min=1": "none", "mic.gain=30": "none", "": "none",
        }
        lines = "\n".join(f'    run("{k}");' for k in cases)
        src = r"""
#include <stdio.h>
#include "mg_minrise.h"
static void run(const char *line) {
    int db = -1;
    switch (mg_minrise_word(line, &db)) {
    case MG_MINRISE_WORD_NONE: puts("none"); break;
    case MG_MINRISE_WORD_BAD: puts("bad"); break;
    case MG_MINRISE_WORD_SHOW: puts("show"); break;
    case MG_MINRISE_WORD_SET: printf("set %d\n", db); break;
    }
}
int main(void) {
@LINES@
    char b[32];
    mg_minrise_text(b, sizeof(b), 15); puts(b);
    mg_minrise_text(b, sizeof(b), 0); puts(b);
    puts(MG_MINRISE_USAGE);
    return 0;
}
""".replace("@LINES@", lines)
        with tempfile.TemporaryDirectory(prefix="mg-minrise-") as tmp:
            c = Path(tmp) / "w.c"
            c.write_text(src)
            out = compile_and_run(self, [c], [], [MG / "include"]).splitlines()
        self.assertEqual(out[:len(cases)], list(cases.values()))
        self.assertEqual(out[len(cases):], [
            "ptt.minrise 15", "ptt.minrise off",
            "ptt.minrise=N (1-40 dB), ptt.minrise=0 (off) or ptt.minrise?"])



class ATurnNotKeptSendsNothingMore(unittest.TestCase):
    def test_mute_drops_before_any_flush(self) -> None:
        # A turn not kept (mute) drops before any flush: the encoder's residue, a held turn, the guard's turn; a live
        # turn ends with its stop_mic and nothing after it.
        src = (MG / "mg_voice.c").read_text()
        end = src[src.index("void mg_voice_end(bool keep)\n{"):]
        end = end[:end.index("\n}\n")]
        nk = end[end.index("    if (!keep) {"):]
        nk = nk[:nk.index("\n        return;\n    }\n")]
        self.assertLess(nk.index("(void)mg_encoder_flush(s_enc, s_out, sizeof(s_out));   /* the residue, discarded */"),
                        nk.index("mg_proto_turn_drop(p, false)"))
        self.assertIn("(void)mg_proto_held_turn_take(p, NULL, 0);", nk)
        self.assertIn("if (!unsent) {\n            mg_proto_live_end(p, true);", nk)
        self.assertNotIn("queue_out(", nk)
        self.assertLess(end.index("    if (!keep) {"), end.index("size_t out = mg_encoder_flush(s_enc, s_out, sizeof(s_out));"))
        self.assertLess(end.index("if (route == MG_ROUTE_QUEUE) {"), end.index("    if (!keep) {"))   # the clip queue: as before

if __name__ == "__main__":
    unittest.main()
