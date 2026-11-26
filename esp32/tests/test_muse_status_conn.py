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

"""The Status page's connection row.

"Connected: <name>" names the phone as the gadget sees it: the central's GAP
Device Name, else "phone" while that read
is pending or after it failed, counted (client_name, 18, is reserved), never plain "Connected" nor "Connected: Muse app"; a Wi-Fi gadget with no phone app shows
"Connected: <Wi-Fi network>". A row too wide for its line wraps
onto a second one (no "...", the whole name
readable), at a space where both lines stay whole, else at whole UTF-8
characters, and only what still doesn't fit is cut, at whole characters.
muse_status_conn.c runs here with muse_text.c; the last case checks the name never reaches the Nordic UART.
"""

from __future__ import annotations

import os
import random
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
MG = ROOT / "components" / "muse_gadget_ble"

DRIVER = r"""
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "muse_status_conn.h"

/* One command a line:
 *   conn KIND|gap|app|version|ssid|ip|rssi   KIND none, bt, app or wifi; "~" is NULL;
 *                                                  prints the row's text, a tab, the tap's text
 *   wrap W1 W2 TEXT   TEXT onto lines W1 and W2 characters wide (each UTF-8 character is 1 wide):
 *                     prints the count, a tab, line 1, a tab, line 2 */
static int width(const char *s, void *ctx)
{
    (void)ctx;
    int w = 0;
    for (; *s; s++) {
        w += ((unsigned char)*s & 0xC0) != 0x80;
    }
    return w;
}

static char *field(char **s)
{
    char *f = *s, *bar = strchr(f, '|');
    if (bar) {
        *bar = 0;
        *s = bar + 1;
    } else {
        *s = f + strlen(f);
    }
    return strcmp(f, "~") ? f : NULL;
}

int main(void)
{
    static char line[1024];
    while (fgets(line, sizeof(line), stdin)) {
        line[strcspn(line, "\n")] = 0;
        if (!strncmp(line, "conn ", 5)) {
            char *s = line + 5;
            const char *kind = field(&s);
            muse_conn_info_t ci = { .kind = !strcmp(kind, "app") ? MUSE_CONN_APP : !strcmp(kind, "wifi") ? MUSE_CONN_WIFI
                                            : !strcmp(kind, "bt") ? MUSE_CONN_BT_ONLY : MUSE_CONN_NONE };
            ci.gap_name = field(&s);
            ci.app_name = field(&s);
            ci.app_version = field(&s);
            ci.ssid = field(&s);
            ci.ip = field(&s);
            const char *r = field(&s);
            ci.rssi = r ? atoi(r) : 0;
            char row[96], detail[96];
            muse_status_conn_text(&ci, row, sizeof(row));
            muse_status_detail_text(&ci, detail, sizeof(detail));
            printf("%s\t%s\n", row, detail);
        } else if (!strncmp(line, "wrap ", 5)) {
            char *end;
            int w1 = (int)strtol(line + 5, &end, 10);
            int w2 = (int)strtol(end, &end, 10);
            char l1[MUSE_STATUS_LINE_MAX], l2[MUSE_STATUS_LINE_MAX];
            int lines = muse_status_wrap(end + 1, w1, w2, width, NULL, l1, l2, sizeof(l1));
            printf("%d\t%s\t%s\n", lines, l1, l2);
        } else if (!strncmp(line, "small ", 6)) {
            char l1[8], l2[8];   /* buffers smaller than the text: whole characters only */
            int lines = muse_status_wrap(line + 6, 1000, 1000, width, NULL, l1, l2, sizeof(l1));
            printf("%d\t%s\t%s\n", lines, l1, l2);
        }
    }
    return 0;
}
"""


class StatusConnTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cc = shlex.split(os.environ.get("CC", "cc"))
        if not cc or shutil.which(cc[0]) is None:
            raise unittest.SkipTest("C compiler not available")
        cls.tmp = tempfile.TemporaryDirectory()
        d = Path(cls.tmp.name)
        (d / "driver.c").write_text(DRIVER)
        cls.exe = d / "status_conn"
        proc = subprocess.run([*cc, "-include", str(ROOT / "tests" / "host_compat.h"), "-std=gnu11", "-Wall",
                               "-Wextra", "-Werror", "-fsanitize=address,undefined", "-I", str(MUSE),
                               str(d / "driver.c"), str(MUSE / "muse_status_conn.c"), str(MUSE / "muse_text.c"),
                               "-o", str(cls.exe)], text=True, capture_output=True)
        if proc.returncode:
            raise AssertionError(proc.stdout + proc.stderr)

    @classmethod
    def tearDownClass(cls) -> None:
        cls.tmp.cleanup()

    def run_lines(self, lines: list[str]) -> list[str]:
        out = run_child([str(self.exe)], input="\n".join(lines).encode() + b"\n", capture_output=True,
                        check=True).stdout
        return out.decode("utf-8").splitlines()

    def conn(self, kind: str, gap="~", app="~", ver="~", ssid="~", ip="~", rssi="0") -> tuple[str, str]:
        row, detail = self.run_lines([f"conn {kind}|{gap}|{app}|{ver}|{ssid}|{ip}|{rssi}"])[0].split("\t")
        return row, detail

    def test_the_name_is_the_gap_name_else_phone(self) -> None:
        # Only the GAP name the gadget read (client_name, 18, is reserved);
        # "phone" while that read is pending or after it failed.
        self.assertEqual(self.conn("app", "Testerphone17ProMax", "Muse app", "16.0.0.86"),
                         ("Connected: Testerphone17ProMax", "Muse app: 16.0.0.86"))
        self.assertEqual(self.conn("app", "Galaxy S22")[0], "Connected: Galaxy S22")
        self.assertEqual(self.conn("app"), ("Connected: phone", "App version unknown"))
        self.assertEqual(self.conn("app", "", "Muse app", "16.0.0.86"), ("Connected: phone", "Muse app: 16.0.0.86"))
        # Never "Connected" alone, never the app's name in the row, never "unknown phone".
        for row, _ in (self.conn("app", "", "Muse app", "16.0.0.86"), self.conn("app", " ")):
            self.assertNotEqual(row, "Connected")
            self.assertNotIn("Muse app", row)
            self.assertEqual(row, "Connected: phone")
        self.assertNotIn("unknown phone", (MUSE / "muse_status_conn.c").read_text())
        self.assertNotIn("in->client_name", (MUSE / "muse_status_conn.c").read_text())

    def test_names_take_the_fonts_stand_ins(self) -> None:
        # The fonts are ASCII (muse_text.h): accents and curly quotes get plain
        # ones; a name with nothing showable left is "phone".
        self.assertEqual(self.conn("app", "R\u00e9n\u00e9e\u2019s iPhone")[0], "Connected: Renee's iPhone")
        self.assertEqual(self.conn("app", "\U0001F4F1")[0], "Connected: phone")


    def wrap(self, w1: int, w2: int, text: str) -> tuple[int, str, str]:
        n, l1, l2 = self.run_lines([f"wrap {w1} {w2} {text}"])[0].split("\t")
        return int(n), l1, l2   # 1 one line, 2 two, 3 two with line 2 cut

    def test_wraps_instead_of_cutting(self) -> None:
        # A name too wide for the row goes onto a
        # second line, whole, with no "...".
        self.assertEqual(self.wrap(30, 30, "Connected: Pixel 9 Pro"), (1, "Connected: Pixel 9 Pro", ""))
        self.assertEqual(self.wrap(22, 22, "Connected: Pixel 9 Pro"), (1, "Connected: Pixel 9 Pro", ""))
        # At a space: whole words on both lines.
        self.assertEqual(self.wrap(20, 20, "Connected: Tester's iPhone 17 Pro Max"),
                         (2, "Connected: Tester's", "iPhone 17 Pro Max"))
        self.assertEqual(self.wrap(15, 22, "Connected: Testerphone17ProMax"),
                         (2, "Connected:", "Testerphone17ProMax"))
        self.assertEqual(self.wrap(12, 12, "Muse app: 16.0.0.86"), (2, "Muse app:", "16.0.0.86"))
        # No space that leaves both lines whole: whole characters, then the rest.
        self.assertEqual(self.wrap(10, 10, "Connected:ABCDEFGHIJKL"), (3, "Connected:", "ABCDEFGHIJ"))
        self.assertEqual(self.wrap(15, 15, "Connected: ABCDEFGHIJKLMNOPQRS"),
                         (2, "Connected: ABCD", "EFGHIJKLMNOPQRS"))
        for r in (self.wrap(20, 20, "Connected: Tester's iPhone 17 Pro Max"),
                  self.wrap(4, 4, "\u65e5\u672c\u8a9e\u30c6\u30ad\u30b9\u30c8\u65e5\u672c\u8a9e")):
            self.assertNotIn("...", r[1] + r[2])
            self.assertNotIn("\u2026", r[1] + r[2])

    def test_at_most_two_lines_then_cut_at_whole_characters(self) -> None:
        # Still too long for two lines: line 2 cut at whole characters, never "...".
        self.assertEqual(self.wrap(5, 5, "\u65e5\u672c\u8a9e\u30c6\u30ad\u30b9\u30c8\u65e5\u672c\u8a9e\u30c6\u30ad"),
                         (3, "\u65e5\u672c\u8a9e\u30c6\u30ad", "\u30b9\u30c8\u65e5\u672c\u8a9e"))
        self.assertEqual(self.wrap(10, 6, "Connected: Testerphone17ProMax"), (3, "Connected:", "Tester"))
        # A line narrower than one character still moves on by one.
        self.assertEqual(self.wrap(0, 0, "ab"), (3, "a", ""))
        # Buffers smaller than the text keep whole characters.
        self.assertEqual(self.run_lines(["small \u65e5\u672c\u8a9e"]), ["1\t\u65e5\u672c\t"])

    def test_wrap_never_splits_a_character(self) -> None:
        rnd = random.Random(5)
        alphabet = ["a", " ", "\u00e9", "\u2019", "\u65e5", "\U0001F4F1", "Z"]
        lines, cases = [], []
        for _ in range(400):
            text = "".join(rnd.choice(alphabet) for _ in range(rnd.randint(1, 40)))
            w1, w2 = rnd.randint(1, 32), rnd.randint(1, 32)
            lines.append(f"wrap {w1} {w2} {text}")
            cases.append((text, w1, w2))
        for (text, w1, w2), out in zip(cases, self.run_lines(lines)):   # decoding as UTF-8 is the check
            n, l1, l2 = out.split("\t")
            if len(text) <= w1:
                self.assertEqual((n, l1, l2), ("1", text, ""))
                continue
            self.assertIn(n, ("2", "3"))
            rest_all = text[len(l1):].lstrip(" ").rstrip(" ")
            self.assertEqual(n == "3", l2 != rest_all, (text, w1, w2, n, l1, l2))
            self.assertLessEqual(len(l1), w1)
            self.assertLessEqual(len(l2), w2)
            self.assertNotIn("...", l1 + l2)
            # What shows is the text in order, only spaces dropped at the break, and nothing
            # cut from line 2 unless the whole rest couldn't fit.
            self.assertTrue(text.startswith(l1), (text, l1))
            rest = text[len(l1):].lstrip(" ")
            if l2:
                self.assertTrue(rest.startswith(l2), (text, l1, l2))
            if len(rest) <= w2 and rest.rstrip(" ") == rest:
                self.assertEqual(l2, rest, (text, w1, w2))


    def test_the_name_never_reaches_nus(self) -> None:
        # Privacy (mgcommands.h): the phone's name is the user's, and the app relays the
        # Nordic UART mirror (every mg.* log line) into its logs. No log or console call that the mirror
        # can carry prints client_name, client_app or the GAP name: they reach the screen only.
        names = re.compile(r"client_name|client_app|gap_name|app_name|app_version|peer\.|s_gap_name|\bb\b,")
        call = re.compile(r"\b(ESP_LOG[EWIDV]|ESP_EARLY_LOG[EWIDV]|printf|nus_printf|mg_nus_\w+|puts)\s*\(", re.S)
        files = [MG / "mg_ble.c", MG / "mg_proto.c", MUSE / "muse_home.c", MUSE / "muse_status_conn.c",
                 MUSE / "muse_input.c", ROOT / "main" / "mg_glue.c", ROOT / "main" / "muse_glue.c"]
        for path in files:
            text = path.read_text()
            for m in call.finditer(text):
                end = text.index(");", m.end())
                stmt = text[m.start():end]
                self.assertIsNone(names.search(stmt), f"{path.name}: {stmt[:160]}")
        # mg_proto holds them and logs nothing at all (no IDF in it).
        self.assertNotIn("ESP_LOG", (MG / "mg_proto.c").read_text())
        # The GAP name's own lines say only whether it came and its length.
        ble = (MG / "mg_ble.c").read_text()
        cb = ble[ble.index("static int gap_name_read_cb("):]
        cb = cb[:cb.index("\n}\n")]
        self.assertIn('"peer: GAP name %s (%u bytes)", ok ? "read" : "not text, not shown", (unsigned)total', cb)


if __name__ == "__main__":
    unittest.main()
