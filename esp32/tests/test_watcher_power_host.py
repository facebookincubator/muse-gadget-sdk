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
        results = {"run_id": "test-run", "run_boot_id": 7, "state": "complete"}
        observed_ops = []
        def owner_command(directory, command):
            observed_ops.append(command["op"])
            if command["op"] == "finish":
                self.assertTrue(Path(self.output).exists())
            return self.owner
        with patch.dict(sys.modules, {"chat": self.chat}), \
             patch.object(power, "send_command", side_effect=owner_command), \
             patch.object(power, "request", side_effect=[{"usb": True}, results]), \
             contextlib.redirect_stdout(io.StringIO()):
            power.collect(self.args)
        self.assertEqual(observed_ops, ["status", "status", "finish"])
        self.assertTrue(power.load_json(self.output)["reconnected_usb_observed"])


if __name__ == "__main__":
    unittest.main()
