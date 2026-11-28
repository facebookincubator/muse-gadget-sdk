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
"""Every unpair and reset also forgets the bonded phones and puts the Muse
server, VM and token back to their defaults (main/app.c's
forget_owner_links()): the user's Reset pairing and the button hold, the
phone's unpair, the server's node.unpaired and device.unpair, and the token
service's refusal."""

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
from test_link_unpair_storage_contract import _function_body
from test_muse_ptt_ui import FAKE_HEADERS, LINK_GC

ROOT = Path(__file__).resolve().parents[1]


class ResetsForgetPairings(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.app = (ROOT / "main" / "app.c").read_text()

    def test_the_helper_clears_both(self) -> None:
        body = _function_body(self.app, "static bool forget_owner_links(")
        self.assertIn("ble_server_forget_bonds();", body)
        self.assertRegex(body, r"#if CONFIG_MUSE_ENABLED\s*return muse_settings_forget_server\(\);")

    def test_the_shared_reset_calls_it_after_the_verified_erase(self) -> None:
        reset = _function_body(self.app, "static bool reset_setup_with_gate_held(")
        self.assertLess(reset.index("if (!setup_wipe_to_clean(scope))"), reset.index("if (!forget_owner_links())"))
        self.assertLess(reset.index("return false;"), reset.index("if (!forget_owner_links())"))
        # A failed token erase never completes the reset
        self.assertLess(reset.index("if (!forget_owner_links())"), reset.index("finish_setup_reset_with_gate_held();"))

    def test_every_unpair_and_reset_path_reaches_it(self) -> None:
        # The phone's unpair over BLE.
        self.assertIn('reset_setup_with_gate_held("BLE unpair", SETUP_RESET_KEEP_WIFI)',
                      _function_body(self.app, "static void on_unpair("))
        # Reset pairing (menu, settings), the button hold, the token proof clear, the deferred resets: all
        # through reset_setup_checked, which runs the shared reset.
        self.assertIn("reset_setup_with_gate_held(source, scope)",
                      _function_body(self.app, "static bool reset_setup_checked("))
        self.assertIn("return reset_setup_checked(source, scope, NULL, NULL, true);",
                      _function_body(self.app, "static bool reset_setup_from_control("))
        self.assertIn('reset_setup_from_control("Muse menu reset", SETUP_RESET_FORGET_WIFI)',
                      _function_body(self.app, "static void reset_setup_task("))
        self.assertIn('reset_setup_from_control("button long-press", SETUP_RESET_FORGET_WIFI)', self.app)
        self.assertIn('reset_setup_checked("token proof clear"', self.app)
        # The server's node.unpaired.
        self.assertIn('reset_setup_from_control("server unpair (node.unpaired)", SETUP_RESET_KEEP_WIFI)',
                      _function_body(self.app, "static void ws_unpaired_task("))
        # The token service's refusal (401): unpairs, then the app task's deferred unpair.
        defer = _function_body(self.app, "static void defer_unpair_after_revocation(")
        self.assertIn("atomic_store_explicit(&s_unpair_pending, true, memory_order_release);", defer)
        self.assertRegex(self.app, r"else if \(unpair\) \{\s*reset_setup_from_control\(\"deferred unpair\", "
                                   r"SETUP_RESET_KEEP_WIFI\);")
        # device.unpair clears the setup itself before answering: it calls the helper there.
        queue = _function_body(self.app, "static cJSON *queue_ws_control(")
        unpair = queue[queue.index("if (action == WS_CONTROL_UNPAIR) {"):]
        self.assertLess(unpair.index("if (!clear_setup_credentials(SETUP_RESET_KEEP_WIFI)) {"),
                        unpair.index("if (!forget_owner_links())"))
        self.assertLess(unpair.index("args->setup_credentials_cleared = true;"), unpair.index("if (!forget_owner_links())"))

    def test_a_failed_setup_keeps_them(self) -> None:
        # A failed setup's own clean-up isn't an unpair: the phone retrying keeps its bond.
        self.assertNotIn("forget_owner_links", _function_body(self.app, "static bool clear_setup_credentials("))
        self.assertEqual(len(re.findall(r"if \(!forget_owner_links\(\)\)", self.app)), 2)

    def test_the_muse_settings_go_back_to_their_defaults(self) -> None:
        muse = ROOT / "components" / "muse"
        with tempfile.TemporaryDirectory() as tmp:
            stage = Path(tmp)
            for source in muse.glob("*.h"):
                shutil.copyfile(source, stage / source.name)
            shutil.copyfile(muse / "muse_settings.c", stage / "muse_settings.c")
            shutil.copyfile(ROOT / "tests/muse_ptt_ui_fakes.h", stage / "muse_ptt_ui_fakes.h")
            for name in FAKE_HEADERS:
                p = stage / name
                p.parent.mkdir(parents=True, exist_ok=True)
                p.write_text('#include "muse_ptt_ui_fakes.h"\n')
            (stage / "sdkconfig.h").write_text("#define CONFIG_MUSE_DEFAULT_VOLUME 20\n")
            exe = str(stage / "reset")
            subprocess.run([*shlex.split(os.environ.get("CC", "cc")), "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                            "-Wno-unused-parameter", "-Wno-unused-function", "-DESP_ERR_INVALID_ARG=0x102", "-DCONFIG_HOMEHUB_CUSTOM_SERVER_HOSTS=1",
                            "-ffunction-sections", "-fdata-sections", *LINK_GC, "-fsanitize=address,undefined",
                            "-include", str(ROOT / "tests/host_compat.h"), "-I", tmp,
                            str(ROOT / "tests/muse_settings_reset_harness.c"), "-o", exe], check=True)
            out = subprocess.run([exe], check=True, capture_output=True, text=True,
                                 env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0:halt_on_error=1"}).stdout
            self.assertIn("back to their defaults", out)

    def test_forget_bonds(self) -> None:
        ble = (ROOT / "main" / "ble_server.c").read_text()
        body = _function_body(ble, "void ble_server_forget_bonds(void)")
        self.assertIn("if (ble_server_is_started()) {", body)
        self.assertIn("ble_store_clear();", body)
        self.assertRegex(body, r"#if CONFIG_BT_NIMBLE_NVS_PERSIST\s*nvs_handle_t h;\s*"
                               r"esp_err_t err = nvs_open\(\"nimble_bond\", NVS_READWRITE, &h\);")
        self.assertIn("err = nvs_erase_all(h);", body)

    def test_an_owner_change_keeps_the_new_phones_bond(self) -> None:
        # A committed re-setup forgets every bonded phone but the connected one (the new owner's, by identity
        # address); with no connected phone, or a bond it couldn't erase, the caller forgets them all.
        ble = (ROOT / "main" / "ble_server.c").read_text()
        head = "bool ble_server_forget_bonds_but_connected(void) {"
        fn = head + _function_body(ble, head) + "}\n"
        harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define MYNEWT_VAL(x) 4
#define BLE_HS_CONN_HANDLE_NONE 0xffff
#define BLE_HS_ENOENT 5
#define ESP_LOGI(tag, ...) ((void)(tag), printf(__VA_ARGS__))
#define ESP_LOGE(tag, ...) ((void)(tag), printf(__VA_ARGS__))
static const char *TAG = "t";
typedef struct { uint8_t type; uint8_t val[6]; } ble_addr_t;
struct ble_gap_conn_desc { ble_addr_t peer_id_addr; };
static bool started = true, store_fails, unpair_fails;
static uint16_t s_conn_handle = 1;
static ble_addr_t bonds[4];
static int nbonds;
static bool ble_server_is_started(void) { return started; }
static int ble_addr_cmp(const ble_addr_t *a, const ble_addr_t *b) { return memcmp(a, b, sizeof(*a)); }
static int ble_gap_conn_find(uint16_t h, struct ble_gap_conn_desc *d) {
    if (h != 1) return 2;
    memset(d, 0, sizeof(*d));
    d->peer_id_addr.val[0] = 0xB;   /* the new owner's phone */
    return 0;
}
static int ble_store_util_bonded_peers(ble_addr_t *out, int *n, int max) {
    if (store_fails) return 1;
    for (int i = 0; i < nbonds && i < max; i++) out[i] = bonds[i];
    *n = nbonds;
    return 0;
}
static int ble_gap_unpair(const ble_addr_t *a) {
    if (unpair_fails) return 3;
    for (int i = 0; i < nbonds; i++) {
        if (!ble_addr_cmp(&bonds[i], a)) { bonds[i] = bonds[--nbonds]; return 0; }
    }
    return BLE_HS_ENOENT;
}
static void set_bonds(void) {
    memset(bonds, 0, sizeof(bonds));
    bonds[0].val[0] = 0xA;   /* an earlier owner's phone */
    bonds[1].val[0] = 0xB;   /* the new owner's */
    bonds[2].val[0] = 0xC;
    nbonds = 3;
}
@FN@
int main(void) {
    set_bonds();
    assert(ble_server_forget_bonds_but_connected());
    assert(nbonds == 1 && bonds[0].val[0] == 0xB);   /* the new owner's phone keeps its bond; the others go */
    set_bonds();
    s_conn_handle = BLE_HS_CONN_HANDLE_NONE;   /* no phone connected: nothing done, the caller forgets them all */
    assert(!ble_server_forget_bonds_but_connected() && nbonds == 3);
    s_conn_handle = 1;
    started = false;
    assert(!ble_server_forget_bonds_but_connected() && nbonds == 3);
    started = true;
    store_fails = true;
    assert(!ble_server_forget_bonds_but_connected() && nbonds == 3);
    store_fails = false;
    unpair_fails = true;   /* a bond it couldn't erase: false, and the caller forgets them all */
    assert(!ble_server_forget_bonds_but_connected());
    puts("PASS keep the new phone");
    return 0;
}
'''.replace("@FN@", fn)
        with tempfile.TemporaryDirectory() as tmp:
            src, exe = Path(tmp) / "bonds.c", str(Path(tmp) / "bonds")
            src.write_text(harness)
            subprocess.run([*shlex.split(os.environ.get("CC", "cc")), "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                            "-fsanitize=address,undefined", str(src), "-o", exe], check=True)
            out = subprocess.run([exe], check=True, capture_output=True, text=True).stdout
        self.assertIn("PASS keep the new phone", out)


if __name__ == "__main__":
    unittest.main()
