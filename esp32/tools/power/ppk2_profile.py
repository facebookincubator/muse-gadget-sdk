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

"""Continuous PPK2 owner CLI. --help and control never open a serial device."""

import argparse
import json
import math
import selectors
import shlex
import signal
import socket
import sys
from pathlib import Path

if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
    from power.ppk2_backend import port_descriptions, validate_voltage
    from power.ppk2_control import ControlServer, dispatch, send_command
    from power.ppk2_session import PPK2Session, SafetyAcknowledgment
    from power.ppk2_stats import JSONLWriter, RawWriter
else:
    from .ppk2_backend import port_descriptions, validate_voltage
    from .ppk2_control import ControlServer, dispatch, send_command
    from .ppk2_session import PPK2Session, SafetyAcknowledgment
    from .ppk2_stats import JSONLWriter, RawWriter


def build_parser():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="mode", required=True)
    sub.add_parser("discover", help="list PPK2 descriptors/both CDC interfaces; no serial opens or output changes")
    for name in ("hold", "record"):
        owner = sub.add_parser(name, help="one long-lived source owner and continuous sample drain")
        owner.add_argument("--port", required=True, help="explicit PPK2 measurement/control interface")
        owner.add_argument("--voltage-mv", type=int, required=True, help="integer mV [800,5000]; no default")
        owner.add_argument("--output", type=Path, required=True, help="new aggregate JSONL (never overwrite)")
        owner.add_argument("--control-dir", type=Path, help="new private FIFO directory; default OUTPUT.control")
        owner.add_argument("--raw", type=Path, help="optional new little-endian <Qf raw file")
        owner.add_argument("--max-output-bytes", type=int, default=16 * 1024 * 1024)
        owner.add_argument("--max-raw-bytes", type=int, default=64 * 1024 * 1024)
        owner.add_argument("--window-s", type=float, default=0.1)
        owner.add_argument("--reservoir-capacity", type=int, default=4096)
        owner.add_argument("--receiver-max-bytes", type=int, default=1024 * 1024,
                           help="bounded fast raw queue byte cap [1024,16777216]")
        owner.add_argument("--gil-switch-interval-s", type=float, default=0.0005,
                           help="isolated CLI only: GIL quantum [0.0001,0.005], restored after finish")
        owner.add_argument("--dut-voltage-mv", type=float, help="optional independently measured DUT voltage in the actual fixture; NOT source setpoint")
        owner.add_argument("--ack-source-wiring", action="store_true")
        owner.add_argument("--ack-battery-isolated", action="store_true")
        owner.add_argument("--ack-no-charging-backfeed", action="store_true")
        owner.add_argument("--ack-usb-connected-before-start", action="store_true")
    client = sub.add_parser("control", help="send one command to existing owner and block for acknowledgment; no serial")
    client.add_argument("--control-dir", type=Path, required=True)
    client.add_argument("--timeout-s", type=float, default=10)
    ops = client.add_subparsers(dest="op", required=True)
    begin = ops.add_parser("begin")
    begin.add_argument("--label", required=True)
    begin.add_argument("--duration-s", type=float)
    ops.add_parser("end")
    ops.add_parser("status")
    finish = ops.add_parser("finish")
    finish.add_argument("--usb-reconnected", action="store_true", required=True,
                        help="actual user confirmation, not a timer or assumption")
    return parser


def _print(value, *, error=False):
    try:
        print(value if isinstance(value, str) else json.dumps(value, allow_nan=False),
              file=sys.stderr if error else sys.stdout, flush=True)
    except OSError:
        pass  # Closed console is NOT authorization to close PPK2.


def interactive_command(line):
    fields = shlex.split(line)
    if not fields:
        raise ValueError("begin LABEL [SECONDS], end, status, finish USB_RECONNECTED")
    if fields[0] == "begin" and 2 <= len(fields) <= 3:
        return {"op": "begin", "label": fields[1],
                "duration_s": float(fields[2]) if len(fields) == 3 else None}
    if fields in (["end"], ["status"]):
        return {"op": fields[0]}
    if fields == ["finish", "USB_RECONNECTED"]:
        return {"op": "finish", "usb_reconnected": True}
    raise ValueError("begin LABEL [SECONDS], end, status, finish USB_RECONNECTED")


def _acknowledgment(args):
    checks = [
        ("ack_source_wiring", "source wiring and polarity verified; source limit/peak (<1 A) suitable"),
        ("ack_battery_isolated", "battery physically disconnected/isolated"),
        ("ack_no_charging_backfeed", "actual fixture USB/source overlap assessed as suitable with no harmful charging backfeed; owner assertion/evidence basis recorded, no external protection inferred"),
        ("ack_usb_connected_before_start", "DUT USB is still connected; do not remove until owner READY"),
    ]
    values = []
    for field, description in checks:
        value = getattr(args, field)
        if not value and sys.stdin.isatty():
            value = input(f"Confirm {description}. Type VERIFIED: ") == "VERIFIED"
        values.append(value)
    acknowledgment = SafetyAcknowledgment(*values)
    acknowledgment.validate()
    return acknowledgment


def run_owner(args, *, isolated=False):
    validate_voltage(args.voltage_mv)
    if not math.isfinite(args.gil_switch_interval_s) or not 0.0001 <= args.gil_switch_interval_s <= 0.005:
        raise ValueError("isolated CLI GIL quantum must be 0.0001..0.005 seconds")
    acknowledgment = _acknowledgment(args)  # ALL checks before opening serial
    control_dir = args.control_dir or Path(str(args.output) + ".control")
    writer = raw = server = session = selector = None
    wake_read = wake_write = None
    previous = {}
    pending_signal = [None]
    gil_previous = None

    def on_signal(signum, frame):
        pending_signal[0] = signal.Signals(signum).name
        try:
            wake_write.send(b"!")
        except OSError:
            pass

    try:
        if isolated:
            # Only the standalone CLI opts in. Importing the reusable session
            # or calling main/run_owner from a library never changes the GIL.
            gil_previous = sys.getswitchinterval()
            sys.setswitchinterval(args.gil_switch_interval_s)
        # Prepare ALL control/signal resources before opening the source. A
        # SIGINT during setup cannot fall through a powered-on readiness gap.
        writer = JSONLWriter(args.output, args.max_output_bytes)
        if args.raw:
            raw = RawWriter(args.raw, args.max_raw_bytes)
        server = ControlServer(control_dir)
        wake_read, wake_write = socket.socketpair()
        wake_read.setblocking(False)
        wake_write.setblocking(False)
        selector = selectors.DefaultSelector()
        selector.register(server.fd, selectors.EVENT_READ, "control")
        selector.register(wake_read, selectors.EVENT_READ, "signal")
        if sys.stdin.isatty():
            selector.register(sys.stdin, selectors.EVENT_READ, "stdin")
        for sig in (signal.SIGINT, signal.SIGTERM):
            previous[sig] = signal.signal(sig, on_signal)
        session = PPK2Session(
            port=args.port, voltage_mv=args.voltage_mv, acknowledgment=acknowledgment,
            writer=writer, raw_writer=raw, window_s=args.window_s,
            reservoir_capacity=args.reservoir_capacity, dut_voltage_mv=args.dut_voltage_mv,
            receiver_max_bytes=args.receiver_max_bytes,
        )
        session.start()
    except BaseException as exc:
        # No READY: initial USB checkpoint still applies. Cleanup failures are
        # not buried behind the original startup exception.
        cleanup_errors = [] if session is None else session.cleanup_errors
        if cleanup_errors:
            _print({"startup_error": f"{type(exc).__name__}: {exc}",
                    "cleanup_errors": cleanup_errors,
                    "warning": "SOURCE STATE UNKNOWN; OFF NOT VERIFIED. Keep DUT USB connected and inspect source safely."}, error=True)
        retained = session is not None and session.started and not session.closed
        if retained:
            startup_error = f"{type(exc).__name__}: {exc}"[:512]
            session.fault = session.fault or startup_error
            session.transport_ready = False
            # Reader join timed out: DO NOT tear down the only usable control
            # handshake or restore default terminate-on-signal behavior.
            _print({"ready": False, "holding": True, "control_dir": str(control_dir),
                    "startup_error": startup_error,
                    "warning": "SOURCE STATE UNKNOWN; OFF NOT VERIFIED. Keep USB connected; owner/control retained."}, error=True)
        else:
            if gil_previous is not None:
                sys.setswitchinterval(gil_previous)
            for sig, handler in previous.items():
                signal.signal(sig, handler)
            if selector is not None:
                selector.close()
            for wake in (wake_read, wake_write):
                if wake is not None:
                    wake.close()
            if server is not None:
                server.close()
            for output in (raw, writer):
                if output is not None:
                    output.close()
            raise

    evidence = session.status()
    if (evidence["transport_ready"] and evidence["holding"] and evidence["fault"] is None
            and not evidence["cleanup_errors"] and evidence.get("cleanup_errors_omitted_count", 0) == 0):
        _print({"ready": True, "holding": True, "control_dir": str(control_dir),
                "port": args.port, "voltage_mv": args.voltage_mv,
                "finite_sample_count": evidence["total_valid_samples"],
                "first_finite_sample_host_offset_s": evidence["first_finite_sample_host_offset_s"],
                "last_data_host_offset_s": evidence["last_data_host_offset_s"],
                "receiver": evidence["receiver"],
                "gil_switch_interval_s": sys.getswitchinterval()}, error=True)
        _print("Owner ready. Only now remove DUT USB. Use begin/end for each state; keep this SAME process alive.\nReconnect DUT USB through VERIFIED protected wiring BEFORE finish USB_RECONNECTED.\nSIGINT/SIGTERM end recording but DO NOT close source ownership.", error=True)
    while not session.closed:
        try:
            for key, _ in selector.select():  # OS event wait, no file polling
                if key.data == "signal":
                    wake_read.recv(4096)
                    session.interrupt(pending_signal[0] or "signal")
                    pending_signal[0] = None
                    _print("Interrupted; owner and drain retained. Reconnect USB and confirm finish, or use control command.", error=True)
                elif key.data == "control":
                    server.service(session)
                else:
                    line = sys.stdin.readline(4097)
                    if not line:
                        selector.unregister(sys.stdin)
                        _print(f"stdin closed; owner retained. Use control --control-dir {control_dir} finish --usb-reconnected AFTER reconnection.", error=True)
                    else:
                        try:
                            if len(line) > 4096:
                                raise ValueError("interactive command exceeds 4096 characters")
                            _print({"ok": True, "result": dispatch(session, interactive_command(line))})
                        except Exception as exc:
                            _print({"ok": False, "error": str(exc)}, error=True)
                if session.closed:
                    break
        except (Exception, KeyboardInterrupt) as exc:
            # Recoverable control/UI faults must leave a functioning handshake,
            # not just a leaked non-daemon thread with no way to confirm finish.
            if not session.closed:
                session.interrupt(f"control error: {type(exc).__name__}: {exc}"[:512])
                _print(f"Control error, owner retained: {exc}", error=True)
                import time
                time.sleep(0.05)

    # Reached ONLY after explicit confirmed finish; serial was already closed.
    if gil_previous is not None:
        sys.setswitchinterval(gil_previous)
    for sig, handler in previous.items():
        signal.signal(sig, handler)
    selector.close()
    wake_read.close()
    wake_write.close()
    server.close()
    return 1 if session.cleanup_errors or session.fault else 0


def main(argv=None, *, isolated=False):
    parser = build_parser()
    args = parser.parse_args(argv)
    try:
        if args.mode == "discover":
            _print(port_descriptions())
            return 0
        if args.mode == "control":
            if not math.isfinite(args.timeout_s) or not 0.1 <= args.timeout_s <= 120:
                raise ValueError("control timeout must be 0.1..120 seconds")
            command = {"op": args.op}
            if args.op == "begin":
                command.update(label=args.label, duration_s=args.duration_s)
            elif args.op == "finish":
                command["usb_reconnected"] = args.usb_reconnected
            _print(send_command(args.control_dir, command, args.timeout_s))
            return 0
        return run_owner(args, isolated=isolated)
    except (Exception, KeyboardInterrupt) as exc:
        _print(f"{type(exc).__name__}: {exc}", error=True)
        return 1


if __name__ == "__main__":
    raise SystemExit(main(isolated=True))
