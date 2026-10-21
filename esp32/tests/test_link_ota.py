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

import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


def function_source(source: str, signature: str) -> str:
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 0
    for end in range(brace, len(source)):
        if source[end] == "{":
            depth += 1
        elif source[end] == "}":
            depth -= 1
            if depth == 0:
                return source[start : end + 1]
    raise AssertionError(f"Unclosed function: {signature}")


class LinkOtaTest(unittest.TestCase):
    def compile_and_run(self, sources: list[Path], include: Path) -> str:
        compiler = shlex.split(os.environ.get("CC", "cc"))
        if not compiler or shutil.which(compiler[0]) is None:
            self.skipTest("C compiler unavailable")
        binary = include / "ota-test"
        compiled = subprocess.run(
            [*compiler, "-std=c11", "-D_GNU_SOURCE", "-Wall", "-Wextra", "-Werror",
             "-DLINK_FAKE_CUSTOM_TASKS=1",
             "-I", str(include), "-I", str(ROOT / "tests/link_fakes"),
             "-I", str(ROOT / "main"), *map(str, sources), "-o", str(binary)],
            capture_output=True, text=True,
        )
        self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
        ran = subprocess.run([str(binary)], capture_output=True, text=True)
        self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
        return ran.stdout

    def test_ota_disabled_and_enabled(self) -> None:
        # mosaico: "" (another board), "board" (the ESP-Mosaico without its
        # Vibe Mode overlay) or "vibe" (its Vibe Mode build).
        for enabled, dev, mosaico in (
            (False, True, ""), (True, True, ""), (True, False, ""),
            (True, True, "board"), (False, True, "vibe"), (True, True, "vibe"),
        ):
            with self.subTest(enabled=enabled, dev=dev, mosaico=mosaico), \
                    tempfile.TemporaryDirectory() as name:
                tmp = Path(name)
                (tmp / "sdkconfig.h").write_text(
                    ("#define CONFIG_HOMEHUB_OTA_ENABLED 1\n" if enabled else "")
                    + f"#define CONFIG_HOMEHUB_DEV_BUILD {int(dev)}\n"
                    + ("#define CONFIG_MUSE_BOARD_ESP_MOSAICO 1\n" if mosaico else "")
                    + ("#define CONFIG_ESP_IRIS_OTA_DEFAULT_VIA_RECOVERY 1\n"
                       if mosaico == "vibe" else "")
                )
                # Type-check log arguments without printing or making them unused.
                (tmp / "esp_log.h").write_text(
                    "#include <stdio.h>\n"
                    "#define ESP_LOGI(tag, ...) ((void)(tag), (void)sizeof(printf(__VA_ARGS__)))\n"
                    "#define ESP_LOGW ESP_LOGI\n#define ESP_LOGE ESP_LOGI\n"
                )
                self.compile_and_run(
                    [ROOT / "main/ota.c", ROOT / "tests/link_ota_harness.c"], tmp,
                )

    def test_disabled_ota_does_not_interrupt_setup(self) -> None:
        # Exercise the actual entry-point functions with fake connectivity/UI,
        # without linking the rest of app.c's hardware orchestration.
        app = (ROOT / "main/app.c").read_text()
        functions = "\n".join(function_source(app, signature) for signature in (
            "static bool start_wifi_join_ota_if_requested(",
            "static void on_ble_ota(",
        ))
        with tempfile.TemporaryDirectory() as name:
            tmp = Path(name)
            (tmp / "sdkconfig.h").write_text("")
            source = tmp / "setup.c"
            source.write_text('''
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include "ota.h"
#include "esp_app_desc.h"
#define ESP_LOGI(...) ((void)0)
static esp_app_desc_t app = { .version = "999.0.0" };
const esp_app_desc_t *esp_app_get_description(void) { return &app; }
static int wifi_checks, statuses, callbacks;
static bool wifi_mgr_is_connected(void) { wifi_checks++; return false; }
static void ble_server_send_status(const char *s) {
    assert(strcmp(s, "ota_skipped") == 0);
    statuses++;
}
static bool ble_server_send_pairing_status(const char *s, uint32_t generation) {
    (void)s; (void)generation;
    assert(!"Disabled OTA must not send pairing status");
    return false;
}
static bool require_provisioning_pairing_session(uint32_t generation) {
    (void)generation;
    assert(!"Disabled OTA must not check the provisioning session");
    return false;
}
static void ui_set_status(const char *s) {
    assert(strcmp(s, "ota_skipped") == 0);
    statuses++;
}
static void ble_ota_status(const ota_event_t *ev, void *user) {
    (void)user;
    assert(ev->result == OTA_RESULT_SKIPPED);
    callbacks++;
}
''' + functions + '''
int main(void) {
    for (int force = 0; force <= 1; force++) {
        // False means provision continues normal setup.
        assert(!start_wifi_join_ota_if_requested("https://example.com/fw.bin", force, 0));
        assert(!start_wifi_join_ota_if_requested("https://example.com/fw.bin", force, 42));
        assert(statuses == force * 2);
        on_ble_ota("https://example.com/fw.bin", force);
        assert(statuses == (force + 1) * 2);
    }
    assert(wifi_checks == 0);
    assert(callbacks == 0); // Reports skipped directly, without calling ota_start.
    return 0;
}
''')
            self.compile_and_run([source, ROOT / "main/ota.c"], tmp)

    def test_boot_checks_only_recorded_updates(self) -> None:
        # app_run's arming block: every PENDING_VERIFY image gets the health
        # check (and its rollback) except the ESP-Mosaico's Vibe Mode install,
        # which is marked valid at once.
        app = (ROOT / "main/app.c").read_text()
        start = app.index("    // Arm OTA rollback verification.")
        block = app[start : app.index("    xTaskCreate(ota_verify_task", start)]
        with tempfile.TemporaryDirectory() as name:
            tmp = Path(name)
            (tmp / "sdkconfig.h").write_text("")
            source = tmp / "arm.c"
            source.write_text("""
#include <assert.h>
#include <stdbool.h>
#include "esp_ota_ops.h"
#define TAG "test"
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
static const esp_partition_t part = { .address = 0x20000 };
static esp_ota_img_states_t state;
static bool vibe_mode, s_ota_pending_verify;
static int marked, forgotten;
const esp_partition_t *esp_ota_get_running_partition(void) { return &part; }
esp_err_t esp_ota_get_state_partition(const esp_partition_t *p, esp_ota_img_states_t *s) {
    assert(p == &part);
    *s = state;
    return ESP_OK;
}
esp_err_t esp_ota_mark_app_valid_cancel_rollback(void) { marked++; return ESP_OK; }
static bool ota_installed_by_vibe_mode(void) { return vibe_mode; }
static void ota_verify_forget(void) { forgotten++; }
static void arm(esp_ota_img_states_t st, bool vibe) {
    state = st; vibe_mode = vibe;
    s_ota_pending_verify = false; marked = forgotten = 0;
""" + block + """
}
int main(void) {
    arm(ESP_OTA_IMG_PENDING_VERIFY, false);   /* any other install: checked */
    assert(s_ota_pending_verify && !marked && !forgotten);
    arm(ESP_OTA_IMG_PENDING_VERIFY, true);    /* Vibe Mode's install: valid now */
    assert(!s_ota_pending_verify && marked == 1 && forgotten == 1);
    arm(ESP_OTA_IMG_VALID, false);            /* an update that never booted */
    assert(!s_ota_pending_verify && !marked && forgotten == 1);
    arm(ESP_OTA_IMG_VALID, true);
    assert(!s_ota_pending_verify && !marked);
    return 0;
}
""")
            self.compile_and_run([source], tmp)

    def test_registration_only_advertises_enabled_ota(self) -> None:
        noise = (ROOT / "main/noise_control.cpp").read_text()
        functions = "\n".join(function_source(noise, signature) for signature in (
            "static cJSON *string_param(",
            "static void add_command(",
            "static void add_register_metadata_string(",
            "static char *build_register_json(",
        ))
        with tempfile.TemporaryDirectory() as name:
            tmp = Path(name)
            source = tmp / "registration.c"
            # These builders use the C subset of C++, with nullptr as the only
            # exception. Execute them with the same cJSON fake as other tests.
            source.write_text('''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "esp_app_desc.h"
#define nullptr NULL
#define CONFIG_HOMEHUB_TUNNEL 1
static bool enabled;
bool ota_is_enabled(void) { return enabled; }
static esp_app_desc_t app = { .version = "999.0.0" };
const esp_app_desc_t *esp_app_get_description(void) { return &app; }
static char s_wifi_ssid[33] = "test-wifi", s_register_req_id[40];
static const char *s_node_id = "test-node", *s_display_name = "Test device";
static void copy_wifi_ssid(char *out, size_t size) { snprintf(out, size, "%s", s_wifi_ssid); }
static void make_uuid(char *out, size_t size) { snprintf(out, size, "test-request"); }
''' + functions + '''
int main(void) {
    for (int on = 0; on <= 1; on++) {
        enabled = on;
        char *json = build_register_json();
        assert(json);
        puts(json);
        free(json);
    }
    return 0;
}
''')
            output = self.compile_and_run([source, ROOT / "tests/link_fakes/cJSON.c"], tmp)
            registrations = [json.loads(line)["params"] for line in output.splitlines()]
            self.assertEqual(len(registrations), 2)
            for enabled, params in zip((False, True), registrations):
                commands = params["commands_v2"]
                self.assertIn("device.health", commands)
                self.assertIn("device.discover", commands)
                self.assertEqual("device.ota" in commands, enabled)
                self.assertEqual(params["version"], "999.0.0")


if __name__ == "__main__":
    unittest.main()
