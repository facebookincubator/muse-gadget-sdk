# Copyright (c) Meta Platforms, Inc. and affiliates.
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy at http://www.apache.org/licenses/LICENSE-2.0
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

import argparse
import contextlib
import copy
import importlib.util
import io
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

SOURCE = Path(__file__).resolve().parents[1] / "tools/muse/watcher_power.py"
spec = importlib.util.spec_from_file_location("watcher_power", SOURCE)
power = importlib.util.module_from_spec(spec)
spec.loader.exec_module(power)


def sleep_evidence():
    """Fake two-resume run, using the emitted firmware contract (no device I/O)."""
    plan = [{"index": i, "id": name, "role": "other", "ref_group": "",
             "knobs": [], "deep_sleep": True, "poll_ms": 1000}
            for i, name in enumerate(("deep_a", "deep_b"))]
    arm = {"type": "arm_ack", "run_id": "test-run", "boot_id": 7,
           "matrix": "sleep", "matrix_version": 2, "plan": plan}
    acquisition = {"state": "armed", "arm": arm, "requested_config": {"matrix": "sleep"}}
    records = []
    for i, name in enumerate(("deep_a", "deep_b")):
        records.append({"index": i, "name": name, "id": name, "repeat": 0,
                        "boot_id": 7 + i, "status": "ok", "apply_err": 0,
                        "measure_start_us": 2_000_000 + i * 3_000_000,
                        "end_us": 5_000_000 + i * 3_000_000,
                        "timeline_uncertainty_us": (i + 1) * 50_000,
                        "deep_sleep": {"entry_boot_id": 7 + i, "resume_boot_id": 8 + i,
                                       "programmed_us": 3_000_000, "rtc_slept_us": 3_000_000,
                                       "wake_cause": "timer", "resume_reset_reason": "deepsleep"}})
    results = {"run_id": "test-run", "run_boot_id": 7, "retrieval_boot_id": 9,
               "matrix": "sleep", "matrix_version": 2, "state": "complete",
               "resume_count": 2, "timeline_offset_us": 6_000_000,
               "timeline_uncertainty_us": 100_000, "records": records}
    live = {"usb": True, "boot_id": 9, "run_boot_id": 7, "run_id": "test-run",
            "now_us": 9_000_000, "matrix": "sleep", "matrix_version": 2,
            "resume_count": 2, "timeline_offset_us": 6_000_000,
            "timeline_uncertainty_us": 100_000}
    return acquisition, results, live


class ReportTests(unittest.TestCase):
    def setUp(self):
        self.bundle = {
            "acquisition": {
                "clock_sync": {"best": {"device_us": 1_000_000, "offset_s": 99,
                                          "uncertainty_s": 0.01}},
                "ppk_before_arm": {"host_monotonic_origin_s": 100, "voltage_mv": 3872,
                                   "owner_port": "fake-ppk", "decoded_missing_frame_count": 0,
                                   "total_invalid_samples": 0, "late_unassigned_sample_count": 0}},
            "firmware": {"state": "complete", "records": [{"name": "baseline", "repeat": 0,
                         "status": "ok", "apply_err": 0, "measure_start_us": 2_000_000,
                         "end_us": 5_000_000}]},
            "ppk_final": {"fault": None, "max_observed_backlog_bytes": 4000,
                          "decoded_missing_frame_count": 0, "total_invalid_samples": 0,
                          "late_unassigned_sample_count": 0}}
        self.begin = {"type": "begin", "label": "watcher-sweep", "host_offset_s": 0.0,
                      "first_valid_received_index": 0, "window_sample_index_start": 0,
                      "decoded_missing_frame_count_at_begin": 0,
                      "total_invalid_samples_at_begin": 0,
                      "late_unassigned_sample_count_at_begin": 0}
        self.frames = []
        for i in range(50):
            t = i / 10
            self.frames.append({"type": "window", "label": "watcher-sweep",
                "host_start_offset_s": t, "host_duration_s": 0.1,
                "sample_index_start": i * 10_000, "partial": False, "window_index": i,
                "sample_index_end": (i + 1) * 10_000, "sample_count": 10_000,
                "mean_uA": 1000, "min_uA": 900, "max_uA": 1100,
                "sampled_charge_mAh": 100 / 3_600_000,
                "sampled_energy_mWh": 387.2 / 3_600_000})

    def report(self, bundle=None, frames=None, **options):
        header = {"type": "session", "schema_version": 1, "sample_rate_hz": 100_000,
                  "host_monotonic_origin_s": 100, "voltage_mv": 3872,
                  "port_descriptor": {"port": "fake-ppk"}}
        return power.analyze_frames(bundle or self.bundle,
                [header, self.begin] + (self.frames if frames is None else frames), **options)

    def row(self, bundle=None, frames=None):
        return self.report(bundle, frames)["rows"][0]

    def test_guarded_mean_units_and_integrals(self):
        report = self.report()
        self.assertEqual(report["requested_guard_s"], 0.5)
        self.assertEqual(report["assumed_clock_drift_ppm"], 100)
        self.assertIn("Conditional", report["timing_model"])
        self.assertIn("history", report["limitations"][0])
        row = report["rows"][0]
        self.assertEqual(row["quality"], "receive_window_estimate")
        self.assertEqual(row["mean_mA"], 1)
        self.assertEqual(row["power_mW_source_setpoint"], 3.872)
        self.assertAlmostEqual(row["included_host_duration_s"], 2)
        self.assertAlmostEqual(row["sampled_charge_mAh"], 2000 / 3_600_000)
        self.assertIn("not recombinable", row["percentiles"])

    def test_codec_diagnostics_pass_through_without_changing_acceptance(self):
        diagnostics = {"codec_regs": {"dac": {"00": 0, "01": 63}, "adc": {"00": None},
                                      "adc_variant": "es7243e"},
                       "codec_regs_available": False, "codec_regs_expected": False, "last_error_log": "diagnostic only"}
        record = self.bundle["firmware"]["records"][0]
        record.update(diagnostics)
        for status in ("ok", "skipped", "error"):
            with self.subTest(status=status):
                record["status"] = status
                row = self.row()
                for key, value in diagnostics.items():
                    self.assertEqual(row[key], value)
                self.assertEqual(row["quality"], "receive_window_estimate" if status == "ok" else "not_measured")
                if status == "ok":
                    self.assertEqual(row["mean_mA"], 1)

    def shift_capture(self, host_prefix_s, sample_prefix, processed_prefix=None):
        self.begin.update({"host_offset_s": host_prefix_s,
                           "first_valid_received_index": sample_prefix if processed_prefix is None else processed_prefix,
                           "window_sample_index_start": sample_prefix})
        self.bundle["acquisition"]["clock_sync"]["best"]["offset_s"] += host_prefix_s
        for frame in self.frames:
            frame["host_start_offset_s"] += host_prefix_s
            frame["sample_index_start"] += sample_prefix
            frame["sample_index_end"] += sample_prefix

    def test_failed_preflight_prefix_excluded_not_erased(self):
        self.shift_capture(70, 5_000_000)
        report = self.report()
        self.assertEqual(report["rows"][0]["quality"], "receive_window_estimate")
        self.assertAlmostEqual(report["rows"][0]["sample_vs_host_drift_s"], 0)
        self.assertEqual(report["transport_anchor"]["excluded_preflight_sample_vs_host_drift_s"], -20)

    def test_queued_pre_begin_prefix_not_recorded_drift(self):
        self.shift_capture(2, 200_000, processed_prefix=180_000)
        self.assertEqual(self.row()["quality"], "receive_window_estimate")
        self.assertAlmostEqual(self.row()["sample_vs_host_drift_s"], 0)

    def test_new_loss_still_rejected_after_bad_preflight(self):
        self.shift_capture(70, 5_000_000)
        for frame in self.frames:
            frame["sample_index_start"] += 100_000
            frame["sample_index_end"] += 100_000
        self.assertEqual(self.row()["quality"], "rejected")

    def test_missing_duplicate_or_invalid_begin_rejected(self):
        with self.assertRaisesRegex(ValueError, "begin anchor"):
            self.report(frames=self.frames + [self.begin])
        for field, value in (("host_offset_s", float("nan")),
                             ("window_sample_index_start", -1),
                             ("first_valid_received_index", True)):
            original = self.begin[field]
            self.begin[field] = value
            with self.assertRaisesRegex(ValueError, "begin clock/sample anchor"):
                self.row()
            self.begin[field] = original
        header = {"type": "session", "schema_version": 1, "sample_rate_hz": 100_000,
                  "host_monotonic_origin_s": 100, "voltage_mv": 3872,
                  "port_descriptor": {"port": "fake-ppk"}}
        with self.assertRaisesRegex(ValueError, "begin anchor"):
            power.analyze_frames(self.bundle, [header] + self.frames)

    def test_analyze_file_preserves_begin_anchor(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            bundle_path, samples_path, output_path = root / "bundle.json", root / "samples.jsonl", root / "report.json"
            power.save_json(bundle_path, self.bundle)
            header = {"type": "session", "schema_version": 1, "sample_rate_hz": 100_000,
                      "host_monotonic_origin_s": 100, "voltage_mv": 3872,
                      "port_descriptor": {"port": "fake-ppk"}}
            samples_path.write_text("".join(power.json.dumps(frame) + "\n"
                                           for frame in [header, self.begin] + self.frames))
            with contextlib.redirect_stdout(io.StringIO()):
                power.analyze(SimpleNamespace(results=str(bundle_path), samples=str(samples_path),
                                              output=str(output_path), guard_s=0.5, clock_drift_ppm=100))
            report = power.load_json(output_path)
            self.assertEqual(report["rows"][0]["quality"], "receive_window_estimate")

    def test_report_device_name_defaults_and_overrides(self):
        self.assertEqual(self.report()["device"], "Seeed SenseCAP Watcher")
        report = self.report(device="  Other Board  ")
        self.assertEqual(report["device"], "Other Board")
        for bad in ("", "   ", "x" * 81, None):
            with self.assertRaisesRegex(ValueError, "device name"):
                self.report(device=bad)

    def test_skipped_and_failed_firmware_never_quantified(self):
        for state in ("skipped", "error"):
            self.bundle["firmware"]["records"][0]["status"] = state
            row = self.row()
            self.assertEqual(row["quality"], "not_measured")
            self.assertIsNone(row["mean_mA"])

    def test_apply_failure_even_if_record_says_ok(self):
        self.bundle["firmware"]["records"][0]["apply_err"] = 1
        self.assertIsNone(self.row()["mean_mA"])

    def test_capture_requires_firmware_timestamps(self):
        del self.bundle["firmware"]["records"][0]["measure_start_us"]
        self.assertIsNone(self.row()["mean_mA"])

    def test_collector_fault_rejected(self):
        self.bundle["ppk_final"]["fault"] = "disk full"
        self.assertEqual(self.row()["quality"], "rejected")

    def test_decode_loss_during_recording_never_quantified(self):
        self.bundle["ppk_final"]["decoded_missing_frame_count"] = 1
        self.assertEqual(self.row()["quality"], "rejected")
        del self.bundle["ppk_final"]["decoded_missing_frame_count"]
        with self.assertRaisesRegex(ValueError, "decode-loss evidence"):
            self.row()

    def test_decode_loss_in_begin_reply_race_not_masked(self):
        self.bundle["acquisition"]["ppk_before_arm"]["decoded_missing_frame_count"] = 1
        self.bundle["ppk_final"]["decoded_missing_frame_count"] = 1
        self.assertEqual(self.row()["quality"], "rejected")
        del self.begin["decoded_missing_frame_count_at_begin"]
        with self.assertRaisesRegex(ValueError, "decode-loss evidence"):
            self.row()

    def test_buffered_invalid_and_late_delivery_rejected(self):
        for counter in ("total_invalid_samples", "late_unassigned_sample_count"):
            self.bundle["ppk_final"][counter] = 1
            self.assertEqual(self.row()["quality"], "rejected")
            self.bundle["ppk_final"][counter] = 0

    def test_buffered_delay_and_peak_backlog_exceeding_guard_rejected(self):
        frames = copy.deepcopy(self.frames)
        for frame in frames:
            frame["label_max_consumer_delay_s"] = 1.0
        self.assertEqual(self.row(frames=frames)["quality"], "rejected")
        self.bundle["ppk_final"]["receiver"] = {"max_combined_backlog_bytes": 400_000}
        self.assertEqual(self.row()["quality"], "rejected")

    def test_sparse_and_overfull_samples_rejected(self):
        for count in (8000, 12000):
            frames = copy.deepcopy(self.frames)
            for i, frame in enumerate(frames):
                frame["sample_count"] = count
                frame["sample_index_start"] = i * count
                frame["sample_index_end"] = (i + 1) * count
            self.assertEqual(self.row(frames=frames)["quality"], "rejected")

    def test_single_interior_missing_window_rejected(self):
        frames = [x for x in self.frames if x["window_index"] != 22]
        self.assertEqual(self.row(frames=frames)["quality"], "rejected")

    def test_missing_window_gap_rejected(self):
        frames = [x for x in self.frames if not 2.0 <= x["host_start_offset_s"] < 2.5]
        self.assertEqual(self.row(frames=frames)["quality"], "rejected")

    def test_dropped_axis_and_backlog_rejected(self):
        frames = copy.deepcopy(self.frames)
        for frame in frames:
            frame["sample_index_start"] += 100_000
            frame["sample_index_end"] += 100_000
        self.assertEqual(self.row(frames=frames)["quality"], "rejected")
        self.bundle["ppk_final"]["max_observed_backlog_bytes"] = 400_000
        self.assertEqual(self.row()["quality"], "rejected")

    def test_no_windows_not_zero_current(self):
        self.assertIsNone(self.row(frames=[])["mean_mA"])
        report = self.report(frames=[])
        # Rejected measurements remain valid strict JSON, never Infinity.
        power.json.dumps(report, allow_nan=False)

    def test_missing_apply_evidence_not_assumed_success(self):
        del self.bundle["firmware"]["records"][0]["apply_err"]
        self.assertIsNone(self.row()["mean_mA"])

    def test_nan_sync_rejected(self):
        self.bundle["acquisition"]["clock_sync"]["best"]["uncertainty_s"] = float("nan")
        with self.assertRaises(ValueError):
            self.row()

    def test_duplicated_window_rejected(self):
        with self.assertRaises(ValueError):
            self.row(frames=self.frames + self.frames)

    def test_partial_windows_never_quantified(self):
        frames = copy.deepcopy(self.frames)
        for frame in frames:
            frame["partial"] = True
        self.assertIsNone(self.row(frames=frames)["mean_mA"])

    def test_wrong_capture_epoch_rejected(self):
        self.bundle["acquisition"]["ppk_before_arm"]["host_monotonic_origin_s"] = 101
        with self.assertRaises(ValueError):
            self.row()

    def test_missing_header_rejected(self):
        with self.assertRaises(ValueError):
            power.analyze_frames(self.bundle, self.frames)

    def test_nan_guard_rejected(self):
        with self.assertRaises(ValueError):
            power.analyze_frames(self.bundle, self.frames, guard_s=float("nan"))

    def test_clock_uncertainty_expands_guard(self):
        self.bundle["acquisition"]["clock_sync"]["best"]["uncertainty_s"] = 0.6
        self.assertGreater(self.row()["capture_guard_s"], 0.8)

    def test_report_explicitly_not_production_profile(self):
        report = self.report()
        self.assertIn("NOT production", report["profile_kind"])
        self.assertTrue(report["limitations"])


    def install_sleep_evidence(self):
        acquisition, firmware, live = sleep_evidence()
        self.bundle["acquisition"].update(acquisition)
        self.bundle.update({"firmware": firmware, "live_status": live})

    def test_legacy_record_absent_timeline_uncertainty_defaults_zero(self):
        self.assertEqual(self.row()["timeline_uncertainty_s"], 0)
        self.assertEqual(self.report()["matrix"], "peripheral")

    def test_per_record_uncertainty_added_to_budget_and_guard(self):
        before = self.row()["clock_uncertainty_budget_s"]
        self.bundle["firmware"]["records"][0]["timeline_uncertainty_us"] = 600_000
        row = self.row()
        self.assertAlmostEqual(row["timeline_uncertainty_s"], 0.6)
        self.assertAlmostEqual(row["clock_uncertainty_budget_s"], before + 0.6)
        self.assertGreater(row["capture_guard_s"], 0.85)

    def test_deep_sleep_uses_virtual_guarded_window_without_light_sleep_counters(self):
        self.install_sleep_evidence()
        report = self.report()
        row = report["rows"][0]
        self.assertEqual(row["quality"], "receive_window_estimate")
        self.assertEqual(row["mean_mA"], 1)
        self.assertEqual(row["deep_sleep"]["resume_boot_id"], 8)
        self.assertEqual(row["timeline_uncertainty_s"], 0.05)
        self.assertEqual(report["matrix_version"], 2)
        self.assertEqual(report["plan"][0]["deep_sleep"], True)
        self.assertEqual(row["role"], "other")
        self.assertEqual(row["poll_ms"], 1000)

    def test_sleep_missing_record_uncertainty_fails_closed(self):
        self.install_sleep_evidence()
        del self.bundle["firmware"]["records"][0]["timeline_uncertainty_us"]
        with self.assertRaisesRegex(ValueError, "missing.*uncertainty"):
            self.row()

    def test_invalid_record_uncertainty_fails_closed_even_for_skipped_records(self):
        for value in (-1, float("nan"), float("inf"), True, 1.0, "1", None):
            with self.subTest(value=value):
                record = self.bundle["firmware"]["records"][0]
                record.update({"timeline_uncertainty_us": value, "status": "skipped"})
                with self.assertRaisesRegex(ValueError, "non-negative integer"):
                    self.row()

    def test_sleep_missing_top_uncertainty_or_live_provenance_rejected(self):
        self.install_sleep_evidence()
        del self.bundle["firmware"]["timeline_uncertainty_us"]
        with self.assertRaisesRegex(ValueError, "missing.*uncertainty"):
            self.row()
        self.install_sleep_evidence()
        del self.bundle["live_status"]
        with self.assertRaises(ValueError):
            self.row()

    def test_sleep_timing_uncertainty_cannot_quantify_no_guarded_span(self):
        self.install_sleep_evidence()
        self.bundle["firmware"]["timeline_uncertainty_us"] = 2_000_000
        self.bundle["live_status"]["timeline_uncertainty_us"] = 2_000_000
        self.bundle["firmware"]["records"][0]["timeline_uncertainty_us"] = 2_000_000
        self.assertEqual(self.row()["quality"], "rejected")
        self.assertIsNone(self.row()["mean_mA"])

    def test_sleep_keeps_coverage_and_new_loss_gates(self):
        self.install_sleep_evidence()
        self.bundle["ppk_final"]["decoded_missing_frame_count"] = 1
        self.assertEqual(self.row()["quality"], "rejected")
        self.bundle["ppk_final"]["decoded_missing_frame_count"] = 0
        frames = copy.deepcopy(self.frames)
        for i, frame in enumerate(frames):
            frame.update({"sample_count": 9800, "sample_index_start": i * 9800,
                          "sample_index_end": (i + 1) * 9800})
        self.assertEqual(self.row(frames=frames)["quality"], "rejected")

    def test_report_rejects_result_matrix_and_plan_identity_mismatch(self):
        self.bundle["firmware"].update({"matrix": "sleep", "matrix_version": 2})
        with self.assertRaisesRegex(ValueError, "matrix"):
            self.row()
        self.install_sleep_evidence()
        self.bundle["firmware"]["records"][0]["index"] = 1
        with self.assertRaisesRegex(ValueError, "identity"):
            self.row()


class SyncTests(unittest.TestCase):
    def test_actual_usb_required(self):
        with patch.object(power, "request", return_value={"usb": False}):
            with self.assertRaises(ValueError):
                power.synchronize(object())

    def test_reboot_during_sync_rejected(self):
        replies = [{"usb": True, "now_us": 1, "boot_id": b} for b in (1, 2, 2)]
        with patch.object(power, "request", side_effect=replies):
            with self.assertRaisesRegex(ValueError, "rebooted"):
                power.synchronize(object())

    def test_smallest_roundtrip_used_and_all_saved(self):
        replies = [{"usb": True, "now_us": 1_000_000, "boot_id": 5}] * 3
        # synchronize reads start/end only; request is mocked.
        with patch.object(power, "request", side_effect=replies), \
             patch.object(power.time, "monotonic", side_effect=(10, 12, 20, 20.2, 30, 30.5)):
            result = power.synchronize(object())
        self.assertAlmostEqual(result["best"]["uncertainty_s"], 0.1)
        self.assertEqual(len(result["observations"]), 3)


    def test_post_resume_virtual_clock_interval_includes_firmware_uncertainty(self):
        _, _, live = sleep_evidence()
        with patch.object(power, "request", return_value=live), \
             patch.object(power.time, "monotonic", side_effect=(110, 110.2, 120, 120.4, 130, 130.6)):
            result = power.synchronize(object())
        self.assertEqual(result["best"]["device_us"], 9_000_000)
        self.assertEqual(result["best"]["boot_id"], 9)
        self.assertAlmostEqual(result["best"]["offset_s"], 101.1)
        self.assertAlmostEqual(result["best"]["uart_rtt_uncertainty_s"], 0.1)
        self.assertAlmostEqual(result["best"]["uncertainty_s"], 0.2)
        self.assertEqual(result["best"]["timeline_uncertainty_us"], 100_000)

    def test_sleep_sync_requires_uncertainty_and_rejects_nonfinite_negative(self):
        for value in (None, -1, float("nan"), True, 0.5):
            with self.subTest(value=value):
                status = {"usb": True, "now_us": 10, "boot_id": 7, "matrix": "sleep"}
                if value is not None:
                    status["timeline_uncertainty_us"] = value
                with patch.object(power, "request", return_value=status), self.assertRaises(ValueError):
                    power.synchronize(object())


class FakeBoard:
    def __init__(self, reply):
        self.reply = reply
        self.commands = []

    def wake(self):
        pass

    def write_line(self, command):
        self.commands.append(command)

    def read_line(self, deadline):
        return "PTEST " + power.json.dumps(self.reply)

    def __enter__(self):
        return self

    def __exit__(self, *args):
        pass


class LifecycleTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.output = str(Path(self.tmp.name) / "output.json")
        self.owner = {"holding": True, "fault": None, "active_label": None,
            "host_monotonic_origin_s": 100, "owner_port": "fake-ppk",
            "voltage_mv": 3872, "total_valid_samples": 10000,
            "elapsed_host_s": 1, "last_data_host_offset_s": 0.99}
        self.args = argparse.Namespace(port="fake-watcher", ppk_control_dir="fake-control",
            output=self.output, run_id="test-run", settle_ms=5000, capture_ms=20000,
            repeats=1, usb_reconnected=True)
        self.ack = {"schema": 1, "type": "arm_ack", "run_id": "test-run",
            "boot_id": 7, "usb": True, "status": "armed", "max_duration_ms": 815000}
        self.sync = {"best": {"boot_id": 7}}
        self.board = FakeBoard(self.ack)
        self.chat = SimpleNamespace(Board=lambda port: self.board)

    def tearDown(self):
        self.tmp.cleanup()

    def run_arm(self, ack=None, owner=None):
        with patch.dict(sys.modules, {"chat": self.chat}), \
             patch.object(power, "send_command", return_value=owner or self.owner) as commands, \
             patch.object(power, "synchronize", return_value=self.sync), \
             patch.object(power, "request", return_value=ack or self.ack), \
             contextlib.redirect_stdout(io.StringIO()):
            power.arm(self.args)
        return commands

    def test_output_reservation_failure_changes_no_owner_state(self):
        self.args.output = str(Path(self.tmp.name) / "absent" / "result.json")
        with patch.dict(sys.modules, {"chat": self.chat}), \
             patch.object(power, "send_command") as commands:
            with self.assertRaises(FileNotFoundError):
                power.arm(self.args)
            commands.assert_not_called()

    def test_lost_arm_ack_banks_recovery_before_command(self):
        def lost_ack(board, command, kind):
            metadata = power.load_json(self.output)
            self.assertEqual(metadata["arm"]["run_id"], "test-run")
            self.assertEqual(metadata["arm"]["boot_id"], 7)
            raise TimeoutError("unknown outcome")
        with patch.dict(sys.modules, {"chat": self.chat}), \
             patch.object(power, "send_command", return_value=self.owner) as commands, \
             patch.object(power, "synchronize", return_value=self.sync), \
             patch.object(power, "request", side_effect=lost_ack):
            with self.assertRaises(TimeoutError):
                power.arm(self.args)
            self.assertEqual([x.args[1]["op"] for x in commands.call_args_list], ["status", "begin"])
        self.assertEqual(power.load_json(self.output)["state"], "arm_outcome_unknown")

    def test_arm_correct_ack_saves_without_finish(self):
        commands = self.run_arm()
        self.assertEqual([x.args[1]["op"] for x in commands.call_args_list], ["status", "begin"])
        self.assertEqual(power.load_json(self.output)["arm"]["run_id"], "test-run")

    def test_arm_stale_or_empty_stream_never_begins(self):
        for change in ({"total_valid_samples": 0}, {"last_data_host_offset_s": -1}):
            Path(self.output).unlink(missing_ok=True)
            owner = dict(self.owner, **change)
            with patch.dict(sys.modules, {"chat": self.chat}), \
                 patch.object(power, "send_command", return_value=owner) as commands:
                with self.assertRaises(ValueError):
                    power.arm(self.args)
                self.assertEqual([x.args[1]["op"] for x in commands.call_args_list], ["status"])

    def test_wrong_run_or_usb_ack_never_ready_and_owner_retained(self):
        for change in ({"run_id": "other"}, {"usb": False}, {"boot_id": 8}):
            with self.assertRaises(ValueError):
                self.run_arm(dict(self.ack, **change))
            self.assertEqual(power.load_json(self.output)["state"], "arm_outcome_unknown")
            Path(self.output).unlink()  # each subcase gets a fresh reservation

    def test_request_wire_kind_schema(self):
        response = power.request(self.board, "ptest.arm={}", "arm_ack")
        self.assertEqual(response, self.ack)
        self.assertEqual(self.board.commands, ["ptest.arm={}"])
        self.board.reply = {"schema": 9, "type": "arm_ack"}
        with self.assertRaises(ValueError):
            power.request(self.board, "ptest.status", "status")

    def test_request_status_nonce_discards_stale_clock_reply(self):
        frames = iter([
            {"schema": 1, "type": "status", "request_id": "old", "now_us": 1},
            {"schema": 1, "type": "status", "request_id": "current", "now_us": 2}])
        with patch.object(power.secrets, "token_hex", return_value="current"), \
             patch.object(self.board, "read_line", side_effect=lambda deadline: "PTEST " + power.json.dumps(next(frames))):
            status = power.request(self.board, "ptest.status", "status")
        self.assertEqual(status["now_us"], 2)
        self.assertEqual(self.board.commands, ["ptest.status=current"])

    def test_collect_changed_epoch_keeps_owner_untouched(self):
        acquisition = {"ppk_before_arm": self.owner, "arm": self.ack}
        path = str(Path(self.tmp.name) / "acquisition.json")
        power.save_json(path, acquisition)
        self.args.acquisition = path
        replaced = dict(self.owner, host_monotonic_origin_s=101)
        with patch.dict(sys.modules, {"chat": self.chat}), \
             patch.object(power, "send_command", return_value=replaced) as commands:
            with self.assertRaises(ValueError):
                power.collect(self.args)
            self.assertEqual([x.args[1]["op"] for x in commands.call_args_list], ["status"])

    def test_collect_matching_run_releases_only_after_evidence_saved(self):
        acquisition = {"ppk_before_arm": self.owner, "arm": self.ack}
        path = str(Path(self.tmp.name) / "acquisition.json")
        power.save_json(path, acquisition)
        self.args.acquisition = path
        results = {"run_id": "test-run", "run_boot_id": 7, "retrieval_boot_id": 7, "state": "complete"}
        observed_ops = []
        def owner_command(directory, command):
            observed_ops.append(command["op"])
            if command["op"] == "finish":
                self.assertTrue(Path(self.output).exists())
                return dict(self.owner, holding=False)
            return self.owner
        with patch.dict(sys.modules, {"chat": self.chat}), \
             patch.object(power, "send_command", side_effect=owner_command), \
             patch.object(power, "request", side_effect=[{"usb": True, "boot_id": 7}, results]), \
             contextlib.redirect_stdout(io.StringIO()):
            power.collect(self.args)
        self.assertEqual(observed_ops, ["status", "status", "finish"])
        self.assertTrue(power.load_json(self.output)["reconnected_usb_observed"])


    def collect_fake(self, acquisition, results, live, *, release=True, finish_result=None):
        Path(self.output).unlink(missing_ok=True)
        path = Path(self.tmp.name) / "acquisition.json"
        path.unlink(missing_ok=True)
        acquisition = dict(acquisition, ppk_before_arm=self.owner)
        power.save_json(path, acquisition)
        self.args.acquisition = str(path)
        self.args.usb_reconnected = release
        self.observed_ops = []
        def command(directory, request):
            self.observed_ops.append(request["op"])
            if request["op"] == "finish":
                self.assertTrue(Path(self.output).exists())
                return dict(self.owner, holding=False) if finish_result is None else finish_result
            return self.owner
        with patch.dict(sys.modules, {"chat": self.chat}), \
             patch.object(power, "send_command", side_effect=command), \
             patch.object(power, "request", side_effect=[live, results]), \
             contextlib.redirect_stdout(io.StringIO()):
            power.collect(self.args)
        return power.load_json(self.output)

    def test_collect_preserves_optional_register_diagnostics_without_gating(self):
        acquisition, results, live = sleep_evidence()
        diagnostics = {"codec_regs": {"dac": {"00": None}, "adc": {"00": 127}, "adc_variant": "es7243"},
                       "codec_regs_available": False, "codec_regs_expected": False, "last_error_log": "diagnostic only"}
        results.update(diagnostics)
        live.update(diagnostics)
        for record in results["records"]:
            record.update(diagnostics)
        bank = self.collect_fake(acquisition, results, live, release=False)
        self.assertEqual(bank["firmware"], results)
        self.assertEqual(bank["live_status"], live)
        self.assertEqual(self.observed_ops, ["status", "status"])

    def assert_collect_fails_without_mutation(self, acquisition, results, live):
        with self.assertRaises(ValueError):
            self.collect_fake(acquisition, results, live)
        self.assertEqual(self.observed_ops, ["status"])
        self.assertFalse(Path(self.output).exists())

    def test_sleep_repeat_two_rejected_before_owner_or_board_touched(self):
        self.args.matrix, self.args.repeats = "sleep", 2
        with patch.dict(sys.modules, {"chat": self.chat}), \
             patch.object(power, "send_command") as commands, \
             patch.object(power, "synchronize") as sync:
            with self.assertRaisesRegex(ValueError, "requires repeats=1"):
                power.arm(self.args)
            commands.assert_not_called()
            sync.assert_not_called()

    def test_arm_sleep_sends_matrix_and_banks_echo(self):
        self.args.matrix = "sleep"
        ack = dict(self.ack, matrix="sleep", matrix_version=2, timeline_uncertainty_us=0)
        with patch.dict(sys.modules, {"chat": self.chat}), \
             patch.object(power, "send_command", return_value=self.owner), \
             patch.object(power, "synchronize", return_value=self.sync), \
             patch.object(power, "request", return_value=ack) as request, \
             contextlib.redirect_stdout(io.StringIO()):
            power.arm(self.args)
        wire = power.json.loads(request.call_args.args[1].split("=", 1)[1])
        self.assertEqual(wire["matrix"], "sleep")
        self.assertEqual(power.load_json(self.output)["arm"]["matrix_version"], 2)

    def test_arm_peripheral_omits_matrix_for_legacy_firmware(self):
        self.run_arm()
        metadata = power.load_json(self.output)
        self.assertNotIn("matrix", metadata["requested_config"])
        self.assertNotIn("matrix", metadata["arm"])

    def test_arm_matrix_echo_mismatch_or_missing_uncertainty_retains_unknown_owner(self):
        self.args.matrix = "sleep"
        for changes in ({}, {"matrix": "peripheral", "matrix_version": 1},
                        {"matrix": "sleep", "matrix_version": 1},
                        {"matrix": "sleep", "matrix_version": True},
                        {"matrix": "sleep", "matrix_version": 2},
                        {"matrix": "sleep", "matrix_version": 2, "timeline_uncertainty_us": -1}):
            with self.subTest(changes=changes):
                Path(self.output).unlink(missing_ok=True)
                with self.assertRaises(ValueError):
                    self.run_arm(dict(self.ack, **changes))
                self.assertEqual(power.load_json(self.output)["state"], "arm_outcome_unknown")

    def test_collect_evidenced_two_resume_chain_banked_before_finish(self):
        acquisition, results, live = sleep_evidence()
        self.owner["active_label"] = "watcher-sweep"
        saved = self.collect_fake(acquisition, results, live)
        self.assertEqual(self.observed_ops, ["status", "end", "status", "finish"])
        self.assertEqual(saved["boot_provenance"]["retrieval_boot_id"], 9)
        self.assertEqual(len(saved["boot_provenance"]["resumes"]), 2)
        self.assertEqual(saved["live_status"]["boot_id"], 9)
        sync = saved["post_clock_sync"]["best"]
        self.assertEqual(sync["device_us"], 9_000_000)
        self.assertEqual(sync["timeline_uncertainty_us"], 100_000)
        self.assertAlmostEqual(sync["uncertainty_s"], sync["uart_rtt_uncertainty_s"] + 0.1)

    def test_collect_holds_without_explicit_finish(self):
        self.collect_fake(*sleep_evidence(), release=False)
        self.assertEqual(self.observed_ops, ["status", "status"])

    def test_collect_legacy_peripheral_without_matrix_or_uncertainty_still_works(self):
        acquisition = {"arm": self.ack}
        results = {"run_id": "test-run", "run_boot_id": 7, "retrieval_boot_id": 7,
                   "state": "complete", "matrix_version": 1, "records": []}
        live = {"usb": True, "boot_id": 7}
        saved = self.collect_fake(acquisition, results, live)
        self.assertEqual(saved["boot_provenance"]["resume_count"], 0)
        self.assertEqual(saved["boot_provenance"]["timeline_uncertainty_us"], 0)
        self.assertIsNone(saved["post_clock_sync"])

    def test_collect_legacy_unexpected_boot_change_is_hard_failure(self):
        acquisition = {"arm": self.ack}
        results = {"run_id": "test-run", "run_boot_id": 7, "retrieval_boot_id": 8,
                   "state": "complete", "records": []}
        self.assert_collect_fails_without_mutation(acquisition, results, {"usb": True, "boot_id": 8})

    def test_collect_broken_chain_wrong_wake_reset_and_boots_fail_closed(self):
        cases = [("entry_boot_id", 6), ("resume_boot_id", 7),
                 ("wake_cause", "gpio"), ("resume_reset_reason", "brownout"),
                 ("programmed_us", 0), ("rtc_slept_us", -1)]
        for field, value in cases:
            with self.subTest(field=field):
                acquisition, results, live = sleep_evidence()
                results["records"][0]["deep_sleep"][field] = value
                self.assert_collect_fails_without_mutation(acquisition, results, live)
        for field in ("entry_boot_id", "resume_boot_id", "wake_cause", "resume_reset_reason"):
            with self.subTest(missing=field):
                acquisition, results, live = sleep_evidence()
                del results["records"][0]["deep_sleep"][field]
                self.assert_collect_fails_without_mutation(acquisition, results, live)
        for location, field, value in (("second", "entry_boot_id", 7),
                                       ("second", "resume_boot_id", 8),
                                       ("results", "run_boot_id", 8),
                                       ("results", "retrieval_boot_id", 8),
                                       ("results", "resume_count", 1),
                                       ("live", "boot_id", 10),
                                       ("live", "resume_count", 1),
                                       ("live", "run_id", "other")):
            with self.subTest(location=location, field=field):
                acquisition, results, live = sleep_evidence()
                target = results["records"][1]["deep_sleep"] if location == "second" else results if location == "results" else live
                target[field] = value
                self.assert_collect_fails_without_mutation(acquisition, results, live)

    def test_collect_successful_planned_deep_state_cannot_omit_resume_object(self):
        acquisition, results, live = sleep_evidence()
        del results["records"][0]["deep_sleep"]
        self.assert_collect_fails_without_mutation(acquisition, results, live)

    def test_collect_record_boot_cannot_disagree_with_chain(self):
        acquisition, results, live = sleep_evidence()
        results["records"][1]["boot_id"] = 7
        self.assert_collect_fails_without_mutation(acquisition, results, live)

    def test_collect_requires_sleep_uncertainty_everywhere(self):
        for location in ("results", "live", "record"):
            with self.subTest(location=location):
                acquisition, results, live = sleep_evidence()
                target = results if location == "results" else live if location == "live" else results["records"][0]
                del target["timeline_uncertainty_us"]
                self.assert_collect_fails_without_mutation(acquisition, results, live)
        for value in (-1, float("nan"), float("inf"), True, 1.0):
            with self.subTest(value=value):
                acquisition, results, live = sleep_evidence()
                results["records"][0]["timeline_uncertainty_us"] = value
                # save_json only banks acquisition, so invalid firmware JSON values
                # reach the validator through the mock without any permissive writer.
                self.assert_collect_fails_without_mutation(acquisition, results, live)

    def test_collect_rejects_uncertainty_or_offset_inconsistent_with_live(self):
        for field in ("timeline_offset_us", "timeline_uncertainty_us"):
            acquisition, results, live = sleep_evidence()
            live[field] += 1
            self.assert_collect_fails_without_mutation(acquisition, results, live)
        acquisition, results, live = sleep_evidence()
        results["records"][0]["timeline_uncertainty_us"] = 100_001
        self.assert_collect_fails_without_mutation(acquisition, results, live)

    def test_collect_matrix_echo_mismatch_is_hard_failure(self):
        for matrix, version in (("peripheral", 1), ("sleep", 1), ("sleep", True), (None, 2)):
            acquisition, results, live = sleep_evidence()
            results.update({"matrix": matrix, "matrix_version": version})
            self.assert_collect_fails_without_mutation(acquisition, results, live)

    def test_collect_omitted_cleanup_errors_cannot_claim_verified_shutdown(self):
        with self.assertRaisesRegex(RuntimeError, "shutdown could not be verified"):
            self.collect_fake(*sleep_evidence(), finish_result={"cleanup_errors": [], "cleanup_errors_omitted_count": 1})
        self.assertTrue(Path(self.output).exists())
        self.assertEqual(self.observed_ops[-1], "finish")

    def test_collect_still_holding_finish_reply_cannot_claim_release(self):
        with self.assertRaisesRegex(RuntimeError, "shutdown could not be verified"):
            self.collect_fake(*sleep_evidence(), finish_result={"holding": True, "cleanup_errors": []})
        self.assertTrue(Path(self.output).exists())

    def test_matrix_and_compare_parser_are_offline_until_dispatched(self):
        base = ["arm", "--port", "fake", "--ppk-control-dir", "fake", "--output", "fake"]
        self.assertEqual(power.parser().parse_args(base).matrix, "peripheral")
        self.assertEqual(power.parser().parse_args(base + ["--matrix", "sleep"]).matrix, "sleep")
        args = power.parser().parse_args(["compare", "--profile", "P", "--output", "O", "--pairs", "J"])
        self.assertEqual((args.action, args.profile, args.output, args.pairs), ("compare", "P", "O", "J"))


class ComparisonTests(unittest.TestCase):
    def setUp(self):
        definitions = [("warm_ref_0", "ref", "warm", 1.0, 100),
                       ("uart_hiz", "variant", "warm", 7.0, 200),
                       ("warm_ref_1", "ref", "warm", 3.0, 300)]
        self.profile = {"matrix": "sleep", "matrix_version": 2, "source_voltage_mv": 3872,
                        "plan": [], "rows": []}
        for index, (name, role, group, mean, count) in enumerate(definitions):
            self.profile["plan"].append({"index": index, "id": name, "role": role,
                                         "ref_group": group, "knobs": ["uart_hiz"] if role == "variant" else []})
            self.profile["rows"].append({"name": name, "plan_index": index, "repeat": 0,
                                         "quality": "receive_window_estimate", "mean_mA": mean,
                                         "sample_count": count, "measure_start_us": index * 10_000_000,
                                         "end_us": index * 10_000_000 + 5_000_000,
                                         "p50_mA": 9999})

    def comparison(self, pairs=None):
        return power.compare_profile(self.profile, pairs)["comparisons"][0]

    def test_count_weighted_baseline_delta_drift_and_no_quantiles(self):
        row = self.comparison()
        self.assertEqual(row["reference_mean_mA"], 2.5)
        self.assertEqual(row["reference_sample_count"], 400)
        self.assertEqual(row["delta_mean_mA"], 4.5)
        self.assertEqual(row["aba_drift_mA"], 2)
        self.assertFalse(row["unresolved"])
        self.assertNotIn("p50", power.json.dumps(row))
        self.assertEqual(row["ref_before"], "warm_ref_0")
        self.assertEqual(row["ref_after"], "warm_ref_1")

    def test_delta_inside_or_equal_drift_is_unresolved(self):
        for mean in (4.0, 4.5, 0.5):
            self.profile["rows"][1]["mean_mA"] = mean
            self.assertTrue(self.comparison()["unresolved"])

    def test_nearest_accepted_matching_refs_skip_rejected_and_wrong_group(self):
        row0 = copy.deepcopy(self.profile["rows"][0])
        row0.update({"name": "warm_ref_far", "plan_index": 3,
                     "measure_start_us": -20_000_000, "end_us": -10_000_000,
                     "mean_mA": 2, "sample_count": 300})
        self.profile["plan"].append({"index": 3, "id": "warm_ref_far", "role": "ref", "ref_group": "warm"})
        self.profile["rows"].insert(0, row0)
        self.profile["rows"][1]["quality"] = "rejected"
        cold = dict(row0, name="cold_i2s_low", plan_index=4, mean_mA=999,
                    measure_start_us=6_000_000, end_us=8_000_000)
        self.profile["plan"].append({"index": 4, "id": "cold_i2s_low", "role": "ref", "ref_group": "cold"})
        self.profile["rows"].insert(2, cold)
        row = self.comparison()
        self.assertEqual(row["ref_before"], "warm_ref_far")
        self.assertEqual(row["reference_mean_mA"], 2.5)

    def test_rejected_variant_or_missing_reference_never_produces_delta(self):
        self.profile["rows"][1]["quality"] = "rejected"
        self.assertIsNone(self.comparison()["delta_mean_mA"])
        self.profile["rows"][1]["quality"] = "receive_window_estimate"
        self.profile["rows"][2]["quality"] = "rejected"
        self.assertEqual(self.comparison()["quality"], "not_compared")

    def test_references_never_borrowed_across_repeats(self):
        self.profile["rows"][2]["repeat"] = 1
        self.assertEqual(self.comparison()["quality"], "not_compared")

    def test_plan_role_other_beats_reference_naming_or_knob_presence(self):
        self.profile["plan"][0].update({"role": "other", "knobs": ["untouched_cold"]})
        self.assertEqual(self.comparison()["quality"], "not_compared")

    def test_legacy_warm_reference_naming_and_knob_variant_supported(self):
        for entry in self.profile["plan"]:
            del entry["role"]
        self.assertEqual(self.comparison()["delta_mean_mA"], 4.5)

    def test_documented_legacy_naming_without_roles_or_groups_supported(self):
        for entry in self.profile["plan"]:
            del entry["role"]
            del entry["ref_group"]
        self.assertEqual(self.comparison()["delta_mean_mA"], 4.5)

    def test_explicit_pair_cannot_override_other_plan_role(self):
        self.profile["plan"][1]["role"] = "other"
        with self.assertRaisesRegex(ValueError, "non-variant"):
            self.comparison([{"variant": "uart_hiz", "ref_before": "warm_ref_0", "ref_after": "warm_ref_1"}])

    def test_explicit_pairs_support_ambiguous_plan_with_no_roles(self):
        self.profile["plan"] = []
        pairs = {"pairs": [{"variant": "uart_hiz", "ref_before": "warm_ref_0", "ref_after": "warm_ref_1"}]}
        self.assertEqual(self.comparison(pairs)["reference_mean_mA"], 2.5)

    def test_explicit_reference_typo_ambiguity_and_cross_group_rejected(self):
        spec = {"variant": "uart_hiz", "ref_before": "warm_ref_0", "ref_after": "warm_ref_1"}
        with self.assertRaisesRegex(ValueError, "ambiguous"):
            power.compare_profile(self.profile, [spec, spec])
        with self.assertRaisesRegex(ValueError, "does not exist"):
            power.compare_profile(self.profile, [dict(spec, ref_before="typo")])
        self.profile["plan"][0]["ref_group"] = "cold"
        with self.assertRaisesRegex(ValueError, "crosses reference groups"):
            power.compare_profile(self.profile, [spec])

    def test_accepted_mean_count_and_duplicate_identity_must_be_valid(self):
        for key, value in (("sample_count", 0), ("sample_count", True),
                           ("mean_mA", float("nan")), ("mean_mA", float("inf"))):
            profile = copy.deepcopy(self.profile)
            profile["rows"][0][key] = value
            with self.assertRaises(ValueError):
                power.compare_profile(profile)
        self.profile["rows"].append(copy.deepcopy(self.profile["rows"][0]))
        with self.assertRaisesRegex(ValueError, "duplicate"):
            power.compare_profile(self.profile)

    def test_references_must_bracket_on_virtual_timeline(self):
        self.profile["rows"][0]["end_us"] = 15_000_000
        with self.assertRaisesRegex(ValueError, "bracket"):
            self.comparison()

    def test_compare_rejects_peripheral_profile(self):
        self.profile.update({"matrix": "peripheral", "matrix_version": 1})
        with self.assertRaisesRegex(ValueError, "matrix"):
            self.comparison()

    def test_cli_dispatch_compare_is_offline_and_writes_report(self):
        import subprocess
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            profile_path, output_path = root / "profile.json", root / "comparison.json"
            power.save_json(profile_path, self.profile)
            process = subprocess.run([sys.executable, str(SOURCE), "compare", "--profile", str(profile_path),
                                      "--output", str(output_path)], capture_output=True, text=True, timeout=10)
            self.assertEqual(process.returncode, 0, process.stderr)
            self.assertEqual(power.load_json(output_path)["comparisons"][0]["delta_mean_mA"], 4.5)
            self.assertEqual(power.json.loads(process.stdout)["matched"], 1)

    def test_compare_file_cli_outputs_strict_json_and_refuses_overwrite(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            profile_path, output_path, pairs_path = root / "profile.json", root / "compare.json", root / "pairs.json"
            power.save_json(profile_path, self.profile)
            power.save_json(pairs_path, [{"variant": "uart_hiz", "ref_before": "warm_ref_0", "ref_after": "warm_ref_1"}])
            args = SimpleNamespace(profile=str(profile_path), output=str(output_path), pairs=str(pairs_path))
            with contextlib.redirect_stdout(io.StringIO()):
                power.compare(args)
            self.assertEqual(power.load_json(output_path)["comparisons"][0]["delta_mean_mA"], 4.5)
            with self.assertRaises(FileExistsError):
                power.compare(args)


class PreFaultPrefixTests(unittest.TestCase):
    def setUp(self):
        self.base = ReportTests("test_guarded_mean_units_and_integrals")
        self.base.setUp()
        self.final = self.base.bundle["ppk_final"]
        fault = {"kind": "queue_overflow", "message": "bounded queue full",
                 "arrival_monotonic_s": 110.0, "received_bytes": 2_002_044, "queued_bytes": 1024}
        self.final.update(total_valid_samples=500_000, max_consumer_delay_s=0.5,
                          max_observed_backlog_bytes=4_000_000,
                          receiver={"fault": fault, "consumed_bytes": 2_000_000,
                                    "received_bytes": 2_003_044, "discarded_bytes": 2020,
                                    "queued_bytes": 1024, "invalidated_queue_bytes": 1024,
                                    "max_combined_backlog_bytes": 4_000_000,
                                    "max_consumer_delay_s": 0.5})
        self.final["fault"] = "raw receiver: queue_overflow: bounded queue full"
        for frame in self.base.frames:
            frame["max_observed_backlog_bytes"] = 4000

    def report(self):
        return self.base.report(pre_fault_prefix=True)

    def surplus(self, amount=32164):
        receiver = self.final["receiver"]
        receiver["consumed_bytes"] += amount
        receiver["fault"]["received_bytes"] += amount
        receiver["received_bytes"] += amount

    def test_default_still_rejects_fault_and_flag_reports_exact_prefix(self):
        old = self.base.report()
        self.assertEqual(old["rows"][0]["quality"], "rejected")
        self.assertNotIn("transport_fault_cutoff", old)
        report = self.report()
        row = report["rows"][0]
        self.assertEqual(row["quality"], "receive_window_estimate")
        self.assertEqual(row["mean_mA"], 1)
        cutoff = report["transport_fault_cutoff"]
        self.assertEqual(cutoff["fault_host_offset_s"], 10)
        self.assertEqual(cutoff["cutoff_host_offset_s"], 9)
        self.assertEqual(cutoff["rows_before_cutoff"], 1)
        self.assertEqual(row["largest_observed_backlog_s"], 0.01)
        self.assertEqual(report["transport_fault"], self.final["fault"])

    def test_larger_session_or_receiver_delay_controls_cutoff(self):
        for session, receiver in ((2, 1), (1, 2)):
            with self.subTest(session=session):
                self.final["max_consumer_delay_s"] = session
                self.final["receiver"]["max_consumer_delay_s"] = receiver
                cutoff = self.report()["transport_fault_cutoff"]
                self.assertEqual(cutoff["max_consumer_delay_s"], 2)
                self.assertEqual(cutoff["cutoff_host_offset_s"], 7.5)

    def test_guarded_end_equal_cutoff_allowed_but_later_row_not_cropped(self):
        self.final["receiver"]["fault"]["arrival_monotonic_s"] = 105
        self.final["max_consumer_delay_s"] = 1
        self.final["receiver"]["max_consumer_delay_s"] = 1
        record = self.base.bundle["firmware"]["records"][0]
        self.base.bundle["firmware"]["records"].append(dict(record, name="later", end_us=5_100_000))
        report = self.report()
        self.assertEqual(report["transport_fault_cutoff"]["cutoff_host_offset_s"], 3.5)
        self.assertEqual(report["rows"][0]["quality"], "receive_window_estimate")
        self.assertEqual(report["rows"][1]["quality"], "rejected")
        self.assertIsNone(report["rows"][1]["mean_mA"])
        self.assertIn("cutoff", report["rows"][1]["reason"])
        self.assertEqual(report["transport_fault_cutoff"]["rows_before_cutoff"], 1)

    def test_raw_read_error_is_allowed_but_not_writer_or_decoder_fault(self):
        fault = self.final["receiver"]["fault"]
        fault.update(kind="read_error", message="OSError: raw read failed", received_bytes=2_001_024)
        self.final["fault"] = "raw receiver: read_error: OSError: raw read failed"
        self.assertEqual(self.report()["rows"][0]["quality"], "receive_window_estimate")
        for kind in ("write_error", "decode_error", "stale_data", "unknown"):
            fault["kind"] = kind
            self.final["fault"] = "raw receiver: " + kind + ": OSError: raw read failed"
            with self.assertRaises(ValueError):
                self.report()
        fault["kind"] = "read_error"
        self.final["fault"] = "aggregate writer failed"
        with self.assertRaises(ValueError):
            self.report()

    def test_invalid_or_missing_fault_time_delay_and_consumed_evidence(self):
        original = copy.deepcopy(self.final)
        cases = [("fault", "arrival_monotonic_s", value) for value in (None, True, float("nan"), float("inf"), 99)]
        cases += [("final", "max_consumer_delay_s", value) for value in (None, True, -1, float("inf"))]
        cases += [("receiver", "consumed_bytes", value) for value in (None, True, -1, 2_000_000.0, 1_999_999)]
        for where, field, value in cases:
            with self.subTest(where=where, value=value):
                self.final.clear()
                self.final.update(copy.deepcopy(original))
                target = self.final if where == "final" else self.final["receiver"] if where == "receiver" else self.final["receiver"]["fault"]
                target[field] = value
                with self.assertRaises(ValueError):
                    self.report()

    def test_in_flight_batch_requires_conservation_and_is_bounded(self):
        self.surplus()
        proof = self.report()["transport_fault_cutoff"]["byte_accounting"]
        self.assertEqual(proof["un_ingested_or_partial_bytes"], 32164)
        self.assertEqual(proof["retained_invalidated_queue_bytes"], 1024)
        self.assertEqual(proof["trigger_discard_bytes"], 1020)
        original = copy.deepcopy(self.final)
        for field in ("received_bytes", "discarded_bytes", "queued_bytes", "invalidated_queue_bytes"):
            self.final.clear()
            self.final.update(copy.deepcopy(original))
            self.final["receiver"][field] += 1
            with self.assertRaises(ValueError):
                self.report()
        self.final.clear()
        self.final.update(original)
        del self.final["receiver"]["invalidated_queue_bytes"]
        with self.assertRaises(ValueError):
            self.report()
        self.setUp()
        self.surplus(power.MAX_BATCH_BYTES + 4)
        with self.assertRaises(ValueError):
            self.report()

    def test_exact_accounting_cannot_ignore_a_contradictory_fault_boundary(self):
        self.final["receiver"]["fault"]["received_bytes"] = 1000
        with self.assertRaises(ValueError):
            self.report()

    def test_read_error_surplus_requires_zero_trigger_discard(self):
        self.surplus()
        receiver = self.final["receiver"]
        receiver["fault"].update(kind="read_error", message="OSError")
        self.final["fault"] = "raw receiver: read_error: OSError"
        with self.assertRaises(ValueError):
            self.report()
        receiver["fault"]["received_bytes"] = receiver["consumed_bytes"] + receiver["queued_bytes"]
        self.assertEqual(self.report()["rows"][0]["quality"], "receive_window_estimate")

    def test_new_missing_invalid_or_late_counts_are_never_excused(self):
        for key in ("decoded_missing_frame_count", "total_invalid_samples", "late_unassigned_sample_count"):
            self.final[key] = 1
            with self.assertRaises(ValueError):
                self.report()
            self.final[key] = 0

    def test_bad_coverage_gap_backlog_and_delay_checks_are_unchanged(self):
        for mode in ("coverage", "gap", "backlog", "delay", "missing_backlog"):
            self.setUp()
            if mode == "coverage":
                for i, frame in enumerate(self.base.frames):
                    frame.update(sample_count=10500, sample_index_start=i*10500, sample_index_end=(i+1)*10500)
                self.final["total_valid_samples"] = 525000
                self.final["receiver"]["consumed_bytes"] = 2100000
                self.final["receiver"]["received_bytes"] += 100000
                self.final["receiver"]["fault"]["received_bytes"] += 100000
            elif mode == "gap":
                del self.base.frames[25]
            elif mode == "backlog":
                self.base.frames[25]["max_observed_backlog_bytes"] = 400000
            elif mode == "delay":
                self.base.frames[25]["label_max_consumer_delay_s"] = 1
            else:
                del self.base.frames[25]["max_observed_backlog_bytes"]
            with self.subTest(mode=mode):
                self.assertEqual(self.report()["rows"][0]["quality"], "rejected")

    def test_cli_flag_is_opt_in_and_banks_the_cutoff(self):
        import subprocess
        header = {"type": "session", "schema_version": 1, "sample_rate_hz": 100000,
                  "host_monotonic_origin_s": 100, "voltage_mv": 3872,
                  "port_descriptor": {"port": "fake-ppk"}}
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            results, samples = root / "results.json", root / "samples.jsonl"
            power.save_json(results, self.base.bundle)
            samples.write_text("".join(power.json.dumps(frame) + "\n" for frame in [header, self.base.begin] + self.base.frames))
            for enabled in (False, True):
                output = root / ("prefix.json" if enabled else "default.json")
                command = [sys.executable, str(SOURCE), "analyze", "--results", str(results),
                           "--samples", str(samples), "--output", str(output)]
                if enabled:
                    command.append("--pre-fault-prefix")
                process = subprocess.run(command, capture_output=True, text=True, timeout=10)
                self.assertEqual(process.returncode, 0, process.stderr)
                report = power.load_json(output)
                self.assertEqual(report["rows"][0]["quality"], "receive_window_estimate" if enabled else "rejected")
                self.assertEqual("transport_fault_cutoff" in report, enabled)

    def test_window_beyond_decoded_ordinals_and_no_fault_flag_fail_closed(self):
        self.final["total_valid_samples"] = 499999
        self.final["receiver"]["consumed_bytes"] = 4*499999
        with self.assertRaises(ValueError):
            self.report()
        self.setUp()
        self.final["fault"] = None
        with self.assertRaises(ValueError):
            self.report()
        options = power.parser().parse_args(["analyze", "--results", "R", "--samples", "S", "--output", "O", "--pre-fault-prefix"])
        self.assertTrue(options.pre_fault_prefix)


class DryrunTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.output = str(Path(self.tmp.name) / "dryrun.json")
        self.args = SimpleNamespace(port="fake-watcher", matrix="sleep", output=self.output, timeout_s=60)
        self.before = {"schema": 1, "type": "status", "usb": True, "boot_id": 7, "state": "idle"}
        self.after = dict(self.before)
        common = {"schema": 1, "matrix": "sleep", "matrix_version": 2,
                  "boot_id": 7, "usb": True, "last_error_log": "", "pm_exercised": False}
        self.states = [dict(common, type="dryrun_state", index=i, id="state_" + str(i), error=0,
                            codec_regs={"dac": {"00": 0}, "adc": {"00": None}, "adc_variant": "unknown"},
                            codec_regs_expected=False, knobs_applied={}, codec_state="cold") for i in range(24)]
        self.done = dict(common, type="dryrun_done", states=24, errors=0, restore_error=0)
        self.lines = None
        self.commands = []
        self.pending = []
        self.closed = False
        test = self

        class Board:
            def __enter__(self):
                return self

            def __exit__(self, *args):
                test.closed = True

            def wake(self):
                pass

            def write_line(self, command):
                test.commands.append(command)
                if command.startswith("ptest.status="):
                    frame = test.before if len(test.commands) == 1 else test.after
                    # A stale status must not satisfy either nonced query.
                    test.pending.extend([test.line(dict(frame, request_id="stale")),
                                         test.line(dict(frame, request_id=command.split("=", 1)[1]))])
                else:
                    self.assert_bank_before_command()
                    test.pending.extend(test.lines if test.lines is not None else
                                        ["ordinary diagnostic line"] + [test.line(s) for s in test.states] + [test.line(test.done)])

            def assert_bank_before_command(self):
                bank = power.load_json(test.output)
                test.assertEqual(bank["status_before"]["boot_id"], 7)
                test.assertFalse(bank["summary"]["ok"])

            def read_line(self, deadline):
                return test.pending.pop(0) if test.pending else None

        self.board = Board()
        self.chat = SimpleNamespace(Board=lambda port: self.board)

    def tearDown(self):
        self.tmp.cleanup()

    def line(self, frame):
        return "PTEST " + power.json.dumps(frame)

    def run_dryrun(self, *, fails=False):
        with patch.dict(sys.modules, {"chat": self.chat}), \
             patch.object(power, "send_command", side_effect=AssertionError("dryrun must never touch PPK2")), \
             contextlib.redirect_stdout(io.StringIO()):
            if fails:
                with self.assertRaises(RuntimeError):
                    power.dryrun(self.args)
            else:
                power.dryrun(self.args)
        return power.load_json(self.output)

    def test_usb_diagnostic_banks_all_lines_and_expected_false_is_not_error(self):
        bank = self.run_dryrun()
        self.assertTrue(bank["summary"]["ok"])
        self.assertFalse(bank["quantitative"])
        self.assertFalse(bank["pm_exercised"])
        self.assertEqual(len(bank["states"]), 24)
        self.assertFalse(bank["states"][0]["codec_regs_expected"])
        self.assertEqual(bank["states"][0]["codec_regs"], self.states[0]["codec_regs"])
        self.assertIn("ordinary diagnostic line", bank["raw_lines"])
        self.assertEqual(len(bank["raw_lines"]), 30)
        self.assertEqual(self.commands[1], 'ptest.dryrun={"matrix":"sleep"}')
        self.assertTrue(self.commands[0].startswith("ptest.status="))
        self.assertTrue(self.commands[2].startswith("ptest.status="))
        self.assertNotEqual(self.commands[0], self.commands[2])
        self.assertTrue(self.closed)

    def test_missing_usb_or_active_run_never_sends_dryrun(self):
        for change in ({"usb": False}, {"usb": 1}, {"power_error": -1}, {"state": "armed"}, {"state": "running"}, {"boot_id": True}):
            with self.subTest(change=change):
                self.before.update(change)
                bank = self.run_dryrun(fails=True)
                self.assertIsNone(bank["done"])
                self.assertEqual(len(self.commands), 1)
                Path(self.output).unlink()
                self.commands.clear()
                self.before = {"schema": 1, "type": "status", "usb": True, "boot_id": 7, "state": "idle"}

    def test_error_states_and_restore_errors_bank_complete_output_before_failure(self):
        self.states[0]["error"] = -1
        self.states[0]["last_error_log"] = "I2C transport error"
        self.done.update(errors=1, restore_error=-1)
        bank = self.run_dryrun(fails=True)
        self.assertEqual(len(bank["states"]), 24)
        self.assertEqual(bank["done"], self.done)
        self.assertEqual(bank["states"][0]["last_error_log"], "I2C transport error")
        self.assertIsNotNone(bank["status_after"])
        self.assertFalse(bank["summary"]["ok"])

    def test_timeout_banks_prefix_and_queries_status_after(self):
        self.lines = [self.line(self.states[0])]
        bank = self.run_dryrun(fails=True)
        self.assertEqual(len(bank["states"]), 1)
        self.assertIsNone(bank["done"])
        self.assertIsNotNone(bank["status_after"])
        self.assertIn("UNKNOWN", " ".join(bank["summary"]["errors"]))

    def test_strict_json_rejects_nonfinite_duplicate_and_nonobject_frames(self):
        for payload in ('{"schema":1,"schema":1}', '{"extra":NaN}', '{"extra":Infinity}',
                        '{"extra":1e999}', '[]', '{invalid'):
            with self.subTest(payload=payload):
                self.lines = ["PTEST " + payload]
                bank = self.run_dryrun(fails=True)
                self.assertIn("PTEST " + payload, bank["raw_lines"])
                self.assertIsNotNone(bank["status_after"])
                Path(self.output).unlink()
                self.commands.clear()

    def test_malformed_matrix_boot_order_error_and_log_fail_closed(self):
        for change in ({"schema": True}, {"matrix_version": True}, {"matrix": "peripheral"},
                       {"boot_id": 8}, {"index": True}, {"index": 2}, {"error": False},
                       {"error": 0.0}, {"codec_regs_expected": 1}, {"last_error_log": "x" * 97},
                       {"last_error_log": "bad\nlog"}, {"pm_exercised": True}):
            with self.subTest(change=change):
                self.lines = [self.line(dict(self.states[0], **change))]
                bank = self.run_dryrun(fails=True)
                self.assertFalse(bank["summary"]["ok"])
                Path(self.output).unlink()
                self.commands.clear()

    def test_duplicates_missing_states_and_false_summary_cannot_pass(self):
        cases = [self.states[:1] + self.states,
                 self.states[:-1],
                 [dict(s, id="duplicate") for s in self.states]]
        for states in cases:
            with self.subTest(states=len(states)):
                self.lines = [self.line(s) for s in states] + [self.line(dict(self.done, states=len(states)))]
                self.assertFalse(self.run_dryrun(fails=True)["summary"]["ok"])
                Path(self.output).unlink()
                self.commands.clear()

    def test_bad_summary_or_extra_done_fails_but_retains_evidence(self):
        for change in ({"states": 23}, {"states": True}, {"errors": True}, {"errors": -1},
                       {"restore_error": False}, {"usb": False}, {"boot_id": 8}):
            with self.subTest(change=change):
                self.lines = [self.line(s) for s in self.states] + [self.line(dict(self.done, **change))]
                self.assertFalse(self.run_dryrun(fails=True)["summary"]["ok"])
                Path(self.output).unlink()
                self.commands.clear()
        self.lines = [self.line(s) for s in self.states] + [self.line(self.done), self.line(self.done)]
        bank = self.run_dryrun(fails=True)
        self.assertEqual(bank["raw_lines"].count(self.line(self.done)), 2)

    def test_post_done_transmit_error_is_not_ignored_by_status_request(self):
        import runpy
        error = {"schema": 1, "type": "error", "error": -1,
                 "where": "dryrun_done_tx_failed"}
        self.lines = [self.line(s) for s in self.states] + [self.line(self.done), self.line(error)]
        argv = [str(SOURCE), "dryrun", "--port", "fake-watcher", "--matrix", "sleep", "--output", self.output]
        with patch.dict(sys.modules, {"chat": self.chat}), patch.object(sys, "argv", argv), \
             patch("tools.power.ppk2_control.send_command", side_effect=AssertionError("no PPK2")), \
             contextlib.redirect_stdout(io.StringIO()):
            with self.assertRaises(SystemExit) as exited:
                runpy.run_path(str(SOURCE), run_name="__main__")
        self.assertNotEqual(exited.exception.code, 0)
        self.assertIn("dryrun_done_tx_failed", str(exited.exception.code))
        bank = power.load_json(self.output)
        self.assertEqual(bank["done"], self.done)
        self.assertEqual(len(bank["states"]), 24)
        self.assertIn(self.line(error), bank["raw_lines"])
        self.assertFalse(bank["summary"]["ok"])
        self.assertIsNone(bank["status_after"])
        self.assertIn("dryrun_done_tx_failed", " ".join(bank["summary"]["errors"]))
        self.assertTrue(self.commands[-1].startswith("ptest.status="))

    def test_usb_loss_reboot_or_active_run_after_completion_fails(self):
        for change in ({"usb": False}, {"boot_id": 8}, {"state": "armed"}):
            with self.subTest(change=change):
                self.after = dict(self.before, **change)
                self.assertFalse(self.run_dryrun(fails=True)["summary"]["ok"])
                Path(self.output).unlink()
                self.commands.clear()

    def test_rejected_command_and_oversized_transcript_bank_failures(self):
        for lines in ([self.line({"schema": 1, "type": "error", "error": "unsupported"})],
                      ["x" * (power.MAX_JSON_BYTES + 1)]):
            self.lines = lines
            bank = self.run_dryrun(fails=True)
            self.assertFalse(bank["summary"]["ok"])
            Path(self.output).unlink()
            self.commands.clear()

    def test_board_open_and_after_status_failures_are_banked(self):
        class BoardError(Exception):
            pass
        with patch.dict(sys.modules, {"chat": self.chat}), \
             patch.object(self.chat, "Board", side_effect=BoardError("open failed")):
            with self.assertRaises(RuntimeError):
                power.dryrun(self.args)
        bank = power.load_json(self.output)
        self.assertFalse(bank["summary"]["ok"])
        self.assertIn("open failed", bank["summary"]["errors"])
        Path(self.output).unlink()
        self.after["schema"] = True
        bank = self.run_dryrun(fails=True)
        self.assertFalse(bank["summary"]["ok"])
        self.assertIsNone(bank["status_after"])

    def test_output_exclusive_and_invalid_timeout_never_open_board(self):
        Path(self.output).write_text("keep", encoding="utf-8")
        with patch.dict(sys.modules, {"chat": self.chat}), patch.object(self.chat, "Board") as board:
            with self.assertRaises(FileExistsError):
                power.dryrun(self.args)
            board.assert_not_called()
        self.assertEqual(Path(self.output).read_text(), "keep")
        for timeout in (0, float("nan"), float("inf"), 301, True):
            self.args.timeout_s = timeout
            with self.assertRaises(ValueError):
                power.dryrun(self.args)
        options = power.parser().parse_args(["dryrun", "--port", "fake", "--matrix", "sleep", "--output", self.output])
        self.assertEqual(options.timeout_s, 60)

    def test_cli_dispatch_nonzero_without_hardware_on_invalid_timeout(self):
        import subprocess
        result = subprocess.run([sys.executable, str(SOURCE), "dryrun", "--port", "fake", "--matrix", "sleep",
                                 "--output", self.output, "--timeout-s", "nan"], capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("dryrun timeout", result.stderr)
        self.assertNotIn("PPK2 owner", result.stderr)


class FirmwareProtocolIntegrationTests(unittest.TestCase):
    """Real C fake emitter -> strict host provenance/diagnostics -> analysis."""
    def test_emitted_dryrun_protocol_is_consumable_without_hardware(self):
        import subprocess
        root = SOURCE.parents[2]
        nonces = ["ab" * 8, "cd" * 8]
        commands = ["ptest.status=" + nonces[0], 'ptest.dryrun={"matrix":"sleep"}',
                    "ptest.status=" + nonces[1]]
        with tempfile.TemporaryDirectory(prefix="watcher-host-dryrun-") as directory:
            binary = Path(directory) / "harness"
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            "-I", str(root / "tests/watcher_power_test_fakes"),
                            str(root / "tests/watcher_power_test_harness.c"), "-o", str(binary)],
                           check=True, capture_output=True, text=True, timeout=30)
            process = subprocess.run([str(binary)], input="\n".join(">" + command for command in commands) + "\n",
                                     capture_output=True, text=True, check=True, timeout=10)
            lines = process.stdout.splitlines()
            emitted = [power.strict_json_object(line[6:]) for line in lines]
            state_frames = [frame for frame in emitted if frame["type"] == "dryrun_state"]
            done = next(frame for frame in emitted if frame["type"] == "dryrun_done")
            self.assertEqual(len(state_frames), 24)
            self.assertEqual(done["errors"], 0)
            self.assertEqual(done["restore_error"], 0)
            cursor = iter(lines)
            observed = []

            class Board:
                def __enter__(self):
                    return self

                def __exit__(self, *args):
                    pass

                def wake(self):
                    pass

                def write_line(self, command):
                    observed.append(command)

                def read_line(self, deadline):
                    return next(cursor, None)

            output = str(Path(directory) / "diagnostics.json")
            args = SimpleNamespace(port="firmware-fake-only", matrix="sleep", output=output, timeout_s=60)
            with patch.dict(sys.modules, {"chat": SimpleNamespace(Board=lambda port: Board())}), \
                 patch.object(power.secrets, "token_hex", side_effect=nonces), \
                 patch.object(power, "send_command", side_effect=AssertionError("no PPK2 in dryrun")), \
                 contextlib.redirect_stdout(io.StringIO()):
                power.dryrun(args)
            bank = power.load_json(output)
        self.assertEqual(observed, commands)
        self.assertEqual(bank["raw_lines"], lines)
        self.assertEqual(bank["states"], state_frames)
        self.assertEqual(bank["done"], done)
        self.assertTrue(bank["summary"]["ok"])
        self.assertFalse(bank["quantitative"])
        self.assertTrue(all("codec_regs" in frame and "last_error_log" in frame for frame in state_frames))

    def test_emitted_sleep_protocol_and_plan_are_consumable_end_to_end(self):
        import subprocess
        root = SOURCE.parents[2]
        with tempfile.TemporaryDirectory(prefix="watcher-host-protocol-") as directory:
            binary = Path(directory) / "harness"
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            "-I", str(root / "tests/watcher_power_test_fakes"),
                            str(root / "tests/watcher_power_test_harness.c"), "-o", str(binary)],
                           check=True, capture_output=True, text=True, timeout=30)
            config = {"run_id": "bench-host", "settle_ms": 1000, "capture_ms": 5000,
                      "repeats": 1, "matrix": "sleep"}
            commands = ["ptest.status", "ptest.arm=" + power.json.dumps(config), "usb 0",
                        "advance 300000", "deep_resume 6000 5 4", "deep_resume 6000 5 4",
                        "usb 1", "ptest.status", "ptest.results"]
            # The firmware fake uses the real console's '>' prefix.
            commands = [">" + line if line.startswith("ptest.") else line for line in commands]
            process = subprocess.run([str(binary)], input="\n".join(commands) + "\n",
                                     capture_output=True, text=True, check=True, timeout=10)
        emitted = [power.json.loads(line[6:]) for line in process.stdout.splitlines()]
        initial, ack, live, firmware = emitted
        self.assertEqual(firmware["state"], "complete")
        self.assertEqual(firmware["resume_count"], 2)
        acquisition = {"state": "armed", "arm": ack, "requested_config": config}
        provenance = power.verify_resume_chain(acquisition, firmware, live)
        self.assertEqual(provenance["retrieval_boot_id"], live["boot_id"])
        self.assertEqual(len(provenance["resumes"]), 2)

        base = ReportTests("test_guarded_mean_units_and_integrals")
        base.setUp()
        acquisition.update(base.bundle["acquisition"])
        acquisition["clock_sync"]["best"].update({"boot_id": initial["boot_id"],
                                                  "device_us": initial["now_us"]})
        bundle = dict(base.bundle, acquisition=acquisition, firmware=firmware, live_status=live)
        count = (firmware["finished_us"] + 100_000 - 1) // 100_000
        windows = []
        for index in range(count):
            frame = dict(base.frames[0], window_index=index, host_start_offset_s=index / 10,
                         sample_index_start=index * 10_000, sample_index_end=(index + 1) * 10_000)
            # Synthetic current only: references=1mA, variants=2mA. This is a
            # protocol/statistics test, never an electrical or hardware result.
            device_at = index * 100_000 + initial["now_us"]
            variant = any(record["measure_start_us"] <= device_at < record["end_us"]
                          and ack["plan"][record["index"]]["role"] == "variant"
                          for record in firmware["records"])
            mean = 2000 if variant else 1000
            frame.update({"mean_uA": mean, "min_uA": mean - 100, "max_uA": mean + 100,
                          "sampled_charge_mAh": mean * 0.1 / 3_600_000,
                          "sampled_energy_mWh": mean * 0.1 * 3872 / 3_600_000_000})
            windows.append(frame)
        profile = base.report(bundle, windows)
        self.assertEqual(profile["plan"], power.capture_plan(acquisition, firmware))
        deep_rows = [row for row in profile["rows"] if row.get("deep_sleep")]
        self.assertEqual(len(deep_rows), 2)
        self.assertTrue(all(row["quality"] == "receive_window_estimate" for row in deep_rows))
        comparisons = power.compare_profile(profile)["comparisons"]
        variants = [entry for entry in ack["plan"] if entry["role"] == "variant"]
        self.assertEqual(len(comparisons), len(variants))
        matched = [row for row in comparisons if row["quality"] == "matched_receive_window_difference"]
        self.assertTrue(matched)
        for row in matched:
            self.assertNotEqual(row["ref_before"], "cold_ref")
            self.assertNotEqual(row["ref_after"], "cold_ref")
            self.assertAlmostEqual(row["delta_mean_mA"], 1)
            self.assertEqual(row["aba_drift_mA"], 0)
            self.assertFalse(row["unresolved"])


if __name__ == "__main__":
    unittest.main()
