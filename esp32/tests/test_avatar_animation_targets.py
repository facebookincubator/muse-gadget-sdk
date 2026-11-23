# Copyright (c) Meta Platforms, Inc. and affiliates.
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at http://www.apache.org/licenses/LICENSE-2.0
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
"""Evaluate the production avatar feature dependencies with ESP-IDF's Kconfig parser."""
from pathlib import Path
import os
import re
import shlex
import shutil
import subprocess
import tempfile
import unittest

try:
    import kconfiglib
except ImportError:
    kconfiglib = None

ROOT = Path(__file__).resolve().parents[1]
TARGETS = ("ESP32S3", "ESP32P4", "ESP32S31", "ESP32C5", "ESP32C6", "ESP32")


@unittest.skipIf(kconfiglib is None, "ESP-IDF's esp-idf-kconfig package is required")
class AvatarAnimationTargets(unittest.TestCase):
    def configure(self, target, *, enabled=True, psram=True, daemon=True, freeink=False,
                  sync=True, animations=True):
        production = (ROOT / "components/muse/Kconfig").read_text()
        blocks = []
        for name in ("MUSE_AVATAR_SYNC", "MUSE_AVATAR_ANIMATIONS"):
            match = re.search(r"(?ms)^    config " + name + r"\n.*?(?=^    (?:config|choice)|\Z)", production)
            self.assertIsNotNone(match, name)
            blocks.append(match.group())
        # Use the real feature stanzas, with only their external dependencies stubbed.
        external = ("MUSE_ENABLED", "SPIRAM", "HOMEHUB_DAEMON_REQUESTS", "MUSE_UI_FREEINK")
        external += tuple("IDF_TARGET_" + name for name in TARGETS)
        text = 'mainmenu "Avatar animation target test"\n'
        text += "\n".join('config %s\n    bool "%s"\n' % (name, name) for name in external)
        text += "\n".join(blocks)
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "Kconfig"
            path.write_text(text)
            config = kconfiglib.Kconfig(str(path), warn=False)
            values = {"MUSE_ENABLED": enabled, "SPIRAM": psram,
                      "HOMEHUB_DAEMON_REQUESTS": daemon, "MUSE_UI_FREEINK": freeink,
                      "MUSE_AVATAR_SYNC": sync}
            values.update({"IDF_TARGET_" + name: name == target for name in TARGETS})
            for name, value in values.items():
                config.syms[name].set_value(2 if value else 0)
            if animations:
                config.syms["MUSE_AVATAR_ANIMATIONS"].set_value(2)
            return {name: config.syms[name].str_value
                    for name in ("MUSE_AVATAR_SYNC", "MUSE_AVATAR_ANIMATIONS")}

    def test_s3_and_p4_can_opt_in(self):
        for target in ("ESP32S3", "ESP32P4"):
            with self.subTest(target=target):
                self.assertEqual(self.configure(target)["MUSE_AVATAR_ANIMATIONS"], "y")

    def test_other_targets_remain_excluded(self):
        for target in TARGETS:
            if target in ("ESP32S3", "ESP32P4"):
                continue
            with self.subTest(target=target):
                self.assertEqual(self.configure(target)["MUSE_AVATAR_ANIMATIONS"], "n")

    def test_all_avatar_prerequisites_still_apply(self):
        for target in ("ESP32S3", "ESP32P4"):
            for override in ({"enabled": False}, {"psram": False}, {"daemon": False},
                             {"freeink": True}, {"sync": False}):
                with self.subTest(target=target, override=override):
                    self.assertEqual(self.configure(target, **override)["MUSE_AVATAR_ANIMATIONS"], "n")

    def test_animation_remains_default_off(self):
        for target in ("ESP32S3", "ESP32P4"):
            with self.subTest(target=target):
                values = self.configure(target, animations=False)
                self.assertEqual(values["MUSE_AVATAR_SYNC"], "y")
                self.assertEqual(values["MUSE_AVATAR_ANIMATIONS"], "n")


class TinyH264LinkerNamespace(unittest.TestCase):
    def test_local_decoder_mapping_does_not_collide_with_esp_h264(self):
        fragment = (ROOT / "components/tinyh264/linker.lf").read_text()
        names = re.findall(r"(?m)^\[mapping:([^\]]+)\]", fragment)
        self.assertEqual(names, ["muse_tinyh264"])
        self.assertRegex(fragment, r"(?m)^archive: libmuse_tinyh264\.a$")

    def test_component_archive_matches_its_linker_mapping(self):
        if not shutil.which("cmake"):
            self.skipTest("cmake required to evaluate the component output name")
        with tempfile.TemporaryDirectory() as tmp:
            stage = Path(tmp)
            # Evaluate the production CMake stanza. ESP-IDF registration is
            # stubbed; native IDF builds separately validate real linkage.
            cmake = '''cmake_minimum_required(VERSION 3.24)
project(muse_decoder_archive_test C)
function(idf_component_register)
    cmake_parse_arguments(ARG "" "" "SRCS;INCLUDE_DIRS;LDFRAGMENTS" ${ARGN})
    add_library(__idf_tinyh264 STATIC ${ARG_SRCS})
    set(COMPONENT_LIB __idf_tinyh264 PARENT_SCOPE)
endfunction()
set(CONFIG_MUSE_AVATAR_ANIMATIONS ON)
include("%s")
get_target_property(decoder_archive __idf_tinyh264 OUTPUT_NAME)
file(WRITE "${CMAKE_BINARY_DIR}/decoder_archive.txt" "${decoder_archive}")
''' % (ROOT / "components/tinyh264/CMakeLists.txt").as_posix()
            (stage / "CMakeLists.txt").write_text(cmake)
            command = ["cmake", "-S", str(stage), "-B", str(stage / "build")]
            compiler = shlex.split(os.environ.get("CC", ""))
            if compiler:
                command.append("-DCMAKE_C_COMPILER=" + compiler[0])
            subprocess.run(command, check=True, capture_output=True, timeout=60)
            output = (stage / "build/decoder_archive.txt").read_text()
            self.assertEqual(output, "muse_tinyh264")
            fragment = (ROOT / "components/tinyh264/linker.lf").read_text()
            self.assertIn("archive: lib" + output + ".a", fragment)


if __name__ == "__main__":
    unittest.main()
