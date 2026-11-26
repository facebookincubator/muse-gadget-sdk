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

"""Confirming Muse Link setup over the serial console: tools/mg_confirm.py against a
simulated console on a pty, and the firmware's side of the contract on both SDKs."""

from __future__ import annotations

import os
import re
import select
import sys
import tempfile
import threading
import time
import tty
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import mg_confirm  # noqa: E402

ZEPHYR = ROOT.parent / "zephyr"


class Board(threading.Thread):
    """A gadget's console on the far side of a pty: prints `lines`, then answers each
    ">pair.confirm" with the next of `answers` ("confirmed" or "none")."""

    def __init__(self, master: int, lines: list[str], answers: list[str]):
        super().__init__(daemon=True)
        self.master, self.answers, self.commands = master, list(answers), []
        self.stop = False
        os.write(master, "".join(f"{ln}\n" for ln in lines).encode())

    def run(self) -> None:
        buf = b""
        while not self.stop:
            if not select.select([self.master], [], [], 0.05)[0]:
                continue
            try:
                buf += os.read(self.master, 256)
            except OSError:
                return
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                self.commands.append(line)
                if line == b">pair.confirm":
                    answer = self.answers.pop(0) if self.answers else "none"
                    os.write(self.master, f"I (1) noise\n@pair.confirm {answer}\n".encode())


class MgConfirmTest(unittest.TestCase):
    def setUp(self) -> None:
        self.master, slave = os.openpty()
        tty.setraw(slave)   # a board's console doesn't echo
        self.port = os.ttyname(slave)
        self.addCleanup(os.close, slave)   # keeps the pty open while the tool reopens it
        self.addCleanup(os.close, self.master)

    def board(self, lines, answers) -> Board:
        b = Board(self.master, lines, answers)
        b.start()

        def stop():
            b.stop = True
            b.join(2)

        self.addCleanup(stop)   # before the pty closes
        return b

    def test_watch_answers_the_pending_confirmation(self) -> None:
        b = self.board(["I (10) link.app: setup stage: advertising -> confirm", "@pair.pending"], ["confirmed"])
        out = []
        self.assertEqual(mg_confirm.watch(self.port, 1, 5.0, out=out.append), 0)
        self.assertEqual(b.commands, [b">pair.confirm"])
        self.assertEqual(out, ["confirmed setup (1)"])

    def test_watch_sends_nothing_without_a_pending_confirmation(self) -> None:
        b = self.board(["I (10) boot", "bench: keys d (talk down)"], [])
        self.assertEqual(mg_confirm.watch(self.port, 1, 0.6, out=lambda _: None), 1)
        self.assertEqual(b.commands, [])

    def test_watch_reports_a_confirmation_that_did_not_take(self) -> None:
        self.board(["@pair.pending"], ["none"])
        out = []
        self.assertEqual(mg_confirm.watch(self.port, 1, 1.0, out=out.append), 1)
        self.assertIn("nothing pending", out[0])

    def test_a_shipping_build_refuses(self) -> None:
        # Gadget setup v2: only bench builds take the console's press.
        self.board(["@pair.pending"], ["disabled", "disabled"])
        out = []
        self.assertEqual(mg_confirm.watch(self.port, 1, 3.0, out=out.append), 2)
        self.assertIn("MUSE_BENCH=1", out[0])
        with self.assertRaises(mg_confirm.ConfirmDisabled):
            mg_confirm.confirm_now(self.port, tries=2.0)

    def test_console_secrets_never_reach_the_log_or_the_screen(self) -> None:
        # A console line from older firmware carrying an SDK token (or any record
        # with one): the saved log and the echo mask it (tools/redact.py), and the
        # log is private.
        fake = "mgst_FAKEfakeFAKEfake0123456789abcdefghijklmnopq"
        self.board([f"I (12) link.pairing: pairing_confirmed carries SDK token {fake}",
                    '{"type":"status","status":"pairing_confirmed","sdk_token":"' + fake + '"}',
                    "@pair.pending"], ["confirmed"])
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        path = Path(tmp.name) / "console.log"
        echoed = []
        console = mg_confirm.Console(self.port, str(path), echo=echoed.append)
        try:
            end = time.monotonic() + 3.0
            while time.monotonic() < end and not any(b"@pair.pending" in ln for ln in echoed):
                console.read_lines()
        finally:
            console.close()
        text = path.read_bytes() + b"".join(echoed)
        self.assertIn(b"@pair.pending", text)
        self.assertIn(b"<redacted: 48 chars, sha256 ", text)
        for i in range(5, len(fake) - 6):
            self.assertNotIn(fake[i:i + 6].encode(), text)
        self.assertEqual(path.stat().st_mode & 0o777, 0o600)


class FirmwareContractTest(unittest.TestCase):
    """Both SDKs print the same marker and take the same line, and the command is the
    talk button's setup path, never push-to-talk."""

    def test_esp32_console(self) -> None:
        src = (ROOT / "components" / "muse" / "muse_input.c").read_text()
        body = re.search(r'if \(!strcmp\(line, "pair\.confirm"\)\) \{(.*?)\n    \}', src, re.S)
        self.assertIsNotNone(body, "muse_input.c takes >pair.confirm")
        self.assertIn("muse_link_bench_confirm()", body.group(1))
        # The bench's confirmation is the talk button's setup press (or a re-setup's hold).
        link = (ROOT / "components" / "muse" / "muse_link.c").read_text()
        bench = re.search(r"bool muse_link_bench_confirm\(void\)\n\{(.*?)\n\}", link, re.S)
        self.assertIsNotNone(bench)
        self.assertIn("return muse_link_talk_press();", bench.group(1))
        self.assertNotIn("post(", body.group(1))
        self.assertIn('"@pair.confirm %s\\n", confirmed ? "confirmed" : "none"', body.group(1))
        self.assertIn('printf("@pair.pending\\n")', src)
        # A simulated press only in bench builds (the Muse Gadget setup
        # contract, the button): the option is off by default and on only in
        # the bench overlay.
        gated = body.group(1).split("#else")
        self.assertEqual(len(gated), 2, "the press sits behind CONFIG_MUSE_PAIR_CONFIRM_CONSOLE")
        self.assertIn("#if CONFIG_MUSE_PAIR_CONFIRM_CONSOLE", body.group(1).split("muse_link_bench_confirm()")[0])
        self.assertNotIn("muse_link_bench_confirm()", gated[1])
        self.assertIn('"@pair.confirm disabled\\n"', gated[1])
        kconfig = (ROOT / "components" / "muse" / "Kconfig").read_text()
        option = re.search(r"config MUSE_PAIR_CONFIRM_CONSOLE\n(.*?)\n\n", kconfig, re.S)
        self.assertIsNotNone(option)
        self.assertIn("default n", option.group(1))
        overlays = sorted(p.name for p in (ROOT / "devices").glob("sdkconfig.*")
                          if "CONFIG_MUSE_PAIR_CONFIRM_CONSOLE=y" in p.read_text())
        self.assertEqual(overlays, ["sdkconfig.muse-bench"])
        for defaults in ROOT.glob("sdkconfig.defaults*"):
            self.assertNotIn("CONFIG_MUSE_PAIR_CONFIRM_CONSOLE=y", defaults.read_text())
        glue = (ROOT / "main" / "muse_glue.c").read_text()
        self.assertRegex(glue, r"op_talk_press\(int64_t pressed_after_us, bool talk_key\) \{\s+"
                               r"return app_confirm_pairing_press\(pressed_after_us, talk_key\);")

    def test_zephyr_bench(self) -> None:
        # The automation tools: one switch, CONFIG_MG_AUTOMATION, off by
        # default (release builds) and on in overlay-bench.conf (lab and CI).
        # The bench console (CONFIG_MG_BENCH) is in every build; its
        # >pair.confirm presses only with automation, else answers disabled
        # (a shipping build is confirmed with the button alone, as the ESP32).
        kconfig = (ZEPHYR / "Kconfig.mg").read_text()
        auto = re.search(r"config MG_AUTOMATION\n(.*?)\n\n", kconfig, re.S)
        self.assertIsNotNone(auto)
        self.assertNotIn("default y", auto.group(1))
        self.assertIn("CONFIG_MG_AUTOMATION=y", (ZEPHYR / "overlay-bench.conf").read_text())
        option = re.search(r"config MG_BENCH\n(.*?)\n\n", kconfig, re.S)
        self.assertIsNotNone(option)
        self.assertNotIn("depends on MG_AUTOMATION", option.group(1))
        self.assertIn("default y", option.group(1))
        self.assertRegex((ZEPHYR / "CMakeLists.txt").read_text(), r"CONFIG_MG_BENCH[^\n]*mg_bench\.c")
        bench = (ZEPHYR / "src" / "mg_bench.c").read_text()
        self.assertIn('strcmp(line, "pair.confirm")', bench)
        # Automation, or bench mode (CONFIG_MG_BENCH_MODE: a never-shipped image).
        press = re.search(r"#if defined\(CONFIG_MG_AUTOMATION\)(?: \|\| defined\(CONFIG_MG_BENCH_MODE\))?\n"
                          r"(.*?)#else\n(.*?)#endif", bench, re.S)
        self.assertIsNotNone(press, "the press sits behind CONFIG_MG_AUTOMATION")
        self.assertIn("mg_setup_confirm_pending() && mg_setup_button(false)", press.group(1))   # injected: never real
        self.assertIn('"@pair.confirm %s\\n", confirmed ? "confirmed" : "none"', press.group(1))
        self.assertNotIn("mg_setup_button(", press.group(2))
        self.assertIn('"@pair.confirm disabled\\n"', press.group(2))
        self.assertIn("k_work_submit_to_queue(mg_app_wq(), &confirm_work)", bench)
        setup = (ZEPHYR / "src" / "mg_setup.c").read_text()
        self.assertRegex(setup, r'#if defined\(CONFIG_MG_AUTOMATION\)[^#]*printk\("@pair\.pending\\n"\);')


if __name__ == "__main__":
    unittest.main()
