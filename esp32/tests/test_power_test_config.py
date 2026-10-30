# Copyright (c) Meta Platforms, Inc. and affiliates.
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy at http://www.apache.org/licenses/LICENSE-2.0
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""Keep the power-test overlay from rotting without an IDF build.

Kconfig silently drops a misspelled symbol, or one whose dependencies are
unmet, so a stale devices/sdkconfig.*-power-test still "builds" a normal app.
These checks read text only: project Kconfig always, the ESP-IDF tree when
IDF_PATH is set (CI exports it).
"""
import os
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
OVERLAYS = sorted((ROOT / "devices").glob("sdkconfig.muse-*-power-test"))
DEFINITION = re.compile(r"^\s*(?:menu)?config\s+([A-Za-z0-9_]+)\s*$", re.M)


def kconfig_symbols(files):
    symbols = set()
    for path in files:
        symbols.update(DEFINITION.findall(path.read_text(encoding="utf-8", errors="replace")))
    return symbols


def kconfig_files(base, depth):
    return [path for level in range(1, depth + 1) for path in base.glob("*/" * level + "Kconfig*")]


def overlay_settings(path):
    settings = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        if line.startswith("CONFIG_"):
            name, _, value = line[len("CONFIG_"):].partition("=")
            settings[name] = value
    return settings


def power_test_entry():
    """The MUSE_POWER_TEST entry only: from its config line to the next entry."""
    text = (ROOT / "components/muse/Kconfig").read_text(encoding="utf-8")
    start = re.search(r"^\s*config MUSE_POWER_TEST\s*$", text, re.M)
    if start is None:
        raise AssertionError("config MUSE_POWER_TEST not found")
    end = re.compile(r"^\s*(?:(?:menu)?config|choice|endchoice|menu|endmenu|if|endif|comment|source)\b", re.M)
    stop = end.search(text, start.end())
    return text[start.start():stop.start() if stop else len(text)]


class PowerTestConfigTests(unittest.TestCase):
    def test_power_test_overlays_exist(self):
        self.assertIn(ROOT / "devices/sdkconfig.muse-watcher-power-test", OVERLAYS)

    def test_overlay_symbols_exist_in_project_or_idf_kconfig(self):
        project = kconfig_symbols(kconfig_files(ROOT / "components", 2) + [ROOT / "main/Kconfig.projbuild"])
        self.assertIn("MUSE_POWER_TEST", project)
        idf_path = os.environ.get("IDF_PATH")
        idf = set()
        if idf_path and (Path(idf_path) / "components").is_dir():
            idf = kconfig_symbols(kconfig_files(Path(idf_path) / "components", 4))
            idf |= kconfig_symbols(kconfig_files(ROOT / "managed_components", 2))
        unresolved = []
        for overlay in OVERLAYS:
            for name in overlay_settings(overlay):
                # Project-owned prefixes must resolve here, with or without IDF.
                if name.startswith(("MUSE_", "HOMEHUB_")) and name not in project:
                    self.fail(f"{overlay.name}: CONFIG_{name} is not defined in project Kconfig")
                if name not in project and name not in idf:
                    unresolved.append(f"{overlay.name}: CONFIG_{name}")
        if idf:
            self.assertEqual(unresolved, [], "overlay names unknown Kconfig symbols")
        elif unresolved:
            self.skipTest("IDF_PATH unset; ESP-IDF symbols not checked: " + ", ".join(unresolved))

    def test_power_test_dependencies_are_set_by_overlay(self):
        entry = power_test_entry()
        depends = re.findall(r"^\s*depends on (.+)$", entry, re.M)
        self.assertEqual(len(depends), 1, entry)
        others = sorted(set(re.findall(r"\b[A-Z][A-Z0-9_]+\b", depends[0])) - set(re.findall(r"\bMUSE_BOARD_[A-Z0-9_]+\b", depends[0])))
        self.assertIn("PM_ENABLE", others)
        for overlay in OVERLAYS:
            settings = overlay_settings(overlay)
            self.assertEqual(settings.get("MUSE_POWER_TEST"), "y", overlay.name)
            for name in others:
                self.assertEqual(settings.get(name), "y", f"{overlay.name} must set CONFIG_{name}=y or Kconfig drops MUSE_POWER_TEST")

    def test_depends_on_board_list_matches_hook_implementations(self):
        depends = re.search(r"^\s*depends on (.+)$", power_test_entry(), re.M).group(1)
        listed = set(re.findall(r"\bMUSE_BOARD_[A-Z0-9_]+\b", depends))
        cmake = (ROOT / "components/muse/CMakeLists.txt").read_text(encoding="utf-8")
        branches = re.findall(r"^\s*(?:else)?if\((.+)\)\s*\n\s*list\(APPEND srcs ([^)]*)\)", cmake, re.M)
        implemented = set()
        for path in sorted((ROOT / "components/muse/boards").glob("board_*.c")):
            source = path.read_text(encoding="utf-8")
            # A definition, not a call or a declaration.
            if not re.search(r"^const char \*muse_ptest_board_name\(void\)\s*\{", source, re.M):
                continue
            self.assertRegex(source[:source.index("muse_ptest_board_name")],
                             r'#include "(?:boards/)?muse_power_test_board\.h"', path.name)
            boards = {symbol for condition, srcs in branches if f'"boards/{path.name}"' in srcs
                      for symbol in re.findall(r"\bCONFIG_(MUSE_BOARD_[A-Z0-9_]+)\b", condition)}
            self.assertTrue(boards, f"{path.name} implements the hooks but no MUSE_BOARD_* builds it")
            implemented |= boards
        self.assertTrue(implemented)
        self.assertEqual(listed, implemented)


if __name__ == "__main__":
    unittest.main()
