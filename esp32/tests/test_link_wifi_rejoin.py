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

"""A paired Link whose boot join failed rejoins Wi-Fi from the heartbeat."""

from __future__ import annotations

import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
APP_C = ROOT / "main" / "app.c"


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


class LinkWifiRejoinTest(unittest.TestCase):
    def test_heartbeat_rejoins_on_link_builds(self) -> None:
        app = APP_C.read_text()
        start = app.index("    // Heartbeat: log full state every 5 s")
        loop = app[start:]
        branch = loop[loop.index("#else", loop.index("muse_keep_vm_session();")):]
        branch = branch[: branch.index("#endif")]
        self.assertIn("link_keep_wifi();", branch)
        # Before the VM retry, so a rejoin reaches the VM in the same tick.
        self.assertLess(branch.index("link_keep_wifi();"),
                        branch.index("schedule_vm_auth_refresh();"))
        guard = app.rindex("#if", 0, app.index("static void link_keep_wifi("))
        self.assertEqual(app[guard : app.index("\n", guard)], "#if !CONFIG_MUSE_ENABLED")

    def test_rejoin_backs_off_and_stops_once_joined(self) -> None:
        compiler = shlex.split(os.environ.get("CC", "cc"))
        if not compiler or shutil.which(compiler[0]) is None:
            self.skipTest("C compiler unavailable")
        app = APP_C.read_text()
        defines = "\n".join(line for line in app.splitlines()
                            if line.startswith("#define LINK_WIFI_"))
        function = function_source(app, "static void link_keep_wifi(")
        with tempfile.TemporaryDirectory() as name:
            tmp = Path(name)
            source = tmp / "rejoin.c"
            source.write_text("""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "app.h"
#define CONFIG_HOMEHUB_WIFI_SSID ""
#define WIFI_KNOWN_SSID_MAX 32
typedef enum { JOIN_ANY, JOIN_ANY_CACHED, JOIN_FIRST } join_mode_t;
enum { LED_STATE_WIFI_CONNECTED = 1, LED_STATE_ERROR = 2 };
static int64_t now_us;
static bool connected, setup_complete = true, gate_free = true;
static int joins, led, persisted;
static app_wifi_join_t next_result = APP_WIFI_NOT_NEARBY;
static char status[32], wifi[33];
static int64_t esp_timer_get_time(void) { return now_us; }
static bool wifi_mgr_is_connected(void) { return connected; }
static bool config_setup_complete(void) { return setup_complete; }
static bool operation_gate_take(int ticks, const char *op) {
    (void)ticks; (void)op; return gate_free;
}
static void operation_gate_give(void) {}
static app_wifi_join_t join_saved_networks(int timeout_ms, join_mode_t mode, bool show,
                                           char *joined) {
    assert(timeout_ms > 0 && mode == JOIN_ANY && show);
    joins++;
    snprintf(joined, WIFI_KNOWN_SSID_MAX + 1, "%s", next_result == APP_WIFI_JOINED ? "home" : "");
    if (next_result == APP_WIFI_JOINED) connected = true;
    return next_result;
}
static void ui_set_wifi(const char *s) { snprintf(wifi, sizeof(wifi), "%s", s); }
static void ui_set_status(const char *s) { snprintf(status, sizeof(status), "%s", s); }
static void led_status_set_state(int s) { led = s; }
static void persist_connected_wifi_channel(void) { persisted++; }
""" + defines + "\n" + function + """
static void tick(int64_t seconds) { now_us += seconds * 1000000LL; link_keep_wifi(); }
int main(void) {
    /* Unpaired: Wi-Fi is the phone's to set up. */
    setup_complete = false;
    tick(0);
    assert(joins == 0);
    setup_complete = true;

    /* The router is down: look at once, then back off 30 s, 60 s, ... */
    tick(1);
    assert(joins == 1 && led == LED_STATE_ERROR && strcmp(status, "wifi_failed") == 0);
    tick(5);
    assert(joins == 1);
    tick(25);
    assert(joins == 2);
    tick(30);
    assert(joins == 2);
    tick(30);
    assert(joins == 3);

    /* Another setup operation owns the gate: nothing happens. */
    gate_free = false;
    tick(600);
    assert(joins == 3);
    gate_free = true;

    /* The backoff never exceeds five minutes. */
    for (int i = 0; i < 10; i++) tick(301);
    assert(joins == 13);

    /* The router is back. */
    next_result = APP_WIFI_JOINED;
    tick(301);
    assert(joins == 14 && connected && led == LED_STATE_WIFI_CONNECTED);
    assert(strcmp(wifi, "home") == 0 && strcmp(status, "wifi_connected") == 0 && persisted == 1);

    /* Once joined, wifi_mgr rejoins by itself: the heartbeat stays out of it. */
    connected = false;
    next_result = APP_WIFI_NOT_NEARBY;
    tick(600);
    assert(joins == 14);
    puts("ok");
    return 0;
}
""")
            binary = tmp / "rejoin"
            built = subprocess.run(
                [*compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
                 "-I", str(ROOT / "main"), str(source), "-o", str(binary)],
                capture_output=True, text=True)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            ran = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
            self.assertEqual(ran.stdout.strip(), "ok")


if __name__ == "__main__":
    unittest.main()
