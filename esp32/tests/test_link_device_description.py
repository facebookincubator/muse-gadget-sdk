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


"""What a gadget tells Muse about itself: family, model and device.guide."""

from __future__ import annotations

import json
import os
import re
import shlex
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
NOISE = (ROOT / "main" / "noise_control.cpp").read_text()
KCONFIG = (ROOT / "main" / "Kconfig.projbuild").read_text()
OVERLAYS = sorted((ROOT / "devices").glob("sdkconfig.*"))
CJSON = Path(os.environ.get("CJSON_SOURCE_DIR", ROOT / "managed_components/espressif__cjson/cJSON"))
REGISTER_LIMIT = 8192
GUIDE_LIMIT = 4096
GUIDE_DESCRIPTION = "This device's usage guide (Markdown): what it is, its controls and limits."
VALID_MODEL_NAME = re.compile(r"[ !#-\[\]-~]{1,128}")
REPOSITORY_GUIDE_LIMIT = 2048
BLE_CLIENT_STUBS = """
#include "ble_client.h"
#define FAIL { return -1; }
int ble_client_scan(uint32_t a, bool b, const char *c, ble_client_dev_t *d, int e, int *f, uint32_t *g) FAIL
int ble_client_connect(const uint8_t a[6], uint8_t b, uint32_t c, ble_client_conn_info_t *d) FAIL
int ble_client_disconnect(uint16_t a) FAIL
int ble_client_connections(ble_client_conn_info_t *a, int b) { return 0; }
int ble_client_secure(uint16_t a, uint32_t b) FAIL
int ble_client_discover(uint16_t a, const ble_client_db_t **b) FAIL
int ble_client_find_chr(uint16_t a, uint16_t b, const ble_client_uuid_t *c, const ble_client_uuid_t *d,
                        ble_client_chr_t *e) FAIL
int ble_client_read(uint16_t a, uint16_t b, uint8_t *c, size_t d, size_t *e) FAIL
int ble_client_write(uint16_t a, uint16_t b, const uint8_t *c, size_t d, bool e) FAIL
int ble_client_subscribe(uint16_t a, const ble_client_chr_t *b, uint32_t c, bool d,
                         ble_client_notification_t *e, int f, int *g, uint32_t *h) FAIL
void ble_client_reset(void) {}
"""


def function_source(source: str, signature: str) -> str:
    start = source.index(signature)
    depth = 0
    for end in range(source.index("{", start), len(source)):
        depth += {"{": 1, "}": -1}.get(source[end], 0)
        if depth == 0:
            return source[start : end + 1]
    raise AssertionError(f"Unclosed function: {signature}")


def kconfig_default(name: str) -> str:
    return re.search(rf'config {name}\n(?:.*\n)*?\s+default "([^"]*)"', KCONFIG).group(1)


def overlay_values(key: str) -> dict[str, str]:
    values = {}
    for overlay in OVERLAYS:
        match = re.search(rf'^CONFIG_{key}="([^"]*)"$', overlay.read_text(), re.M)
        if match:
            values[overlay.name] = match.group(1)
    return values


def policy_problems(guide: str) -> list[str]:
    """What breaks the mechanical rules for this repository's guides (protocols/DEVICE.md)."""
    checks = {
        "a link": re.search(r"https?://|www\.", guide),
        "HTML": re.search(r"<[A-Za-z!/]", guide),
        "a code block": re.search(r"^\s*(```|~~~)", guide, re.M),
        "a control character": re.search(r"[\x00-\x09\x0b-\x1f\x7f-\x9f]", guide),
        "not one top-level heading first": not guide.startswith("# ") or len(re.findall(r"^# ", guide, re.M)) != 1,
        f"over {REPOSITORY_GUIDE_LIMIT} bytes": len(guide.encode()) > REPOSITORY_GUIDE_LIMIT,
    }
    return [problem for problem, found in checks.items() if found]


def assemble_guide(out: Path, guide: str | None = None) -> subprocess.CompletedProcess:
    defines = [f"-DCONFIG_MUSE_DEVICE_GUIDE={guide}"] if guide else []
    return subprocess.run(
        ["cmake", f"-DGUIDE_OUT={out}", *defines, "-P", str(ROOT / "cmake/device_guide.cmake")],
        capture_output=True, text=True)


class DeviceDescriptionTest(unittest.TestCase):
    def compile_and_run(self, tmp: Path, harness: str, defines: list[str] = (), ble: bool = False) -> str:
        if not (CJSON / "cJSON.c").exists():
            self.skipTest("cJSON sources not available (run idf.py build once)")
        cc = shlex.split(os.environ.get("CC", "cc"))
        if not cc or shutil.which(cc[0]) is None:
            self.skipTest("C compiler unavailable")
        flags = ["-std=c11", "-D_DEFAULT_SOURCE", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
                 "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                 "-DCJSON_NESTING_LIMIT=16", "-I", str(CJSON)]
        sources = [CJSON / "cJSON.c"]
        if ble:
            (tmp / "ble_client_stubs.c").write_text(BLE_CLIENT_STUBS)
            sources += [ROOT / "main/ble_commands.c", tmp / "ble_client_stubs.c"]
            flags += ["-I", str(ROOT / "tests/ble_commands_fakes"), "-I", str(ROOT / "main")]
        (tmp / "harness.c").write_text(harness)
        built = subprocess.run(
            [*cc, *flags, "-Wno-unused-function", *defines, str(tmp / "harness.c"), *map(str, sources),
             "-lm", "-o", str(tmp / "harness")],
            capture_output=True, text=True)
        self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
        ran = subprocess.run([str(tmp / "harness")], capture_output=True, text=True)
        self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
        return ran.stdout

    def registrations(self, defines: list[str], node_id: str = "homelink-0a1b2c",
                      display_name: str = "MuseGadget-0A1B2C", ble: bool = False,
                      ssid: str = "0123456789abcdef0123456789abcdef") -> list[str]:
        family = NOISE[NOISE.index("#ifndef MUSE_DEVICE_FAMILY"):]
        family = family[: family.index("#endif") + len("#endif")]
        functions = "\n".join(function_source(NOISE, signature) for signature in (
            "static cJSON *string_param(", "static cJSON *row_params(", "static void add_command(",
            "static void add_register_metadata_string(", "static char *build_register_json(",
        ))
        # Print however large it gets, so the test reports the real size.
        cap = f"size <= {REGISTER_LIMIT} && !json"
        self.assertIn(cap, functions)
        harness = f'''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#define nullptr NULL
typedef struct {{ char version[32]; }} esp_app_desc_t;
static esp_app_desc_t app = {{ .version = "999.0.0-0123456789abcdefghij" }};
const esp_app_desc_t *esp_app_get_description(void) {{ return &app; }}
static char s_wifi_ssid[33] = "{ssid}", s_register_req_id[40];
static const char *s_node_id = "{node_id}", *s_display_name = "{display_name}";
static void copy_wifi_ssid(char *out, size_t size) {{ snprintf(out, size, "%s", s_wifi_ssid); }}
static void make_uuid(char *out, size_t size) {{ snprintf(out, size, "00000000-0000-4000-8000-000000000000"); }}
static int bits = 16;
bool led_status_display_info(int *w, int *h) {{ *w = 800; *h = 480; return true; }}
int led_status_display_bits(void) {{ return bits; }}
bool ota_is_enabled(void) {{ return true; }}
void ble_commands_register(cJSON *commands);
{family}
{functions.replace(cap, "size <= 65536 && !json")}
int main(void) {{
    const int depths[] = {{1, 2, 4, 16}};
    for (int i = 0; i < 4; i++) {{
        bits = depths[i];
        char *json = build_register_json();
        assert(json);
        puts(json);
        free(json);
    }}
    return 0;
}}
'''
        with tempfile.TemporaryDirectory() as name:
            output = self.compile_and_run(Path(name), harness, defines, ble)
        return output.splitlines()

    def test_register_describes_the_device(self) -> None:
        params = json.loads(self.registrations([
            "-DCONFIG_HOMEHUB_TUNNEL=1",
            '-DCONFIG_MUSE_DEVICE_MODEL_ID="acme-lamp"', '-DCONFIG_MUSE_DEVICE_MODEL_NAME="Acme Lamp"',
        ])[0])["params"]
        self.assertEqual((params["device_family"], params["model_id"]), ("gadget", "acme-lamp"))
        self.assertEqual(params["metadata"]["model_name"], "Acme Lamp")
        self.assertEqual(params["commands_v2"]["device.guide"],
                         {"description": GUIDE_DESCRIPTION, "required": {}, "optional": {}})

    def test_register_fits_its_buffer_in_the_largest_builds(self) -> None:
        # The richest option sets a board can build with, plus the opt-in BLE
        # commands, OTA and the tunnel, at the largest values the build
        # accepts: a 64-character model id, a 128-character model name (no
        # character of it is escaped) and long node id and BLE names. The
        # SSID is 32 quotation marks, each escaped to two bytes: an SSID with a
        # control character (six bytes each) isn't sent.
        profiles = {
            "e-paper status screen": ["CONFIG_HOMEHUB_DISPLAY_COMMANDS", "CONFIG_HOMEHUB_RETERMINAL_SHT4X"],
            "SenseCAP Indicator": ["CONFIG_HOMEHUB_DISPLAY_COMMANDS", "CONFIG_HOMEHUB_SENSECAP_SENSORS"],
            "four-colour e-paper": ["CONFIG_HOMEHUB_DISPLAY_COMMANDS", "CONFIG_HOMEHUB_LED_BACKEND_WAVESHARE_EPD154G",
                                    "CONFIG_HOMEHUB_WAVESHARE_154G_SHTC3", "CONFIG_HOMEHUB_VOICE"],
            "full UI": ["CONFIG_HOMEHUB_LED_BACKEND_MUSE", "CONFIG_HOMEHUB_DISPLAY_COMMANDS",
                        "CONFIG_MUSE_WATCHER_CAMERA"],
            "full UI, P4 camera": ["CONFIG_HOMEHUB_LED_BACKEND_MUSE", "CONFIG_HOMEHUB_DISPLAY_COMMANDS",
                                   "CONFIG_MUSE_P4_CSI_CAMERA"],
            "voice": ["CONFIG_HOMEHUB_VOICE"],
        }
        model_id, model_name = "z" * 64, "~" * 128
        self.assertTrue(VALID_MODEL_NAME.fullmatch(model_name))
        common = [
            "-DCONFIG_HOMEHUB_TUNNEL=1", "-DCONFIG_HOMEHUB_BLE_COMMANDS=1",
            f'-DCONFIG_MUSE_DEVICE_MODEL_ID="{model_id}"', f'-DCONFIG_MUSE_DEVICE_MODEL_NAME="{model_name}"',
        ]
        for profile, options in profiles.items():
            with self.subTest(profile=profile):
                lines = self.registrations(
                    [*common, *(f"-D{option}=1" for option in options)], "n" * 31, "d" * 31, ble=True,
                    ssid='\\"' * 32)
                for line in lines:
                    params = json.loads(line)["params"]
                    self.assertEqual((params["model_id"], params["metadata"]["model_name"]), (model_id, model_name))
                    self.assertEqual(params["metadata"]["network_ssid"], '"' * 32)
                    self.assertIn("device.guide", params["commands_v2"])
                    self.assertIn("ble.subscribe", params["commands_v2"])
                size = max(len(line) for line in lines)
                print(f"link.register, {profile}: {size} of {REGISTER_LIMIT} bytes")
                # cJSON_PrintPreallocated asks for 5 bytes beyond the output.
                self.assertLessEqual(size + 5, REGISTER_LIMIT)

    def test_register_leaves_out_an_ssid_with_control_characters(self) -> None:
        defines = ['-DCONFIG_MUSE_DEVICE_MODEL_ID="acme-lamp"', '-DCONFIG_MUSE_DEVICE_MODEL_NAME="Acme Lamp"']
        for ssid, sent in (("\\x01" * 32, None), ("Home\\tNet", None), ('\\"Caf\\xc3\\xa9\\x7f', '"Caf\u00e9\x7f')):
            with self.subTest(ssid=ssid):
                metadata = json.loads(self.registrations(defines, ssid=ssid)[0])["params"]["metadata"]
                self.assertEqual(metadata.get("network_ssid"), sent)
                self.assertEqual(metadata["model_name"], "Acme Lamp")

    def test_every_model_line_is_valid(self) -> None:
        ids = {"Kconfig": kconfig_default("MUSE_DEVICE_MODEL_ID"), **overlay_values("MUSE_DEVICE_MODEL_ID")}
        names = {"Kconfig": kconfig_default("MUSE_DEVICE_MODEL_NAME"), **overlay_values("MUSE_DEVICE_MODEL_NAME")}
        self.assertGreater(len(ids), 1)
        self.assertEqual(ids.keys(), names.keys())
        for where, model_id in ids.items():
            with self.subTest(where=where):
                self.assertRegex(model_id, r"^[a-z0-9-]{1,64}$")
                self.assertNotIn("hatch", model_id)
        for where, name in names.items():
            with self.subTest(where=where):
                self.assertTrue(VALID_MODEL_NAME.fullmatch(name), name)
                self.assertNotIn("Hatch", name)

    def test_every_board_overlay_sets_its_model(self) -> None:
        # A board overlay sets the target; stacked profiles (bench, video,
        # power-test, camera) inherit their board's model.
        for overlay in OVERLAYS:
            text = overlay.read_text()
            if not re.search(r'^CONFIG_IDF_TARGET=', text, re.M):
                continue
            with self.subTest(overlay=overlay.name):
                self.assertRegex(text, r'(?m)^CONFIG_MUSE_DEVICE_MODEL_ID="[^"]+"$')
                self.assertRegex(text, r'(?m)^CONFIG_MUSE_DEVICE_MODEL_NAME="[^"]+"$')

    def test_the_build_refuses_a_model_muse_would_not_take(self) -> None:
        if shutil.which("cmake") is None:
            self.skipTest("cmake unavailable")
        base = ["-DCONFIG_CJSON_NESTING_LIMIT=16", "-DCONFIG_LWIP_TCP_SND_BUF_DEFAULT=16384",
                "-DCONFIG_LWIP_TCP_WND_DEFAULT=16384"]
        for model_id, name, accepted in (
                ("acme-lamp", "Acme Lamp (2nd gen) #1 ~ 100%", True), ("z" * 64, "~" * 128, True),
                ("Acme", "Lamp", False), ("a" * 65, "Lamp", False),
                ("acme-lamp", "", False), ("acme-lamp", "x" * 129, False), ("acme-lamp", "Caf\u00e9 Lamp", False),
                ("acme-lamp", "Lamp\tTwo", False), ("acme-lamp", "Lamp\x85", False),
                ("acme-lamp", 'The "Lamp"', False), ("acme-lamp", "Lamp\\Two", False)):
            with self.subTest(model_id=model_id, name=name):
                ran = subprocess.run(
                    ["cmake", *base, f"-DCONFIG_MUSE_DEVICE_MODEL_ID={model_id}",
                     f"-DCONFIG_MUSE_DEVICE_MODEL_NAME={name}", "-P", str(ROOT / "cmake/validate_config.cmake")],
                    capture_output=True, text=True)
                self.assertEqual(ran.returncode == 0, accepted, ran.stderr)

    def test_guide_command_returns_the_embedded_markdown(self) -> None:
        # Fails the Nth allocation once: every run sends a result, the guide
        # or an error, and leaks nothing.
        harness = '''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
typedef uint64_t noise_ctrl_session_generation_t;
const char muse_device_guide_md[] = "# Test\\n\\nA \\"quoted\\" guide.\\n";
static cJSON *queued;
static int allocations, fail_at;
static void *test_malloc(size_t size) { return ++allocations == fail_at ? NULL : malloc(size); }
static void queue_result(noise_ctrl_session_generation_t generation, const char *id, cJSON *r) {
    assert(generation == 7 && !strcmp(id, "req-1") && !queued);
    queued = r;
}
''' + function_source(NOISE, "static void send_device_guide(") + '''
int main(void) {
    cJSON_Hooks hooks = { test_malloc, free };
    cJSON_InitHooks(&hooks);
    for (int n = 0; n < 16; n++) {
        allocations = 0;
        fail_at = n;
        send_device_guide(7, "req-1");
        fail_at = 0;
        char *json = queued ? cJSON_PrintUnformatted(queued) : NULL;
        puts(json ? json : "null");
        free(json);
        cJSON_Delete(queued);
        queued = NULL;
    }
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as name:
            results = [json.loads(line) for line in self.compile_and_run(Path(name), harness).splitlines()]
        guide = {"ok": True, "payload": {"format": "markdown", "guide": '# Test\n\nA "quoted" guide.\n'}}
        self.assertEqual(results[0], guide)
        for result in results[1:]:
            self.assertIn(result, (guide, {"ok": False, "error": {
                "code": "out_of_memory", "message": "could not build the device guide"}}))
        self.assertIn(False, [result["ok"] for result in results])

    def test_guide_is_dispatched_by_the_control_session(self) -> None:
        invoke = function_source(NOISE, "static void handle_invoke_request(")
        self.assertIn('strcmp(command, "device.guide") == 0', invoke)
        self.assertIn("send_device_guide(session_generation, request_id);", invoke)
        self.assertLess(invoke.index('"device.guide"'), invoke.index("s_command_cb("))
        cmake = (ROOT / "main" / "CMakeLists.txt").read_text()
        self.assertIn("muse_device_guide_assemble(", cmake)
        self.assertIn('target_add_binary_data(${COMPONENT_LIB} "${CMAKE_CURRENT_BINARY_DIR}/muse_device_guide.md" TEXT)',
                      cmake)

    def test_every_configured_guide_embeds_within_the_cap(self) -> None:
        if shutil.which("cmake") is None:
            self.skipTest("cmake unavailable")
        guides = {kconfig_default("MUSE_DEVICE_GUIDE"), *overlay_values("MUSE_DEVICE_GUIDE").values()}
        with tempfile.TemporaryDirectory() as name:
            for i, guide in enumerate(sorted(guides)):
                with self.subTest(guide=guide):
                    out = Path(name) / f"{i}.md"
                    ran = assemble_guide(out, guide)
                    self.assertEqual(ran.returncode, 0, ran.stderr)
                    text = out.read_text()
                    print(f"{guide}: {len(text.encode())} of {GUIDE_LIMIT} bytes")
                    self.assertLessEqual(len(text.encode()), GUIDE_LIMIT)
                    self.assertTrue(text.startswith("# "), text[:40])
                    self.assertNotIn("<!--", text)
                    self.assertNotIn("Hatch", text)

    def test_boards_get_a_guide_that_fits_their_hardware(self) -> None:
        # The XVF3800 has no status light, and the LCD-7 no microphone, so their
        # guides must not promise one.
        guides = overlay_values("MUSE_DEVICE_GUIDE")
        self.assertEqual(guides["sdkconfig.seeed-respeaker-xvf3800"], "devices/guides/voice.md")
        voice = " ".join((ROOT / "devices/guides/voice.md").read_text().split())
        self.assertIn("Seeed reSpeaker XVF3800 running", voice)
        self.assertIn("The reSpeaker XVF3800 shows no state", voice)
        self.assertNotIn("sdkconfig.muse-waveshare-s3-lcd7", guides)
        self.assertEqual(guides["sdkconfig.muse"], "devices/guides/muse.md")
        muse = " ".join((ROOT / "devices/guides/muse.md").read_text().split())
        self.assertIn("on boards with a microphone, people talk to Muse", muse)
        self.assertIn("On boards with a microphone, people talk to Muse", muse)
        self.assertIn("where the board has a microphone", muse)

    def test_this_repositorys_guides_follow_the_policy(self) -> None:
        if shutil.which("cmake") is None:
            self.skipTest("cmake unavailable")
        guides = [ROOT / "main" / "guide.md", *sorted((ROOT / "devices" / "guides").glob("*.md"))]
        with tempfile.TemporaryDirectory() as name:
            for i, path in enumerate(guides):
                with self.subTest(guide=path.name):
                    out = Path(name) / f"{i}.md"
                    self.assertEqual(assemble_guide(out, str(path)).returncode, 0)
                    self.assertEqual(policy_problems(out.read_text()), [])
        for bad in ("# A\n\nSee https://example.com\n", "# A\n\n<b>B</b>\n", "# A\n\n```\nx\n```\n",
                    "# A\tB\n", "Intro\n# A\n", "# A\n# B\n", "# A\n" + "x" * REPOSITORY_GUIDE_LIMIT):
            with self.subTest(bad=bad[:30]):
                self.assertNotEqual(policy_problems(bad), [])

    def test_a_custom_guide_embeds_and_a_bad_one_fails_the_build(self) -> None:
        if shutil.which("cmake") is None:
            self.skipTest("cmake unavailable")
        with tempfile.TemporaryDirectory() as name:
            tmp = Path(name)
            cases = {
                "board.md": ("<!--\nlicense\n-->\r\n\n# My board\n\nPress *Go*; $5; ${X} @Y@.\n", None),
                "big.md": ("x" * (GUIDE_LIMIT + 1), "4097 bytes"),
                "open.md": ("<!-- no end\n# Board\n", "comment"),
            }
            for file, (text, error) in cases.items():
                with self.subTest(file=file):
                    (tmp / file).write_text(text)
                    ran = assemble_guide(tmp / f"out-{file}", str(tmp / file))
                    if error:
                        self.assertNotEqual(ran.returncode, 0)
                        self.assertIn(error, ran.stderr)
                    else:
                        self.assertEqual(ran.returncode, 0, ran.stderr)
                        self.assertEqual((tmp / f"out-{file}").read_text(), "# My board\n\nPress *Go*; $5; ${X} @Y@.\n")
            ran = assemble_guide(tmp / "missing.md", str(tmp / "none.md"))
            self.assertNotEqual(ran.returncode, 0)
            self.assertIn("not found", " ".join(ran.stderr.split()))


if __name__ == "__main__":
    unittest.main()
