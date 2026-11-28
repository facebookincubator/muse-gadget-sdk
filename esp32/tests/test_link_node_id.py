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

A gadget's new setup offers node_id gadget-XXXXXX (the same six hex digits as
before) with CONFIG_MUSE_GADGET_NODE_ID_PREFIX, else homelink-XXXXXX, and
stores it with its tokens (and K). A device set up earlier keeps its stored
id, or the homelink-XXXXXX a setup that stored none was issued tokens for,
until a setup commits a new one. A plain Home Link keeps homelink-XXXXXX.
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
    def run_harness(self, gadget: bool, gadget_ids: bool = False) -> str:
        cc = shlex.split(os.environ.get("CC", "cc"))
        if not cc or shutil.which(cc[0]) is None:
            raise unittest.SkipTest("C compiler not available")
        defines = [
            f'-DCONFIG_GADGET_SDK_TOKEN="{BUILD_TOKEN}"',
            '-DCONFIG_HOMEHUB_BLE_NAME_PREFIX="MuseGadget"',
            '-DCONFIG_HOMEHUB_BLE_NAME_SUFFIX=""',
            '-DCONFIG_HOMEHUB_BLE_PRODUCT_CODE=""',
        ]
        if gadget:
            defines += ["-DGADGET_BUILD", "-DCONFIG_MUSE_GADGET_BLE_AUDIO=1"]
        if gadget_ids:
            defines += ["-DGADGET_IDS", "-DCONFIG_MUSE_GADGET_NODE_ID_PREFIX=1"]
        with tempfile.TemporaryDirectory(prefix="link-node-id-") as directory:
            binary = Path(directory) / "harness"
            r = subprocess.run(
                [*cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-variable", *defines,
                 "-I", str(ROOT / "tests/identity_fakes"), "-I", str(ROOT / "tests/link_fakes"),
                 "-I", str(ROOT / "main"),
                 str(ROOT / "tests/link_node_id_harness.c"), str(ROOT / "main/identity.c"),
                 str(ROOT / "main/config_store.c"), "-o", str(binary)],
                capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            r = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            return r.stdout

    def test_gadget_build(self) -> None:
        # New setup -> gadget-abcdef; a stored homelink- id survives reboots,
        # an update's boot and a credentials update; a stored id is never
        # renamed; a re-setup takes gadget-, an abandoned or power-cut one
        # keeps homelink-; a reset, or an unpair, then a new setup ->
        # gadget-; credentials without a stored id stay homelink-.
        self.assertIn("node id harness (gadget): PASS", self.run_harness(True, gadget_ids=True))

    def test_gadget_release_build(self) -> None:
        # CONFIG_MUSE_GADGET_NODE_ID_PREFIX off (off by default; bench builds
        # turn it on): homelink- for new setups, the id
        # still stored at the commit, a stored gadget- id kept until the next
        # setup, the handover naming the current pair's id.
        self.assertIn("node id harness (gadget, release): PASS", self.run_harness(True))

    def test_plain_home_link_build(self) -> None:
        # Homelink- offered and stored; a stored gadget- id kept until the
        # next setup; credentials without a stored id are homelink-.
        self.assertIn("node id harness (Home Link): PASS", self.run_harness(False))

    def test_plain_home_link_ignores_the_switch(self) -> None:
        # A plain Home Link offers homelink- with
        # CONFIG_MUSE_GADGET_NODE_ID_PREFIX on too.
        self.assertIn("node id harness (Home Link, gadget ids on): PASS", self.run_harness(False, gadget_ids=True))


class NodeIdSourceTest(unittest.TestCase):
    """Where the app takes the setup id and where the id in use."""

    app = (ROOT / "main/app.c").read_text()


    def test_runtime_uses_the_id_in_use(self) -> None:
        for body in ("static bool migrate_legacy_auth_token_locked(",
                     "static bool ensure_access_token_ready_with_gate_held("):
            self.assertIn("identity_node_id()", _function_body(self.app, body))
            self.assertNotIn("identity_setup_node_id()", _function_body(self.app, body))
        self.assertIn("noise_ctrl_init(identity_node_id(), identity_ble_name(), on_ws_control_status)", self.app)


    def test_handover_names_the_current_pairs_id(self) -> None:
        # The device session's credentials: the id stored with the current
        # pair (journal-aware, as the pair is), else the id in use; never the
        # setup id a re-setup offers before its commit.
        glue = (ROOT / "main/mg_glue.c").read_text()
        creds = _function_body(glue, "static bool session_credentials(mg_session_creds_t *c)")
        self.assertIn("app_gadget_read_session_setup(c->node_id, sizeof(c->node_id)", creds)
        app = (ROOT / "main/app.c").read_text()
        snapshot = _function_body(app, "bool app_gadget_read_session_setup(")
        self.assertIn("config_get_node_id(node_id, node_size)", snapshot)
        self.assertIn('snprintf(c->node_id, sizeof(c->node_id), "%s", identity_node_id());', creds)
        self.assertNotIn("identity_setup_node_id", creds)
        store = (ROOT / "main/config_store.c").read_text()
        self.assertIn('config_get_str(config_reprovision_open() ? "rp_node" : "node_id", out, size)',
                      _function_body(store, "bool config_get_node_id(char *out, size_t size)"))

    def test_switch_defaults_off_and_bench_turns_it_on(self) -> None:
        kconfig = (ROOT / "main/Kconfig.projbuild").read_text()
        entry = kconfig[kconfig.index("config MUSE_GADGET_NODE_ID_PREFIX"):]
        entry = entry[:entry.index("\n    config ")]
        self.assertIn("bool ", entry)
        self.assertIn("default n", entry)
        self.assertIn("CONFIG_MUSE_GADGET_NODE_ID_PREFIX=y",
                      (ROOT / "devices/sdkconfig.muse-bench").read_text().splitlines())
        identity = (ROOT / "main/identity.c").read_text()
        self.assertIn("#if CONFIG_MUSE_GADGET_BLE_AUDIO && CONFIG_MUSE_GADGET_NODE_ID_PREFIX", identity)

    def test_the_id_goes_with_the_tokens(self) -> None:
        store = (ROOT / "main/config_store.c").read_text()
        for clear in ("bool config_clear_pairing(void)", "bool config_clear_setup(void)"):
            self.assertIn('"node_id"', _function_body(store, clear))


if __name__ == "__main__":
    unittest.main()
