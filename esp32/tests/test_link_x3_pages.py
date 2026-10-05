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

"""The Xteink X3's text commands: advertised and dispatched under one option."""

from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]


def block(source: str, option: str, start: int = 0) -> str:
    """The first `#if option` block at or after `start`, up to its #endif."""
    begin = source.index(f"#if {option}\n", start)
    return source[begin:source.index("#endif", begin)]


class LinkX3PagesTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        noise = (ROOT / "main/noise_control.cpp").read_text()
        cls.register = noise[noise.index("build_register_json"):]
        app = (ROOT / "main/app.c").read_text()
        start = app.index("static cJSON *on_ws_command(")
        cls.dispatch = app[start:app.index("unsupported command", start)]

    def test_commands_are_advertised_and_dispatched_with_the_feature(self):
        for option, commands in (
            ("CONFIG_HOMEHUB_TEXT_PAGES", ("pages.set", "pages.clear")),
            ("CONFIG_HOMEHUB_DISPLAY_TEXT", ("display.show_text",)),
        ):
            advertised = block(self.register, option)
            dispatched = block(self.dispatch, option)
            for command in commands:
                self.assertIn(f'add_command(commands, "{command}"', advertised)
                self.assertIn(f'"{command}"', dispatched)

    def test_advertised_limits_match_the_page_store(self):
        header = (ROOT / "main/x3_pages.h").read_text()
        limits = {name: int(value) for name, value in
                  re.findall(r"#define X3_PAGE_(\w+)_MAX\s+(\d+)", header)}
        store = (ROOT / "main/x3_pages.c").read_text()
        sizes = {name.upper(): int(size) for name, size in
                 re.findall(r"char (name|title|text)\[(\d+)\];", store)}
        for field, limit in limits.items():
            # Room for the terminator.
            self.assertGreater(sizes[field], limit, field)
        advertised = block(self.register, "CONFIG_HOMEHUB_TEXT_PAGES")
        self.assertIn(f"up to {limits['NAME']} characters", advertised)
        self.assertIn(f"up to {limits['TEXT']} ", advertised)
        self.assertIn(f"up to {limits['TITLE']} characters", advertised)

    def test_only_the_x3_backend_builds_the_pages(self):
        cmake = (ROOT / "main/CMakeLists.txt").read_text()
        start = cmake.index("if(CONFIG_HOMEHUB_LED_BACKEND_XTEINK_X3)")
        self.assertIn('"x3_pages.c"', cmake[start:cmake.index("elseif(", start)])
        kconfig = (ROOT / "main/Kconfig.projbuild").read_text()
        option = kconfig[kconfig.index("config HOMEHUB_TEXT_PAGES"):]
        self.assertIn("default y if HOMEHUB_LED_BACKEND_XTEINK_X3", option[:option.index("help")])


if __name__ == "__main__":
    unittest.main()
