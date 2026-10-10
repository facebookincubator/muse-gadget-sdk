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
from power_test_fixture import prepare_board_matrix

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "components/muse/muse_power_test.c"


def arm(**changes):
    values = dict(run_id="bench-1", settle_ms=1000, capture_ms=1000, repeats=1)
    values.update(changes)
    return ">ptest.arm=" + json.dumps(values)


class PowerTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix="power-test-")
        cls.binary = Path(cls.tmp.name) / "harness"
        prepare_board_matrix(cls.tmp.name, ROOT)
        subprocess.run(
            [os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
             "-I", str(ROOT / "tests/power_test_fakes"),
             "-I", str(ROOT / "components/muse"), "-I", cls.tmp.name,
             str(ROOT / "tests/power_test_harness.c"), "-o", str(cls.binary)],
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

    def test_board_identity_is_additive_in_status_ack_and_results(self):
        status, ack, results = self.run_commands(
            ">ptest.status", arm(), "usb 0", "advance 180000", ">ptest.results",
        )
        for frame in (status, ack, results):
            self.assertEqual(frame["board"], "Seeed SenseCAP Watcher")
            self.assertEqual(frame["schema"], 1)
        self.assertEqual(ack["rtc_bytes"], 7680)

    def test_core_uses_other_board_matrix_and_rejects_unavailable_matrix(self):
        ack, results = self.run_commands(
            "generic_board 1", arm(), "usb 0", "advance 180000", ">ptest.results",
        )
        self.assertEqual(ack["board"], "Generic test board")
        self.assertEqual([p["id"] for p in ack["plan"]], ["other_board_ref", "other_board_rest"])
        self.assertEqual([r["name"] for r in results["records"]], ["other_board_ref", "other_board_rest"])
        self.assertTrue(results["complete"])
        error = self.run_commands("generic_board 1", arm(matrix="sleep"))[0]
        self.assertEqual(error["type"], "error")
        self.assertIn("matrix_unavailable", error["message"])
        error = self.run_commands("generic_board 1", '>ptest.dryrun={"matrix":"sleep"}')[0]
        self.assertEqual(error["message"], "matrix_unavailable")

    def test_board_name_is_json_escaped_and_unknown_board_matrix_is_null(self):
        frame = self.run_commands("generic_board 2", ">ptest.status")[0]
        self.assertEqual(frame["board"], 'Test "quoted" board\\name')
        unknown = self.run_commands("matrix_unknown")[0]
        self.assertEqual(unknown["count"], 0)
        self.assertFalse(unknown["supported"])

    def test_watcher_descriptor_block_is_unchanged(self):
        import hashlib
        board = (ROOT / "components/muse/boards/board_sensecap_watcher.c").read_text()
        start = board.index("#define STATE(")
        end = board.index("_Static_assert(SLEEP_MATRIX_COUNT", start)
        # Exact descriptor block from the pre-refactor 09dc31d tree: IDs,
        # notes, ordering, loads and every knob/role/poll field stay identical.
        self.assertEqual(hashlib.sha256(board[start:end].encode()).hexdigest(),
                         "eafd80cb3432294891d5689600d96fcff686be4873cae96111cf3c7d483a8ac8")
        source = SOURCE.read_text()
        self.assertNotIn("board_sensecap_watcher", source)
        self.assertNotIn("s_sleep_matrix", source)
        self.assertIn("muse_ptest_board_matrix", source)

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
        self.assertGreaterEqual(after["now_us"], after["finished_us"])
        self.assertGreater(after["timeline_uncertainty_us"], 0)

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
        self.assertIn("fw_enter_deep();", source)
        self.assertIn("fw_timer_reset(reset_reason)", source)
        self.assertIn("cfg.nvs_enable = false", source)
        self.assertIn("ESP_PM_NO_LIGHT_SLEEP", source)
        self.assertIn("s_usb_lock_held", source)
        self.assertIn("uart_read_bytes", source)
        main = (ROOT / "main/main.c").read_text()
        self.assertLess(main.index("muse_power_test_run();"), main.index("diagnostic_log_init()"))
        self.assertLess(main.index("muse_power_test_run();"), main.index("muse_glue_start();"))
        config = (ROOT / "components/muse/Kconfig").read_text().split("config MUSE_POWER_TEST", 1)[1].split("config MUSE_WATCHER_CAMERA", 1)[0]
        self.assertIn("default n", config)
        self.assertIn("MUSE_BOARD_SENSECAP_WATCHER", config)


    def sleep_complete(self, *extra):
        return self.run_commands(
            arm(matrix="sleep"), "usb 0", "advance 180000",
            "deep_resume 2000 5 4", "deep_resume 2000 5 4", *extra,
            ">ptest.results", "stats",
        )

    def test_sleep_plan_selection_exact_order_and_bounds(self):
        ack = self.run_commands(arm(matrix="sleep", settle_ms=5000, capture_ms=20000))[0]
        names = [
            "cold_ref", "cold_i2s_low", "cold_i2s_hiz", "cold_i2s_low_b",
            "codec_initialized", "codec_suspended", "warm_ref", "uart_hiz",
            "warm_ref_2", "rgb_low", "warm_ref_3", "pulls_off", "warm_ref_4",
            "unused_hiz", "warm_ref_5", "gpio_isolate", "warm_ref_6", "cpu_pd",
            "warm_ref_7", "best_combined", "warm_ref_8", "best_combined_b",
            "best_poll_5s", "warm_ref_9", "deep_sleep_timer", "deep_sleep_timer_held",
        ]
        self.assertEqual(ack["matrix"], "sleep")
        self.assertEqual(ack["matrix_version"], 2)
        self.assertEqual([p["id"] for p in ack["plan"]], names)
        self.assertEqual(ack["max_duration_ms"], 940000)
        self.assertEqual(ack["plan"][22]["poll_ms"], 5000)
        self.assertEqual([p["deep_sleep"] for p in ack["plan"]], [False] * 24 + [True] * 2)
        self.assertEqual(ack["plan"][0]["ref_group"], "")
        self.assertEqual(ack["plan"][2]["role"], "variant")
        self.assertEqual(ack["plan"][23]["ref_group"], "warm")
        self.assertEqual(ack["timeline_uncertainty_us"], 0)
        self.assertLessEqual(ack["rtc_bytes"], 7680)
        for kwargs in [{}, {"matrix": "peripheral"}]:
            legacy = self.run_commands(arm(**kwargs))[0]
            self.assertEqual((legacy["matrix"], legacy["matrix_version"]), ("peripheral", 1))
            self.assertEqual(len(legacy["plan"]), 27)
            self.assertEqual(sum(p["deep_sleep"] for p in legacy["plan"]), 0)

    def test_sleep_parser_strict_matrix_and_irreversible_cold_repeat(self):
        rejected = self.run_commands(arm(matrix="sleep", repeats=2))[0]
        self.assertEqual(rejected["type"], "error")
        self.assertEqual(rejected["message"], "sleep_requires_one_repeat_cold_not_reversible")
        for matrix in ["", "Sleep", "unknown", 0, False, None]:
            with self.subTest(matrix=matrix):
                self.assertEqual(self.run_commands(arm(matrix=matrix))[0]["type"], "error")
        duplicate = arm(matrix="sleep")[:-1] + ', "matrix":"sleep"}'
        self.assertEqual(self.run_commands(duplicate)[0]["type"], "error")
        reordered = '>ptest.arm={"matrix":"sleep","repeats":1,"capture_ms":1000,"run_id":"test","settle_ms":1000}'
        self.assertEqual(self.run_commands(reordered)[0]["matrix"], "sleep")

    def test_sleep_core_applies_variants_and_reverts_refs(self):
        ack, result, stats = self.sleep_complete()
        self.assertTrue(result["complete"])
        self.assertEqual([r["name"] for r in result["records"]], [p["id"] for p in ack["plan"]])
        self.assertTrue(all(r["valid"] for r in result["records"]))
        self.assertEqual(stats["knobs"], 0)
        self.assertGreater(stats["knob_reverts"], 8)
        self.assertGreater(stats["uart_restores"], 0)
        records = {r["name"]: r for r in result["records"]}
        self.assertEqual(records["cold_ref"]["actual"]["codec_state"], "cold")
        self.assertEqual(records["codec_initialized"]["actual"]["codec_state"], "initialized")
        self.assertEqual(records["codec_suspended"]["actual"]["codec_state"], "suspended")
        for name in ["warm_ref", "warm_ref_2", "warm_ref_3", "warm_ref_9"]:
            self.assertEqual(records[name]["actual"]["knobs_applied"], 0)
        self.assertEqual(records["cold_i2s_low"]["actual"]["knobs_applied"], 1)
        self.assertEqual(records["cold_i2s_hiz"]["actual"]["knobs_applied"], 2)
        self.assertEqual(records["best_poll_5s"]["poll_ms"], 5000)
        self.assertEqual(records["deep_sleep_timer_held"]["actual"]["knobs_applied"] & 256, 256)

    def test_parked_audio_cold_honesty_and_uart_usb_restore(self):
        low, blocked, init, not_cold = self.run_commands(
            "usb 0", "knob_apply 1", "audio_open", "knob_apply 4", "knob_apply 0",
        )
        self.assertEqual(low["knobs"], 1)
        self.assertNotEqual(blocked["error"], 0)
        self.assertEqual(init["error"], 0)
        self.assertEqual(init["codec_state"], 2)
        self.assertNotEqual(not_cold["error"], 0)
        for power in ["usb 1", "power_error 263"]:
            with self.subTest(power=power):
                parked, _, stats = self.run_commands(
                    "usb 0", "knob_apply 7", power, ">ptest.status", "stats",
                )
                self.assertTrue(parked["uart_parked"])
                self.assertEqual(stats["uart_restores"], 1)
                self.assertEqual(stats["knobs"] & 4, 0)

    def test_deep_pending_two_matched_resumes_and_virtual_timeline(self):
        ack, pending, between, result, stats = self.run_commands(
            arm(matrix="sleep"), "usb 0", "advance 180000", ">ptest.results",
            "deep_resume 2000 5 4", ">ptest.status",
            "deep_resume 2000 5 4", ">ptest.results", "stats",
        )
        self.assertFalse(pending["complete"])
        self.assertEqual(pending["records"][-1]["status"], "deep_sleep_pending")
        self.assertFalse(pending["records"][-1]["valid"])
        self.assertEqual(pending["resume_count"], 0)
        self.assertEqual(between["resume_count"], 1)
        self.assertEqual(result["resume_count"], 2)
        self.assertEqual(result["run_boot_id"], ack["boot_id"])
        self.assertEqual(result["retrieval_boot_id"], 333)
        self.assertEqual(stats["deep_entries"], 2)
        self.assertEqual(stats["holds_released"], 1)
        uncertainty = 0
        previous_end = result["sequence_start_us"]
        previous_boot = ack["boot_id"]
        for record in result["records"]:
            self.assertGreaterEqual(record["enter_us"], previous_end)
            previous_end = record["end_us"]
            if "deep_sleep" not in record:
                continue
            deep = record["deep_sleep"]
            self.assertEqual(record["boot_id"], previous_boot)
            self.assertEqual(deep["entry_boot_id"], previous_boot)
            self.assertEqual(deep["wake_cause"], "timer")
            self.assertEqual(deep["resume_reset_reason"], "deepsleep")
            self.assertEqual(deep["programmed_us"], 2000000)
            self.assertEqual(record["end_us"] - record["enter_us"], deep["rtc_slept_us"])
            self.assertEqual(record["measure_start_us"] - record["enter_us"], 1000000)
            self.assertEqual(record["actual"]["slept_us"], 0)
            uncertainty += (deep["rtc_slept_us"] + 99) // 100 + 50000
            self.assertEqual(record["timeline_uncertainty_us"], uncertainty)
            previous_boot = deep["resume_boot_id"]
        self.assertEqual(result["timeline_uncertainty_us"], uncertainty)
        self.assertGreater(result["timeline_offset_us"], 0)
        self.assertGreaterEqual(result["now_us"], result["finished_us"])

    def test_deep_only_matching_timer_journal_can_resume(self):
        for reset, wake, elapsed, mutation in [
            (3, 4, 2000, None), (5, 0, 2000, None), (5, 4, 2000, "wrong_pending"),
            (5, 4, 0, None), (5, 4, 20000, None),
        ]:
            with self.subTest(reset=reset, wake=wake, elapsed=elapsed, mutation=mutation):
                commands = [arm(matrix="sleep"), "usb 0", "advance 180000"]
                if mutation:
                    commands.append(mutation)
                commands += [f"deep_resume {elapsed} {reset} {wake}", ">ptest.results", "stats"]
                _, result, stats = self.run_commands(*commands)
                self.assertEqual(result["state"], "incomplete_reboot")
                self.assertEqual(result["resume_count"], 0)
                self.assertEqual(result["records"][-1]["end_us"], 0)
                self.assertFalse(result["records"][-1]["valid"])
                self.assertEqual(stats["deep_entries"], 1)

    def test_usb_dryrun_happy_is_24_states_and_preserves_journal(self):
        rows = self.run_commands("journal_hash", ">ptest.status", '>ptest.dryrun={"matrix":"sleep"}', ">ptest.status", "journal_hash", "stats")
        states = [r for r in rows if r["type"] == "dryrun_state"]
        done = [r for r in rows if r["type"] == "dryrun_done"]
        self.assertEqual([r["index"] for r in states], list(range(24)))
        self.assertEqual(len(done), 1)
        self.assertEqual((done[0]["states"], done[0]["errors"], done[0]["restore_error"]), (24, 0, 0))
        self.assertTrue(all(r["usb"] and not r["pm_exercised"] for r in states))
        self.assertEqual(rows[0]["hash"], rows[-2]["hash"])
        self.assertGreaterEqual(rows[-3]["now_us"], rows[1]["now_us"]) # bounded I/O executes, no capture dwell
        self.assertTrue(all("measure_start_us" not in row for row in states))
        self.assertEqual(rows[-3]["state"], "idle")
        self.assertEqual(rows[-1]["deep_entries"], 0)
        self.assertEqual(rows[-1]["knobs"], 0)

    def test_usb_dryrun_strict_json_usb_and_active_gates(self):
        for payload in ['{}', '[]', '{"matrix":"peripheral"}', '{"matrix":"sleep","matrix":"sleep"}', '{"matrix":"sleep","extra":1}', '{"matrix":"sleep"} junk', '{"matrix":"sleep"', '{"matrix":1}', '{"matrix":"sl\\\\u0065ep"}']:
            with self.subTest(payload=payload):
                row = self.run_commands(">ptest.dryrun=" + payload)[0]
                self.assertEqual(row["type"], "error")
        for gate in ["usb 0", arm(), "init_error 263"]:
            with self.subTest(gate=gate):
                rows = self.run_commands(gate, '>ptest.dryrun={"matrix":"sleep"}')
                self.assertEqual(rows[-1]["type"], "error")
                self.assertFalse(any(r["type"] == "dryrun_state" for r in rows))

    def test_usb_dryrun_reports_error_and_continues_with_per_state_log_reset(self):
        rows = self.run_commands("readback_error 4 263", '>ptest.dryrun={"matrix":"sleep"}')
        self.assertEqual(len(rows), 25)
        self.assertEqual(rows[4]["error"], 263)
        self.assertIn("fake readback", rows[4]["last_error_log"])
        self.assertNotIn('"', rows[4]["last_error_log"])
        self.assertNotIn("\\\\", rows[4]["last_error_log"])
        self.assertEqual(rows[5]["last_error_log"], "")
        self.assertEqual((rows[-1]["errors"], rows[-1]["restore_error"]), (1, 0))
        self.assertTrue(rows[-1]["usb"])

    def test_usb_dryrun_restoration_failure_has_done_and_log(self):
        rows = self.run_commands("readback_error 26 263", '>ptest.dryrun={"matrix":"sleep"}')
        self.assertEqual(len(rows), 25)
        self.assertEqual((rows[-1]["errors"], rows[-1]["restore_error"]), (1, 263))
        self.assertIn("fake readback", rows[-1]["last_error_log"])
        self.assertTrue(rows[-1]["usb"])

    def test_codec_mismatch_is_retained_evidence_not_run_error(self):
        _, result = self.run_commands("codec_mismatch 1", arm(matrix="sleep"), "usb 0", "advance 180000", "deep_resume 2000 5 4", "deep_resume 2000 5 4", ">ptest.results")
        self.assertTrue(result["complete"])
        self.assertTrue(all(r["valid"] for r in result["records"]))
        self.assertTrue(all(not r["codec_regs_expected"] and r["codec_regs"]["dac"]["0d"] == 0x99 for r in result["records"]))

    def test_error_capture_is_bounded_safe_ascii_and_not_uart(self):
        message = 'E ' + ('quotes"slash\\\\tab\\t' * 20)
        row = self.run_commands("init_error 263", "log " + message, ">ptest.status")[0]
        value = row["last_error_log"]
        self.assertEqual(len(value), 96)
        self.assertTrue(all(32 <= ord(c) <= 126 and c not in '\"\\\\' for c in value))

    def test_split_error_log_preserves_body_through_newline_and_ansi_reset(self):
        row = self.run_commands("log_split", "init_error 263", ">ptest.status")[0]
        self.assertEqual(row["last_error_log"], "E (42) fake: transport 263")

    def test_uart_dryrun_vbus_loss_restores_before_error_frame(self):
        rows = self.run_commands("drop_usb 7", '>ptest.dryrun={"matrix":"sleep"}', "stats")
        states = [r for r in rows if r["type"] == "dryrun_state"]
        self.assertEqual(len(states), 8)
        self.assertFalse(states[-1]["usb"])
        self.assertNotEqual(states[-1]["error"], 0)
        self.assertEqual(rows[-2]["type"], "dryrun_done")
        self.assertGreater(rows[-1]["uart_restores"], 0) # fw_write asserts UART is restored
        self.assertEqual(rows[-1]["knobs"], 0)

    def test_first_error_log_survives_reset_inside_cleanup(self):
        ack, result, status = self.run_commands("reset_cleanup 1", "readback_error 4 263", arm(matrix="sleep"), "usb 0", "advance 180000", ">ptest.results", ">ptest.status")
        self.assertEqual(result["status"], "error")
        self.assertEqual(result["error"], 263)
        self.assertEqual(result["records"][-1]["status"], "error")
        self.assertEqual(result["records"][-1]["error"], 263)
        self.assertIn("fake readback", result["records"][-1]["last_error_log"])
        self.assertEqual(result["last_error_log"], status["last_error_log"])
        self.assertEqual(status["boot_id"], 222)
        self.assertFalse(result["complete"])

    def test_dryrun_drains_fifo_before_every_next_state_and_done(self):
        rows = self.run_commands('>ptest.dryrun={"matrix":"sleep"}', "flush_stats")
        self.assertEqual(rows[-2]["states"], 24)
        self.assertEqual(rows[-1]["calls"], 25)
        self.assertFalse(rows[-1]["pending"])
        # The real core runs against fw_apply's no-pending-TX parking assertion.
        self.assertEqual([r["index"] for r in rows[:-2]], list(range(24)))

    def test_dryrun_tx_timeout_stops_states_and_reports_final_failure(self):
        rows = self.run_commands("flush_error 263", '>ptest.dryrun={"matrix":"sleep"}')
        self.assertEqual(len(rows), 3)
        self.assertEqual(rows[1]["states"], 1)
        self.assertEqual((rows[1]["errors"], rows[1]["restore_error"]), (1, 263))
        self.assertIn("UART TX drain", rows[1]["last_error_log"])
        self.assertEqual(rows[2]["type"], "error")

    def test_final_done_drain_failure_is_explicit_error_not_silent_success(self):
        rows = self.run_commands("flush_error 263", "flush_nth 25", '>ptest.dryrun={"matrix":"sleep"}')
        self.assertEqual(len(rows), 26)
        self.assertEqual(rows[-2]["type"], "dryrun_done")
        self.assertEqual(rows[-2]["states"], 24)
        self.assertEqual(rows[-1]["type"], "error")

    def test_successful_records_do_not_expose_ignored_component_error_logs(self):
        rows = self.run_commands("noise 1", '>ptest.dryrun={"matrix":"sleep"}', ">ptest.status")
        self.assertTrue(all(r["last_error_log"] == "" for r in rows))
        self.assertEqual(rows[-2]["errors"], 0)
        self.assertEqual(self.run_commands("log ignored", ">ptest.status")[0]["last_error_log"], "")

    def test_uart_parking_waits_before_any_pin_change(self):
        board = (ROOT / "components/muse/boards/board_sensecap_watcher.c").read_text()
        start = board.index("if (knobs & MUSE_PTEST_UART_HIZ)")
        end = board.index("if (knobs & MUSE_PTEST_RGB_LOW)", start)
        block = board[start:end]
        self.assertLess(block.index("uart_wait_tx_done"), block.index("s_ptest_uart_parked = true"))
        self.assertLess(block.index("uart_wait_tx_done"), block.index("GPIO_NUM_43"))
        self.assertIn("ESP_RETURN_ON_ERROR(uart_wait_tx_done", block)

    def test_runtime_error_capture_and_dryrun_uart_restore_guards(self):
        core = SOURCE.read_text()
        self.assertIn('esp_log_level_set("*", ESP_LOG_NONE)', core)
        self.assertIn('esp_log_level_set("board", ESP_LOG_ERROR)', core)
        self.assertIn('esp_rom_install_channel_putc(1, NULL)', core)
        self.assertIn('esp_rom_install_channel_putc(2, NULL)', core)
        self.assertIn("esp_log_set_vprintf(capture_error_log)", core)
        self.assertIn("if (s_dryrun || e || out->usb)", core)
        self.assertIn("remember_error(); save_run();", core)

    def test_semantically_invalid_checksummed_journals_are_discarded(self):
        for mutation in range(23):
            with self.subTest(mutation=mutation):
                _, result, stats = self.run_commands(
                    arm(matrix="sleep"), "usb 0", "advance 180000",
                    f"journal_bad {mutation}", "deep_resume 2000 5 4", ">ptest.results", "stats",
                )
                self.assertFalse(result["complete"])
                self.assertEqual(result["state"], "idle")
                self.assertEqual(result["records"], [])
                self.assertEqual(result["resume_count"], 0)
                self.assertEqual(stats["deep_entries"], 1)

    def test_finish_readback_failure_prevents_complete_sleep_run(self):
        _, result = self.run_commands(
            arm(matrix="sleep"), "usb 0", "advance 180000",
            "deep_resume 2000 5 4", "readback_error 26 263",
            "deep_resume 2000 5 4", ">ptest.results",
        )
        self.assertFalse(result["complete"])
        self.assertEqual(result["state"], "error")
        self.assertEqual(result["restore_error"], 263)

    def test_deep_timer_config_and_resume_usb_fail_closed(self):
        _, result, stats = self.run_commands(
            "timer_error 263", arm(matrix="sleep"), "usb 0", "advance 180000", ">ptest.results", "stats",
        )
        self.assertEqual(result["state"], "error")
        self.assertFalse(result["records"][-1]["valid"])
        self.assertEqual(result["error"], 263)
        self.assertEqual(stats["deep_entries"], 0)
        for failure in ["usb 1", "power_error 263", "init_error 263"]:
            with self.subTest(failure=failure):
                _, result, stats = self.run_commands(
                    arm(matrix="sleep"), "usb 0", "advance 180000", failure,
                    "deep_resume 2000 5 4", ">ptest.results", "stats",
                )
                self.assertFalse(result["complete"])
                self.assertFalse(result["records"][-1]["valid"])
                self.assertEqual(result["resume_count"], 0)
                self.assertEqual(stats["deep_entries"], 1)

    def test_fresh_arm_retains_status_epoch_and_uncertainty(self):
        _, before, ack = self.run_commands(
            arm(matrix="sleep"), "usb 0", "advance 180000",
            "deep_resume 2000 5 4", "deep_resume 2000 5 4",
            "usb 1", ">ptest.status", arm(run_id="next"),
        )
        self.assertEqual(ack["ack_us"], before["now_us"])
        self.assertEqual(ack["timeline_offset_us"], before["timeline_offset_us"])
        self.assertEqual(ack["timeline_uncertainty_us"], before["timeline_uncertainty_us"])

    def test_board_knob_source_contracts_and_cpu_veto_release(self):
        board = (ROOT / "components/muse/boards/board_sensecap_watcher.c").read_text()
        apply = board.split("esp_err_t muse_ptest_board_apply(", 1)[1].split("static esp_err_t ptest_codec_byte", 1)[0]
        self.assertLess(apply.index("ptest_revert_knobs()"), apply.index("ptest_audio_initialize()"))
        self.assertLess(apply.index("audio conflicts with parked pins"), apply.index("ptest_revert_knobs()"))
        self.assertIn("codec register transport", board)
        self.assertIn("ptest_codec_snapshot()", board)
        self.assertNotIn("value == expected ? ESP_OK : ESP_FAIL", board)
        self.assertIn("gpio_deep_sleep_hold_en();", board)
        self.assertTrue("gpio_sleep_sel_dis(lcd[i])" in board)
        self.assertTrue("I2C_SDA, I2C_SCL, EXP_INT, KNOB_A, KNOB_B" in board)
        self.assertTrue("CAM_SCLK, CAM_MOSI, CAM_MISO, CAM_CS, GPIO_NUM_46, GPIO_NUM_17, GPIO_NUM_18" in board)
        source = SOURCE.read_text()
        self.assertTrue("esp_sleep_pd_config(ESP_PD_DOMAIN_CPU, veto ? ESP_PD_OPTION_ON : ESP_PD_OPTION_OFF)" in source)
        self.assertNotIn("esp_sleep_pd_config(ESP_PD_DOMAIN_CPU, ESP_PD_OPTION_AUTO)", source)
        self.assertIn("esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL)", source)


if __name__ == "__main__":
    unittest.main()
