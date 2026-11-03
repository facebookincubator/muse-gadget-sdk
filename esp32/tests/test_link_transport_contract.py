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
NOISE_CONTROL_CPP = ROOT / "main" / "noise_control.cpp"
NOISE_TUNNEL_CPP = ROOT / "main" / "noise_tunnel.cpp"


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


class LinkTransportContractTest(unittest.TestCase):
    def test_app_connects_control_and_multiplexes_data_plane(self) -> None:
        source = APP_C.read_text()
        connect = _function_body(source, "static bool connect_vm_target(")
        session = _function_body(NOISE_CONTROL_CPP.read_text(), "static session_result_t run_session(")

        # ID derivation and per-entry credential selection are exercised by
        # link_vm_connect_harness.c against vm_connect.h. Pin the app wiring:
        # refuse unusable entries, then pass the resolved pair to control.
        params = "if (!vm_connect_params(target, vm_id, sizeof(vm_id), &auth_token))"
        self.assertIn("return false;", _function_body(connect, params))
        self.assertLess(connect.index(params), connect.index("noise_ctrl_connect("))
        self.assertIn(
            "bool ctrl_started = noise_ctrl_connect(vm_id, auth_token, network_ssid);",
            connect,
        )
        failure = _function_body(connect, "if (!ctrl_started)")
        self.assertIn("return false;", failure)
        registered = _function_body(session, "if (!tunnel_opened && s_register_acked)")
        self.assertIn("noise_tunnel_on_session_up(&tun_emit)", registered)
        failure = _function_body(registered, "if (!noise_tunnel_on_session_up(&tun_emit))")
        # A failed stream open alone does not tear down control. A poisoned
        # Noise session must still reconnect via the event loop's health check.
        self.assertNotIn("goto cleanup", failure)
        self.assertNotIn("return", failure)
        self.assertNotIn("error = true", failure)
        self.assertNotIn("break;", failure)
        self.assertIn("noise_tunnel_maybe_reopen(&tun_emit)", session)
        unhealthy = _function_body(session, "if (!session.isEstablished())")
        self.assertIn("error = true;", unhealthy)
        self.assertIn("break;", unhealthy)

    def test_app_initializes_control_and_logs_both_transport_states(self) -> None:
        source = APP_C.read_text()
        self.assertIn("noise_ctrl_init(identity_node_id(), identity_ble_name(), on_ws_control_status)", source)
        self.assertIn("noise_ctrl_set_command_cb(on_ws_command)", source)
        self.assertIn('"hb t=%llds setup=%s wifi=%s ws=%s raw=%s ble=%s "', source)
        self.assertIn('noise_ctrl_is_connected() ? "up" : "down"', source)
        self.assertIn('noise_tunnel_is_connected() ? "up" : "down"', source)
        netif = (ROOT / "main" / "tunnel_netif.c").read_text()
        self.assertIn("noise_tunnel_set_packet_cb(on_tunnel_packet)", netif)

    def test_tunnel_state_controls_netif_link(self) -> None:
        source = APP_C.read_text()
        ws_status = _function_body(source, "static void on_ws_control_status(")
        connected = _function_body(ws_status, 'if (strcmp(status, "ws_connected") == 0)')
        self.assertNotIn("tunnel_netif_set_link(true)", connected)
        self.assertIn("tunnel_ensure_started()", connected)
        self.assertIn("tunnel_netif_set_link(false)", ws_status)
        self.assertIn("schedule_vm_auth_refresh();", ws_status)
        app_run = _function_body(source, "void app_run(")
        reconcile = _function_body(app_run, "if (noise_ctrl_is_connected())")
        self.assertIn("bool tun_up = noise_tunnel_is_connected()", reconcile)
        self.assertIn("tunnel_netif_set_link(tun_up)", reconcile)
        self.assertLess(reconcile.index("bool tun_up = noise_tunnel_is_connected()"),
                        reconcile.index("tunnel_netif_set_link(tun_up)"))

    def test_session_start_reports_local_task_failure(self) -> None:
        header = (ROOT / "main" / "noise_control.h").read_text()
        connect = _function_body(NOISE_CONTROL_CPP.read_text(), 'extern "C" bool noise_ctrl_connect(')
        self.assertIn("bool noise_ctrl_connect", header)
        failure = _function_body(connect, "if (xTaskCreate(noise_ctrl_task")
        self.assertIn("!= pdPASS", connect)
        self.assertIn("s_running = false", failure)
        self.assertIn("s_task = nullptr", failure)
        self.assertIn("return false", failure)
        self.assertIn("return true", connect)

    def test_tunnel_stream_tracks_state_separately_from_control(self) -> None:
        source = NOISE_TUNNEL_CPP.read_text()
        up = _function_body(source, "static bool try_open_stream(")
        down = _function_body(source, 'extern "C" void noise_tunnel_on_session_down(')
        self.assertIn("s_connected.store(true", up)
        self.assertIn("s_connected.store(false", down)
        self.assertLess(up.index("if (!emit->open_stream(emit->ctx))"),
                        up.index("s_connected.store(true"))
        self.assertIn("return false", _function_body(up, "if (!emit->open_stream(emit->ctx))"))
        session = _function_body(NOISE_CONTROL_CPP.read_text(), "static session_result_t run_session(")
        self.assertIn("frame.stream_id == TUNNEL_STREAM_ID", session)
        self.assertIn("noise_tunnel_on_session_down()", session)
        reopen = _function_body(source, 'extern "C" void noise_tunnel_maybe_reopen(')
        self.assertIn("s_connected.load(", reopen)
        self.assertIn("now - s_last_open_attempt_us < TUN_REOPEN_INTERVAL_US", reopen)
        self.assertIn("try_open_stream(emit)", reopen)
        self.assertNotIn('"ws_connected"', source)

    def test_control_registers_link_and_maps_session_failures(self) -> None:
        source = NOISE_CONTROL_CPP.read_text()
        register = _function_body(source, "static char *build_register_json(")
        session = _function_body(source, "static session_result_t run_session(")
        task = _function_body(source, "static void noise_ctrl_task(")
        upgrade = _function_body(source, "static noise_upgrade_result_t ws_upgrade(")

        self.assertIn('"link.register"', register)
        self.assertIn('#define MUSE_DEVICE_FAMILY "gadget"', source)
        self.assertIn('"device_family", MUSE_DEVICE_FAMILY', register)
        self.assertIn('"model_id", CONFIG_MUSE_DEVICE_MODEL_ID', register)
        self.assertIn('"model_name"', register)
        self.assertIn("CONFIG_MUSE_DEVICE_MODEL_NAME", register)
        self.assertIn('"device.health"', register)
        self.assertIn('"device.guide"', register)
        self.assertIn('"device.discover"', register)
        self.assertIn('"timeout_ms"', register)
        # Management commands remain handled by on_ws_command, but are not
        # advertised in the registration schema.
        for command in ("device.unpair", "device.list_vms", "device.set_vm",
                        "device.reset_vm", "device.set_wifi", "device.set_auth"):
            self.assertNotIn(f'"{command}"', register)
        self.assertIn("build_register_json()", session)
        self.assertIn("control_tx = {reg_json, strlen(reg_json), 0, false, session_generation};", session)
        auth_failure = _function_body(task, "if (result == SESSION_AUTH_FAILED)")
        self.assertIn('notify_status("ws_auth_failed")', auth_failure)
        refresh = _function_body(task, "if (update_reconnect_policy(")
        self.assertIn('notify_status("ws_refresh_needed")', refresh)
        # link_noise_upgrade_harness.c runs noise_upgrade.h's request builder
        # and classifier: exact bearer header, no token in the request target,
        # CR/LF rejection, and 401/403 authentication failures. Check that the
        # transport uses those helpers and refuses invalid requests before I/O.
        self.assertIn("int req_len = noise_upgrade_build_request(", upgrade)
        self.assertIn("NOISE_PATH, s_vm_id, s_noise_host,", upgrade)
        self.assertIn('s_auth_token ? s_auth_token : "", req, req_cap)', upgrade)
        invalid_request = _function_body(upgrade, "if (req_len <= 0)")
        self.assertIn("return NOISE_UPGRADE_FAILED;", invalid_request)
        self.assertLess(upgrade.index("if (req_len <= 0)"),
                        upgrade.index("write_all(tls, req, req_len)"))
        self.assertIn("noise_upgrade_classify(hdr, have,", upgrade)
        self.assertIn("noise_upgrade_result_t upgrade = ws_upgrade(tls);", session)
        rejected = _function_body(session, "if (upgrade != NOISE_UPGRADE_OK)")
        self.assertIn("esp_tls_conn_destroy(tls);", rejected)
        self.assertIn(
            "return upgrade == NOISE_UPGRADE_AUTH_REJECTED ? SESSION_AUTH_FAILED",
            rejected,
        )
        self.assertIn(": SESSION_FAILED;", rejected)

    def test_websocket_control_commands_run_outside_ble(self) -> None:
        source = APP_C.read_text()
        handler = _function_body(source, "static cJSON *on_ws_command(")
        task = _function_body(source, "static void ws_control_task(")

        self.assertIn('strcmp(command, "device.unpair")', handler)
        self.assertIn('strcmp(command, "device.list_vms")', handler)
        self.assertIn('strcmp(command, "device.set_vm")', handler)
        self.assertIn('strcmp(command, "device.reset_vm")', handler)
        self.assertNotIn('strcmp(command, "device.set_wifi")', handler)
        self.assertNotIn('strcmp(command, "device.set_auth")', handler)
        self.assertIn("vTaskDelay(pdMS_TO_TICKS(300))", task)
        self.assertIn("set_vm_with_gate_held(args->url)", task)
        self.assertIn("reset_default_vm_with_gate_held()", task)
        self.assertIn("finish_precleared_setup_reset_with_gate_held", task)


if __name__ == "__main__":
    unittest.main()
