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

"""How tools/muse/avatar.py installs a renderer Muse sends: a candidate that
doesn't build or run, or a reply cut off on the way, must leave the avatar you
have (and the one before it) as they were. Runs the real host checks on small
test renderers (flat squares, not anyone's avatar), with a fake board."""

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
import textwrap
import time
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools" / "muse"))

import avatar  # noqa: E402
import chat  # noqa: E402


def renderer(colour, body=""):
    """A complete renderer drawing a flat square in `colour`; `body` goes into render()."""
    return f"""// Copyright (c) Meta Platforms, Inc. and affiliates.
/* Test renderer: a flat square, colour {colour:06x}. */
#include <stdint.h>
#include <string.h>
#include "muse_pixel.h"

static uint8_t s_px[MUSE_PX_H][MUSE_PX_W];
static int s_size = MUSE_PX_W;

uint32_t muse_pixel_accent(muse_mode_t mode)
{{
    (void)mode;
    return 0x{colour:06x};
}}

void muse_pixel_set_size(int px)
{{
    s_size = px > 0 ? px : MUSE_PX_W;
}}

void muse_pixel_render(const muse_pose_t *pose)
{{
    int r = 8 + (int)pose->mode * 2;
    memset(s_px, 0, sizeof(s_px));
    for (int y = 32 - r; y < 32 + r; y++) {{
        for (int x = 32 - r; x < 32 + r; x++) {{
            s_px[y][x] = 1;
        }}
    }}
{body}}}

void muse_pixel_scale(uint16_t *dst, int stride_px, int x0, int x1, int y0, int y1)
{{
    uint16_t c = (uint16_t)(((0x{colour:06x} >> 19) & 31) << 11 | ((0x{colour:06x} >> 10) & 63) << 5 | ((0x{colour:06x} >> 3) & 31));
    for (int y = y0; y <= y1; y++, dst += stride_px) {{
        for (int x = x0; x <= x1; x++) {{
            dst[x - x0] = s_px[y * MUSE_PX_H / s_size][x * MUSE_PX_W / s_size] ? c : 0;
        }}
    }}
}}
"""


GOOD_A = renderer(0x3366CC)
GOOD_B = renderer(0xCC6633)
NO_BUILD = renderer(0x11AA11).replace("memset(s_px, 0, sizeof(s_px));", "memset(s_px, 0, sizeof(s_px))")
OUT_OF_BOUNDS = renderer(0xAA1111, "    s_px[MUSE_PX_H][MUSE_PX_W + 100] = 2;\n")


def fenced(code):
    return f"Here I am:\n\n```c\n{code}```\n"


def chat_reply(text, complete=True, lost=0):
    r = chat.Reply()
    r.feed({"type": "text", "msg": 0, "text": text})
    r.feed({"type": "message_done", "msg": 0, "bytes": len(text.encode("utf-8"))})
    r.complete = complete
    r.lost = lost
    return r


class FakeBoard:
    """Answers each typed turn with the next scripted Reply."""

    def __init__(self, *replies):
        self.replies = list(replies)
        self.sent = []

    def chat(self, text, progress=None):
        self.sent.append(text)
        return self.replies.pop(0)

    def cancel(self):
        pass


class Installing(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.tmp)
        self.dir = os.path.join(self.tmp, "avatar")
        os.makedirs(self.dir)
        self.active = os.path.join(self.dir, "muse_pixel.c")
        for name, value in (("AVATAR_DIR", self.dir), ("AVATAR_SRC", self.active),
                            ("LAST_REPLY", os.path.join(self.dir, "last_reply.md")),
                            ("BUILD_LOG", os.path.join(self.tmp, "build_{}.log"))):   # not your real log
            patcher = mock.patch.object(avatar, name, value)
            patcher.start()
            self.addCleanup(patcher.stop)
        quiet = mock.patch.object(avatar, "say", lambda msg: None)
        quiet.start()
        self.addCleanup(quiet.stop)
        Path(self.active).write_text(GOOD_A)

    def active_text(self):
        return Path(self.active).read_text() if os.path.exists(self.active) else None

    def recoverable(self, code):
        """`code` is still on disk in the avatar directory, under any name."""
        for root, _, files in os.walk(self.dir):
            for name in files:
                if Path(root, name).read_text(errors="replace").strip() == code.strip():
                    return True
        return False

    def test_a_good_candidate_replaces_the_avatar_and_keeps_the_last(self):
        avatar.make_avatar(None, None, fenced(GOOD_B))
        self.assertEqual(self.active_text().strip(), GOOD_B.strip())
        self.assertTrue(self.recoverable(GOOD_A))

    def test_a_candidate_that_does_not_build_leaves_the_avatar(self):
        with self.assertRaises(avatar.Stop):
            avatar.make_avatar(None, None, fenced(NO_BUILD))
        self.assertEqual(self.active_text(), GOOD_A)

    def test_a_candidate_that_crashes_leaves_the_avatar(self):
        with self.assertRaises(avatar.Stop):
            avatar.make_avatar(None, None, fenced(OUT_OF_BOUNDS))
        self.assertEqual(self.active_text(), GOOD_A)

    def test_failed_repair_rounds_keep_the_original(self):
        board = FakeBoard(chat_reply(fenced(NO_BUILD)), chat_reply(fenced(OUT_OF_BOUNDS)))
        with self.assertRaises(avatar.Stop):
            avatar.make_avatar(board, None, fenced(NO_BUILD))
        self.assertEqual(len(board.sent), 2)                 # both repair rounds were asked for
        self.assertEqual(self.active_text(), GOOD_A)
        self.assertTrue(self.recoverable(GOOD_A))

    def test_a_cut_off_reply_is_not_installed(self):
        board = FakeBoard(chat_reply(fenced(GOOD_B), complete=False))
        with self.assertRaises(avatar.Stop):
            avatar.make_avatar(board, None, avatar.ask(board, "draw yourself", "reply"))
        self.assertEqual(self.active_text(), GOOD_A)

    def test_a_reply_with_lost_lines_is_not_installed(self):
        board = FakeBoard(chat_reply(fenced(GOOD_B), lost=2))
        with self.assertRaises(avatar.Stop):
            avatar.make_avatar(board, None, avatar.ask(board, "draw yourself", "reply"))
        self.assertEqual(self.active_text(), GOOD_A)

    def test_a_cut_off_reply_is_kept_for_you_to_look_at(self):
        board = FakeBoard(chat_reply(fenced(GOOD_B), complete=False))
        with self.assertRaises(avatar.Stop):
            avatar.ask(board, "draw yourself", "reply")
        self.assertIn("colour cc6633", Path(self.dir, "last_reply.md").read_text())

    def test_the_new_avatar_is_a_new_file(self):
        old = time.time() - 3600
        os.utime(self.active, (old, old))
        avatar.make_avatar(None, None, fenced(GOOD_B))
        self.assertGreater(os.stat(self.active).st_mtime, old + 60)   # a build can't take it for the old one

    def test_nothing_half_written_is_left(self):
        avatar.make_avatar(None, None, fenced(GOOD_B))
        with self.assertRaises(avatar.Stop):
            avatar.make_avatar(None, None, fenced(NO_BUILD))
        self.assertEqual([n for n in os.listdir(self.dir) if n.endswith(avatar.TMP_SUFFIX)], [])


class FirmwareBuild(Installing):
    """With a board, the candidate's firmware builds in a copy of the tree, never in yours."""

    def setUp(self):
        super().setUp()
        self.calls = []
        self.result = (True, "built")
        self.log = ""
        fake = mock.patch.object(avatar, "board_sh", self.board_sh)
        fake.start()
        self.addCleanup(fake.stop)

    def board_sh(self, *args, root=avatar.ROOT):
        avatar_c = os.path.join(root, "components", "muse", "avatar", "muse_pixel.c")
        built = Path(avatar_c).read_text() if os.path.exists(avatar_c) else None
        self.calls.append((args, root, built))
        if args[0] == "build":
            Path(avatar.BUILD_LOG.format(args[1])).write_text(self.log)
            if self.result[0] and self.image:
                b = os.path.join(root, avatar.build_dir(args[1], root))
                os.makedirs(b, exist_ok=True)
                Path(b, "project_description.json").write_text(json.dumps({"app_bin": "muse.bin"}))
                Path(b, "muse.bin").write_text("image with " + (built or ""))
            return self.result
        return self.flash_result

    image = True
    flash_result = (True, "flashed")

    def test_it_builds_the_candidate_in_a_copy_and_keeps_that(self):
        made = avatar.make_avatar(None, "s3", fenced(GOOD_B))
        (args, root, built), = self.calls
        self.assertEqual(args, ("build", "s3"))
        self.assertNotEqual(root, avatar.ROOT)
        self.assertEqual(made.root, avatar.kept_dir("s3"))
        self.assertEqual(built.strip(), GOOD_B.strip())
        self.assertEqual(self.active_text().strip(), GOOD_B.strip())

    def test_a_firmware_build_error_in_the_avatar_leaves_yours(self):
        self.result = (False, "failed")
        self.log = "components/muse/avatar/muse_pixel.c:12:5: error: oops\n  x\n  ^\n"
        with self.assertRaises(avatar.Stop):
            avatar.make_avatar(None, "s3", fenced(GOOD_B))
        self.assertEqual(self.active_text(), GOOD_A)

    def test_a_firmware_build_error_elsewhere_leaves_yours(self):
        self.result = (False, "failed")
        self.log = "main/app.c:1:1: error: unrelated\n"
        with self.assertRaises(avatar.Stop):
            avatar.make_avatar(None, "s3", fenced(GOOD_B))
        self.assertEqual(self.active_text(), GOOD_A)


class SameConfiguration(unittest.TestCase):
    """The candidate's firmware is configured as your build for that board was, or, with no
    build of yours yet, from the defaults as a first build would be."""

    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.tmp)
        self.root = Path(self.tmp, "esp32")
        (self.root / "tools/muse").mkdir(parents=True)
        shutil.copyfile(ROOT / "tools/muse/board.sh", self.root / "tools/muse/board.sh")
        (self.root / "components/muse").mkdir(parents=True)
        self.src = Path(self.tmp, "muse_pixel.c")
        self.src.write_text(GOOD_B)
        self.seen = {}
        for name, value in (("ROOT", str(self.root)), ("AVATAR_DIR", str(self.root / "components/muse/avatar")),
                            ("say", lambda msg: None), ("board_sh", self.board_sh)):
            patcher = mock.patch.object(avatar, name, value)
            patcher.start()
            self.addCleanup(patcher.stop)

    def board_sh(self, *args, root=None):
        cfg = Path(root, self.expect_dir, "sdkconfig")
        self.seen["sdkconfig"] = cfg.read_bytes() if cfg.exists() else None
        return True, "built"

    def mine(self, build_dir):
        cfg = self.root / build_dir / "sdkconfig"
        cfg.parent.mkdir()
        cfg.write_bytes(b'CONFIG_GADGET_SDK_TOKEN="test-token-not-real"\nCONFIG_SOME_OPTION=y\n')
        return cfg.read_bytes()

    def test_it_builds_with_your_configuration(self):
        self.expect_dir = "build-muse-waveshare-s3-175c"
        yours = self.mine(self.expect_dir)
        avatar.build_candidate(str(self.src), "s3", os.path.join(self.tmp, "work"))
        self.assertEqual(self.seen["sdkconfig"], yours)   # byte for byte

    def test_the_bench_build_has_its_own(self):
        self.expect_dir = "build-muse-waveshare-c6-18-bench"
        self.mine("build-muse-waveshare-c6-18")         # not this one
        yours = self.mine(self.expect_dir)
        with mock.patch.dict(os.environ, {"MUSE_BENCH": "1"}):
            avatar.build_candidate(str(self.src), "c6", os.path.join(self.tmp, "work"))
        self.assertEqual(self.seen["sdkconfig"], yours)

    def test_without_a_build_of_yours_it_starts_from_the_defaults(self):
        self.expect_dir = "build-muse-waveshare-s3-175c"
        avatar.build_candidate(str(self.src), "s3", os.path.join(self.tmp, "work"))
        self.assertIsNone(self.seen["sdkconfig"])

    def test_an_unknown_board_stops(self):
        with self.assertRaises(avatar.Stop):
            avatar.build_dir("not-a-board")


class TreeCopy(unittest.TestCase):
    def test_the_copy_leaves_out_builds_downloads_and_your_avatar(self):
        with tempfile.TemporaryDirectory() as tmp:
            src = Path(tmp, "esp32")
            for rel in ("main/app.c", "components/muse/muse_ui.c", "build-muse-x/app.bin", "managed_components/c/x",
                        "dependencies.lock", "components/muse/avatar/muse_pixel.c",
                        "components/muse/avatar/muse_pixel.c.prev"):
                Path(src, rel).parent.mkdir(parents=True, exist_ok=True)
                Path(src, rel).write_text(rel)
            with mock.patch.object(avatar, "ROOT", str(src)), \
                    mock.patch.object(avatar, "AVATAR_DIR", os.path.join(tmp, "elsewhere")):
                dest = avatar.copy_tree(os.path.join(tmp, "copy"))
            got = sorted(str(p.relative_to(dest)) for p in Path(dest).rglob("*") if p.is_file())
            self.assertEqual(got, ["components/muse/muse_ui.c", "main/app.c"])


DRIVER = textwrap.dedent("""
    import os, sys
    sys.path.insert(0, sys.argv[1]); sys.path.insert(0, sys.argv[2])
    import avatar, test_avatar_install as t
    from unittest import mock
    d = sys.argv[3]
    crash_at, kind = int(sys.argv[4]), sys.argv[5]
    calls = [0]
    real = {"replace": os.replace, "fsync": os.fsync}[kind]
    def hit(*a, **k):
        calls[0] += 1
        if calls[0] == crash_at:
            os._exit(70)          # the process dies here: no cleanup runs
        return real(*a, **k)
    with mock.patch.object(avatar, "AVATAR_DIR", d), \\
            mock.patch.object(avatar, "AVATAR_SRC", os.path.join(d, "muse_pixel.c")), \\
            mock.patch.object(avatar, "LAST_REPLY", os.path.join(d, "last_reply.md")), \\
            mock.patch.object(avatar, "say", lambda m: None), mock.patch.object(os, kind, hit):
        avatar.make_avatar(None, None, t.fenced(t.GOOD_B))
""")


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


class KeptBuild(unittest.TestCase):
    """The firmware checked with your new avatar stays until it's flashed, and only it is flashed."""

    def setUp(self):
        Installing.setUp(self)
        self.calls, self.result, self.log = [], (True, "built"), ""
        self.out = []
        say = mock.patch.object(avatar, "say", self.out.append)
        say.start()
        self.addCleanup(say.stop)
        for name, value in (("pick_port", lambda: "PORT0"), ("Board", self.no_status)):
            p = mock.patch.object(avatar.chat, name, value)
            p.start()
            self.addCleanup(p.stop)
        for name, value in (("open_board", self.no_answer), ("time", mock.Mock()), ("board_sh", self.board_sh)):
            p = mock.patch.object(avatar, name, value)
            p.start()
            self.addCleanup(p.stop)
        self.reply = os.path.join(self.tmp, "reply.md")
        Path(self.reply).write_text(fenced(GOOD_B))

    board_sh = FirmwareBuild.board_sh
    image = True
    flash_result = (True, "flashed")
    active_text = Installing.active_text

    @staticmethod
    def no_answer(port):
        raise avatar.Stop("no answer")

    @staticmethod
    def no_status(port):
        raise avatar.chat.BoardError("no answer")

    def run_tool(self, *argv):
        ap = argparse.Namespace(port=None, board="s3", edit=None, reply=self.reply, no_flash=False)
        for a in argv:
            setattr(ap, a, True)
        avatar.run(ap)

    def test_no_flash_keeps_the_checked_build_and_says_how_to_flash_it(self):
        self.run_tool("no_flash")
        kept = avatar.kept_dir("s3")
        self.assertTrue(os.path.isdir(kept))
        said = "\n".join(self.out)
        self.assertIn(avatar.flash_command("s3", None, kept), said)
        self.assertIn(os.path.join(avatar.rel(kept), "tools", "muse", "board.sh") + " flash s3", said)
        self.assertIn("rm -r " + avatar.rel(kept), said)

    def test_the_kept_build_has_your_avatar_and_its_image(self):
        self.run_tool("no_flash")
        kept = avatar.kept_dir("s3")
        record = dict(l.split(" ", 1) for l in Path(kept, "VERIFIED").read_text().splitlines())
        self.assertEqual(record["renderer_sha256"], sha(self.active))
        self.assertEqual(record["renderer_sha256"], sha(os.path.join(kept, "components", "muse", "avatar",
                                                                      "muse_pixel.c")))
        self.assertEqual(record["image_sha256"], sha(avatar.image_path("s3", kept)))
        self.assertEqual(avatar.verify_kept("s3"), kept)

    def test_a_build_without_an_image_is_not_called_checked(self):
        self.image = False
        with self.assertRaises(avatar.Stop):
            self.run_tool("no_flash")
        self.assertEqual(self.active_text(), GOOD_A)
        self.assertFalse(os.path.exists(avatar.kept_dir("s3")))

    def test_a_later_failing_candidate_leaves_the_kept_build(self):
        self.run_tool("no_flash")
        kept = avatar.kept_dir("s3")
        before = Path(kept, "VERIFIED").read_text()
        Path(self.reply).write_text(fenced(NO_BUILD))
        with self.assertRaises(avatar.Stop):
            self.run_tool("no_flash")
        self.result = (False, "failed")
        self.log = "main/app.c:1:1: error: unrelated\n"
        Path(self.reply).write_text(fenced(GOOD_A))
        with self.assertRaises(avatar.Stop):
            self.run_tool("no_flash")
        self.assertEqual(Path(kept, "VERIFIED").read_text(), before)
        self.assertEqual(avatar.verify_kept("s3"), kept)

    def test_a_new_avatar_without_a_build_drops_the_old_build(self):
        self.run_tool("no_flash")
        Path(self.reply).write_text(fenced(GOOD_A))
        avatar.run(argparse.Namespace(port=None, board=None, edit=None, reply=self.reply, no_flash=True))
        self.assertFalse(os.path.exists(avatar.kept_dir("s3")))

    def test_another_boards_build_of_the_same_avatar_stays(self):
        self.run_tool("no_flash")
        avatar.run(argparse.Namespace(port=None, board="c6", edit=None, reply=self.reply, no_flash=True))
        self.assertEqual(avatar.verify_kept("s3"), avatar.kept_dir("s3"))
        self.assertEqual(avatar.verify_kept("c6"), avatar.kept_dir("c6"))

    def test_a_changed_image_is_not_flashed(self):
        self.run_tool("no_flash")
        with open(avatar.image_path("s3", avatar.kept_dir("s3")), "a") as f:
            f.write("x")
        with self.assertRaises(avatar.Stop):
            avatar.verify_kept("s3")

    def test_a_changed_avatar_is_not_flashed(self):
        self.run_tool("no_flash")
        Path(self.active).write_text(GOOD_A)
        with self.assertRaises(avatar.Stop):
            avatar.verify_kept("s3")

    def test_it_flashes_the_kept_build_and_then_removes_it(self):
        self.run_tool()
        kept = avatar.kept_dir("s3")
        flashes = [(args, root) for args, root, _ in self.calls if args[0] == "flash"]
        self.assertEqual(flashes, [(("flash", "s3", "PORT0"), kept)])
        self.assertFalse(os.path.exists(kept))

    def test_a_failed_flash_keeps_the_build_and_points_at_it(self):
        self.flash_result = (False, "port busy")
        with self.assertRaises(avatar.Stop) as stop:
            self.run_tool()
        kept = avatar.kept_dir("s3")
        self.assertTrue(os.path.isdir(kept))
        self.assertIn(avatar.flash_command("s3", "PORT0", kept), str(stop.exception))
        self.assertNotIn("run: tools/muse/board.sh", str(stop.exception))


class Interrupted(unittest.TestCase):
    """A run killed at any write leaves your avatar whole, and the next run carries on."""

    def run_until(self, kind):
        outcomes = []
        for crash_at in range(1, 40):
            with tempfile.TemporaryDirectory() as d:
                active = Path(d, "muse_pixel.c")
                active.write_text(GOOD_A)
                p = subprocess.run([sys.executable, "-c", DRIVER, str(ROOT / "tools" / "muse"), str(ROOT / "tests"),
                                    d, str(crash_at), kind], capture_output=True, text=True, timeout=120)
                self.assertIn(p.returncode, (0, 70), p.stderr)
                now = active.read_text().strip()
                self.assertIn(now, (GOOD_A.strip(), GOOD_B.strip()))           # whole, old or new
                prev = Path(d, "muse_pixel.c.prev")
                if now == GOOD_B.strip():
                    self.assertEqual(prev.read_text().strip(), GOOD_A.strip())  # the old one is kept
                with mock.patch.object(avatar, "AVATAR_DIR", d):
                    avatar.recover()
                self.assertEqual([n for n in os.listdir(d) if n.endswith(avatar.TMP_SUFFIX)], [])
                outcomes.append(now == GOOD_B.strip())
                if p.returncode == 0:
                    return outcomes
        self.fail("never finished")

    def test_killed_at_any_rename(self):
        outcomes = self.run_until("replace")
        self.assertGreater(len(outcomes), 2)
        self.assertTrue(outcomes[-1])

    def test_killed_at_any_sync(self):
        outcomes = self.run_until("fsync")
        self.assertGreater(len(outcomes), 2)
        self.assertTrue(outcomes[-1])


class OneAtATime(unittest.TestCase):
    def test_a_second_run_stops_while_the_first_holds_the_lock(self):
        with tempfile.TemporaryDirectory() as d:
            holder = subprocess.Popen([sys.executable, "-c", textwrap.dedent(f"""
                import fcntl, os, sys, time
                fd = os.open({os.path.join(d, avatar.LOCK)!r}, os.O_CREAT | os.O_RDWR)
                fcntl.flock(fd, fcntl.LOCK_EX)
                print("held", flush=True)
                time.sleep(30)
            """)], stdout=subprocess.PIPE, text=True)
            try:
                self.assertEqual(holder.stdout.readline().strip(), "held")
                with mock.patch.object(avatar, "AVATAR_DIR", d):
                    with self.assertRaises(avatar.Stop):
                        with avatar.lock():
                            pass
            finally:
                holder.kill()
                holder.wait()
                holder.stdout.close()
            with mock.patch.object(avatar, "AVATAR_DIR", d):
                with avatar.lock():   # free once its holder is gone, even killed
                    pass


if __name__ == "__main__":
    unittest.main()
