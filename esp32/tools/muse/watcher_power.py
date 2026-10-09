#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy at http://www.apache.org/licenses/LICENSE-2.0
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""Arm a USB-free Watcher sweep without interrupting the existing PPK2 owner.

Start tools/power/ppk2_profile.py hold first. This tool only controls that
owner through a private host FIFO; it NEVER opens the PPK2 serial port.
arm -> unplug USB -> wait the conservative bound -> reconnect USB -> collect
-> analyze. A failed arm/collect leaves the PPK2 owner powered and alive.
See docs/power/sensecap-watcher.md for electrical prerequisites and limitations.
"""

import argparse
import json
import math
import os
from pathlib import Path
import secrets
import sys
import time

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent.parent))
sys.path.insert(0, str(HERE))
from tools.power.ppk2_control import send_command  # noqa: E402

MAX_JSON_BYTES = 128 * 1024


def load_json(path):
    with open(path, encoding="utf-8") as stream:
        data = stream.read(MAX_JSON_BYTES + 1)
    if len(data.encode()) > MAX_JSON_BYTES:
        raise ValueError("metadata exceeds 128 KiB")
    return json.loads(data)


def save_json(path, obj):
    with open(path, "x", encoding="utf-8") as stream:
        json.dump(obj, stream, indent=2, allow_nan=False)
        stream.write("\n")


def request(board, command, kind, timeout_s=5.0):
    request_id = secrets.token_hex(8) if command == "ptest.status" else None
    if request_id is not None:
        command += "=" + request_id
    board.wake()
    board.write_line(command)
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        line = board.read_line(deadline)
        if line is None:
            break
        at = line.find("PTEST ")
        if at < 0:
            continue
        if len(line) > MAX_JSON_BYTES:
            raise ValueError("oversized firmware reply")
        frame = json.loads(line[at + 6:])
        if frame.get("schema") != 1:
            raise ValueError("unsupported PTEST schema")
        if frame.get("type") == "error":
            raise RuntimeError(f"Watcher rejected command: {frame}")
        if frame.get("type") == kind:
            if request_id is not None and frame.get("request_id") != request_id:
                continue  # stale/late response is not this request's clock evidence
            return frame
    raise TimeoutError("Watcher did not acknowledge; command outcome UNKNOWN. Query ptest.status; do not re-arm blindly")


def synchronize(board, count=3):
    """Bound a same-host monotonic/ESP clock mapping by each UART round trip."""
    observations = []
    for _ in range(count):
        start = time.monotonic()
        status = request(board, "ptest.status", "status")
        end = time.monotonic()
        if status.get("usb") is not True:
            raise ValueError("Watcher must report actual USB power before arming")
        now_us = status.get("now_us")
        if type(now_us) is not int or type(status.get("boot_id")) is not int:
            raise ValueError("firmware lacks clock/boot evidence")
        observations.append({
            "boot_id": status["boot_id"], "device_us": now_us,
            "host_start_s": start, "host_end_s": end,
            "offset_s": (start + end) / 2 - now_us / 1e6,
            "uncertainty_s": (end - start) / 2,
        })
    if len({x["boot_id"] for x in observations}) != 1:
        raise ValueError("Watcher rebooted during synchronization")
    best = min(observations, key=lambda x: x["uncertainty_s"])
    return {"best": best, "observations": observations,
            "clock": "same-host monotonic seconds mapped to ESP boot microseconds by bounded UART RTT"}


def arm(args):
    import chat
    # Reserve a writable output before touching recording/firmware state.
    with open(args.output, "x", encoding="utf-8") as stream:
        _arm(args, chat, stream)


def persist_metadata(stream, metadata):
    stream.seek(0)
    json.dump(metadata, stream, indent=2, allow_nan=False)
    stream.write("\n")
    stream.truncate()
    stream.flush()
    os.fsync(stream.fileno())


def _arm(args, chat, stream):
    persist_metadata(stream, {"schema": 1, "type": "watcher-acquisition", "state": "preparing"})
    owner = send_command(args.ppk_control_dir, {"op": "status"})
    if not owner.get("holding") or owner.get("fault"):
        raise ValueError("PPK2 owner is not ready and fault-free; keep DUT USB plugged in")
    if owner.get("active_label"):
        raise ValueError("PPK2 already recording: do not commandeer an active acquisition")
    if not finite_number(owner.get("host_monotonic_origin_s")):
        raise ValueError("PPK2 owner lacks local clock origin")
    if not finite_number(owner.get("total_valid_samples")) or owner["total_valid_samples"] <= 0:
        raise ValueError("PPK2 owner has no valid sample evidence; keep DUT USB plugged in")
    if (not finite_number(owner.get("elapsed_host_s"))
            or not finite_number(owner.get("last_data_host_offset_s"))
            or not 0 <= owner["elapsed_host_s"] - owner["last_data_host_offset_s"] <= 1):
        raise ValueError("PPK2 sample stream is stale; keep DUT USB plugged in")
    with chat.Board(args.port) as board:
        sync = synchronize(board)
        # No OFF, mode change, serial close, or reconnect occurs here.
        recording = send_command(args.ppk_control_dir, {"op": "begin", "label": "watcher-sweep"})
        config = {"run_id": args.run_id or secrets.token_hex(8),
                  "settle_ms": args.settle_ms, "capture_ms": args.capture_ms,
                  "repeats": args.repeats}
        metadata = {"schema": 1, "type": "watcher-acquisition", "state": "arm_outcome_unknown",
                    "arm": {"run_id": config["run_id"], "boot_id": sync["best"]["boot_id"]},
                    "requested_config": config, "clock_sync": sync, "ppk_before_arm": recording,
                    "timing_limitations": "UART RTT, device VBUS polling, PPK USB buffering and unknown sample loss; boundaries require guard/rejection"}
        # Bank recovery identity BEFORE a potentially unacknowledged arm.
        persist_metadata(stream, metadata)
        try:
            ack = request(board, "ptest.arm=" + json.dumps(config, separators=(",", ":")), "arm_ack")
        except BaseException:
            # Keep source power and the recording. Query status before recovery.
            raise
    if (ack.get("boot_id") != sync["best"]["boot_id"]
            or ack.get("run_id") != config["run_id"]
            or ack.get("usb") is not True or ack.get("state", ack.get("status")) != "armed"):
        raise ValueError("arming acknowledgment lacks matching run/boot/USB evidence; do not unplug")
    bound = ack.get("max_duration_ms")
    if type(bound) is not int or not 1 <= bound <= 3_600_000:
        raise ValueError("firmware did not provide a bounded sweep duration")
    metadata.update({"state": "armed", "arm": ack})
    persist_metadata(stream, metadata)
    print(json.dumps({"ready_to_unplug": True, "run_id": ack["run_id"],
                      "ppk_holding": True, "minimum_wait_after_unplug_s": bound / 1000 + 10,
                      "next": "Remove ALL Watcher USB power connections, leave PPK2 USB/source connected. Wait the bound, reconnect Watcher USB, then collect. Completion is not proven until firmware results are retrieved."}, indent=2))


def collect(args):
    import chat
    if Path(args.output).exists():
        raise FileExistsError("choose a fresh collected-results path")
    acquisition = load_json(args.acquisition)
    owner = send_command(args.ppk_control_dir, {"op": "status"})
    if not owner.get("holding"):
        raise ValueError("continuous PPK2 owner was lost; reject seamless-handoff claim")
    original_owner = acquisition["ppk_before_arm"]
    for key in ("host_monotonic_origin_s", "voltage_mv", "owner_port"):
        if key not in original_owner or owner.get(key) != original_owner[key]:
            raise ValueError("PPK2 owner identity/clock or voltage changed; keep source ON and reject continuity")
    with chat.Board(args.port) as board:
        status = request(board, "ptest.status", "status")
        if status.get("usb") is not True:
            raise ValueError("Watcher does not confirm actual USB reconnection; PPK2 remains ON")
        results = request(board, "ptest.results", "results")
    if results.get("run_id") != acquisition["arm"].get("run_id"):
        raise ValueError("results do not belong to this acquisition")
    if results.get("run_boot_id") != acquisition["arm"].get("boot_id"):
        raise ValueError("result boot identity differs from synchronized acquisition")
    if results.get("state") in ("idle", "armed", "running"):
        raise ValueError("sweep is not finished; do not release PPK2 or publish a profile")
    if owner.get("active_label") == "watcher-sweep":
        send_command(args.ppk_control_dir, {"op": "end"})
    final_status = send_command(args.ppk_control_dir, {"op": "status"})
    save_json(args.output, {"schema": 1, "acquisition": acquisition,
                            "firmware": results, "ppk_final": final_status,
                            "reconnected_usb_observed": True})
    # Releasing the source is a distinct explicit action after evidence is saved.
    if args.usb_reconnected:
        finished = send_command(args.ppk_control_dir, {"op": "finish", "usb_reconnected": True})
        if finished.get("cleanup_errors"):
            raise RuntimeError(f"source shutdown could not be verified: {finished['cleanup_errors']}")
    print(json.dumps({"saved": args.output, "state": results.get("state"),
                      "source_released": args.usb_reconnected}, indent=2))


def finite_number(value):
    return isinstance(value, (float, int)) and not isinstance(value, bool) and math.isfinite(value)


def analyze_frames(bundle, frames, *, guard_s=0.5, clock_drift_ppm=100):
    """Conservative receive-window estimates, NEVER exact hardware timing.

    Complete aggregate windows only, after guard on both ends. Reject invalid
    firmware states, sparse/overfull coverage, excessive cumulative sample-clock
    drift or buffering. Quantiles cannot be recombined from window reservoirs.
    """
    if not finite_number(guard_s) or guard_s < 0.25:
        raise ValueError("guard must be finite and at least 0.25 seconds")
    if not finite_number(clock_drift_ppm) or not 0 <= clock_drift_ppm <= 10_000:
        raise ValueError("clock drift budget must be 0..10000 ppm")
    acquisition = bundle["acquisition"]
    sync = acquisition["clock_sync"]["best"]
    owner = acquisition["ppk_before_arm"]
    if (not finite_number(owner.get("host_monotonic_origin_s"))
            or not finite_number(sync.get("offset_s"))
            or not finite_number(sync.get("uncertainty_s"))
            or sync["uncertainty_s"] < 0
            or type(sync.get("device_us")) is not int
            or type(owner.get("voltage_mv")) is not int
            or not 800 <= owner["voltage_mv"] <= 5000):
        raise ValueError("invalid acquisition clock or voltage evidence")
    origin = owner["host_monotonic_origin_s"]
    offset = sync["offset_s"] - origin
    headers = [x for x in frames if x.get("type") == "session"]
    if len(headers) != 1:
        raise ValueError("capture must contain exactly one PPK session header")
    header = headers[0]
    if (header.get("schema_version") != 1 or header.get("sample_rate_hz") != 100_000
            or header.get("host_monotonic_origin_s") != origin
            or header.get("voltage_mv") != owner["voltage_mv"]
            or not owner.get("owner_port")
            or header.get("port_descriptor", {}).get("port") != owner["owner_port"]):
        raise ValueError("sample capture does not match the armed owner/clock/voltage")
    rate = 100_000
    beginnings = [x for x in frames if x.get("type") == "begin" and x.get("label") == "watcher-sweep"]
    if len(beginnings) != 1:
        raise ValueError("capture must contain exactly one watcher-sweep begin anchor")
    beginning = beginnings[0]
    anchor_host = beginning.get("host_offset_s")
    processed_at_begin = beginning.get("first_valid_received_index")
    anchor_index = beginning.get("window_sample_index_start", processed_at_begin)
    if (not finite_number(anchor_host) or anchor_host < 0
            or type(processed_at_begin) is not int or processed_at_begin < 0
            or type(anchor_index) is not int or anchor_index < processed_at_begin):
        raise ValueError("invalid watcher-sweep begin clock/sample anchor")
    # Retain the lifetime prefix as evidence, but judge this recording against
    # its own immutable boundary. Earlier failed preflight is not test data.
    transport_anchor = {"host_offset_s": anchor_host, "sample_index": anchor_index,
                        "processed_samples_at_begin": processed_at_begin,
                        "excluded_preflight_sample_vs_host_drift_s": anchor_index / rate - anchor_host}
    windows = [x for x in frames if x.get("type") == "window" and x.get("label") == "watcher-sweep"]
    previous_end = anchor_host
    previous_index = anchor_index
    for window in windows:
        t, duration = window.get("host_start_offset_s"), window.get("host_duration_s")
        first, last, count = window.get("sample_index_start"), window.get("sample_index_end"), window.get("sample_count")
        if (not finite_number(t) or not finite_number(duration) or duration <= 0
                or t < previous_end - 1e-6 or type(first) is not int or type(last) is not int
                or type(count) is not int or count < 0 or last - first != count
                or first < previous_index or window.get("partial") is not False):
            # Final partial windows are permitted in the file but never quantified.
            if window.get("partial") is True and finite_number(t) and finite_number(duration):
                continue
            raise ValueError("duplicate, overlapping, malformed, or unordered capture windows")
        previous_end = t + duration
        previous_index = last
    windows = [x for x in windows if x.get("partial") is False]
    session_fault = bundle.get("ppk_final", {}).get("fault")
    def recording_loss_delta(counter):
        final = bundle.get("ppk_final", {}).get(counter)
        initial = beginning.get(counter + "_at_begin")
        returned = owner.get(counter)
        if (type(final) is not int or type(initial) is not int
                or type(returned) is not int or initial < 0
                or not initial <= returned <= final):
            raise ValueError("invalid recording decode-loss evidence: " + counter)
        return final - initial

    decoded_missing_during_recording = recording_loss_delta("decoded_missing_frame_count")
    invalid_during_recording = recording_loss_delta("total_invalid_samples")
    late_during_recording = recording_loss_delta("late_unassigned_sample_count")
    rows = []
    for record in bundle["firmware"].get("records", []):
        row = {"name": record.get("name"), "repeat": record.get("repeat"),
               "firmware_status": record.get("status"), "quality": "not_measured",
               "mean_mA": None, "power_mW_source_setpoint": None}
        rows.append(row)
        if (record.get("status") != "ok" or type(record.get("apply_err")) is not int
                or record["apply_err"] != 0):
            row["reason"] = record.get("reason", "firmware skipped or failed state")
            continue
        start, end = record.get("measure_start_us"), record.get("end_us")
        if type(start) is not int or type(end) is not int or end <= start:
            row["reason"] = "missing/reversed firmware capture interval"
            continue
        age = abs(end - sync["device_us"]) / 1e6
        uncertainty = sync["uncertainty_s"] + age * clock_drift_ppm / 1e6
        edge = max(guard_s, uncertainty + 0.25)
        a, b = offset + start / 1e6 + edge, offset + end / 1e6 - edge
        selected = []
        for window in windows:
            t = window.get("host_start_offset_s")
            duration = window.get("host_duration_s")
            if finite_number(t) and finite_number(duration) and duration > 0 and a <= t and t + duration <= b:
                selected.append(window)
        interior_gap = any(
            abs(right["host_start_offset_s"] - (left["host_start_offset_s"] + left["host_duration_s"])) > 1e-6
            or right["sample_index_start"] != left["sample_index_end"]
            or type(right.get("window_index")) is not int
            or type(left.get("window_index")) is not int
            or right["window_index"] != left["window_index"] + 1
            for left, right in zip(selected, selected[1:]))
        edge_gap = bool(selected) and (
            selected[0]["host_start_offset_s"] - a > selected[0]["host_duration_s"] + 1e-6
            or b - selected[-1]["host_start_offset_s"] - selected[-1]["host_duration_s"] > selected[-1]["host_duration_s"] + 1e-6)
        count = sum(x.get("sample_count", 0) for x in selected)
        duration = sum(x["host_duration_s"] for x in selected)
        span = max(0.0, b - a)
        coverage = count / (duration * rate) if duration else 0.0
        transport_drift = max((abs((x.get("sample_index_end", 0) - anchor_index) / rate -
                                   (x["host_start_offset_s"] + x["host_duration_s"] - anchor_host))
                               for x in selected), default=math.inf)
        backlog_bytes = max(bundle.get("ppk_final", {}).get("max_observed_backlog_bytes", 0),
                            bundle.get("ppk_final", {}).get("receiver", {}).get("max_combined_backlog_bytes", 0))
        if type(backlog_bytes) is not int or backlog_bytes < 0:
            raise ValueError("invalid receive backlog evidence")
        backlog_s = backlog_bytes / (4 * rate)
        delays = [x.get("label_max_consumer_delay_s", 0.0) for x in selected]
        if any(not finite_number(delay) or delay < 0 for delay in delays):
            row["quality"] = "rejected"
            row["reason"] = "invalid receive-to-consumer delay evidence"
            continue
        consumer_delay_s = max(delays, default=0.0)
        row.update({"included_host_duration_s": duration, "sample_count": count,
                    "coverage_ratio_estimate": coverage, "capture_guard_s": edge,
                    "clock_uncertainty_budget_s": uncertainty,
                    "sample_vs_host_drift_s": transport_drift if math.isfinite(transport_drift) else None,
                    "largest_observed_backlog_s": backlog_s,
                    "largest_label_consumer_delay_s": consumer_delay_s,
                    "percentiles": "not recombinable from aggregate reservoirs"})
        if session_fault or decoded_missing_during_recording or invalid_during_recording or late_during_recording or not selected or interior_gap or edge_gap or not 0.99 <= coverage <= 1.01 or duration < span * 0.9 or transport_drift + max(backlog_s, consumer_delay_s) > edge:
            row["quality"] = "rejected"
            row["reason"] = "PPK fault, gaps/coverage, or timing/buffering exceeds guard; no quantitative attribution"
            continue
        if any(not finite_number(x.get("mean_uA")) for x in selected):
            row["quality"] = "rejected"
            row["reason"] = "invalid aggregate current"
            continue
        mean = math.fsum(x["mean_uA"] * x["sample_count"] for x in selected) / count
        row.update({"quality": "receive_window_estimate", "mean_mA": mean / 1000,
                    "min_mA": min(x["min_uA"] for x in selected) / 1000,
                    "max_mA": max(x["max_uA"] for x in selected) / 1000,
                    "power_mW_source_setpoint": mean * owner["voltage_mv"] / 1e6,
                    "sampled_charge_mAh": math.fsum(x["sampled_charge_mAh"] for x in selected),
                    "sampled_energy_mWh_source_setpoint": math.fsum(x["sampled_energy_mWh"] for x in selected)})
    return {"schema": 1, "device": "Seeed SenseCAP Watcher", "profile_kind": "isolated BSP characterization, NOT production runtime",
            "run_state": bundle["firmware"].get("state"), "source_voltage_mv": owner["voltage_mv"],
            "requested_guard_s": guard_s, "assumed_clock_drift_ppm": clock_drift_ppm,
            "timing_model": "Conditional receive-window estimates under the assumed drift budget; UART endpoints do not validate interior clock wander.",
            "transport_anchor": transport_anchor, "rows": rows, "limitations": [
                "Sequential board-input differences can be confounded by initialization/cleanup history; use matched controls before assigning marginal costs, and never infer isolated IC supply current.",
                "PPK2 API lacks exact lost-frame positions. Quantitative rows are conditional guarded receive-window estimates, not sample-perfect synchronization.",
                "Source setpoint is not measured DUT voltage. Wiring and power-path elements may add voltage drop; no DUT energy claim without voltage measurement.",
                "Skipped/rejected states and aborted runs cannot establish a complete device power profile."]}


def analyze(args):
    bundle = load_json(args.results)
    frames = []
    with open(args.samples, encoding="utf-8") as stream:
        for line in stream:
            if len(line) > MAX_JSON_BYTES:
                raise ValueError("oversized PPK aggregate record")
            frame = json.loads(line)
            if frame.get("type") in ("window", "session", "begin"):
                frames.append(frame)
            if len(frames) > 50_000:
                raise ValueError("aggregate report exceeds bounded window budget")
    report = analyze_frames(bundle, frames, guard_s=args.guard_s, clock_drift_ppm=args.clock_drift_ppm)
    save_json(args.output, report)
    print(json.dumps({"saved": args.output, "states": len(report["rows"]),
                      "accepted_estimates": sum(x["quality"] == "receive_window_estimate" for x in report["rows"])}, indent=2))


def parser():
    ap = argparse.ArgumentParser(description=__doc__)
    sub = ap.add_subparsers(dest="action", required=True)
    a = sub.add_parser("arm", help="arm only after independent PPK owner is ON and ready")
    a.add_argument("--port", required=True)
    a.add_argument("--ppk-control-dir", required=True)
    a.add_argument("--output", required=True)
    a.add_argument("--run-id")
    a.add_argument("--settle-ms", type=int, default=5000)
    a.add_argument("--capture-ms", type=int, default=20000)
    a.add_argument("--repeats", type=int, choices=(1, 2), default=1)
    c = sub.add_parser("collect", help="read results after actual USB reconnection; keep owner on unless explicitly released")
    c.add_argument("--port", required=True)
    c.add_argument("--ppk-control-dir", required=True)
    c.add_argument("--acquisition", required=True)
    c.add_argument("--output", required=True)
    c.add_argument("--usb-reconnected", action="store_true", help="release source ONLY after live Watcher reports USB")
    r = sub.add_parser("analyze", help="offline guarded aggregate estimates")
    r.add_argument("--results", required=True)
    r.add_argument("--samples", required=True)
    r.add_argument("--output", required=True)
    r.add_argument("--guard-s", type=float, default=0.5)
    r.add_argument("--clock-drift-ppm", type=float, default=100)
    return ap


if __name__ == "__main__":
    options = parser().parse_args()
    try:
        {"arm": arm, "collect": collect, "analyze": analyze}[options.action](options)
    except (OSError, ValueError, RuntimeError, TimeoutError) as exc:
        sys.exit(f"{exc}\nPPK2 owner is NOT automatically stopped. Reconnect Watcher USB before explicit finish.")
