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
"""The apps API 1.0: api.json, what gen_api.py makes from it (components/muse_apps/sdk/gadget.h,
the header apps include, among them), and an app built against it."""

import importlib.util
import json
import os
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
API = ROOT / "components/muse_apps/sdk" / "api"
HEADER = ROOT / "components/muse_apps/sdk" / "gadget.h"
EXAMPLE = API.parent / "examples" / "dashboard.c"


def wasm_cc():
    """A clang that targets wasm32, found as components/muse_apps/sdk/build.py finds one: wasi-sdk
    ($WASI_SDK, ~/wasi-sdk, /opt/wasi-sdk), else Zig's (pip install ziglang)."""
    for d in (os.environ.get("WASI_SDK"), os.path.expanduser("~/wasi-sdk"), "/opt/wasi-sdk"):
        if d and (Path(d) / "bin" / "clang").exists():
            return [str(Path(d) / "bin" / "clang"), "--target=wasm32"]
    if importlib.util.find_spec("ziglang") is None:
        return None
    return [sys.executable, "-m", "ziglang", "cc", "--target=wasm32-freestanding"]


def leb(b, i):
    r = s = 0
    while True:
        c = b[i]
        i += 1
        r |= (c & 0x7F) << s
        s += 7
        if c < 0x80:
            return r, i


def wasm_name(b, i):
    n, i = leb(b, i)
    return b[i:i + n].decode(), i + n


def read_module(data):
    """Imports as (module, name, params, results), exports and custom sections."""
    assert data[:8] == b"\0asm\x01\0\0\0"
    vt = {0x7F: "i", 0x7E: "I", 0x7D: "f", 0x7C: "F"}
    types, imports, exports, customs = [], [], [], {}
    i = 8
    while i < len(data):
        sid = data[i]
        size, j = leb(data, i + 1)
        end = j + size
        if sid == 0:
            n, k = wasm_name(data, j)
            customs[n] = data[k:end]
        elif sid == 1:
            count, k = leb(data, j)
            for _ in range(count):
                k += 1
                n, k = leb(data, k)
                params = "".join(vt[data[k + x]] for x in range(n))
                k += n
                n, k = leb(data, k)
                results = "".join(vt[data[k + x]] for x in range(n))
                k += n
                types.append((params, results))
        elif sid == 2:
            count, k = leb(data, j)
            for _ in range(count):
                m, k = wasm_name(data, k)
                f, k = wasm_name(data, k)
                kind = data[k]
                k += 1
                assert kind == 0, f"{m}.{f} isn't a function import"
                t, k = leb(data, k)
                imports.append((m, f) + types[t])
        elif sid == 7:
            count, k = leb(data, j)
            for _ in range(count):
                n, k = wasm_name(data, k)
                k += 1
                _, k = leb(data, k)
                exports.append(n)
        i = end
    return imports, exports, customs


def wamr_to_wasm(sig):
    """A WAMR native signature, "(ii$*~)i", as wasm value types, ("iiiii", "i")."""
    params, result = sig[1:].split(")")
    return params.replace("*~", "ii").replace("$", "i").replace("*", "i"), result


class AppApiTest(unittest.TestCase):
    def gen(self, *args, cwd=None):
        return subprocess.run([sys.executable, str(API / "gen_api.py"), *args],
                              capture_output=True, text=True, cwd=cwd)

    def test_generated_files_are_current(self):
        r = self.gen("--check")
        self.assertEqual(r.returncode, 0, r.stderr)

    def test_apps_include_the_generated_header(self):
        # One header, beside build.py, where MUSE.md and the apps take it from.
        self.assertIn("Generated from components/muse_apps/sdk/api/api.json by components/muse_apps/sdk/api/gen_api.py", HEADER.read_text())
        self.assertFalse((API / "gadget.h").exists(), "the header lives in apps/, not components/muse_apps/sdk/api/")
        self.assertIn('#include "../gadget.h"', EXAMPLE.read_text())

    def test_mistakes_in_api_json_are_caught(self):
        raw = json.loads((API / "api.json").read_text())
        cases = {
            "repeated property id": lambda a: a["props"].append([1, "x2", "int", "obj", "rw", ""]),
            "unknown widget": lambda a: a["props"].append([998, "nope", "int", ["dial"], "rw", ""]),
            "unknown argument type": lambda a: a["functions"][0]["args"].append(["x", "float", ""]),
            "wrong group prefix": lambda a: a["functions"].append(
                {"name": "beep", "group": "outputs", "args": [], "ret": None, "doc": ""}),
            "repeated constant": lambda a: a["values"].append(["ERR_ARG", "1", "int", ""]),
            "unknown permission": lambda a: a["functions"][0].update(perm="camera"),
            "repeated device field": lambda a: a["devices"]["well_known"].append(
                ["probe", "", [["v", "%"], ["v", "%"]], [], []]),
            "action parameter range": lambda a: a["devices"]["well_known"].append(
                ["valve", "", [], [["open", "", [["pct", "%", 100, 0]]]], []]),
        }
        for what, mutate in cases.items():
            with self.subTest(what), tempfile.TemporaryDirectory() as d:
                bad = json.loads(json.dumps(raw))
                mutate(bad)
                (Path(d) / "api.json").write_text(json.dumps(bad))
                r = self.gen("--check", "--dir", d)
                self.assertEqual(r.returncode, 1)
                self.assertIn("api.json:", r.stderr)

    def test_ids_and_values_match_lvgl_where_passed_through(self):
        # The firmware hands align and dir values to LVGL unchanged.
        raw = json.loads((API / "api.json").read_text())
        enums = {e["name"]: {n: v for n, v, _ in e["values"]} for e in raw["ui_enums"]}
        self.assertEqual(enums["align"]["CENTER"], 9)          # LV_ALIGN_CENTER
        self.assertEqual(enums["align"]["OUT_RIGHT_BOTTOM"], 21)
        self.assertEqual(enums["dir"]["ALL"], 15)              # LV_DIR_ALL

    def test_example_builds_on_the_host(self):
        cc = os.environ.get("CC") or shutil.which("cc") or shutil.which("clang") or shutil.which("gcc")
        if not cc:
            self.skipTest("no host C compiler")
        with tempfile.TemporaryDirectory() as d:
            r = subprocess.run(shlex.split(cc) + ["-std=c11", "-Wall", "-Wextra", "-Werror", "-c", str(EXAMPLE),
                                "-o", str(Path(d) / "dashboard.o")], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr)

    def test_example_imports_match_the_firmware_signatures(self):
        cc = wasm_cc()
        if not cc:
            self.skipTest("no wasm32 clang (set WASI_SDK, or pip install ziglang)")
        with tempfile.TemporaryDirectory() as d:
            out = Path(d) / "dashboard.wasm"
            r = subprocess.run(cc + ["-O2", "-nostdlib", "-Wl,--no-entry", "-Wall", "-Wextra", "-Werror",
                                     "-o", str(out), str(EXAMPLE)], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr)
            imports, exports, customs = read_module(out.read_bytes())
        capabilities = json.loads((API / "api.capabilities.json").read_text())
        self.assertGreater(len(imports), 20)
        for module, name, params, results in imports:
            with self.subTest(name):
                self.assertEqual(module, "gadget")
                self.assertIn(name, capabilities["functions"])
                self.assertEqual((params, results), wamr_to_wasm(capabilities["functions"][name]))
        self.assertIn("on_start", exports)
        manifest = json.loads(customs["gadget.manifest"].rstrip(b"\0"))
        required_api = tuple(map(int, manifest["api"].split(".")))
        served_api = tuple(map(int, capabilities["api"].split(".")))
        self.assertEqual(required_api[0], served_api[0])
        self.assertLessEqual(required_api[1], served_api[1])

    def test_every_signature_is_one_wamr_takes(self):
        capabilities = json.loads((API / "api.capabilities.json").read_text())
        for name, sig in capabilities["functions"].items():
            params, result = wamr_to_wasm(sig)
            self.assertRegex(params, r"^[iI]*$", name)
            self.assertIn(result, ("", "i", "I"), name)


if __name__ == "__main__":
    unittest.main()
