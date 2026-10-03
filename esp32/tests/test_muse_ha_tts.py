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

"""Speaking replies with Home Assistant (muse_ha_tts_text.c): keeping a reply's
whole text, readying it for TTS, finding where HA's MP3 is, and skipping the ID3
tag HA puts on it."""

from __future__ import annotations

import os
import shlex
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def id3(size: int, footer: bool = False) -> bytes:
    """An ID3v2.4 header for a tag of `size` bytes after it, size syncsafe."""
    syncsafe = bytes((size >> shift) & 0x7F for shift in (21, 14, 7, 0))
    return b"ID3\x04\x00" + bytes([0x10 if footer else 0]) + syncsafe


class HarnessTest(unittest.TestCase):
    binary: Path

    @classmethod
    def setUpClass(cls) -> None:
        cc = shlex.split(os.environ.get("CC", "cc"))
        if not cc or shutil.which(cc[0]) is None:
            raise unittest.SkipTest("C compiler not available")
        cls.tmp = tempfile.TemporaryDirectory()
        cls.binary = Path(cls.tmp.name) / "muse_ha_tts_harness"
        proc = subprocess.run(
            [
                *cc,
                "-include",
                str(ROOT / "tests" / "host_compat.h"),
                "-std=gnu11",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-fsanitize=address,undefined",
                "-I",
                str(ROOT / "components" / "muse"),
                str(ROOT / "tests" / "muse_ha_tts_harness.c"),
                str(ROOT / "components" / "muse" / "muse_ha_tts_text.c"),
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

    def run_harness(self, *args: str, data: bytes = b"", code: int = 0) -> bytes:
        proc = subprocess.run([str(self.binary), *args], input=data, capture_output=True)
        self.assertEqual(proc.returncode, code, msg=proc.stderr.decode())
        return proc.stdout

    def clean(self, text: str) -> str:
        return self.run_harness("clean", data=text.encode()).decode()

    def test_markdown_is_not_read_out(self) -> None:
        cases = {
            "**Paris** is the *capital*.": "Paris is the capital.",
            "# Heading\nBody": " Heading Body",
            "Run `ls -la` now": "Run ls -la now",
            "line one\r\nline two": "line one  line two",
            "Plain text, café, 日本語 \U0001F989": "Plain text, café, 日本語 \U0001F989",
            "": "",
        }
        for text, spoken in cases.items():
            with self.subTest(text=text):
                self.assertEqual(self.clean(text), spoken)

    def test_links_keep_their_label(self) -> None:
        cases = {
            "See [the docs](https://example.com/a_b) for more.": "See the docs for more.",
            "[**bold** link](http://x)": "bold link",
            "[a](x) and [b](y)": "a and b",
            # Not links: left alone, apart from the usual Markdown.
            "[no target] here": "[no target] here",
            "[spaced](not a url)": "[spaced](not a url)",
            "[open](unclosed": "[open](unclosed",
            "[split\nlabel](x)": "[split label](x)",
            "]stray[ brackets": "]stray[ brackets",
        }
        for text, spoken in cases.items():
            with self.subTest(text=text):
                self.assertEqual(self.clean(text), spoken)

    def test_long_reply_survives(self) -> None:
        text = "**word** [link](u) " * 2000
        self.assertEqual(self.clean(text), "word link " * 2000)

    def speech(self, max_bytes: int, *pieces: str, grows: int = -1) -> tuple[str, bool, str]:
        out = self.run_harness("speech", str(max_bytes), str(grows), *pieces).decode()
        results, summary, text = out.split("\n", 2)
        self.assertTrue(text.endswith("\n"))
        cut = summary.split()[0] == "cut=1"
        self.assertEqual(summary.split()[1], f"len={len(text[:-1].encode())}")
        return results, cut, text[:-1]

    def test_speech_keeps_the_whole_reply(self) -> None:
        # Far past the captions' 1,023 bytes, in the small pieces replies stream in.
        pieces = [f"Sentence {i} of a long answer. " for i in range(200)]
        results, cut, text = self.speech(8192, *pieces)
        self.assertEqual(results, "1" * 200)
        self.assertFalse(cut)
        self.assertEqual(text, "".join(pieces))
        self.assertGreater(len(text), 1023)

    def test_speech_stops_at_the_cap(self) -> None:
        results, cut, text = self.speech(16, "0123456789", "abcdefghij", "more")
        self.assertEqual(results, "100")
        self.assertTrue(cut)
        self.assertEqual(text, "0123456789abcde")   # 15 bytes and the NUL

    def test_speech_never_splits_a_character(self) -> None:
        for piece in ("é" * 20, "日本語" * 5, "\U0001F989" * 5, "aé日\U0001F989" * 4):
            for cap in range(1, 24):
                with self.subTest(piece=piece, cap=cap):
                    results, cut, text = self.speech(cap, piece)
                    fits = len(piece.encode()) <= cap - 1
                    self.assertEqual((results, cut), ("1", False) if fits else ("0", True))
                    self.assertTrue(piece.startswith(text))   # whole characters only
                    self.assertLessEqual(len(text.encode()), cap - 1)
                    self.assertGreater(len(text.encode()), cap - 1 - 4)   # backs off at most one

    def test_speech_without_memory(self) -> None:
        # The first allocation fails: nothing to speak, and it says so.
        results, cut, text = self.speech(8192, "hello", "world", grows=0)
        self.assertEqual((results, cut, text), ("00", True, ""))
        # Growing fails later: it keeps what fit in the buffer it had.
        results, cut, text = self.speech(8192, "a" * 200, "b" * 200, grows=1)
        self.assertEqual(results, "10")
        self.assertTrue(cut)
        self.assertEqual(text, "a" * 200 + "b" * 55)   # the first buffer is 256 bytes

    def test_speech_with_nothing_to_add(self) -> None:
        self.assertEqual(self.speech(8192, ""), ("1", False, ""))
        self.assertEqual(self.speech(4, "abc", ""), ("11", False, "abc"))
        self.assertEqual(self.speech(4, "abcd", ""), ("01", True, "abc"))

    def test_id3_tag_size(self) -> None:
        frame = b"\xff\xf3\x60\xc4"
        cases = [
            (id3(35) + b"\x00" * 35 + frame, 45),   # what HA sends: 45 bytes, then audio
            (id3(0x0FFFFFFF), 10 + 0x0FFFFFFF),     # the largest syncsafe size
            (id3(100, footer=True), 120),
            (frame * 4, 0),                          # untagged
            (b"ID3\x04\x00\x00\x00", 0),             # header not all here yet
            (b"", 0),
        ]
        for data, size in cases:
            with self.subTest(data=data[:12]):
                self.assertEqual(self.run_harness("id3", data=data), f"{size}\n".encode())

    def test_base_drops_trailing_slashes(self) -> None:
        cases = [
            ("http://10.0.0.5:8123/", "200", "http://10.0.0.5:8123"),
            ("http://ha.local:8123///", "200", "http://ha.local:8123"),
            ("http://ha.local:8123", "200", "http://ha.local:8123"),
            ("http://ha.local:8123", "8", "http://"),
            ("/", "200", ""),
        ]
        for base, cap, want in cases:
            with self.subTest(base=base, cap=cap):
                self.assertEqual(self.run_harness("base", base, cap), f"{want}\n".encode())
        self.assertEqual(self.run_harness("base", "x", "0"), b"")

    def test_audio_url_prefers_the_path(self) -> None:
        base = "http://10.31.30.100:8123"
        path = "/api/tts_proxy/abc.mp3"
        url = "https://external.example.com/api/tts_proxy/abc.mp3"
        self.assertEqual(self.run_harness("url", base, path, url, "320"), f"{base}{path}\n".encode())
        self.assertEqual(self.run_harness("url", base, "-", url, "320"), f"{url}\n".encode())
        self.assertEqual(self.run_harness("url", base, "relative.mp3", url, "320"), f"{url}\n".encode())
        self.run_harness("url", base, "-", "-", "320", code=1)
        self.run_harness("url", base, "-", "", "320", code=1)
        # Too long to fit: refused rather than fetched truncated.
        self.run_harness("url", base, path, url, str(len(base + path)), code=1)
        self.assertEqual(self.run_harness("url", base, path, url, str(len(base + path) + 1)), f"{base}{path}\n".encode())


if __name__ == "__main__":
    unittest.main()
