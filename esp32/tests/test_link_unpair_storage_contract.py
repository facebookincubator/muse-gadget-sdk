#!/usr/bin/env python3
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


class LinkUnpairStorageContractTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.app = APP_C.read_text()

    def test_ble_unpair_reports_success_only_after_verified_clear(self) -> None:
        body = _function_body(self.app, "static void on_unpair(")
        reset = _function_body(self.app, "static bool reset_setup_with_gate_held(")
        finish = _function_body(self.app, "static void finish_setup_reset_with_gate_held(")
        gate = body.index('operation_gate_take(0, "BLE unpair")')
        shared = body.index('reset_setup_with_gate_held("BLE unpair")')
        disconnect = body.index("ble_server_disconnect_client()")
        self.assertLess(gate, shared)
        self.assertLess(shared, disconnect)
        self.assertIn("if (!setup_wipe_to_clean())", reset)
        self.assertIn('ble_server_send_status("error_storage")', reset)
        self.assertLess(reset.index("if (!setup_wipe_to_clean())"),
                        reset.index("finish_setup_reset_with_gate_held()"))
        self.assertIn('ble_server_send_status("unpaired")', finish)
        # Invalidate the old confirmation while locked, before any success
        # indication. Check the helper too: a no-op must not satisfy this test.
        advance = _function_body(
            self.app, "static uint32_t advance_confirm_generation_locked("
        )
        self.assertRegex(advance, r"return \+\+s_confirm_generation;\s*\Z")
        self.assertRegex(
            finish,
            r"\A\s*setup_window_lock_take\(\);\s*"
            r"advance_confirm_generation_locked\(\);\s*"
            r"setup_window_lock_give\(\);",
        )
        unlocked = finish.index("setup_window_lock_give()")
        for success in ('ui_set_status("unpaired")',
                        "led_status_set_state(LED_STATE_UNPAIRED)",
                        'ble_server_send_status("unpaired")'):
            self.assertLess(unlocked, finish.index(success))
        self.assertIn("if (ble_server_has_connection())", body)
        self.assertIn('open_setup_window("BLE unpair")', body)
        self.assertNotIn("esp_restart", body)
        self.assertGreater(body.rindex("operation_gate_give()"), disconnect)

        disconnect_client = _function_body(
            BLE_SERVER_C.read_text(), "void ble_server_disconnect_client("
        )
        self.assertIn("ble_gap_terminate", disconnect_client)
        self.assertNotIn("s_advertising_enabled", disconnect_client)

    def test_operation_gate_supports_cross_task_handoff(self) -> None:
        app_run = _function_body(self.app, "void app_run(")
        queue = _function_body(self.app, "static cJSON *queue_ws_control(")
        worker = _function_body(self.app, "static void ws_control_task(")

        self.assertIn("s_operation_gate = xSemaphoreCreateBinary()", app_run)
        self.assertNotIn("s_operation_gate = xSemaphoreCreateMutex()", self.app)
        self.assertIn("xSemaphoreGive(s_operation_gate)", app_run)
        self.assertIn("operation_gate_take(0, ws_control_action_name(action))", queue)
        self.assertIn("operation_gate_give()", worker)

    def test_setup_writers_are_serialized_against_unpair(self) -> None:
        body = _function_body(self.app, "static void on_provision(")
        take = body.index('operation_gate_take(0, "BLE provision")')
        write = body.index("complete_setup_and_stop_ble")
        give = body.rindex("operation_gate_give()")
        self.assertLess(take, write)
        self.assertLess(write, give)

        helper = _function_body(self.app, "static void set_vm_with_gate_held(")
        self.assertIn('config_set_str("vm_url", url)', helper)

        worker = _function_body(self.app, "static void ws_control_task(")
        self.assertIn("set_vm_with_gate_held(args->url)", worker)

    def test_long_press_restarts_only_after_verified_clear(self) -> None:
        button = _function_body(self.app, "static void on_button_long_press(")
        reset = _function_body(self.app, "static bool reset_setup_from_control(")
        helper = _function_body(self.app, "static bool reset_setup_with_gate_held(")

        self.assertIn('reset_setup_from_control("button long-press")', button)
        self.assertIn("if (!setup_wipe_to_clean())", helper)
        self.assertLess(helper.index("if (!setup_wipe_to_clean())"),
                        helper.index("finish_setup_reset_with_gate_held()"))
        self.assertIn("if (!reset) {", reset)
        self.assertIn("operation_gate_take(portMAX_DELAY", reset)
        self.assertNotIn("pdMS_TO_TICKS(30000)", reset)
        failure = _function_body(reset, "if (!reset)")
        self.assertIn("operation_gate_give()", failure)
        self.assertIn("return false", failure)
        self.assertIn("restart_after_setup_reset()", reset)

    def test_remote_device_unpair_restarts_only_after_verified_clear(self) -> None:
        worker = _function_body(self.app, "static void ws_control_task(")
        queue = _function_body(self.app, "static cJSON *queue_ws_control(")
        branch = worker[worker.index("case WS_CONTROL_UNPAIR:") :]
        clear = queue.index("if (!clear_setup_credentials())")
        task = queue.index("xTaskCreate(ws_control_task")
        accepted = queue.index("return command_ok_accepted()")
        self.assertLess(clear, task)
        self.assertLess(clear, accepted)
        self.assertIn('return command_error("storage_error"', queue[clear:accepted])
        self.assertIn("args->setup_credentials_cleared = true", queue[clear:task])
        # Every pre-accept give belongs to an error return. The successful path
        # transfers the reserved binary gate to the worker.
        self.assertLess(
            queue.rfind("operation_gate_give()", 0, accepted),
            queue.rfind("return command_error", 0, accepted),
        )
        self.assertIn("if (args->setup_credentials_cleared)", branch)
        self.assertIn("finish_precleared_setup_reset_with_gate_held", branch)
        self.assertNotIn("setup_wipe_to_clean", branch)
        self.assertNotIn("clear_setup_credentials", branch)
        self.assertIn("release_operation_gate = false", branch)
        self.assertLess(branch.index("finish_precleared_setup_reset_with_gate_held"),
                        branch.index("restart_after_setup_reset()"))

    def test_node_unpaired_never_restarts_on_lock_or_storage_failure(self) -> None:
        worker = _function_body(self.app, "static void ws_unpaired_task(")
        status = _function_body(self.app, "static void on_ws_control_status(")
        reset = _function_body(self.app, "static bool reset_setup_from_control(")

        self.assertIn('reset_setup_from_control("server unpair (node.unpaired)")',
                      worker)
        self.assertIn("operation_gate_take(portMAX_DELAY", reset)
        self.assertIn("if (xTaskCreate(ws_unpaired_task", status)
        self.assertIn("!= pdPASS", status)
        self.assertIn(
            "atomic_store_explicit(&s_setup_reset_pending, true",
            status,
        )
        self.assertNotIn("reset_setup_from_control", status)

        app_run = _function_body(self.app, "void app_run(")
        deferred = app_run.index(
            "atomic_exchange_explicit(&s_setup_reset_pending, false"
        )
        reset_call = app_run.index(
            'reset_setup_from_control("deferred setup reset")'
        )
        self.assertLess(deferred, reset_call)

    def test_failed_unpair_worker_keeps_gate_reserved_until_restart(self) -> None:
        cc = shlex.split(os.environ.get("CC", "cc"))
        if not cc or shutil.which(cc[0]) is None:
            self.skipTest("C compiler not available")
        queue = _function_body(self.app, "static cJSON *queue_ws_control(")
        failure = _function_body(queue, "if (xTaskCreate(ws_control_task")
        restart = _function_body(self.app, "static void restart_after_setup_reset(")
        harness = r"""
#include <assert.h>
#include <stdbool.h>
#define pdPASS 1
#define xTaskCreate(...) create_restart_task()
#define stack_monitor_record(...) ((void)0)
typedef struct { bool setup_credentials_cleared; } ws_control_args_t;
static bool gate_held;
static int task_result, releases, restarts, reboots, frees;
static void operation_gate_give(void) {
    assert(gate_held);
    gate_held = false;
    releases++;
}
static int create_restart_task(void) {
    assert(gate_held);
    restarts++;
    return task_result;
}
static void esp_restart(void) {
    assert(gate_held);
    reboots++;
}
static void free_ws_control_args(ws_control_args_t *args) {
    (void)args;
    frees++;
}
static int command_error(const char *code, const char *message) {
    (void)code;
    (void)message;
    return -1;
}
"""
        harness += "static void restart_after_setup_reset(void) {" + restart + "}\n"
        harness += "static int worker_failed(ws_control_args_t *args) {" + failure + "}\n"
        harness += r"""
static void check(bool cleared, int restart_result, bool expect_held,
                  int expect_reboots) {
    ws_control_args_t args = {.setup_credentials_cleared = cleared};
    gate_held = true;
    task_result = restart_result;
    releases = restarts = reboots = frees = 0;
    assert(worker_failed(&args) == -1);
    assert(gate_held == expect_held && releases == !expect_held);
    assert(restarts == cleared && reboots == expect_reboots);
    assert(frees == 1);
}
int main(void) {
    check(true, pdPASS, true, 0);
    check(true, 0, true, 1);
    check(false, pdPASS, false, 0);
    check(false, 0, false, 0);
    return 0;
}
"""
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "unpair_worker_failure.c"
            binary = source.with_suffix("")
            source.write_text(harness)
            compiled = subprocess.run(
                [*cc, "-std=c11", "-Wall", "-Wextra", "-Werror", str(source),
                 "-o", str(binary)], capture_output=True, text=True,
            )
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            ran = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)

    def test_token_refusal_reopens_pairing_through_the_app_task(self) -> None:
        defer = _function_body(self.app, "static void defer_pairing_reopen(")
        ensure = _function_body(
            self.app, "static bool ensure_access_token_ready_with_gate_held("
        )
        reopen = _function_body(
            self.app, "static void reopen_pairing_after_token_refusal("
        )
        app_run = _function_body(self.app, "void app_run(")

        # A refusal never wipes: it asks the app task to reopen pairing.
        self.assertNotIn("setup_wipe_to_clean", defer)
        self.assertNotIn("config_clear", defer)
        self.assertNotIn("s_setup_reset_pending", defer)
        self.assertNotIn("noise_ctrl_disconnect", defer)
        self.assertNotIn("esp_restart", defer)
        setup_guard = defer.index('strcmp(s_setup_stage, "done") != 0')
        pending = defer.index(
            "atomic_store_explicit(&s_pairing_reopen_pending, true"
        )
        self.assertLess(setup_guard, pending)
        self.assertIn("return;", defer[setup_guard:pending])
        self.assertIn("defer_pairing_reopen()", ensure)
        self.assertNotIn("config_clear", ensure)
        self.assertIn("reopen_pairing_after_token_refusal()", app_run)
        self.assertIn("close_pairing_after_token_accepted()", app_run)
        # Reopening takes the gate without blocking and retries a busy one.
        busy = _function_body(reopen, 'if (!operation_gate_take(0, "reopen pairing"))')
        self.assertIn("atomic_store_explicit(&s_pairing_reopen_pending, true", busy)
        self.assertIn('operation_gate_take(portMAX_DELAY, "boot VM connect")',
                      app_run)
        # The token-maintenance call in the heartbeat loop must take the gate
        # non-blocking (timeout 0). Blocking here would let it sit on the gate
        # while a deferred pairing reopen or setup reset is waiting to run. The label is
        # incidental — assert the timeout and that it actually wraps the
        # token-readiness call, so a rename cannot silently drop the guarantee.
        maintenance = app_run.index('operation_gate_take(0, ')
        self.assertIn("ensure_access_token_ready_with_gate_held(false, true)",
                      app_run[maintenance:])

    def test_button_cannot_race_boot_setup_migration(self) -> None:
        app_run = _function_body(self.app, "void app_run(")
        state_read = app_run.index("bool setup_complete = config_setup_complete()")
        state_finalized = app_run.index(
            's_setup_stage = setup_complete ? "done" : "idle"'
        )
        button_start = app_run.index("button_init(on_button_short_press")

        self.assertLess(state_read, state_finalized)
        self.assertLess(state_finalized, button_start)
        self.assertLess(button_start, app_run.index('open_setup_window("boot: unpaired")'))

    def test_setup_clear_has_no_identity_or_flash_erase_path(self) -> None:
        source = CONFIG_STORE_C.read_text()
        body = _function_body(source, "bool config_clear_setup(")
        self.assertNotIn("nvs_flash_erase", source)
        self.assertNotIn("lock_wipe", self.app)

    def test_wifi_channel_hint_follows_wifi_credentials(self) -> None:
        source = CONFIG_STORE_C.read_text()
        self.assertIn('"wifi_channel"', _function_body(source, "bool config_clear_setup("))
        self.assertNotIn('"wifi_channel"', _function_body(source, "bool config_clear_pairing("))
        self.assertNotIn('"wifi_channel"', _function_body(source, "bool config_unpair("))

    def test_only_a_full_setup_clear_forgets_wifi(self) -> None:
        # Unpair and every pairing-recovery path keep saved Wi-Fi; the app
        # never calls the full setup clear.
        self.assertNotIn("config_clear_setup()", self.app)
        clear = _function_body(self.app, "static bool clear_setup_credentials(")
        self.assertIn("config_unpair()", clear)

    def test_endpoint_urls_cleared_on_pairing_and_setup_reset(self) -> None:
        source = CONFIG_STORE_C.read_text()
        pairing = _function_body(source, "bool config_clear_pairing(")
        setup = _function_body(source, "bool config_clear_setup(")
        for body, label in [(pairing, "clear_pairing"), (setup, "clear_setup")]:
            self.assertIn('"api_url"', body, f"{label} must clear api_url")
            self.assertIn('"api_url_v2"', body, f"{label} must clear api_url_v2")
            self.assertIn('"noise_host"', body, f"{label} must clear noise_host")


if __name__ == "__main__":
    unittest.main()
