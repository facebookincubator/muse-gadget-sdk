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
"""Every setup or re-setup failure leaves both the stored and the in-memory
server addresses as they were before it (main/app.c's
setup_fail_for_session): a re-setup's rollback brings the old ones back,
in RAM too, before the old setup's tokens are used again; a first setup's
wipe leaves the defaults. On the host, with the real setters and
muse_server_hosts.h: a release build puts a stored server it doesn't allow
back as the default."""

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

ROOT = Path(__file__).resolve().parents[1]

HARNESS = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "vm_api.h"
#include "muse_server_hosts.h"
#define ESP_LOGW(...) ((void)0)
#if CONFIG_HOMEHUB_CUSTOM_SERVER_HOSTS
#define OLD_API_IN_RAM "https://old-api.example"
#define OLD_NOISE_IN_RAM "old-noise.example"
#else
/* A release build doesn't allow the old setup's servers: the defaults. */
#define OLD_API_IN_RAM VM_API_DEFAULT_BASE_URL
#define OLD_NOISE_IN_RAM NOISE_DEFAULT_HOST
#endif
static const char *TAG = "t";
#define LED_STATE_ERROR 1
#define CONFIRM_TO_IDLE 0
static char stored_api[256], stored_noise[256];
static bool journal_open, rolled_back, wiped;
static int disconnects, releases;
static bool config_get_str(const char *key, char *out, size_t cap) {
    const char *v = !strcmp(key, "api_url_v2") ? stored_api : !strcmp(key, "noise_host") ? stored_noise : "";
    snprintf(out, cap, "%s", v);
    return v[0] != '\0';
}
static bool config_erase_key(const char *key) { (void)key; return true; }
#define OWNER_WIPE_KEY "owner_wipe"   /* app.c: an owner change's mark, erased by a failed setup */
static bool config_reprovision_open(void) { return journal_open; }
static bool config_reprovision_rollback(void) {
    /* The old setup's servers come back in the store. */
    snprintf(stored_api, sizeof(stored_api), "https://old-api.example");
    snprintf(stored_noise, sizeof(stored_noise), "old-noise.example");
    journal_open = false;
    rolled_back = true;
    return true;
}
@PRODUCTION@
static void disconnect_vm_transports(void) { disconnects++; }
static void session_tokens_release(void) {
    /* The old tokens are usable again only once the old servers are back in RAM. */
    assert(disconnects == 1);
    assert(!strcmp(s_api_base, OLD_API_IN_RAM) && !strcmp(s_noise_host, OLD_NOISE_IN_RAM));
    releases++;
}
typedef enum { SETUP_RESET_KEEP_WIFI, SETUP_RESET_FORGET_WIFI } setup_reset_scope_t;
static bool setup_wipe_to_clean(setup_reset_scope_t scope) {
    (void)scope;
    stored_api[0] = stored_noise[0] = '\0';
    wiped = true;
    return true;
}
static void setup_window_lock_take(void) {}
static void setup_window_lock_give(void) {}
static bool link_pairing_session_is_current(uint32_t g) { (void)g; return true; }
static void setup_stage_set(const char *s) { (void)s; }
static void ui_set_status(const char *s) { (void)s; }
static void led_status_set_state(int s) { (void)s; }
static void ble_server_send_pairing_status(const char *s, uint32_t g) { (void)s; (void)g; }
static void ble_server_send_status(const char *s) { (void)s; }
static bool link_pairing_extend_provisioning_deadline(uint32_t g) { (void)g; return false; }
static uint32_t advance_confirm_generation_locked(void) { return 1; }
static void start_confirm_timeout(int to, uint32_t a, uint32_t b) { (void)to; (void)a; (void)b; }
static uint32_t link_pairing_session_generation(void) { return 1; }
static void ble_server_disconnect_pairing_session(uint32_t g) { (void)g; }
static void link_pairing_reset(void) {}
static void ble_server_delayed_disconnect(uint32_t ms) { (void)ms; }
@FAIL@

static void new_setup_live(void) {
    /* What a setup has done by the time it fails: the new servers stored and in RAM. */
    snprintf(stored_api, sizeof(stored_api), "https://new-api.example");
    snprintf(stored_noise, sizeof(stored_noise), "new-noise.example");
    vm_api_set_base_url(stored_api);
    noise_ctrl_set_host(stored_noise);
    disconnects = releases = 0;
    rolled_back = wiped = false;
}

int main(void) {
    const char *stages[] = { "wifi", "auth", "vm", "storage", "pairing" };
    for (unsigned i = 0; i < sizeof(stages) / sizeof(stages[0]); i++) {
#if CONFIG_MUSE_GADGET_BLE_AUDIO
        /* A re-setup: the rollback's servers, stored and in RAM, before the old tokens. */
        new_setup_live();
        journal_open = true;
        setup_fail_for_session(stages[i], "auth_failed", 1);
        assert(rolled_back && !wiped && releases == 1);
        assert(!strcmp(s_api_base, OLD_API_IN_RAM) && !strcmp(s_noise_host, OLD_NOISE_IN_RAM));
#endif
        /* A first setup: wiped, and the defaults in RAM. */
        new_setup_live();
        journal_open = false;
        setup_fail_for_session(stages[i], "auth_failed", 1);
        assert(wiped && !stored_api[0] && !stored_noise[0]);
        assert(!strcmp(s_api_base, VM_API_DEFAULT_BASE_URL) && !strcmp(s_noise_host, NOISE_DEFAULT_HOST));
    }
    puts("setup failure: servers restored, stored and in RAM");
    return 0;
}
"""


def _function(source: str, prefix: str) -> str:
    start = source.index(prefix)
    brace = source.index("{", start)
    return source[start:brace] + "{" + _function_body(source, prefix) + "}\n"


class SetupFailureRestoresServers(unittest.TestCase):
    def run_profile(self, gadget: bool, custom_hosts: bool = True) -> None:
        app = (ROOT / "main/app.c").read_text()
        api = (ROOT / "main/vm_api.c").read_text()
        noise = (ROOT / "main/noise_control.cpp").read_text()
        production = "\n".join(re.findall(r"^(?:static char s_(?:api_base|noise_host)\[.*|#define NOISE_DEFAULT_HOST .*)$",
                                          api + noise, re.MULTILINE))
        production += "\n" + _function(api, "void vm_api_set_base_url(const char *url)")
        production += _function(noise, 'extern "C" void noise_ctrl_set_host(const char *host)').replace('extern "C" ', "")
        production += _function(app, "static void endpoints_from_store(void)")
        fail = _function(app, "static void setup_fail_for_session(")
        code = HARNESS.replace("@PRODUCTION@", production).replace("@FAIL@", fail)
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            (d / "t.c").write_text(code)
            exe = str(d / "t")
            cc = shlex.split(os.environ.get("CC", "cc"))
            subprocess.run(cc + ["-std=gnu11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-variable",
                                 "-Wno-unused-function", f"-DCONFIG_MUSE_GADGET_BLE_AUDIO={int(gadget)}",
                                 "-DCONFIG_MUSE_GADGET_BLE_STANDALONE=0",
                                 f"-DCONFIG_HOMEHUB_CUSTOM_SERVER_HOSTS={int(custom_hosts)}",
                                 "-include", str(ROOT / "tests/host_compat.h"),
                                 "-I", str(ROOT / "main"), "-I", str(ROOT / "components/muse"),
                                 str(d / "t.c"), "-o", exe], check=True)
            out = subprocess.run([exe], check=True, capture_output=True, text=True).stdout
            self.assertIn("servers restored", out)

    def test_gadget_with_resetup(self) -> None:
        self.run_profile(True)

    def test_link_only(self) -> None:
        self.run_profile(False)

    def test_release_build_drops_a_server_it_does_not_allow(self) -> None:
        self.run_profile(True, custom_hosts=False)
        self.run_profile(False, custom_hosts=False)


if __name__ == "__main__":
    unittest.main()
