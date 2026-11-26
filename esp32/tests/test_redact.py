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

"""No token characters in what the firmware logs or the bench tools print.

tools/redact.py masks secrets in records and device output; every tool that
prints a protocol record or a gadget's console/log output goes through it, and
the firmware logs a token's length and SHA-256 prefix only (test_token_logs,
AGENTS.md, Muse Gadgets SDK token)."""

from __future__ import annotations

import asyncio
import contextlib
import hashlib
import io
import json
import re
import sys
import types
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import redact  # noqa: E402

FAKE = "mgst_FAKEfakeFAKEfake0123456789abcdefghijklmnopq"   # made up: never a real token
ACCESS = "AcXq7Zk2Pq9Lm4Rt8Wv1Yb3NuQ5sD6fG"
REFRESH = "RfZt5Hj8Kd2Qw6Er9Ty0Ui4OpA7sS1dF3"


class RedactTest(unittest.TestCase):
    def assert_clean(self, text: str | bytes) -> None:
        """None of a secret's characters: no 6-character piece of any of them."""
        if isinstance(text, bytes):
            text = text.decode("utf-8", "replace")
        for secret in (FAKE[5:], ACCESS, REFRESH, "hunter2wifi"):
            for i in range(len(secret) - 5):
                self.assertNotIn(secret[i:i + 6], text, f"a piece of a secret in: {text[:300]}")

    def test_fingerprint(self) -> None:
        self.assertEqual(redact.fingerprint(None), "none")
        self.assertEqual(redact.fingerprint(""), "none")
        self.assertEqual(redact.fingerprint("abc"), "3 chars, sha256 ba7816bf")
        self.assertEqual(redact.fingerprint(FAKE),
                         f"48 chars, sha256 {hashlib.sha256(FAKE.encode()).hexdigest()[:8]}")

    def test_records(self) -> None:
        # The Link setup records that carry secrets, as the bench tools see them.
        confirmed = {"type": "status", "status": "pairing_confirmed", "sdk_token": FAKE}
        provision = {"action": "provision_v2", "access_token": ACCESS, "refresh_token": REFRESH,
                     "token_type": "device", "ssid": "home", "password": "hunter2wifi"}
        nested = {"type": "pairing_encrypted", "inner": [confirmed, provision], "note": f"sdk {FAKE}"}
        for rec in (confirmed, provision, nested):
            for shown in (redact.record_text(rec), json.dumps(redact.redact_record(rec)),
                          redact.redact_text(json.dumps(rec)), redact.redact_text(repr(rec)),
                          redact.redact_text(str(rec))):
                self.assert_clean(shown)
        out = redact.redact_record(provision)
        self.assertEqual(out["token_type"], "device")   # the rest stays readable
        self.assertEqual(out["ssid"], "home")
        self.assertTrue(out["access_token"].startswith("<redacted: 32 chars, sha256 "))
        self.assertEqual(redact.redact_record({"token": True})["token"], True)   # status flags stay

    def test_log_lines(self) -> None:
        for line in (f"I (123) link.pairing: pairing_confirmed carries SDK token {FAKE[:12]}",
                     f"I (9) sdk_token: stored SDK token {FAKE}, restarting",
                     f">sdk_token={FAKE}", f"hatch.token={ACCESS}",
                     f"Authorization: Bearer {ACCESS}", f"Bearer hatch_refresh:{REFRESH}",
                     f'"sdk_token":"{FAKE}"'):
            self.assert_clean(redact.redact_text(line))
            self.assert_clean(redact.redact_bytes(line.encode()))
        self.assertEqual(redact.redact_text("I (1) mg.face: face thinking"), "I (1) mg.face: face thinking")

    def test_a_secret_split_across_pieces(self) -> None:
        lines = redact.LineRedactor()
        out = lines.feed(f"I (1) x: {FAKE[:20]}") + lines.feed(f"{FAKE[20:]}\nnext ") + lines.feed("line") \
            + lines.flush()
        self.assert_clean(out)
        self.assertTrue(out.endswith("next line"))


class ClientOutputTest(unittest.TestCase):
    """tools/mg_ble_client.py: records in errors, NUS log lines, `log`, the SDK token line."""

    @classmethod
    def setUpClass(cls) -> None:
        if not (ROOT / "tools/mg_ble_client.py").exists():
            raise unittest.SkipTest("no bench client on this branch")
        import mg_ble_client
        cls.C = mg_ble_client

    def test_mask_token_shows_no_characters(self) -> None:
        self.assertEqual(self.C.mask_token(FAKE), redact.fingerprint(FAKE))
        self.assertEqual(self.C.mask_token(""), "none")

    def test_every_record_in_an_error_is_redacted(self) -> None:
        # Link setup's errors name the record they didn't expect: through redact.
        src = (ROOT / "tools/mg_ble_client.py").read_text()
        raw = re.findall(r'raise \w+Error\(f"[^"]*\{(ready|msg|info|st|plain|status)\}', src)
        self.assertEqual(raw, [], "a Link setup record formatted into an error without redact")
        self.assertGreaterEqual(src.count("redact.record_text("), 6)

    def test_nus_log_lines(self) -> None:
        g = self.C.Gadget(client=None, verbose=False)
        if not hasattr(g, "nus_watch"):
            self.skipTest("this client prints no NUS log lines")
        g.nus_watch = re.compile("link")
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            g._on_nus(None, bytearray(f"I (5) link.pairing: carries SDK token {FAKE[:12]}\n".encode()))
        self.assertIn("log:", out.getvalue())
        RedactTest.assert_clean(self, out.getvalue())

    def test_log_command(self) -> None:
        # The NUS mirror arrives in notifications; a token split across two is caught.
        chunks = [f"I (7) app: token {FAKE[:15]}".encode(), f"{FAKE[15:]} ok\nI (8) mg.face".encode(),
                  b": face idle\n"]

        class FakeClient:
            def __init__(self, *_a, **_k):
                pass

            async def __aenter__(self):
                return self

            async def __aexit__(self, *exc):
                return False

            async def start_notify(self, _uuid, cb):
                for c in chunks:
                    cb(None, bytearray(c))

            async def write_gatt_char(self, *_a, **_k):
                pass

        async def pick(_args):
            return "AA:BB", "MuseGadget-AABBCC"

        saved_bleak, saved_pick = sys.modules.get("bleak"), self.C.pick
        sys.modules["bleak"] = types.SimpleNamespace(BleakClient=FakeClient)
        self.C.pick = pick
        out = io.StringIO()
        try:
            with contextlib.redirect_stdout(out):
                asyncio.run(self.C.cmd_log(types.SimpleNamespace(secs=0, command=None)))
        finally:
            self.C.pick = saved_pick
            if saved_bleak is None:
                del sys.modules["bleak"]
            else:
                sys.modules["bleak"] = saved_bleak
        RedactTest.assert_clean(self, out.getvalue())
        self.assertIn("mg.face: face idle", out.getvalue())


class ToolsContractTest(unittest.TestCase):
    def test_console_tools_go_through_redact(self) -> None:
        # The tools that print a gadget's console or status output.
        for rel, needle in (("tools/muse/monitor.py", "redact.redact_text("),
                            ("tools/muse/chat.py", "redact.redact_record(st)"),
                            ("tools/muse/iris_bench.py", "redact.redact_record("),
                            ("tools/mg_confirm.py", "redact.redact_bytes("),
                            ("tools/mg_ble_client.py", "redact.LineRedactor()")):
            with self.subTest(tool=rel):
                if (ROOT / rel).exists():   # not every branch has every tool
                    self.assertIn(needle, (ROOT / rel).read_text())


if __name__ == "__main__":
    unittest.main()
