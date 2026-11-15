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

"""Host test for the WG1200 swipe-down card prompt template
(components/muse/wg1200_card_prompt.h): placeholder filling and the chunked
card.prompt= / card.prompt+= rules shared by USB and BLE.
"""

from __future__ import annotations

import os
import shlex
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

HARNESS = r"""
#include <stdio.h>
#include <string.h>
#include "wg1200_card_prompt.h"

/* expand TEMPLATE [CAP]: prints the filled template.
 * apply  CMD...      : runs "set:TEXT" / "add:TEXT" in order, printing ok/no and the result. */
int main(int argc, char **argv) {
    if (argc >= 3 && !strcmp(argv[1], "expand")) {
        char out[4096];
        size_t cap = argc > 3 ? (size_t)atoi(argv[3]) : sizeof(out);
        size_t n = wg1200_card_prompt_expand(argv[2], "16.1C, 88% RH.", "37.9807N, 23.7108E (station GPS)",
                                             "Cyberpunk Neon", out, cap);
        printf("%zu|%s\n", n, out);
        return 0;
    }
    if (argc >= 2 && !strcmp(argv[1], "default")) {
        printf("%s\n", WG1200_CARD_PROMPT_DEFAULT);
        return 0;
    }
    char cur[WG1200_CARD_PROMPT_MAX + 1] = "";
    for (int i = 2; i < argc; i++) {
        bool append = !strncmp(argv[i], "add:", 4);
        bool ok = wg1200_card_prompt_apply(cur, sizeof(cur), append, argv[i] + 4);
        printf("%s %zu\n", ok ? "ok" : "no", strlen(cur));
    }
    printf("%s\n", cur);
    return 0;
}
"""


def _cc_command() -> list[str]:
    cmd = shlex.split(os.environ.get("CC", "cc"))
    if not cmd or shutil.which(cmd[0]) is None:
        raise RuntimeError("C compiler not available; set CC")
    return cmd


class Wg1200CardPromptTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.tmp = tempfile.TemporaryDirectory()
        src = Path(cls.tmp.name) / "prompt_harness.c"
        src.write_text("#include <stdlib.h>\n" + HARNESS)
        cls.binary = Path(cls.tmp.name) / "prompt_harness"
        subprocess.run([
            *_cc_command(), "-std=c11", "-Wall", "-Wextra", "-Werror",
            "-I", str(ROOT / "components/muse"), str(src), "-o", str(cls.binary),
        ], check=True)

    @classmethod
    def tearDownClass(cls) -> None:
        cls.tmp.cleanup()

    def run_harness(self, *args: str) -> list[str]:
        out = subprocess.run([str(self.binary), *args], check=True, capture_output=True, text=True).stdout
        return out.split("\n")[:-1]

    def test_fills_every_placeholder_and_leaves_other_braces(self) -> None:
        [line] = self.run_harness("expand", "W: {weather} at {location}, {style}; {style} again; {other} {weather")
        n, text = line.split("|", 1)
        self.assertEqual(
            text,
            "W: 16.1C, 88% RH. at 37.9807N, 23.7108E (station GPS), Cyberpunk Neon; "
            "Cyberpunk Neon again; {other} {weather",
        )
        self.assertEqual(int(n), len(text))

    def test_truncates_to_the_buffer(self) -> None:
        [line] = self.run_harness("expand", "{location}{location}", "20")
        n, text = line.split("|", 1)
        self.assertEqual((int(n), text), (19, "37.9807N, 23.7108E "))

    def test_default_has_all_placeholders(self) -> None:
        [text] = self.run_harness("default")
        for name in ("{weather}", "{location}", "{style}"):
            self.assertEqual(text.count(name), 1, name)
        self.assertTrue(text.startswith("Weather: {weather} Station location: {location}."))
        self.assertLessEqual(len(text), 1023)

    def test_set_append_and_reset(self) -> None:
        self.assertEqual(
            self.run_harness("apply", "set:Hello {weather}", "add: and {style}", "set:"),
            ["ok 15", "ok 27", "ok 0", ""],
        )

    def test_rejects_append_without_set_and_overlong(self) -> None:
        lines = self.run_harness("apply", "add:orphan", "set:" + "x" * 1000, "add:" + "y" * 24)
        self.assertEqual(lines[:3], ["no 0", "ok 1000", "no 1000"])
        self.assertEqual(lines[3], "x" * 1000)  # a rejected chunk leaves the template as it was


if __name__ == "__main__":
    unittest.main()
