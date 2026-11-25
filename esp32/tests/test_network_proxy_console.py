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

"""Serial entry validates before saving and keeps credentials out of readback."""
import json
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

from host_cjson import cjson_source

ROOT = Path(__file__).resolve().parents[1]
JSON = Path(os.environ.get("CJSON_SOURCE_DIR", ROOT / "managed_components/espressif__cjson/cJSON"))


class ProxyConsole(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not (JSON / "cJSON.c").is_file():
            raise unittest.SkipTest("cJSON sources missing; run idf.py build")
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        folder = Path(cls.tmp.name)
        source = (ROOT / "main/sdk_token_console.c").read_text()
        command = source[source.index("static bool proxy_command("):source.index("\nbool sdk_token_console_command(")]
        fixture = r'''
#include "network_proxy.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define WIFI_KNOWN_SSID_MAX 32
#define WIFI_IF_STA 0
#define ESP_OK 0
typedef struct { struct { unsigned char ssid[32]; } sta; } wifi_config_t;
static network_proxy_config_t saved;
static bool known(const char *s) { return !strcmp(s, "Office"); }
static bool wifi_known_set_proxy(const char *s, const network_proxy_config_t *p) {
    if (!known(s)) return false;
    saved = *p;
    // Simulate a write acknowledged before a failed durable readback.
    if (p->port == 99) saved.port = 100;
    return true;
}
#if CONFIG_MUSE_ENABLED
static bool muse_link_wifi_proxy_set(const char *s, const network_proxy_config_t *p) {
    return wifi_known_set_proxy(s, p);
}
static int muse_link_wifi_proxy_get(const char *s, network_proxy_config_t *p) {
    *p = saved;
    return known(s) ? 0 : -1;
}
#else
static bool wifi_known_get_proxy(const char *s, network_proxy_config_t *p) { *p = saved; return known(s); }
static int esp_wifi_get_config(int mode, wifi_config_t *out) { (void)mode; memcpy(out->sta.ssid, "Office", 7); return 0; }
static void network_proxy_set_active(const network_proxy_config_t *p) { (void)p; }
#endif
'''
        main = r'''
int main(void) {
    char line[2048];
    while (fgets(line, sizeof(line), stdin)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!proxy_command(line)) return 3;
    }
    return 0;
}
'''
        src = folder / "console.c"
        src.write_text(fixture + command + main)
        cls.binaries = []
        for muse in (0, 1):
            binary = folder / f"console-{muse}"
            built = subprocess.run([
                *shlex.split(os.environ.get("CC", "cc")), "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                f"-DCONFIG_MUSE_ENABLED={muse}", "-DCJSON_NESTING_LIMIT=16",
                "-I", str(ROOT / "components/network_proxy/include"),
                str(src), str(ROOT / "components/network_proxy/network_proxy.c"), *cjson_source(JSON), "-o", str(binary),
            ], capture_output=True, text=True)
            if built.returncode:
                raise AssertionError(built.stdout + built.stderr)
            cls.binaries.append(binary)

    def run_commands(self, binary, commands):
        result = subprocess.run([str(binary)], input="".join("wifi_proxy=" + json.dumps(c) + "\n" for c in commands),
                                capture_output=True, text=True, timeout=3)
        self.assertEqual(result.returncode, 0, result.stderr)
        return result.stdout.splitlines()

    def test_each_console_variant_sets_reads_and_clears_without_leaking_auth(self):
        for binary in self.binaries:
            proxy = {"type": "socks5", "host": "proxy.example", "port": 1080, "username": "private-user", "password": "secret-pass"}
            with self.subTest(binary=binary):
                lines = self.run_commands(binary, [{"ssid": "Office", "proxy": proxy}, {"ssid": "Office"},
                    {"ssid": "Office", "proxy": {"type": "off"}}, {"ssid": "Office"}])
                self.assertNotIn("private-user", "\n".join(lines))
                self.assertNotIn("secret-pass", "\n".join(lines))
                state = json.loads(lines[1].removeprefix("@wifi_proxy "))
                self.assertEqual((state["type"], state["host"], state["port"], state["authenticated"]),
                                 ("socks5", "proxy.example", 1080, True))
                off = json.loads(lines[3].removeprefix("@wifi_proxy "))
                self.assertEqual((off["type"], off["authenticated"]), ("off", False))

    def test_invalid_and_unknown_network_commands_leave_config_unchanged(self):
        for binary in self.binaries:
            lines = self.run_commands(binary, [{"ssid": "Office", "proxy": {"type": "pac"}},
                {"ssid": "missing", "proxy": {"type": "off"}}, {"ssid": "Office"}])
            self.assertIn("error invalid", lines[0])
            self.assertIn("error", lines[1])
            self.assertEqual(json.loads(lines[2].removeprefix("@wifi_proxy "))["type"], "off")

    def test_headless_save_requires_durable_readback(self):
        lines = self.run_commands(self.binaries[0], [{"ssid": "Office", "proxy": {"type": "http", "host": "proxy.example", "port": 99}}])
        self.assertEqual(lines, ["@wifi_proxy error storage_or_unknown_network"])

    def test_provisioning_applies_proxy_before_cloud_credentials(self):
        ble = (ROOT / "main/ble_server.c").read_text()
        # Skip the shared authentication precheck and unrelated direct-OTA
        # handler: only provisioning must parse its proxy before activation.
        start = ble.index('} else if (decrypted && strcmp(act, "provision_v2") == 0) {')
        provision = ble[start:]
        self.assertLess(provision.index("network_proxy_parse(proxy, &a->proxy)"),
                        provision.index("link_pairing_mark_provisioning_active()"))
        self.assertIn("a->proxy_present ? &a->proxy : NULL", ble)
        app = (ROOT / "main/app.c").read_text()
        provision = app[app.index("static void on_provision("):]
        self.assertLess(provision.index("wifi_known_set_proxy(ssid, proxy)"),
                        provision.index("accept_pairing_credentials("))
        self.assertLess(provision.index("network_proxy_set_active(proxy)"),
                        provision.index("connect_preferred_vm_with_gate_held("))


if __name__ == "__main__":
    unittest.main()
