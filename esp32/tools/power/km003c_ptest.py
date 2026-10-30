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

"""Measure USB-powered PTEST windows with a POWER-Z KM003C (docs/power/km003c.md)."""
import argparse
from contextlib import ExitStack, closing
import json
import signal
import sys
import threading
import time
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools.power.km003c_backend import KM003CBackend
from tools.power.ppk2_stats import CurrentStats, JSONLWriter

class Console:
    def __init__(self, port):
        import serial
        self.port = serial.Serial(port=None, baudrate=115200, timeout=0.2)
        self.port.port, self.port.dtr, self.port.rts = port, False, False
        self.port.open()
        self.buffer = ""
    def send(self, line, wake=True):
        if wake:  # Terminate any partial command left in the console RX buffer.
            self.port.write(b"\n")
            self.port.flush()
            time.sleep(0.1)
        self.port.write((">" + line + "\n").encode())
        self.port.flush()
    def lines(self, seconds):
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            self.buffer += self.port.read(4096).decode("utf-8", "replace")
            while "\n" in self.buffer:
                line, self.buffer = self.buffer.split("\n", 1)
                yield line.strip()
    def close(self):
        self.port.close()

class Recorder(threading.Thread):
    """Samples are read only after join; USB reads have bounded timeouts."""
    def __init__(self, meter, rate_hz):
        super().__init__(daemon=True)
        self.meter, self.period = meter, 1.0 / rate_hz
        self.samples, self.error = [], None
        self.stopped = threading.Event()
    def run(self):
        next_read = time.monotonic()
        while not self.stopped.is_set():
            try:
                r = self.meter.read()
            except Exception as e:
                self.error = e
                return
            self.samples.append((r.host_time_s, abs(r.ibus_a) * 1e6, r.vbus_v))
            next_read += self.period
            now = time.monotonic()
            if next_read <= now:  # A slow read restarts the schedule; never burst to catch up.
                next_read = now + self.period
            self.stopped.wait(next_read - now)
    def stop(self):
        self.stopped.set()
        if self.ident is not None:
            self.join()  # Never close the meter under a live reader.
        return self.samples

def windows_from(records, anchors):
    """Least-squares device seconds -> host receipt time, including offset/drift."""
    if not anchors:
        return []
    x0, y0 = anchors[0]
    xs, ys = [x - x0 for x, _ in anchors], [y - y0 for _, y in anchors]
    mx, my = sum(xs) / len(xs), sum(ys) / len(ys)
    variance = sum((x - mx) ** 2 for x in xs)
    slope = sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / variance if variance else 1.0
    def host(us):
        return y0 + my + slope * (us / 1e6 - x0 - mx)
    return [dict(state=r["id"], repeat=r["repeat"], start=host(r["measure_start_us"]), end=host(r["end_us"]))
            for r in records.values() if r.get("measure_start_us") and r.get("end_us")]

def summarise(samples, window, guard_s, rate_hz):
    lo, hi = window["start"] + guard_s, window["end"] - guard_s
    inside = [s for s in samples if lo <= s[0] <= hi]
    if hi <= lo or len(inside) < max(2, 0.8 * rate_hz * (hi - lo)):
        return None
    stats = CurrentStats(4096)
    stats.add([s[1] for s in inside])
    vbus_v = sum(s[2] for s in inside) / len(inside)
    # Charge integrates at the observed rate; coverage is against the requested rate.
    expected = rate_hz * (hi - lo)
    coverage = dict(expected_sample_count_estimate=expected, coverage_ratio_estimate=len(inside) / expected,
                    missing_sample_count_estimate=max(0.0, expected - len(inside)))
    return {**stats.summary(hi - lo, vbus_v * 1000, len(inside) / (hi - lo)), **coverage, "mean_vbus_v": vbus_v,
            "mean_power_mW": sum(s[1] * s[2] for s in inside) / len(inside) / 1000, "included_s": hi - lo}

def ptest_lines(console, seconds):
    for line in console.lines(seconds):
        if line.startswith("PTEST "):
            message = json.loads(line[6:])
            if not isinstance(message, dict):
                raise ValueError("expected PTEST JSON object")
            yield time.monotonic(), message

def collect_results(console, run_id):
    for _ in range(2):  # One bounded retry for a missing results response.
        console.send("ptest.results", wake=True)
        for _, message in ptest_lines(console, 5):
            if message.get("type") == "results":
                error = None
                if message.get("run_id") != run_id or message.get("state") != "complete":
                    error = "wrong run or incomplete results"
                return message, error
    return {}, "missing results"

def reconcile(ack, repeats, records, windows):
    plan = [s["id"] for s in ack.get("plan", [])]
    planned = {(i, r) for r in range(repeats) for i in plan}
    measured = {(w["state"], w["repeat"]) for w in windows}
    if not plan or len(set(plan)) != len(plan) or records.keys() != planned:
        return "results/plan mismatch"
    if measured - planned or len(measured) != len(windows):
        return "unexpected or duplicate windows"
    for key, record in records.items():
        if record.get("status") not in ("ok", "skipped"):
            return "unfinished or failed record"
        if (record["status"] == "ok" or record.get("measure_start_us", 0)) and key not in measured:
            return "missing captured window"

def main(argv=None, *, meter_factory=KM003CBackend, console_factory=Console):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--console", required=True)
    parser.add_argument("--output", type=Path, required=True, help="new JSONL file (never overwritten)")
    parser.add_argument("--run-id", default=time.strftime("usb5v-%Y%m%d-%H%M%S"))
    parser.add_argument("--settle-ms", type=int, default=5000)
    parser.add_argument("--capture-ms", type=int, default=20000)
    parser.add_argument("--repeats", type=int, default=1)
    parser.add_argument("--guard-s", type=float, default=1.0, help="dropped at both window ends")
    parser.add_argument("--rate-hz", type=float, default=20.0)
    parser.add_argument("--boot-wait-s", type=float, default=20.0, help="console open may reset board")
    parser.add_argument("--meter-serial")
    parser.add_argument("--charging-off", action="store_true")
    args = parser.parse_args(argv)
    if not 0 < args.rate_hz <= 40 or args.guard_s < 0 or args.capture_ms / 1000 <= 2 * args.guard_s:
        parser.error("rate must be in (0, 40]; capture must exceed twice the nonnegative guard")
    ack, anchors, results, results_error, records = None, [], {}, None, {}
    with ExitStack() as stack:
        meter = stack.enter_context(closing(meter_factory(args.meter_serial)))
        writer = stack.enter_context(closing(JSONLWriter(args.output)))
        def interrupted(signum, frame):
            raise KeyboardInterrupt(signal.Signals(signum).name)
        for signum in (signal.SIGTERM, signal.SIGHUP):
            previous = signal.signal(signum, interrupted)
            stack.callback(signal.signal, signum, previous)
        recorder = Recorder(meter, args.rate_hz)
        armed = False
        try:
            console = stack.enter_context(closing(console_factory(args.console)))
            for _ in console.lines(args.boot_wait_s):
                pass
            recorder.start()
            arm = dict(run_id=args.run_id, settle_ms=args.settle_ms, capture_ms=args.capture_ms,
                       repeats=args.repeats, usb_powered=1)
            if args.charging_off:
                arm["charging_off"] = 1
            armed = True  # A partial send may already have armed the device.
            console.send("ptest.arm=" + json.dumps(arm), wake=False)
            for at, m in ptest_lines(console, 10):
                if m.get("type") in ("arm_ack", "error"):
                    ack = m
                    if "now_us" in m: anchors.append((m["now_us"] / 1e6, at))
                    break
            if ack and ack.get("type") == "error":
                armed = False  # Refused: never abort a run this tool did not start.
                raise ValueError(f"USB sweep did not arm: {ack.get('message')} ({ack.get('error')})")
            if not ack or ack.get("type") != "arm_ack" or not ack.get("usb_powered") or not anchors:
                raise ValueError("USB sweep did not arm")
            if args.charging_off and ack.get("charging_off") is not True:
                raise ValueError("charging_off not acknowledged")
            deadline = time.monotonic() + ack.get("max_duration_ms", 3600000) / 1000 + 30
            next_poll, finished = time.monotonic(), False
            while time.monotonic() < deadline and not finished:
                if recorder.error is not None:
                    raise RuntimeError(f"meter error: {recorder.error}")
                if time.monotonic() >= next_poll:
                    console.send("ptest.status")
                    next_poll = time.monotonic() + 10
                for at, m in ptest_lines(console, 1):
                    if m.get("type") == "status" and m.get("run_id") == args.run_id:
                        anchors.append((m["now_us"] / 1e6, at))
                        finished = m.get("state") not in ("armed", "running")
                        if finished: break
            if not finished:
                raise TimeoutError("run exceeded duration bound")
            armed = False  # The run has ended; nothing left to abort.
            recorder.stop()
            results, results_error = collect_results(console, args.run_id)
            if results.get("run_id") == args.run_id:
                records = {(r["id"], r["repeat"]): r for r in results.get("records", [])}
                if len(records) != len(results.get("records", [])):
                    results_error = "duplicate records"
        except (Exception, KeyboardInterrupt, SystemExit) as e:
            results_error = f"run error: {e}"
        finally:
            if armed:
                try:
                    console.send("ptest.abort")  # The newline ends any interrupted command.
                except (Exception, KeyboardInterrupt, SystemExit) as e:
                    results_error = results_error or f"abort failed: {e}"
            samples = recorder.stop()
        windows = windows_from(records, anchors)
        results_error = results_error or reconcile(ack or {}, args.repeats, records, windows)
        failures = [results_error] if results_error else []
        if recorder.error is not None:
            failures.append(f"meter error: {recorder.error}")
        rows = []
        for window in windows:
            record = records.get((window["state"], window["repeat"]), {})
            summary = summarise(samples, window, args.guard_s, args.rate_hz)
            if summary is None:
                reason = "window too short after guards or sample coverage below 80% (minimum two samples)"
                failures.append(f"{window['state']}#{window['repeat']}: {reason}")
            rows.append({"type": "window", **window, "status": record.get("status"), "record": record,
                         **(summary or {"too_short": True, "error": reason})})
        writer.write({"type": "header", "meter": "POWER-Z KM003C", "measures": "USB 5 V input",
                      "arm_ack": ack, "results_state": results.get("state"), "results_error": results_error,
                      "results": results, "guard_s": args.guard_s, "charging_off": args.charging_off,
                      "rate_hz": args.rate_hz, "meter_error": repr(recorder.error) if recorder.error else None,
                      "errors": failures})
        print(f"{'state':<26} {'status':<9} {'mean mA':>8} {'p95 mA':>8} {'mean mW':>8}")
        for row in rows:
            writer.write(row)
            if "mean_uA" in row:
                print(f"{row['state']:<26} {str(row['status']):<9} {row['mean_uA'] / 1000:8.2f} "
                      f"{row['p95_uA'] / 1000:8.2f} {row['mean_power_mW']:8.1f}")
        if failures:
            writer.write({"type": "partial_capture", "anchors": anchors, "samples": samples})
            print(f"power-test failed: {'; '.join(failures)}; data saved to {args.output}", file=sys.stderr)
        return int(bool(failures))

if __name__ == "__main__":
    sys.exit(main())
