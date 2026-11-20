# Copyright (c) Meta Platforms, Inc. and affiliates.
# SPDX-License-Identifier: Apache-2.0
"""join_saved_networks()'s rejoin shortcut on the host: the production join
and wifi_mgr_has_cached_ap() against a fake radio."""

import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile
import unittest

from test_link_ota import function_source

ROOT = Path(__file__).resolve().parents[1]

HARNESS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define portENTER_CRITICAL(l) ((void)(l))
#define portEXIT_CRITICAL(l) ((void)(l))
#define pdMS_TO_TICKS(x) (x)
#define LED_STATE_WIFI_CONNECTING 0
#define WIFI_KNOWN_MAX 4
#define WIFI_KNOWN_SSID_MAX 32
#define MAX_CACHED_SCANS 25
enum { APP_WIFI_JOINED, APP_WIFI_NOT_NEARBY, APP_WIFI_FAILED, APP_WIFI_BUSY };
typedef int app_wifi_join_t;
typedef struct { char ssid[33]; char password[65]; bool hidden; } wifi_known_net_t;
typedef struct { int count; wifi_known_net_t nets[WIFI_KNOWN_MAX]; } wifi_known_list_t;
typedef struct { char ssid[33]; int8_t rssi; uint8_t auth_mode; } wifi_scan_entry_t;
typedef struct { char ssid[33]; uint8_t bssid[6]; int8_t rssi; uint8_t channel; } bssid_cache_entry_t;
typedef struct { uint8_t bssid[6]; uint8_t reason; int8_t rssi; } wifi_event_sta_disconnected_t;
static bssid_cache_entry_t s_bssid_cache[MAX_CACHED_SCANS];
static int s_bssid_cache_count, s_bssid_lock;
static int64_t s_now;
static bool s_ap_here, s_password_ok;
static int s_scans, s_connects, s_pinned_connects, s_scans_before_connect = -1;
static int8_t s_ap_rssi, s_drop_rssi;
static int64_t esp_timer_get_time(void) { return s_now; }
static void vTaskDelay(int ms) { s_now += ms * 1000LL; }
static void ui_set_status(const char *s) { (void)s; }
static void led_status_set_state(int s) { (void)s; }
static void restore_wifi_channel_hint(const char *s) { (void)s; }
static uint8_t saved_wifi_channel(const char *s) { (void)s; return 6; }
static bool wifi_mgr_is_connected(void) { return false; }
static void wifi_mgr_disconnect(void) {}
static int wifi_mgr_get_cached_scan(wifi_scan_entry_t *o, int m) { (void)o; (void)m; return 0; }
static void wifi_known_wipe(wifi_known_list_t *l) { memset(l, 0, sizeof(*l)); }
static bool wifi_known_remember(const char *s, const char *p, bool h) { (void)s; (void)p; (void)h; return true; }
static int wifi_known_load(wifi_known_list_t *l) {
    memset(l, 0, sizeof(*l));
    l->count = 1;
    strcpy(l->nets[0].ssid, "home");
    strcpy(l->nets[0].password, "pw");
    return 1;
}
static int wifi_mgr_scan(wifi_scan_entry_t *out, int max, uint8_t ch, const char *ssid) {
    (void)max; (void)ch; (void)ssid;
    s_scans++;
    s_now += 2000 * 1000LL;
    if (!s_ap_here) return 0;
    memset(out, 0, sizeof(*out));
    strcpy(out->ssid, "home");
    out->rssi = s_ap_rssi;
    return 1;
}
static bool wifi_mgr_connect(const char *ssid, const char *pass, int timeout_ms) {
    (void)ssid; (void)pass;
    if (s_scans_before_connect < 0) s_scans_before_connect = s_scans;
    s_connects++;
    bool pinned = s_bssid_cache_count > 0;
    s_pinned_connects += pinned;
    if (s_ap_here && s_password_ok) { s_now += 1000 * 1000LL; return true; }
    s_now += (int64_t)timeout_ms * 1000;
    if (pinned) s_bssid_cache_count = 0;   /* a miss drops the cache */
    return false;
}
@@WIFI_MGR@@
@@APP@@
static int run(int cached_rssi, bool here, int8_t rssi, bool password_ok) {
    s_now = 0; s_scans = s_connects = s_pinned_connects = 0; s_scans_before_connect = -1;
    s_ap_here = here; s_ap_rssi = rssi; s_password_ok = password_ok;
    s_bssid_cache_count = 1;
    strcpy(s_bssid_cache[0].ssid, "home");
    memcpy(s_bssid_cache[0].bssid, "\x02\x11\x22\x33\x44\x55", 6);
    s_bssid_cache[0].rssi = (int8_t)cached_rssi;
    if (s_drop_rssi) {   /* the link dropped from that AP at this signal */
        wifi_event_sta_disconnected_t ev = { .reason = 200, .rssi = s_drop_rssi };
        memcpy(ev.bssid, s_bssid_cache[0].bssid, 6);
        cache_disconnect_rssi(&ev);
    }
    char joined[WIFI_KNOWN_SSID_MAX + 1];
    return join_saved_networks(20000, JOIN_ANY, false, joined);
}
int main(int argc, char **argv) {
    switch (argc > 1 ? atoi(argv[1]) : -1) {
    case 0:   /* near cached AP: joined straight away, no probe first */
        assert(run(-60, true, -60, true) == APP_WIFI_JOINED);
        assert(s_scans_before_connect == 0 && s_connects == 1 && s_pinned_connects == 1);
        break;
    case 1:   /* weak cached AP (a far mesh node): probe first */
        assert(run(-80, true, -55, true) == APP_WIFI_JOINED);
        assert(s_scans_before_connect == 1);
        break;
    case 2:   /* away from home: the shortcut's miss is still "not nearby" */
        assert(run(-60, false, 0, true) == APP_WIFI_NOT_NEARBY);
        assert(s_pinned_connects == 1 && s_scans >= 1);
        break;
    case 3:   /* in range but the join fails: a failure */
        assert(run(-60, true, -60, false) == APP_WIFI_FAILED);
        break;
    case 4:   /* joined near, dropped far from it: probe first */
        s_drop_rssi = -84;
        assert(run(-60, true, -55, true) == APP_WIFI_JOINED);
        assert(s_scans_before_connect == 1);
        break;
    default: return 2;
    }
    return 0;
}
'''


class WifiRejoinShortcut(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        out = Path(cls.tmp.name)
        app = (ROOT / "main/app.c").read_text()
        wifi = (ROOT / "main/wifi_mgr.c").read_text()
        defines = "\n".join(re.findall(r"^#define WIFI_(?:JOIN_\w+|QUICK_MIN_RSSI|REJOIN_MS)\b.*$", app, re.M))
        join = app[app.index("static int64_t s_none_nearby_us;"):app.index("// A network the phone set up")]
        cache = function_source(wifi, "static void cache_disconnect_rssi(") + "\n" + function_source(
            wifi, "bool wifi_mgr_has_cached_ap(")
        code = HARNESS.replace("@@WIFI_MGR@@", cache).replace(
            "@@APP@@", defines + "\n" + join)
        source = out / "rejoin.c"
        source.write_text(code)
        cls.binary = out / "rejoin"
        command = [*shlex.split(os.environ.get("CC", "cc")), "-std=c11", "-Wall", "-Wextra", "-Werror",
                   "-Wno-unused-function", str(source), "-o", str(cls.binary)]
        compiled = subprocess.run(command, capture_output=True, text=True)
        if compiled.returncode:
            raise AssertionError(compiled.stdout + compiled.stderr)

    def run_case(self, case):
        result = subprocess.run([str(self.binary), str(case)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_near_cached_ap_joins_without_probe(self):
        self.run_case(0)

    def test_weak_cached_ap_probes_first(self):
        self.run_case(1)

    def test_miss_away_from_home_is_not_nearby(self):
        self.run_case(2)

    def test_failed_join_in_range_is_failure(self):
        self.run_case(3)

    def test_drop_far_from_cached_ap_probes_first(self):
        self.run_case(4)


if __name__ == "__main__":
    unittest.main()
