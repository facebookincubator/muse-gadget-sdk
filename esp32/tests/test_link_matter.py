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


"""Matter devices through Link commands (components/esp32_matter_controller): the
protocol helpers on the host, and the commands' registration and dispatch."""

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
COMPONENT = ROOT / "components/esp32_matter_controller"
# CI supplies pinned upstream sources; local IDF builds already have cJSON.
JSON = Path(os.environ.get(
    "CJSON_SOURCE_DIR", ROOT / "managed_components/espressif__cjson/cJSON"
))
COMMANDS = ("matter.commission", "matter.commissionables", "matter.nodes", "matter.remove", "matter.invoke",
            "matter.read", "matter.write", "matter.open_window")


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


class MatterProtocolTest(unittest.TestCase):
    def test_protocol_helpers(self):
        cc = compiler()
        with tempfile.TemporaryDirectory() as name:
            out = Path(name)
            flags = ["-std=c11", "-Wall", "-Wextra", "-Werror", "-DCJSON_NESTING_LIMIT=16",
                     "-I", str(JSON), "-I", str(COMPONENT)]
            run([*cc, *flags, "-c", str(JSON / "cJSON.c"), "-o", str(out / "cjson.o")])
            run([*cc, *flags, str(COMPONENT / "esp32_matter_controller_proto.c"),
                 str(ROOT / "tests/link_matter_proto_harness.c"), str(out / "cjson.o"), "-lm",
                 "-o", str(out / "proto")])
            self.assertIn("esp32_matter_controller_proto: ok", run([str(out / "proto")]))


class MatterCommandsTest(unittest.TestCase):
    def registration(self) -> dict:
        noise = (ROOT / "main/noise_control.cpp").read_text()
        functions = "\n".join(function_source(noise, signature) for signature in (
            "static cJSON *string_param(",
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
#define CONFIG_HOMEHUB_MATTER_CONTROLLER 1
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
            run([*compiler(), "-std=c11", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "tests/link_fakes"),
                 str(source), str(ROOT / "tests/link_fakes/cJSON.c"), "-o", str(binary)])
            return json.loads(run([str(binary)]))

    def test_commands_are_advertised(self):
        params = self.registration()
        commands = params["params"]["commands_v2"]
        for name in COMMANDS:
            self.assertIn(name, commands)
        # link.register is printed into at most 8 KB; a device whose
        # registration doesn't fit never registers.
        size = len(json.dumps(params, separators=(",", ":")))
        self.assertLess(size, 7 * 1024, f"link.register is {size} bytes")

    def test_commands_are_dispatched_with_the_feature(self):
        app = (ROOT / "main/app.c").read_text()
        start = app.index("static cJSON *on_ws_command(")
        dispatch = app[start:app.index("unsupported command", start)]
        block = dispatch[dispatch.index("#if CONFIG_HOMEHUB_MATTER_CONTROLLER"):]
        block = block[:block.index("#endif")]
        self.assertIn('strncmp(command, "matter.", 7) == 0', block)
        self.assertIn("esp32_matter_controller_command(command, params, request_id, session_generation,", block)
        source = (COMPONENT / "esp32_matter_controller.cpp").read_text()
        handlers = (function_source(source, "cJSON *esp32_matter_controller_command(")
                    + function_source(source, "static cJSON *run_command(const char *command, cJSON *params, "
                                              "const char *request_id, uint64_t session) {"))
        for name in COMMANDS:
            self.assertIn(f'strcmp(command, "{name}") == 0', handlers, name)


if __name__ == "__main__":
    unittest.main()
