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


"""The Matter forwarder (the chip-tool-remote-forwarder component): the Link command's
registration and dispatch, and the setup reset. The component's own tests are
in its repository."""

from __future__ import annotations

import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
# On the Link; chip-tool-remote calls the rest over the forwarder's stream.
COMMANDS = ("matter.connect",)


def function_source(source: str, signature: str) -> str:
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 0
    for end in range(brace, len(source)):
        if source[end] == "{":
            depth += 1
        elif source[end] == "}":
            depth -= 1
            if depth == 0:
                return source[start : end + 1]
    raise AssertionError(f"Unclosed function: {signature}")


def compiler() -> list[str]:
    cc = shlex.split(os.environ.get("CC", "cc"))
    if not cc or shutil.which(cc[0]) is None:
        raise unittest.SkipTest("C compiler unavailable")
    return cc


def run(cmd: list[str]) -> str:
    done = subprocess.run(cmd, capture_output=True, text=True)
    if done.returncode:
        raise AssertionError(" ".join(cmd[:2]) + "\n" + done.stdout + done.stderr)
    return done.stdout


class ForwarderCommandsTest(unittest.TestCase):
    def registration(self, forwarder: bool = True) -> dict:
        noise = (ROOT / "main/noise_control.cpp").read_text()
        functions = "\n".join(function_source(noise, signature) for signature in (
            "static cJSON *string_param(",
            "static cJSON *row_params(",
            "static cJSON *typed_param(",
            "static void add_command(",
            "static void add_register_metadata_string(",
            "static char *build_register_json(",
        ))
        with tempfile.TemporaryDirectory() as name:
            tmp = Path(name)
            source = tmp / "registration.c"
            # As in test_link_ota.py: the C subset of C++, with the cJSON fake.
            source.write_text('''
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "esp_app_desc.h"
#define nullptr NULL
''' + ('''#define CONFIG_HOMEHUB_CHIP_TOOL_FORWARDER 1
''' if forwarder else '') + '''/* The most a forwarder board registers: the tunnel (which it needs) and display commands. */
#define CONFIG_HOMEHUB_TUNNEL 1
#define CONFIG_HOMEHUB_DISPLAY_COMMANDS 1
bool led_status_display_info(int *width, int *height) { *width = 800; *height = 480; return true; }
int led_status_display_bits(void) { return 16; }
bool ota_is_enabled(void) { return true; }
static esp_app_desc_t app = { .version = "999.0.0" };
const esp_app_desc_t *esp_app_get_description(void) { return &app; }
static char s_wifi_ssid[33] = "a-thirty-two-character-wifi-ssid", s_register_req_id[40];
static const char *s_node_id = "test-node-827fe8", *s_display_name = "Test device";
static void copy_wifi_ssid(char *out, size_t size) { snprintf(out, size, "%s", s_wifi_ssid); }
static void make_uuid(char *out, size_t size) { snprintf(out, size, "00000000-0000-0000-0000-000000000000"); }
''' + functions + '''
int main(void) {
    char *json = build_register_json();
    if (!json) return 1;
    puts(json);
    free(json);
    return 0;
}
''')
            binary = tmp / "registration"
            # typed_param() is only used by the feature (#if in noise_control.cpp; copied here without it)
            unused = [] if forwarder else ["-Wno-unused-function"]
            run([*compiler(), "-std=c11", "-Wall", "-Wextra", "-Werror", *unused, "-I", str(ROOT / "tests/link_fakes"),
                 str(source), str(ROOT / "tests/link_fakes/cJSON.c"), "-o", str(binary)])
            return json.loads(run([str(binary)]))

    def test_commands_are_advertised(self):
        params = self.registration()
        commands = params["params"]["commands_v2"]
        for name in COMMANDS:
            self.assertIn(name, commands)
        self.assertNotIn("matter.commission", commands)  # over the stream only
        # link.register is printed into at most 8 KB; a device whose
        # registration doesn't fit never registers.
        size = len(json.dumps(params, separators=(",", ":")))
        self.assertLess(size, 8192, f"link.register is {size} bytes")

    def test_commands_are_not_advertised_without_the_feature(self):
        commands = self.registration(forwarder=False)["params"]["commands_v2"]
        self.assertFalse([name for name in commands if name.startswith("matter.")])

    def test_commands_are_dispatched_with_the_feature(self):
        app = (ROOT / "main/app.c").read_text()
        start = app.index("static cJSON *on_ws_command(")
        dispatch = app[start:app.index("unsupported command", start)]
        block = dispatch[dispatch.index("#if CONFIG_HOMEHUB_CHIP_TOOL_FORWARDER"):]
        block = block[:block.index("#endif")]
        for name in COMMANDS:
            self.assertIn(f'strcmp(command, "{name}") == 0', block, name)
        self.assertIn("return chip_tool_forwarder_connect(params);", block)

    def test_setup_reset_forgets_the_fabric_and_boot_says_if_paired(self):
        app = (ROOT / "main/app.c").read_text()
        reset = function_source(app, "static void finish_setup_reset_with_gate_held(")
        self.assertIn("chip_tool_forwarder_erase();", reset)
        # An unpaired boot erases what a previous owner left (the component).
        run = function_source(app, "void app_run(void)")
        self.assertIn(".setup_complete = setup_complete,", run)
        self.assertIn("chip_tool_forwarder_init(&forwarder);", run)


if __name__ == "__main__":
    unittest.main()
