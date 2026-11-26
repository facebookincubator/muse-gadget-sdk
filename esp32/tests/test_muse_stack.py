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

"""App task stacks in PSRAM (CONFIG_MUSE_STACKS_IN_PSRAM, muse_stack.h): tasks
that map flash keep internal stacks (a PSRAM stack asserts while the cache is
frozen).
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

SLOT_TASKS = {
    "main/noise_control.cpp": ["noise_ctrl_task"],
    "main/muse_glue.c": ["scan_task", "boot_task"],
}
FOREVER_TASKS = {
    "main/noise_control.cpp": ["noise_ctrl_ble_task"],   # a BLE-only gadget's device session (mg_vm)
    "main/muse_glue.c": ["keeper_task"],
    "main/tunnel_netif.c": ["tx_task"],
    "components/muse_gadget_ble/mg_ble.c": ["player"],
}
# Tasks that map flash (or may: the console), so their stacks stay internal:
# mapping freezes the cache, and a PSRAM stack asserts
# (s_task_stack_is_sane_when_cache_frozen; the musegadgets worker's SMP
# healthy mark did on the W175). Each is a plain xTaskCreate*().
INTERNAL_TASKS = {
    "components/muse_gadget_ble/mg_ble.c": ["worker"],   # SMP: OTA states, image checks, the healthy mark
    "main/ota.c": ["ota_task"],                          # esp_https_ota
    "main/app.c": ["ota_verify_task"],                   # esp_ota_mark_app_*
    "main/bug_report.c": ["bug_report_task"],            # esp_ota_get_boot_partition / get_state_partition
    "components/muse/muse_f2.c": ["f2_task"],            # esp_image_verify
    "components/muse/muse_input.c": ["input_task", "serial_task"],   # power off, light sleep; the console
}
# What maps flash, directly or through ESP-IDF.
FLASH_MAPPING = ("esp_partition_mmap", "spi_flash_mmap", "esp_mmu_map", "bootloader_mmap",
                 "esp_ota_get_state_partition", "esp_ota_get_boot_partition", "esp_ota_mark_app_valid",
                 "esp_ota_mark_app_invalid", "esp_ota_set_boot_partition", "esp_ota_end", "esp_image_verify",
                 "esp_https_ota")


def code_only(source: str) -> str:
    """The source without its comments and string literals."""
    source = re.sub(r"/\*.*?\*/", " ", source, flags=re.S)
    source = re.sub(r"//[^\n]*", " ", source)
    return re.sub(r'"(?:\\.|[^"\\])*"', '""', source)


def body(source: str, name: str) -> str:
    m = re.search(r"^static void " + re.escape(name) + r"\(void \*arg\)\s*\{", source, re.M)
    assert m, name
    i, depth = m.end(), 1
    while depth:
        depth += {"{": 1, "}": -1}.get(source[i], 0)
        i += 1
    return source[m.end():i]


class WiringTest(unittest.TestCase):


    def test_tasks_that_map_flash_keep_internal_stacks(self) -> None:
        for path, names in INTERNAL_TASKS.items():
            source = (ROOT / path).read_text()
            for name in names:
                body(source, name)   # still there
                self.assertIsNone(re.search(r"muse_task_(create|slot_run)\((&\w+, )?" + name + r"\b", source),
                                  f"{path}: {name} maps flash: its stack must stay internal")
                self.assertRegex(source, r"xTaskCreate\w*\(\s*" + name + r"\b", f"{path}: {name}")

    def test_moved_tasks_files_never_map_flash(self) -> None:
        moved = set(SLOT_TASKS) | set(FOREVER_TASKS)
        for path in moved:
            code = code_only((ROOT / path).read_text())
            for call in FLASH_MAPPING:
                self.assertFalse(call in code, f"{path} calls {call}: a PSRAM stack would assert there")
        # The list catches what SMP does (so the check above can fail).
        smp = code_only((ROOT / "components" / "muse_gadget_ble" / "mg_smp_esp.c").read_text())
        self.assertTrue(any(call in smp for call in FLASH_MAPPING))

    def test_session_commands_never_map_flash(self) -> None:
        # on_ws_command() runs on the session tasks above (Wi-Fi's and the BLE
        # device session's): what maps flash goes to a task of its own (the
        # bug report's, internal above).
        app = code_only((ROOT / "main" / "app.c").read_text())
        start = app.index("static cJSON *on_ws_command(")
        end = app.index("\n}\n", start)
        handler = app[start:end]
        for call in FLASH_MAPPING:
            self.assertNotIn(call, handler, f"on_ws_command() calls {call}: a PSRAM stack would assert there")
        self.assertIn("bug_report_command(", handler)


if __name__ == "__main__":
    unittest.main()
