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

"""WebAssembly apps (CONFIG_MUSE_WASM_APPS, components/muse_apps), API 1.0: the
component stays optional and Link reaches it only under the option; the app.*
commands are advertised and dispatched; the firmware's import table is the
generated one, and nothing of API 0 is left; the apps build with components/muse_apps/sdk/build.py,
which refuses what the gadget refuses; apps keep out of internal RAM; and
MUSE.md, the guide for Muse, is current and fits in one message. The API's own
files are tests/test_app_api.py's."""

import importlib.util
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
APPS = ROOT / "components/muse_apps/sdk"
COMPONENT = ROOT / "components/muse_apps"
EXAMPLES = ["components/muse_apps/sdk/examples/hello.c", "components/muse_apps/sdk/examples/stopwatch.c", "components/muse_apps/sdk/examples/chat.c", "components/muse_apps/sdk/examples/dashboard.c",
            "components/muse_apps/sdk/examples/canvas_effects.c"]
COMMANDS = ["app.install", "app.list", "app.remove", "app.run", "app.stop", "app.status", "app.logs", "app.api",
            "files.put", "files.list", "files.remove"]


def read(path):
    return (ROOT / path).read_text()


def block(text, start, end="#endif"):
    i = text.index(start)
    return text[i:text.index(end, i)]


def function(text, signature):
    i = text.index(signature)
    return text[i:text.index("\n}\n", i)]


def capabilities():
    return json.loads(read("components/muse_apps/sdk/api/api.capabilities.json"))


def have_wasm_compiler():
    """What components/muse_apps/sdk/build.py looks for: wasi-sdk, or the ziglang package of this Python."""
    for d in (os.environ.get("WASI_SDK"), os.path.expanduser("~/wasi-sdk"), "/opt/wasi-sdk"):
        if d and (Path(d) / "bin" / "clang").exists():
            return True
    return importlib.util.find_spec("ziglang") is not None


def build(*args):
    return subprocess.run([sys.executable, str(APPS / "build.py"), *map(str, args)],
                          capture_output=True, text=True, cwd=ROOT)


def enum_values(text, prefix):
    """NAME -> value for the enum members starting with prefix, counting as C does."""
    out = {}
    for body in re.findall(r"enum\s*\{([^}]*)\}", re.sub(r"/\*.*?\*/", "", text, flags=re.S)):
        value = -1
        for item in filter(None, (i.strip() for i in body.split(","))):
            name, _, given = (p.strip() for p in item.partition("="))
            value = int(given, 0) if given else value + 1
            if name.startswith(prefix):
                out[name[len(prefix):]] = value
    return out


class WasmAppsTest(unittest.TestCase):
    def test_component_instructions_are_local(self):
        parent = read("AGENTS.md")
        overview = block(parent, "## WebAssembly apps\n", "## Configuration gotchas\n")
        self.assertLess(len(overview), 1000)
        self.assertIn("components/muse_apps/AGENTS.md", overview)
        instructions = read("components/muse_apps/AGENTS.md")
        for topic in ("CONFIG_MUSE_WASM_APPS", "CONFIG_CAPABILITIES_REGISTRY",
                      "muse_apps_alloc", "api.json", "cv_rect_style",
                      "tests.test_wasm_apps", "muse_brief.py", "app.api"):
            self.assertIn(topic, instructions)
        self.assertIn("[the ESP32 instructions](../../AGENTS.md)", instructions)
        self.assertIn("[the component instructions](../AGENTS.md)", read("components/muse_apps/sdk/README.md"))
        self.assertIn("[component instructions](../../AGENTS.md)", read("components/muse_apps/sdk/api/README.md"))

    def test_examples_are_grouped_and_guide_hello_is_standalone(self):
        for name in ("hello.c", "stopwatch.c", "chat.c", "dashboard.c", "canvas_effects.c"):
            self.assertTrue((APPS / "examples" / name).is_file(), name)
            self.assertFalse((APPS / name).exists(), name)
        self.assertFalse((APPS / "api" / "examples").exists())
        if not have_wasm_compiler():
            self.skipTest("no wasm32 compiler")
        guide = read("components/muse_apps/sdk/MUSE.md")
        with tempfile.TemporaryDirectory() as d:
            folder = Path(d)
            for name, lang in (("gadget.h", "c"), ("hello.c", "c"), ("build.py", "python")):
                pattern = r"<!-- BEGIN %s -->\n```%s\n(.*?)\n```\n<!-- END %s -->" % (
                    re.escape(name), lang, re.escape(name))
                body = re.search(pattern, guide, re.S).group(1)
                (folder / name).write_text(body + "\n")
            self.assertIn('#include "gadget.h"', (folder / "hello.c").read_text())
            r = subprocess.run([sys.executable, str(folder / "build.py"), str(folder / "hello.c"),
                                "-o", str(folder / "hello.wasm")], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr)

    def test_component_is_optional(self):
        cmake = read("components/muse_apps/CMakeLists.txt")
        self.assertIn('"muse_apps.c"', block(cmake, "if(CONFIG_MUSE_WASM_APPS)", "endif()"))
        self.assertIn("WASM_ENABLE_INSTRUCTION_METERING=1",
                      block(cmake, "if(CONFIG_MUSE_WASM_APPS)\n    # WAMR", "endif()"))
        # Whole, so the modules' imports win over the weak stubs.
        self.assertIn("WHOLE_ARCHIVE", cmake)
        self.assertIn("#define MUSE_APPS_DEFINE_STUBS", read("components/muse_apps/muse_apps_stubs.c"))
        manifest = read("components/muse_apps/idf_component.yml")
        # Keyed on an option every target defines, as MUSE_BOARD_ID rules are.
        self.assertIn('if: "$CONFIG{MUSE_APPS_WAMR} == \\"y\\""', manifest)
        # WAMR 2.4.5 or later: earlier fast-interpreter loaders overflow
        # (CVE-2026-54912, CVE-2025-64713).
        wamr = block(manifest, "  wasm-micro-runtime:", "rules:")
        self.assertIn("git: https://github.com/bytecodealliance/wasm-micro-runtime.git", wamr)
        self.assertIn("version: 25bd7eb63e828e4bd242cc9b38d260b4b31c6605", wamr)
        self.assertNotIn("espressif/wasm-micro-runtime", manifest)
        kconfig = read("components/muse_apps/Kconfig")
        self.assertIn('default "y" if MUSE_WASM_APPS', kconfig)
        self.assertIn("default n", block(kconfig, "config MUSE_WASM_APPS", "help"))
        self.assertIn("select MUSE_APP_VIEW", kconfig)
        # The overlay that turns it on, and the interpreter it wants.
        overlay = read("devices/sdkconfig.muse-apps")
        for line in ["CONFIG_MUSE_WASM_APPS=y", "CONFIG_WAMR_ENABLE_AOT=n", "CONFIG_WAMR_ENABLE_LIBC_WASI=n"]:
            self.assertIn(line, overlay)
        self.assertIn("sdkconfig.muse-apps", block(read("tools/muse/board.sh"), "MUSE_APPS:-", "\nfi"))
        # Link knows WAMR only through the component.
        for path in ["main/CMakeLists.txt", "main/idf_component.yml", "CMakeLists.txt"]:
            self.assertNotIn("wasm-micro-runtime", read(path), path)
        self.assertIn("muse_apps", read("main/CMakeLists.txt"))
        # Every call into it is under the option.
        for path, call in [("main/app.c", "muse_apps_handles(command)"),
                           ("main/app.c", "muse_apps_command("),
                           ("main/noise_control.cpp", "muse_apps_advertise(commands)"),
                           ("main/muse_glue.c", "muse_apps_start(noise_ctrl_send_command_result)"),
                           ("main/muse_glue.c", "muse_apps_console(line)")]:
            source = read(path)
            self.assertIn(call, source)
            before = source[:source.index(call)]
            self.assertGreater(before.rfind("#if CONFIG_MUSE_WASM_APPS"), before.rfind("#endif"), path)

    def test_app_view_belongs_to_the_ui(self):
        muse = read("components/muse/CMakeLists.txt")
        self.assertIn('"muse_app_view.c"', block(muse, "if(CONFIG_MUSE_APP_VIEW)", "endif()"))
        ui = read("components/muse/muse_ui.c")
        self.assertIn("muse_app_view_tick()", block(ui, "#if CONFIG_MUSE_APP_VIEW\n    if"))
        for path in ["components/muse/muse_ui.c", "components/muse/muse_input.c",
                     "components/muse/muse_app_view.c"]:
            self.assertNotIn("WASM", read(path), path)

    def test_commands_are_advertised_and_dispatched(self):
        apps = read("components/muse_apps/muse_apps.c")
        advertised = function(apps, "void muse_apps_advertise(")
        handled = block(apps, "static const char *const s_commands[]", ";")
        for name in COMMANDS:
            self.assertIn('add_command(commands, "%s"' % name, advertised)
            self.assertIn('"%s"' % name, handled)
        app = read("main/app.c")
        start = app.index("static cJSON *on_ws_command(")
        dispatch = app[start:app.index("unsupported command", start)]
        self.assertIn("muse_apps_command(", block(dispatch, "#if CONFIG_MUSE_WASM_APPS"))
        # The console's lines for the bench tool, beside the loader's.
        console = function(apps, "bool muse_apps_console(")
        for line in ['"app.status"', '"app.logs"', '"app.api"', '"app.begin"', '"app.run="', '"app.event="',
                     '"app.install="', '"app.install.upload="', '"app.remove="', '"app.list"', '"app.launcher"',
                     '"app.key"', '"files.put="', '"files.upload="', '"files.remove="', '"files.list"']:
            self.assertIn(line, console)
        tool = read("tools/muse/app.py")
        for line in ['"app.status"', '"app.logs"', '"app.api=0"', '"app.api=%d"', '"app.run="', '"app.event="',
                     '"app.install="', '"app.install.upload="', '"app.remove="', '"app.list"', '"app.launcher"',
                     '"app.key"', '"files.put="', '"files.upload="', '"files.remove="', '"files.list"']:
            self.assertIn(line, tool)

    def test_apps_are_installed_on_the_gadget(self):
        # The partition comes last in the 16 MB table, so no offset moves.
        rows = [line.split(",") for line in read("partitions_muse.csv").splitlines()
                if line.strip() and not line.startswith("#")]
        self.assertEqual([[c.strip() for c in r[:3]] for r in rows[-2:]],
                         [["apps", "data", "littlefs"], ["files", "data", "littlefs"]])
        self.assertEqual(rows[-3][0], "prod_bak")
        offsets = [int(r[3], 16) for r in rows]
        self.assertEqual(offsets, sorted(offsets))
        cmake = read("components/muse_apps/CMakeLists.txt")
        for module in ["store", "catalog", "launcher", "icons", "flash"]:
            self.assertRegex(cmake, r"foreach\(module [^)]*\b%s\b" % module)
        manifest = read("components/muse_apps/idf_component.yml")
        self.assertIn('if: "$CONFIG{MUSE_APPS_WAMR} == \\"y\\""', manifest[manifest.index("joltwallet/littlefs"):])
        # Only the store's task touches the catalog and the flash; the app task
        # asks it. Its stack is in PSRAM only where flash writes leave the caches on.
        self.assertNotIn("muse_apps_catalog_", read("components/muse_apps/muse_apps.c"))
        self.assertNotIn("muse_apps_catalog_", read("components/muse_apps/muse_apps_launcher.c"))
        store = read("components/muse_apps/muse_apps_store.c")
        self.assertIn("xTaskCreateWithCaps(store_task", store)
        # Internal RAM is audio's and TLS's: the lists live in PSRAM.
        for path in ["muse_apps_store.c", "muse_apps_catalog.c", "muse_apps_launcher.c"]:
            source = read("components/muse_apps/" + path)
            for decl in re.findall(r"^static [^(=;]*\[MUSE_APPS_MAX\];", source, re.M):
                self.assertIn("EXT_RAM_BSS_ATTR", decl, path)
        self.assertIn("CONFIG_LITTLEFS_MALLOC_STRATEGY_SPIRAM=y", read("devices/sdkconfig.muse-apps"))
        # The manifest names an install: id, launcher name, icon and colour.
        install = function(store, "static void install(")
        self.assertIn("muse_apps_manifest_info(", install)
        fields = [f[0] for f in json.loads(read("components/muse_apps/sdk/api/api.json"))["manifest"]["fields"]]
        for field in ["name", "title", "icon", "color"]:
            self.assertIn(field, fields)
        # Unpairing removes the apps, and an install still downloading doesn't land after it.
        self.assertIn("muse_apps_forget_all();",
                      function(read("main/app.c"), "static void finish_setup_reset_with_gate_held("))
        self.assertIn("forget_if_asked()", install)
        self.assertIn("muse_apps_flash_wipe(FILES_ROOT)", function(store, "static bool forget_if_asked("))

    def test_gadget_files(self):
        # Writes on the store's task, apps' reads on its reader task: neither is
        # the app task or the VM's, whose stacks are in PSRAM.
        store = read("components/muse_apps/muse_apps_store.c")
        self.assertIn('mount(FILES_LABEL, FILES_ROOT,', store)
        self.assertIn("xTaskCreateWithCaps(reader_task", function(store, "void muse_apps_store_start("))
        self.assertIn("TASK_STACK_CAPABILITIES", function(store, "void muse_apps_store_start(")
                      [function(store, "void muse_apps_store_start(").index("reader_task"):])
        self.assertIn("muse_apps_vm_file_stream(", function(store, "static void file_put("))
        for path in ["muse_apps.c", "muse_apps_files.c", "muse_apps_ui.c", "muse_apps_vm.c"]:
            self.assertNotIn("muse_apps_flash_read(", read("components/muse_apps/" + path), path)
            self.assertNotIn("fopen(", read("components/muse_apps/" + path), path)
        # Apps reach them with a path from "/", and the files.gadget permission.
        api = json.loads(read("components/muse_apps/sdk/api/api.json"))
        self.assertIn("files.gadget", [p[0] for p in api["manifest"]["permissions"]])
        apps = read("components/muse_apps/muse_apps.c")
        self.assertIn('muse_apps_permitted("files.gadget")', function(apps, "int32_t muse_apps_gadget_path("))
        self.assertIn("muse_apps_store_file_read(", read("components/muse_apps/muse_apps_files.c"))
        self.assertIn("muse_apps_store_file_load(", read("components/muse_apps/muse_apps_ui.c"))

    def test_launcher_opens_from_settings(self):
        settings = read("components/muse/muse_settings_ui.c")
        self.assertIn("muse_app_view_home()", function(settings, "static void on_apps("))
        home = function(settings, "static void build_home(")
        self.assertIn('row(list, LV_SYMBOL_LIST, "Apps", NULL, on_apps, NULL);', home)
        self.assertLess(home.index('switch_row(list, "Run apps", muse_settings_apps_on(), on_apps_sw);'),
                        home.index('if (muse_settings_apps_on()) {'))
        before = home[:home.index('"Apps"')]
        self.assertGreater(before.rfind("#if CONFIG_MUSE_APP_VIEW"), before.rfind("#endif"))
        self.assertNotIn("WASM", settings)
        apps = read("components/muse_apps/muse_apps.c")
        self.assertIn("muse_app_view_set_home(open_launcher)", apps)
        # The power key: an app goes back to the launcher if it came from there.
        stop = function(apps, "static void handle_stop(")
        self.assertIn("bool back = s_app.from_launcher;", stop)
        self.assertIn("hide_launcher(false);", stop)
        # The launcher is a list of LVGL objects, sized from the screen, and
        # gone when it's hidden; button boards open it from the menu.
        launcher = read("components/muse_apps/muse_apps_launcher.c")
        self.assertIn("lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN)", launcher)
        self.assertIn("muse_app_view_size(&L.w, &L.h, &L.round)", function(launcher, "static void layout("))
        self.assertIn("muse_apps_safe_inset()", function(launcher, "static void layout("))
        self.assertNotIn("gadget_", launcher)
        self.assertNotIn("466", launcher)
        self.assertIn("muse_apps_launcher_hide();", function(apps, "static void hide_launcher("))
        self.assertFalse((ROOT / "components/muse_apps/muse_apps_paint.c").exists())
        menu = read("components/muse/muse_menu.c")
        self.assertRegex(menu, r"#if CONFIG_MUSE_APP_VIEW\s+ITEM_APPS_ON,\s+ITEM_APPS,")
        self.assertIn("muse_settings_set_apps_on(!muse_settings_apps_on());", menu)
        self.assertRegex(menu, r"case ITEM_APPS:[^\n]*\n\s*if \(!muse_settings_apps_on\(\)\) \{\s*break;\s*\}\s*muse_menu_close\(\);\s*muse_app_view_home\(\);")
        # LVGL's callbacks only post: the rest runs on the app task under the lock.
        for cb in ["static void on_row(", "static void on_answer("]:
            self.assertIn("muse_apps_post(", function(launcher, cb))
            self.assertNotIn("muse_apps_store_", function(launcher, cb))

    def test_budget_follows_free_psram(self):
        # Up to 2 MB, less where PSRAM is short, never under 1 MB, and the
        # gadget always keeps 1 MB: set when the app starts, reported as is.
        apps = read("components/muse_apps/muse_apps.c")
        self.assertIn("#define BUDGET_MAX_BYTES (2 * 1024 * 1024)", apps)
        self.assertIn("#define BUDGET_MIN_BYTES (1024 * 1024)", apps)
        now = function(apps, "static size_t budget_now(")
        self.assertIn("free_psram - RESERVE_BYTES", now)
        self.assertIn("BUDGET_MAX_BYTES ? BUDGET_MAX_BYTES", now)
        self.assertNotRegex(apps, r"\bBUDGET_BYTES\b")
        self.assertIn("if (budget < BUDGET_MIN_BYTES)", apps)
        self.assertIn(".max_memory_pages = budget / 65536", apps)
        for alloc in ["static void *mem_alloc(", "bool muse_apps_charge(", "void *muse_apps_alloc("]:
            self.assertIn("fits_locked(", function(apps, alloc))
        # Checked without wrapping around: a size near SIZE_MAX never passes.
        fits = function(apps, "static bool fits_locked(")
        self.assertIn("used <= s_budget && bytes <= s_budget - used", fits)
        self.assertNotRegex(apps, r"used_locked\(\) \+ \w+ (<=|>) s_budget")
        self.assertIn("size > UINT32_MAX - sizeof(block_t)", function(apps, "static void *mem_alloc("))
        self.assertIn("bytes > SIZE_MAX - sizeof(block_t)", function(apps, "void *muse_apps_alloc("))
        self.assertEqual((apps + read("components/muse_apps/muse_apps_capabilities.c")).count('"budget_kb", (double)(muse_apps_mem_budget() / 1024)'), 2)
        api = json.loads(read("components/muse_apps/sdk/api/api.json"))
        self.assertEqual(dict((l[0], l[1]) for l in api["limits"])["budget_bytes"], 2 * 1024 * 1024)

    def test_exit_hold_on_every_board(self):
        # Most boards have no power key: holding the exit button 2 s closes the
        # app or the launcher, and its release goes to nobody.
        view = read("components/muse/muse_app_view.c")
        self.assertIn("#define EXIT_HOLD_US (2000 * 1000)", view)
        edge = function(view, "static void button_edge(")
        self.assertIn("esp_timer_start_once(s_exit_timer, EXIT_HOLD_US)", edge)
        self.assertIn("atomic_exchange(&s_exit_swallow, false)", edge)
        self.assertIn("s_cbs.exit(MUSE_APP_STOP_USER)", function(view, "static void on_exit_hold("))
        exit_button = function(view, "int muse_app_view_exit_button(")
        self.assertIn("return MUSE_APP_BUTTON_PRIMARY;", exit_button)
        self.assertNotIn("muse_board", exit_button)
        self.assertIn('"exit_hold"', read("components/muse_apps/muse_apps_capabilities.c"))
        self.assertNotIn("aux_taps_only", read("components/muse/muse_board.h"))
        self.assertNotIn("aux_taps_only", view)
        self.assertIn("exit_hold", read("components/muse_apps/sdk/MUSE.md"))

    def test_import_table_comes_from_the_generated_header(self):
        apps = read("components/muse_apps/muse_apps.c")
        self.assertIn("MUSE_APPS_FUNCTIONS(GADGET_FN)", apps)
        self.assertIn('wasm_runtime_register_natives("gadget"', apps)
        self.assertIn('#include "muse_apps_api.h"', read("components/muse_apps/muse_apps_priv.h"))
        self.assertFalse((COMPONENT / "muse_apps_host.c").exists(), "API 0's hand-written import table")
        # The table, components/muse_apps/sdk/gadget.h and api.capabilities.json name the same functions.
        table = dict(re.findall(r'GADGET_FN\((\w+), "([^"]*)"', read("components/muse_apps/muse_apps_api.h")))
        self.assertEqual(table, capabilities()["functions"])
        imports = re.findall(r"^GADGET_IMPORT\((\w+)\)", read("components/muse_apps/sdk/gadget.h"), re.M)
        self.assertEqual(sorted(imports), sorted(table))
        # The firmware checks a module's imports against what it registered.
        self.assertIn("strcmp(imp.module_name, \"gadget\")", function(apps, "static bool imports_allowed("))

    def test_nothing_of_api_0_is_left(self):
        api0 = re.compile(r"\b(gfx_\w+|vm_(request|read|status|close)\s*\(|muse_send\s*\(|EV_VM\b|"
                          r"EV_BUTTON_(DOWN|UP)\b|BUTTON_(TALK|SECOND)\b|MUSE_APPS_EV_\w+)")
        self.assertFalse((APPS / "API.md").exists())
        self.assertFalse((APPS / "api" / "gadget.h").exists())
        places = [ROOT / p for p in ["components/muse_apps", "components/muse", "components/capabilities_registry",
                                     "tools/muse", "devices"]]
        # Link's own files that reach the apps (image_fetch.c's vm_read() is its own).
        glue = [p for p in (ROOT / "main").glob("*.c*") if "muse_apps" in p.read_text(errors="replace")]
        self.assertTrue(glue)
        found = []
        for place in places + glue:
            for path in place.rglob("*") if place.is_dir() else [place]:
                if path.suffix not in (".c", ".cpp", ".h", ".py", ".md", ".txt", ".yml") and \
                        not path.name.startswith(("Kconfig", "sdkconfig")):
                    continue
                for n, line in enumerate(path.read_text(errors="replace").splitlines(), 1):
                    if api0.search(line):
                        found.append("%s:%d: %s" % (path.relative_to(ROOT), n, line.strip()))
        for doc in ["AGENTS.md", "README.md", "../README.md"]:
            for n, line in enumerate(read(doc).splitlines(), 1):
                if api0.search(line) or "API.md" in line:
                    found.append("%s:%d: %s" % (doc, n, line.strip()))
        self.assertEqual(found, [])

    def test_view_events_are_the_apis(self):
        api = json.loads(read("components/muse_apps/sdk/api/api.json"))
        enums = {e["name"]: {n: v for n, v, _ in e["values"]} for e in api["enums"]}
        events = {name: v for name, v, _, _, _ in api["events"]}
        view = read("components/muse/muse_app_view.h")
        for name, value in enum_values(view, "MUSE_APP_EV_").items():
            self.assertEqual(events[name], value, name)
        prefixes = {"button": "MUSE_APP_BUTTON_", "button_action": "MUSE_APP_BTN_",
                    "gesture": "MUSE_APP_GESTURE_", "stop_reason": "MUSE_APP_STOP_"}
        for enum in api["enums"]:
            prefix = prefixes.get(enum["name"])
            if not prefix:
                continue
            ours = enum_values(view, prefix)
            self.assertTrue(ours, prefix)
            for name, value in ours.items():
                self.assertEqual(enums[enum["name"]][name], value, prefix + name)
        # The runtime checks the same when it builds.
        self.assertEqual(read("components/muse_apps/muse_apps.c").count("_Static_assert(MUSE_APP_"), 3)

    def test_build_refuses_what_the_gadget_refuses(self):
        source = read("components/muse_apps/sdk/build.py")
        self.assertIn('"-mexec-model=reactor"', source)
        self.assertIn('"on_start" not in exports', source)
        self.assertIn('customs.get("gadget.manifest")', source)
        self.assertIn('wasm_runtime_lookup_function(s_app.inst, "_initialize")', read("components/muse_apps/muse_apps.c"))
        if not have_wasm_compiler():
            self.skipTest("no wasm compiler (set WASI_SDK, or run with a Python that has ziglang)")
        cases = {
            "wasi": ('__attribute__((import_module("wasi_snapshot_preview1"), import_name("fd_write"))) '
                     "int fd_write(int, int, int, int);\n"
                     'GADGET_MANIFEST("{\\"api\\":\\"1.0\\",\\"name\\":\\"t\\"}");\n'
                     "GADGET_EXPORT(on_start) void on_start(void) { fd_write(1, 0, 0, 0); }\n",
                     "gadgets don't offer"),
            "not in the API": ('__attribute__((import_module("gadget"), import_name("gfx_clear"))) void gfx_clear(int);\n'
                               "GADGET_EXPORT(on_start) void on_start(void) { gfx_clear(0); }\n", "doesn't have"),
            "wrong signature": ('__attribute__((import_module("gadget"), import_name("sys_log"))) void my_log(int, int);\n'
                                "GADGET_EXPORT(on_start) void on_start(void) { my_log(1, 2); }\n", "the API's is"),
            "no on_start": ('GADGET_MANIFEST("{\\"api\\":\\"1.0\\",\\"name\\":\\"t\\"}");\n'
                            "GADGET_EXPORT(on_frame) void on_frame(int32_t t) { (void)t; }\n", "exports no on_start"),
            "another major API": ('GADGET_MANIFEST("{\\"api\\":\\"2.0\\",\\"name\\":\\"t\\"}");\n'
                                  "GADGET_EXPORT(on_start) void on_start(void) {}\n", "asks for API 2.0"),
            "bad name": ('GADGET_MANIFEST("{\\"api\\":\\"1.0\\",\\"name\\":\\"My App\\"}");\n'
                         "GADGET_EXPORT(on_start) void on_start(void) {}\n", "name must be"),
            "not JSON": ('GADGET_MANIFEST("{api: 1.0}");\nGADGET_EXPORT(on_start) void on_start(void) {}\n',
                         "isn't JSON"),
        }
        with tempfile.TemporaryDirectory() as d:
            for what, (body, message) in cases.items():
                with self.subTest(what):
                    src, out = Path(d) / "t.c", Path(d) / "t.wasm"
                    src.write_text('#include "gadget.h"\n' + body)
                    r = build(src, "-o", out)
                    self.assertEqual(r.returncode, 1, r.stderr)
                    self.assertIn(message, r.stderr)
                    self.assertFalse(out.exists(), "the refused module is removed")
            # Allowed, with warnings: no manifest; a call without its permission.
            folder = Path(d) / "warn"
            folder.mkdir()
            (folder / "warn.c").write_text('#include "gadget.h"\n'
                                           'GADGET_MANIFEST("{\\"api\\":\\"1.0\\",\\"name\\":\\"warn\\"}");\n'
                                           "GADGET_EXPORT(on_start) void on_start(void) { chat_open(); }\n")
            r = build(folder)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertTrue((folder / "app.wasm").exists(), "a folder builds into its app.wasm")
            self.assertIn('add "chat.read" to the manifest', r.stderr)
            (folder / "warn.c").write_text('#include "gadget.h"\nGADGET_EXPORT(on_start) void on_start(void) {}\n')
            r = build(folder)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertIn("no manifest", r.stderr)

    def test_example_apps_build(self):
        if not have_wasm_compiler():
            self.skipTest("no wasm compiler (set WASI_SDK, or run with a Python that has ziglang)")
        sys.path.insert(0, str(APPS))
        try:
            from build import read_module
        finally:
            sys.path.remove(str(APPS))
        with tempfile.TemporaryDirectory() as d:
            for path in EXAMPLES:
                with self.subTest(path):
                    out = Path(d) / (Path(path).stem + ".wasm")
                    r = build(path, "-o", out)
                    self.assertEqual(r.returncode, 0, r.stderr)
                    self.assertNotIn("warning", r.stderr)
                    imports, exports, customs = read_module(out.read_bytes())
                    manifest = json.loads(customs["gadget.manifest"].rstrip(b"\0"))
                    required_api = tuple(map(int, manifest["api"].split(".")))
                    served_api = tuple(map(int, capabilities()["api"].split(".")))
                    self.assertEqual(required_api[0], served_api[0])
                    self.assertLessEqual(required_api[1], served_api[1])
                    self.assertEqual(manifest["name"], Path(path).stem.replace("_", "-"))
                    if path == "components/muse_apps/sdk/examples/hello.c":
                        # MUSE.md's starting point: small, and drawn by widgets, not frames.
                        self.assertLess(out.stat().st_size, 2048)
                        self.assertNotIn("on_frame", exports)

    def test_apps_keep_out_of_internal_ram(self):
        """Internal RAM is what Wi-Fi, Noise and the panel's DMA run short of
        (display.draw_file needs about 30 KB of it): apps take theirs from PSRAM.
        Every file of the component is checked, the modules to come included."""
        files = [ROOT / "components/muse/muse_app_view.c"] + sorted(COMPONENT.glob("*.c"))
        self.assertGreater(len(files), 4)
        for path in files:
            source = path.read_text()
            name = path.relative_to(ROOT)
            defines = {k: int(v) for k, v in re.findall(r"#define\s+(\w+)\s+\(?(\d+)\)?\s", source)}
            for kind, var, size in re.findall(
                    r"^[ \t]*static\s+(?!const)([\w ]+?)\s+\**(\w+)\[(\w+)\]\s*[;=\[]", source, re.M):
                count = int(size) if size.isdigit() else defines.get(size, 0)
                width = 1 if "char" in kind or "int8" in kind else 4
                self.assertLess(count * width, 512, "%s: static %s[%s] belongs in PSRAM" % (name, var, size))
            if path.name == "muse_apps_store.c":
                # The store writes flash, which turns the caches off: without
                # XIP from PSRAM its stack has to be internal, and only then.
                xip = ("#if CONFIG_SPIRAM_FETCH_INSTRUCTIONS && CONFIG_SPIRAM_RODATA\n"
                       "#define TASK_STACK_CAPABILITIES (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)\n#else\n"
                       "#define TASK_STACK_CAPABILITIES (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)\n#endif\n")
                self.assertIn(xip, source, name)
                source = source.replace(xip, "")
            # Queues and task stacks from internal RAM: none.
            for call in ["xQueueCreate(", "xQueueCreateStatic(", "xTaskCreate(", "xTaskCreatePinnedToCore(",
                         "MALLOC_CAP_INTERNAL", "MALLOC_CAP_DMA"]:
                self.assertNotIn(call, source, "%s: %s" % (name, call))
            for args in re.findall(r"xQueueCreateWithCaps\(([^;]*)\);", source):
                self.assertIn("MALLOC_CAP_SPIRAM", args, name)
            for args in re.findall(r"heap_caps_(?:malloc|calloc|realloc|aligned_alloc)\(([^;]*)\);", source):
                self.assertIn("MALLOC_CAP_SPIRAM", args, "%s: heap_caps_*(%s)" % (name, args))
        apps = read("components/muse_apps/muse_apps.c")
        # The app task's stack, WAMR's memory and the import table are in PSRAM.
        self.assertIn("cfg.stack_alloc_caps = MALLOC_CAP_SPIRAM", apps)
        self.assertIn("static const NativeSymbol natives[]", apps)
        self.assertIn("init.mem_alloc_option.allocator.malloc_func = (void *)mem_alloc;", apps)

    def test_linear_memory_is_psram_and_budgeted(self):
        """WAMR maps linear memory through its platform layer, not the runtime
        allocator. ESP-IDF's layer would take it from internal RAM first and
        outside the budget; the component's replaces it."""
        cmake = read("components/muse_apps/CMakeLists.txt")
        self.assertIn('esp-idf/espidf_memmap.c"\n        TARGET_DIRECTORY ${wamr_lib} PROPERTIES HEADER_FILE_ONLY TRUE', cmake)
        self.assertIn('target_sources(${wamr_lib} PRIVATE "${CMAKE_CURRENT_LIST_DIR}/muse_apps_wamr_mem.c")', cmake)
        self.assertNotIn('"muse_apps_wamr_mem.c"', block(cmake, "set(srcs)", "idf_component_register"))
        shim = read("components/muse_apps/muse_apps_wamr_mem.c")
        for fn in ["void *os_mmap(", "void *os_mremap(", "void os_munmap(", "int os_mprotect(",
                   "void os_dcache_flush(", "void os_icache_flush("]:
            self.assertIn(fn, shim)
        self.assertIn("prot & MMAP_PROT_EXEC ? NULL : muse_apps_linear_map(size)", shim)
        self.assertNotRegex(re.sub(r"/\*.*?\*/", "", shim, flags=re.S), r"heap_caps_\w+\(")
        apps = read("components/muse_apps/muse_apps.c")
        self.assertIn("mem_alloc((unsigned)size)", function(apps, "void *muse_apps_linear_map("))
        self.assertIn("memset(p, 0, size)", function(apps, "void *muse_apps_linear_map("))
        grow = function(apps, "void *muse_apps_linear_remap(")
        self.assertIn("fits_locked(more)", grow)
        self.assertIn("MALLOC_CAP_SPIRAM", grow)
        self.assertIn("memset((uint8_t *)(n + 1) + keep, 0, new_size - keep)", grow)
        self.assertIn("mem_free(p)", function(apps, "void muse_apps_linear_unmap("))

    def test_a_control_message_held_for_dma_says_so(self):
        """Apps take internal RAM, and Link holds its registration and command
        results back while DMA-capable RAM is under its reserve: in apps
        builds that wait is logged, not silent."""
        source = read("main/noise_control.cpp")
        warn = block(source, "        // Apps builds run closest to the DMA reserve", "#endif")
        before = source[:source.index("        // Apps builds run closest to the DMA reserve")]
        self.assertTrue(before.endswith("#if CONFIG_MUSE_WASM_APPS\n"))
        for part in ["control_tx.json || uxQueueMessagesWaiting(s_result_q)", "!noise_tx_has_dma_headroom(&dma_free)",
                     "!noise_tx_has_contiguous_dma_headroom()", "ESP_LOGW(", '"link.register"', "5000000"]:
            self.assertIn(part, warn)

    def test_a_frame_rate_set_to_zero_in_on_frame_divides_nothing(self):
        turn = function(read("components/muse_apps/muse_apps.c"), "static void turn(")
        self.assertNotIn("/ s_app.frame_rate", turn)
        self.assertIn("int rate = s_app.frame_rate;", turn)
        self.assertIn("if (rate > 0) {", turn)

    def test_lvgl_layouts_and_ranges_stay_in_bounds(self):
        """LVGL's grid indexes its tracks with each child's cell unchecked,
        drops every scale's needle hook when one scale goes, and divides by a
        scale's or chart's range: the wrapper keeps them in bounds."""
        ui = read("components/muse_apps/muse_apps_ui.c")
        fits = function(ui, "static bool track_fits(")
        self.assertIn("(uint32_t)span <= tracks - (uint32_t)pos", fits)
        cell = function(ui, "int32_t gadget_ui_grid_cell(")
        self.assertIn("grid_tracks(p->grid, false)", cell)
        self.assertIn("!track_fits(col, col_span, cols) || !track_fits(row, row_span, rws)", cell)
        self.assertNotIn("col + col_span", cell)
        grid = function(ui, "int32_t gadget_ui_grid(")
        self.assertLess(grid.index("first_cell(child)"), grid.index("lv_obj_set_grid_dsc_array("))
        self.assertIn("first_cell(s->obj)", function(ui, "int32_t gadget_ui_set_parent("))
        self.assertIn("m == LV_LAYOUT_GRID && !s->grid", function(ui, "static int32_t obj_set("))
        needle = function(ui, "int32_t gadget_ui_scale_needle(")
        self.assertIn("l->needle_of && l->needle_of != h && find(l->needle_of)", needle)
        self.assertLess(needle.index("range_ok("), needle.index("lv_scale_set_line_needle_value("))
        self.assertIn("return lo < hi && (int64_t)hi - lo <= INT32_MAX;", function(ui, "static bool range_ok("))
        widget = function(ui, "static int32_t widget_set(")
        self.assertIn("!range_ok(lo, hi) && has_needle(s)", widget)
        self.assertIn("if (range_ok(c->min, c->max)) {", widget)

    def test_store_requests_from_before_an_unpairing_are_dropped(self):
        store = read("components/muse_apps/muse_apps_store.c")
        self.assertIn("r->epoch = atomic_load(&s_epoch);", function(store, "static bool post("))
        task = function(store, "static void store_task(")
        self.assertIn("r.epoch != atomic_load(&s_epoch)", task)
        self.assertLess(task.index("r.epoch != atomic_load(&s_epoch)"), task.index("case ST_INSTALL"))
        forget = function(store, "void muse_apps_store_forget_all(")
        self.assertLess(forget.index("atomic_fetch_add(&s_epoch, 1);"), forget.index("xQueueSend("))

    def test_a_release_the_app_never_saw_pressed_goes_to_the_firmware(self):
        """Holding push-to-talk (or recording) while an app opens: the release
        must still reach the firmware, or the mic stays open."""
        view = read("components/muse/muse_app_view.c")
        buttons = function(view, "unsigned muse_app_view_buttons(unsigned ev)")
        guard = "if (!(bits & held[i].press) && !(atomic_load(&s_app_down) & row)) {"
        self.assertIn(guard, buttons)
        self.assertLess(buttons.index(guard), buttons.index("ev &= ~bits;"))
        self.assertIn("atomic_fetch_or(&s_app_down, row);", buttons)
        self.assertIn("atomic_store(&s_app_down, 0);", function(view, "bool muse_app_view_open("))

    def test_console_install_line_is_parsed_by_its_blanks(self):
        """A word too long for its buffer, or words split by a tab, once made
        the name lookup step through a NULL pointer."""
        console = function(read("components/muse_apps/muse_apps.c"), "bool muse_apps_console(")
        install = console[console.index('if (!strncmp(line, "app.install=", 12))'):]
        install = install[:install.index("return true;")]
        self.assertNotIn("strchr(strchr(", install)
        self.assertIn("n1 >= sizeof(a) || n2 > 179 ? 0 :", install)
        self.assertIn('name = w2 + n2;', install)

    def test_a_gesture_routed_to_muse_stays_muse_s(self):
        """muse_talk_key() can switch the talk key between a press and its
        release: the gesture's release goes where its press went."""
        buttons = function(read("components/muse/muse_app_view.c"), "unsigned muse_app_view_buttons(unsigned ev)")
        routed = buttons[buttons.index("if (talk_to_muse && held[i].button == MUSE_APP_BUTTON_PRIMARY)"):]
        routed = routed[:routed.index("continue;   /* left for Muse's push-to-talk */")]
        self.assertIn("atomic_fetch_and(&s_app_down, ~(1u << i));", routed)
        self.assertIn("esp_timer_stop(s_exit_timer);", routed)
        self.assertIn("esp_timer_stop(s_long_timer);", routed)

    def test_an_unpairing_s_wipe_survives_a_restart(self):
        store = read("components/muse_apps/muse_apps_store.c")
        forget = function(store, "void muse_apps_store_forget_all(")
        self.assertLess(forget.index("forget_mark(true);"), forget.index("xQueueSend("))
        self.assertIn("forget_mark(false);", function(store, "static bool forget_if_asked("))
        task = function(store, "static void store_task(")
        self.assertLess(task.index("if (forget_marked()) {"), task.index("for (st_req_t r;;)"))
        self.assertIn("nvs_flash", read("components/muse_apps/CMakeLists.txt"))

    def test_vm_locks_are_made_once(self):
        setup = function(read("components/muse_apps/muse_apps_vm.c"), "static bool setup(")
        self.assertIn("taskENTER_CRITICAL(&s_setup_mux);", setup)
        self.assertIn("if (!s_one && one && lock) {", setup)
        self.assertIn("vSemaphoreDelete(one);", setup)

    def test_png_pixels_come_from_lvgl_s_draw_buffer(self):
        """LVGL's lodepng returns an lv_draw_buf_t, not bare RGBA: reading
        width*height*4 bytes from it read past the descriptor."""
        png = function(read("components/muse_apps/muse_apps_ui.c"), "static int32_t decode_png(")
        self.assertIn("lv_draw_buf_t *decoded = NULL;", png)
        self.assertIn("decoded->data + (size_t)y * stride", png)
        self.assertIn("(uint64_t)stride * h > decoded->data_size", png)
        self.assertIn("lv_draw_buf_destroy(decoded);", png)
        self.assertNotIn("lv_free(rgba)", png)

    def test_app_py_installs_with_an_id_word(self):
        app = read("tools/muse/app.py")
        self.assertIn('line = "app.install=" + " ".join([word((args.id or "-") + look), word(args.install)]', app)
        # files.put's words, too, are split at blanks: a blank in one is refused, not misread.
        self.assertIn('"files.put=" + word(args.file_put) + (" " + word(args.as_name, 96)', app)
        self.assertIn('if len(s.encode()) > limit:', app)

    def test_png_with_16_bit_channels_is_refused(self):
        """LVGL's lodepng unfilters into its 4-byte-a-pixel buffer before
        converting: 16-bit channels overran it."""
        png = function(read("components/muse_apps/muse_apps_ui.c"), "static int32_t decode_png(")
        self.assertIn("unsigned depth = st.info_png.color.bitdepth;", png)
        self.assertIn("depth > 8", png)
        self.assertLess(png.index("depth > 8"), png.index("lodepng_decode32("))

    def test_a_textarea_is_charged_for_the_text_it_keeps(self):
        text = function(read("components/muse_apps/muse_apps_ui.c"), "static int32_t text_set(")
        after = text[text.index("lv_textarea_set_text(o, text);"):]
        self.assertIn("charge_delta(s, len, kept ? strlen(kept) : 0);", after[:400])

    def test_an_unpairing_wipe_that_left_a_file_is_tried_again(self):
        forget = function(read("components/muse_apps/muse_apps_store.c"), "static bool forget_if_asked(")
        self.assertIn("(s_files && !muse_apps_flash_wipe(FILES_ROOT)) || unmounted(LABEL, s_persistent) ||", forget)
        self.assertIn("unmounted(FILES_LABEL, s_files)", forget)
        self.assertLess(forget.index("!muse_apps_flash_wipe(FILES_ROOT)"), forget.index("forget_mark(false);"))
        self.assertIn("bool muse_apps_flash_wipe(const char *root);", read("components/muse_apps/muse_apps_flash.h"))

    def test_reopening_the_view_drops_held_buttons_and_their_timers(self):
        opened = function(read("components/muse/muse_app_view.c"), "bool muse_app_view_open(")
        reset = opened[opened.index("atomic_store(&s_app_down, 0);"):opened.index("atomic_store(&s_open, true);")]
        self.assertIn("esp_timer_stop(s_long_timer);", reset)
        self.assertIn("esp_timer_stop(s_exit_timer);", reset)
        self.assertIn("atomic_store(&s_long_button, 0);", reset)

    def test_a_queue_without_its_task_is_dropped(self):
        """Requests to a queue nobody serves would wait for ever."""
        start = function(read("components/muse_apps/muse_apps_store.c"), "void muse_apps_store_start(")
        self.assertIn("vQueueDeleteWithCaps(s_q);\n        s_q = NULL;", start)
        self.assertIn("if (s_rq) vQueueDeleteWithCaps(s_rq);\n        s_rq = NULL;", start)
        apps = function(read("components/muse_apps/muse_apps.c"), "void muse_apps_start(")
        self.assertIn("vQueueDeleteWithCaps(s_reqs);", apps[apps.index('"couldn\'t start the app task"'):])

    def test_the_store_waits_for_nvs_and_its_wipe_before_files_are_read(self):
        """The store starts before the firmware initializes NVS: an unpairing's
        mark mustn't be missed, and no file is read until it's wiped."""
        store = read("components/muse_apps/muse_apps_store.c")
        marked = function(store, "static bool forget_marked(")
        self.assertIn("== ESP_ERR_NVS_NOT_INITIALIZED) {", marked)
        task = function(store, "static void store_task(")
        self.assertLess(task.index("forget_marked()"), task.index("atomic_store(&s_ready, true);"))
        reader = function(store, "static void reader_task(")
        self.assertIn("!atomic_load(&s_ready) || atomic_load(&s_unwiped)", reader)
        self.assertIn("atomic_store(&s_unwiped, true);", function(store, "void muse_apps_store_forget_all("))

    def test_a_toggled_voice_note_keeps_the_talk_key(self):
        """Its next press stops the recording: an app opening meanwhile mustn't take it."""
        task = function(read("components/muse/muse_input.c"), "static void input_task(")
        self.assertIn("unsigned keep = NOTE_TOGGLED() ? ev & (MUSE_BTN_TALK_PRESS | MUSE_BTN_TALK_RELEASE) : 0;", task)
        self.assertIn("ev = muse_app_view_buttons(ev & ~keep) | keep;", task)

    def test_the_catalog_is_saved_whole_or_not_at_all(self):
        catalog = read("components/muse_apps/muse_apps_catalog.c")
        save = function(catalog, "static bool save(")
        self.assertIn("bool built = root && apps &&", save)
        self.assertLess(save.index("if (!built) {"), save.index("write_all("))
        listing = function(catalog, "cJSON *muse_apps_catalog_json(")
        self.assertIn("if (!ok || !cJSON_AddItemToArray(apps, app)) {", listing)

    def test_a_view_exit_is_never_lost(self):
        """A call or video taking the screen stops the app even with the request queue full."""
        apps = read("components/muse_apps/muse_apps.c")
        exit_ = function(apps, "static void on_view_exit(int reason)\n{")
        self.assertIn("atomic_store(&s_view_stop, reason + 1);", exit_)
        task = function(apps, "static void *app_task(")
        self.assertIn("int pending = atomic_exchange(&s_view_stop, 0);", task)

    def test_a_shrinking_table_refunds_its_cells(self):
        ui = read("components/muse_apps/muse_apps_ui.c")
        self.assertIn("charge_delta(s, strlen(v) + 8, 0);", function(ui, "static void table_refund("))
        rows = ui[ui.index("case GADGET_P_ROW_COUNT:"):]
        self.assertLess(rows.index("table_refund(s, (uint32_t)v,"), rows.index("lv_table_set_row_count("))
        cols = ui[ui.index("case GADGET_P_COLUMN_COUNT:"):]
        self.assertLess(cols.index("table_refund(s, lv_table_get_row_count(o), (uint32_t)v);"), cols.index("lv_table_set_column_count("))

    def test_a_degenerate_crowded_line_chart_is_not_drawn(self):
        """LVGL allocates (w + 2 * extra) * 3 points for up to w + 1 columns."""
        ui = read("components/muse_apps/muse_apps_ui.c")
        guard = function(ui, "static void chart_guard(")
        self.assertIn("if (w < 1 || extra < 1) {", guard)
        self.assertIn("lv_event_stop_processing(e);", guard)
        self.assertIn("lv_obj_add_event_cb(o, chart_guard, LV_EVENT_DRAW_MAIN | LV_EVENT_PREPROCESS, NULL);", ui)

    def test_unpair_records_the_apps_wipe_before_its_worker_starts(self):
        """A worker that can't start reboots: the wipe must already be recorded."""
        app = read("main/app.c")
        unpair = app[app.index("if (action == WS_CONTROL_UNPAIR) {"):]
        unpair = unpair[:unpair.index('xTaskCreate(ws_control_task')]
        self.assertIn("muse_apps_forget_all();", unpair)
        self.assertLess(unpair.index("clear_setup_credentials("), unpair.index("muse_apps_forget_all();"))

    def test_a_wipe_root_that_cant_be_opened_isnt_wiped(self):
        sweep = function(read("components/muse_apps/muse_apps_flash.c"), "static bool sweep(")
        self.assertIn("if (!dir) return !all || (depth == 0 && errno == ENOENT);", sweep)

    def test_no_app_opens_during_a_call(self):
        """The call's hang-up buttons would go to the app."""
        run = function(read("components/muse_apps/muse_apps.c"), "static void run(")
        self.assertLess(run.index("if (muse_voice_in_call()) {"), run.index("muse_app_view_open(&cbs)"))

    def test_a_turn_takes_at_most_a_queue_of_input(self):
        apps = read("components/muse_apps/muse_apps.c")
        self.assertIn("for (int k = 0; k < INPUT_QUEUE_LEN && s_app.running && xQueueReceive(s_inputs, &in, 0); k++) {",
                      function(apps, "static void turn("))
        start = function(apps, "void muse_apps_start(")
        failed = start[start.index('"no memory for the app queues"'):]
        self.assertLess(failed.index("s_state = NULL;"), failed.index("return;"))

    def test_muse_caption_returns_the_whole_caption(self):
        self.assertIn("char caption[MUSE_CAPTION_MAX]", function(read("components/muse_apps/muse_apps_sys.c"), "int32_t gadget_muse_caption("))

    def test_a_refused_app_gives_the_screen_back(self):
        """With the view already covered (a replaced app, the launcher), a
        refusal must close it, or the face and the call's buttons stay hidden."""
        apps = read("components/muse_apps/muse_apps.c")
        run = function(apps, "static void run(")
        self.assertIn('run_failed(r, covered, "busy", "a call is on");', run)
        self.assertIn('run_failed(r, covered, "no_display", "the screen isn\'t available");', run)
        launcher = function(apps, "static void show_launcher(")
        self.assertLess(launcher.index("if (muse_voice_in_call()) {"), launcher.index("muse_app_view_open(&cbs)"))

    def test_a_turn_delivers_at_most_max_ops_completions(self):
        turn = function(read("components/muse_apps/muse_apps.c"), "static void turn(")
        self.assertIn("for (int k = 0; k < MAX_OPS && s_app.running && take_done(&op); k++) {", turn)

    def test_an_id_too_long_is_refused_not_cut(self):
        """Cut to 31 bytes, it could name another installed app."""
        store = read("components/muse_apps/muse_apps_store.c")
        for name in ["bool muse_apps_store_remove(", "bool muse_apps_store_open("]:
            self.assertIn("if (strlen(id) >= sizeof(r.id)) return false;", function(store, name), name)
        for name in ["bool muse_apps_store_install(", "bool muse_apps_store_install_bytes("]:
            self.assertIn("if (id && strlen(id) >= sizeof(r.id)) return false;", function(store, name), name)

    def test_no_app_object_goes_inside_a_roller(self):
        """LVGL takes a roller's first child as its label: an app object
        moved ahead of it was read as a label (type confusion)."""
        ui = read("components/muse_apps/muse_apps_ui.c")
        self.assertIn("return p->type != GADGET_W_ROLLER;", function(ui, "static bool takes_children("))
        for name in ["int32_t gadget_ui_create(", "int32_t gadget_ui_set_parent("]:
            body = function(ui, name)
            self.assertLess(body.index("if (!takes_children(p)) {"), body.index("lv_obj_") if "lv_obj_" in body else len(body), name)

    def test_a_manifest_name_too_long_is_not_cut_into_an_id(self):
        info = function(read("components/muse_apps/muse_apps.c"), "bool muse_apps_manifest_info(")
        self.assertIn('if (!copy_string(j, "name", name, name_cap) && name_cap) {', info)

    def test_a_put_path_too_long_is_refused_not_cut(self):
        console = function(read("components/muse_apps/muse_apps.c"), "bool muse_apps_console(")
        put = console[console.index('if (!strncmp(line, "app.put=", 8)) {'):]
        self.assertLess(put.index("strlen(line + 8) >= sizeof(r.path)"), put.index("snprintf(r.path"))

    def test_a_vm_switch_stops_the_running_app(self):
        """An app run for one VM doesn't carry on with the next."""
        app = read("main/app.c")
        for name in ["static void set_vm_with_gate_held(", "static void reset_default_vm_with_gate_held("]:
            self.assertIn("muse_apps_vm_changed();", function(app, name)[:300], name)
        changed = function(read("components/muse_apps/muse_apps.c"), "void muse_apps_vm_changed(")
        self.assertLess(changed.index("muse_apps_vm_close_all();"), changed.index("on_view_exit(GADGET_STOP_USER);"))

    def test_the_view_rechecks_for_a_call_once_open(self):
        """A call checks for an open view after it starts; the view checks for
        a call after it opens: one of them sees the other."""
        opened = function(read("components/muse/muse_app_view.c"), "bool muse_app_view_open(")
        self.assertLess(opened.index("atomic_store(&s_open, true);"), opened.index("if (muse_voice_in_call()) {"))

    def test_apps_only_where_their_exit_is_in_reach(self):
        kconfig = read("components/muse_apps/Kconfig")
        for board in ["MUSE_BOARD_OSTB_ECHOEAR_2ST", "MUSE_BOARD_WAVESHARE_S3_LCD7", "MUSE_BOARD_VIEWE_SMARTRING_PLUS"]:
            self.assertIn("!" + board, kconfig)
        # Boards that turn a held touch on the face into push-to-talk don't, under an app.
        face = function(read("components/muse/muse_ui.c"), "bool muse_ui_face_active(")
        self.assertLess(face.index("if (muse_app_view_tick()) {"), face.index("return s_face_active;"))

    def test_apps_are_off_until_the_person_turns_them_on(self):
        """Run apps in Settings, saved, off by default: off, Muse isn't offered
        the app commands and they're refused; changing it restarts the gadget."""
        settings = read("components/muse/muse_settings.c")
        self.assertIn('nvs_get_u8(s_nvs, "apps_on", &b)', settings)
        self.assertIn("static bool s_apps_on;", settings)
        self.assertIn('save_u8("apps_on", on);', function(settings, "void muse_settings_set_apps_on("))
        self.assertIn("esp_restart();", function(settings, "void muse_settings_set_apps_on("))
        apps = read("components/muse_apps/muse_apps.c")
        self.assertIn("if (!muse_settings_apps_on()) {", function(apps, "void muse_apps_advertise(")[:120])
        command = function(apps, "cJSON *muse_apps_command(")
        self.assertLess(command.index('muse_apps_error("apps_off"'), command.index("app.status"))
        self.assertIn('apps_off', function(apps, "bool muse_apps_console("))
        self.assertIn('cJSON_AddBoolToObject(wasm, "enabled", muse_settings_apps_on())',
                      read("components/muse_apps/muse_apps_capabilities.c"))

    def test_no_app_opens_over_the_video_view(self):
        run = function(read("components/muse_apps/muse_apps.c"), "static void run(")
        self.assertLess(run.index("if (muse_video_is_open()) {"), run.index("muse_app_view_open(&cbs)"))

    def test_gadget_files_read_and_replace_safely(self):
        flash = read("components/muse_apps/muse_apps_flash.c")
        reading = function(flash, "const char *muse_apps_flash_read(")
        self.assertLess(reading.index("fopen(path, \"rb\")"), reading.index("fstat(fileno(f), &st)"))
        self.assertNotIn("stat(path, &st)", reading)
        self.assertIn("errno == EEXIST && remove(dest) == 0", function(flash, "const char *muse_apps_flash_commit("))

    def test_app_py_runs_a_vm_folder_by_path(self):
        self.assertIn('vm = args.vm if "/" in args.vm or args.vm.endswith(".wasm") else args.vm + "/"',
                      read("tools/muse/app.py"))

    def test_the_launcher_doesnt_open_over_the_video_view(self):
        launcher = function(read("components/muse_apps/muse_apps.c"), "static void show_launcher(")
        self.assertLess(launcher.index("if (muse_video_is_open()) {"), launcher.index("muse_app_view_open(&cbs)"))

    def test_a_run_path_too_long_is_refused_not_cut(self):
        console = function(read("components/muse_apps/muse_apps.c"), "bool muse_apps_console(")
        run = console[console.index('if (!strncmp(line, "app.run=", 8)) {'):]
        self.assertLess(run.index("strlen(line + 8) >= sizeof(r.path) - 12"), run.index("snprintf(r.path"))

    def test_an_arc_range_is_never_empty(self):
        """LVGL divides by an arc's range when it's dragged."""
        widget = function(read("components/muse_apps/muse_apps_ui.c"), "static int32_t widget_set(")
        arc = widget[widget.index("if (t == GADGET_W_ARC) {"):]
        self.assertLess(arc.index("lv_arc_get_max_value(o) : lv_arc_get_min_value(o))"), arc.index("lv_arc_set_min_value("))

    def test_a_vm_app_downloads_behind_the_face(self):
        """Stop and exit wait behind a download on the app task: the face, not
        a covered screen, shows meanwhile, and the wait for another transfer
        counts against the download's timeout."""
        run = function(read("components/muse_apps/muse_apps.c"), "static void run(")
        vm = run[run.index("if (r->kind == REQ_RUN_VM) {"):]
        self.assertLess(vm.index("muse_app_view_close();"), vm.index("muse_apps_vm_file_read("))
        read_vm = function(read("components/muse_apps/muse_apps_vm.c"), "bool muse_apps_vm_file_read(")
        self.assertIn("xSemaphoreTake(s_one, pdMS_TO_TICKS(timeout_ms)) != pdTRUE", read_vm)

    def test_a_failed_state_save_gives_its_quota_back(self):
        files = read("components/muse_apps/muse_apps_files.c")
        self.assertIn("sizes_undo(j->name, j->prev);", function(files, "static void save_run("))
        self.assertIn("sizes_undo(j->name, j->prev);", function(files, "static void save_drop("))
        save = function(files, "int32_t gadget_state_save(")
        self.assertIn("sizes_take(name, data_len, &j->prev);", save)
        self.assertIn("sizes_undo(name, j->prev);", save)

    def test_one_button_doing_both_is_one_button(self):
        present = function(read("components/muse/muse_app_view.c"), "unsigned muse_app_view_buttons_present(")
        self.assertIn("(aux && !(talk && !strcmp(aux, talk)))", present)

    def test_the_chat_example_can_be_closed(self):
        chat = read("components/muse_apps/sdk/examples/chat.c")
        self.assertIn("a == close_button && b == UI_CLICKED", chat)
        self.assertIn("a == BUTTON_SECONDARY && b == BTN_LONG", chat)
        self.assertGreaterEqual(chat.count("sys_exit();"), 2)

    def test_files_put_words_too_long_are_refused(self):
        console = function(read("components/muse_apps/muse_apps.c"), "bool muse_apps_console(")
        put = console[console.index('if (!strncmp(line, "files.put=", 10)) {'):]
        self.assertLess(put.index("if (n1 > 179 || n2 > 96) {"), put.index("sscanf(line + 10"))

    def test_callbacks_are_checked_before_running(self):
        """A callback declared with results (or other parameters) by mistake:
        WAMR writes results over the arguments it's given."""
        apps = read("components/muse_apps/muse_apps.c")
        check = function(apps, "static bool callback_is(")
        self.assertIn("wasm_func_get_result_count(fn, s_app.inst) || wasm_func_get_param_count(fn, s_app.inst) != n", check)
        self.assertIn("types[i] != WASM_I32", check)
        run = function(apps, "static void run(")
        for call in ["callback_is(on_start, 0)", "callback_is(init, 0)", "callback_is(s_app.on_frame, 1)",
                     "callback_is(s_app.on_event, 3)", "callback_is(s_app.on_stop, 1)"]:
            self.assertLess(run.index(call), run.index("muse_app_view_open(&cbs)"), call)

    def test_nothing_runs_before_the_instruction_limit(self):
        """A start function, __post_instantiate or __wasm_call_ctors would run
        while instantiating, unmetered: such an app is refused."""
        apps = read("components/muse_apps/muse_apps.c")
        itself = function(apps, "static const char *runs_itself(")
        self.assertIn("if (id == 8) {", itself)
        self.assertIn('"__post_instantiate"', itself)
        self.assertIn('"__wasm_call_ctors"', itself)
        run = function(apps, "static void run(")
        self.assertLess(run.index("runs_itself(bytes, len)"), run.index("free(bytes);\n    if (!s_app.module)"))
        self.assertLess(run.index("if (itself) {"), run.index("wasm_runtime_instantiate_ex("))

    def test_ui_geometry_and_shadows_are_bounded(self):
        """Common-sense limits: pixels within 4096, shadows and radii within
        64 px as cv_rect_style's (RADIUS_CIRCLE still rounds)."""
        ui = read("components/muse_apps/muse_apps_ui.c")
        self.assertIn("*out = clamp(v, size ? 0 : -PX_MAX, PX_MAX);", function(ui, "static bool coord_in("))
        value = function(ui, "static int32_t style_value(")
        self.assertIn("case K_SHADOW: out->num = clamp(v, 0, SHADOW_MAX);", value)
        self.assertIn("case K_OFFSET: out->num = clamp(v, -SHADOW_MAX, SHADOW_MAX);", value)
        self.assertIn("v == GADGET_RADIUS_CIRCLE ? v : clamp(v, 0, SHADOW_MAX)", value)
        for prop, kind in [("SHADOW_WIDTH", "K_SHADOW"), ("SHADOW_SPREAD", "K_SHADOW"), ("SHADOW_OFFSET_X", "K_OFFSET"),
                           ("SHADOW_OFFSET_Y", "K_OFFSET"), ("RADIUS", "K_RADIUS")]:
            self.assertIn("[GADGET_P_%s - 100] = {LV_STYLE_%s, %s}," % (prop, prop, kind), ui)
        self.assertIn("dsc.shadow_spread = clamp(dsc.shadow_spread, 0, 64);", read("components/muse_apps/muse_apps_canvas.c"))

    def test_a_refused_modules_reason_is_written_before_it_unloads(self):
        run = function(read("components/muse_apps/muse_apps.c"), "static void run(")
        refusal = run[run.index("if (itself) {"):]
        self.assertLess(refusal.index("snprintf(err"), refusal.index("unload();"))

    def test_buggy_values_dont_divide_by_zero(self):
        ui = read("components/muse_apps/muse_apps_ui.c")
        self.assertIn("[GADGET_P_LINE_DASH_GAP - 100] = {LV_STYLE_LINE_DASH_GAP, K_SHADOW},", ui)
        self.assertIn("[GADGET_P_LINE_DASH_WIDTH - 100] = {LV_STYLE_LINE_DASH_WIDTH, K_SHADOW},", ui)
        self.assertIn("[GADGET_P_TEXT_LINE_SPACE - 100] = {LV_STYLE_TEXT_LINE_SPACE, K_SPACE},", ui)
        self.assertIn("case K_SPACE: out->num = v < 0 ? 0 : v;", function(ui, "static int32_t style_value("))
        roller = function(ui, "static int32_t roller_set(")
        self.assertLess(roller.index("lv_obj_get_style_text_letter_space(s->obj, LV_PART_MAIN) <= 0"),
                        roller.index("lv_roller_set_options("))

    def test_storage_out_of_memory_paths(self):
        flash = function(read("components/muse_apps/muse_apps_flash.c"), "static void walk(")
        self.assertIn('if (!cJSON_AddStringToObject(entry, "name", name)', flash)
        start = function(read("components/muse_apps/muse_apps_store.c"), "void muse_apps_store_start(")
        self.assertIn("if (s_q) vQueueDeleteWithCaps(s_q);", start)

    def test_style_attachments_per_object_are_capped(self):
        """LVGL asserts (and so aborts) past 63 styles on one object: shared
        styles and local selectors count together, the theme's don't."""
        ui = read("components/muse_apps/muse_apps_ui.c")
        room = function(ui, "static bool style_room(")
        self.assertIn("if (local && st->is_local && st->selector == sel) {", room)
        self.assertIn("n += !st->is_theme && !st->is_trans;", room)
        self.assertIn("return n < STYLES_PER_OBJECT;", room)
        add = function(ui, "int32_t gadget_ui_style_add(")
        self.assertLess(add.index("if (!style_room(s->obj, false, sel)) {"), add.index("lv_obj_add_style("))
        local = function(ui, "int32_t gadget_ui_style(")
        self.assertLess(local.index("if (!style_room(s->obj, true, sel)) {"), local.index("style_set("))

    def test_the_unpair_mark_waits_for_the_apps_to_go(self):
        catalog = read("components/muse_apps/muse_apps_catalog.c")
        self.assertIn("return delete_unnamed() && saved;", function(catalog, "bool muse_apps_catalog_wipe("))
        self.assertIn("if (remove(path) != 0) {", function(catalog, "static bool delete_unnamed("))
        forget = function(read("components/muse_apps/muse_apps_store.c"), "static bool forget_if_asked(")
        self.assertIn("bool apps_gone = muse_apps_catalog_wipe();", forget)
        self.assertIn("if (!apps_gone ||", forget)

    def test_app_py_checks_lengths_and_acknowledged_bytes(self):
        app = read("tools/muse/app.py")
        self.assertIn('"files.remove=" + word(args.file_remove, 96)', app)
        console = function(read("components/muse_apps/muse_apps.c"), "bool muse_apps_console(")
        remove = console[console.index('} else if (!strncmp(line, "files.remove=", 13)) {'):]
        self.assertLess(remove.index("if (strlen(line + 13) >= sizeof(name)) {"), remove.index("snprintf(name"))
        self.assertIn('got = f.get("bytes")', app)
        self.assertIn("if got != sent:", app)

    def test_stopping_an_app_frees_what_it_held(self):
        stop = function(read("components/muse_apps/muse_apps.c"), "static void stop(")
        for call in ["ops_clear();", "muse_apps_ui_stop();", "muse_apps_canvas_stop();", "muse_apps_chat_stop();",
                     "muse_apps_files_stop();", "muse_apps_dev_stop();", "muse_apps_sys_stop();",
                     "muse_apps_vm_close_all();", "muse_app_view_close();", "unload();"]:
            self.assertIn(call, stop)

    def test_talk_key_can_go_back_to_muse(self):
        view = read("components/muse/muse_app_view.c")
        self.assertIn("talk_to_muse && held[i].button == MUSE_APP_BUTTON_PRIMARY",
                      function(view, "unsigned muse_app_view_buttons("))
        # Each app starts without it.
        self.assertIn("atomic_store(&s_talk_to_muse, false);", function(view, "bool muse_app_view_open("))
        self.assertIn("muse_app_view_talk_to_muse(on != 0);",
                      function(read("components/muse_apps/muse_apps_sys.c"), "void gadget_muse_talk_key("))

    def test_app_downloads_are_ranged_and_bounded(self):
        vm = read("components/muse_apps/muse_apps_vm.c")
        self.assertIn('#define PATH_PREFIX "/fs/raw/"', vm)
        self.assertIn("muse_link_req_max_body()", vm)
        self.assertIn('"Range"', vm)
        self.assertIn("max_bytes + 1 - have", vm)   # one byte past the limit tells a file that's too large
        self.assertIn(".req_max_body = noise_ctrl_req_max_body", read("main/muse_glue.c"))
        self.assertIn("#define APP_MAX_BYTES (256 * 1024)", read("components/muse_apps/muse_apps.c"))
        self.assertEqual(capabilities()["limits"]["module_bytes"], 256 * 1024)

    def test_muse_guide_is_current_and_fits_one_message(self):
        check = subprocess.run([sys.executable, str(APPS / "muse_brief.py"), "--check"],
                               capture_output=True, text=True)
        self.assertEqual(check.returncode, 0, check.stderr)
        # chat.py --file sends it as one typed message, which the board collects up to CHAT_MAX.
        chat_max = re.search(r"#define CHAT_MAX \((\d+) \* 1024\)", read("components/muse/muse_input.c"))
        brief = read("components/muse_apps/sdk/muse_brief.py")
        self.assertIn("MAX_BYTES = %s * 1024 - 1" % chat_max.group(1), brief)
        guide = read("components/muse_apps/sdk/MUSE.md")
        self.assertLess(len(guide.encode()), int(chat_max.group(1)) * 1024)
        for word in COMMANDS + ["workspace/gadgets/apps/", "GADGET_MANIFEST", "api.capabilities.json", "ziglang"]:
            self.assertIn(word, guide)
        # The person's own style section stays.
        # The person's own style section stays.
        self.assertIn("### Style\nIf I don't ask for another style, use this one:", guide)
        self.assertIn("#define STYLE_GOLD 0xC7A252", guide)
        self.assertNotRegex(guide, r"(?i)cool ?tech|\bCOOL_")   # the style has no name
        # The design: laid out from the screen's size and shape, with every glyph and colour the
        # launcher knows, for the manifest's icon and color.
        design = guide[guide.index("## Design\n"):guide.index("### Style\n")]
        self.assertIn("`ui_span(y, margin)`", design)
        # Laid out from the screen device.guide describes, not one board's.
        self.assertIn("INFO_SAFE_INSET", design)
        self.assertNotRegex(guide[guide.index("## Design\n"):guide.index("## gadget.h\n")], r"\b(466|233)\b")
        self.assertIn("ui_fit_text(", design)
        icons = read("components/muse_apps/muse_apps_icons.c")
        glyphs = re.findall(r"GLYPH\((\w+)\)", icons[icons.index("muse_apps_glyphs[] = {"):])
        colors = re.findall(r'\{"(\w+)", 0x[0-9A-F]{6},', icons)
        self.assertEqual((len(glyphs), len(colors)), (35, 12))
        for name in glyphs + colors:
            self.assertRegex(design, r"\b%s\b" % name)


    def test_ci_builds_apps(self):
        """Apps are off by default, so no other build compiles them: CI builds
        one board with the overlay."""
        workflow = (ROOT.parent / ".github/workflows/esp32.yml").read_text()
        self.assertIn('- board: muse-m5stack-stopwatch-apps\n            target: esp32s3\n'
                      '            overlays: ";devices/sdkconfig.muse;devices/sdkconfig.muse-m5stack-stopwatch;'
                      'devices/sdkconfig.muse-apps"', workflow)

    def test_not_on_e_paper_boards(self):
        # FreeInk boards build no LVGL, which the app view and the API's widgets need.
        kconfig = read("components/muse_apps/Kconfig")
        self.assertRegex(kconfig, r"config MUSE_WASM_APPS\s+bool[^\n]*\n(\s*#[^\n]*\n)*\s*depends on [^\n]*!MUSE_UI_FREEINK")

    def test_builds_without_apps_carry_none_of_it(self):
        """Gadgets built without apps pay nothing: the registry, its drivers and the
        apps' hooks are all behind CONFIG_CAPABILITIES_REGISTRY or CONFIG_MUSE_APP_VIEW, which
        only CONFIG_MUSE_WASM_APPS selects."""
        kconfig = (ROOT / "components/muse_apps/Kconfig").read_text()
        self.assertIn("select CAPABILITIES_REGISTRY", kconfig)
        self.assertIn("select MUSE_APP_VIEW", kconfig)
        dev_cmake = (ROOT / "components/capabilities_registry/CMakeLists.txt").read_text()
        self.assertRegex(dev_cmake, r'if\(CONFIG_CAPABILITIES_REGISTRY\)\s*list\(APPEND srcs "capabilities_registry.c"\)')
        cmake = (ROOT / "components/muse/CMakeLists.txt").read_text()
        for src in ("board_capabilities.c",):
            i = cmake.index(src)
            guard = cmake.rfind("if(", 0, i)
            self.assertIn("CONFIG_CAPABILITIES_REGISTRY", cmake[guard:i], src + " is built without CONFIG_CAPABILITIES_REGISTRY")
        # The battery registers from main/muse_glue.c: muse_app.c, whose
        # ESP_ERROR_CHECK lines carry __LINE__, stays as it was without apps.
        self.assertNotIn("board_capabilities", (ROOT / "components/muse/muse_app.c").read_text())
        glue = function((ROOT / "main/muse_glue.c").read_text(), "static void boot_task(")
        self.assertRegex(glue, r"#if CONFIG_CAPABILITIES_REGISTRY\s*(//[^\n]*\s*)*board_capabilities_init\(\);")
        # Board files register their devices only with the registry (test_apps_dev.py checks each).
        ui = (ROOT / "components/muse/muse_ui.c").read_text()
        self.assertRegex(ui, r"#if CONFIG_MUSE_APP_VIEW\s*static atomic_int s_app_brightness")
        # Declared always, cJSON would move in the link of builds without apps.
        manifest = (ROOT / "components/muse_apps/idf_component.yml").read_text()
        self.assertRegex(manifest, r'espressif/cjson:(?:\n    .*)*\n      - if: "\$CONFIG\{MUSE_APPS_WAMR\} == \\"y\\""')
        link = (ROOT / "components/muse/muse_link.h").read_text()
        self.assertRegex(link, r'#if CONFIG_MUSE_WASM_APPS\s*size_t \(\*req_max_body\)')
        self.assertIn('__has_include("sdkconfig.h")', link)

if __name__ == "__main__":
    unittest.main()
