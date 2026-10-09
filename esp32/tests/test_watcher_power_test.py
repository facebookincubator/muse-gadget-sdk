# Copyright (c) Meta Platforms, Inc. and affiliates.
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy at http://www.apache.org/licenses/LICENSE-2.0
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""Execute the real firmware parser/FSM against deterministic C fakes.

No IDF, serial ports, hardware, credentials, or third-party Python packages.
This verifies orchestration/error/persistence semantics, NOT electrical loads.
"""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "components/muse/muse_watcher_power_test.c"


def arm(**changes):
    values = dict(run_id="bench-1", settle_ms=1000, capture_ms=1000, repeats=1)
    values.update(changes)
    return ">ptest.arm=" + json.dumps(values)


class WatcherPowerTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix="watcher-power-test-")
        cls.binary = Path(cls.tmp.name) / "harness"
        subprocess.run(
            [os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
             "-I", str(ROOT / "tests/watcher_power_test_fakes"),
             str(ROOT / "tests/watcher_power_test_harness.c"), "-o", str(cls.binary)],
            check=True, capture_output=True, text=True,
        )

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def run_commands(self, *commands):
        proc = subprocess.run(
            [str(self.binary)], input="\n".join(commands) + "\n",
            capture_output=True, text=True, check=True, timeout=10,
        )
        self.assertEqual(proc.stderr, "")
        rows = []
        for line in proc.stdout.splitlines():
            self.assertTrue(line.startswith("PTEST "), line)
            self.assertLess(len(line), 128 * 1024)
            rows.append(json.loads(line[6:]))
        return rows

    def complete(self, repeats=1, *extra):
        return self.run_commands(
            arm(repeats=repeats), "usb 0", "advance 180000", *extra,
            ">ptest.results", "stats",
        )

    def test_status_has_live_power_clock_and_boot(self):
        before, after = self.run_commands(
            ">ptest.status", "usb 0", "advance 100", ">ptest.status",
        )
        self.assertEqual(before["type"], "status")
        self.assertEqual(before["schema"], 1)
        self.assertEqual(before["state"], "idle")
        self.assertTrue(before["usb"])
        self.assertFalse(after["usb"])
        self.assertEqual(before["boot_id"], after["boot_id"])
        self.assertGreater(after["now_us"], before["now_us"])

    def test_status_nonce_echo_and_rejection(self):
        plain, first, second = self.run_commands(
            ">ptest.status", ">ptest.status=0123456789abcdef", ">ptest.status=other_ID-1",
        )
        self.assertEqual(plain["request_id"], "")
        self.assertEqual(first["request_id"], "0123456789abcdef")
        self.assertEqual(second["request_id"], "other_ID-1")
        for nonce in ["", "x" * 33, 'quote"', "space value", "back\\\\slash"]:
            with self.subTest(nonce=nonce):
                self.assertEqual(self.run_commands(">ptest.status=" + nonce)[0]["type"], "error")

    def test_capture_boundaries_reject_usb_seen_only_by_readback(self):
        for boundary in [1, 2, 3]:  # apply, capture start, capture end
            with self.subTest(boundary=boundary):
                _, result = self.run_commands(
                    f"readback_usb 0 {boundary}", arm(), "usb 0", "advance 180000", ">ptest.results",
                )
                self.assertEqual(result["state"], "aborted")
                self.assertFalse(result["complete"])
                self.assertEqual(len(result["records"]), 1)
                self.assertFalse(result["records"][0]["valid"])
                self.assertEqual(result["records"][0]["status"], "aborted")

    def test_configured_auto_sleep_without_observed_sleep_is_explicit_skip(self):
        for index in [0, 5, 26]:
            with self.subTest(index=index):
                _, result = self.run_commands(
                    f"no_sleep {index}", arm(), "usb 0", "advance 180000", ">ptest.results",
                )
                self.assertEqual(result["state"], "complete")
                record = result["records"][index]
                self.assertEqual(record["status"], "skipped")
                self.assertFalse(record["valid"])
                self.assertEqual(record["reason"], "automatic_light_sleep_not_observed")
                self.assertEqual(record["actual"]["slept_us"], 0)
                self.assertEqual(record["actual"]["sleep_count"], 0)
                self.assertGreater(record["measure_start_us"], 0)

    def test_partial_audio_ownership_and_pwm_readback_contract(self):
        board = (ROOT / "components/muse/boards/board_sensecap_watcher.c").read_text()
        audio = board.split("static esp_err_t audio_init(", 1)[1].split("/* The wheel's push", 1)[0]
        self.assertLess(audio.index("i2s_new_channel"), audio.index("s_ptest_tx = tx"))
        self.assertLess(audio.index("s_ptest_tx = tx"), audio.index("i2s_channel_init_std_mode(tx"))
        self.assertLess(audio.index("s_ptest_rx = rx"), audio.index("i2s_channel_enable(tx"))
        close = board.split("static esp_err_t ptest_audio_close(void)", 1)[1].split("static esp_err_t ptest_audio_open", 1)[0]
        self.assertIn("s_ptest_tx, s_ptest_rx", close)
        self.assertIn("i2s_channel_disable(channels[i])", close)
        self.assertIn("out->backlight_duty == s_ptest_expected_duty", board)

    def test_arm_ack_plan_and_bounds(self):
        ack = self.run_commands(arm(settle_ms=5000, capture_ms=20000))[0]
        self.assertEqual(ack["type"], "arm_ack")
        self.assertTrue(ack["usb"])
        self.assertEqual(ack["settle_ms"], 5000)
        self.assertEqual(ack["capture_ms"], 20000)
        self.assertEqual(ack["gate_poll_ms"], 100)
        self.assertEqual(ack["usb_abort_poll_ms"], 1000)
        self.assertLessEqual(ack["max_duration_ms"], 3600000)
        self.assertLessEqual(ack["rtc_bytes"], 7680)
        self.assertEqual(ack["plan"][0]["id"], "baseline_pre")
        self.assertEqual(ack["plan"][-1]["id"], "baseline_post")
        self.assertEqual(sum(p["timed"] for p in ack["plan"]), 23)
        self.assertEqual(len(ack["plan"]), 27)

    def test_arm_waits_actual_vbus_not_command_or_time(self):
        ack, waiting, running = self.run_commands(
            arm(), "advance 200000", ">ptest.status",
            "usb 0", "advance 100", ">ptest.status",
        )
        self.assertEqual(waiting["state"], "armed")
        self.assertEqual(waiting["record_count"], 0)
        self.assertEqual(waiting["sequence_start_us"], 0)
        self.assertEqual(running["state"], "running")
        self.assertGreater(running["sequence_start_us"], ack["ack_us"])

    def test_arm_requires_usb_and_working_expander(self):
        for commands in [
            ("usb 0", arm()),
            ("power_error 263", arm()),
            ("init_error 257", arm()),
        ]:
            with self.subTest(commands=commands):
                response = self.run_commands(*commands)[-1]
                self.assertEqual(response["type"], "error")
                self.assertNotEqual(response["error"], 0)

    def test_parser_arbitrary_order_whitespace(self):
        response = self.run_commands(
            '>ptest.arm= { "capture_ms" : 1000, "repeats": 1, "run_id":"safe_ID-1", "settle_ms":1000 }'
        )[0]
        self.assertEqual(response["type"], "arm_ack")
        self.assertEqual(response["run_id"], "safe_ID-1")

    def test_parser_rejects_unsafe_or_ambiguous_input(self):
        malformed = [
            "{}", "[]", "", '{"run_id":"x"}',
            '{"run_id":"x","settle_ms":1000,"capture_ms":1000,"repeats":1,"repeats":1}',
            '{"run_id":"x","settle_ms":1000,"capture_ms":1000,"repeats":1,"unknown":1}',
            '{"run_id":"x","settle_ms":01000,"capture_ms":1000,"repeats":1}',
            '{"run_id":"x","settle_ms":1000.0,"capture_ms":1000,"repeats":1}',
            '{"run_id":"x","settle_ms":-1000,"capture_ms":1000,"repeats":1}',
            '{"run_id":"x","settle_ms":4294967296,"capture_ms":1000,"repeats":1}',
            '{"run_id":"x","settle_ms":1000,"capture_ms":1000,"repeats":1}junk',
            '{"run_id":"quote\\\"x","settle_ms":1000,"capture_ms":1000,"repeats":1}',
            '{"run_id":"x y","settle_ms":1000,"capture_ms":1000,"repeats":1}',
        ]
        for payload in malformed:
            with self.subTest(payload=payload):
                self.assertEqual(self.run_commands(">ptest.arm=" + payload)[0]["type"], "error")
        for changes in [dict(run_id=""), dict(run_id="x" * 33), dict(settle_ms=999),
                        dict(capture_ms=999), dict(settle_ms=60001), dict(capture_ms=120001),
                        dict(repeats=0), dict(repeats=3),
                        dict(settle_ms=60000, capture_ms=120000, repeats=2)]:
            with self.subTest(changes=changes):
                self.assertEqual(self.run_commands(arm(**changes))[0]["type"], "error")

    def test_unknown_command_and_double_arm_rejected(self):
        unknown, ack, second = self.run_commands(">ptest.nonsense", arm(), arm(run_id="different"))
        self.assertEqual(unknown["type"], "error")
        self.assertEqual(ack["type"], "arm_ack")
        self.assertEqual(second["message"], "already_active")

    def test_exact_order_repeats_and_skip_truth(self):
        ack, result, stats = self.complete(repeats=2)
        self.assertTrue(result["complete"])
        self.assertEqual(result["state"], "complete")
        self.assertEqual(len(result["records"]), 54)
        expected = [(p["id"], repeat) for repeat in range(2) for p in ack["plan"]]
        self.assertEqual([(r["name"], r["repeat"]) for r in result["records"]], expected)
        for record in result["records"]:
            if ack["plan"][record["index"]]["timed"]:
                self.assertEqual(record["status"], "ok")
                self.assertTrue(record["valid"])
                self.assertEqual(record["apply_err"], 0)
            else:
                self.assertEqual(record["status"], "skipped")
                self.assertFalse(record["valid"])
                self.assertNotEqual(record["error"], 0)
                self.assertEqual(record["measure_start_us"], 0)
                self.assertEqual(record["enter_us"], record["end_us"])
        self.assertEqual(stats["active"], 26)

    def test_actual_timestamps_not_nominal_host_schedule(self):
        ack, result, _ = self.complete()
        self.assertLessEqual(result["last_usb_present_us"], result["vbus_removed_us"])
        self.assertEqual(result["vbus_removed_us"], result["sequence_start_us"])
        previous = result["sequence_start_us"]
        for record in result["records"]:
            self.assertGreaterEqual(record["enter_us"], previous)
            if record["valid"]:
                self.assertEqual(record["applied_us"] - record["enter_us"], 25000)
                self.assertGreaterEqual(record["measure_start_us"] - record["applied_us"], ack["settle_ms"] * 1000)
                self.assertGreaterEqual(record["end_us"] - record["measure_start_us"], ack["capture_ms"] * 1000)
            previous = record["end_us"]
        self.assertLess(result["finished_us"] - result["sequence_start_us"], ack["max_duration_ms"] * 1000)

    def test_readback_and_observed_work_preserved(self):
        _, result, _ = self.complete()
        records = {r["name"]: r for r in result["records"]}
        for frequency in [40, 80, 160, 240]:
            actual = records[f"cpu_fixed_{frequency}_idle"]["actual"]
            self.assertEqual(actual["cpu_readback_mhz"], frequency)
            self.assertEqual(actual["pm_min_mhz"], frequency)
            self.assertFalse(actual["auto_light_sleep"])
        self.assertGreater(records["baseline_post"]["actual"]["slept_us"], 0)
        self.assertGreater(records["mic_capture_16k"]["actual"]["io_frames"], 0)
        self.assertGreater(records["sine_1k_minus18dbfs"]["actual"]["io_frames"], 0)
        self.assertEqual(records["sine_1k_minus18dbfs"]["actual"]["configured_volume"], 25)
        self.assertGreater(records["adc_sample_1hz"]["actual"]["adc_samples"], 0)
        self.assertGreater(records["wifi_scanning"]["actual"]["wifi_scans"], 0)
        self.assertEqual(records["adc_divider_rail_on"]["actual"]["adc_samples"], 0)

    def test_usb_reconnect_aborts_active_window_and_restores(self):
        ack, result, stats = self.run_commands(
            arm(), "usb 0", "advance 4000", "usb 1", "advance 1200", ">ptest.results", "stats",
        )
        self.assertEqual(result["state"], "aborted")
        self.assertFalse(result["complete"])
        self.assertTrue(result["usb"])
        self.assertTrue(any(r["valid"] for r in result["records"]))
        self.assertEqual(result["records"][-1]["status"], "aborted")
        self.assertFalse(result["records"][-1]["valid"])
        self.assertEqual(stats["active"], len(ack["plan"]) - 1)

    def test_power_read_failure_is_not_usb_absence(self):
        _, result = self.run_commands(
            arm(), "power_error 263", "advance 1000", ">ptest.results",
        )
        self.assertEqual(result["state"], "error")
        self.assertEqual(result["records"], [])
        self.assertEqual(result["sequence_start_us"], 0)
        self.assertEqual(result["error"], 263)

    def test_apply_readback_and_service_errors_are_not_passes(self):
        for injection in ["apply_error 7 263", "readback_error 7 263", "service_error 7 263"]:
            with self.subTest(injection=injection):
                _, result, stats = self.run_commands(
                    injection, arm(), "usb 0", "advance 180000", ">ptest.results", "stats",
                )
                self.assertEqual(result["state"], "error")
                failed = result["records"][-1]
                self.assertEqual(failed["name"], "lcd_black_bl_0")
                self.assertEqual(failed["status"], "error")
                self.assertEqual(failed["error"], 263)
                self.assertFalse(failed["valid"])
                self.assertEqual(stats["active"], 26)

    def test_runtime_unsupported_skips_and_restores_before_continuing(self):
        _, result = self.run_commands(
            "apply_error 7 262", arm(), "usb 0", "advance 180000", ">ptest.results",
        )
        self.assertEqual(result["state"], "complete")
        failed = result["records"][7]
        self.assertEqual(failed["status"], "skipped")
        self.assertFalse(failed["valid"])
        self.assertEqual(failed["cleanup_error"], 0)
        self.assertEqual(result["records"][8]["status"], "ok")

    def test_configured_work_without_observed_io_is_not_passed(self):
        for index in [15, 16, 19, 21]:
            with self.subTest(index=index):
                _, result = self.run_commands(
                    f"empty_work {index}", arm(), "usb 0", "advance 180000", ">ptest.results",
                )
                self.assertEqual(result["state"], "error")
                record = result["records"][-1]
                self.assertEqual(record["index"], index)
                self.assertEqual(record["status"], "error")
                self.assertFalse(record["valid"])
                self.assertNotEqual(record["error"], 0)

    def test_restore_failure_prevents_complete(self):
        _, result = self.run_commands(
            "restore_error 263", arm(), "usb 0", "advance 180000", ">ptest.results",
        )
        self.assertEqual(result["state"], "error")
        self.assertEqual(result["restore_error"], 263)
        self.assertFalse(result["complete"])

    def test_completed_rtc_results_survive_reconnect_reset(self):
        rows = self.run_commands(
            arm(), "usb 0", "advance 180000", ">ptest.results",
            "usb 1", "reboot", ">ptest.status", ">ptest.results",
        )
        before, status, after = rows[-3:]
        self.assertTrue(status["restored_from_rtc"])
        self.assertTrue(status["usb"])
        self.assertEqual(status["boot_id"], 222)
        self.assertEqual(status["run_boot_id"], 111)
        self.assertEqual(after["run_boot_id"], before["run_boot_id"])
        self.assertEqual(after["retrieval_boot_id"], 222)
        self.assertEqual(after["state"], "complete")
        self.assertEqual(after["records"], before["records"])
        self.assertLess(after["now_us"], after["finished_us"])

    def test_interrupted_rtc_run_never_resumes_and_keeps_completed_windows(self):
        _, result, status = self.run_commands(
            arm(), "usb 0", "advance 4000", "reboot", "advance 180000",
            ">ptest.results", ">ptest.status",
        )
        self.assertEqual(result["state"], "incomplete_reboot")
        self.assertFalse(result["complete"])
        self.assertTrue(result["records"][0]["valid"])
        self.assertFalse(result["records"][-1]["valid"])
        self.assertEqual(result["records"][-1]["end_us"], 0)
        self.assertEqual(status["state"], "incomplete_reboot")

    def test_rtc_checksum_corruption_discards_run(self):
        _, status, result = self.run_commands(arm(), "corrupt", "reboot", ">ptest.status", ">ptest.results")
        self.assertEqual(status["state"], "idle")
        self.assertFalse(status["restored_from_rtc"])
        self.assertEqual(result["records"], [])
        self.assertFalse(result["complete"])

    def test_abort_command_before_unplug_disarms(self):
        _, response, result = self.run_commands(arm(), ">ptest.abort", "usb 0", "advance 180000", ">ptest.results")
        self.assertEqual(response["type"], "abort_ack")
        self.assertEqual(result["state"], "aborted")
        self.assertEqual(result["records"], [])

    def test_no_continuous_logs_or_adc_in_core_gate(self):
        rows = self.run_commands(arm(), "usb 0", "advance 180000")
        self.assertEqual(len(rows), 1)  # ACK only, no periodic UART during sweep.
        source = SOURCE.read_text()
        self.assertNotIn("nvs_open", source)
        self.assertNotIn("app_run()", source)
        self.assertNotIn("muse_glue_start()", source)
        self.assertNotIn("esp_deep_sleep_start", source)
        self.assertIn("cfg.nvs_enable = false", source)
        self.assertIn("ESP_PM_NO_LIGHT_SLEEP", source)
        self.assertIn("s_usb_lock_held", source)
        self.assertIn("uart_read_bytes", source)
        main = (ROOT / "main/main.c").read_text()
        self.assertLess(main.index("muse_watcher_power_test_run();"), main.index("diagnostic_log_init()"))
        self.assertLess(main.index("muse_watcher_power_test_run();"), main.index("muse_glue_start();"))
        config = (ROOT / "components/muse/Kconfig").read_text().split("config MUSE_WATCHER_POWER_TEST", 1)[1].split("config MUSE_WATCHER_CAMERA", 1)[0]
        self.assertIn("default n", config)
        self.assertIn("MUSE_BOARD_SENSECAP_WATCHER", config)


if __name__ == "__main__":
    unittest.main()
