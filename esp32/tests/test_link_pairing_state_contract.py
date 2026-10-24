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

import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
APP_C = ROOT / "main" / "app.c"
CONFIG_STORE_C = ROOT / "main" / "config_store.c"
WIFI_MGR_C = ROOT / "main" / "wifi_mgr.c"


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


class LinkPairingStateContractTest(unittest.TestCase):
    """Locks in the setup/pairing state machine: a failure at any stage wipes
    the partial pairing to a clean slate (saved Wi-Fi stays) and the device
    stays advertising for a clean re-pair (no half-paired state, no reboot)."""

    def test_setup_wipe_clears_all_state_and_disconnects_wifi(self) -> None:
        source = APP_C.read_text()
        body = _function_body(source, "static bool setup_wipe_to_clean(")
        disconnect = _function_body(source, "static void setup_disconnect_to_clean(")
        clear = _function_body(source, "static bool clear_setup_credentials(")

        # The single source of truth for "wipe partial pairing state": every
        # recovery path must drop transports, drop the radio, and clear the
        # pairing from NVS. Saved Wi-Fi networks are not pairing state.
        self.assertIn("setup_disconnect_to_clean()", body)
        self.assertIn("disconnect_vm_transports()", disconnect)
        self.assertIn("wifi_mgr_disconnect()", disconnect)
        self.assertIn("clear_setup_credentials()", body)
        self.assertIn("config_unpair()", clear)
        self.assertNotIn("config_clear_setup()", clear)
        # Device tokens now refresh on rejection, not on a timer. Reset every
        # part of the VM-auth retry cadence so a later pair starts fresh.
        locked = clear[clear.index("auth_lock_take()") : clear.index("auth_lock_give()")]
        self.assertIn("s_last_vm_auth_refresh_attempt_us = 0", locked)
        self.assertIn("s_vm_auth_refresh_interval_us = VM_AUTH_REFRESH_RETRY_INTERVAL_US", locked)
        self.assertIn("s_vm_auth_refusals = 0", locked)

    def test_setup_disconnect_clears_diagnostics_after_transport_teardown(self) -> None:
        source = APP_C.read_text()
        disconnect = _function_body(source, "static void setup_disconnect_to_clean(")
        order = [
            disconnect.index("disconnect_vm_transports()"),
            disconnect.index("wifi_mgr_disconnect()"),
            disconnect.index("diagnostic_log_clear()"),
        ]
        self.assertEqual(order, sorted(order))

        # Server unpair clears credentials before acknowledging the command;
        # its later teardown must use the same diagnostic cleanup as BLE reset.
        precleared = _function_body(
            source, "static void finish_precleared_setup_reset_with_gate_held("
        )
        self.assertIn("setup_disconnect_to_clean()", precleared)
        self.assertNotIn(
            "diagnostic_log_clear()",
            _function_body(source, "static void disconnect_vm_transports("),
        )
        self.assertNotIn(
            "diagnostic_log_clear()",
            _function_body(source, "static void on_client_disconnected("),
        )

    def test_setup_fail_wipes_reports_and_stays_advertising(self) -> None:
        body = _function_body(APP_C.read_text(), "static void setup_fail_for_session(")

        self.assertIn('setup_stage_set("failed")', body)
        self.assertIn("setup_wipe_to_clean()", body)
        self.assertIn("ble_server_send_status(reported_status)", body)
        self.assertIn('cleared ? status : "error_storage"', body)
        self.assertIn("led_status_set_state(LED_STATE_ERROR)", body)
        # A mid-setup failure must NOT reboot or tear BLE down — the app retries
        # on the same connection.
        self.assertNotIn("esp_restart", body)
        self.assertNotIn("restart_after_setup_reset", body)
        self.assertNotIn("ble_server_full_shutdown", body)
        self.assertNotIn("complete_setup_and_stop_ble", body)

    def test_provision_routes_every_failure_through_setup_fail(self) -> None:
        body = _function_body(APP_C.read_text(), "static void on_provision(")

        self.assertIn('setup_fail_for_session("wifi", "wifi_failed", session_generation)', body)
        self.assertIn('setup_fail_for_session("auth", "auth_failed", session_generation)', body)
        self.assertIn('setup_fail_for_session("vm", "auth_failed", session_generation)', body)
        # Only the fully-successful path completes setup and shuts BLE down.
        self.assertIn('setup_stage_set("done")', body)
        self.assertIn('complete_setup_and_stop_ble("provision", session_generation)', body)

    def test_provision_advances_stages_in_order(self) -> None:
        body = _function_body(APP_C.read_text(), "static void on_provision(")
        order = [
            body.index('setup_stage_set("wifi")'),
            body.index('setup_stage_set("auth")'),
            body.index('setup_stage_set("vm")'),
            body.index('setup_stage_set("done")'),
        ]
        self.assertEqual(order, sorted(order))

    def test_ble_unpair_clears_pairing_and_marker_but_keeps_wifi(self) -> None:
        source = APP_C.read_text()
        body = _function_body(source, "static void on_unpair(")
        reset = _function_body(source, "static bool reset_setup_with_gate_held(")
        unpair = _function_body(CONFIG_STORE_C.read_text(), "bool config_unpair(")

        # Unpair goes through the shared wipe, which also clears the
        # setup-complete marker: leaving that behind is what left a device
        # half-unpaired (pairing refused). Wi-Fi survives so it can pair again.
        self.assertIn('reset_setup_with_gate_held("BLE unpair")', body)
        self.assertIn("setup_wipe_to_clean()", reset)
        self.assertNotIn("config_clear_pairing()", reset)
        self.assertIn("config_clear_pairing()", unpair)
        self.assertIn("config_clear_setup_complete()", unpair)
        self.assertLess(unpair.index("config_clear_pairing()"),
                        unpair.index("config_clear_setup_complete()"))
        self.assertNotIn("ssid", unpair)
        self.assertNotIn("password", unpair)

    def test_wifi_failure_keeps_the_ble_retry_session(self) -> None:
        body = _function_body(APP_C.read_text(), "static void setup_fail_for_session(")
        wifi_branch = _function_body(body, "if (wifi_only)")

        self.assertIn("link_pairing_extend_provisioning_deadline(session_generation)", wifi_branch)
        self.assertNotIn("ble_server_stop_advertising", wifi_branch)
        self.assertNotIn("ble_server_delayed_disconnect", wifi_branch)
        self.assertNotIn("link_pairing_reset()", wifi_branch)

    def test_setup_stage_set_logs_transitions(self) -> None:
        body = _function_body(APP_C.read_text(), "static void setup_stage_set(")
        self.assertIn('"setup stage: %s -> %s"', body)
        self.assertIn("s_setup_stage = stage", body)

    def test_boot_wipes_partial_state_but_exempts_dev_override(self) -> None:
        body = _function_body(APP_C.read_text(), "void app_run(")

        self.assertIn("if (CONFIG_HOMEHUB_WIFI_SSID[0]) {", body)
        self.assertIn("partial setup at boot", body)

        # The boot-time partial-state wipe must live in the non-override branch:
        # a token-only NVS is valid for the CONFIG_HOMEHUB_WIFI_SSID dev path
        # (Wi-Fi comes from menuconfig), so it must not be wiped there.
        override_branch = _function_body(body, "if (CONFIG_HOMEHUB_WIFI_SSID[0])")
        self.assertNotIn("partial setup at boot", override_branch)
        self.assertNotIn("config_clear_setup()", override_branch)

    def test_boot_keeps_tokens_with_wifi_and_clears_tokens_without(self) -> None:
        body = _function_body(APP_C.read_text(), "void app_run(")
        # Tokens and Wi-Fi without the marker are kept, with pairing open,
        # until a token is accepted; boot must not mark them complete (a
        # refused device would restart into the same refusal). A token without
        # Wi-Fi is a partial pair, and only its pairing is cleared.
        kept = _function_body(body, "if (!setup_complete && provisioned && have_wifi)")
        self.assertNotIn("config_mark_setup_complete", kept)
        self.assertNotIn("config_clear", kept)
        partial = _function_body(body, "} else if (!setup_complete && provisioned)")
        self.assertIn("config_clear_pairing()", partial)
        self.assertNotIn("config_clear_setup()", body)

    def test_wifi_connect_polls_past_wifi_state(self) -> None:
        body = _function_body(WIFI_MGR_C.read_text(), "bool wifi_mgr_connect(")

        # Re-provisioning over a still-tearing-down STA returns ESP_ERR_WIFI_STATE
        # from esp_wifi_set_config; poll past it instead of a false wifi_failed.
        self.assertIn("ESP_ERR_WIFI_STATE", body)
        self.assertIn("esp_wifi_set_config(WIFI_IF_STA, &wc)", body)
        self.assertIn("if (err != ESP_ERR_WIFI_STATE) break", body)


if __name__ == "__main__":
    unittest.main()
