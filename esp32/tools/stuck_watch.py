#!/usr/bin/env python3
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
"""Run a test command under a freeze watchdog.

    stuck_watch.py --suite NAME [--cap SECONDS] -- command ...
    stuck_watch.py [--cap SECONDS] -- PYTHON -m unittest [-v] MODULE...
    stuck_watch.py [--cap SECONDS] -- PYTHON -m unittest discover -s DIR [-p PATTERN] [-v]

The last two forms run one module at a time, each as its own suite (esp32-host-<module>; discover runs each
module's file through discover itself), so a module that freezes is caught at its own threshold, not the whole
run's; each module prints its own unittest summary, then one line sums them up. A unittest command with options
this tool doesn't know is watched whole, unchanged. Their exit status: 124 if a module was STUCK, else 1 if one
failed, else 5 if no module ran a test, else 0. A module with no tests alongside others isn't a failure; one whose
unittest exits 5 but printed FAILED (a setUpClass that failed, on Python 3.12) is.
--failfast stops after the first failed or STUCK module. --state DIR overrides <state>.

Typical duration: the median of the suite's last 5 passing runs, as this tool recorded them in
<state>/<suite>.jsonl (per machine: a CI job that keeps no state always runs at the cap). Until 5 exist, the
watchdog fires at the cap (--cap, default 1800 s). Then it fires at twice the typical duration, at least
typical + 5 min. A run that reaches it is STUCK, never a pass:
- captured, read-only, to <state>/<suite>-STUCK-<time>-<id>.log: the output's tail, the group's processes with
  their CPU and state (where the first 8 wait: on macOS a 1 s `sample`, on Linux the wchan), the seconds since the
  last output, what the group has open (`lsof -g`); the capture is also printed;
- stopped by its own process group only: SIGTERM, then SIGKILL 10 s later to whatever of the group is left. No board
  is reset or powered off. A failed capture never keeps the run from being stopped;
- reported to $STUCK_WATCH_NOTIFY when set: that command runs with the message as its last argument (nothing
  happens when it's unset);
- recorded as STUCK (never counted toward the typical duration); "STUCK: ..." is printed last; the exit status is
  124, also for a split unittest run with a STUCK module.
Otherwise the exit status is the command's (5 becomes 1 when the output had unittest's FAILED summary, or its
end wasn't read), and the run is recorded as PASS or FAIL. The command's output goes
through unchanged; its stdin is this tool's. <state> is $STUCK_WATCH_DIR, else
~/.cache/stuck-watch.
"""
import argparse
import fnmatch
import json
import math
import os
import platform
import re
import secrets
import shlex
import signal
import socket
import statistics
import subprocess
import sys
import threading
import time

TAIL_BYTES = 64 * 1024
TAIL_LINES = 80
MIN_RUNS = 5
GRACE_S = 10
FLOOR_S = 300   # at least typical + 5 min
DEFAULT_CAP_S = 1800
SAMPLED = 8   # processes whose wait is captured (a 1 s sample each on macOS) before the stop


def state_dir():
    return os.environ.get("STUCK_WATCH_DIR", os.path.expanduser("~/.cache/stuck-watch"))


def safe_name(suite):
    """The suite as one file name component."""
    return re.sub(r"[^A-Za-z0-9._-]", "_", suite)[:120] or "_"


def typical(state, suite):
    """The median of the last MIN_RUNS passing durations, or None."""
    passed = []
    try:
        with open(os.path.join(state, safe_name(suite) + ".jsonl")) as f:
            for line in f:
                try:
                    r = json.loads(line)
                except ValueError:
                    continue
                if isinstance(r, dict) and r.get("result") == "PASS" and r.get("tool") == "stuck_watch" and \
                        isinstance(r.get("seconds"), (int, float)) and math.isfinite(r["seconds"]) and \
                        r["seconds"] > 0:
                    passed.append(r["seconds"])
    except OSError:
        return None
    if len(passed) < MIN_RUNS:
        return None
    return statistics.median(passed[-MIN_RUNS:])


def threshold(typ, cap):
    if typ is None:
        return float(cap)
    return max(2.0 * typ, typ + FLOOR_S)


def record(state, suite, start, seconds, rc, result):
    """Best effort: a history that can't be written never changes the run's result or its report."""
    try:
        with open_private(os.path.join(state, safe_name(suite) + ".jsonl"), os.O_CREAT | os.O_APPEND) as f:
            f.write(json.dumps({"suite": suite, "start": int(start), "seconds": max(round(seconds, 3), 0.001), "rc": rc,
                                "result": result, "host": socket.gethostname(), "tool": "stuck_watch"}) + "\n")
    except OSError as e:
        print(f"stuck_watch: history not written ({e})", file=sys.stderr)


def run_quiet(cmd, timeout=15):
    """A local read-only tool's output (killed on timeout)."""
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, errors="replace", timeout=timeout,
                           stdin=subprocess.DEVNULL)
        return r.stdout + (r.stderr if r.returncode else "")
    except (OSError, subprocess.SubprocessError) as e:
        return f"({cmd[0]} failed: {e})\n"


def group_ps(pgid):
    """`ps` rows of the processes in process group pgid (ps -g means a session on Linux, a group on macOS)."""
    out = run_quiet(["ps", "-Ao", "pid,ppid,pgid,pcpu,stat,etime,args"])
    lines = out.splitlines()
    rows = [l for l in lines[1:] if len(l.split()) > 2 and l.split()[2] == str(pgid)]
    return lines[:1] + rows


def group_pids(pgid):
    return [int(l.split()[0]) for l in group_ps(pgid)[1:]]


def capture(pgid, idle_s):
    """What the frozen run looks like, read-only."""
    parts = ["-- processes (its group: CPU, state, elapsed) --",
             "\n".join(group_ps(pgid)),
             f"-- idle --\nno output for {idle_s:.0f} s; each process's %CPU above"]
    for pid in group_pids(pgid)[:SAMPLED]:
        if platform.system() == "Darwin":
            s = run_quiet(["sample", str(pid), "1", "-mayDie"], timeout=30)
            frames = [line.rstrip() for line in s.splitlines() if re.match(r"^\s+\d+ ", line)]
            parts.append(f"-- {pid}: where it waits (innermost last) --\n" + "\n".join(frames[-6:]))
        else:
            try:
                with open(f"/proc/{pid}/wchan") as f:
                    wchan = f.read().strip()
            except OSError:
                wchan = "?"
            parts.append(f"-- {pid}: waits in {wchan or '-'} --")
    parts.append("-- what the group has open (lsof -g) --")
    parts.append(run_quiet(["lsof", "-nP", "-g", str(pgid)], timeout=30))
    return "\n".join(p.rstrip("\n") for p in parts) + "\n"


def group_alive(pgid):
    try:
        os.killpg(pgid, 0)
        return True
    except ProcessLookupError:
        return False
    except PermissionError:
        return True


def stop_group(p, pgid):
    """SIGTERM to the group; SIGKILL to the group if any of it is left GRACE_S after the TERM, leader gone or not."""
    try:
        os.killpg(pgid, signal.SIGTERM)
    except OSError:
        pass
    deadline = time.monotonic() + GRACE_S
    while time.monotonic() < deadline and group_alive(pgid):
        if p.poll() is not None:
            time.sleep(0.2)   # the leader is reaped; the rest of the group gets the same grace
            continue
        try:
            p.wait(timeout=0.2)
        except subprocess.TimeoutExpired:
            pass
    killed = group_alive(pgid)
    if killed:
        try:
            os.killpg(pgid, signal.SIGKILL)
        except OSError as e:
            return f"SIGKILL to process group {pgid} failed: {e}"
    try:
        p.wait(timeout=GRACE_S)
    except subprocess.TimeoutExpired:
        return f"sent process group {pgid} SIGKILL; its leader {p.pid} hasn't exited"
    return f"stopped process group {pgid}" + (" (SIGKILL after the grace)" if killed else "")


def notify(message):
    hook = os.environ.get("STUCK_WATCH_NOTIFY")
    if not hook:
        return "(no STUCK_WATCH_NOTIFY)"
    try:
        argv = shlex.split(hook)
    except ValueError as e:
        return f"(STUCK_WATCH_NOTIFY not run: {e})"
    return run_quiet([*argv, message], timeout=60).strip()[:500] if argv else "(STUCK_WATCH_NOTIFY is empty)"


def watch(suite, cmd, cap, state):
    os.makedirs(state, mode=0o700, exist_ok=True)   # before the run: a bad state path fails here, not mid-stop
    typ = typical(state, suite)
    limit = threshold(typ, cap)
    run_id = secrets.token_hex(8)
    tail = bytearray()
    lock = threading.Lock()
    start = time.time()
    t0 = time.monotonic()
    last_out = [t0]
    done = threading.Event()
    failed_seen = [False]
    eof = threading.Event()   # set only when the output was read to its end

    def pump():
        fd = p.stdout.fileno()
        forward = True
        while True:
            chunk = os.read(fd, 65536)
            if not chunk:
                eof.set()
                return
            with lock:
                last_out[0] = time.monotonic()
                # unittest's FAILED summary, seen anywhere in the stream (the tail is bounded; a line may span reads)
                if not failed_seen[0] and re.search(rb"(?:^|\n)FAILED \(", bytes(tail[-16:]) + chunk if tail else
                                                    b"\n" + chunk):
                    failed_seen[0] = True
                tail.extend(chunk)
                del tail[:-TAIL_BYTES]
            if done.is_set():   # a survivor of a finished run never writes into the next one's output
                return
            if forward:
                try:
                    sys.stdout.buffer.write(chunk)
                    sys.stdout.buffer.flush()
                except (OSError, ValueError, AttributeError):   # our stdout went away (`| head`, closed): keep draining
                    forward = False
                    stdout_gone()

    p = None
    try:   # from the moment the run exists, any way out of here stops it first
        with deferred_signals() as held:   # an interrupt while the run starts is acted on once it's running
            p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, start_new_session=True)
            t = threading.Thread(target=pump, daemon=True)
            t.start()
        if held.got:   # checked once the handlers are back: none held during the start is missed
            raise KeyboardInterrupt
        rc = p.wait(timeout=limit)
        t.join(timeout=5)   # (an interrupt while the output drains still stops what's left of the group)
        done.set()
        if rc == 5:   # Python 3.12's unittest says "no tests ran" before it checks errors: a failed setUpClass exits 5
            with lock:
                if failed_seen[0] or not eof.is_set():   # a FAILED summary, or output not read to its end
                    rc = 1
        record(state, suite, start, time.monotonic() - t0, rc, "PASS" if rc == 0 else "FAIL")
        return rc
    except subprocess.TimeoutExpired:
        try:
            with deferred_signals() as held:   # Ctrl-C or SIGTERM from here waits until the run is stopped and reported
                rc = stuck(suite, cmd, state, start, time.monotonic() - t0, limit, typ, p, run_id, tail, lock,
                           last_out, done)
        except KeyboardInterrupt:   # one that came just before the signals were held
            done.set()
            with deferred_signals():
                say("stuck_watch: stopped the run on the way out: " + stop_group(p, p.pid))
            raise
        if held.got:
            raise KeyboardInterrupt
        return rc
    except BaseException:   # Ctrl-C, SIGTERM (below), or a failure starting the pump: the run never outlives this tool
        done.set()
        if p is not None and (p.poll() is None or group_alive(p.pid)):   # the group, even once its leader is gone
            with deferred_signals():
                say("stuck_watch: stopped the run on the way out: " + stop_group(p, p.pid))
        raise


def stdout_gone():
    """Our stdout went away (`| head`): point it at /dev/null, so no later write or the exit's flush can fail."""
    try:
        fd = os.open(os.devnull, os.O_WRONLY)
        os.dup2(fd, sys.stdout.fileno())
        os.close(fd)
    except (OSError, ValueError, AttributeError):
        pass


def say(text):
    """This tool's own lines; a stdout that went away never turns a result into a traceback or another status."""
    try:
        print(text, flush=True)
    except UnicodeEncodeError:   # a stdout that isn't UTF-8 still gets the line
        try:
            sys.stdout.buffer.write(text.encode("utf-8", "replace") + b"\n")
            sys.stdout.buffer.flush()
        except (OSError, ValueError, AttributeError):
            stdout_gone()
    except (OSError, ValueError):
        stdout_gone()


def open_private(path, flags):
    """A state file, never through a symlink."""
    return os.fdopen(os.open(path, flags | os.O_WRONLY | getattr(os, "O_NOFOLLOW", 0), 0o600), "a")


def stuck(suite, cmd, state, start, took, limit, typ, p, run_id, tail, lock, last_out, done):
    pgid = p.pid
    typ_text = "none yet" if typ is None else "%.0f s" % typ
    with lock:
        tail_text = bytes(tail).decode("utf-8", "replace")
        idle_s = time.monotonic() - last_out[0]
    tail_text = "\n".join(tail_text.splitlines()[-TAIL_LINES:]) + "\n"
    out = os.path.join(state, "%s-STUCK-%s-%s.log" % (safe_name(suite), time.strftime("%Y%m%dT%H%M%S"), run_id))
    text = ""
    try:
        try:
            snap = capture(pgid, idle_s)
        except Exception as e:   # the stop must happen whatever the capture does
            snap = f"(capture failed: {e!r})\n"
        text = ("suite %s: STUCK after %.0f s (threshold %.0f s, typical %s) on %s\ncommand: %s\n\n%s\n"
                "-- output, last %d lines --\n%s" % (suite, took, limit, typ_text, socket.gethostname(), " ".join(cmd),
                                                     snap, TAIL_LINES, tail_text))
        try:
            with open_private(out, os.O_CREAT | os.O_EXCL) as f:
                f.write(text)
        except OSError as e:
            out = f"(capture not saved: {e})"
    finally:
        stopped = stop_group(p, pgid)
        done.set()   # nothing of the stopped run reaches the output after this
    record(state, suite, start, took, None, "STUCK")
    msg = "STUCK (not a pass): %s on %s after %.0f s (threshold %.0f s, typical %s); %s; capture %s" % (
        suite, socket.gethostname(), took, limit, typ_text, stopped, out)
    told = notify(msg)
    try:
        if os.path.isfile(out):
            with open_private(out, os.O_APPEND) as f:
                f.write("\n-- stop --\n" + stopped + "\n-- notify --\n" + told + "\n")
    except OSError:
        pass
    try:
        sys.stdout.flush()
    except (OSError, ValueError, AttributeError):
        stdout_gone()
    # the capture in the output too: a CI job's state directory goes with its runner
    say("\n-- stuck_watch capture (%s) --\n%s" % (out, text))
    say("STUCK: %s after %.0f s (threshold %.0f s); %s; state and log tail in %s" % (suite, took, limit, stopped, out))
    return 124


UNITTEST_FLAGS = {"-v", "--verbose", "-q", "--quiet", "-f", "--failfast", "-c", "--catch", "-b", "--buffer",
                  "--locals"}


def unittest_modules(cmd):
    """[(module, command)] for `PYTHON -m unittest [flags] MODULE...` or `... discover -s DIR [-p PAT] [flags]`, else
    None (an option this tool doesn't know: the command is watched whole)."""
    if len(cmd) < 4 or cmd[1:3] != ["-m", "unittest"]:
        return None
    args = cmd[3:]
    if args[:1] == ["discover"]:
        rest, flags, opts = args[1:], [], {"-s": ".", "-p": "test*.py"}
        long = {"--start-directory": "-s", "--pattern": "-p"}
        i = 0
        while i < len(rest):
            a = rest[i]
            if a in UNITTEST_FLAGS:
                flags.append(a)
                i += 1
            elif a in opts or a in long:
                if i + 1 >= len(rest):
                    return None
                opts[long.get(a, a)] = rest[i + 1]
                i += 2
            else:
                return None   # -t, -k, positional start/pattern, anything else: watched whole
        try:
            entries = os.listdir(opts["-s"])
        except OSError:
            return None
        if any(os.path.exists(os.path.join(opts["-s"], e, "__init__.py")) for e in entries) or \
                os.path.exists(os.path.join(opts["-s"], "__init__.py")):
            return None   # packages (nested modules, load_tests): discover's own walk, watched whole
        names = sorted(n[:-3] for n in entries if n.endswith(".py") and fnmatch.fnmatch(n, opts["-p"]))
        # Each module through discover itself (its import rules and skips), one file pattern at a time.
        runs = [(n, cmd[:3] + ["discover", "-s", opts["-s"], "-p", n + ".py"] + flags) for n in names]
        return runs or None
    if any(a.startswith("-") and a not in UNITTEST_FLAGS for a in args):
        return None
    flags = [a for a in args if a.startswith("-")]
    mods = [a for a in args if not a.startswith("-")]
    return [(os.path.splitext(os.path.basename(m))[0] if m.endswith(".py") else m, cmd[:3] + flags + [m])
            for m in mods] or None


# The watchdog's own end (a terminal closed, ssh dropped, Ctrl-\\, kill): handled like Ctrl-C, so the run, in a
# session of its own and never sent these itself, is stopped first.
STOP_SIGNALS = (signal.SIGINT, signal.SIGTERM, signal.SIGHUP, signal.SIGQUIT)


def _sigterm(signum, frame):
    raise KeyboardInterrupt


class deferred_signals:
    """Holds the stop signals (STOP_SIGNALS) over a critical stretch (the start, a STUCK run's capture, stop
    and report, a stop on the way out); got says whether one came, for the caller to raise once it's done."""

    def __enter__(self):
        self.got = []

        self.old = {s: signal.signal(s, lambda n, f: self.got.append(n)) for s in STOP_SIGNALS}
        return self

    def __exit__(self, *exc):
        for s, h in self.old.items():
            signal.signal(s, h)


def main(argv=None):
    for sig in STOP_SIGNALS[1:]:
        signal.signal(sig, _sigterm)
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--suite")
    ap.add_argument("--cap", type=float, default=DEFAULT_CAP_S, help="seconds, until 5 passing runs exist")
    ap.add_argument("--state", default=state_dir())
    ap.add_argument("cmd", nargs=argparse.REMAINDER)
    a = ap.parse_args(argv)
    cmd = a.cmd[1:] if a.cmd[:1] == ["--"] else a.cmd
    if not cmd:
        ap.error("no command")
    split = None if a.suite else unittest_modules(cmd)
    if not split:
        return watch(a.suite or os.path.basename(cmd[0]), cmd, a.cap, a.state)
    failed, stuck, empty, ran = [], [], [], 0
    failfast = any(f in ("-f", "--failfast") for f in split[0][1])
    for m, run in split:
        rc = watch("esp32-host-" + m.split(".")[-1], run, a.cap, a.state)
        ran += 1
        # 5: unittest ran no tests in that module (Python 3.12+), which a whole discover run doesn't fail on
        (stuck if rc == 124 else empty if rc == 5 else failed if rc else []).append(m)
        if failfast and (stuck or failed):
            break
    say("stuck_watch: %d modules, %d failed, %d STUCK%s%s%s" % (
        ran, len(failed), len(stuck), (" (%d not run: --failfast)" % (len(split) - ran)) if ran < len(split) else "",
        (" (failed: " + " ".join(failed) + ")") if failed else "",
        (" (STUCK: " + " ".join(stuck) + ")") if stuck else ""))
    if not stuck and not failed and empty and len(empty) == len(split):
        return 5   # no module ran a test: as unittest says it
    return 124 if stuck else 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
