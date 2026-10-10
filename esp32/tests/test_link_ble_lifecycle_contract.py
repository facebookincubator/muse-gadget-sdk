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
import shlex
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
APP_C = ROOT / "main" / "app.c"
BLE_SERVER_C = ROOT / "main" / "ble_server.c"
CONFIG_STORE_C = ROOT / "main" / "config_store.c"


def _function_body(source: str, signature: str) -> str:
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 0
    for i in range(brace, len(source)):
        if source[i] == "{":
            depth += 1
        elif source[i] == "}":
            depth -= 1
            if depth == 0:
                return source[brace + 1 : i]
    raise AssertionError(f"could not find end of {signature}")


class LinkBleLifecycleContractTest(unittest.TestCase):
    def test_setup_incomplete_boot_advertises(self) -> None:
        source = APP_C.read_text()
        body = _function_body(source, "void app_run(")
        start_helper = _function_body(source, "static void start_ble_setup_server_if_needed(")
        open_window = _function_body(
            source, "static void open_setup_window(const char *reason) {"
        )
        enter_adv = _function_body(
            source, "static void enter_advertising_state(const char *reason) {"
        )

        self.assertIn("setup_complete = config_setup_complete()", body)
        self.assertIn("provisioned = config_is_provisioned()", body)
        self.assertIn("if (!setup_complete) {", body)
        setup_block = body[body.rindex("if (!setup_complete) {") :]
        # Unpaired devices advertise automatically at boot — no button press is
        # needed to become discoverable; the advertised confirmation policy gates setup.
        self.assertIn('open_setup_window("boot: unpaired")', setup_block)
        self.assertIn("schedule_scan_refresh(false, NULL)", setup_block)
        self.assertLess(
            setup_block.index('open_setup_window("boot: unpaired")'),
            setup_block.index("schedule_scan_refresh(false, NULL)"),
        )
        self.assertLess(
            body.index("button_init(on_button_short_press"),
            body.index('open_setup_window("boot: unpaired")'),
        )
        self.assertNotIn("LED_STATE_SETUP_IDLE", setup_block)
        self.assertNotIn('ui_set_ble("idle")', setup_block)
        # Paired devices keep BLE off.
        self.assertIn('ui_set_ble("off")', setup_block)
        self.assertIn("ble_server_start", start_helper)
        # Advertising is persistent while unpaired: no timed window, no auto-close.
        self.assertIn("start_ble_setup_server_if_needed()", open_window)
        self.assertIn("enter_advertising_state(reason)", open_window)
        self.assertNotIn("SETUP_ADVERTISING_WINDOW_MS", open_window)
        self.assertIn("ble_server_begin_advertising()", enter_adv)
        self.assertIn("LED_STATE_BLE_ADVERTISING", enter_adv)
        self.assertNotIn("setup_window_timeout_task", source)

    def test_successful_setup_marks_complete_and_stops_ble(self) -> None:
        source = APP_C.read_text()
        dispatch = _function_body(BLE_SERVER_C.read_text(), "static void dispatch_command_ex(")

        # BLE dispatch only reaches this callback through decrypted provision_v2.
        self.assertIn('decrypted && strcmp(act, "provision_v2") == 0', dispatch)
        self.assertIn('complete_setup_and_stop_ble("provision", session_generation)', source)
        complete = _function_body(source, "static bool complete_setup_and_stop_ble(")
        self.assertIn("config_mark_setup_complete()", complete)
        self.assertIn("ble_server_full_shutdown()", complete)
        advertise = _function_body(BLE_SERVER_C.read_text(), "static void start_advertising(void) {")
        self.assertIn("if (config_setup_complete()) return;", advertise)

    def test_auth_rejection_does_not_reopen_setup(self) -> None:
        body = _function_body(APP_C.read_text(), "static void handle_token_revoked_unlocked(")

        self.assertIn("config_clear_pairing()", body)
        self.assertNotIn("config_clear_setup", body)
        self.assertNotIn("config_clear_setup_complete", body)
        self.assertNotIn("ble_server_start", body)

    def test_long_press_resets_setup_and_reboots(self) -> None:
        source = APP_C.read_text()
        body = _function_body(source, "static void on_button_long_press(")
        reset = _function_body(source, "static bool reset_setup_with_gate_held(")
        restart = _function_body(source, "static bool reset_setup_from_control(")

        wipe = _function_body(source, "static bool setup_wipe_to_clean(")
        disconnect = _function_body(source, "static void setup_disconnect_to_clean(")
        clear = _function_body(source, "static bool clear_setup_credentials(")
        self.assertIn('reset_setup_from_control("button long-press")', body)
        self.assertIn("setup_wipe_to_clean()", reset)
        self.assertIn("clear_setup_credentials()", wipe)
        self.assertIn("config_clear_setup()", clear)
        self.assertIn("setup_disconnect_to_clean()", wipe)
        self.assertIn("disconnect_vm_transports()", disconnect)
        self.assertIn("wifi_mgr_disconnect()", disconnect)
        self.assertIn("operation_gate_take(portMAX_DELAY", restart)
        self.assertIn("restart_after_setup_reset()", restart)
        self.assertNotIn("on_unpair()", body)

    def test_confirmation_workers_keep_the_originating_session(self) -> None:
        app = APP_C.read_text()
        ble = BLE_SERVER_C.read_text()
        button = _function_body(app, "static void on_button_short_press(")
        finished = _function_body(app, "static void on_pairing_client_finished(")
        confirmed = _function_body(app, "static void notify_pairing_confirmed(")
        worker = _function_body(ble, "static void pairing_client_finished_task(")
        disconnected = _function_body(app, "static void on_client_disconnected(")

        self.assertIn("link_pairing_confirm_active_session()", button)
        self.assertIn("notify_pairing_confirmed(session_generation)", button)
        self.assertIn('open_setup_window("button short-press")', button)
        self.assertIn("link_pairing_session_is_current(generation)", worker)
        self.assertIn("s_cb.on_pairing_client_finished(generation)", worker)
        for handler, status in ((finished, "confirm_required"), (confirmed, "pairing_confirmed")):
            self.assertIn("link_pairing_session_is_current(session_generation)", handler)
            self.assertIn(f'ble_server_send_pairing_status("{status}", session_generation)', handler)
            self.assertIn("ble_server_disconnect_pairing_session(session_generation)", handler)
            # No UI lock may be held across the staggered BLE notifications.
            self.assertLess(handler.rindex("setup_window_lock_give()"),
                            handler.rindex("ble_server_send_pairing_status("))
        self.assertLess(finished.index('ble_server_send_pairing_status("confirm_required"'),
                        finished.index("link_pairing_arm_confirmation(session_generation)"))
        self.assertLess(finished.index("link_pairing_arm_confirmation(session_generation)"),
                        finished.index("LED_STATE_PAIRING_CONFIRM_REQUIRED"))
        self.assertIn("link_pairing_provisioning_session_valid(0)", confirmed)
        self.assertIn("LED_STATE_WIFI_CONNECTING", confirmed)
        self.assertLess(disconnected.index("led_status_set_state("),
                        disconnected.index("setup_window_lock_give()"))
        self.assertLess(disconnected.index("setup_window_lock_give()"),
                        disconnected.index("ble_server_begin_advertising()"))

    def test_confirmation_timeouts_cannot_disconnect_a_replacement_session(self) -> None:
        app = APP_C.read_text()
        ble = BLE_SERVER_C.read_text()
        start_timeout = _function_body(app, "static BaseType_t start_confirm_timeout(")
        self.assertIn("s_confirm_generation == generation", start_timeout)
        self.assertIn("link_pairing_session_is_current(session_generation)", start_timeout)
        for signature, timeout in (
            ("static void confirm_timeout_task(", "PAIRING_CONFIRM_WINDOW_MS"),
            ("static void confirmed_idle_timeout_task(void *arg) {", "PAIRING_CONFIRMED_IDLE_TIMEOUT_MS"),
        ):
            handler = _function_body(app, signature)
            self.assertIn(timeout, handler)
            self.assertIn("link_pairing_session_is_current(session_generation)", handler)
            self.assertIn("ble_server_disconnect_pairing_session(session_generation)", handler)
            self.assertNotIn("link_pairing_reset()", handler)
            self.assertNotIn("ble_server_stop_advertising(true)", handler)
        disconnect = _function_body(ble, "static void disconnect_pairing_session_event(")
        self.assertLess(disconnect.index("link_pairing_reset_session(generation)"),
                        disconnect.index("ble_server_stop_advertising(true)"))
        queue = _function_body(ble, "void ble_server_disconnect_pairing_session(")
        self.assertIn("nimble_port_get_dflt_eventq()", queue)

    def test_encrypted_record_allocation_and_send_share_tx_order(self) -> None:
        ble = BLE_SERVER_C.read_text()
        for signature, encryption in (
            ("static bool send_status(", "link_pairing_encrypt_status("),
            ("bool ble_server_send_encrypted_json(", "link_pairing_encrypt_json("),
        ):
            sender = _function_body(ble, signature)
            self.assertLess(sender.index("xSemaphoreTake(s_tx_mutex"), sender.index(encryption))
            self.assertLess(sender.index(encryption), sender.index("send_chunked_locked("))
            self.assertLess(sender.index("send_chunked_locked("), sender.index("xSemaphoreGive(s_tx_mutex)"))
        chunks = _function_body(ble, "static bool send_chunked_locked(")
        self.assertIn("link_pairing_record_session_is_current(record_generation)", chunks)
        self.assertNotIn("link_pairing_session_is_current", chunks)

    def test_chunked_transmit_checks_capacity_before_framing(self) -> None:
        cc = shlex.split(os.environ.get("CC", "cc"))
        if not cc or shutil.which(cc[0]) is None:
            self.skipTest("C compiler not available")
        ble = BLE_SERVER_C.read_text()
        constants = "\n".join(line for line in ble.splitlines() if line.startswith((
            "#define MAX_NOTIFY_CHUNK", "#define CHUNK_HEADER_BYTES", "#define CHUNK_MAGIC",
        )))
        body = _function_body(ble, "static bool send_chunked_locked(")
        harness = constants + r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define pdMS_TO_TICKS(ms) (ms)
#define vTaskDelay(ticks) ((void)(ticks))
static uint16_t s_mtu;
static bool s_shutting_down;
static char payload[221];
static size_t capacity, total_len, expected_chunks, sent_chunks, sent_bytes;
static bool link_pairing_record_session_is_current(uint32_t generation) {
    return generation == 1;
}
static bool notify_payload(const uint8_t *data, size_t len) {
    assert(s_mtu > 6 && len >= CHUNK_HEADER_BYTES);
    assert(len <= MAX_NOTIFY_CHUNK && len <= (size_t)s_mtu - 3);
    assert(data[0] == CHUNK_MAGIC && data[1] == sent_chunks);
    assert(data[2] == expected_chunks);
    size_t remaining = total_len - sent_bytes;
    size_t expected_len = remaining < capacity ? remaining : capacity;
    assert(len - CHUNK_HEADER_BYTES == expected_len);
    assert(!memcmp(data + CHUNK_HEADER_BYTES, payload + sent_bytes, expected_len));
    sent_bytes += expected_len;
    sent_chunks++;
    return true;
}
"""
        harness += "static bool send_chunked_locked(const char *data, uint32_t record_generation) {"
        harness += body + "}\n"
        harness += r"""
static void check(uint16_t mtu, size_t usable, size_t len) {
    s_mtu = mtu;
    capacity = usable;
    total_len = len;
    sent_chunks = sent_bytes = 0;
    for (size_t i = 0; i < len; i++) payload[i] = (char)('!' + i % 90);
    payload[len] = '\0';
    expected_chunks = usable ? (len ? (len + usable - 1) / usable : 1) : 0;
    assert(send_chunked_locked(payload, 1) == (usable != 0));
    assert(sent_chunks == expected_chunks);
    assert(sent_bytes == (usable ? len : 0));
}
int main(void) {
    const size_t lengths[] = {0, 1, 220};
    for (size_t i = 0; i < sizeof(lengths) / sizeof(lengths[0]); i++) {
        for (uint16_t mtu = 0; mtu <= 23; mtu++) {
            check(mtu, mtu > 6 ? mtu - 6 : 0, lengths[i]);
        }
        const struct { uint16_t mtu; size_t usable; } cases[] = {
            {64, 58}, {162, 156}, {163, 157}, {164, 157},
            {185, 157}, {247, 157}, {517, 157}, {UINT16_MAX, 157},
        };
        for (size_t j = 0; j < sizeof(cases) / sizeof(cases[0]); j++) {
            check(cases[j].mtu, cases[j].usable, lengths[i]);
        }
    }
    return 0;
}
"""
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "ble_chunk_capacity.c"
            binary = source.with_suffix("")
            source.write_text(harness)
            compiled = subprocess.run(
                [*cc, "-std=c11", "-Wall", "-Wextra", "-Werror",
                 str(source), "-o", str(binary)], capture_output=True, text=True,
            )
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            ran = subprocess.run([str(binary)], capture_output=True, text=True, timeout=5)
            self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)

    def test_chunked_receive_sizes_its_buffer_to_the_message(self) -> None:
        cc = shlex.split(os.environ.get("CC", "cc"))
        if not cc or shutil.which(cc[0]) is None:
            self.skipTest("C compiler not available")
        ble = BLE_SERVER_C.read_text()
        constants = "\n".join(line for line in ble.splitlines() if line.startswith((
            "#define MAX_RX_TOTAL_BYTES", "#define CHUNK_HEADER_BYTES", "#define CHUNK_MAGIC",
        )))
        harness = constants + r"""
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
static size_t largest_free_block;
static void *limited_malloc(size_t n) {
    return n > largest_free_block ? NULL : malloc(n);
}
#define malloc limited_malloc
static uint16_t s_mtu;
static uint8_t *s_rx_buf;
static size_t s_rx_len, s_rx_cap;
static uint8_t s_rx_total, s_rx_count, s_rx_next_idx;
"""
        harness += "static void rx_reset_locked(void) {"
        harness += _function_body(ble, "static void rx_reset_locked(") + "}\n"
        harness += ("static void handle_rx_write_locked(const uint8_t *data, size_t len,"
                    " uint8_t **complete, size_t *complete_len) {")
        harness += _function_body(ble, "static void handle_rx_write_locked(") + "}\n"
        harness += r"""
// Returns the reassembled length, or 0 when the message was dropped.
static size_t send_message(size_t len, size_t chunk) {
    static uint8_t message[MAX_RX_TOTAL_BYTES + 1], frame[CHUNK_HEADER_BYTES + 512];
    for (size_t i = 0; i < len; i++) message[i] = (uint8_t)(i * 7 + 1);
    size_t total = (len + chunk - 1) / chunk;
    uint8_t *complete = NULL;
    size_t complete_len = 0;
    for (size_t i = 0; i < total; i++) {
        size_t off = i * chunk;
        size_t flen = len - off < chunk ? len - off : chunk;
        frame[0] = CHUNK_MAGIC;
        frame[1] = (uint8_t)i;
        frame[2] = (uint8_t)total;
        memcpy(frame + CHUNK_HEADER_BYTES, message + off, flen);
        assert(!complete);
        handle_rx_write_locked(frame, flen + CHUNK_HEADER_BYTES, &complete, &complete_len);
    }
    if (!complete) return 0;
    assert(complete_len == len && !memcmp(complete, message, len));
    free(complete);
    return complete_len;
}
int main(void) {
    // The Muse app's credentials message, with the 7 KB largest free block
    // the FoloToy AI Passport has while pairing.
    s_mtu = 247;
    largest_free_block = 7168;
    assert(send_message(2800, 241) == 2800);
    // The overall limit stays.
    largest_free_block = SIZE_MAX;
    assert(send_message(MAX_RX_TOTAL_BYTES, 241) == MAX_RX_TOTAL_BYTES);
    assert(send_message(MAX_RX_TOTAL_BYTES + 1, 241) == 0);
    // Chunks longer than one ATT write, and chunks before the MTU is known.
    assert(send_message(1500, 500) == 1500);
    s_mtu = 23;
    assert(send_message(2800, 241) == 2800);
    return 0;
}
"""
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "ble_rx_reassembly.c"
            binary = source.with_suffix("")
            source.write_text(harness)
            compiled = subprocess.run(
                [*cc, "-std=c11", "-Wall", "-Wextra", "-Werror",
                 str(source), "-o", str(binary)], capture_output=True, text=True,
            )
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            ran = subprocess.run([str(binary)], capture_output=True, text=True, timeout=5)
            self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)

    def test_ble_shutdown_stops_nimble_and_blocks_advertising_restart(self) -> None:
        source = BLE_SERVER_C.read_text()
        shutdown = _function_body(source, "void ble_server_full_shutdown(")
        start_adv = _function_body(source, "static void start_advertising(void) {")
        gap = _function_body(source, "static int gap_event_cb(")
        begin_adv = _function_body(source, "void ble_server_begin_advertising(")
        stop_adv = _function_body(source, "void ble_server_stop_advertising(")

        self.assertIn("s_shutting_down = true", shutdown)
        self.assertIn("s_advertising_enabled = false", shutdown)
        self.assertIn("ble_gap_adv_stop()", shutdown)
        self.assertIn("ble_gap_terminate", shutdown)
        self.assertIn("nimble_port_stop()", shutdown)
        self.assertIn("nimble_port_deinit()", shutdown)
        self.assertIn("if (s_shutting_down) return", start_adv)
        self.assertIn("if (!s_advertising_enabled) return", start_adv)
        self.assertIn("if (!s_shutting_down && s_advertising_enabled) start_advertising()", gap)
        self.assertIn("s_advertising_enabled = true", begin_adv)
        self.assertIn("s_advertising_enabled = false", stop_adv)
        self.assertIn("ble_gap_adv_stop()", stop_adv)

    def test_ble_server_copies_callbacks_instead_of_borrowing_stack_pointer(self) -> None:
        source = BLE_SERVER_C.read_text()
        start = _function_body(source, "void ble_server_start(")

        self.assertIn("static ble_callbacks_t s_cb", source)
        self.assertNotIn("static const ble_callbacks_t *s_cb", source)
        self.assertIn("s_cb = *cb", start)
        self.assertNotIn("s_cb = cb", start)

    def test_plaintext_set_wifi_is_rejected(self) -> None:
        ble = BLE_SERVER_C.read_text()
        dispatch = _function_body(ble, "static void dispatch_command_ex(")
        sensitive = _function_body(ble, "static bool is_sensitive_setup_action(")

        self.assertIn('strcmp(act, "set_wifi") == 0', sensitive)
        self.assertIn("link_pairing_plaintext_setup_blocked()", dispatch)
        self.assertIn('ble_server_send_status("error_encryption_required")', dispatch)

    def test_encrypted_provision_accepts_credentials_before_optional_ota_and_skips_vm_on_ota(self) -> None:
        app = APP_C.read_text()
        ble = BLE_SERVER_C.read_text()
        header = (ROOT / "main" / "ble_server.h").read_text()

        self.assertIn("typedef void (*ble_provision_cb)(const char *ssid, const char *password,", header)
        self.assertIn("const char *ota_url,", header)
        self.assertIn("bool ota_force", header)

        provision = _function_body(app, "static void on_provision(")
        self.assertIn("const char *ota_url", app)
        # 565462d deliberately moved OTA after credential acceptance: persist
        # tokens before the OTA reboot so the device auto-connects afterward
        # without requiring a second BLE pairing.
        credentials = "accept_pairing_credentials(access_token, refresh_token, username)"
        ota = "start_wifi_join_ota_if_requested(ota_url, ota_force, session_generation)"
        self.assertLess(
            provision.index('setup_stage_set("auth")'),
            provision.index(credentials),
        )
        self.assertLess(provision.index(credentials), provision.index(ota))
        # A started OTA short-circuits VM connection while it updates/reboots.
        self.assertLess(
            provision.index(ota),
            provision.index('setup_stage_set("vm")'),
        )
        self.assertRegex(
            provision,
            r"if \(start_wifi_join_ota_if_requested\(ota_url, ota_force, session_generation\)\)\s*"
            r"\{\s*goto done;\s*\}",
        )

        dispatch = _function_body(ble, "static void dispatch_command_ex(")
        self.assertIn('decrypted && strcmp(act, "provision_v2") == 0', dispatch)
        self.assertIn("const char *ota_url = optional_ota_url(root)", dispatch)
        self.assertIn("a->ota_url = dup_str(ota_url)", dispatch)
        self.assertIn("a->ota_force = optional_ota_force(root)", dispatch)

        # Incomplete setup is handled by the normal boot path, so no dedicated
        # reopen-after-reboot flag is needed.
        self.assertNotIn("s_open_setup_window_after_reboot", app)

    def test_config_setup_reset_clears_wifi_and_marker(self) -> None:
        body = _function_body(CONFIG_STORE_C.read_text(), "bool config_clear_setup(")

        self.assertIn('"ssid"', body)
        self.assertIn('"password"', body)
        self.assertIn('"wifi_channel"', body)
        self.assertIn('"setup_complete"', body)
        self.assertIn("clear_and_verify", body)


if __name__ == "__main__":
    unittest.main()
