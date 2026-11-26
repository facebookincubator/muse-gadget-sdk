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

"""Every POST /chat/stream body the firmware sends (as the VM reads them): a
top-level node_id (the one link.register sends), never a chat_context (the VM
answers 400 "conflicting chat context fields"), no session_id or thread_id, and a non-empty message or
items. Each body is built here from the code that builds it on the gadget."""

from __future__ import annotations

import json
import os
import shlex
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MAIN = ROOT / "main"
MUSE = ROOT / "components/muse"
FAKES = ROOT / "tests/link_fakes"

NODE = "homelink-0a1b2c"


HARNESS = r"""
#include <stdio.h>
#include <string.h>

#include "muse_chat_priv.h"
#include "session_image_core.h"

int main(void)
{
    static char body[4096];
    size_t n = simg_chat_body(body, sizeof(body), "MuseGadget-SCW1-5AFC3C", "@NODE@");
    printf("image %s\n", n ? body : "");
    n = muse_note_head(body, sizeof(body), "@NODE@");
    if (n) {
        /* a short base64 WAV in place of a recording */
        strcpy(body + n, "UklGRg==" MUSE_HATCH_NOTE_TAIL);
    }
    printf("note %s\n", n ? body : "");
    /* a node id's own characters only: nothing a JSON string would need escaped */
    n = muse_note_head(body, sizeof(body), "bad\"node\\\n");
    if (n) {
        strcpy(body + n, "UklGRg==" MUSE_HATCH_NOTE_TAIL);
    }
    printf("note_odd %s\n", n ? body : "");
    printf("note_small %zu\n", muse_note_head(body, 40, "@NODE@"));
    return 0;
}
""".replace("@NODE@", NODE)


class ChatStreamBodies(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        with tempfile.TemporaryDirectory(prefix="chatbody-") as d:
            h = Path(d) / "h.c"
            h.write_text(HARNESS)
            exe = Path(d) / "a.out"
            cc = shlex.split(os.environ.get("CC", "cc"))
            cmd = [*cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined", "-g",
                   f"-I{FAKES}", f"-I{MAIN}", f"-I{MUSE}", str(h),
                   str(MAIN / "session_image_core.c"), str(FAKES / "cJSON.c"), "-lm", "-o", str(exe)]
            r = subprocess.run(cmd, text=True, capture_output=True)
            if r.returncode:
                raise AssertionError(r.stdout + r.stderr)
            r = subprocess.run([str(exe)], text=True, capture_output=True)
            if r.returncode:
                raise AssertionError(r.stdout + r.stderr)
        cls.out = {}
        for line in r.stdout.splitlines():
            key, _, rest = line.partition(" ")
            cls.out[key] = rest

    def check_body(self, raw: str, node: str = NODE) -> dict:
        body = json.loads(raw)
        self.assertEqual(body.get("node_id"), node, raw[:120])
        for banned in ("chat_context", "session_id", "thread_id", "device_id"):
            self.assertNotIn(banned, body, raw[:120])
        message = body.get("message")
        items = body.get("items")
        self.assertTrue((isinstance(message, str) and message) or (isinstance(items, list) and items), raw[:120])
        return body

    def test_ask_muse_for_an_image(self) -> None:
        body = self.check_body(self.out["image"])
        self.assertIn("MuseGadget-SCW1-5AFC3C", body["message"])

    def test_voice_note(self) -> None:
        body = self.check_body(self.out["note"])
        self.assertEqual(body["message"], "")
        self.assertEqual(body["items"][0]["mime_type"], "audio/wav")
        self.assertEqual(body["items"][0]["data_base64"], "UklGRg==")
        # a node id with JSON's specials still makes a parsable body (they're left out)
        self.check_body(self.out["note_odd"], node="badnode")
        self.assertEqual(self.out["note_small"], "0")


if __name__ == "__main__":
    unittest.main()
