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

ROOT = Path(__file__).resolve().parents[1]
# CI supplies pinned upstream sources; local IDF builds already have cJSON.
JSON = Path(os.environ.get(
    "CJSON_SOURCE_DIR", ROOT / "managed_components/espressif__cjson/cJSON"
))


class LinkReterminalSht4xTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        out = Path(cls.temp.name)
        source = (ROOT / "main/reterminal_sht4x.c").read_text()
        start = source.index("// ---- Protocol (host-tested)")
        (out / "sht4x_protocol.inc").write_text(source[start:source.index("// ---- I2C driver", start)])
        cc = shlex.split(os.environ.get("CC", "cc"))
        flags = ["-Wall", "-Wextra", "-Werror", "-DCJSON_NESTING_LIMIT=16", "-I", str(JSON), "-I", str(out)]
        commands = [
            [*cc, "-std=c11", *flags, "-c", str(JSON / "cJSON.c"), "-o", str(out / "cjson.o")],
            [*cc, "-std=c11", *flags, str(ROOT / "tests/link_reterminal_sht4x_harness.c"), str(out / "cjson.o"), "-lm", "-o", str(out / "sht4x")],
        ]
        for cmd in commands:
            compiled = subprocess.run(cmd, capture_output=True, text=True)
            if compiled.returncode:
                raise AssertionError(compiled.stdout + compiled.stderr)
        cls.out = out

    def test_bytes_to_sensors_read_result(self):
        result = subprocess.run([str(self.out / "sht4x")], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_command_is_advertised_and_dispatched_with_the_feature(self):
        noise = (ROOT / "main/noise_control.cpp").read_text()
        start = noise.index("#if CONFIG_HOMEHUB_RETERMINAL_SHT4X")
        self.assertIn('add_command(commands, "sensors.read"', noise[start:noise.index("#endif", start)])
        app = (ROOT / "main/app.c").read_text()
        start = app.index("static cJSON *on_ws_command(")
        dispatch = app[start:app.index("unsupported command", start)]
        block = dispatch[dispatch.index("#if CONFIG_HOMEHUB_RETERMINAL_SHT4X"):]
        self.assertIn('"sensors.read"', block[:block.index("#endif")])


if __name__ == "__main__":
    unittest.main()
