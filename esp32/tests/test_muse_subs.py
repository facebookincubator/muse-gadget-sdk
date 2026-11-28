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

"""Live call subtitles (mg_command_subtitle).

The model (components/muse/muse_subs_core.c, tests/muse_subs_harness.c):
words revealed on the frame that carries them or the phone's playout clock,
late words shown anyway, word wrap with no re-wrap on appends, the last lines
scrolling, cuts with an ellipsis, UTF-8 kept whole, resends and replaces,
the band's end. The protocol's parsing is in mg_proto_harness (test_mg_ble),
and the drawing's wiring below."""

import os
import shlex
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))   # its helpers, also when run from esp32/ as tests.<module>
from host_child import run_child  # noqa: E402 (macOS's pipe deadlock on a driver's input: host_child.py)

ROOT = Path(__file__).resolve().parents[1]
MUSE = ROOT / "components" / "muse"


class SubtitleModelTest(unittest.TestCase):
    def test_harness(self) -> None:
        cc = shlex.split(os.environ.get("CC", "cc"))
        with tempfile.TemporaryDirectory() as tmp:
            exe = Path(tmp) / "subs"
            cmd = cc + ["-std=gnu11", "-O1", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                        "-Wall", "-Wextra", "-Werror", "-I", str(MUSE),
                        str(ROOT / "tests" / "muse_subs_harness.c"), str(MUSE / "muse_subs_core.c"), "-lm", "-o", str(exe)]
            r = subprocess.run(cmd, capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            r = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            self.assertIn("all passed", r.stdout)
            self.assertIn("band: never while locked, whatever is in front", r.stdout)
            self.assertIn("barge-in: a dropped reply's id is free again after the next reply or the mode's end", r.stdout)
            src = (MUSE / "muse_subs.c").read_text()
            tick = src[src.index("void muse_subs_tick(void)"):]
            tick = tick[:tick.index("\n}\n")]
            self.assertIn("bool show = muse_subs_band_shows(f, s_video, muse_passcode_locked()) && !s_card;", tick)


class SubtitleVectorsTest(unittest.TestCase):
    """protocols/test-vectors/mg-subtitle-v1.json through mg_proto and the model."""

    def test_vectors(self) -> None:
        import json
        import sys
        sys.path.insert(0, str(ROOT / "tests"))
        import test_mg_ble as t
        data = json.loads((ROOT.parent / "protocols/test-vectors/mg-subtitle-v1.json").read_text())
        script, expect = [], []
        for m in data["messages"]:
            if m["fields"]["type"] == "capabilities":
                continue   # device -> phone: the phone's parse
            script.append("M " + m["hex"])
            expect.append(("msg", m))
        for m in data["malformed"]:
            script.append("M " + m["hex"])
            expect.append(("bad", m))
        for case in data["reveal"]:
            # Each case starts with subtitle mode off; its own first state message turns it on (a case that
            # sends words first checks they're ignored).
            script.append("O")
            for st in case["steps"]:
                if "message" in st:
                    script.append("M " + st["message"])
                elif "frame_shown" in st:
                    script.append(f"F {st['frame_shown']}")
                elif "frame_received" in st:
                    script.append(f"R {st['frame_received']}")
                elif st.get("stream_start"):
                    script.append("S")
                elif st.get("lock"):
                    script.append("L")
                elif st.get("unlock"):
                    script.append("U")
                else:
                    self.fail(f"{case['name']}: a step of a shape this replay doesn't know: {st}")
                expect.append(("text", case["name"], st))
        with tempfile.TemporaryDirectory() as tmp, tempfile.TemporaryDirectory(prefix="mg-gen-") as gen:
            sp = Path(tmp) / "script.txt"
            sp.write_text("\n".join(script) + "\n")
            t.proof_vectors_header(Path(gen))
            srcs, defs, incs = t.codec_build(False)
            MG = t.MG
            out = t.compile_and_run(
                self,
                [ROOT / "tests/subs_vectors_harness.c", MG / "mg_proto.c", MG / "mg_session.c", MG / "mg_ui.c", MG / "mg_queue.c",
                 MG / "mg_route.c", MG / "mg_codec.c", MG / "mg_play.c", MG / "mg_resample.c",
                 ROOT / "components/muse_vstream/muse_vstream.c", ROOT / "tests/mg_token_proof_ref.c",
                 MUSE / "muse_subs_core.c"],
                defs, [Path(gen), MG, MG / "include", ROOT / "components/muse_vstream/include", t.PROTOCOLS,
                       MUSE, *incs], args=[str(sp)], vendor=srcs)
        lines = out.splitlines()
        i = 0
        for kind, *rest in expect:
            if kind in ("msg", "bad"):
                m = rest[0]
                res = lines[i]
                i += 2   # the result, then its T line
                if kind == "bad":
                    self.assertTrue(res.startswith("ERR"), f"{m['name']}: accepted: {res}")
                    continue
                self.assertTrue(res.startswith("OK"), f"{m['name']}: refused")
                f = m["fields"]
                if f["type"] == "words":
                    want = " ".join(f"{w['show_at']}:{w['text'].encode('utf-8').hex()}" for w in f["words"])
                    self.assertIn(f"reply={f['reply_id']} flags={f['flags']} offset={f['offset']} {want}", res,
                                  m["name"])
                elif f["type"] == "state":
                    self.assertIn(f"active={int(f['active'])} timing={f['timing']}", res, m["name"])
                elif f["type"] == "reply_end":
                    self.assertIn(f"reply={f['reply_id']} show_at={f['show_at']}", res, m["name"])
                elif f["type"] == "clear":
                    self.assertIn(f"reply={f['reply_id']} reason={f['reason']}", res, m["name"])
                elif f["type"] == "clock":
                    self.assertIn(f"reply={f['reply_id']} playout={f['playout_ms']}", res, m["name"])
            else:
                name, st = rest
                if "message" in st:
                    i += 1   # its OK/ERR line
                got = bytes.fromhex(lines[i].split(" ", 1)[1] if " " in lines[i] else "").decode("utf-8")
                i += 1
                self.assertEqual(got, st["text"], f"{name}: after {st}")


    def test_a_whole_words_message_fits_the_replay(self) -> None:
        # The replay's fields line holds a words message up to max_words_bytes (480), here two 200-byte words, and
        # writes nothing past its buffer (AddressSanitizer on, where the host has it).
        import sys
        sys.path.insert(0, str(ROOT / "tests"))
        sys.path.insert(0, str(ROOT.parent / "protocols/test-vectors"))
        import mg_subtitle_ref as ref
        import test_mg_ble as t
        cc = shlex.split(os.environ.get("CC", "cc"))
        probe = run_child([*cc, "-fsanitize=address", "-x", "c", "-", "-o", os.devnull],
                          input="int main(void){return 0;}", text=True, capture_output=True)
        asan = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"] if probe.returncode == 0 else []
        big = ref.words(9, 0, 0, [(1, b"a" * 200), (2, b"b" * 200)])
        script = ["O", "M " + ref.state(True, 0).hex(), "M " + big.hex()]
        with tempfile.TemporaryDirectory() as tmp, tempfile.TemporaryDirectory(prefix="mg-gen-") as gen:
            sp = Path(tmp) / "script.txt"
            sp.write_text("\n".join(script) + "\n")
            t.proof_vectors_header(Path(gen))
            srcs, defs, incs = t.codec_build(False)
            MG = t.MG
            out = t.compile_and_run(
                self,
                [ROOT / "tests/subs_vectors_harness.c", MG / "mg_proto.c", MG / "mg_session.c", MG / "mg_ui.c", MG / "mg_queue.c",
                 MG / "mg_route.c", MG / "mg_codec.c", MG / "mg_play.c", MG / "mg_resample.c",
                 ROOT / "components/muse_vstream/muse_vstream.c", ROOT / "tests/mg_token_proof_ref.c",
                 MUSE / "muse_subs_core.c"],
                defs, [Path(gen), MG, MG / "include", ROOT / "components/muse_vstream/include", t.PROTOCOLS,
                       MUSE, *incs], extra=asan, args=[str(sp)], vendor=srcs)
        lines = out.splitlines()
        self.assertEqual(lines[2], "OK 3 reply=9 flags=0 offset=0 1:" + "61" * 200 + " 2:" + "62" * 200)


def home_geom_header(out: Path) -> None:
    """muse_home.c's muse_home_geom (and what it needs) as a host header: the
    tall band's top row and clock come from the home's own geometry."""
    import re
    hdr = (MUSE / "muse_home.h").read_text()
    src = (MUSE / "muse_home.c").read_text()
    struct = re.search(r"typedef struct \{.*?\} muse_home_geom_t;", hdr, re.S).group(0)

    def func(start: str) -> str:
        i = src.index(start)
        return src[i:src.index("\n}\n", i) + 3]

    ink = "\n".join(re.findall(r"#define MUSE_INK_ROW\d \d+", src))
    out.write_text("#pragma once\n#include <math.h>\n#include <stdbool.h>\n#include <string.h>\n" + struct + "\n" + ink +
                   "\n" + func("static int clampi(") + func("static int pr(double v)") +
                   func("void muse_home_geom(int w, int h, bool round, int corner_px, muse_home_geom_t *g)"))


class SubtitleTallBandTest(unittest.TestCase):
    """With no video on screen the
    band takes the screen: as many lines as fit up to the top row (or the clock),
    newest at the bottom, scrolling one line at a time, one wrap width (the
    bottom line's chord on a round panel); <= 2 lines over video."""

    def run_harness(self) -> str:
        import json
        cc = shlex.split(os.environ.get("CC", "cc"))
        data = json.loads((ROOT.parent / "protocols/test-vectors/mg-subtitle-v1.json").read_text())
        lines = []
        long_text = []
        for sp in data["splits"]:
            lines.append(f"V {sp['name']}")
            for w in sp["words"]:
                lines.append(f"W {w['offset']} {w['text'].encode('utf-8').hex()}")
            long_text.append(sp)
        # The long muted reply: every vector's words, six times over (past the pool's 128 words), one reply's offsets.
        lines.append("L")
        off = 0
        for _ in range(6):
            for sp in long_text:
                ws = sp["words"]
                for k, w in enumerate(ws):
                    t = w["text"].encode("utf-8")
                    if k + 1 == len(ws) and not t.endswith(b" "):
                        t += b" "
                    lines.append(f"W {off} {t.hex()}")
                    off += len(t)
        with tempfile.TemporaryDirectory() as tmp:
            home_geom_header(Path(tmp) / "home_geom_extract.h")
            (Path(tmp) / "vectors.txt").write_text("\n".join(lines) + "\n")
            exe = Path(tmp) / "tall"
            cmd = cc + ["-std=gnu11", "-O1", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                        "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", "-I", tmp, "-I", str(MUSE),
                        str(ROOT / "tests" / "muse_subs_tall_harness.c"), str(MUSE / "muse_subs_core.c"), "-lm",
                        "-o", str(exe)]
            r = subprocess.run(cmd, capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            r = subprocess.run([str(exe), str(Path(tmp) / "vectors.txt")], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            self.assertIn("all passed", r.stdout)
            return r.stdout

    def test_harness(self) -> None:
        self.run_harness()

    def test_both_bitmaps_start_blank(self) -> None:
        # Both text bitmaps start blank, before the band is ready: the first video piece may read the front one
        # before any render for video wrote it.
        src = (MUSE / "muse_subs.c").read_text()
        init = src[src.index("void muse_subs_ui_init(void)\n{"):]
        init = init[:init.index("\n}\n")]
        self.assertLess(init.index("memset(s_text[0], 0, n);\n    memset(s_text[1], 0, n);"), init.index("s_ready = true;"))


class VideoHandsBackTest(unittest.TestCase):
    def test_a_stall_gives_the_band_back_to_the_gui(self) -> None:
        # Whenever the video hands the panel back to LVGL (pause_lvgl(false): a stall with
        # the stream still open, the passcode hold, the end), the GUI's tall band shows the words; the next
        # picture drawn takes them back (muse_subs_frame_shown). The frame timing isn't reset.
        video = (MUSE / "muse_video.c").read_text()
        pause = video[video.index("static void pause_lvgl(bool pause)\n{"):]
        pause = pause[:pause.index("\n}\n")]
        self.assertIn("muse_subs_video_yield();", pause[pause.index("if (!pause) {"):])
        subs = (MUSE / "muse_subs.c").read_text()
        y = subs[subs.index("void muse_subs_video_yield(void)\n{"):]
        y = y[:y.index("\n}\n")]
        self.assertIn("s_video = false;", y)
        self.assertNotIn("muse_subs_no_frame", y)


class UiStreamSubtitlesTest(unittest.TestCase):
    def test_the_frame_buffer_path_draws_the_band(self) -> None:
        # A UI stream through the frame buffer (s_hq, the default) gets the band like camera
        # video: every frame buffer draw of a picture, a still patch or the whole picture goes through
        # ui_fb_fill_subs (the pixels, then muse_subs_overlay); the picture's words come due with it
        # (muse_subs_frame_shown); a band that changes with no new picture is drawn again from the frame buffer
        # (ui_subs_redraw, between pictures, not while held).
        video = (MUSE / "muse_video.c").read_text()
        fill = video[video.index("static void ui_fb_fill_subs("):]
        fill = fill[:fill.index("\n}\n")]
        self.assertLess(fill.index("muse_ui_fb_fill(dst, stride, row, rows, (void *)&a->r);"),
                        fill.index("muse_subs_overlay(dst, stride, a->px, a->py + row, a->r.w, rows);"))
        self.assertEqual(video.count("PIECE_ROWS, ui_fb_fill_subs, &a);"), 4)   # picture, patch, whole, redraw
        pic = video[video.index("fb_rects = muse_ui_fb_picture("):]
        pic = pic[:pic.index("if (fb_rects < 0) {")]
        self.assertLess(pic.index("muse_subs_frame_shown(sl->number32);"), pic.index("ui_fb_fill_subs, &a);"))
        self.assertIn("ui_subs_redraw();", pic)
        self.assertIn("if (s_streaming && s_ui && s_paused && !held()) {\n                ui_subs_redraw();", video)


class SubtitleModeTest(unittest.TestCase):
    """Only the phone's state 1 turns subtitles on; state 0 clears the band at once."""

    def test_through_the_protocol(self) -> None:
        # The messages as the phone sends them, through mg_proto and the model:
        # words, reply_end and clock before state 1 show nothing; after state 1
        # they do; state 0 mid-reply empties the band; the rest of that reply
        # and a new reply's words after it stay off.
        import sys
        sys.path.insert(0, str(ROOT / "tests"))
        sys.path.insert(0, str(ROOT.parent / "protocols/test-vectors"))
        import mg_subtitle_ref as ref
        import test_mg_ble as t
        now = 0xFFFFFFFF
        steps = [
            (ref.words(1, 0, 0, [(now, b"hello "), (now, b"there")]), ""),
            (ref.reply_end(1, now), ""),
            (ref.clock(1, 500), ""),
            (ref.state(True, 1), ""),
            (ref.words(2, 0, 0, [(now, b"hello "), (now, b"there")]), "hello there"),
            (ref.state(False, 1), ""),
            (ref.words(2, 1, 11, [(now, b" friend.")]), ""),
            (ref.words(3, 0, 0, [(now, b"new ")]), ""),
            (ref.state(True, 0), ""),
            (ref.words(4, 0, 0, [(now, b"back ")]), "back "),
            (ref.clear(4, 1), ""),
        ]
        script = ["O"] + ["M " + m.hex() for m, _ in steps]
        with tempfile.TemporaryDirectory() as tmp, tempfile.TemporaryDirectory(prefix="mg-gen-") as gen:
            sp = Path(tmp) / "script.txt"
            sp.write_text("\n".join(script) + "\n")
            t.proof_vectors_header(Path(gen))
            srcs, defs, incs = t.codec_build(False)
            MG = t.MG
            out = t.compile_and_run(
                self,
                [ROOT / "tests/subs_vectors_harness.c", MG / "mg_proto.c", MG / "mg_session.c", MG / "mg_ui.c", MG / "mg_queue.c",
                 MG / "mg_route.c", MG / "mg_codec.c", MG / "mg_play.c", MG / "mg_resample.c",
                 ROOT / "components/muse_vstream/muse_vstream.c", ROOT / "tests/mg_token_proof_ref.c",
                 MUSE / "muse_subs_core.c"],
                defs, [Path(gen), MG, MG / "include", ROOT / "components/muse_vstream/include", t.PROTOCOLS,
                       MUSE, *incs], args=[str(sp)], vendor=srcs)
        lines = out.splitlines()
        self.assertEqual(len(lines), 2 * len(steps), out)
        for k, (m, want) in enumerate(steps):
            self.assertTrue(lines[2 * k].startswith("OK"), f"step {k}: {lines[2 * k]}")
            text = lines[2 * k + 1].split(" ", 1)
            got = bytes.fromhex(text[1] if len(text) > 1 else "").decode()
            self.assertEqual(got, want, f"step {k}: {ref.parse(m)}")

if __name__ == "__main__":
    unittest.main()
