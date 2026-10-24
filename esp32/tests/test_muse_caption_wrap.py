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

"""How reply captions wrap to the screen's page (muse_chat_text.c): words stay
whole, and CJK, which has no spaces, breaks between characters but never puts
closing punctuation at the start of a line. Letters the ASCII fonts lack show
as their plain ones (muse_text.c), or with CONFIG_MUSE_LATIN_FONT, accented
ones as they are."""

from __future__ import annotations

import os
import re
import shlex
import shutil
import subprocess
import tempfile
import unicodedata
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
CLOSING = "、。，．：；！？）」』】》"


class CaptionWrapTest(unittest.TestCase):
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
                *cc,
                "-include",
                str(ROOT / "tests" / "host_compat.h"),
                "-std=gnu11",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-I",
                str(ROOT / "components" / "muse"),
                str(ROOT / "tests" / "muse_serial_chat_harness.c"),
                str(ROOT / "components" / "muse" / "muse_chat_text.c"),
                str(ROOT / "components" / "muse" / "muse_text.c"),
                "-o",
                str(cls.binary),
            ],
            cwd=ROOT,
            text=True,
            capture_output=True,
        )
        if proc.returncode:
            raise AssertionError(proc.stdout + proc.stderr)

    @classmethod
    def tearDownClass(cls) -> None:
        cls.tmp.cleanup()

    def wrap(self, text: str, cols: int) -> list[str]:
        proc = subprocess.run(
            [str(self.binary), "caption", str(cols)],
            input=text.encode(),
            capture_output=True,
            check=True,
        )
        return proc.stdout.decode().split("\n")

    def test_words_stay_whole(self) -> None:
        self.assertEqual(self.wrap("the quick brown fox jumps", 10), ["the quick", "brown fox", "jumps"])

    def test_cjk_fills_each_line(self) -> None:
        text = "还伴有八级左右的雷雨大风和短时强降水出门记得带伞注意安全"
        lines = self.wrap(text, 13)
        self.assertEqual("".join(lines), text)
        self.assertEqual([len(line) for line in lines], [13, 13, 2])

    def test_ascii_runs_inside_cjk_stay_whole(self) -> None:
        text = "广州今天雷阵雨，30°/24°，还伴有8级左右的雷雨大风和短时强降水，出门小心。"
        lines = self.wrap(text, 13)
        self.assertEqual("".join(lines), text)
        self.assertTrue(any(line.startswith("30°/24°") for line in lines), lines)
        for line in lines:
            self.assertLessEqual(len(line), 13, line)

    def test_closing_punctuation_never_starts_a_line(self) -> None:
        self.assertEqual(self.wrap("一二三四五，六七", 5), ["一二三四", "五，六七"])
        for cols in range(4, 16):
            for line in self.wrap("好消息在后头：冷空气正在南下，明天雨就收了。气温一路往下走。", cols)[1:]:
                self.assertNotIn(line[:1], CLOSING, f"{cols} columns: {line!r}")

    def test_closing_punctuation_after_a_full_line_of_ascii(self) -> None:
        self.assertEqual(self.wrap("好abcdefghijklm，后", 13), ["好", "abcdefghijkl", "m，后"])
        text = "广州今天雷阵雨，30°/24°，还伴有8级左右的雷雨大风和短时强降水，出门小心。"
        for cols in range(4, 16):
            lines = self.wrap(text, cols)
            self.assertEqual("".join(lines), text)
            for line in lines[1:]:
                self.assertNotIn(line[:1], CLOSING, f"{cols} columns: {line!r}")

    def test_mixed_text_breaks_at_spaces_or_between_cjk(self) -> None:
        self.assertEqual(self.wrap("Muse 说 hello world", 8), ["Muse 说", "hello", "world"])

    def shown(self, text: str) -> str:
        proc = subprocess.run(
            [str(self.binary), "ascii"], input=text.encode(), capture_output=True, check=True
        )
        return proc.stdout.decode()

    def test_vietnamese_shows_its_plain_letters(self) -> None:
        self.assertEqual(
            self.shown("Thời tiết Hà Nội hôm nay đẹp, được không?"),
            "Thoi tiet Ha Noi hom nay dep, duoc khong?",
        )
        # Every Vietnamese letter, upper and lower case, decomposed or not.
        letters = "".join(map(chr, range(0x1EA0, 0x1EFA))) + "ƠơƯưĐđ"
        plain = "".join(unicodedata.normalize("NFD", c)[0] for c in letters).replace("Đ", "D").replace("đ", "d")
        self.assertEqual(self.shown(letters), plain)
        self.assertEqual(self.shown(unicodedata.normalize("NFD", letters)), plain)

    def test_the_latin_font_keeps_accented_letters(self) -> None:
        def latin(text: str) -> str:
            proc = subprocess.run(
                [str(self.binary), "latin"], input=text.encode(), capture_output=True, check=True
            )
            return proc.stdout.decode()

        self.assertEqual(latin("\u201cThời tiết\u201d \u2014 Hà Nội đẹp"), '1:"Thời tiết" -- Hà Nội đẹp')
        self.assertEqual(latin("plain \u2026"), "0:plain ...")
        # Every letter a caption keeps, and the plain caption doesn't, is in the font.
        every = "".join(map(chr, range(0xC0, 0x1EFA)))
        kept = {ord(c) for c in latin(every)} - {ord(c) for c in self.shown(every)}
        font = (ROOT / "components" / "muse" / "fonts" / "muse_font_latin_16.c").read_text(encoding="utf-8")
        has = {int(cp, 16) for cp in re.findall(r"/\* U\+([0-9A-F]+) ", font)}
        self.assertLessEqual(set(range(0x1EA0, 0x1EFA)) | {0x1A0, 0x1A1, 0x1AF, 0x1B0, 0x110, 0x111}, kept)
        self.assertEqual(sorted(kept - has), [])


if __name__ == "__main__":
    unittest.main()
