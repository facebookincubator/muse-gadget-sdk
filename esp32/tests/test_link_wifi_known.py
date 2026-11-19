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

from host_cjson import cjson_source


ROOT = Path(__file__).resolve().parents[1]
# CI supplies pinned upstream sources; local IDF builds already have cJSON.
JSON = Path(os.environ.get(
    "CJSON_SOURCE_DIR", ROOT / "managed_components/espressif__cjson/cJSON"
))


class LinkWifiKnownHarnessTest(unittest.TestCase):
    def test_saved_networks(self) -> None:
        cc = shlex.split(os.environ.get("CC", "cc"))
        if not cc or shutil.which(cc[0]) is None:
            raise unittest.SkipTest("C compiler not available")
        if not (JSON / "cJSON.c").exists():
            raise unittest.SkipTest("cJSON sources not available")

        with tempfile.TemporaryDirectory() as tmpdir:
            binary = Path(tmpdir) / "link_wifi_known_harness"
            compile_proc = subprocess.run(
                [
                    *cc,
                    "-std=c11",
                    "-Wall",
                    "-Wextra",
                    "-DCJSON_NESTING_LIMIT=16",
                    # Ahead of the fakes, which have a cJSON.h of their own.
                    "-I",
                    str(JSON),
                    "-I",
                    str(ROOT / "tests" / "link_fakes"),
                    "-I",
                    str(ROOT / "main"),
                    str(ROOT / "tests" / "link_wifi_known_harness.c"),
                    str(ROOT / "main" / "wifi_known.c"),
                    str(ROOT / "components/network_proxy/network_proxy.c"),
                    str(ROOT / "main" / "config_store.c"),
                    *cjson_source(JSON),
                    "-lm",
                    "-o",
                    str(binary),
                ],
                cwd=ROOT,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )
            self.assertEqual(
                compile_proc.returncode,
                0,
                msg=compile_proc.stdout + compile_proc.stderr,
            )

            run_proc = subprocess.run(
                [str(binary)],
                cwd=ROOT,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )
            self.assertEqual(
                run_proc.returncode,
                0,
                msg=run_proc.stdout + run_proc.stderr,
            )

    def test_setup_reset_forgets_every_network(self) -> None:
        source = (ROOT / "main" / "config_store.c").read_text()
        start = source.index("bool config_clear_setup(")
        body = source[start:source.index("}", start)]
        self.assertIn('"wifi_others"', body)
        self.assertIn('"wifi_hidden"', body)
        start = source.index("bool config_clear_pairing(")
        body = source[start:source.index("}", start)]
        self.assertNotIn('"wifi_others"', body)


if __name__ == "__main__":
    unittest.main()
