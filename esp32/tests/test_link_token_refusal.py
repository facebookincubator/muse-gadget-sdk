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

"""A refused device token must not cost the device its Wi-Fi.

Runs app.c's real token refresh, refresh scheduler, unpair, reset and
boot-validation code against the real config_store.c
(link_token_refusal_harness.c), for both the Link and the Muse build: a 401 on
a refresh or mint unpairs but keeps Wi-Fi and stops retrying; a 403 or a
network failure keeps everything and backs off, also through
schedule_vm_auth_refresh(); explicit unpairs keep Wi-Fi; the user's factory
reset (5 s hold, Muse menu/Settings) forgets it; reboot in each state.
"""

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

FUNCTIONS = (
    "static void note_access_token_fresh(",
    "static void auth_lock_take(",
    "static void auth_lock_give(",
    "static void count_vm_auth_refusal_unlocked(",
    "static void note_vm_credentials_refused(",
    "static bool vm_auth_backoff_active_unlocked(",
    "static void note_vm_credentials_accepted(",
    "static void setup_stage_set(",
    "static bool clear_setup_credentials(",
    "static bool setup_wipe_to_clean(",
    "static bool complete_setup_and_stop_ble(",
    "static char *load_token_unlocked(",
    "static char *load_token(",
    "static bool store_token_pair_unlocked(",
    "static void store_pairing_username_unlocked(",
    "static void handle_device_token_revoked_unlocked(",
    "static void note_device_token_refused_unlocked(",
    "static void defer_unpair_after_revocation(",
    "static bool mint_and_store_device_tokens_unlocked(",
    "static bool accept_pairing_credentials(",
    "static bool migrate_legacy_auth_token_locked(",
    "static bool ensure_access_token_ready_with_gate_held(",
    "static int fetch_vms_with_refresh_with_gate_held(",
    "static bool connect_preferred_vm_with_gate_held(",
    "static void finish_vm_auth_refresh_task(",
    "static void vm_auth_refresh_task(",
    "static void schedule_vm_auth_refresh(",
    "static bool reset_setup_with_gate_held(",
    "static void on_unpair(",
    "static bool reset_setup_from_control(",
    "static void on_button_long_press(",
    "static void reset_setup_task(",
    "void app_reset_setup_async(",
    "static void ws_unpaired_task(",
)


def _function(source: str, prefix: str) -> str:
    start = source.index(prefix)
    brace = source.index("{", start)
    return source[start:brace] + "{" + _function_body(source, prefix) + "}\n"


class LinkTokenRefusalTest(unittest.TestCase):
    def test_token_refusal_and_resets(self) -> None:
        cc = shlex.split(os.environ.get("CC", "cc"))
        if not cc or shutil.which(cc[0]) is None:
            self.skipTest("C compiler not available")
        app = (ROOT / "main/app.c").read_text()

        defines = "\n".join(m.group(0) for m in re.finditer(
            r"^#define (?:TOKEN_BUF_BYTES|VM_AUTH_REFRESH_\w+|"
            r"TOKEN_PERIODIC_REFRESH_INTERVAL_US)\b(?:.*\\\n)*.*$",
            app, re.MULTILINE))
        defines += "\n" + re.search(
            r"^typedef enum \{[^}]*\} setup_reset_scope_t;$", app, re.MULTILINE
        ).group(0)
        paths = "".join(_function(app, prefix)
                        for prefix in sorted(FUNCTIONS, key=app.index))
        app_run = _function_body(app, "void app_run(")
        boot = app_run[app_run.index("bool setup_complete = config_setup_complete();"):]
        boot = boot.split("bool skip_boot_scan", 1)[0]
        # The heartbeat's deferred resets and Link's disconnected retry.
        heartbeat = app_run[app_run.index("while (1) {") + len("while (1) {"):]
        heartbeat = heartbeat.split("        // Runs on every heartbeat tick", 1)[0]

        with tempfile.TemporaryDirectory() as directory:
            generated = Path(directory)
            (generated / "app_defines.inc").write_text(defines + "\n")
            (generated / "app_token_paths.inc").write_text(paths)
            (generated / "app_boot_setup.inc").write_text(boot)
            (generated / "app_heartbeat.inc").write_text(heartbeat)
            for name, muse, unpaired_wifi in (("link", 0, 0), ("muse", 1, 1)):
                with self.subTest(profile=name):
                    binary = generated / name
                    compiled = subprocess.run(
                        # config_store.c's TAG and the eFuse fake are only used by
                        # logging and NVS encryption, which link_fakes compile out.
                        [*cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-variable",
                         f"-DMUSE_BUILD={muse}",
                         f"-DWIFI_WITHOUT_PAIRING={unpaired_wifi}",
                         "-I", str(generated),
                         "-I", str(ROOT / "tests" / "link_fakes"),
                         "-I", str(ROOT / "main"),
                         str(ROOT / "tests" / "link_token_refusal_harness.c"),
                         str(ROOT / "main" / "config_store.c"),
                         "-o", str(binary)],
                        capture_output=True, text=True,
                    )
                    self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
                    ran = subprocess.run([str(binary)], capture_output=True, text=True)
                    self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)


if __name__ == "__main__":
    unittest.main()
