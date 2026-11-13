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

"""wifi_mgr's reconnect after an association, when the AP it pinned is gone.

wifi_mgr_connect() targets the strongest BSSID from the last scan. This builds
the real disconnect handling from main/wifi_mgr.c against fakes and checks that
a reconnect keeps that BSSID while the AP may come back, and stops pinning it
once the driver reports the AP isn't there, so another AP of the same network
can take over.
"""

from __future__ import annotations

import os
import shlex
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
WIFI_MGR_C = ROOT / "main" / "wifi_mgr.c"

PRELUDE = r"""
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef const char *esp_event_base_t;
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
static const char WIFI_EVENT[] = "WIFI_EVENT";
static const char IP_EVENT[] = "IP_EVENT";
enum { WIFI_EVENT_STA_START = 2, WIFI_EVENT_STA_DISCONNECTED = 5 };
enum { IP_EVENT_STA_GOT_IP = 0 };
enum {
    WIFI_REASON_BEACON_TIMEOUT = 200,
    WIFI_REASON_NO_AP_FOUND = 201,
    WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY = 210,
    WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD = 211,
    WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD = 212,
};
typedef enum { WIFI_FAST_SCAN = 0, WIFI_ALL_CHANNEL_SCAN = 1 } wifi_scan_method_t;
typedef enum { WIFI_CONNECT_AP_BY_SIGNAL = 0, WIFI_CONNECT_AP_BY_SECURITY = 1 } wifi_sort_method_t;
typedef enum { WIFI_IF_STA = 0 } wifi_interface_t;
typedef struct {
    uint8_t ssid[32];
    uint8_t password[64];
    wifi_scan_method_t scan_method;
    bool bssid_set;
    uint8_t bssid[6];
    uint8_t channel;
    wifi_sort_method_t sort_method;
    struct { int8_t rssi; int authmode; } threshold;
} wifi_sta_config_t;
typedef union { wifi_sta_config_t sta; } wifi_config_t;
typedef struct { uint8_t reason; int8_t rssi; } wifi_event_sta_disconnected_t;
typedef struct { struct { uint32_t addr; } ip; } esp_ip_info_t;
typedef struct { esp_ip_info_t ip_info; } ip_event_got_ip_t;
#define IPSTR "%u"
#define IP2STR(ip) (unsigned)(ip)->addr
static void log_ignore(const char *tag, const char *fmt, ...) { (void)tag; (void)fmt; }
#define ESP_LOGI(tag, ...) log_ignore(tag, __VA_ARGS__)
#define ESP_LOGW(tag, ...) log_ignore(tag, __VA_ARGS__)
static const char *TAG = "test";

static wifi_config_t s_cfg;
static int s_connects, s_reconnects, s_set_configs;
static esp_err_t esp_wifi_get_config(wifi_interface_t i, wifi_config_t *c) {
    (void)i; *c = s_cfg; return ESP_OK;
}
static esp_err_t esp_wifi_set_config(wifi_interface_t i, wifi_config_t *c) {
    (void)i; s_cfg = *c; s_set_configs++; return ESP_OK;
}
static esp_err_t esp_wifi_connect(void) { s_connects++; return ESP_OK; }
static int64_t esp_timer_get_time(void) { return 1000000; }
enum { LINK_EVENT_WIFI_DOWN, LINK_EVENT_WIFI_UP };
static void link_events_record(int ev, const char *fmt, ...) { (void)ev; (void)fmt; }
typedef int EventGroupHandle_t;
static EventGroupHandle_t s_events;
#define BIT_CONNECTED 1
#define BIT_FAIL 2
#define BIT_GOT_IP 4
static void xEventGroupClearBits(EventGroupHandle_t e, int b) { (void)e; (void)b; }
static void xEventGroupSetBits(EventGroupHandle_t e, int b) { (void)e; (void)b; }
static bool s_has_ip;
static int64_t s_ip_lost_us;
static atomic_bool s_sta_idle;
static bool s_connecting, s_keep_connected;
static int s_retry;
#define MAX_INITIAL_RETRY 3
#define RECONNECT_BACKOFF_MIN_MS 1000
static int s_reconnect_backoff_ms = 1000;
static void schedule_reconnect(void) { s_reconnects++; }
static void cache_connected_ap(void) {}
static void cache_disconnect_rssi(const wifi_event_sta_disconnected_t *ev) { (void)ev; }
"""

HARNESS = r"""
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); return 1; } } while (0)

static void disconnect(uint8_t reason) {
    wifi_event_sta_disconnected_t ev = { .reason = reason, .rssi = -60 };
    event_handler(NULL, WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &ev);
}

int main(void) {
    static const uint8_t pinned[6] = {1, 2, 3, 4, 5, 6};
    memcpy(s_cfg.sta.ssid, "home", 4);
    memcpy(s_cfg.sta.bssid, pinned, 6);
    s_cfg.sta.bssid_set = true;
    s_cfg.sta.channel = 6;
    s_cfg.sta.scan_method = WIFI_FAST_SCAN;
    s_keep_connected = true;   // associated once: reconnects run forever

    // The AP drops out: the first reconnect still targets it.
    disconnect(WIFI_REASON_BEACON_TIMEOUT);
    CHECK(s_reconnects == 1);
    CHECK(s_cfg.sta.bssid_set);
    CHECK(memcmp(s_cfg.sta.bssid, pinned, 6) == 0);

    // That reconnect finds no AP with that BSSID: stop pinning it.
    disconnect(WIFI_REASON_NO_AP_FOUND);
    CHECK(s_reconnects == 2);
    CHECK(!s_cfg.sta.bssid_set);
    CHECK(s_cfg.sta.channel == 0);
    CHECK(s_cfg.sta.scan_method == WIFI_ALL_CHANNEL_SCAN);
    CHECK(s_cfg.sta.sort_method == WIFI_CONNECT_AP_BY_SIGNAL);
    CHECK(memcmp(s_cfg.sta.ssid, "home", 4) == 0);

    // Nothing pinned any more: further misses don't rewrite the config.
    int writes = s_set_configs;
    disconnect(WIFI_REASON_NO_AP_FOUND);
    CHECK(s_reconnects == 3);
    CHECK(s_set_configs == writes);
    printf("ok\n");
    return 0;
}
"""


def _extract(source: str) -> str:
    start = source.index("#if CONFIG_MUSE_ENABLED\n// A saved channel only")
    sig = source.index("static void event_handler(void *arg, esp_event_base_t base,", start)
    brace = source.index("{", sig)
    depth = 0
    for i in range(brace, len(source)):
        if source[i] == "{":
            depth += 1
        elif source[i] == "}":
            depth -= 1
            if depth == 0:
                return source[start : i + 1]
    raise AssertionError("could not find the end of event_handler")


class WifiReconnectUnpinTest(unittest.TestCase):
    def _run(self, muse: int) -> None:
        code = (f"#define CONFIG_MUSE_ENABLED {muse}\n" + PRELUDE
                + _extract(WIFI_MGR_C.read_text()) + HARNESS)
        with tempfile.TemporaryDirectory() as tmp:
            src = Path(tmp) / "wifi_reconnect.c"
            binary = Path(tmp) / "wifi_reconnect"
            src.write_text(code)
            built = subprocess.run(
                [*shlex.split(os.environ.get("CC", "cc")), "-std=c11", "-O1",
                 "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
                 "-Wno-unused-parameter",
                 "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                 str(src), "-o", str(binary)],
                capture_output=True, text=True)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            ran = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
            self.assertIn("ok", ran.stdout)

    def test_link_build_stops_pinning_a_vanished_ap(self) -> None:
        self._run(0)

    def test_muse_build_stops_pinning_a_vanished_ap(self) -> None:
        self._run(1)


if __name__ == "__main__":
    unittest.main()
