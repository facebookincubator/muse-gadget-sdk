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

import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
APP_C = ROOT / "main" / "app.c"
BLE_SERVER_C = ROOT / "main" / "ble_server.c"
LINK_PAIRING_C = ROOT / "main" / "link_pairing.c"
LINK_PAIRING_H = ROOT / "main" / "link_pairing.h"
TRANSCRIPT_H = ROOT / "main" / "pairing_transcript.h"
KCONFIG = ROOT / "main" / "Kconfig.projbuild"


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


class LinkEncryptedPairingContractTest(unittest.TestCase):
    def test_plaintext_setup_is_hard_rejected(self) -> None:
        source = BLE_SERVER_C.read_text()
        sensitive = _function_body(source, "static bool is_sensitive_setup_action(")
        dispatch = _function_body(source, "static void dispatch_command_ex(")

        self.assertIn('strcmp(act, "wifi_scan") == 0', sensitive)
        self.assertIn('strcmp(act, "ota") == 0', sensitive)
        self.assertIn('strcmp(act, "device.ota") == 0', sensitive)
        self.assertIn('strcmp(act, "unpair") == 0', sensitive)
        self.assertIn('strcmp(act, "set_wifi") == 0', sensitive)
        self.assertIn('strcmp(act, "set_auth") == 0', sensitive)
        self.assertIn('decrypted && strcmp(act, "wifi_scan") == 0', dispatch)
        self.assertIn("link_pairing_plaintext_setup_blocked()", dispatch)
        self.assertIn('ble_server_send_status("error_encryption_required")', dispatch)
        self.assertLess(
            dispatch.index("link_pairing_plaintext_setup_blocked()"),
            dispatch.index('decrypted && strcmp(act, "wifi_scan") == 0'),
        )
        scan = _function_body(dispatch, 'decrypted && strcmp(act, "wifi_scan") == 0')
        self.assertIn('xTaskCreate(scan_task, "scan", 4096, NULL, 5, NULL)', scan)
        for action in ("wifi_scan", "set_wifi", "set_auth"):
            self.assertNotIn(
                f'}} else if (strcmp(act, "{action}") == 0)', dispatch
            )

        pairing = LINK_PAIRING_C.read_text()
        blocker = _function_body(pairing, "bool link_pairing_plaintext_setup_blocked(")
        self.assertIn("return true", blocker)
        combined = KCONFIG.read_text() + pairing
        self.assertNotIn("CONFIG_HOMEHUB_PAIRING_REQUIRED", combined)
        self.assertIn("#define PAIRING_VERSION 5", TRANSCRIPT_H.read_text())
        # Pairing uses no shared fleet HMAC secret.
        self.assertNotIn("CONFIG_HOMEHUB_PAIRING_FLEET_SECRET_HEX", combined)
        self.assertIn("CONFIG_HOMEHUB_PAIRING_AUTH_EPOCH", combined)
        self.assertNotIn("BACKEND_PUBKEY", combined)
        self.assertNotIn("pairing_server_attestation", BLE_SERVER_C.read_text())

    def test_plaintext_status_allowlist_is_minimal(self) -> None:
        source = BLE_SERVER_C.read_text()
        allowlist = _function_body(source, "static bool plaintext_status_allowed(")
        send_status = _function_body(source, "static bool send_status(")

        self.assertEqual(
            set(re.findall(r'strcmp\(status, "([^"]+)"\)', allowlist)),
            {
                "error_encryption_required",
                "error_pairing_invalid_hello",
                "error_pairing_unavailable",
                "error_pairing_decrypt",
            },
        )
        self.assertIn("link_pairing_encrypt_status(status, generation, &record_generation)", send_status)
        self.assertLess(
            send_status.index("link_pairing_encrypt_status(status, generation, &record_generation)"),
            send_status.index("!plaintext_status_allowed(status)"),
        )
        self.assertIn("s_plaintext_status_blocked || !plaintext_status_allowed(status)", send_status)
        self.assertIn("plaintext status suppressed", send_status)
        self.assertNotIn("version:", allowlist)
        self.assertNotIn("error_unknown_action", allowlist)
        self.assertNotIn("error_invalid_command", allowlist)
        self.assertNotIn("error_message_too_large", allowlist)
        self.assertNotIn("error_pairing_auth_failed", allowlist)

    def test_plaintext_statuses_close_after_pairing_ready_until_disconnect(self) -> None:
        source = BLE_SERVER_C.read_text()
        dispatch = _function_body(source, "static void dispatch_command_ex(")
        status = _function_body(source, "static bool send_status(")
        gap = _function_body(source, "static int gap_event_cb(")
        reset = _function_body(source, "static void on_reset(")

        self.assertIn("static bool s_plaintext_status_blocked = false;", source)
        self.assertIn("s_plaintext_status_blocked = true;", dispatch)
        self.assertLess(
            dispatch.index("s_plaintext_status_blocked = true;"),
            dispatch.index("ble_server_send_chunked(reply)"),
        )
        self.assertIn("s_plaintext_status_blocked || !plaintext_status_allowed(status)", status)
        self.assertLess(
            status.index("s_plaintext_status_blocked || !plaintext_status_allowed(status)"),
            status.index("notify_payload((const uint8_t *)s_last_status"),
        )
        self.assertIn('!decrypted && s_plaintext_status_blocked', dispatch)
        self.assertLess(
            dispatch.index('strcmp(act, "pairing_encrypted") == 0'),
            dispatch.index("!decrypted && s_plaintext_status_blocked"),
        )
        self.assertLess(
            dispatch.index("!decrypted && s_plaintext_status_blocked"),
            dispatch.index("link_pairing_plaintext_setup_blocked()"),
        )
        self.assertGreaterEqual(gap.count("s_plaintext_status_blocked = false;"), 2)
        self.assertIn("s_plaintext_status_blocked = false;", reset)

    def test_encrypted_provision_v2_requires_device_token_pair(self) -> None:
        source = BLE_SERVER_C.read_text()
        dispatch = _function_body(source, "static void dispatch_command_ex(")

        self.assertIn('decrypted && strcmp(act, "provision_v2") == 0', dispatch)
        self.assertIn('cJSON_GetObjectItem(root, "token_type")', dispatch)
        self.assertIn("link_pairing_mark_provisioning_active()", dispatch)
        self.assertIn('strcmp(tt->valuestring, "device") != 0', dispatch)
        self.assertIn('!cJSON_IsString(rt) || !rt->valuestring || !*rt->valuestring', dispatch)

        provision_v2_block = _function_body(
            dispatch, 'decrypted && strcmp(act, "provision_v2") == 0'
        )
        # The confirmed session replaces the removed shared client proof.
        self.assertNotIn("client_proof", provision_v2_block)
        self.assertNotIn("verify_client_proof_or_reset", provision_v2_block)
        self.assertNotIn('cJSON_GetObjectItem(root, "token")', provision_v2_block)
        self.assertLess(
            provision_v2_block.index("link_pairing_mark_provisioning_active()"),
            provision_v2_block.index("helper_task_create(provision_task"),
        )
        self.assertIn('!= pdPASS', provision_v2_block)
        self.assertIn("secure_free_str(a->password)", provision_v2_block)
        self.assertIn("secure_free_str(a->access_token)", provision_v2_block)
        self.assertIn("secure_free_str(a->refresh_token)", provision_v2_block)
        self.assertIn("secure_free_str(a->api_url)", provision_v2_block)
        self.assertIn("secure_free_str(a->api_url_v2)", provision_v2_block)
        self.assertIn("secure_free_str(a->noise_host)", provision_v2_block)

    def test_decrypted_setup_actions_require_confirmed_session(self) -> None:
        source = BLE_SERVER_C.read_text()
        confirm_gate = _function_body(source, "static bool is_sensitive_setup_action(")
        dispatch = _function_body(source, "static void dispatch_command_ex(")

        for action in [
            "provision",
            "provision_v2",
            "wifi_scan",
            "ota",
            "device.ota",
            "unpair",
            "set_wifi",
            "set_auth",
        ]:
            self.assertIn(f'strcmp(act, "{action}") == 0', confirm_gate + source)

        self.assertIn("decrypted && is_sensitive_setup_action(act)", dispatch)
        self.assertIn("!link_pairing_session_confirmed()", dispatch)
        self.assertIn('ble_server_send_status("error_pairing_confirm_required")', dispatch)
        self.assertLess(
            dispatch.index("} else if (decrypted && is_sensitive_setup_action(act)"),
            dispatch.index('decrypted && strcmp(act, "wifi_scan") == 0'),
        )
        self.assertLess(
            dispatch.index("} else if (decrypted && is_sensitive_setup_action(act)"),
            dispatch.index('decrypted && strcmp(act, "provision_v2") == 0'),
        )


    def test_factory_signature_uses_strict_pairing_signer_policy(self) -> None:
        pairing = LINK_PAIRING_C.read_text()
        factory = _function_body(
            pairing, "const char *link_pairing_sign_factory_test("
        )

        self.assertIn("pairing_signer_t signer = pairing_signer()", factory)
        self.assertIn("signer == PAIRING_SIGNER_COMMUNITY", factory)
        self.assertIn("signer != PAIRING_SIGNER_EFUSE", factory)
        self.assertNotIn("esp_efuse_key_block_unused", factory)
        self.assertNotIn("psa_import_key", factory)
        self.assertNotIn("psa_sign_hash", factory)
        self.assertIn("hardware_sign_hash(hash, sig)", factory)
        self.assertLess(
            factory.index("signer != PAIRING_SIGNER_EFUSE"),
            factory.index("hardware_sign_hash(hash, sig)"),
        )

    def test_all_hardware_signatures_share_determinism_guard(self) -> None:
        pairing = LINK_PAIRING_C.read_text()
        hardware = _function_body(pairing, "static bool hardware_sign_hash(")
        transcript = _function_body(pairing, "static bool sign_transcript_ecdsa(")
        factory = _function_body(
            pairing, "const char *link_pairing_sign_factory_test("
        )

        self.assertEqual(hardware.count("psa_sign_hash("), 2)
        self.assertIn("memcmp(out_sig, sig2", hardware)
        self.assertIn("hardware_sign_hash(s_transcript_hash, out_sig)", transcript)
        self.assertIn("hardware_sign_hash(hash, sig)", factory)

    def test_pairing_ready_requires_authenticated_finished_and_policy_confirmation(self) -> None:
        pairing = LINK_PAIRING_C.read_text()
        header = LINK_PAIRING_H.read_text()
        hello = _function_body(pairing, "const char *link_pairing_handle_client_hello(")
        finished = _function_body(pairing, "uint32_t link_pairing_handle_client_finished(")
        confirm = _function_body(pairing, "uint32_t link_pairing_confirm_active_session(")
        encrypt = _function_body(pairing, "static char *encrypt_json_for_session(")

        self.assertIn("PAIRING_WAIT_CLIENT_FINISHED", pairing)
        self.assertIn("PAIRING_CONFIRM_REQUIRED", pairing)
        self.assertIn("PAIRING_CLIENT_FINISHED_TIMEOUT_US", pairing)
        self.assertIn("PAIRING_CONFIRM_TIMEOUT_US", pairing)
        self.assertIn("link_pairing_handle_client_finished", header)
        self.assertIn("link_pairing_confirmation_required", header)
        self.assertIn("link_pairing_confirm_active_session", header)
        self.assertIn("link_pairing_session_confirmed", header)
        # client_proof is gone from both the header and implementation.
        self.assertNotIn("link_pairing_verify_client_proof", header)
        self.assertNotIn("link_pairing_client_authenticated", header)
        self.assertIn("s_state = PAIRING_WAIT_CLIENT_FINISHED", hello)
        self.assertIn(
            "s_deadline_us = esp_timer_get_time() + PAIRING_CLIENT_FINISHED_TIMEOUT_US",
            hello,
        )
        self.assertIn("s_state == PAIRING_WAIT_CLIENT_FINISHED", finished)
        self.assertIn("s_rx_counter == 1", finished)
        self.assertNotIn("verify_client_proof", finished)
        self.assertIn("s_state = PAIRING_CONFIRM_REQUIRED", finished)
        self.assertIn(
            "s_deadline_us = esp_timer_get_time() + PAIRING_CONFIRM_TIMEOUT_US",
            finished,
        )
        self.assertIn("s_state = PAIRING_READY", confirm)
        self.assertIn("s_deadline_us = esp_timer_get_time() + PAIRING_TIMEOUT_US", confirm)
        self.assertIn("session_keys_available_locked()", encrypt)

    def test_confirmation_ui_starts_only_after_client_finished(self) -> None:
        ble = BLE_SERVER_C.read_text()
        dispatch = _function_body(ble, "static void dispatch_command_ex(")
        exact_finished = _function_body(ble, "static bool is_exact_client_finished(")
        hello_block = dispatch[
            dispatch.index('strcmp(act, "pairing_client_hello") == 0') :
            dispatch.index('strcmp(act, "pairing_encrypted") == 0')
        ]
        finished_block = dispatch[
            dispatch.index('decrypted && strcmp(act, "pairing_client_finished") == 0') :
            dispatch.index("} else if (decrypted && is_sensitive_setup_action(act)")
        ]

        self.assertNotIn("pairing_client_finished_task", hello_block)
        self.assertNotIn('cJSON_GetObjectItem(root, "client_proof")', finished_block)
        self.assertIn("is_exact_client_finished(root)", finished_block)
        self.assertIn("link_pairing_handle_client_finished", finished_block)
        self.assertIn("pairing_client_finished_task", finished_block)
        self.assertLess(
            finished_block.index("link_pairing_handle_client_finished"),
            finished_block.index("pairing_client_finished_task"),
        )
        self.assertIn("!= pdPASS", finished_block)
        self.assertIn('ble_server_send_pairing_status("error_pairing_unavailable", generation)', finished_block)
        self.assertIn('ble_server_send_status("error_pairing_decrypt")', finished_block)
        self.assertIn("link_pairing_reset()", finished_block)
        self.assertIn("root->child", exact_finished)
        self.assertIn("!only->next", exact_finished)
        self.assertIn('strcmp(only->string, "action") == 0', exact_finished)

    def test_counter_parsing_is_string_based_and_fail_closed(self) -> None:
        pairing = LINK_PAIRING_C.read_text()
        parser = _function_body(pairing, "static bool parse_u64_decimal(")
        decrypt = _function_body(pairing, "const char *link_pairing_decrypt_command(")
        envelope = _function_body(pairing, "static char *make_envelope(")

        self.assertIn("cJSON_AddStringToObject(root, \"counter\"", envelope)
        self.assertIn('json_string(root, "counter", &counter_str)', decrypt)
        self.assertNotIn("cJSON_IsNumber", decrypt)
        self.assertNotIn("valuedouble", decrypt)
        self.assertIn("*p < '0' || *p > '9'", parser)
        self.assertIn("UINT64_MAX", parser)
        self.assertIn("counter != s_rx_counter", decrypt)

    def test_pairing_state_is_cleared_on_disconnect_host_reset_and_errors(self) -> None:
        ble = BLE_SERVER_C.read_text()
        gap = _function_body(ble, "static int gap_event_cb(")
        reset = _function_body(ble, "static void on_reset(")

        self.assertIn("rx_reset()", gap)
        self.assertIn("link_pairing_reset()", gap)
        self.assertIn("rx_reset()", reset)
        self.assertIn("link_pairing_reset()", reset)
        # The shared-secret client_proof helper is gone entirely.
        self.assertNotIn("verify_client_proof_or_reset", ble)

    def test_decrypted_json_and_task_secrets_are_zeroized(self) -> None:
        ble = BLE_SERVER_C.read_text()
        delete_json = _function_body(ble, "static void delete_command_json(")
        wipe = _function_body(ble, "static void wipe_json_strings(")
        provision_task = _function_body(ble, "static void provision_task(")
        dispatch = _function_body(ble, "static void dispatch_command_ex(")

        self.assertIn("wipe_json_strings(root)", delete_json)
        self.assertIn("mbedtls_platform_zeroize(cur->valuestring", wipe)
        self.assertIn("secure_free_str(a->password)", provision_task)
        self.assertIn("secure_free_str(a->access_token)", provision_task)
        self.assertIn("mbedtls_platform_zeroize(plain", dispatch)

    def test_wifi_scan_results_require_encrypted_pairing(self) -> None:
        app = APP_C.read_text()
        header = LINK_PAIRING_H.read_text()
        scan = _function_body(app, "static bool send_cached_scan_to_client(")
        on_scan = _function_body(app, "static void on_wifi_scan(")
        refresh = _function_body(app, "static void scan_refresh_task(")
        on_connect = _function_body(app, "static void on_client_connected(")

        self.assertIn("link_pairing_encrypt_json", header)
        self.assertIn("link_pairing_session_confirmed", header)
        self.assertIn("link_pairing_session_confirmed()", scan)
        self.assertIn("ble_server_send_encrypted_json(json, session_generation)", scan)
        self.assertLess(scan.index("link_pairing_session_generation()"),
                        scan.index("link_pairing_session_confirmed()"))
        self.assertNotIn("ble_server_send_chunked(json)", scan)
        self.assertIn("send_cached_scan_or_error()", on_scan)
        self.assertIn("schedule_scan_refresh(true, &joined)", on_scan)
        self.assertIn("if (!joined)", on_scan)
        self.assertIn("if (total > 0)", refresh)
        self.assertIn("completed_flags & SCAN_REFRESH_REPLY_PENDING", refresh)
        self.assertIn("(void)send_cached_scan_to_client()", refresh)
        self.assertLess(
            refresh.index("if (total > 0)"),
            refresh.index("send_cached_scan_to_client"),
        )
        self.assertNotIn("scan_refresh_task", on_connect)

    def test_encrypted_provisioning_extends_pairing_session(self) -> None:
        ble = BLE_SERVER_C.read_text()
        pairing = LINK_PAIRING_C.read_text()
        header = LINK_PAIRING_H.read_text()
        status = _function_body(ble, "static bool send_status(")
        encrypt = _function_body(pairing, "static char *encrypt_json_for_session(")
        active = _function_body(pairing, "uint32_t link_pairing_mark_provisioning_active(")

        self.assertIn("link_pairing_mark_provisioning_active", header)
        self.assertIn("link_pairing_encrypt_status(status, generation, &record_generation)", status)
        self.assertIn("!plaintext_status_allowed(status)", status)
        self.assertIn("plaintext status suppressed", status)
        self.assertIn("session_keys_available_locked()", pairing)
        self.assertIn("session_confirmed_locked()", pairing)
        self.assertLess(
            status.index("link_pairing_encrypt_status(status, generation, &record_generation)"),
            status.index("!plaintext_status_allowed(status)"),
        )
        self.assertLess(
            status.index("!plaintext_status_allowed(status)"),
            status.index("notify_payload((const uint8_t *)s_last_status"),
        )
        self.assertIn("PAIRING_PROVISIONING", pairing)
        self.assertIn("PAIRING_PROVISIONING_TIMEOUT_US", pairing)
        self.assertIn("s_state = PAIRING_PROVISIONING", active)
        self.assertIn("s_deadline_us = esp_timer_get_time() + PAIRING_PROVISIONING_TIMEOUT_US", active)
        self.assertIn("session_keys_available_locked()", encrypt)

    def test_encrypted_provisioning_aborts_when_session_expires(self) -> None:
        app = APP_C.read_text()
        pairing = LINK_PAIRING_C.read_text()
        header = LINK_PAIRING_H.read_text()
        require = _function_body(app, "static bool require_provisioning_pairing_session(")
        on_provision = _function_body(app, "static void on_provision(")
        valid = _function_body(pairing, "bool link_pairing_provisioning_session_valid(")

        self.assertIn("link_pairing_provisioning_session_valid", header)
        self.assertIn("link_pairing_provisioning_session_valid(session_generation)", require)
        self.assertIn('setup_fail_for_session("pairing", "auth_failed", session_generation)', require)
        self.assertIn("!expired_locked()", valid)
        self.assertIn("s_state == PAIRING_PROVISIONING", valid)
        # Provisioning validity no longer depends on a symmetric client proof.
        self.assertNotIn("s_client_authenticated", valid)
        self.assertGreaterEqual(
            on_provision.count("require_provisioning_pairing_session(session_generation)"),
            5,
        )
        self.assertLess(
            on_provision.index("link_pairing_provisioning_session_valid(session_generation)"),
            on_provision.index('setup_stage_set("wifi")'),
        )
        ble = BLE_SERVER_C.read_text()
        worker = _function_body(ble, "static void provision_task(")
        self.assertIn("a->session_generation = link_pairing_mark_provisioning_active()", ble)
        self.assertIn("link_pairing_provisioning_session_valid(a->session_generation)", worker)
        self.assertIn("a->session_generation);", worker)
        self.assertNotIn("ble_server_send_status(", on_provision)
        completion = _function_body(app, "static bool complete_setup_and_stop_ble(")
        self.assertIn("link_pairing_commit_provisioning(session_generation, config_mark_setup_complete)", completion)
        failure = _function_body(app, "static void setup_fail_for_session(")
        self.assertIn("!link_pairing_session_is_current(session_generation)", failure)
        self.assertIn("ble_server_disconnect_pairing_session(session_generation)", failure)

    def test_ble_ota_and_unpair_require_encrypted_confirmed_session(self) -> None:
        source = BLE_SERVER_C.read_text()
        sensitive = _function_body(source, "static bool is_sensitive_setup_action(")
        dispatch = _function_body(source, "static void dispatch_command_ex(")
        ota_block = dispatch[
            dispatch.index('decrypted && strcmp(act, "device.ota") == 0') :
            dispatch.index('} else if (decrypted && strcmp(act, "unpair") == 0)')
        ]
        unpair_block = dispatch[
            dispatch.index('decrypted && strcmp(act, "unpair") == 0') :
            dispatch.index('} else if (decrypted && strcmp(act, "provision_v2") != 0)')
        ]

        self.assertIn('strcmp(act, "ota") == 0', sensitive)
        self.assertIn('strcmp(act, "device.ota") == 0', sensitive)
        self.assertNotIn('strcmp(act, "ota") == 0', dispatch)
        self.assertIn('decrypted && strcmp(act, "device.ota") == 0', ota_block)
        self.assertNotIn("verify_client_proof_or_reset", ota_block)
        self.assertNotIn("client_proof", ota_block)
        self.assertIn('xTaskCreate(ble_ota_task, "ble_ota"', ota_block)
        self.assertNotIn("verify_client_proof_or_reset", unpair_block)
        self.assertNotIn("client_proof", unpair_block)
        self.assertIn('xTaskCreate(unpair_task, "unpair"', unpair_block)

    def test_ble_set_vm_is_not_supported(self) -> None:
        ble = BLE_SERVER_C.read_text()
        header = (ROOT / "main" / "ble_server.h").read_text()

        self.assertNotIn('strcmp(act, "set_vm")', ble)
        self.assertNotIn("set_vm_task", ble)
        self.assertNotIn("on_set_vm", header)


if __name__ == "__main__":
    unittest.main()
