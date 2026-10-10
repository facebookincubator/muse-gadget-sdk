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

"""Default/custom avatar selection and incremental simulator rebuilds."""

import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ESP32 = Path(__file__).resolve().parents[1]
SIM_CMAKE = ESP32 / "simulator/CMakeLists.txt"


@unittest.skipUnless(shutil.which("cmake") and shutil.which("cc"), "CMake and C compiler required")
class AvatarSelection(unittest.TestCase):
    """Compile the simulator's production source-selection block without SDL/LVGL."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="muse-avatar-selection-")
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        sim = self.root / "simulator"
        sim.mkdir()
        (sim / "src").mkdir()
        muse = self.root / "components/muse"
        muse.mkdir(parents=True)
        default = self.root / "avatar"
        default.mkdir()
        (default / "muse_pixel.c").write_text('const char *avatar(void) { return "default"; }\n')
        (sim / "src/main.c").write_text(
            '#include <stdio.h>\nconst char *avatar(void);\n'
            'int main(void) { puts(avatar()); return 0; }\n'
        )
        for name in ("sim_board.c", "sim_platform.c", "sim_services.c"):
            (sim / "src" / name).write_text("typedef int unused;\n")
        for name in ("muse_ui.c", "muse_state.c", "muse_text.c"):
            (muse / name).write_text("typedef int unused;\n")
        text = SIM_CMAKE.read_text()
        start = text.index('set(MUSE_COMPONENT_DIR ')
        end = text.index('target_include_directories(muse_simulator', start)
        (sim / "CMakeLists.txt").write_text(
            'cmake_minimum_required(VERSION 3.24)\nproject(avatar_selection C)\n'
            + text[start:end]
        )
        self.custom = muse / "avatar/muse_pixel.c"
        self.build = self.root / "build"

    def write_custom(self):
        self.custom.parent.mkdir(exist_ok=True)
        self.custom.write_text('const char *avatar(void) { return "custom"; }\n')

    def configure(self):
        subprocess.run(
            ["cmake", "-S", str(self.root / "simulator"), "-B", str(self.build)],
            check=True, capture_output=True, text=True, timeout=60,
        )

    def selected(self):
        subprocess.run(
            ["cmake", "--build", str(self.build)],
            check=True, capture_output=True, text=True, timeout=60,
        )
        return subprocess.check_output([str(self.build / "muse_simulator")], text=True).strip()

    def test_custom_at_configure(self):
        self.write_custom()
        self.configure()
        self.assertEqual(self.selected(), "custom")

    def test_default_without_custom(self):
        self.configure()
        self.assertEqual(self.selected(), "default")

    def test_add_and_remove_without_manual_reconfigure(self):
        self.configure()
        self.assertEqual(self.selected(), "default")
        self.write_custom()
        self.assertEqual(self.selected(), "custom")
        self.custom.unlink()
        self.assertEqual(self.selected(), "default")


if __name__ == "__main__":
    unittest.main()
