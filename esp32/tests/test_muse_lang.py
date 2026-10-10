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

"""The screen's translations (components/muse/muse_lang.c): each is looked up
by English text that's still in the code, keeps its format conversions, and
draws with letters its fonts have."""

from __future__ import annotations

import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MUSE = ROOT / "components" / "muse"
ENTRY = re.compile(r'\{ "((?:[^"\\]|\\.)*)",\s*"((?:[^"\\]|\\.)*)" \}')
CONVERSION = re.compile(r"%[-+ #0]*\d*(?:\.\d+)?[diouxXcsfeEgGp%]")


def tables() -> dict[str, list[tuple[str, str]]]:
    source = (MUSE / "muse_lang.c").read_text(encoding="utf-8")
    found = {}
    for name, body in re.findall(r"static const muse_lang_entry_t (\w+)\[\] = \{(.*?)\n\};", source, re.S):
        found[name] = [(en.replace('\\"', '"'), text.replace('\\"', '"')) for en, text in ENTRY.findall(body)]
    return found


class LangTest(unittest.TestCase):
    def setUp(self) -> None:
        self.tables = tables()

    def test_tables_have_entries(self) -> None:
        self.assertEqual(sorted(self.tables), ["MENU", "MESSAGES", "STATUS"])
        for name, entries in self.tables.items():
            self.assertGreater(len(entries), 10, name)
            english = [en for en, _ in entries]
            self.assertEqual(len(english), len(set(english)), f"{name} repeats an entry")

    def test_english_is_still_in_the_code(self) -> None:
        code = "".join(
            p.read_text(encoding="utf-8")
            for p in sorted(MUSE.glob("*.c*"))
            if p.name != "muse_lang.c"
        )
        literals = {s.replace('\\"', '"') for s in re.findall(r'"((?:[^"\\\n]|\\.)*)"', code)}
        for name, entries in self.tables.items():
            for en, _ in entries:
                self.assertTrue(en in literals, f"{name}: nothing in the code says {en!r} any more")

    def test_translations_keep_their_conversions(self) -> None:
        for name, entries in self.tables.items():
            for en, text in entries:
                self.assertEqual(CONVERSION.findall(text), CONVERSION.findall(en), f"{name}: {en!r}")

    def test_status_names_stay_upper_case(self) -> None:
        for en, text in self.tables["STATUS"]:
            words = CONVERSION.sub("", text)
            self.assertEqual(words, words.upper(), en)

    def test_every_letter_is_in_the_fonts(self) -> None:
        def glyphs(name: str) -> set[int]:
            font = (MUSE / "fonts" / name).read_text(encoding="utf-8")
            return {int(cp, 16) for cp in re.findall(r"/\* U\+([0-9A-F]+) ", font)}

        latin = glyphs("muse_font_latin_16.c")
        for name, entries in self.tables.items():
            for en, text in entries:
                missing = sorted({c for c in text if ord(c) not in latin})
                self.assertEqual(missing, [], f"{name}: {text!r}")
        # The menu is in Montserrat too: ASCII built in, the rest from its
        # Vietnamese letters at each size.
        for px in (12, 14, 20, 28):
            has = glyphs(f"muse_font_vi_{px}.c")
            for en, text in self.tables["MENU"]:
                missing = sorted({c for c in text if ord(c) > 0x7E and ord(c) not in has})
                self.assertEqual(missing, [], f"{px} px: {text!r}")


if __name__ == "__main__":
    unittest.main()
