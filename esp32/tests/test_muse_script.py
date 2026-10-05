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

"""On-device scripts: the Lua sandbox (main/muse_script_sandbox.c) on the real
Lua (components/lua), built for the host; and the wiring around it."""

from pathlib import Path
import os
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
JSON = Path(os.environ.get(
    "CJSON_SOURCE_DIR", ROOT / "managed_components/espressif__cjson/cJSON"
))
LUA = ROOT / "components/lua/src"
LUA_FILES = """lapi lcode lctype ldebug ldo ldump lfunc lgc llex lmem lobject lopcodes lparser lstate
    lstring ltable ltm lundump lvm lzio lauxlib lbaselib lcorolib lmathlib lstrlib ltablib lutf8lib""".split()


class MuseScriptTest(unittest.TestCase):
    @unittest.skipUnless((JSON / "cJSON.c").exists(), "needs cJSON: run idf.py build, or set CJSON_SOURCE_DIR")
    def test_sandbox(self):
        with tempfile.TemporaryDirectory() as out:
            cc = shlex.split(os.environ.get("CC", "cc"))
            binary = Path(out) / "sandbox"
            # The same definitions components/lua/CMakeLists.txt gives the firmware.
            defs = ["-DLUAI_MAXCCALLS=48", "-DMAXCCALLS=64", "-DLUAI_MATCHSTEP_FUNC=sb_matchstep"]
            cmd = [*cc, "-std=gnu99", "-O1", *defs, "-I", str(LUA), "-I", str(JSON), "-I", str(ROOT / "main"),
                   *[str(LUA / f"{f}.c") for f in LUA_FILES], str(JSON / "cJSON.c"),
                   str(ROOT / "main/muse_script_sandbox.c"), str(ROOT / "tests/muse_script_harness.c"),
                   "-lm", "-o", str(binary)]
            built = subprocess.run(cmd, capture_output=True, text=True)
            self.assertEqual(built.returncode, 0, built.stderr[-3000:])
            ran = subprocess.run([str(binary)], capture_output=True, text=True, timeout=120)
            self.assertEqual(ran.returncode, 0, ran.stdout[-3000:] + ran.stderr[-3000:])

    def test_lua_is_lua_org_5_4_9_with_the_matcher_patch(self):
        self.assertIn('#define LUA_VERSION_RELEASE_NUM\t\t(LUA_VERSION_NUM * 100 + 9)', (LUA / "lua.h").read_text())
        self.assertEqual((LUA / "lstrlib.c").read_text().count("SANDBOX PATCH"), 2)
        for f in ("liolib.c", "loslib.c", "loadlib.c", "ldblib.c", "linit.c", "lua.c", "luac.c"):
            self.assertFalse((LUA / f).exists(), f)

    def test_scripts_cannot_reach_what_writes_flash_or_manages_the_device(self):
        source = (ROOT / "main/muse_script.c").read_text()
        allowed = source[source.index("static const char *const COMMANDS[]"):]
        allowed = allowed[:allowed.index("NULL,")]
        for forbidden in ("display.set_brightness", "audio.set_volume", "device.reboot", "device.power_off",
                          "script.", "device.set_vm", "device.unpair", "device.ota", "input.read"):
            self.assertNotIn(f'"{forbidden}', allowed)
        dispatch = (ROOT / "main/muse_hw_commands.c").read_text()
        self.assertIn("if (session_generation == HW_SCRIPT_SESSION) return error_result", dispatch)

    def test_the_scripts_partition_moves_nothing(self):
        rows = [l.split(",") for l in (ROOT / "partitions_muse.csv").read_text().splitlines()
                if l.strip() and not l.startswith("#")]
        spans = sorted((int(r[3], 0), int(r[3], 0) + (int(r[4].strip().rstrip("M"), 0) << 20
                        if r[4].strip().endswith("M") else int(r[4], 0)), r[0].strip()) for r in rows)
        self.assertIn(("scripts",), [(s[2],) for s in spans])
        for (a0, a1, an), (b0, b1, bn) in zip(spans, spans[1:]):
            self.assertLessEqual(a1, b0, f"{an} overlaps {bn}")
        self.assertLessEqual(max(s[1] for s in spans), 16 << 20)   # fits the 16 MB boards


if __name__ == "__main__":
    unittest.main()
