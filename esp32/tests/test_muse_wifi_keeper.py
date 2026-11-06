# Copyright (c) Meta Platforms, Inc. and affiliates.
# SPDX-License-Identifier: Apache-2.0
"""Run the production keeper through Wi-Fi stop/resume transitions on the host."""

import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

from test_link_ota import function_source

ROOT = Path(__file__).resolve().parents[1]


class MuseWifiKeeper(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        out = Path(cls.tmp.name)
        glue = (ROOT / "main/muse_glue.c").read_text()
        wifi = (ROOT / "main/wifi_mgr.c").read_text()
        constants = glue[glue.index("#define CONNECT_TIMEOUT_MS"):glue.index("_Static_assert")]
        code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <setjmp.h>
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define pdFALSE 0
#define pdTRUE 1
#define pdMS_TO_TICKS(x) (x)
#define portMAX_DELAY -1
#define BIT_LINK 1
#define BIT_MUSE 2
#define KEEP_BLE 1
#define KEEP_SAVE 2
#define KEEP_FORGET 4
#define KEEP_RELOAD 8
#define KEEP_WIFI 16
#define STACK_MONITOR_INIT 0
#define BIT_CONNECTED 1
#define BIT_GOT_IP 2
typedef int stack_monitor_t;
typedef void *TimerHandle_t;
enum { APP_WIFI_JOINED, APP_WIFI_NOT_NEARBY, APP_WIFI_FAILED, APP_WIFI_BUSY };
typedef int app_wifi_join_t;
static int s_ready, s_events;
static uint32_t s_creds_gen;
static bool s_keep_connected, s_inited = true;
static atomic_bool s_sta_idle;
static bool wifi_on, connected, timer_pending, gate_busy;
static bool s_away, s_nap, s_chosen, s_joining, s_linked, s_editing;
static char s_ssid[33];
static TimerHandle_t s_reconnect_timer = (void *)1;
static int disconnects, connects, joins;
static jmp_buf tick_end;
typedef struct {
    bool on, saved, connected, timer, nap, busy;
} tick_t;
static tick_t ticks[4];
static unsigned nticks, at;

static void xEventGroupWaitBits(int a, int b, int c, int d, int e) {
    (void)a; (void)b; (void)c; (void)d; (void)e;
}
static void load_creds(void) {}
static void reload_creds(uint32_t *g) { (void)g; }
static void save_saved(uint32_t p, int64_t *t) { (void)p; (void)t; }
static bool config_setup_complete(void) { return false; }
static bool config_is_provisioned(void) { return true; }
static bool muse_settings_ble_on(void) { return false; }
static void muse_settings_set_ble_on(bool b) { (void)b; }
static bool muse_settings_wifi_on(void) { return wifi_on; }
static void app_ble_companion_set(bool b) { (void)b; }
static bool none_nearby_lately(void) { return false; }
static bool wifi_mgr_is_connected(void) { return connected; }
static int xTimerStop(TimerHandle_t t, int w) {
    (void)t; (void)w; timer_pending = false; return 1;
}
static void esp_wifi_disconnect(void) { disconnects++; connected = false; }
static void xEventGroupClearBits(int a, int b) { (void)a; (void)b; }
static void esp_wifi_connect(void) { connects++; }
''' + function_source(wifi, "void wifi_mgr_disconnect(") + "\n" + function_source(
            wifi, "static void reconnect_timer_cb("
        ) + r'''
static bool app_wifi_nap(void) {
    if (gate_busy) return false;
    wifi_mgr_disconnect(); return true;
}
static bool muse_state_asleep(void) { return false; }
static int64_t esp_timer_get_time(void) { return (at + 10LL) * 1000000; }
static app_wifi_join_t app_wifi_join_saved(int timeout, bool first) {
    (void)timeout; (void)first;
    if (gate_busy) return APP_WIFI_BUSY;
    joins++;
    // join_saved_networks cancels wifi_mgr's reconnects before scanning.
    wifi_mgr_disconnect();
    connected = true;
    return APP_WIFI_JOINED;
}
static void set_fail(const char *s) { (void)s; }
static void stack_monitor_poll(stack_monitor_t *s) { (void)s; }
static void apply_tick(void) {
    tick_t *t = &ticks[at];
    wifi_on = t->on; s_ssid[0] = t->saved ? 's' : '\0';
    connected = t->connected; timer_pending = t->timer;
    s_nap = t->nap; gate_busy = t->busy;
    s_keep_connected = t->timer || t->connected;
}
static void xTaskNotifyWait(int a, unsigned b, uint32_t *pending, int wait) {
    (void)a; (void)b; (void)wait;
    // A scheduled callback runs after the keeper has observed this tick.
    if (timer_pending) {
        timer_pending = false;
        reconnect_timer_cb(s_reconnect_timer);
    }
    if (++at == nticks) longjmp(tick_end, 1);
    apply_tick();
    *pending = KEEP_WIFI;
}
''' + constants + function_source(glue, "static void keeper_task(") + r'''
static void run(void) {
    apply_tick();
    if (!setjmp(tick_end)) keeper_task(NULL);
}
int main(int argc, char **argv) {
    assert(argc == 2);
    switch (atoi(argv[1])) {
    case 0: // OFF with no IP must still cancel the old retry.
        ticks[0] = (tick_t){ .saved = true, .timer = true };
        nticks = 1; run();
        assert(disconnects == 1 && connects == 0 && joins == 0);
        break;
    case 1: // Forgetting the last network while disconnected has the same rule.
        ticks[0] = (tick_t){ .on = true, .timer = true };
        nticks = 1; run();
        assert(disconnects == 1 && connects == 0 && joins == 0);
        break;
    case 2: // Stable OFF must not call the driver every keeper tick.
        ticks[0] = ticks[1] = ticks[2] = (tick_t){ .saved = true };
        nticks = 3; run();
        assert(disconnects == 1 && connects == 0 && joins == 0);
        break;
    case 3: // An association that finishes late while OFF is disconnected too.
        ticks[0] = (tick_t){ .saved = true };
        ticks[1] = (tick_t){ .saved = true, .connected = true };
        nticks = 2; run();
        assert(disconnects == 2 && !connected && connects == 0);
        break;
    case 4: // ON again rejoins; the next OFF transition cancels a new retry.
        ticks[0] = (tick_t){ .saved = true, .timer = true };
        ticks[1] = (tick_t){ .on = true, .saved = true };
        ticks[2] = (tick_t){ .saved = true, .timer = true };
        nticks = 3; run();
        assert(disconnects == 3 && connects == 0 && joins == 1);
        break;
    case 5: // A connected ON network needs no stop or reconnect.
        ticks[0] = ticks[1] = (tick_t){ .on = true, .saved = true, .connected = true };
        nticks = 2; run();
        assert(disconnects == 0 && connects == 0 && joins == 0 && connected);
        break;
    case 6: // Nap still stops once; a wake resumes the normal join path.
        ticks[0] = (tick_t){ .on = true, .saved = true, .timer = true, .nap = true };
        ticks[1] = (tick_t){ .on = true, .saved = true, .nap = true };
        ticks[2] = (tick_t){ .on = true, .saved = true };
        nticks = 3; run();
        assert(disconnects == 2 && connects == 0 && joins == 1 && connected);
        break;
    case 7: // OFF retries cancellation once the setup operation releases the gate.
        ticks[0] = (tick_t){ .on = true, .saved = true, .busy = true };
        ticks[1] = (tick_t){ .saved = true, .busy = true };
        ticks[2] = (tick_t){ .saved = true, .timer = true };
        nticks = 3; run();
        assert(disconnects == 1 && connects == 0 && joins == 0);
        break;
    case 8: // An initial provisioning join must not be disconnected while busy.
        ticks[0] = (tick_t){ .saved = true, .busy = true };
        nticks = 1; run();
        assert(disconnects == 0 && connects == 0 && joins == 0);
        break;
    default: return 2;
    }
    return 0;
}
'''
        source = out / "keeper.c"
        source.write_text(code)
        cls.binary = out / "keeper"
        command = [*shlex.split(os.environ.get("CC", "cc")), "-std=c11", "-Wall",
                   "-Wextra", "-Werror", "-Wno-unused-parameter", "-I", str(out),
                   str(source), "-o", str(cls.binary)]
        compiled = subprocess.run(command, capture_output=True, text=True)
        if compiled.returncode:
            raise AssertionError(compiled.stdout + compiled.stderr)

    def run_case(self, case):
        result = subprocess.run([str(self.binary), str(case)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_off_cancels_pending_reconnect_without_ip(self):
        self.run_case(0)

    def test_no_saved_network_cancels_pending_reconnect(self):
        self.run_case(1)

    def test_stable_off_stops_only_once(self):
        self.run_case(2)

    def test_late_association_while_off_is_disconnected(self):
        self.run_case(3)

    def test_on_rejoins_and_next_off_cancels_again(self):
        self.run_case(4)

    def test_connected_on_is_unchanged(self):
        self.run_case(5)

    def test_nap_and_wake_still_work(self):
        self.run_case(6)

    def test_off_cancellation_retries_after_setup_releases_gate(self):
        self.run_case(7)

    def test_off_does_not_interrupt_setup_while_gate_is_busy(self):
        self.run_case(8)


if __name__ == "__main__":
    unittest.main()
