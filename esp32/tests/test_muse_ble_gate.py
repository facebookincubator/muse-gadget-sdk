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
"""Muse's phone setup service takes Wi-Fi and account writes only while the
gadget isn't set up, or in the window the user opens:
components/muse/muse_ble_gate.c on the host, and its wiring."""

from __future__ import annotations

import os
import shlex
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MUSE = ROOT / "components" / "muse"

DRIVER = r"""
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "muse_ble_gate.h"
int main(int argc, char **argv)
{
    if (argc == 5 && !strcmp(argv[1], "link")) {   /* link encrypted authenticated secure_connections */
        printf("%d\n", muse_ble_gate_link_ok(atoi(argv[2]), atoi(argv[3]), atoi(argv[4])));
        return 0;
    }
    if (argc == 3) {   /* cmd test_build: in this build at all */
        printf("%d\n", muse_ble_gate_in_build(argv[1], atoi(argv[2])));
        return 0;
    }
    if (argc != 5) return 2;   /* cmd set_up now_us window_until_us */
    printf("%d\n", muse_ble_gate_allows(argv[1], atoi(argv[2]), atoll(argv[3]), atoll(argv[4])));
    return 0;
}
"""

WIN = 10 * 60 * 1000 * 1000
PROVISIONING = ["wifi.ssid", "wifi.pass", "wifi.connect", "wifi.forget", "hatch.host", "hatch.vm", "hatch.token",
                "hatch.token+"]


class MuseBleGate(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.tmp = tempfile.TemporaryDirectory()
        d = Path(cls.tmp.name)
        (d / "driver.c").write_text(DRIVER)
        cls.exe = d / "gate"
        cc = shlex.split(os.environ.get("CC", "cc"))
        subprocess.run(cc + ["-std=c11", "-Wall", "-Wextra", "-Werror", "-I", str(MUSE), str(d / "driver.c"),
                             str(MUSE / "muse_ble_gate.c"), "-o", str(cls.exe)], check=True)

    @classmethod
    def tearDownClass(cls) -> None:
        cls.tmp.cleanup()

    def allows(self, cmd: str, set_up: bool, now: int, until: int) -> bool:
        out = subprocess.run([str(self.exe), cmd, str(int(set_up)), str(now), str(until)], capture_output=True,
                             text=True, check=True).stdout.strip()
        return out == "1"

    def in_build(self, cmd: str, test_build: bool) -> bool:
        out = subprocess.run([str(self.exe), cmd, str(int(test_build))], capture_output=True, text=True,
                             check=True).stdout.strip()
        return out == "1"

    def test_a_release_build_takes_no_provisioning_write(self) -> None:
        for cmd in PROVISIONING:
            self.assertFalse(self.in_build(cmd, False), cmd)
            self.assertTrue(self.in_build(cmd, True), cmd)   # a test build: the window-gated path
        for cmd in ("volume", "speaker", "mic_gain", "brightness", "sleep", "hatch.test", "test.loopback"):
            self.assertTrue(self.in_build(cmd, False), cmd)
        ble = (MUSE / "muse_ble.c").read_text()
        run = ble[ble.index("static void run_command(char *cmd, muse_line_origin_t origin)"):]
        run = run[:run.index("\n}\n")]
        self.assertIn("#if CONFIG_MUSE_GADGET_TEST_BUILD\n    const bool test_build = true;\n#else\n"
                      "    const bool test_build = false;\n#endif", run)
        self.assertLess(run.index("if (wireless && !muse_ble_gate_in_build(cmd, test_build)) {"),
                        run.index("} else if (!part && wireless && !muse_ble_gate_allows(cmd, was_set_up, "
                                  "esp_timer_get_time(), s_window_until_us)) {"))
        self.assertIn('res = "not in this build";', run)
        # Test builds are the bench ones.
        bench = (ROOT / "devices" / "sdkconfig.muse-bench").read_text()
        self.assertIn("\nCONFIG_MUSE_GADGET_TEST_BUILD=y\n", bench)
        kconfig = (ROOT / "main" / "Kconfig.projbuild").read_text()
        block = kconfig[kconfig.index("config MUSE_GADGET_TEST_BUILD"):]
        self.assertIn("default n", block[:block.index("help")])
        for overlay in (ROOT / "devices").glob("sdkconfig.muse*"):
            if overlay.name != "sdkconfig.muse-bench":
                self.assertNotIn("CONFIG_MUSE_GADGET_TEST_BUILD=y", overlay.read_text(), overlay.name)
        # Link setup's own Wi-Fi never goes through these commands.
        for f in (ROOT / "main").glob("*.c"):
            self.assertNotIn("muse_ble_gate", f.read_text(), f.name)

    def test_the_wired_console_skips_the_window(self) -> None:
        # A release build refuses Wi-Fi and account writes from a wireless origin only; a wired console's line runs
        # with no window, set up or not, bench and release alike (the console is fully open: an automated run never
        # waits on a person). The window gate holds for every wireless line. The entry point names the origin.
        origin = (MUSE / "muse_line_origin.h").read_text()
        self.assertIn("MUSE_LINE_WIRELESS = 0,", origin)
        self.assertIn('_Static_assert(MUSE_LINE_WIRELESS == 0, "an unknown origin must be wireless (fail closed)");',
                      origin)
        ble = (MUSE / "muse_ble.c").read_text()
        console = ble[ble.index("void muse_ble_command(char *cmd, muse_line_origin_t origin)"):]
        self.assertTrue(console.startswith("void muse_ble_command(char *cmd, muse_line_origin_t origin)\n{\n"
                                           "    run_command(cmd, origin);\n}"))
        run = ble[ble.index("static void run_command(char *cmd, muse_line_origin_t origin)"):]
        run = run[:run.index("\n}\n")]
        self.assertIn("bool wireless = origin != MUSE_LINE_WIRED;", run)
        self.assertIn("if (wireless && !muse_ble_gate_in_build(cmd, test_build)) {", run)
        self.assertIn("} else if (!part && wireless && !muse_ble_gate_allows(cmd, was_set_up, esp_timer_get_time(), "
                      "s_window_until_us)) {", run)   # wireless lines only
        # The GATT write is wireless; only the console port's serial task says wired; ESP-Iris's bench line is
        # wireless (an RPC, settings only).
        self.assertEqual(ble.count("run_command(cmd, MUSE_LINE_WIRELESS);"), 1)
        write = ble[ble.index("if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {"):]
        self.assertIn("run_command(cmd, MUSE_LINE_WIRELESS);", write[:write.index("return rc == 0")])
        inp = (MUSE / "muse_input.c").read_text()
        self.assertEqual(inp.count("muse_ble_command("), 1)
        serial = inp[inp.index("static void serial_task(void *arg)"):]
        self.assertIn("muse_ble_command(line, MUSE_LINE_WIRED);", serial[:serial.index("\n}\n")])
        iris = (ROOT / "components_mosaico/muse_mosaico_iris/muse_mosaico_iris.c").read_text()
        self.assertIn("muse_ble_command(line, MUSE_LINE_WIRELESS);", iris)
        self.assertNotIn("MUSE_LINE_WIRED", iris)

    def test_writes_need_a_secure_connections_bond_made_with_the_code(self) -> None:
        def ok(enc: int, auth: int, sc: int) -> bool:
            return subprocess.run([str(self.exe), "link", str(enc), str(auth), str(sc)], capture_output=True,
                                  text=True, check=True).stdout.strip() == "1"
        self.assertTrue(ok(1, 1, 1))
        for row in ((1, 1, 0), (1, 0, 1), (0, 1, 1), (0, 0, 0), (1, 0, 0)):
            self.assertFalse(ok(*row), row)   # a legacy pairing's keys, no passkey, no encryption
        ble = (MUSE / "muse_ble.c").read_text()
        write = ble[ble.index("if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {"):]
        self.assertTrue(write.startswith("if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {\n        if (!link_ok(conn)) {"))
        self.assertLess(write.index("return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;"), write.index("run_command(cmd, MUSE_LINE_WIRELESS);"))
        link = ble[ble.index("static bool link_ok(uint16_t conn)"):]
        link = link[:link.index("\n}\n")]
        self.assertIn("muse_ble_gate_link_ok(d.sec_state.encrypted, d.sec_state.authenticated, "
                      "bond_is_sc(&d.peer_id_addr))", link)
        bond = ble[ble.index("static bool bond_is_sc(const ble_addr_t *peer)"):]
        bond = bond[:bond.index("\n}\n")]
        self.assertIn("ble_store_read_peer_sec(&key, &v) == 0 && v.sc", bond)
        # The pairing itself asks for Secure Connections with MITM; the store record checks what was made.
        self.assertIn("ble_hs_cfg.sm_sc = 1;", ble)
        self.assertIn("ble_hs_cfg.sm_mitm = 1;", ble)

    def test_rejected_when_set_up(self) -> None:
        for cmd in PROVISIONING:
            self.assertFalse(self.allows(cmd, True, 5_000_000, 0), cmd)

    def test_accepted_when_not_set_up(self) -> None:
        for cmd in PROVISIONING:
            self.assertTrue(self.allows(cmd, False, 5_000_000, 0), cmd)

    def test_accepted_in_the_users_window_and_not_after(self) -> None:
        opened = 100_000_000
        for cmd in PROVISIONING:
            self.assertTrue(self.allows(cmd, True, opened + 1, opened + WIN), cmd)
            self.assertTrue(self.allows(cmd, True, opened + WIN - 1, opened + WIN), cmd)
            self.assertFalse(self.allows(cmd, True, opened + WIN, opened + WIN), cmd)

    def test_other_commands_are_not_gated(self) -> None:
        for cmd in ("volume", "speaker", "mic_gain", "brightness", "sleep", "hatch.test", "test.loopback"):
            self.assertTrue(self.allows(cmd, True, 5_000_000, 0), cmd)

    def test_wiring(self) -> None:
        ble = (MUSE / "muse_ble.c").read_text()
        self.assertIn("} else if (!part && wireless && !muse_ble_gate_allows(cmd, was_set_up, esp_timer_get_time(), "
                      "s_window_until_us)) {", ble)
        self.assertIn("return muse_link_token_stored() || muse_settings_hatch_token_len() > 0;", ble)
        # The writes also need a passkey-authenticated bond (the code on the screen).
        self.assertIn(".flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_ENC | BLE_GATT_CHR_F_WRITE_AUTHEN,", ble)
        self.assertIn("muse_ble_open_setup_window();   /* the user's own action */", ble)
        self.assertIn("MUSE_BLE_SETUP_WINDOW_US (10LL * 60 * 1000 * 1000)", (MUSE / "muse_ble_gate.h").read_text())


if __name__ == "__main__":
    unittest.main()
