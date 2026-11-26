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

"""Emoji in captions (CONFIG_MUSE_EMOJI): muse_text.c keeps the ones the emoji
font has and drops the rest as before, sequences show as their first emoji,
and reply pages wrap them as 16 px glyphs (muse_chat_text.c). Also checks the
generated font against its list, and the option's wiring."""

from __future__ import annotations

import os
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))   # its helpers, also when run from esp32/ as tests.<module>
from host_child import run_child  # noqa: E402 (macOS's pipe deadlock on a driver's input: host_child.py)


ROOT = Path(__file__).resolve().parents[1]
MUSE = ROOT / "components" / "muse"
FONTS = MUSE / "fonts"


def code_points() -> list[int]:
    header = (FONTS / "muse_emoji.h").read_text(encoding="utf-8")
    body = header[header.index("#define MUSE_EMOJI_CODE_POINTS"):]
    return [int(cp, 16) for cp in re.findall(r"0x([0-9A-F]+)", body)]


class EmojiTextTest(unittest.TestCase):
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
                "-DCONFIG_MUSE_EMOJI=1",
                "-I",
                str(MUSE),
                str(ROOT / "tests" / "muse_serial_chat_harness.c"),
                str(MUSE / "muse_chat_text.c"),
                str(MUSE / "muse_text.c"),
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

    def run_harness(self, mode: str, text: str, *args: str) -> str:
        proc = run_child(
            [str(self.binary), mode, *args], input=text.encode(), capture_output=True, check=True
        )
        return proc.stdout.decode()

    def caption(self, text: str) -> str:
        return self.run_harness("emoji", text)

    def test_the_fonts_emoji_stay(self) -> None:
        self.assertEqual(self.caption("Done ✅ Have a great day! \U0001F60A\U0001F389"),
                         "Done ✅ Have a great day! \U0001F60A\U0001F389")
        every = "".join(map(chr, code_points()))
        self.assertEqual(self.caption(every), every)

    def test_other_emoji_still_go(self) -> None:
        # A unicorn, a flag and a keycap: not in the font, so no boxes.
        self.assertEqual(self.caption("Hi \U0001F984 \U0001F1E7\U0001F1F7 1️⃣!"), "Hi   1!")

    def test_stand_ins_and_letters_are_as_before(self) -> None:
        text = "“Café” — ✔ © …"
        self.assertEqual(self.caption(text), self.run_harness("ascii", text))
        self.assertEqual(self.caption(text), '"Cafe" -- v (c) ...')

    def test_without_the_font_emoji_go(self) -> None:
        # muse_text_to_ascii() never keeps them, as on a small screen, where
        # muse_text_keep_emoji() is never called.
        self.assertEqual(self.run_harness("ascii", "Yes \U0001F44D❤️"), "Yes ")

    def test_sequences_show_their_first_emoji(self) -> None:
        cases = {
            "\U0001F44D\U0001F3FD": "\U0001F44D",                    # skin tone
            "❤️": "❤",                                 # emoji style
            "❤️‍\U0001F525": "❤",                 # heart on fire
            "\U0001F937\U0001F3FB‍♀️ ok": "\U0001F937 ok",
            # Not in the font, and nor is its sequence: the laptop goes too.
            "\U0001F468‍\U0001F4BB coder": " coder",
            "\U0001F44D‍": "\U0001F44D",                        # a stray joiner
        }
        for text, shown in cases.items():
            self.assertEqual(self.caption(text), shown, repr(text))

    def pages(self, text: str, cols: int, px: int, lines: int) -> list[list[str]]:
        out = self.run_harness("emoji-pages", text, str(cols), str(px), str(lines))
        return [page.split("\n") for page in out.split("\f")]

    @staticmethod
    def px(line: str) -> int:
        """The harness's fake font (fake_glyph_px): 16 px emoji."""
        total = 0
        for c in line:
            cp = ord(c)
            total += (18 if c in "WM" else 4 if c in "il ." else
                      16 if 0x2300 <= cp <= 0x2BFF or cp >= 0x1F000 else 11 if cp >= 0x80 else 10)
        return total

    def test_pages_wrap_emoji_by_their_width(self) -> None:
        text = " ".join(f"Great{i} \U0001F389\U0001F389\U0001F389 job \U0001F44D" for i in range(8))
        pages = self.pages(text, 21, 100, 3)
        shown = []
        for page in pages:
            self.assertLessEqual(len(page), 3, page)
            for line in page:
                self.assertLessEqual(self.px(line), 100, f"{line!r} wraps again on screen")
            shown.extend(" ".join(page).split())
        words = iter(shown)
        for word in text.split():
            self.assertIn(word, words, f"{word!r} is never shown")
        self.assertGreater(len(pages), 4)
        self.assertEqual(pages[0][0], "Great0")   # "Great0 " and three emoji are 112 px

    def test_emoji_count_two_columns(self) -> None:
        # Without pixels, ten columns hold five emoji.
        self.assertEqual(self.pages("\U0001F600" * 7, 10, 0, 4), [["\U0001F600" * 5, "\U0001F600" * 2]])


class EmojiFontTest(unittest.TestCase):
    def test_the_font_draws_every_listed_emoji(self) -> None:
        codes = code_points()
        self.assertEqual(codes, sorted(set(codes)), "the list must be sorted for the binary searches")
        self.assertTrue(100 <= len(codes) <= 200, len(codes))
        header = (FONTS / "muse_emoji.h").read_text(encoding="utf-8")
        self.assertIn(f"#define MUSE_EMOJI_COUNT {len(codes)}\n", header)
        font = (FONTS / "muse_font_emoji_16.c").read_text(encoding="utf-8")
        drawn = [int(cp, 16) for cp in re.findall(r"/\* U\+([0-9A-F]+) ", font)]
        self.assertEqual(drawn, codes)
        for cp in codes:
            self.assertFalse(0x1F3FB <= cp <= 0x1F3FF or cp in (0x200D, 0xFE0F), hex(cp))

    def test_attribution(self) -> None:
        font = (FONTS / "muse_font_emoji_16.c").read_text(encoding="utf-8")
        self.assertIn("CC-BY 4.0", font)
        self.assertTrue((FONTS / "LICENSE-twemoji.txt").read_text(encoding="utf-8").startswith(
            "Attribution 4.0 International"))
        for readme in (ROOT.parent / "README.md", ROOT / "README.md", FONTS / "README.md"):
            self.assertIn("Twemoji", readme.read_text(encoding="utf-8"), readme)

    def test_option_is_off_by_default_and_wired(self) -> None:
        kconfig = (MUSE / "Kconfig").read_text(encoding="utf-8")
        entry = kconfig[kconfig.index("    config MUSE_EMOJI\n"):]
        entry = entry[:entry.index("\n\n")]
        self.assertIn("depends on MUSE_ENABLED && !MUSE_UI_FREEINK", entry)
        self.assertIn("default n", entry)
        cmake = (MUSE / "CMakeLists.txt").read_text(encoding="utf-8")
        self.assertIn('if(CONFIG_MUSE_EMOJI)\n        list(APPEND srcs "fonts/muse_font_emoji_16.c")', cmake)
        ui = (MUSE / "muse_ui.c").read_text(encoding="utf-8")
        # Only screens whose captions are in caption_font() keep emoji.
        self.assertRegex(ui, r"if \(!s_small\) \{[^}]*muse_text_keep_emoji\(\);")
        # The menu's fonts have no emoji.
        self.assertIn("muse_text_to_label(buf, n);", (MUSE / "muse_menu.c").read_text(encoding="utf-8"))


if __name__ == "__main__":
    unittest.main()
