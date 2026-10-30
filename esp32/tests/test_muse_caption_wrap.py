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
closing punctuation at the start of a line. With a proportional caption font,
lines also wrap to its width in pixels, so every page fits and none is skipped. Letters the ASCII fonts lack show
as their plain ones (muse_text.c)."""

from __future__ import annotations

import os
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

    def page(self, text: str) -> str:
        proc = subprocess.run(
            [str(self.binary), "page"], input=text.encode(), capture_output=True, check=True
        )
        return proc.stdout.decode().strip()

    def test_hangul_takes_the_cjk_font_and_page(self) -> None:
        for text in (
            "\u1100",                                       # a lone leading jamo, as the font draws it
            unicodedata.normalize("NFD", "안녕하세요"),   # conjoining jamo, drawn uncomposed
            "안녕하세요",                                  # syllables
            "ㅎㅎ",                                        # compatibility jamo
            "Muse 안녕",
        ):
            self.assertEqual(self.page(text), "cjk cjk", repr(text))
        self.assertEqual(self.page("hello there"), "latin latin")

    def test_hangul_fillers_are_dropped(self) -> None:
        # Blank characters that the CJK font draws as boxes.
        self.assertEqual(self.shown("\u115f\u1160\u3164한\uffa0글"), "한글")

    def shown(self, text: str) -> str:
        proc = subprocess.run(
            [str(self.binary), "ascii"], input=text.encode(), capture_output=True, check=True
        )
        return proc.stdout.decode()

    def pages(self, text: str, cols: int, px: int, lines: int) -> list[list[str]]:
        proc = subprocess.run(
            [str(self.binary), "pages", str(cols), str(px), str(lines)],
            input=text.encode(),
            capture_output=True,
            check=True,
        )
        return [page.split("\n") for page in proc.stdout.decode().split("\f")]

    @staticmethod
    def px(line: str) -> int:
        """The harness's proportional font (fake_glyph_px), as LVGL adds it up."""
        total = 0
        for i, c in enumerate(line):
            w = 18 if c in "WM" else 4 if c in "il ." else 11 if ord(c) >= 0x80 else 10
            total += w - (2 if c == "A" and line[i + 1 : i + 2] == "V" else 0)
        return total

    def assert_pages_show_everything(self, text: str, cols: int, px: int, lines: int) -> list[list[str]]:
        pages = self.pages(text, cols, px, lines)
        shown = []
        for page in pages:
            self.assertLessEqual(len(page), lines, page)
            for line in page:
                self.assertLessEqual(self.px(self.shown(line)), px, f"{line!r} wraps again on screen")
                self.assertLessEqual(len(line), cols, line)
            shown.extend(" ".join(page).split())
        # Pages overlap by a line, so words repeat; none may be missing.
        words = iter(shown)
        for word in text.split():
            self.assertIn(word, words, f"{word!r} is never shown")
        return pages

    def test_wide_letters_wrap_to_the_label_in_pixels(self) -> None:
        # The LCD-3.49's long caption: 148 px wide, 12 lines, 21 characters at most.
        text = " ".join(["WWWWWWW MMMMMM WWW"] * 12 + ["the end"])
        pages = self.assert_pages_show_everything(text, 21, 148, 12)
        self.assertGreater(len(pages), 2)
        self.assertEqual(pages[0][0], "WWWWWWW")   # 126 px; a space and three Ms would be 184

    def test_ordinary_and_narrow_text_fills_lines(self) -> None:
        text = ("Tomorrow will be mild and dry, with a light breeze from the west in the "
                "afternoon and clear skies until late evening. ") * 4
        pages = self.assert_pages_show_everything(text, 21, 148, 12)
        self.assertEqual(pages[0][0], "Tomorrow will be")
        narrow = " ".join(["ill"] * 60)
        for page in self.assert_pages_show_everything(narrow, 21, 148, 12):
            self.assertTrue(all(len(line) <= 21 for line in page))
        self.assertEqual(self.pages(narrow, 21, 148, 12)[0][0], "ill ill ill ill ill")

    def test_kerning_counts_towards_the_width(self) -> None:
        # "AVAVAVAVAVAVAVA": each A before a V is 8 px, so 15 letters make 136.
        self.assertEqual(self.pages("AVAVAVAVAVAVAVA", 21, 136, 12), [["AVAVAVAVAVAVAVA"]])
        self.assertEqual(self.pages("AVAVAVAVAVAVAVA", 21, 135, 12), [["AVAVAVAVAVAVAV", "A"]])

    def test_stand_ins_are_measured_as_shown(self) -> None:
        # An em dash shows as "--", two 10 px glyphs: "a --" is 34 px, and
        # "-- b" 34 too. Measured as one 11 px glyph, "a \u2014" would fit in 33.
        self.assertEqual(self.pages("a \u2014 b", 21, 34, 12), [["a \u2014", "b"]])
        self.assertEqual(self.pages("a \u2014 b", 21, 33, 12), [["a", "\u2014", "b"]])

    def assert_lines_cover(self, text: str, pages: list[list[str]], cap: int = 400) -> None:
        """Every page fits the buffer, and its lines, less each page's overlap, are the text."""
        shown: list[str] = []
        for page in pages:
            self.assertLess(len("\n".join(page).encode()), cap, page)
            shown.extend(page[1:] if shown and page[0] == shown[-1] else page)
        self.assertEqual("".join(shown).replace(" ", ""), "".join(text.split()))

    def test_multibyte_lines_that_overflow_the_buffer_turn_the_page(self) -> None:
        # Twelve lines of 21 narrow letters, three bytes each: 64 bytes a line,
        # so only six fit the 400-byte page. The rest start the next one.
        text = "\n".join(["\u1ecb" * 21] * 6 + ["\u1ec9" * 21] * 6)
        pages = self.pages(text, 21, 148, 12)
        self.assertEqual([len(page) for page in pages], [6, 6, 2])
        self.assert_lines_cover(text, pages)
        # Without pixels, CJK lines of 24 three-byte characters overflow the same way.
        cjk = "还伴有八级左右的雷雨大风和短时强降水出门记得带伞注意安全" * 9
        self.assert_lines_cover(cjk, self.pages(cjk, 24, 0, 10))

    def test_pages_that_fit_turn_every_lines_minus_one(self) -> None:
        text = " ".join(f"w{i:02d}" for i in range(20))
        pages = self.pages(text, 3, 0, 4)
        self.assertEqual([page[0] for page in pages], ["w00", "w03", "w06", "w09", "w12", "w15", "w18"])
        self.assert_lines_cover(text, pages)

    def test_cjk_keeps_its_fixed_width_page(self) -> None:
        text = "还伴有八级左右的雷雨大风和短时强降水出门记得带伞注意安全"
        self.assertEqual([len(line) for line in self.pages(text, 13, 20, 12)[0]], [13, 13, 2])

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


if __name__ == "__main__":
    unittest.main()
