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

"""Markdown in Muse's replies shows as plain text on the screen
(muse_text_strip_markdown in muse_text.c): markers go, their text stays, and
a lone * or _ that isn't Markdown is left alone."""

from __future__ import annotations

import os
import shlex
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class MarkdownCaptionTest(unittest.TestCase):
    binary: Path

    @classmethod
    def setUpClass(cls) -> None:
        cc = shlex.split(os.environ.get("CC", "cc"))
        if not cc or shutil.which(cc[0]) is None:
            raise unittest.SkipTest("C compiler not available")
        cls.tmp = tempfile.TemporaryDirectory()
        cls.binary = Path(cls.tmp.name) / "muse_serial_chat_harness"
        proc = subprocess.run(
            [
                *cc, "-include", str(ROOT / "tests" / "host_compat.h"),
                "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                "-I", str(ROOT / "components" / "muse"),
                str(ROOT / "tests" / "muse_serial_chat_harness.c"),
                str(ROOT / "components" / "muse" / "muse_chat_text.c"),
                str(ROOT / "components" / "muse" / "muse_text.c"),
                "-o", str(cls.binary),
            ],
            cwd=ROOT, text=True, capture_output=True,
        )
        if proc.returncode:
            raise AssertionError(proc.stdout + proc.stderr)

    @classmethod
    def tearDownClass(cls) -> None:
        cls.tmp.cleanup()

    def run_harness(self, mode: str, text: str, *args: str) -> str:
        proc = subprocess.run(
            [str(self.binary), mode, *args], input=text.encode(), capture_output=True,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr.decode())
        return proc.stdout.decode()

    def plain(self, text: str) -> str:
        return self.run_harness("markdown", text)

    def test_emphasis_keeps_its_text(self) -> None:
        for text, shown in (
            ("**Great question!** Here it is.", "Great question! Here it is."),
            ("This is *really* simple.", "This is really simple."),
            ("This is _really_ simple.", "This is really simple."),
            ("__Note:__ wear a coat", "Note: wear a coat"),
            ("*Note:* wear a coat", "Note: wear a coat"),
            ("It's ~~cold~~ warm.", "It's cold warm."),
            ("Run `ls -la` to see them.", "Run ls -la to see them."),
            ("***Very*** important", "Very important"),
        ):
            self.assertEqual(self.plain(text), shown, text)

    def test_text_that_only_looks_like_markdown_stays(self) -> None:
        for text in (
            "2 * 3 = 6",
            "2*3*4 = 24",
            "snake_case_name and file_name.txt",
            "Read the #1 hit, #hashtag",
            "Rain - then sun - by noon",
            "5 - 3 = 2",
            "a|b",
            "price is $5 [approx]",
            "1. First\n2. Second",
            "广州今天雷阵雨，30°/24°。",
            "",
        ):
            self.assertEqual(self.plain(text), text.replace("a|b", "a,b"), text)

    def test_headings_quotes_and_bullets(self) -> None:
        self.assertEqual(
            self.plain("# Weather today\n## Morning\n> Bring an umbrella\n* Rain\n+ Wind\n- Cold\n  * Nested"),
            "Weather today\nMorning\nBring an umbrella\n- Rain\n- Wind\n- Cold\n  - Nested",
        )
        self.assertEqual(self.plain("####### Seven"), "####### Seven")

    def test_links_keep_their_text(self) -> None:
        self.assertEqual(
            self.plain("See [the forecast](https://example.com/a_b*c) for more."),
            "See the forecast for more.",
        )
        self.assertEqual(self.plain("Broken [link](no end"), "Broken [link](no end")

    def test_tables_read_as_rows(self) -> None:
        self.assertEqual(
            self.plain("| Day | High |\n|-----|:----:|\n| Mon | 72 |\n| Tue | 68 |"),
            "Day, High\nMon, 72\nTue, 68",
        )

    def test_rules_and_fences_go(self) -> None:
        self.assertEqual(
            self.plain("Before\n---\n```python\nprint(1)\n```\nAfter"),
            "Before\nprint(1)\nAfter",
        )

    def test_escaped_markers_are_kept_as_characters(self) -> None:
        self.assertEqual(self.plain(r"5 \* 3 and \_x\_"), "5 * 3 and _x_")

    def test_never_grows(self) -> None:
        samples = ["|" * 50, "a|b|c|d", "| | |", "* * *", "[x](y)" * 20, "**" * 30, "\\" * 9,
                   "`" * 7, "#" * 8 + " x", "_a_ *b* __c__ **d**", "> > nested\n>\n#\n*"]
        for text in samples:
            shown = self.plain(text)
            self.assertLessEqual(len(shown.encode()), len(text.encode()), text)

    def test_captions_page_without_markers(self) -> None:
        reply = "**Good morning!** Here's the plan:\n* Coffee at *8*\n* Walk at 9"
        self.assertEqual(
            self.run_harness("caption", reply, "40"),
            "Good morning! Here's the plan:\n- Coffee at 8\n- Walk at 9",
        )

    def test_wrapped_lines_keep_what_they_start_with(self) -> None:
        # A "+", "*" or ">" that a line wraps before is text, not a marker.
        for reply, shown in (
            ("The answer is 7 + 5 = 12", "The answer is 7\n+ 5 = 12"),
            ("The answer is 6 * 7 = 42", "The answer is 6\n* 7 = 42"),
            ("Keep it on if x > 3 or so", "Keep it on if x\n> 3 or so"),
            ("* Coffee at eight then a walk", "- Coffee at\neight then a\nwalk"),
        ):
            self.assertEqual(self.run_harness("caption", reply, "15"), shown, reply)


if __name__ == "__main__":
    unittest.main()
