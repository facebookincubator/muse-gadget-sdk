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

from pathlib import Path
import os
import shlex
import subprocess
import tempfile
import unittest

from test_link_ota import function_source

ROOT = Path(__file__).resolve().parents[1]
# CI supplies pinned upstream sources; local IDF builds already have cJSON.
JSON = Path(os.environ.get(
    "CJSON_SOURCE_DIR", ROOT / "managed_components/espressif__cjson/cJSON"
))


class LinkDiscoveryTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        out = Path(cls.temp.name)
        discovery = (ROOT / "main/net_discovery.c").read_text()
        (out / "discovery_all.inc").write_text(discovery[discovery.index('static const char *TAG'):])
        noise = (ROOT / "main/noise_control.cpp").read_text()
        start = noise.index("static char *wrap_result_json(")
        (out / "noise_result.inc").write_text(noise[start:noise.index("// ---- Reconnect policy", start)])
        app = (ROOT / "main/app.c").read_text()
        start = app.index("typedef struct {", app.index("// ---- Network discovery task"))
        (out / "discover_task.inc").write_text(
            app[start:app.index("static void discover_task(", start)]
            + function_source(app, "static void discover_task("))
        cc = shlex.split(os.environ.get("CC", "cc"))
        cxx = shlex.split(os.environ.get("CXX", "c++"))
        # glibc hides ip_mreq in strict C11 mode without its default extensions.
        flags = ["-Wall", "-Wextra", "-Werror", "-D_DEFAULT_SOURCE", "-DCJSON_NESTING_LIMIT=16", "-I", str(JSON), "-I", str(out)]
        commands = [
            [*cc, "-std=c11", *flags, "-c", str(JSON / "cJSON.c"), "-o", str(out / "cjson.o")],
            [*cc, "-std=c11", *flags, str(ROOT / "tests/link_discovery_harness.c"), str(out / "cjson.o"), "-lm", "-o", str(out / "discovery")],
            [*cxx, "-std=c++17", *flags, str(ROOT / "tests/link_discovery_payload_harness.cpp"), str(out / "cjson.o"), "-lm", "-o", str(out / "payload")],
            [*cc, "-std=c11", *flags, str(ROOT / "tests/link_discover_task_harness.c"), str(out / "cjson.o"), "-lm", "-o", str(out / "discover_task")],
        ]
        for cmd in commands:
            compiled = subprocess.run(cmd, capture_output=True, text=True)
            if compiled.returncode:
                raise AssertionError(compiled.stdout + compiled.stderr)
        cls.out = out

    def run_harness(self, name):
        result = subprocess.run([str(self.out / name)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_discovery_clamp_caps_reporting_allocations_and_shared_budget(self):
        self.run_harness("discovery")

    def test_direct_payload_ownership_and_legacy_compatibility(self):
        self.run_harness("payload")

    def test_discovery_task_always_answers(self):
        # Out of memory, it used to send nothing, leaving the Muse to wait out
        # device.discover's 90 s timeout.
        self.run_harness("discover_task")

    def test_discovery_task_transfers_tree_without_serialization(self):
        app = (ROOT / "main/app.c").read_text()
        start = app.index("static void discover_task(void *arg)")
        task = app[start:app.index("// ---- WebSocket command callbacks", start)]
        self.assertIn('cJSON_AddItemToObject(wrapper, "payload", result)', task)
        self.assertNotIn("cJSON_Print", task)
        self.assertNotIn("payload_json", task)


if __name__ == "__main__":
    unittest.main()
