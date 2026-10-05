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

"""The hardware commands for the agent: muse_hw.c's events, capture and light,
and muse_hw_commands.c's formats and registration, on the host; and the
wiring that must stay in place in the firmware around them."""

from pathlib import Path
import os
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
JSON = Path(os.environ.get("CJSON_SOURCE_DIR", ROOT / "tests/link_fakes"))


def compile_and_run(cmds, binary):
    for cmd in cmds:
        compiled = subprocess.run(cmd, capture_output=True, text=True)
        if compiled.returncode:
            raise AssertionError(" ".join(map(str, cmd)) + "\n" + compiled.stdout + compiled.stderr)
    return subprocess.run([str(binary)], capture_output=True, text=True)


class MuseHwTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        cls.out = Path(cls.temp.name)
        cls.cc = shlex.split(os.environ.get("CC", "cc"))
        cls.flags = ["-std=gnu11", "-Wall", "-Wextra", "-Werror"]

    def test_events_capture_and_light(self):
        # A copy, so its #include "muse_board.h" finds the fake, not the real one.
        src = self.out / "hw"
        src.mkdir()
        for name in ("muse_hw.c", "muse_hw.h"):
            shutil.copy(ROOT / "components/muse" / name, src / name)
        binary = self.out / "muse_hw"
        result = compile_and_run([[
            *self.cc, *self.flags, "-I", str(ROOT / "tests/muse_hw_fakes"), "-I", str(src),
            str(src / "muse_hw.c"), str(ROOT / "tests/muse_hw_harness.c"), "-lm", "-o", str(binary),
        ]], binary)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_formats_and_registration(self):
        source = (ROOT / "main/muse_hw_commands.c").read_text()
        start = source.index("// ---- Host-tested")
        (self.out / "muse_hw_commands.inc").write_text(source[start:source.index("// ---- Host-tested end", start)])
        binary = self.out / "muse_hw_commands"
        flags = [*self.flags, "-I", str(JSON), "-I", str(self.out)]
        result = compile_and_run([
            [*self.cc, *flags, "-Wno-unused-parameter", "-c", str(JSON / "cJSON.c"), "-o", str(self.out / "cjson.o")],
            [*self.cc, *flags, str(ROOT / "tests/muse_hw_commands_harness.c"), str(self.out / "cjson.o"), "-lm",
             "-o", str(binary)],
        ], binary)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_camera_frames_and_commands(self):
        def section(path, first, last):
            source = (ROOT / path).read_text()
            start = source.index(first)
            return source[start:source.index(last, start)]
        (self.out / "camera_frames.inc").write_text(
            section("components/muse/muse_camera.c", "/* ---- Frames (host-tested)", "/* ---- Frames end"))
        (self.out / "camera_cmds.inc").write_text(
            section("main/muse_hw_camera.c", "// ---- Host-tested", "// ---- Host-tested end"))
        binary = self.out / "muse_camera"
        result = compile_and_run([[
            *self.cc, *self.flags, "-Wno-unused-function", "-I", str(self.out),
            str(ROOT / "tests/muse_camera_harness.c"), "-o", str(binary),
        ]], binary)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_ports_clock_and_paths(self):
        def section(path, first, last):
            source = (ROOT / path).read_text()
            start = source.index(first)
            return source[start:source.index(last, start)]
        (self.out / "io.inc").write_text(section("main/muse_hw_io.c", "// ---- Host-tested", "// ---- Host-tested end"))
        (self.out / "storage.inc").write_text(
            section("main/muse_hw_storage.c", "// ---- Host-tested", "// ---- Host-tested end"))
        binary = self.out / "muse_hw_io"
        result = compile_and_run([[
            *self.cc, *self.flags, "-Wno-unused-function", "-I", str(self.out),
            str(ROOT / "tests/muse_hw_io_harness.c"), "-o", str(binary),
        ]], binary)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_commands_are_registered_and_dispatched_with_the_feature(self):
        noise = (ROOT / "main/noise_control.cpp").read_text()
        start = noise.index("muse_hw_commands_register(commands);")
        self.assertIn("#if CONFIG_MUSE_HW_COMMANDS", noise[start - 60:start])
        self.assertLess(start, noise.index('cJSON_AddItemToObject(params, "commands_v2", commands);'))
        app = (ROOT / "main/app.c").read_text()
        start = app.index("static cJSON *on_ws_command(")
        dispatch = app[start:app.index("unsupported command", start)]
        block = dispatch[dispatch.index("#if CONFIG_MUSE_HW_COMMANDS"):]
        self.assertIn("muse_hw_command(command, params, request_id, session_generation)",
                      block[:block.index("#endif")])

    def test_capture_keeps_input_from_the_ui(self):
        board = (ROOT / "components/muse/boards/board_sensecap_watcher.c").read_text()
        self.assertIn("*count = s_tp_down && !muse_hw_captured();", board)
        self.assertIn("muse_hw_wheel_push(raw);", board)
        self.assertIn(".set_led = set_led,", board)
        muse_input = (ROOT / "components/muse/muse_input.c").read_text()
        loop = muse_input[muse_input.index("static void input_task("):]
        capture = loop[loop.index("if (muse_hw_captured()) {"):]
        self.assertIn("ev &= ~(MUSE_BTN_TALK_PRESS | MUSE_BTN_AUX_PRESS);", capture[:capture.index("}")])
        self.assertLess(loop.index("if (muse_hw_captured()) {"), loop.index("talk_button(ev);"))


if __name__ == "__main__":
    unittest.main()
