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

from __future__ import annotations

import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
WIFI_MGR_C = ROOT / "main" / "wifi_mgr.c"
APP_C = ROOT / "main" / "app.c"


def _function_body(source: str, signature: str) -> str:
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 0
    for i in range(brace, len(source)):
        if source[i] == "{":
            depth += 1
        elif source[i] == "}":
            depth -= 1
            if depth == 0:
                return source[brace + 1 : i]
    raise AssertionError(f"could not find end of {signature}")


class LinkWifiScanContractTest(unittest.TestCase):
    """The scan list is presented to the user one row per network: the firmware
    collapses the raw per-BSSID scan to one entry per SSID before it leaves the
    device, so a dual-band/mesh network isn't shown several times."""

    def test_scan_collapses_to_one_entry_per_ssid(self) -> None:
        body = _function_body(WIFI_MGR_C.read_text(), "int wifi_mgr_scan(")

        # Dedup by SSID name.
        self.assertIn("strcmp(out[j].ssid, ssid) == 0", body)
        # Drop hidden/blank SSIDs (useless in a picker).
        self.assertIn("if (ssid[0] == '\\0') continue;", body)
        # Keep the strongest BSSID's RSSI for the SSID.
        self.assertIn("if (recs[i].rssi > out[found].rssi)", body)
        # Mark secure if any BSSID of the SSID is secured.
        self.assertIn("if (out[found].auth_mode == 0)", body)
        # Cap unique SSIDs at the caller's buffer; return the unique count.
        self.assertIn("if (n >= max_entries) continue;", body)
        self.assertIn("return n;", body)

    def test_scan_relies_on_driver_rssi_order_no_manual_resort(self) -> None:
        body = _function_body(WIFI_MGR_C.read_text(), "int wifi_mgr_scan(")
        # esp_wifi_scan_get_ap_records returns RSSI-desc, so deduping in order
        # keeps the strongest BSSID first and the result stays RSSI-ordered —
        # no manual re-sort.
        self.assertNotIn("qsort(", body)

    def test_scan_retains_cached_bssids_missed_by_refresh(self) -> None:
        body = _function_body(WIFI_MGR_C.read_text(), "int wifi_mgr_scan(")

        self.assertIn("strcmp(s_bssid_cache[j].ssid, ssid) == 0", body)
        self.assertIn("cached = s_bssid_cache_count++", body)
        self.assertNotIn("s_bssid_cache_count =", body)

    def test_connect_preempts_background_scan_and_checks_start(self) -> None:
        source = WIFI_MGR_C.read_text()
        body = _function_body(source, "bool wifi_mgr_connect(")

        self.assertIn("s_connect_pending, true", body)
        self.assertIn("xSemaphoreTake(s_scan_mutex, 0)", body)
        self.assertIn("esp_wifi_scan_stop()", body)
        self.assertLess(
            body.index("s_connect_pending, true"),
            body.index("esp_wifi_scan_stop()"),
        )
        self.assertIn("err = esp_wifi_connect()", body)
        self.assertIn('"wifi connect start failed: %s"', body)
        self.assertIn("xSemaphoreGive(s_scan_mutex)", body)
        # Preserve main's existing retry policy; this PR only arbitrates scans.
        self.assertIn("#define MAX_INITIAL_RETRY        3", source)

    def test_scan_yields_to_pending_connect(self) -> None:
        body = _function_body(WIFI_MGR_C.read_text(), "int wifi_mgr_scan(")

        self.assertGreaterEqual(body.count("s_connect_pending"), 2)
        self.assertLess(
            body.index("s_connect_pending"),
            body.index("esp_wifi_scan_start"),
        )
        self.assertIn("scan cancelled for pending wifi connect", body)
        self.assertIn("esp_wifi_clear_ap_list()", body)

    def test_cancelled_refresh_does_not_republish_old_cache(self) -> None:
        source = WIFI_MGR_C.read_text()
        merge = _function_body(source, "int wifi_mgr_scan_and_merge_cache(")

        self.assertIn("if (n <= 0) return 0", merge)

    def test_saved_channel_hint_uses_fast_scan_without_pinning_bssid(self) -> None:
        source = WIFI_MGR_C.read_text()
        valid = _function_body(source, "static bool channel_hint_valid(")
        seed = _function_body(source, "bool wifi_mgr_seed_channel_hint(")
        connect = _function_body(source, "bool wifi_mgr_connect(")
        channel = _function_body(source, "bool wifi_mgr_get_connected_channel(")

        self.assertIn("channel >= 1 && channel <= 13", valid)
        self.assertIn("channel >= 36 && channel <= 64", valid)
        self.assertIn("channel >= 100 && channel <= 144", valid)
        self.assertIn("channel >= 149 && channel <= 177", valid)
        self.assertIn("!channel_hint_valid(channel)", seed)
        self.assertIn("s_channel_hint.channel = channel", seed)
        self.assertIn("!have_bssid && s_channel_hint.channel != 0", connect)
        self.assertIn("wc.sta.channel = s_channel_hint.channel", connect)
        self.assertIn("have_bssid || have_channel_hint", connect)
        hint_branch = connect[
            connect.index("bool have_channel_hint") :
            connect.index("if (have_bssid || have_channel_hint)")
        ]
        self.assertNotIn("bssid_set", hint_branch)
        self.assertIn("esp_wifi_sta_get_ap_info", channel)
        self.assertIn("ap.primary", channel)

    def test_provisioned_boot_skips_only_the_explicit_picker_scan(self) -> None:
        source = APP_C.read_text()
        body = _function_body(source, "void app_run(")
        restore = _function_body(source, "static bool restore_wifi_channel_hint(")
        persist = _function_body(source, "static void persist_connected_wifi_channel(")

        state = body.index("bool setup_complete = config_setup_complete()")
        scan_guard = body.index("if (!skip_boot_scan)")
        scan = body.index("wifi_mgr_scan_and_cache()")
        self.assertLess(state, scan_guard)
        self.assertLess(scan_guard, scan)
        self.assertIn("setup_complete && !CONFIG_HOMEHUB_WIFI_SSID[0]", body)
        self.assertIn('config_key_lookup("password") == CONFIG_KEY_FOUND', body)
        self.assertIn("restore_wifi_channel_hint(hint_ssid)", body)
        self.assertIn('config_get_str(WIFI_CHANNEL_KEY', restore)
        self.assertIn("wifi_mgr_seed_channel_hint", restore)
        self.assertGreaterEqual(restore.count("config_erase_key(WIFI_CHANNEL_KEY)"), 2)
        self.assertIn("wifi_mgr_get_connected_channel", persist)
        self.assertIn("strcmp(current, value) == 0", persist)
        self.assertGreaterEqual(body.count("persist_connected_wifi_channel()"), 1)

    def test_unpaired_boot_and_mobile_share_one_background_scan(self) -> None:
        source = APP_C.read_text()
        body = _function_body(source, "void app_run(")
        schedule = _function_body(source, "static bool schedule_scan_refresh(")
        task = _function_body(source, "static void scan_refresh_task(")
        on_scan = _function_body(source, "static void on_wifi_scan(")
        disconnected = _function_body(source, "static void on_client_disconnected(")

        unpaired = body[body.rindex("if (!setup_complete) {") :]
        self.assertIn("schedule_scan_refresh(false, NULL)", unpaired)
        self.assertLess(
            unpaired.index('open_setup_window("boot: unpaired")'),
            unpaired.index("schedule_scan_refresh(false, NULL)"),
        )
        self.assertIn("atomic_fetch_or_explicit", schedule)
        self.assertIn("SCAN_REFRESH_RUNNING", schedule)
        self.assertIn("SCAN_REFRESH_REPLY_PENDING", schedule)
        self.assertIn("previous_flags & SCAN_REFRESH_RUNNING", schedule)
        self.assertEqual(schedule.count("helper_task_create("), 1)
        self.assertIn(
            "atomic_store_explicit(&s_scan_refresh_flags, 0",
            schedule,
        )

        self.assertLess(
            on_scan.index("schedule_scan_refresh(true, &joined)"),
            on_scan.rindex("send_cached_scan_or_error()"),
        )
        self.assertIn("if (!joined)", on_scan)
        self.assertIn("if (joined) *joined = already_running", schedule)
        self.assertIn("atomic_exchange_explicit", task)
        self.assertIn("completed_flags & SCAN_REFRESH_REPLY_PENDING", task)
        self.assertIn("wifi_mgr_scan_and_merge_cache()", task)
        self.assertIn("~SCAN_REFRESH_REPLY_PENDING", disconnected)

    def test_disconnect_logs_reason_and_signal_only(self) -> None:
        body = _function_body(WIFI_MGR_C.read_text(), "static void event_handler(")

        self.assertIn("wifi_event_sta_disconnected_t", body)
        self.assertIn("ev ? ev->reason : 0", body)
        self.assertIn("ev ? ev->rssi : 0", body)


if __name__ == "__main__":
    unittest.main()
