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

"""Setup tasks whose stacks may live in PSRAM pair creation with deletion.

A task created WithCaps keeps its stack and TCB after a plain vTaskDelete(),
so every exit and cancellation goes through helper_task_delete(). The stacks
go to PSRAM only where flash writes leave the cache on: provisioning writes
NVS, which asserts on a PSRAM stack otherwise.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
HELPER_H = ROOT / "main" / "helper_task.h"
APP_C = ROOT / "main" / "app.c"
BLE_SERVER_C = ROOT / "main" / "ble_server.c"
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


def _strip_comments(source: str) -> str:
    source = re.sub(r"/\*.*?\*/", "", source, flags=re.S)
    return re.sub(r"//[^\n]*", "", source)


class HelperTaskContractTest(unittest.TestCase):
    def test_psram_stacks_only_where_flash_writes_keep_the_cache(self) -> None:
        header = _strip_comments(HELPER_H.read_text())
        gate = re.search(r"#if (.*)\n#include \"esp_heap_caps.h\"", header)
        self.assertIsNotNone(gate, "PSRAM gate not found")
        condition = gate.group(1)
        self.assertIn("CONFIG_HOMEHUB_SETUP_TASKS_IN_PSRAM", condition)
        self.assertIn("CONFIG_SPIRAM_FETCH_INSTRUCTIONS", condition)
        self.assertIn("CONFIG_SPIRAM_RODATA", condition)
        self.assertNotIn("||", condition)

        # Opt-in per board, and only where flash writes keep the cache on.
        kconfig = KCONFIG.read_text()
        option = kconfig[kconfig.index("config HOMEHUB_SETUP_TASKS_IN_PSRAM") :]
        option = option[: option.index("\n    config ", 1)]
        self.assertIn("depends on SPIRAM && SPIRAM_FETCH_INSTRUCTIONS && SPIRAM_RODATA", option)
        self.assertIn("default n", option)

        create = _function_body(header, "static inline BaseType_t helper_task_create(")
        delete = _function_body(header, "static inline void helper_task_delete(")
        for body, caps, plain in (
            (create, "xTaskCreateWithCaps(", "xTaskCreate("),
            (delete, "vTaskDeleteWithCaps(", "vTaskDelete("),
        ):
            psram, _, internal = body.partition("#else")
            self.assertIn("#if HELPER_TASK_STACK_IN_PSRAM", psram)
            self.assertIn(caps, psram)
            self.assertIn(plain, internal)
            self.assertNotIn(caps, internal)

    def test_setup_tasks_do_not_choose_psram_stacks_themselves(self) -> None:
        for path in (APP_C, BLE_SERVER_C):
            source = _strip_comments(path.read_text())
            for name in ("scan_refresh_task", "provision_task",
                         "confirm_timeout_task", "confirmed_idle_timeout_task"):
                self.assertNotRegex(
                    source, rf"xTaskCreate(?:PinnedToCore)?WithCaps\(\s*{name}\b",
                    f"{path.name}: {name} bypasses helper_task_create()",
                )
            self.assertNotIn("xTaskCreateWithCaps(task,", source)

    def test_every_helper_task_exit_and_cancel_uses_helper_task_delete(self) -> None:
        app = _strip_comments(APP_C.read_text())
        ble = _strip_comments(BLE_SERVER_C.read_text())

        # start_confirm_timeout() starts whichever worker it is handed.
        confirm = _function_body(app, "static BaseType_t start_confirm_timeout(")
        self.assertIn("helper_task_create(task,", confirm)
        confirm_workers = set(re.findall(r"start_confirm_timeout\(\s*(\w+)\s*,", app))
        self.assertEqual(confirm_workers, {"confirm_timeout_task", "confirmed_idle_timeout_task"})

        started = {
            (APP_C.name, name)
            for name in re.findall(r"helper_task_create\(\s*(\w+)\s*,", app)
            if name != "task"
        }
        started |= {(APP_C.name, name) for name in confirm_workers}
        started |= {
            (BLE_SERVER_C.name, name)
            for name in re.findall(r"helper_task_create\(\s*(\w+)\s*,", ble)
        }
        self.assertIn((APP_C.name, "scan_refresh_task"), started)
        self.assertIn((BLE_SERVER_C.name, "provision_task"), started)

        sources = {APP_C.name: app, BLE_SERVER_C.name: ble}
        for file_name, name in sorted(started):
            body = _function_body(sources[file_name], f"static void {name}(void *arg) {{")
            self.assertIn("helper_task_delete(NULL);", body, f"{name} must free its WithCaps stack")
            self.assertNotIn("vTaskDelete(", body, f"{name} leaks a WithCaps stack")
            self.assertNotRegex(body, r"\breturn\s*;", f"{name} exits without deleting itself")

        # A pending confirmation timer is cancelled (disconnect, new session)
        # before it ever fires: that path frees the stack too.
        cancel = _function_body(app, "static uint32_t advance_confirm_generation_locked(")
        self.assertIn("helper_task_delete(s_confirm_timeout_task);", cancel)
        self.assertNotIn("vTaskDelete(s_confirm_timeout_task)", app)


if __name__ == "__main__":
    unittest.main()
