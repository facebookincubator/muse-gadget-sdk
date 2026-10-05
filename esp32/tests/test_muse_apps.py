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

"""Apps and the pet: the example apps the firmware ships (main/apps) checked
against what components/muse/muse_apps.c builds and run in the real script
sandbox; and the pet's care model (components/muse/muse_pet.c)."""

from pathlib import Path
import json
import os
import re
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
APPS = ROOT / "main/apps"
JSON = Path(os.environ.get(
    "CJSON_SOURCE_DIR", ROOT / "managed_components/espressif__cjson/cJSON"
))
LUA = ROOT / "components/lua/src"
LUA_FILES = """lapi lcode lctype ldebug ldo ldump lfunc lgc llex lmem lobject lopcodes lparser lstate
    lstring ltable ltm lundump lvm lzio lauxlib lbaselib lcorolib lmathlib lstrlib ltablib lutf8lib""".split()

SCREEN = 412
TYPES = """box row column label button switch checkbox slider arc bar spinner image chart led roller
    dropdown line canvas qr table input""".split()
COMMON = """type id children w h x y align bg bg_opa opa radius border border_color pad gap font
    text_align hidden scroll clickable live justify items wrap""".split()
OWN = {
    "label": "text icon color long", "button": "text icon color talk photo", "checkbox": "text icon color on",
    "switch": "on color", "slider": "min max value color", "bar": "min max value color",
    "arc": "min max value start end rotation thickness knob color", "spinner": "color",
    "image": "src zoom angle", "chart": "kind min max points series push", "led": "color brightness on",
    "roller": "options selected rows", "dropdown": "options selected", "line": "points color width",
    "canvas": "fill draw clear add", "qr": "text size color", "table": "rows col_w",
    "input": "text placeholder",
}
PAGE = "id title order bg layout children _about".split()


def source(path):
    return (ROOT / path).read_text()


def cc():
    return shlex.split(os.environ.get("CC", "cc"))


def library_ids():
    text = source("main/muse_hw_apps.c")
    return re.findall(r"X\((\w+)\)", re.search(r"#define LIBRARY\(X\) (.*)", text).group(1))


def widgets(node, depth=1):
    for kid in node.get("children", []):
        yield kid, depth
        yield from widgets(kid, depth + 1)


class AppLibraryTest(unittest.TestCase):
    def test_the_library_is_what_main_apps_holds(self):
        files = sorted(p.stem for p in APPS.glob("*.json"))
        self.assertEqual(sorted(library_ids()), files)
        for name in files:
            self.assertTrue((APPS / f"{name}.lua").exists(), name)

    def test_every_page_is_one_muse_apps_c_builds(self):
        limits = source("components/muse/muse_apps.c")
        widgets_max = int(re.search(r"#define WIDGETS_MAX (\d+)", limits).group(1))
        depth_max = int(re.search(r"#define DEPTH_MAX (\d+)", limits).group(1))
        def_max = int(re.search(r"#define DEF_MAX \((\d+) \* 1024\)", source("main/muse_hw_apps.c")).group(1)) * 1024
        script_max = int(re.search(r"#define SCRIPT_MAX \((\d+) \* 1024\)", source("main/muse_script.c")).group(1)) * 1024
        apps_max = int(re.search(r"#define MUSE_APPS_MAX (\d+)", source("components/muse/muse_apps.h")).group(1))
        self.assertLess(len(library_ids()), apps_max, "no room left for Muse's own apps")
        orders = {}
        for path in sorted(APPS.glob("*.json")):
            name = path.stem
            page = json.loads(path.read_text())
            self.assertEqual(page["id"], name)
            self.assertRegex(name, r"^[a-z0-9_-]{1,15}$")
            self.assertNotIn(name, ("face", "pet", "settings"))
            self.assertTrue(page.get("title") and page.get("_about"), name)
            self.assertLessEqual(set(page), set(PAGE), name)
            self.assertNotIn(page["order"], orders, f"{name} and {orders.get(page['order'])} share an order")
            orders[page["order"]] = name
            self.assertLess(len(json.dumps(page, separators=(",", ":"))), def_max, name)
            self.assertLess(len((APPS / f"{name}.lua").read_bytes()), script_max, name)
            ids = set()
            found = list(widgets(page))
            self.assertLessEqual(len(found) + 1, widgets_max, name)
            for w, depth in found:
                where = f"{name}.{w.get('id')}"
                self.assertIn(w["type"], TYPES, where)
                self.assertLessEqual(depth, depth_max, where)
                self.assertRegex(w["id"], r"^[a-z0-9_]{1,15}$", where)
                self.assertNotIn(w["id"], ids, where)
                ids.add(w["id"])
                self.assertLessEqual(set(w), set(COMMON) | set(OWN.get(w["type"], "").split()), where)
                self.assertEqual("min" in w, "max" in w, f"{where}: min and max go together")
                for key in ("w", "h"):
                    if isinstance(w.get(key), int):
                        self.assertLessEqual(w[key], SCREEN, where)

    @unittest.skipUnless((JSON / "cJSON.c").exists(), "needs cJSON: run idf.py build, or set CJSON_SOURCE_DIR")
    def test_the_scripts_drive_their_pages_in_the_sandbox(self):
        with tempfile.TemporaryDirectory() as out:
            binary = Path(out) / "library"
            defs = ["-DLUAI_MAXCCALLS=48", "-DMAXCCALLS=64", "-DLUAI_MATCHSTEP_FUNC=sb_matchstep"]
            cmd = [*cc(), "-std=gnu99", "-O1", *defs, "-I", str(LUA), "-I", str(JSON), "-I", str(ROOT / "main"),
                   *[str(LUA / f"{f}.c") for f in LUA_FILES], str(JSON / "cJSON.c"),
                   str(ROOT / "main/muse_script_sandbox.c"), str(ROOT / "tests/muse_app_library_harness.c"),
                   "-lm", "-o", str(binary)]
            built = subprocess.run(cmd, capture_output=True, text=True)
            self.assertEqual(built.returncode, 0, built.stderr[-3000:])
            ran = subprocess.run([str(binary), str(APPS), *library_ids()], capture_output=True, text=True,
                                 timeout=300)
            self.assertEqual(ran.returncode, 0, ran.stdout[-3000:] + ran.stderr[-3000:])

    def test_scripts_cant_install_examples(self):
        # app.install writes flash; app.get and app.library read it. A
        # script's task, its stack in PSRAM, can do neither.
        text = source("main/muse_script.c")
        allowed = text[text.index("static const char *const COMMANDS[]"):]
        allowed = allowed[:allowed.index("NULL,")]
        for command in ("app.install", "app.get", "app.library", "app.example", "script.read"):
            self.assertNotIn(f'"{command}"', allowed)
        self.assertIn('"pet.care"', allowed)


class PetModelTest(unittest.TestCase):
    def test_the_care_model(self):
        text = source("components/muse/muse_pet.c")
        model = text[text.index("/* ---- The model (host-tested"):text.index("/* ---- Model end ---- */")]
        with tempfile.TemporaryDirectory() as out:
            (Path(out) / "muse_pet_model.inc").write_text(model)
            binary = Path(out) / "pet"
            cmd = [*cc(), "-std=gnu99", "-O1", "-Wall", "-Werror", "-Wno-unused-function", "-I", out,
                   "-I", str(ROOT / "components/muse"), str(ROOT / "tests/muse_pet_harness.c"), "-lm",
                   "-o", str(binary)]
            built = subprocess.run(cmd, capture_output=True, text=True)
            self.assertEqual(built.returncode, 0, built.stderr[-3000:])
            ran = subprocess.run([str(binary)], capture_output=True, text=True, timeout=60)
            self.assertEqual(ran.returncode, 0, ran.stdout[-3000:] + ran.stderr[-3000:])

    def test_saves_stay_off_the_callers_task(self):
        # A script's pet.care runs on its PSRAM-stacked task: NVS only on the esp_timer task.
        text = source("components/muse/muse_pet.c")
        care = text[text.index("const char *muse_pet_care("):]
        care = care[:care.index("\n}\n")]
        self.assertNotIn("save_locked", care)
        self.assertIn("save_soon", care)


if __name__ == "__main__":
    unittest.main()
