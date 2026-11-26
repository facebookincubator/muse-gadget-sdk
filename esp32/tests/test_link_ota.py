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
             "-I", str(ROOT / "main"), "-I", str(ROOT / "components/muse_image_id/include"),
             *map(str, sources), "-o", str(binary)],
            capture_output=True, text=True,
        )
        self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
        ran = subprocess.run([str(binary)], capture_output=True, text=True)
        self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
        return ran.stdout

    def harness(self, enabled: bool, dev: bool = True, mosaico: str = "", gadget: str = "") -> str:
        """Runs link_ota_harness.c in one configuration; returns its output
        (log lines included). mosaico: "" (another board), "board" (the
        ESP-Mosaico without its Vibe Mode overlay), "vibe" (its Vibe Mode
        build) or "vibe2" (for Vibe Mode 0.2, Recovery ABI 2). gadget: ""
        (a plain Home Link build), "refuse" (a Muse Gadget, musegadgets BLE
        audio: Wi-Fi OTA refused) or "same-board" (a Muse Gadget with
        CONFIG_MUSE_GADGET_WIFI_OTA_SAME_BOARD)."""
        with tempfile.TemporaryDirectory() as name:
            tmp = Path(name)
            (tmp / "sdkconfig.h").write_text(
                ("#define CONFIG_HOMEHUB_OTA_ENABLED 1\n" if enabled else "")
                + f"#define CONFIG_HOMEHUB_DEV_BUILD {int(dev)}\n"
                + ("#define CONFIG_MUSE_BOARD_ESP_MOSAICO 1\n" if mosaico else "")
                + ("#define CONFIG_ESP_IRIS_OTA_DEFAULT_VIA_RECOVERY 1\n"
                   if mosaico.startswith("vibe") else "")
                + ("#define CONFIG_ESP_IRIS_RECOVERY_ABI 2\n"
                   "#define VIBE_SUBTYPE 0x20\n"  # app/test
                   if mosaico == "vibe2" else "")
                + ("#define CONFIG_MUSE_GADGET_BLE_AUDIO 1\n#define CONFIG_MUSE_GADGET_BLE_SMP 1\n"
                   "#define CONFIG_IDF_FIRMWARE_CHIP_ID 9\n"
                   "#define CONFIG_HOMEHUB_BLE_PRODUCT_CODE \"W175\"\n"
                   "#define CONFIG_MUSE_BOARD_ID \"waveshare_s3_175c\"\n" if gadget else "")
                + ("#define CONFIG_MUSE_GADGET_WIFI_OTA_SAME_BOARD 1\n" if gadget == "same-board" else "")
            )
            # Print log lines (the refusals are checked), type-checking their arguments.
            (tmp / "esp_log.h").write_text(
                "#include <stdio.h>\n"
                "#define ESP_LOGI(tag, ...) (printf(\"%s: \", (tag)), printf(__VA_ARGS__), putchar('\\n'))\n"
                "#define ESP_LOGW ESP_LOGI\n#define ESP_LOGE ESP_LOGI\n"
            )
            sources = [ROOT / "main/ota.c", ROOT / "tests/link_ota_harness.c"]
            if gadget == "same-board":
                sources.append(ROOT / "components/muse_image_id/muse_image_id.c")
            return self.compile_and_run(sources, tmp)

    def test_ota_disabled_and_enabled(self) -> None:
        for enabled, dev, mosaico in (
            (False, True, ""), (True, True, ""), (True, False, ""),
            (True, True, "board"), (False, True, "vibe"), (True, True, "vibe"),
            (True, True, "vibe2"),
        ):
            with self.subTest(enabled=enabled, dev=dev, mosaico=mosaico):
                self.assertIn("OTA harness passed", self.harness(enabled, dev, mosaico))

    def test_a_muse_gadget_refuses_wifi_ota(self) -> None:
        # An image built for the same chip with the dev key passes ESP-IDF's
        # chip id and signature checks. A Muse Gadget build updates over
        # Bluetooth only: ota_start refuses at once (UNSUPPORTED), downloads
        # nothing, and logs the URL's host and path, never its userinfo or
        # query.
        for enabled in (False, True):
            with self.subTest(enabled=enabled):
                out = self.harness(enabled, gadget="refuse")
                self.assertIn("OTA harness passed", out)
                line = next(l for l in out.splitlines() if "refused" in l)
                self.assertIn("link.ota: OTA refused: this Muse Gadget updates over Bluetooth", line)
                self.assertIn("offered version unknown (nothing downloaded)", line)
                self.assertIn("from ota.example.com:8443/fw/esp32c5/homelink-1.3.0.bin (query dropped)", line)
                for secret in ("secret-token-value", "X-Amz", "user:pw", "frag"):
                    self.assertNotIn(secret, out)

    def test_same_board_opt_in_checks_the_image_before_writing(self) -> None:
        # CONFIG_MUSE_GADGET_WIFI_OTA_SAME_BOARD: the download's first bytes
        # (seen through the HTTP client's ON_DATA events, matched against
        # ESP-IDF's own read of the description) must name this board before
        # esp_https_ota_perform (esp_ota_begin, the first write) runs: the C5
        # image, a Home Link image for the S3, other products and older
        # images are refused with nothing written; this board's installs.
        out = self.harness(True, gadget="same-board")
        self.assertIn("OTA harness passed", out)
        refusals = [l for l in out.splitlines() if "Wi-Fi OTA refused before writing" in l]
        self.assertEqual(len(refusals), 8, out)
        self.assertIn("built for another chip (offered chip 23, (no product code) none, version 1000.0.0; "
                      "this is chip 9, W175 waveshare_s3_175c), from example.com/update.bin", refusals[0])
        self.assertIn("built for another board (offered chip 9, (no product code) none", refusals[1])
        self.assertIn("built for another board (offered chip 9, SCW1 sensecap_watcher", refusals[2])
        self.assertIn("no board description", refusals[4])
        self.assertIn("weren't seen whole", out)   # the tampered copy
        self.assertEqual(out.count("image is this board's (W175)"), 3)

    def test_url_for_log(self) -> None:
        with tempfile.TemporaryDirectory() as name:
            tmp = Path(name)
            (tmp / "sdkconfig.h").write_text("")
            (tmp / "esp_log.h").write_text(
                "#include <stdio.h>\n"
                "#define ESP_LOGI(tag, ...) ((void)(tag), (void)sizeof(printf(__VA_ARGS__)))\n"
                "#define ESP_LOGW ESP_LOGI\n#define ESP_LOGE ESP_LOGI\n")
            source = tmp / "url.c"
            source.write_text(r'''
#include <stdio.h>
#include "ota.h"
#include "esp_app_desc.h"
static esp_app_desc_t app;
const esp_app_desc_t *esp_app_get_description(void) { return &app; }
int main(void) {
    static const char *urls[] = {
        "https://cdn.example.com/fw/esp32c5/muse-gadget-1.3.0.bin",
        "https://u:p@cdn.example.com/a?token=abc#x",
        "https://cdn.example.com/dl/AbCdEfGhIjKlMnOpQrStUvWxYz0123456789_-/fw.bin",
        "cdn.example.com/plain.bin",
        "",
        "https://cdn.example.com/" "0123456789012345678901234567890123456789012345678901234567890123456789"
        "/0123456789.0123456789.0123456789.0123456789.0123456789.0123456789.0123456789.bin",
    };
    for (unsigned i = 0; i < sizeof(urls) / sizeof(urls[0]); i++) {
        char out[96];
        ota_url_for_log(urls[i], out, sizeof(out));
        puts(out);
    }
    char tiny[8];
    ota_url_for_log("https://cdn.example.com/x.bin", tiny, sizeof(tiny));
    puts(tiny);
    return 0;
}
''')
            out = self.compile_and_run([source, ROOT / "main/ota.c"], tmp).splitlines()
        self.assertEqual(out[0], "cdn.example.com/fw/esp32c5/muse-gadget-1.3.0.bin")
        self.assertEqual(out[1], "cdn.example.com/a (query dropped)")
        self.assertEqual(out[2], "cdn.example.com/dl/<38 chars>/fw.bin")
        self.assertEqual(out[3], "cdn.example.com/plain.bin")
        self.assertEqual(out[4], "(no URL)")
        self.assertEqual(out[5], "cdn.example.com/<70 chars>/0123456789.0123456789.0123456789.0123456789.0123456789"
                                 ".0123456789...")
        self.assertEqual(len(out[5]), 95)
        self.assertEqual(out[6], "cdn....")

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
static esp_app_desc_t app = { .version = "1.0.0+g1234abcd" };
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

    def test_a_muse_gadget_ignores_setup_ota(self) -> None:
        # Link setup's ota_url and Link's BLE device.ota action on a Muse
        # Gadget: refused and logged, setup goes on, nothing is downloaded.
        app = (ROOT / "main/app.c").read_text()
        functions = "\n".join(function_source(app, signature) for signature in (
            "static bool start_wifi_join_ota_if_requested(",
            "static void on_ble_ota(",
        ))
        with tempfile.TemporaryDirectory() as name:
            tmp = Path(name)
            # OTA's own code stays out (it's off here): the refusal comes first either way.
            (tmp / "sdkconfig.h").write_text("#define CONFIG_MUSE_GADGET_BLE_AUDIO 1\n")
            (tmp / "esp_log.h").write_text(
                "#include <stdio.h>\n"
                "#define ESP_LOGI(tag, ...) (printf(\"%s: \", (tag)), printf(__VA_ARGS__), putchar('\\n'))\n"
                "#define ESP_LOGW ESP_LOGI\n#define ESP_LOGE ESP_LOGI\n")
            source = tmp / "setup.c"
            source.write_text('''
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include "ota.h"
#include "esp_app_desc.h"
#include "esp_log.h"
static const char *TAG = "link.app";
static esp_app_desc_t app = { .version = "1.0.0+g1234abcd" };
const esp_app_desc_t *esp_app_get_description(void) { return &app; }
static int statuses;
static bool wifi_mgr_is_connected(void) { assert(!"a refused OTA never asks for Wi-Fi"); return true; }
static void ble_server_send_status(const char *s) { assert(strcmp(s, "ota_skipped") == 0); statuses++; }
static bool ble_server_send_pairing_status(const char *s, uint32_t g) {
    (void)s; (void)g; assert(!"no pairing status"); return false;
}
static bool require_provisioning_pairing_session(uint32_t g) { (void)g; assert(!"no session check"); return false; }
static void ui_set_status(const char *s) { assert(strcmp(s, "ota_skipped") == 0); statuses++; }
static void ble_ota_status(const ota_event_t *ev, void *user) { (void)ev; (void)user; assert(!"no ota_start"); }
''' + functions + '''
int main(void) {
    (void)TAG;
    assert(!start_wifi_join_ota_if_requested("https://example.com/fw.bin?sig=hidden", true, 42));
    assert(!start_wifi_join_ota_if_requested("https://example.com/fw.bin", false, 0));
    assert(!start_wifi_join_ota_if_requested("", false, 0));   /* nothing asked: nothing logged */
    on_ble_ota("https://example.com/fw.bin", true);
    assert(statuses == 2);
    return 0;
}
''')
            out = self.compile_and_run([source, ROOT / "main/ota.c"], tmp)
        lines = [l for l in out.splitlines() if "refused" in l]
        self.assertEqual(len(lines), 3, out)
        self.assertIn("Link setup's ota_url refused: this Muse Gadget updates over Bluetooth", lines[0])
        self.assertIn("from example.com/fw.bin (query dropped)", lines[0])
        self.assertIn("Link's device.ota refused", lines[2])
        self.assertNotIn("hidden", out)

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

    def test_boot_leaves_a_bluetooth_update_to_smp(self) -> None:
        # With MCUmgr SMP in the build, a PENDING_VERIFY image a Wi-Fi OTA
        # didn't record is a Bluetooth update's: SMP keeps it at its own mark
        # and the health check never rolls it back; a Wi-Fi OTA's own still is.
        app = (ROOT / "main/app.c").read_text()
        start = app.index("    // Arm OTA rollback verification.")
        block = app[start : app.index("    xTaskCreate(ota_verify_task", start)]
        with tempfile.TemporaryDirectory() as name:
            tmp = Path(name)
            (tmp / "sdkconfig.h").write_text("#define CONFIG_MUSE_GADGET_BLE_SMP 1\n")
            source = tmp / "arm.c"
            source.write_text("""
#include <assert.h>
#include <stdbool.h>
#include "sdkconfig.h"
#include "esp_ota_ops.h"
#define TAG "test"
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
static const esp_partition_t part = { .address = 0x20000 };
static esp_ota_img_states_t state;
static bool vibe_mode, recorded, s_ota_pending_verify;
static int marked, forgotten;
const esp_partition_t *esp_ota_get_running_partition(void) { return &part; }
esp_err_t esp_ota_get_state_partition(const esp_partition_t *p, esp_ota_img_states_t *s) {
    assert(p == &part);
    *s = state;
    return ESP_OK;
}
esp_err_t esp_ota_mark_app_valid_cancel_rollback(void) { marked++; return ESP_OK; }
static bool ota_installed_by_vibe_mode(void) { return vibe_mode; }
static bool ota_verify_recorded_for_running(void) { return recorded; }
static void ota_verify_forget(void) { forgotten++; }
static void arm(esp_ota_img_states_t st, bool rec) {
    state = st; recorded = rec; vibe_mode = false;
    s_ota_pending_verify = false; marked = forgotten = 0;
""" + block + """
}
int main(void) {
    arm(ESP_OTA_IMG_PENDING_VERIFY, true);    /* a Wi-Fi OTA's: checked, rolled back if it fails */
    assert(s_ota_pending_verify && !marked && !forgotten);
    arm(ESP_OTA_IMG_PENDING_VERIFY, false);   /* a Bluetooth update's: SMP's, never rolled back here */
    assert(!s_ota_pending_verify && !marked && forgotten == 1);
    arm(ESP_OTA_IMG_VALID, false);
    assert(!s_ota_pending_verify && !marked);
    return 0;
}
""")
            self.compile_and_run([source], tmp)

    def test_a_bluetooth_update_on_a_wifi_gadget_is_kept_at_either_mark(self) -> None:
        # mg_glue.c's keep mark for an SMP image on a Wi-Fi + BLE build: the
        # control session up, or the boot guard's healthy mark (BLE reachable +
        # 30 s), whichever comes first, saved network or not (mg_ble adds the
        # 30 s floor). BLE-only builds keep the boot guard's mark (no hook).
        glue = (ROOT / "main/mg_glue.c").read_text()
        start = glue.index("static bool smp_keep_mark(void) {")
        fn = glue[start : glue.index("\n}\n", start) + 3]
        self.assertIn("#if CONFIG_MUSE_GADGET_BLE_SMP && !CONFIG_MUSE_GADGET_BLE_STANDALONE\n    platform.boot_healthy = smp_keep_mark;", glue)
        self.assertNotIn("s_no_wifi_saved", glue)
        with tempfile.TemporaryDirectory() as name:
            tmp = Path(name)
            (tmp / "sdkconfig.h").write_text("")
            source = tmp / "keep.c"
            source.write_text("""
#include <assert.h>
#include <stdbool.h>
static bool session_up, healthy, reachable, ota_recorded;
static bool noise_ctrl_is_connected(void) { return session_up; }
static bool ota_verify_recorded_for_running(void) { return ota_recorded; }
static bool muse_bootguard_is_healthy(void) { return healthy; }
static bool ble_server_ever_reachable(void) { return reachable; }
""" + fn + """
int main(void) {
    session_up = healthy = reachable = false; assert(!smp_keep_mark());
    session_up = true;                         assert(smp_keep_mark());   /* the control session first */
    session_up = false; healthy = reachable = true; assert(smp_keep_mark());   /* or BLE healthy, away from Wi-Fi */
    reachable = false;                         assert(!smp_keep_mark());
    healthy = false; reachable = true;         assert(!smp_keep_mark());   /* not before 30 s */
    /* An image device.ota installed has one verifier, its control session: never Bluetooth's mark first. */
    ota_recorded = true; healthy = reachable = true; session_up = false; assert(!smp_keep_mark());
    session_up = true;                         assert(smp_keep_mark());
    return 0;
}
""")
            self.compile_and_run([source], tmp)
        # The mark is asked only until it came (device.ota's record is a flash read), not every worker round.
        ble_c = (ROOT / "components/muse_gadget_ble/mg_ble.c").read_text()
        self.assertIn("bool mark = !(s_smp_marks & SMP_HEALTHY) &&\n"
                      "                (s_platform.boot_healthy ? s_platform.boot_healthy() : muse_bootguard_is_healthy());",
                      ble_c)


if __name__ == "__main__":
    unittest.main()
