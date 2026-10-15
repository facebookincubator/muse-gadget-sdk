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

from __future__ import annotations

import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import tempfile
import unittest

from test_link_unpair_storage_contract import _function_body


ROOT = Path(__file__).resolve().parents[1]


class LinkEndpointLifecycleTest(unittest.TestCase):
    def test_endpoint_lifecycle(self) -> None:
        cc = shlex.split(os.environ.get("CC", "cc"))
        if not cc or shutil.which(cc[0]) is None:
            self.skipTest("C compiler not available")
        app = (ROOT / "main/app.c").read_text()
        api = (ROOT / "main/vm_api.c").read_text()
        noise = (ROOT / "main/noise_control.cpp").read_text()
        provision = _function_body(app, "static void on_provision(")
        provision = provision.split('config_erase_key("vm_url");', 1)[1]
        provision = provision.split("if (!require_provisioning_pairing_session", 1)[0]
        boot = _function_body(app, "void app_run(")
        boot = boot.split('heap_snapshot("after noise_ctrl_init");', 1)[1]
        boot = boot.split("bool skip_boot_scan", 1)[0]

        # Execute the real setters and application blocks, with only storage,
        # transport and unrelated boot services replaced by host fakes.
        production = "\n".join(re.findall(
            r"^(?:static char s_(?:api_base|noise_host)\[.*|"
            r"#define NOISE_DEFAULT_HOST .*)$",
            api + noise, re.MULTILINE,
        ))
        for source, signature in (
            (api, "void vm_api_set_base_url(const char *url)"),
            (api, "static void make_api_url(char *out, size_t out_cap, const char *path)"),
            (noise, "void noise_ctrl_set_host(const char *host)"),
            (app, "static void setup_disconnect_to_clean(void)"),
            (app, "static bool setup_wipe_to_clean(void)"),
        ):
            production += "\n" + signature + " {" + _function_body(source, signature) + "}\n"

        harness = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vm_api.h"
#include "diagnostic_log.h"
#define CONFIG_MUSE_ENABLED 0
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define ESP_OK 0
#define ESP_OTA_IMG_PENDING_VERIFY 1
#define esp_ota_get_running_partition() NULL
#define esp_ota_get_state_partition(p, s) ((void)(p), (void)(s), -1)
#define xTaskCreate(...) ((void)0)
typedef int esp_partition_t;
typedef int esp_ota_img_states_t;
static bool s_ota_pending_verify;
static char stored[4][256];
static bool complete, provisioned, booting, checked_setup;
static const char *fail_key;
static int failures, disconnects, log_clears;
static const char *custom_api = "https://custom-api.example";
static const char *custom_api_v2 = "https://custom-api-v2.example";
static const char *custom_noise = "custom-noise.example";

static int key_index(const char *key) {
    if (!strcmp(key, "ssid")) return 0;
    if (!strcmp(key, "api_url")) return 1;
    if (!strcmp(key, "api_url_v2")) return 3;
    assert(!strcmp(key, "noise_host"));
    return 2;
}
static bool config_set_str(const char *key, const char *value) {
    if (fail_key && !strcmp(key, fail_key)) return false;
    snprintf(stored[key_index(key)], sizeof(stored[0]), "%s", value);
    return true;
}
static bool config_erase_key(const char *key) { return config_set_str(key, ""); }
static bool config_get_str(const char *key, char *value, size_t cap) {
    int index = key_index(key);
    if (booting && index != 0) assert(checked_setup && complete && provisioned);
    snprintf(value, cap, "%s", stored[index]);
    return stored[index][0] != '\0';
}
static bool config_setup_complete(void) { checked_setup = true; return complete; }
static bool config_is_provisioned(void) { return provisioned; }
static bool config_mark_setup_complete(void) { complete = true; return true; }
static bool config_clear_setup(void) {
    memset(stored, 0, sizeof(stored));
    complete = provisioned = false;
    return true;
}
static bool clear_setup_credentials(void) { return config_clear_setup(); }
static void disconnect_vm_transports(void) { disconnects++; }
void diagnostic_log_clear(void) {
    assert(disconnects > log_clears);
    log_clears++;
}
static void wifi_mgr_disconnect(void) {}
static void ui_set_vm(const char *value) { (void)value; }
static void ui_set_wifi(const char *value) { (void)value; }
@PRODUCTION@
static void setup_fail_for_session(const char *stage, const char *status,
                                   unsigned generation) {
    assert(!strcmp(stage, "storage") && !strcmp(status, "error_storage"));
    assert(generation == 1);
    failures++;
    assert(setup_wipe_to_clean());
}
static bool apply_endpoints_v2(const char *api_url_v2, const char *api_url,
                               const char *noise_host) {
    unsigned session_generation = 1;
    @PROVISION@
    return true;
done:
    return false;
}
static bool apply_endpoints(const char *api_url, const char *noise_host) {
    return apply_endpoints_v2(NULL, api_url, noise_host);
}
static void boot_endpoints(void) {
    booting = true;
    @BOOT@
    booting = false;
}
static void expect_endpoints(const char *api_url, const char *noise_host) {
    assert(!strcmp(s_api_base, api_url));
    assert(!strcmp(s_noise_host, noise_host));
    char url[512], expected[512];
    make_api_url(url, sizeof(url), "/fetch_vms");
    snprintf(expected, sizeof(expected), "%s/fetch_vms", api_url);
    assert(!strcmp(url, expected));
}
static void reset_fixture(void) {
    fail_key = NULL;
    booting = checked_setup = false;
    failures = disconnects = log_clears = 0;
    config_clear_setup();
    vm_api_set_base_url(NULL);
    noise_ctrl_set_host(NULL);
}
static void check_boot_v2(bool setup, bool token, bool wifi, bool keep_endpoints,
                          bool with_v2) {
    reset_fixture();
    config_set_str("api_url", custom_api);
    if (with_v2) config_set_str("api_url_v2", custom_api_v2);
    config_set_str("noise_host", custom_noise);
    if (wifi) config_set_str("ssid", "saved-wifi");
    complete = setup;
    provisioned = token;
    boot_endpoints();
    const char *noise = keep_endpoints ? custom_noise : NOISE_DEFAULT_HOST;
    expect_endpoints(keep_endpoints && with_v2 ? custom_api_v2 : VM_API_DEFAULT_BASE_URL,
                     noise);
    assert((stored[1][0] != '\0') == keep_endpoints);
    assert((stored[2][0] != '\0') == keep_endpoints);
    assert((stored[3][0] != '\0') == (keep_endpoints && with_v2));
    if (WIFI_WITHOUT_PAIRING && !setup && !token && wifi) assert(stored[0][0]);
}
static void check_boot(bool setup, bool token, bool wifi, bool keep_endpoints) {
    check_boot_v2(setup, token, wifi, keep_endpoints, false);
    check_boot_v2(setup, token, wifi, keep_endpoints, true);
}
int main(void) {
    reset_fixture();
    assert(apply_endpoints(custom_api, custom_noise));
    assert(setup_wipe_to_clean());
    assert(disconnects == 1 && log_clears == CONFIG_HOMEHUB_SUPPORT_BUG_REPORT);
    expect_endpoints(VM_API_DEFAULT_BASE_URL, NOISE_DEFAULT_HOST);
    assert(apply_endpoints(NULL, NULL));
    expect_endpoints(VM_API_DEFAULT_BASE_URL, NOISE_DEFAULT_HOST);

    assert(apply_endpoints(custom_api, custom_noise));
    assert(apply_endpoints(NULL, custom_noise));
    expect_endpoints(VM_API_DEFAULT_BASE_URL, custom_noise);
    assert(!stored[1][0] && stored[2][0]);
    // api_url is saved for older firmware but never routes this one.
    assert(apply_endpoints(custom_api, ""));
    expect_endpoints(VM_API_DEFAULT_BASE_URL, NOISE_DEFAULT_HOST);
    assert(stored[1][0] && !stored[2][0]);
    noise_ctrl_set_host(custom_noise);
    noise_ctrl_set_host("");
    assert(!strcmp(s_noise_host, NOISE_DEFAULT_HOST));

    // api_url_v2 sets the base; api_url is still saved for older firmware.
    reset_fixture();
    assert(apply_endpoints_v2(custom_api_v2, custom_api, custom_noise));
    expect_endpoints(custom_api_v2, custom_noise);
    assert(stored[1][0] && stored[3][0]);
    // A later pairing without it drops the saved value and uses the default.
    assert(apply_endpoints(custom_api, custom_noise));
    expect_endpoints(VM_API_DEFAULT_BASE_URL, custom_noise);
    assert(stored[1][0] && !stored[3][0]);
    assert(apply_endpoints_v2(custom_api_v2, NULL, NULL));
    assert(setup_wipe_to_clean());
    expect_endpoints(VM_API_DEFAULT_BASE_URL, NOISE_DEFAULT_HOST);

    const char *failed_keys[] = {"api_url", "api_url_v2", "noise_host"};
    for (size_t i = 0; i < 3; i++) {
        reset_fixture();
        assert(apply_endpoints(custom_api, custom_noise));
        fail_key = failed_keys[i];
        assert(!apply_endpoints(custom_api, NULL));
        assert(failures == 1 && log_clears == CONFIG_HOMEHUB_SUPPORT_BUG_REPORT);
        expect_endpoints(VM_API_DEFAULT_BASE_URL, NOISE_DEFAULT_HOST);
    }
    check_boot(false, false, false, false);  // Orphan endpoint keys.
    check_boot(false, false, true, false);   // Unpaired Wi-Fi on Muse.
    check_boot(true, false, true, false);    // Marker without credentials.
    check_boot(true, true, true, true);     // Complete custom setup.
    check_boot(false, true, true, true);    // Legacy marker migration.
    check_boot(false, true, false, CONFIG_HOMEHUB_WIFI_SSID[0] != '\0');
    return 0;
}
"""
        harness = harness.replace("@PRODUCTION@", production)
        harness = harness.replace("@PROVISION@", provision).replace("@BOOT@", boot)
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "endpoint_lifecycle.c"
            source.write_text(harness)
            # The diagnostic log only exists, and is only cleared, with bug reports on.
            for name, wifi, unpaired_wifi, bug_report in (
                    ("devkit", "", 0, 1), ("muse", "", 1, 1), ("dev_wifi", "dev", 0, 1),
                    ("no_bug_report", "", 0, 0)):
                with self.subTest(profile=name):
                    binary = Path(directory) / name
                    compiled = subprocess.run(
                        [*cc, "-std=c11", "-Wall", "-Wextra", "-Werror",
                         f'-DCONFIG_HOMEHUB_WIFI_SSID="{wifi}"',
                         f"-DWIFI_WITHOUT_PAIRING={unpaired_wifi}",
                         f"-DCONFIG_HOMEHUB_SUPPORT_BUG_REPORT={bug_report}",
                         "-I", str(ROOT / "main"), str(source), "-o", str(binary)],
                        capture_output=True, text=True,
                    )
                    self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
                    ran = subprocess.run([str(binary)], capture_output=True, text=True)
                    self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)


if __name__ == "__main__":
    unittest.main()
