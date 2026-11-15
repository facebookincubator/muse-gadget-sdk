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

"""Node ids.

A new setup offers node_id gadget-XXXXXX (the same six hex digits as before)
and stores it with its tokens. A device set up by earlier firmware, which
stored no id, keeps homelink-XXXXXX, the id its tokens were issued for, until
it is set up again.
"""

from __future__ import annotations

import os
import shlex
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
BUILD_TOKEN = "mgst_" + "A" * 43


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
                return source[brace:i + 1]
    raise AssertionError(f"unterminated {signature}")


class NodeIdHarnessTest(unittest.TestCase):
    def test_node_ids(self) -> None:
        # With gadget ids on: new setup -> gadget-abcdef; an earlier setup
        # keeps homelink- through reboots, an update's boot and a token
        # refresh; setting up again, a reset or an unpair then a setup ->
        # gadget-; a stored id is kept.
        self.assertIn("node id harness: PASS", self.run_harness(gadget_ids=True))

    def test_node_ids_off(self) -> None:
        # Off (the default): new setups offer homelink- and store it; a stored
        # gadget- id is kept until the next setup.
        self.assertIn("node id harness (gadget ids off): PASS", self.run_harness(gadget_ids=False))

    def run_harness(self, gadget_ids: bool) -> str:
        cc = shlex.split(os.environ.get("CC", "cc"))
        if not cc or shutil.which(cc[0]) is None:
            raise unittest.SkipTest("C compiler not available")
        with tempfile.TemporaryDirectory(prefix="link-node-id-") as directory:
            binary = Path(directory) / "harness"
            r = subprocess.run(
                [*cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-variable",
                 f'-DCONFIG_GADGET_SDK_TOKEN="{BUILD_TOKEN}"',
                 '-DCONFIG_HOMEHUB_BLE_NAME_PREFIX="MuseGadget"', '-DCONFIG_HOMEHUB_BLE_NAME_SUFFIX=""',
                 *(["-DGADGET_IDS", "-DCONFIG_MUSE_GADGET_NODE_ID_PREFIX=1"] if gadget_ids else []),
                 "-I", str(ROOT / "tests/identity_fakes"), "-I", str(ROOT / "tests/link_fakes"),
                 "-I", str(ROOT / "main"),
                 str(ROOT / "tests/link_node_id_harness.c"), str(ROOT / "main/identity.c"),
                 str(ROOT / "main/config_store.c"), "-o", str(binary)],
                capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            r = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            return r.stdout


class NodeIdSourceTest(unittest.TestCase):
    """Where the app takes the setup id and where the id in use."""

    app = (ROOT / "main/app.c").read_text()

    def test_setup_offers_and_stores_the_setup_id(self) -> None:
        info = _function_body(self.app, "static void on_get_device_info(void)")
        self.assertIn('cJSON_AddStringToObject(root, "node_id", identity_setup_node_id());', info)
        self.assertIn("link_pairing_init(identity_setup_node_id(), identity_device_id(), identity_mac(),", self.app)
        accept = _function_body(self.app, "static bool accept_pairing_credentials(")
        store = 'config_set_str("node_id", identity_setup_node_id())'
        # Stored before any of the setup's tokens, then taken.
        self.assertLess(accept.index(store), accept.index("store_token_pair_unlocked("))
        self.assertLess(accept.index(store), accept.index("mint_and_store_device_tokens_unlocked("))
        self.assertIn("identity_setup_node_id(), NULL);", accept)
        self.assertLess(accept.index("auth_lock_give();"), accept.index("identity_refresh_node_id()"))
        self.assertIn("noise_ctrl_set_node_id(identity_node_id());", accept)

    def test_runtime_uses_the_id_in_use(self) -> None:
        for body in ("static bool migrate_legacy_auth_token_locked(",
                     "static bool ensure_access_token_ready_with_gate_held("):
            self.assertIn("identity_node_id()", _function_body(self.app, body))
            self.assertNotIn("identity_setup_node_id()", _function_body(self.app, body))
        self.assertIn("noise_ctrl_init(identity_node_id(), identity_ble_name(), on_ws_control_status)", self.app)

    def test_switch_defaults_off_and_bench_turns_it_on(self) -> None:
        kconfig = (ROOT / "main/Kconfig.projbuild").read_text()
        entry = kconfig[kconfig.index("config MUSE_GADGET_NODE_ID_PREFIX"):]
        entry = entry[:entry.index("\n    config ")]
        self.assertIn("bool ", entry)
        self.assertIn("default n", entry)
        self.assertIn("CONFIG_MUSE_GADGET_NODE_ID_PREFIX=y",
                      (ROOT / "devices/sdkconfig.muse-bench").read_text().splitlines())

    def test_the_id_goes_with_the_tokens(self) -> None:
        store = (ROOT / "main/config_store.c").read_text()
        for clear in ("bool config_clear_pairing(void)", "bool config_clear_setup(void)"):
            self.assertIn('"node_id"', _function_body(store, clear))


if __name__ == "__main__":
    unittest.main()
