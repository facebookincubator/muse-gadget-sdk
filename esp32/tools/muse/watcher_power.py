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
The separate dryrun command uses Watcher USB only, never the PPK2 owner;
it is a functional diagnostic, not a PM/timing/current measurement.
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
from tools.power.ppk2_receiver import MAX_BATCH_BYTES  # noqa: E402

MAX_JSON_BYTES = 128 * 1024
MATRIX_VERSIONS = {"peripheral": 1, "sleep": 2}


def matrix_echo(frame, expected):
    """Old peripheral firmware may omit matrix; sleep must echo both fields."""
    if expected not in MATRIX_VERSIONS:
        raise ValueError("matrix must be peripheral or sleep")
    matrix, version = frame.get("matrix"), frame.get("matrix_version")
    if matrix is None and expected == "peripheral":
        if "matrix" not in frame and ("matrix_version" not in frame or type(version) is int and version == 1):
            return
    if matrix != expected or type(version) is not int or version != MATRIX_VERSIONS[expected]:
        raise ValueError("firmware matrix/matrix_version echo does not match the requested matrix")


def acquisition_matrix(acquisition):
    matrix = acquisition.get("requested_config", {}).get("matrix", "peripheral")
    if matrix not in MATRIX_VERSIONS:
        raise ValueError("invalid acquisition matrix")
    arm_frame = acquisition.get("arm", {})
    if (acquisition.get("state") == "armed" or arm_frame.get("type") == "arm_ack"
            or "matrix" in arm_frame or "matrix_version" in arm_frame):
        matrix_echo(arm_frame, matrix)
    return matrix


def nonnegative_int(value, name):
    if type(value) is not int or not 0 <= value <= (1 << 63) - 1:
        raise ValueError(name + " must be a finite non-negative integer within firmware int64 range")
    return value


def timeline_uncertainty(frame, *, required=False):
    if required and "timeline_uncertainty_us" not in frame:
        raise ValueError("missing firmware timeline_uncertainty_us")
    return nonnegative_int(frame.get("timeline_uncertainty_us", 0), "timeline_uncertainty_us")


def timeline_evidence(frame, *, required=False):
    if required and any(key not in frame for key in ("resume_count", "timeline_offset_us")):
        raise ValueError("missing firmware resume/timeline evidence")
    offset = frame.get("timeline_offset_us", 0)
    if type(offset) is not int:
        raise ValueError("timeline_offset_us must be integer microseconds")
    return {"resume_count": nonnegative_int(frame.get("resume_count", 0), "resume_count"),
            "timeline_offset_us": offset,
            "timeline_uncertainty_us": timeline_uncertainty(frame, required=required)}


def capture_plan(acquisition, firmware):
    armed = acquisition.get("arm", {}).get("plan")
    reported = firmware.get("plan")
    if armed is not None and reported is not None and armed != reported:
        raise ValueError("result plan differs from the armed plan")
    plan = armed if armed is not None else reported if reported is not None else []
    if not isinstance(plan, list) or len(plan) > 128:
        raise ValueError("invalid or oversized matrix plan")
    seen_ids, seen_indices = set(), set()
    normalized = []
    for position, entry in enumerate(plan):
        if not isinstance(entry, dict):
            raise ValueError("plan entries must be objects")
        name, index = entry.get("id"), entry.get("index", position)
        if (not isinstance(name, str) or not name or len(name) > 128 or name in seen_ids
                or type(index) is not int or index < 0 or index in seen_indices):
            raise ValueError("invalid/duplicate plan identity")
        if "role" in entry and entry["role"] not in ("ref", "variant", "other"):
            raise ValueError("invalid plan role")
        if "ref_group" in entry and (not isinstance(entry["ref_group"], str) or len(entry["ref_group"]) > 64):
            raise ValueError("invalid plan ref_group")
        seen_ids.add(name)
        seen_indices.add(index)
        normalized.append(dict(entry, index=index))
    return normalized


def record_plan(record, plan):
    if not plan:
        return {}
    if "index" in record:
        entries = [entry for entry in plan if entry["index"] == record["index"]]
    else:
        entries = [entry for entry in plan if entry["id"] == record.get("name")]
    if len(entries) != 1 or entries[0]["id"] != record.get("name"):
        raise ValueError("record identity does not match the armed plan")
    return entries[0]


def verify_resume_chain(acquisition, results, live):
    """Only a fully evidenced timer/deepsleep chain permits a changed boot."""
    matrix = acquisition_matrix(acquisition)
    matrix_echo(results, matrix)
    armed_boot = nonnegative_int(acquisition["arm"].get("boot_id"), "armed boot_id")
    run_boot = nonnegative_int(results.get("run_boot_id"), "run_boot_id")
    retrieval = nonnegative_int(results.get("retrieval_boot_id"), "retrieval_boot_id")
    live_boot = nonnegative_int(live.get("boot_id"), "live boot_id")
    if run_boot != armed_boot:
        raise ValueError("result boot identity differs from synchronized acquisition")
    for key, expected in (("run_id", acquisition["arm"].get("run_id")), ("run_boot_id", armed_boot)):
        if key in live and live[key] != expected:
            raise ValueError("live status does not belong to the armed run")
    required = matrix == "sleep"
    top, current = timeline_evidence(results, required=required), timeline_evidence(live, required=required)
    if top != current:
        raise ValueError("live and result resume/timeline evidence differs")
    plan = capture_plan(acquisition, results)
    boot, seen, resumes = armed_boot, {armed_boot}, []
    records = results.get("records", [])
    if not isinstance(records, list) or len(records) > 256:
        raise ValueError("invalid/oversized firmware records")
    for record in records:
        if not isinstance(record, dict):
            raise ValueError("invalid firmware record")
        uncertainty = timeline_uncertainty(record, required=required)
        if (required or "timeline_uncertainty_us" in results) and uncertainty > top["timeline_uncertainty_us"]:
            raise ValueError("record timeline uncertainty exceeds final firmware evidence")
        entry = record_plan(record, plan)
        if "boot_id" in record and nonnegative_int(record["boot_id"], "record.boot_id") != boot:
            raise ValueError("record boot identity breaks the resume chain")
        deep = record.get("deep_sleep")
        if record.get("status") != "ok":
            continue
        if entry.get("deep_sleep") and deep is None:
            raise ValueError("successful deep-sleep state lacks resume evidence")
        if deep is None:
            continue
        if not required or not isinstance(deep, dict):
            raise ValueError("unexpected or malformed deep-sleep resume evidence")
        before = nonnegative_int(deep.get("entry_boot_id"), "deep_sleep.entry_boot_id")
        after = nonnegative_int(deep.get("resume_boot_id"), "deep_sleep.resume_boot_id")
        if (before != boot or after in seen or deep.get("wake_cause") != "timer"
                or deep.get("resume_reset_reason") != "deepsleep"):
            raise ValueError("broken deep-sleep boot chain or unexpected reset/wake cause")
        for key in ("programmed_us", "rtc_slept_us"):
            if nonnegative_int(deep.get(key), "deep_sleep." + key) == 0:
                raise ValueError("deep-sleep duration must be positive")
        resumes.append({"entry_boot_id": before, "resume_boot_id": after})
        seen.add(after)
        boot = after
    if top["resume_count"] != len(resumes) or retrieval != boot or live_boot != boot:
        raise ValueError("unexpected reboot: resume count or final retrieval/live boot breaks the chain")
    return {"run_boot_id": armed_boot, "retrieval_boot_id": retrieval,
            "resumes": resumes, **top}


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


def strict_json_object(text):
    """Reject duplicate keys, non-finite numbers, and non-object protocol frames."""
    def pairs(items):
        result = {}
        for key, value in items:
            if key in result:
                raise ValueError("duplicate JSON key: " + key)
            result[key] = value
        return result

    def number(text):
        value = float(text)
        if not math.isfinite(value):
            raise ValueError("non-finite JSON number")
        return value

    def constant(text):
        raise ValueError("non-finite JSON constant: " + text)

    frame = json.loads(text, object_pairs_hook=pairs, parse_float=number, parse_constant=constant)
    if not isinstance(frame, dict):
        raise ValueError("firmware reply must be a JSON object")
    return frame


def request(board, command, kind, timeout_s=5.0, *, observe=None):
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
        if observe is not None:
            observe(line)
        at = line.find("PTEST ")
        if at < 0:
            continue
        if len(line.encode("utf-8")) > MAX_JSON_BYTES:
            raise ValueError("oversized firmware reply")
        frame = strict_json_object(line[at + 6:])
        if type(frame.get("schema")) is not int or frame["schema"] != 1:
            raise ValueError("unsupported PTEST schema")
        if frame.get("type") == "error":
            raise RuntimeError(f"Watcher rejected command: {frame}")
        if frame.get("type") == kind:
            if request_id is not None and frame.get("request_id") != request_id:
                continue  # stale/late response is not this request's clock evidence
            return frame
    raise TimeoutError("Watcher did not acknowledge; command outcome UNKNOWN. Query ptest.status; do not re-arm blindly")


def clock_observation(status, start, end, *, require_timeline=False):
    if not finite_number(start) or not finite_number(end) or end < start:
        raise ValueError("invalid host clock round trip")
    now_us = nonnegative_int(status.get("now_us"), "firmware clock now_us")
    nonnegative_int(status.get("boot_id"), "firmware boot_id")
    uncertainty_us = timeline_uncertainty(status, required=require_timeline or status.get("matrix") == "sleep")
    return {"boot_id": status["boot_id"], "device_us": now_us,
            "host_start_s": start, "host_end_s": end,
            "offset_s": (start + end) / 2 - now_us / 1e6,
            "uart_rtt_uncertainty_s": (end - start) / 2,
            "timeline_uncertainty_us": uncertainty_us,
            "uncertainty_s": (end - start) / 2 + uncertainty_us / 1e6}


def synchronize(board, count=3):
    """Bound a same-host monotonic/ESP clock mapping by each UART round trip."""
    observations = []
    for _ in range(count):
        start = time.monotonic()
        status = request(board, "ptest.status", "status")
        end = time.monotonic()
        if status.get("usb") is not True:
            raise ValueError("Watcher must report actual USB power before arming")
        observations.append(clock_observation(status, start, end))
    if len({x["boot_id"] for x in observations}) != 1:
        raise ValueError("Watcher rebooted during synchronization")
    best = min(observations, key=lambda x: x["uncertainty_s"])
    return {"best": best, "observations": observations,
            "clock": "same-host monotonic seconds mapped to firmware virtual microseconds by UART RTT plus firmware timeline uncertainty"}


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
    matrix = getattr(args, "matrix", "peripheral")
    if matrix not in MATRIX_VERSIONS:
        raise ValueError("matrix must be peripheral or sleep")
    if matrix == "sleep" and (type(args.repeats) is not int or args.repeats != 1):
        raise ValueError("sleep matrix requires repeats=1: untouched cold state cannot be recreated")
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
        if matrix != "peripheral":
            config["matrix"] = matrix  # Omit for old peripheral firmware.
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
    matrix_echo(ack, matrix)
    timeline_uncertainty(ack, required=matrix == "sleep")
    bound = ack.get("max_duration_ms")
    if type(bound) is not int or not 1 <= bound <= 3_600_000:
        raise ValueError("firmware did not provide a bounded sweep duration")
    metadata.update({"state": "armed", "arm": ack})
    persist_metadata(stream, metadata)
    print(json.dumps({"ready_to_unplug": True, "run_id": ack["run_id"], "matrix": matrix,
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
        status_start = time.monotonic()
        status = request(board, "ptest.status", "status")
        status_end = time.monotonic()
        if status.get("usb") is not True:
            raise ValueError("Watcher does not confirm actual USB reconnection; PPK2 remains ON")
        results = request(board, "ptest.results", "results")
    if results.get("run_id") != acquisition["arm"].get("run_id"):
        raise ValueError("results do not belong to this acquisition")
    provenance = verify_resume_chain(acquisition, results, status)
    # Reuse the existing live-status RTT: no extra UART requests or clock reset.
    post_sync = None
    if "now_us" in status:
        observation = clock_observation(status, status_start, status_end,
                                        require_timeline=acquisition_matrix(acquisition) == "sleep")
        post_sync = {"best": observation, "observations": [observation],
                     "clock": "post-run virtual timeline; UART RTT plus firmware timeline uncertainty"}
    if results.get("state") in ("idle", "armed", "running"):
        raise ValueError("sweep is not finished; do not release PPK2 or publish a profile")
    if owner.get("active_label") == "watcher-sweep":
        send_command(args.ppk_control_dir, {"op": "end"})
    final_status = send_command(args.ppk_control_dir, {"op": "status"})
    save_json(args.output, {"schema": 1, "acquisition": acquisition,
                            "firmware": results, "ppk_final": final_status,
                            "live_status": status, "boot_provenance": provenance,
                            "post_clock_sync": post_sync,
                            "reconnected_usb_observed": True})
    # Releasing the source is a distinct explicit action after evidence is saved.
    if args.usb_reconnected:
        finished = send_command(args.ppk_control_dir, {"op": "finish", "usb_reconnected": True})
        if (finished.get("holding") is not False or finished.get("cleanup_errors")
                or finished.get("cleanup_errors_omitted_count", 0) != 0):
            raise RuntimeError(f"source shutdown could not be verified: {finished.get('cleanup_errors')}; holding={finished.get('holding')}; omitted={finished.get('cleanup_errors_omitted_count', 0)}")
    print(json.dumps({"saved": args.output, "state": results.get("state"),
                      "source_released": args.usb_reconnected}, indent=2))


def dryrun(args):
    """Bank a USB-only functional diagnostic; never involve the PPK2 owner."""
    if args.matrix != "sleep":
        raise ValueError("dryrun supports only matrix sleep")
    timeout = getattr(args, "timeout_s", 60.0)
    if not finite_number(timeout) or not 1 <= timeout <= 300:
        raise ValueError("dryrun timeout must be finite and between 1 and 300 seconds")
    import chat
    data = {"schema": 1, "type": "watcher_usb_dryrun", "matrix": "sleep",
            "quantitative": False, "pm_exercised": False,
            "limitations": "USB-powered GPIO/I2C diagnostic only; no current, timing, PM-permission or deep-sleep validation. Audio constructor history may become warm.",
            "status_before": None, "status_after": None, "states": [], "done": None,
            "raw_lines": [], "summary": {"ok": False, "errors": []}}
    errors = data["summary"]["errors"]
    raw_bytes = 0
    phase = "before_status"

    def observe(line):
        nonlocal raw_bytes
        size = len(line.encode("utf-8"))
        if size > MAX_JSON_BYTES or raw_bytes + size > 2 * MAX_JSON_BYTES or len(data["raw_lines"]) >= 256:
            data["transcript_truncated"] = True
            raise ValueError("dryrun transcript exceeds bounded size/line limit")
        raw_bytes += size
        data["raw_lines"].append(line)
        if phase == "after_status" and "PTEST " in line:
            frame = strict_json_object(line.split("PTEST ", 1)[1])
            if frame.get("type") in ("dryrun_state", "dryrun_done"):
                errors.append("extra dryrun reply after completion/failure")

    def live_status(frame):
        if frame.get("usb") is not True:
            raise ValueError("dryrun requires live Watcher USB power")
        if "power_error" in frame and (type(frame["power_error"]) is not int or frame["power_error"] != 0):
            raise ValueError("dryrun USB readback failed")
        if nonnegative_int(frame.get("boot_id"), "dryrun boot_id") == 0:
            raise ValueError("dryrun requires a nonzero boot_id")
        if not isinstance(frame.get("state"), str) or frame["state"] in ("armed", "running"):
            raise ValueError("dryrun cannot alter an active or unknown run")

    def validate(frame):
        if type(frame.get("schema")) is not int or frame["schema"] != 1:
            raise ValueError("unsupported dryrun schema")
        matrix_echo(frame, "sleep")
        if type(frame.get("boot_id")) is not int or frame["boot_id"] != data["status_before"]["boot_id"]:
            raise ValueError("Watcher rebooted during dryrun")
        if frame.get("usb") is not True:
            errors.append("Watcher lost USB power during dryrun")
        if "pm_exercised" in frame and frame["pm_exercised"] is not False:
            raise ValueError("dryrun must not exercise PM")
        log = frame.get("last_error_log")
        if not isinstance(log, str) or len(log) > 96 or any(not 32 <= ord(c) <= 126 for c in log):
            raise ValueError("invalid dryrun last_error_log")

    def error_code(value):
        if type(value) is not int or not -(1 << 31) <= value < (1 << 31):
            raise ValueError("invalid dryrun ESP error code")
        return value

    # Reserve and bank before opening a port or changing constructor history.
    with open(args.output, "x", encoding="utf-8") as stream:
        persist_metadata(stream, data)
        try:
            with chat.Board(args.port) as board:
                started = False
                try:
                    data["status_before"] = request(board, "ptest.status", "status", observe=observe)
                    live_status(data["status_before"])
                    persist_metadata(stream, data)
                    phase = "command"
                    started = True  # A failed write may still have reached firmware.
                    board.wake()
                    board.write_line('ptest.dryrun={"matrix":"sleep"}')
                    deadline = time.monotonic() + timeout
                    while time.monotonic() < deadline:
                        line = board.read_line(deadline)
                        if line is None:
                            break
                        observe(line)
                        if "PTEST " not in line:
                            continue  # Preserve ordinary diagnostic/log lines too.
                        frame = strict_json_object(line.split("PTEST ", 1)[1])
                        kind = frame.get("type")
                        if kind == "dryrun_state":
                            data["states"].append(frame)
                            validate(frame)
                            index = frame.get("index")
                            if type(index) is not int or index != len(data["states"]) - 1 or not 0 <= index < 24:
                                raise ValueError("missing/duplicate/out-of-order dryrun state index")
                            identity = frame.get("id")
                            if not isinstance(identity, str) or not identity or len(identity) > 128 or any(s.get("id") == identity for s in data["states"][:-1]):
                                raise ValueError("invalid/duplicate dryrun state id")
                            if type(frame.get("codec_regs_expected")) is not bool:
                                raise ValueError("invalid dryrun codec_regs_expected")
                            if error_code(frame.get("error")) != 0:
                                errors.append("dryrun state error: " + identity)
                            persist_metadata(stream, data)
                        elif kind == "dryrun_done":
                            data["done"] = frame
                            validate(frame)
                            if nonnegative_int(frame.get("states"), "dryrun states") != len(data["states"]):
                                raise ValueError("dryrun summary state count mismatch")
                            if nonnegative_int(frame.get("errors"), "dryrun errors") or error_code(frame.get("restore_error")):
                                errors.append("dryrun summary reports firmware/restore errors")
                            if len(data["states"]) != 24:
                                errors.append("dryrun did not cover all 24 non-deep sleep states")
                            break
                        else:
                            raise RuntimeError("unexpected/rejected dryrun reply: " + str(frame))
                    if data["done"] is None:
                        raise TimeoutError("dryrun completion not received; command outcome UNKNOWN")
                except (OSError, ValueError, RuntimeError, TimeoutError) as exc:
                    errors.append(str(exc))
                finally:
                    if started:
                        phase = "after_status"
                        try:
                            data["status_after"] = request(board, "ptest.status", "status", observe=observe)
                            live_status(data["status_after"])
                            if data["status_after"]["boot_id"] != data["status_before"]["boot_id"]:
                                raise ValueError("Watcher rebooted after dryrun")
                        except (OSError, ValueError, RuntimeError, TimeoutError) as exc:
                            errors.append(str(exc))
        except Exception as exc:
            # chat.BoardError (including open/close failures) is not an OSError.
            errors.append(str(exc))
        finally:
            data["summary"].update(ok=not errors and data["done"] is not None and data["status_after"] is not None,
                                   states=len(data["states"]))
            persist_metadata(stream, data)
    if not data["summary"]["ok"]:
        raise RuntimeError(f"USB dryrun failed; diagnostics saved to {args.output}: " + "; ".join(errors))
    print(json.dumps({"saved": args.output, **data["summary"], "quantitative": False}))


def finite_number(value):
    return isinstance(value, (float, int)) and not isinstance(value, bool) and math.isfinite(value)


def transport_fault_cutoff(final, origin, guard_s):
    """Prove a decoded prefix, without clearing the original transport fault."""
    receiver = final.get("receiver")
    if not isinstance(receiver, dict) or not isinstance(receiver.get("fault"), dict):
        raise ValueError("pre-fault prefix requires timestamped receiver fault evidence")
    fault = receiver["fault"]
    kind, message = fault.get("kind"), fault.get("message")
    if (kind not in ("queue_overflow", "read_error") or not isinstance(message, str)
            or final.get("fault") != f"raw receiver: {kind}: {message}"[:512]):
        raise ValueError("pre-fault prefix is only allowed for the final raw receiver overflow/read fault")
    arrival = fault.get("arrival_monotonic_s")
    delay = final.get("max_consumer_delay_s")
    receiver_delay = receiver.get("max_consumer_delay_s", delay)
    if (not finite_number(arrival) or arrival < origin
            or not finite_number(delay) or delay < 0
            or not finite_number(receiver_delay) or receiver_delay < 0):
        raise ValueError("invalid receiver fault clock/consumer-delay evidence")
    delay = max(delay, receiver_delay)
    valid = nonnegative_int(final.get("total_valid_samples"), "total_valid_samples")
    invalid = nonnegative_int(final.get("total_invalid_samples"), "total_invalid_samples")
    missing = nonnegative_int(final.get("decoded_missing_frame_count"), "decoded_missing_frame_count")
    consumed = nonnegative_int(receiver.get("consumed_bytes"), "receiver consumed_bytes")
    accounted = 4 * (valid + invalid + missing)
    surplus = consumed - accounted
    if valid == 0 or not 0 <= surplus <= MAX_BATCH_BYTES + 3:
        raise ValueError("receiver consumed bytes do not prove a bounded decoded prefix")
    proof = {"consumed_bytes": consumed, "decoded_frame_bytes": accounted,
             "un_ingested_or_partial_bytes": surplus}
    has_boundary = any(key in receiver for key in ("received_bytes", "discarded_bytes", "queued_bytes", "invalidated_queue_bytes")) or any(key in fault for key in ("received_bytes", "queued_bytes"))
    if surplus or has_boundary:
        # read_batch atomically refuses dequeues after the first fault. A fault
        # during decode/ingest can discard at most one already-dequeued 64 KiB
        # batch plus a three-byte framing tail. Do not infer this from size
        # alone: prove receiver conservation and the unchanged invalidated queue.
        received = nonnegative_int(receiver.get("received_bytes"), "receiver received_bytes")
        discarded = nonnegative_int(receiver.get("discarded_bytes"), "receiver discarded_bytes")
        queued = nonnegative_int(receiver.get("queued_bytes"), "receiver queued_bytes")
        invalidated = nonnegative_int(receiver.get("invalidated_queue_bytes"), "invalidated queue bytes")
        fault_received = nonnegative_int(fault.get("received_bytes"), "received bytes at fault")
        fault_queued = nonnegative_int(fault.get("queued_bytes"), "queued bytes at fault")
        trigger_discard = fault_received - consumed - queued
        if (queued != fault_queued or queued != invalidated
                or received < fault_received or received != consumed + queued + discarded
                or (kind == "queue_overflow" and not 1 <= trigger_discard <= MAX_BATCH_BYTES)
                or (kind == "read_error" and trigger_discard != 0)):
            raise ValueError("receiver conservation/first-fault boundary does not prove the decoded prefix")
        proof.update({"received_bytes_at_fault": fault_received,
                      "retained_invalidated_queue_bytes": queued,
                      "trigger_discard_bytes": trigger_discard,
                      "basis": "bounded pre-fault in-flight batch, no post-fault dequeue, and four-byte frame accounting"})
    else:
        proof["basis"] = "consumed bytes exactly match decoded four-byte frames"
    fault_offset = arrival - origin
    cutoff = fault_offset - delay - guard_s
    if not finite_number(cutoff):
        raise ValueError("invalid pre-fault cutoff")
    return {"fault_kind": kind, "fault_host_offset_s": fault_offset,
            "cutoff_host_offset_s": cutoff, "max_consumer_delay_s": delay,
            "rows_before_cutoff": 0, "byte_accounting": proof}


def analyze_frames(bundle, frames, *, guard_s=0.5, clock_drift_ppm=100, pre_fault_prefix=False):
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
    matrix = acquisition_matrix(acquisition)
    matrix_echo(bundle["firmware"], matrix)
    if matrix == "sleep":
        verify_resume_chain(acquisition, bundle["firmware"], bundle.get("live_status", {}))
    plan = capture_plan(acquisition, bundle["firmware"])
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
    if type(pre_fault_prefix) is not bool:
        raise ValueError("pre_fault_prefix must be boolean")
    prefix = None
    if pre_fault_prefix:
        if decoded_missing_during_recording or invalid_during_recording or late_during_recording:
            raise ValueError("pre-fault prefix cannot excuse new decoded/invalid/late sample loss")
        prefix = transport_fault_cutoff(bundle["ppk_final"], origin, guard_s)
        if any(window["sample_index_end"] > bundle["ppk_final"]["total_valid_samples"] for window in windows):
            raise ValueError("window sample ordinals exceed the proven decoded prefix")
    rows = []
    for record in bundle["firmware"].get("records", []):
        timeline_s = timeline_uncertainty(record, required=matrix == "sleep") / 1e6
        entry = record_plan(record, plan)
        row = {"name": record.get("name"), "repeat": record.get("repeat"),
               "plan_index": entry.get("index", record.get("index")),
               "timeline_uncertainty_s": timeline_s,
               "firmware_status": record.get("status"), "quality": "not_measured",
               "mean_mA": None, "power_mW_source_setpoint": None}
        for key in ("role", "ref_group", "knobs", "poll_ms"):
            if key in entry:
                row[key] = entry[key]
        if "deep_sleep" in entry:
            row["plan_deep_sleep"] = entry["deep_sleep"]
        for key in ("codec_regs", "codec_regs_available", "codec_regs_expected", "last_error_log"):
            if key in record:
                row[key] = record[key]  # Diagnostic pass-through, never an acceptance gate.
        if "deep_sleep" in record:
            row["deep_sleep"] = record["deep_sleep"]
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
        uncertainty = sync["uncertainty_s"] + age * clock_drift_ppm / 1e6 + timeline_s
        row.update({"measure_start_us": start, "end_us": end})
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
        if prefix is None:
            backlog_bytes = max(bundle.get("ppk_final", {}).get("max_observed_backlog_bytes", 0),
                                bundle.get("ppk_final", {}).get("receiver", {}).get("max_combined_backlog_bytes", 0))
        else:
            # A later receiver stall cannot retroactively enlarge an earlier
            # window's backlog. Require its captured cumulative peak instead.
            backlogs = [window.get("max_observed_backlog_bytes") for window in selected]
            if any(type(value) is not int or value < 0 for value in backlogs):
                row["quality"] = "rejected"
                row["reason"] = "missing/invalid pre-fault window backlog evidence"
                continue
            backlog_bytes = max(backlogs, default=0)
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
        if prefix is not None:
            row.update(guarded_host_start_offset_s=a, guarded_host_end_offset_s=b)
            if b > prefix["cutoff_host_offset_s"]:
                row["quality"] = "rejected"
                row["reason"] = "guarded capture ends after the conservative receiver fault cutoff"
                continue
        if (session_fault and prefix is None) or decoded_missing_during_recording or invalid_during_recording or late_during_recording or not selected or interior_gap or edge_gap or not 0.99 <= coverage <= 1.01 or duration < span * 0.9 or transport_drift + max(backlog_s, consumer_delay_s) > edge:
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
    report = {"schema": 1, "device": "Seeed SenseCAP Watcher", "profile_kind": "isolated BSP characterization, NOT production runtime",
            "run_state": bundle["firmware"].get("state"), "source_voltage_mv": owner["voltage_mv"],
            "matrix": matrix, "matrix_version": MATRIX_VERSIONS[matrix], "plan": plan,
            "requested_guard_s": guard_s, "assumed_clock_drift_ppm": clock_drift_ppm,
            "timing_model": "Conditional receive-window estimates under the assumed drift budget; UART endpoints do not validate interior clock wander.",
            "transport_anchor": transport_anchor, "rows": rows, "limitations": [
                "Sequential board-input differences can be confounded by initialization/cleanup history; use matched controls before assigning marginal costs, and never infer isolated IC supply current.",
                "PPK2 API lacks exact lost-frame positions. Quantitative rows are conditional guarded receive-window estimates, not sample-perfect synchronization.",
                "Source setpoint is not measured DUT voltage. Wiring and power-path elements may add voltage drop; no DUT energy claim without voltage measurement.",
                "Skipped/rejected states and aborted runs cannot establish a complete device power profile."]}
    if prefix is not None:
        prefix["rows_before_cutoff"] = sum(row["quality"] == "receive_window_estimate" for row in rows)
        report["transport_fault_cutoff"] = prefix
        report["transport_fault"] = session_fault
        report["limitations"].append("Opt-in pre-fault prefix only: the receiver/capture remains INVALID after its fault. Accepted whole rows end before the delay-plus-guard cutoff and use their original window backlog evidence; no later continuation is recovered.")
    return report


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
    report = analyze_frames(bundle, frames, guard_s=args.guard_s, clock_drift_ppm=args.clock_drift_ppm,
                            pre_fault_prefix=getattr(args, "pre_fault_prefix", False))
    save_json(args.output, report)
    print(json.dumps({"saved": args.output, "states": len(report["rows"]),
                      "accepted_estimates": sum(x["quality"] == "receive_window_estimate" for x in report["rows"])}, indent=2))


def compare_profile(profile, pairs=None):
    """Matched A/B/A of accepted, count-weighted means; never pool quantiles.

    Explicit pairs: {"pairs":[{"variant":"B","ref_before":"A0",
    "ref_after":"A1","repeat":0}]}; repeat is optional (all repeats).
    Plan role/ref_group is authoritative. Legacy plans can use warm_ref*,
    cold_ref, cold_i2s_low/cold_i2s_low_b, or explicit reference identities.
    """
    matrix_echo(profile, "sleep")
    rows = profile.get("rows")
    if not isinstance(rows, list) or len(rows) > 256:
        raise ValueError("invalid/oversized profile rows")
    plan = capture_plan({}, profile)
    identities, entries = set(), []
    for row in rows:
        if not isinstance(row, dict):
            raise ValueError("invalid profile row")
        name, repeat = row.get("name"), row.get("repeat")
        if not isinstance(name, str) or not name or repeat is not None and (type(repeat) is not int or repeat < 0):
            raise ValueError("invalid profile row identity/repeat")
        identity = (name, repeat)
        if identity in identities:
            raise ValueError("duplicate profile row identity within a repeat")
        identities.add(identity)
        record = {"name": name}
        if row.get("plan_index") is not None:
            record["index"] = row["plan_index"]
        entries.append(record_plan(record, plan))
        if row.get("quality") == "receive_window_estimate":
            if type(row.get("sample_count")) is not int or row["sample_count"] <= 0 or not finite_number(row.get("mean_mA")):
                raise ValueError("accepted profile row lacks a finite count-weighted mean")

    specs = [] if pairs is None else pairs.get("pairs") if isinstance(pairs, dict) else pairs
    if not isinstance(specs, list) or len(specs) > 128:
        raise ValueError("pairs must be a list or an object with a bounded pairs list")
    for spec in specs:
        if (not isinstance(spec, dict) or set(spec) - {"variant", "ref_before", "ref_after", "repeat"}
                or any(not isinstance(spec.get(key), str) or not spec[key] for key in ("variant", "ref_before", "ref_after"))
                or len({spec["variant"], spec["ref_before"], spec["ref_after"]}) != 3
                or "repeat" in spec and (type(spec["repeat"]) is not int or spec["repeat"] < 0)):
            raise ValueError("invalid explicit A/B/A pair")
        if not any(name == spec["variant"] and ("repeat" not in spec or repeat == spec["repeat"]) for name, repeat in identities):
            raise ValueError("explicit variant does not exist in the profile")
        if any(not any(name == spec[key] for name, _ in identities) for key in ("ref_before", "ref_after")):
            raise ValueError("explicit reference does not exist in the profile")

    def role_group(index):
        entry, name = entries[index], rows[index]["name"]
        named_ref = name.startswith("warm_ref") or name in ("cold_ref", "cold_i2s_low", "cold_i2s_low_b")
        if name == "cold_ref" or name.startswith("cold_i2s_"):
            legacy_group = "cold"
        elif name.startswith(("warm_ref", "best_")) or name in (
                "uart_hiz", "rgb_low", "pulls_off", "unused_hiz", "gpio_isolate", "cpu_pd"):
            legacy_group = "warm"
        else:
            legacy_group = ""
        role = entry.get("role", "ref" if named_ref else "variant" if entry.get("knobs") else "other")
        return role, entry.get("ref_group", legacy_group)

    def accepted(index):
        return rows[index].get("quality") == "receive_window_estimate"

    comparisons = []
    for index, variant in enumerate(rows):
        selected_specs = [spec for spec in specs if spec["variant"] == variant["name"] and ("repeat" not in spec or spec["repeat"] == variant.get("repeat"))]
        if len(selected_specs) > 1:
            raise ValueError("ambiguous explicit pair for variant/repeat")
        role, group = role_group(index)
        if selected_specs and entries[index].get("role") not in (None, "variant"):
            raise ValueError("explicit pair attempts to use a non-variant plan state")
        if not selected_specs and role != "variant":
            continue
        row = {"variant": variant["name"], "repeat": variant.get("repeat"),
               "ref_group": group, "knobs": entries[index].get("knobs", []),
               "quality": "not_compared", "delta_mean_mA": None,
               "aba_drift_mA": None, "unresolved": None}
        comparisons.append(row)
        if not accepted(index):
            row["reason"] = "variant is not an accepted guarded estimate"
            continue
        spec = selected_specs[0] if selected_specs else None

        def matching_ref(candidate, side):
            if not accepted(candidate) or rows[candidate].get("repeat") != variant.get("repeat"):
                return False
            ref_role, ref_group = role_group(candidate)
            if spec:
                if rows[candidate]["name"] != spec["ref_" + side]:
                    return False
                if entries[candidate].get("role") not in (None, "ref"):
                    raise ValueError("explicit pair attempts to use a non-reference plan state")
                if group and ref_group and group != ref_group:
                    raise ValueError("explicit pair crosses reference groups")
                return True
            return bool(group) and ref_role == "ref" and ref_group == group

        before = next((candidate for candidate in range(index - 1, -1, -1) if matching_ref(candidate, "before")), None)
        after = next((candidate for candidate in range(index + 1, len(rows)) if matching_ref(candidate, "after")), None)
        if before is None or after is None:
            row["reason"] = "no preceding/following accepted matching references in this repeat; provide --pairs for an ambiguous legacy plan"
            continue
        a0, a1 = rows[before], rows[after]
        if all(key in item for item in (a0, variant, a1) for key in ("measure_start_us", "end_us")):
            malformed = any(
                type(item["measure_start_us"]) is not int or type(item["end_us"]) is not int
                or item["end_us"] <= item["measure_start_us"]
                for item in (a0, variant, a1))
            if malformed or not (a0["end_us"] <= variant["measure_start_us"]
                                 < variant["end_us"] <= a1["measure_start_us"]):
                raise ValueError("A/B/A references do not bracket the variant on the virtual timeline")
        count = a0["sample_count"] + a1["sample_count"]
        baseline = math.fsum((a0["mean_mA"] * a0["sample_count"], a1["mean_mA"] * a1["sample_count"])) / count
        delta = variant["mean_mA"] - baseline
        drift = abs(a1["mean_mA"] - a0["mean_mA"])
        if not all(finite_number(value) for value in (baseline, delta, drift)):
            raise ValueError("non-finite A/B/A arithmetic")
        row.update({"quality": "matched_receive_window_difference", "ref_before": a0["name"], "ref_after": a1["name"],
                    "ref_before_mean_mA": a0["mean_mA"], "ref_after_mean_mA": a1["mean_mA"],
                    "ref_before_sample_count": a0["sample_count"], "ref_after_sample_count": a1["sample_count"],
                    "reference_sample_count": count, "reference_mean_mA": baseline,
                    "variant_sample_count": variant["sample_count"], "variant_mean_mA": variant["mean_mA"],
                    "delta_mean_mA": delta, "aba_drift_mA": drift, "unresolved": abs(delta) <= drift})
    return {"schema": 1, "matrix": "sleep", "matrix_version": 2,
            "source_voltage_mv": profile.get("source_voltage_mv"), "comparisons": comparisons,
            "baseline_method": "pool the nearest accepted preceding/following matching reference means, weighted by their valid sample counts; never borrow across repeats",
            "drift_rule": "unresolved when abs(B - count-weighted A mean) <= abs(A_after - A_before)",
            "limitations": ["No recombined quantiles. Differences are conditional guarded board-input estimates, not isolated IC supply currents.",
                            "A/B/A drift is an observed confounding diagnostic, not a statistical confidence interval or proof of causality."]}


def compare(args):
    profile = load_json(args.profile)
    pairs = load_json(args.pairs) if args.pairs else None
    report = compare_profile(profile, pairs)
    save_json(args.output, report)
    print(json.dumps({"saved": args.output, "comparisons": len(report["comparisons"]),
                      "matched": sum(row["quality"] == "matched_receive_window_difference" for row in report["comparisons"])}, indent=2))


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
    a.add_argument("--repeats", type=int, choices=(1, 2), default=1,
                   help="sleep requires 1: untouched cold state cannot be recreated")
    a.add_argument("--matrix", choices=tuple(MATRIX_VERSIONS), default="peripheral",
                   help="peripheral omits the wire key for old firmware; sleep requires matrix/version echo")
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
    r.add_argument("--pre-fault-prefix", action="store_true", help="opt-in whole rows before an evidenced raw receiver fault; never validates post-fault continuation")
    c = sub.add_parser("compare", help="offline matched count-weighted A/B/A differences; no pooled quantiles")
    c.add_argument("--profile", required=True)
    c.add_argument("--output", required=True)
    c.add_argument("--pairs", help='optional JSON: {"pairs":[{"variant":"B","ref_before":"A0","ref_after":"A1","repeat":0}]}; repeat optional')
    d = sub.add_parser("dryrun", help="USB-only functional diagnostics; no PPK2, PM, timing or current measurement")
    d.add_argument("--port", required=True)
    d.add_argument("--matrix", choices=["sleep"], default="sleep")
    d.add_argument("--output", required=True)
    d.add_argument("--timeout-s", type=float, default=60.0)
    return ap


if __name__ == "__main__":
    options = parser().parse_args()
    try:
        {"arm": arm, "collect": collect, "analyze": analyze, "compare": compare, "dryrun": dryrun}[options.action](options)
    except (OSError, ValueError, RuntimeError, TimeoutError) as exc:
        suffix = "" if options.action == "dryrun" else "\nPPK2 owner is NOT automatically stopped. Reconnect Watcher USB before explicit finish."
        sys.exit(str(exc) + suffix)
