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
"""A SAFE mode that can't serve Muse advertises under another identity and
refuses the Muse GATT, so a phone's saved reconnect never lands on it
(main/ble_server.c, mg_ble.c's access check)."""

import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def body(src: str, sig: str) -> str:
    i = src.index(sig)
    j = src.index("\n}\n", i)
    return src[i:j]


class GattBeforeAdvertising(unittest.TestCase):
    def setUp(self) -> None:
        self.src = (ROOT / "main" / "ble_server.c").read_text()

    def test_safe_mode_advertises_as_someone_else(self) -> None:
        # A SAFE mode that can't serve Muse advertises under another
        # identity, so a phone's saved reconnect never lands on it: a static random address,
        # "SAFE-<product>-<tail>", no service UUIDs.
        adv = body(self.src, "static void start_advertising(void) {")
        self.assertIn("if (s_safe_identity) {", adv)
        self.assertIn('snprintf(safe_name, sizeof(safe_name), "SAFE-%s", tail);', adv)
        self.assertIn("memset(&adv, 0, sizeof(adv));", adv)
        self.assertIn("s_safe_identity && !s_safe_public ? BLE_OWN_ADDR_RANDOM : BLE_OWN_ADDR_PUBLIC", adv)
        sync = body(self.src, "static void on_sync(void) {")
        self.assertLess(sync.index("ble_hs_id_set_rnd(a)"), sync.index("start_advertising();"))
        self.assertIn("a[5] |= 0xC0;", sync)
        # Never silent and never the Muse identity: a second random address,
        # else the public address with the SAFE name, no UUIDs and the Muse GATT refused.
        self.assertNotIn("s_safe_identity = false", sync + adv)
        self.assertIn("rc = ble_hs_id_gen_rnd(0, &r);", sync)
        self.assertIn("s_safe_public = true;", sync)
        self.assertIn("if (rc != 0 && s_safe_identity && !s_safe_public) {", adv)
        self.assertIn("return s_safe_identity && s_safe_public;", body(self.src, "bool ble_server_muse_gatt_refused(void) {"))
        self.assertIn("if (ble_server_muse_gatt_refused()) return BLE_ATT_ERR_INSUFFICIENT_AUTHOR;", body(self.src, "static int rx_access("))
        mg = (ROOT / "components" / "muse_gadget_ble" / "mg_ble.c").read_text()
        acc = body(mg, "static int mg_access(")
        self.assertLess(acc.index("s_platform.muse_gatt_refused()"), acc.index("OS_MBUF_PKTLEN"))
        self.assertIn(".muse_gatt_refused = ble_server_muse_gatt_refused,", (ROOT / "main" / "mg_glue.c").read_text())
        # The bench's one-boot SAFE identity comes back by itself.
        self.assertIn("#define SAFEID_BOOT_MS 120000", self.src)
        take = body(self.src, "bool ble_server_bench_safe_identity_take(void) {")
        self.assertIn("esp_timer_start_once(s_safeid_timer", take)
        app = (ROOT / "main" / "app.c").read_text()
        self.assertLess(app.index("ble_server_set_safe_identity(safe_identity || ble_server_bench_safe_identity_take());"),
                        app.index("ble_server_start(identity_ble_name(), &cb);"))


if __name__ == "__main__":
    unittest.main()
