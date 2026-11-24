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

"""The servers the device takes for its credentials (muse_server_hosts.h).

Runs the real check, and provision_v2's use of it, on the host; the endpoint
lifecycle test covers the setters, boot and the order setup applies them in.
"""

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

from host_cjson import cjson_source
from test_link_unpair_storage_contract import _function_body


ROOT = Path(__file__).resolve().parents[1]
JSON = Path(os.environ.get(
    "CJSON_SOURCE_DIR", ROOT / "managed_components/espressif__cjson/cJSON"
))

API_ALLOWED = ["https://api.muse.ai", "https://hatch-api.meta.ai"]
NOISE_ALLOWED = ["hatch.metaaivm.com"]

API_REFUSED = [
    "http://api.muse.ai",
    "HTTPS://api.muse.ai",
    "https://API.MUSE.AI",
    "api.muse.ai",
    "//api.muse.ai",
    "https://api.muse.ai/",
    "https://api.muse.ai/hatch",
    "https://api.muse.ai?x=1",
    "https://api.muse.ai#x",
    "https://api.muse.ai.",
    "https://api.muse.ai:443",
    "https://api.muse.ai:8443",
    "https://user@api.muse.ai",
    "https://api.muse.ai@evil.example",
    "https://api.muse.ai\\@evil.example",
    "https://evil.example/https://api.muse.ai",
    "https://api.muse.ai.evil.example",
    "https://evilapi.muse.ai",
    "https://evil.api.muse.ai",
    "https://muse.ai",
    "https://evilmuse.ai",
    "https://meta.ai",
    "https://evil.meta.ai",
    "https://www.meta.ai",
    "https://hatch-api.meta.ai.evil.example",
    "https://hatch.metaaivm.com",
    "https://vm-1.metaaivm.com",
    " https://api.muse.ai",
    "https://api.muse.ai ",
    "https://api.muse.ai\r\nX-Injected: 1",
    "wss://api.muse.ai",
]
NOISE_REFUSED = [
    "evilmetaaivm.com",
    "metaaivm.com",
    "metaaivm.com.evil.com",
    "hatch.metaaivm.com.evil.com",
    "evil.hatch.metaaivm.com",
    "hatchmetaaivm.com",
    "vm-1.metaaivm.com",
    "0123abcd-0000-4000-8000-000000000000.metaaivm.com",
    "HATCH.METAAIVM.COM",
    "hatch.metaaivm.com.",
    "hatch.metaaivm.com:443",
    "hatch.metaaivm.com:8443",
    "user@hatch.metaaivm.com",
    "hatch.metaaivm.com@evil.com",
    "https://hatch.metaaivm.com",
    "wss://hatch.metaaivm.com",
    "hatch.metaaivm.com/",
    "hatch.metaaivm.com/v1/noise",
    "hatch.metaaivm.com?vm_id=x",
    "hatch.metaaivm.com\r\nX-Injected: 1",
    " hatch.metaaivm.com",
    "hatch.metaaivm.com ",
    "api.muse.ai",
    "evil.example",
]


def c_string(value: str) -> str:
    return json.dumps(value)


def compiler(name: str, default: str) -> list[str]:
    cmd = shlex.split(os.environ.get(name, default))
    if not cmd or shutil.which(cmd[0]) is None:
        raise unittest.SkipTest(f"{default} not available")
    return cmd


def run(args: list[str]) -> subprocess.CompletedProcess:
    return subprocess.run(args, cwd=ROOT, text=True, capture_output=True)


class LinkServerHostsTest(unittest.TestCase):
    def check_harness(self, source: str, cmd: list[str], flags: list[str], name: str) -> None:
        with tempfile.TemporaryDirectory() as directory:
            src = Path(directory) / name
            src.write_text(source)
            binary = Path(directory) / "harness"
            compiled = run([*cmd, *flags, "-Wall", "-Wextra", "-Werror",
                            "-I", str(ROOT / "components/muse"), str(src), "-o", str(binary)])
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            ran = run([str(binary)])
            self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)

    def test_allowlist(self) -> None:
        cases = []
        for value in API_ALLOWED + API_REFUSED:
            cases.append(("muse_server_api_allowed", value, value in API_ALLOWED))
        for value in NOISE_ALLOWED + NOISE_REFUSED:
            cases.append(("muse_server_noise_allowed", value, value in NOISE_ALLOWED))
        rows = "\n".join(
            f"    {{{fn}, {c_string(value)}, {int(ok)}}}," for fn, value, ok in cases
        )
        harness = f"""
#include <stdio.h>
#include "muse_server_hosts.h"
typedef bool (*check_fn)(const char *);
static const struct {{ check_fn fn; const char *value; int release_ok; }} cases[] = {{
{rows}
}};
int main(void) {{
    int failed = 0;
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {{
        // A development build that opts in takes any host.
        int want = CUSTOM_HOSTS ? 1 : cases[i].release_ok;
        if ((int)cases[i].fn(cases[i].value) != want) {{
            printf("unexpected result for \\"%s\\"\\n", cases[i].value);
            failed = 1;
        }}
    }}
    // Absent or empty means the default, in every build.
    if (!muse_server_api_allowed(NULL) || !muse_server_api_allowed("")
        || !muse_server_noise_allowed(NULL) || !muse_server_noise_allowed("")) {{
        printf("absent host refused\\n");
        failed = 1;
    }}
    return failed;
}}
"""
        for custom in (0, 1):
            flags = [f"-DCUSTOM_HOSTS={custom}"]
            if custom:
                flags.append("-DCONFIG_HOMEHUB_CUSTOM_SERVER_HOSTS=1")
            with self.subTest(language="c", custom_hosts=custom):
                self.check_harness(harness, compiler("CC", "cc"), ["-std=c11", *flags],
                                   "server_hosts.c")
            # noise_control.cpp includes it too.
            with self.subTest(language="c++", custom_hosts=custom):
                self.check_harness(harness, compiler("CXX", "c++"), ["-std=c++17", *flags],
                                   "server_hosts.cpp")

    def test_provision_v2_refuses_hosts_before_applying_any(self) -> None:
        source = (ROOT / "main/ble_server.c").read_text()
        dispatch = _function_body(source, "static void dispatch_command_ex(")
        block = dispatch[dispatch.index('strcmp(act, "provision_v2") == 0'):]
        block = block[:block.index('strcmp(act, "get_device_info") == 0')]
        refuse = block.index("} else if (!setup_hosts_allowed(root)) {")
        self.assertIn('ble_server_send_status("error_invalid_host");',
                      block[refuse:refuse + 120])
        # Nothing is allocated, marked or started before the refusal.
        for later in ("calloc(", "link_pairing_mark_provisioning_active()",
                      "helper_task_create(provision_task"):
            self.assertLess(refuse, block.index(later), later)

        if not (JSON / "cJSON.c").exists():
            self.skipTest("cJSON sources not available")
        helpers = ""
        for signature in ("static const char *optional_str(cJSON *root, const char *key)",
                          "static bool setup_hosts_allowed(cJSON *root)"):
            helpers += signature + " {" + _function_body(source, signature) + "}\n"
        documents = [
            ({}, 1),
            ({"api_url": "https://hatch-api.meta.ai", "api_url_v2": "https://api.muse.ai",
              "noise_host": "hatch.metaaivm.com"}, 1),
            ({"api_url": "", "api_url_v2": "", "noise_host": ""}, 1),
            # Fields that aren't strings were always ignored, as absent.
            ({"api_url_v2": 1, "noise_host": None, "api_url": ["https://evil.example"]}, 1),
            ({"api_url": "https://evil.example"}, 0),
            ({"api_url_v2": "https://api.muse.ai.evil.example"}, 0),
            ({"api_url_v2": "https://api.muse.ai:8443"}, 0),
            ({"api_url_v2": "https://api.muse.ai@evil.example"}, 0),
            ({"noise_host": "evilmetaaivm.com"}, 0),
            ({"noise_host": "metaaivm.com.evil.com"}, 0),
            ({"noise_host": "vm-1.metaaivm.com"}, 0),
            ({"api_url": "https://hatch-api.meta.ai", "api_url_v2": "https://api.muse.ai",
              "noise_host": "hatch.metaaivm.com.evil.com"}, 0),
        ]
        rows = "\n".join(
            f"    {{{c_string(json.dumps({'action': 'provision_v2', **doc}))}, {ok}}},"
            for doc, ok in documents
        )
        harness = f"""
#include <stdio.h>
#include "cJSON.h"
#include "muse_server_hosts.h"
{helpers}
static const struct {{ const char *json; int release_ok; }} cases[] = {{
{rows}
}};
int main(void) {{
    int failed = 0;
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {{
        cJSON *root = cJSON_Parse(cases[i].json);
        if (!root) return 2;
        int want = CUSTOM_HOSTS ? 1 : cases[i].release_ok;
        if ((int)setup_hosts_allowed(root) != want) {{
            printf("unexpected result for %s\\n", cases[i].json);
            failed = 1;
        }}
        cJSON_Delete(root);
    }}
    return failed;
}}
"""
        cc = compiler("CC", "cc")
        for custom in (0, 1):
            flags = ["-std=c11", f"-DCUSTOM_HOSTS={custom}", "-DCJSON_NESTING_LIMIT=16",
                     "-I", str(JSON), *cjson_source(JSON), "-lm"]
            if custom:
                flags.append("-DCONFIG_HOMEHUB_CUSTOM_SERVER_HOSTS=1")
            with self.subTest(custom_hosts=custom):
                self.check_harness(harness, cc, flags, "provision_hosts.c")

    def test_muse_host_setting(self) -> None:
        from test_muse_ptt_ui import FAKE_HEADERS, LINK_GC

        ble = (ROOT / "components/muse/muse_ble.c").read_text()
        self.assertRegex(ble, r'"hatch\.host"\)\) \{\s*if \(!muse_settings_set_hatch_host\(v\)\) '
                              r'\{\s*res = "error: server not allowed";')
        cc = compiler("CC", "cc")
        for custom in (0, 1):
            with self.subTest(custom_hosts=custom), tempfile.TemporaryDirectory() as tmp:
                stage = Path(tmp)
                for source in (ROOT / "components/muse").glob("*.h"):
                    shutil.copyfile(source, stage / source.name)
                shutil.copyfile(ROOT / "components/muse/muse_settings.c", stage / "muse_settings.c")
                shutil.copyfile(ROOT / "tests/muse_ptt_ui_fakes.h", stage / "muse_ptt_ui_fakes.h")
                for name in FAKE_HEADERS:
                    fake = stage / name
                    fake.parent.mkdir(parents=True, exist_ok=True)
                    fake.write_text('#include "muse_ptt_ui_fakes.h"\n')
                (stage / "sdkconfig.h").write_text("#define CONFIG_MUSE_DEFAULT_VOLUME 20\n")
                binary = stage / "settings"
                flags = [f"-DCUSTOM_HOSTS={custom}"]
                if custom:
                    flags.append("-DCONFIG_HOMEHUB_CUSTOM_SERVER_HOSTS=1")
                compiled = run([*cc, "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                                "-Wno-unused-parameter", "-Wno-unused-function",
                                "-DESP_ERR_INVALID_ARG=0x102", "-ffunction-sections",
                                "-fdata-sections", *LINK_GC, "-fsanitize=address,undefined",
                                *flags, "-include", str(ROOT / "tests/host_compat.h"),
                                "-I", tmp, str(ROOT / "tests/muse_server_host_settings_harness.c"),
                                "-o", str(binary)])
                self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
                ran = subprocess.run([str(binary)], text=True, capture_output=True,
                                     env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0:halt_on_error=1"})
                self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)

    def test_voice_session_ignores_a_saved_host_this_build_refuses(self) -> None:
        voice = (ROOT / "main/voice_muse_chat.c").read_text()
        body = _function_body(voice, "void voice_hatch_refresh(void)")
        self.assertRegex(body, r'config_get_str\("noise_host"[^;]*\|\|\s*'
                               r'!muse_server_noise_allowed\(host\)\)')

    def test_custom_hosts_stay_off_in_committed_builds(self) -> None:
        kconfig = (ROOT / "main/Kconfig.projbuild").read_text()
        option = kconfig[kconfig.index("config HOMEHUB_CUSTOM_SERVER_HOSTS"):]
        option = option[:option.index("\n    config ")]
        self.assertIn("default n", option)
        files = run(["git", "ls-files", "--", "*sdkconfig*"])
        if files.returncode != 0:
            self.skipTest("not a git checkout")
        configs = [ROOT / name for name in files.stdout.split()]
        self.assertTrue(configs)
        for config in configs:
            text = config.read_text(errors="replace")
            self.assertIsNone(
                re.search(r"^\s*CONFIG_HOMEHUB_CUSTOM_SERVER_HOSTS\s*=\s*y", text, re.M),
                f"{config.relative_to(ROOT)} turns on custom server hosts",
            )


if __name__ == "__main__":
    unittest.main()
