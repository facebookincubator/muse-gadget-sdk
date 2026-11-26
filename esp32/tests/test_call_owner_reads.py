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

"""The build check for Live call and subtitles: the list of every wireless read that prints the owner's call or
subtitle content (call_owner_reads.txt) matches the source, none prints reply or caption text, and the one wireless
way out (the Nordic UART log mirror) is gated on release builds."""

import re
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import call_owner_reads as reads  # noqa: E402

ROOT = reads.ROOT


class OwnerReads(unittest.TestCase):
    def listed(self) -> list[str]:
        return sorted(ln for ln in reads.LIST.read_text().splitlines() if ln and not ln.startswith("#"))

    def test_the_list_is_every_mirrored_call_and_subtitle_line(self) -> None:
        found, listed = reads.scan(), self.listed()
        self.assertEqual([x for x in found if x not in listed], [], "mirrored call/subtitle lines not in the list")
        self.assertEqual([x for x in listed if x not in found], [], "listed lines no longer in the source")

    def test_no_reply_or_caption_text_reaches_the_log(self) -> None:
        # The words themselves never go to a log line (captions only, never the log): no line formats a word's bytes
        # or the model's pool.
        subs = (ROOT / "components/muse/muse_subs.c").read_text()
        for m in re.finditer(r"ESP_LOG[IWED]\((.*?)\);", subs, re.S):
            self.assertNotRegex(m.group(1), r"utf8|\.pool|%\.\*s", m.group(1)[:120])
        for ln in self.listed():
            self.assertNotIn("%.*s", ln)
        # No Control or GATT read returns call metadata or subtitle text: the subtitle command answers only its
        # capabilities (status), live_call answers nothing (the device's own requests are [live_call, start|end]).
        proto = (ROOT / "components/muse_gadget_ble/mg_proto.c").read_text()
        sub = proto[proto.index("static void cmd_subtitle("):]
        sub = sub[:sub.index("\n}\n")]
        self.assertEqual(re.findall(r"send_subtitle_caps\(p\)", sub), ["send_subtitle_caps(p)"])
        self.assertNotRegex(sub, r"notify\(|send_reply\(")
        call = proto[proto.index("static void cmd_live_call("):]
        call = call[:call.index("\n}\n")]
        self.assertNotRegex(call, r"notify\(|send_reply\(")

    def test_the_mirror_is_gated_on_release_builds(self) -> None:
        # The NUS log mirror stays silent on a set-up release build until this connection's token proof matched (or
        # in the local window); only the open status words answer an unproven central, from their handler's task.
        ble = (ROOT / "components/muse_gadget_ble/mg_ble.c").read_text()

        def body(head: str) -> str:
            i = ble.index(head)
            return ble[i:ble.index("\n}\n", i)]

        # The mirror's gate (s_nus_owner, refreshed under the lock): nothing but a status word's own reply is queued
        # before the proof, and a new link starts with an empty ring.
        self.assertIn("if (s_nus_sub && s_nus_ring && (s_nus_owner || (reply && reply == xTaskGetCurrentTaskHandle()))) {",
                      body("static int nus_vprintf("))
        self.assertIn("nus_ring_reset();   /* nothing queued for an earlier link goes to this one */", ble)


if __name__ == "__main__":
    unittest.main()
