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

"""The commands a Muse can invoke on this device.

Shell commands, file operations and the owner's drop-in commands
(``musegadget.commands``) run in child processes as a separate,
unprivileged account (``run_as``), never as the service account, which owns
the device credentials. A command gets exactly the access that account has.
"""

from __future__ import annotations

import json
import logging
import os
import pwd
import select
import shutil
import signal
import socket
import subprocess
import sys
import time
from dataclasses import dataclass
from typing import Iterable

from musegadget import __version__
from musegadget.commands import DropInCommand, InvalidCommand
from musegadget.link_client import (
    DeviceDescription, MAX_CONTROL_MESSAGE_BYTES, REGISTER_ID_BYTES, encode_message,
)

log = logging.getLogger(__name__)

DEFAULT_TIMEOUT_S = 120
MAX_TIMEOUT_S = 600
# How long to wait for output after killing a timed-out command's process group.
KILL_GRACE_S = 2
# Bound draining after the direct child exits, even if descendants hold its pipes.
DRAIN_S = 0.5
# Give all three pipes a turn, even if one output stream stays readable.
READ_SLICE_S = 0.01
# /link-control accepts at most 256 KiB per message from the device; leave
# room for the JSON envelope and escaping.
MAX_OUTPUT_BYTES = 96 * 1024
SAFE_PATH = "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"

COMMAND_SPECS = {
    "system.run": {
        "description": (
            "Run a shell command on this Linux device with bash and return its "
            "stdout, stderr and exit code. Output over 96 KiB per stream is "
            "truncated."
        ),
        "required": {
            "command": {"type": "string", "description": "Shell command line to run."},
        },
        "optional": {
            "cwd": {"type": "string", "description": "Working directory. Default: the account's home."},
            "timeout_ms": {"type": "integer", "description": "Kill the command after this long. Default 120000, max 600000."},
        },
        "timeout_ms": MAX_TIMEOUT_S * 1000 + 5000,
    },
    "file.read": {
        "description": "Read up to 64 KiB of a file, base64-encoded. Call again with next_offset until eof.",
        "required": {
            "path": {"type": "string", "description": "Absolute path."},
        },
        "optional": {
            "offset": {"type": "integer", "description": "Byte offset. Default 0."},
            "limit": {"type": "integer", "description": "Max bytes, up to 65536."},
        },
    },
    "file.write": {
        "description": (
            "Write a file in chunks of up to 64 KiB (base64). Start at offset 0, "
            "send chunks in order, and set final=true on the last one; the file "
            "is replaced atomically, and only if sha256 (when given) matches."
        ),
        "required": {
            "path": {"type": "string", "description": "Absolute destination path."},
            "data_b64": {"type": "string", "description": "This chunk's bytes, base64-encoded."},
        },
        "optional": {
            "offset": {"type": "integer", "description": "Byte offset of this chunk. Default 0."},
            "final": {"type": "boolean", "description": "True on the last chunk. Default false."},
            "sha256": {"type": "string", "description": "Hex SHA-256 of the whole file, checked on the final chunk."},
            "create_parents": {"type": "boolean", "description": "Create missing parent directories. Default false."},
            "overwrite": {"type": "boolean", "description": "Replace an existing file. Default true."},
        },
    },
    "device.health": {
        "description": "Device status: uptime, load, memory, disk, temperature, software version.",
        "required": {},
        "optional": {},
    },
}


@dataclass(frozen=True)
class Account:
    name: str
    uid: int
    gid: int
    home: str

    @classmethod
    def lookup(cls, name: str) -> "Account":
        entry = pwd.getpwnam(name)
        return cls(entry.pw_name, entry.pw_uid, entry.pw_gid, entry.pw_dir)

    @classmethod
    def current(cls) -> "Account":
        entry = pwd.getpwuid(os.getuid())
        return cls(entry.pw_name, entry.pw_uid, entry.pw_gid, entry.pw_dir)


def ok(payload: dict) -> dict:
    return {"ok": True, "payload": payload}


def error(message: str) -> dict:
    return {"ok": False, "error": message}


class Executor:
    def __init__(self, account: Account, drop_ins: Iterable[DropInCommand] = ()) -> None:
        self.account = account
        self._drop_ins = {c.name: c for c in drop_ins if c.name not in COMMAND_SPECS}
        self.drop_ins = dict(self._drop_ins)

    def specs(self, *, node_id: str = "", display_name: str = "",
              version: str = __version__) -> dict:
        """Admit whole definitions in name order using the actual register envelope.

        Service supplies its metadata. Refused definitions aren't executable; the
        built-ins are always retained. No command count or character-count estimate.
        """
        specs = dict(COMMAND_SPECS)
        device = DeviceDescription(node_id, display_name, version, specs)

        def fits() -> bool:
            encoded = encode_message(device.register_message("0" * REGISTER_ID_BYTES))
            return len(encoded) <= MAX_CONTROL_MESSAGE_BYTES

        if not fits():
            raise ValueError("device metadata and built-in commands exceed the registration budget")
        admitted = {}
        for name, command in sorted(self._drop_ins.items()):
            specs[name] = command.spec
            if fits():
                admitted[name] = command
            else:
                del specs[name]
                log.warning("skipping command %s: link.register would exceed %d bytes",
                            name, MAX_CONTROL_MESSAGE_BYTES)
        self.drop_ins = admitted
        return specs

    def run(self, command: str, params: dict, timeout_ms: int | None = None) -> dict:
        try:
            if command == "system.run":
                return self.system_run(params, timeout_ms)
            if command in ("file.read", "file.write"):
                return self.file_op(command.split(".")[1], params)
            if command == "device.health":
                return ok(device_health())
            if command in self.drop_ins:
                return self.run_drop_in(self.drop_ins[command], params)
        except Exception as exc:
            log.exception("%s failed", command)
            return error(f"{type(exc).__name__}: {exc}")
        return error(f"unsupported command: {command}")

    # -- Child processes ------------------------------------------------------

    def _child_options(self) -> dict:
        env = {
            "HOME": self.account.home,
            "USER": self.account.name,
            "LOGNAME": self.account.name,
            "PATH": SAFE_PATH,
            "LANG": os.environ.get("LANG", "C.UTF-8"),
        }
        options: dict = {"env": env, "start_new_session": True}
        if os.geteuid() == 0 and self.account.uid != 0:
            options.update(user=self.account.uid, group=self.account.gid,
                           extra_groups=os.getgrouplist(self.account.name, self.account.gid))
        return options

    def system_run(self, params: dict, timeout_ms: int | None) -> dict:
        command = params.get("command")
        if not isinstance(command, str) or not command.strip():
            return error("command is required")
        requested = params.get("timeout_ms") or timeout_ms
        timeout_s = min(int(requested) / 1000 if requested else DEFAULT_TIMEOUT_S, MAX_TIMEOUT_S)
        cwd = params.get("cwd") or self.account.home
        log.info("system.run as %s (timeout %ss)", self.account.name, timeout_s)
        started = time.monotonic()
        try:
            stdout, stderr, exit_code, timed_out = self._run_child(
                ["/bin/bash", "-c", command], cwd, timeout_s)
        except OSError as exc:
            return error(f"could not start command: {exc}")
        out, out_cut = _clip(stdout)
        err, err_cut = _clip(stderr)
        return ok({
            "stdout": out,
            "stderr": err,
            "exit_code": exit_code,
            "timed_out": timed_out,
            "truncated": out_cut or err_cut,
            "duration_ms": int((time.monotonic() - started) * 1000),
        })

    def run_drop_in(self, command: DropInCommand, params: dict) -> dict:
        try:
            request = json.dumps(command.check_params(params)).encode()
        except InvalidCommand as exc:
            return error(str(exc))
        timeout_s = command.timeout_ms / 1000
        log.info("%s as %s (timeout %ss)", command.name, self.account.name, timeout_s)
        try:
            stdout, stderr, exit_code, timed_out = self._run_child(
                list(command.argv), self.account.home, timeout_s, stdin=request)
        except OSError as exc:
            return error(f"could not start {command.argv[0]}: {exc}")
        if timed_out:
            return error(f"{command.name} timed out after {timeout_s:g}s")
        if exit_code != 0:
            lines = stderr.decode(errors="replace").strip().splitlines()
            reason = f": {lines[-1][:500]}" if lines else ""
            return error(f"{command.name} exited with {exit_code}{reason}")
        out, cut = _clip(stdout)
        if not cut:
            try:
                payload = json.loads(out)
            except ValueError:
                payload = None
            if isinstance(payload, dict):
                return ok(payload)
        return ok({"output": out, "truncated": True} if cut else {"output": out})

    def _run_child(self, argv: list, cwd: str, timeout_s: float,
                   stdin: bytes | None = None) -> tuple[bytes, bytes, int, bool]:
        """Run ``argv`` as the account. Returns stdout, stderr, exit code, timed out.

        On timeout the whole process group is killed. Raises OSError if it
        can't start.
        """
        proc = subprocess.Popen(
            argv, cwd=cwd,
            stdin=subprocess.DEVNULL if stdin is None else subprocess.PIPE,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, bufsize=0,
            **self._child_options(),
        )
        try:
            timed_out, stdout, stderr = _wait_for_shell(proc, timeout_s, stdin)
            if timed_out:
                _kill_group(proc)
                _, more_out, more_err = _wait_for_shell(proc, KILL_GRACE_S)
                stdout = (stdout + more_out)[:MAX_OUTPUT_BYTES + 1]
                stderr = (stderr + more_err)[:MAX_OUTPUT_BYTES + 1]
            proc.wait(timeout=KILL_GRACE_S)
            return stdout, stderr, proc.returncode, timed_out
        except BaseException:
            # Close and reap our own child on cancellation or a pipe failure, too.
            _kill_group(proc)
            proc.wait(timeout=KILL_GRACE_S)
            raise
        finally:
            for pipe in (proc.stdin, proc.stdout, proc.stderr):
                if pipe is not None:
                    pipe.close()

    def file_op(self, op: str, params: dict) -> dict:
        request = json.dumps({**params, "op": op})
        proc = subprocess.run(
            [sys.executable, "-m", "musegadget.fileops"], input=request.encode(),
            capture_output=True, timeout=60, cwd="/", **self._child_options(),
        )
        try:
            return json.loads(proc.stdout)
        except json.JSONDecodeError:
            return error(proc.stderr.decode(errors="replace")[-2000:] or "file operation failed")


def _read_available(pipe, deadline: float) -> tuple[bytes, bool]:
    # Stop at the deadline too: a writer that outpaces the reads would
    # otherwise keep this loop from ever seeing an empty pipe.
    chunks = bytearray()
    while time.monotonic() < deadline:
        try:
            chunk = pipe.read(65536)
        except BlockingIOError:
            return bytes(chunks), True
        if chunk is None:
            return bytes(chunks), True
        if chunk == b"":
            return bytes(chunks), False
        chunks.extend(chunk[:max(0, MAX_OUTPUT_BYTES + 1 - len(chunks))])
    return bytes(chunks), True


def _wait_for_shell(proc, timeout_s: float, stdin: bytes | None = None) -> tuple[bool, bytes, bytes]:
    # Wait until the shell exits. Read only bytes already queued so a grandchild
    # that inherited the pipes cannot hold this open until EOF.
    streams: dict = {}
    for pipe in (proc.stdout, proc.stderr):
        if pipe is None:
            continue
        os.set_blocking(pipe.fileno(), False)
        streams[pipe] = bytearray()
    watching = list(streams)
    pending = memoryview(stdin or b"")
    writer = proc.stdin if proc.stdin is not None and pending else None
    if writer is not None:
        os.set_blocking(writer.fileno(), False)
    elif proc.stdin is not None:
        proc.stdin.close()
    deadline = time.monotonic() + timeout_s
    while proc.poll() is None:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            return True, _joined(streams, proc.stdout), _joined(streams, proc.stderr)
        if not watching and writer is None:
            time.sleep(min(remaining, 0.05))
            continue
        ready, writable, _ = select.select(
            watching, [writer] if writer is not None else [], [], min(remaining, 0.05))
        if writable:
            try:
                sent = os.write(writer.fileno(), pending[:4096])
                pending = pending[sent:]
            except BlockingIOError:
                pass
            except BrokenPipeError:
                pending = pending[len(pending):]
            if not pending:
                writer.close()
                writer = None
        for pipe in ready:
            data, still_open = _read_available(
                pipe, min(deadline, time.monotonic() + READ_SLICE_S))
            if data:
                _keep_output(streams[pipe], data)
            if not still_open:
                watching.remove(pipe)
    drain_deadline = time.monotonic() + DRAIN_S
    for pipe in streams:
        if pipe.closed:
            continue
        data, _ = _read_available(pipe, drain_deadline)
        if data:
            _keep_output(streams[pipe], data)
    return False, _joined(streams, proc.stdout), _joined(streams, proc.stderr)


def _joined(streams: dict, pipe) -> bytes:
    return bytes(streams.get(pipe, b""))


def _keep_output(buffer: bytearray, data: bytes) -> None:
    # One extra byte lets _clip report truncation without retaining unbounded output.
    buffer.extend(data[:max(0, MAX_OUTPUT_BYTES + 1 - len(buffer))])


def _kill_group(proc) -> None:
    try:
        os.killpg(proc.pid, signal.SIGKILL)
    except ProcessLookupError:
        pass


def _clip(data: bytes) -> tuple[str, bool]:
    cut = len(data) > MAX_OUTPUT_BYTES
    text = data[:MAX_OUTPUT_BYTES].decode("utf-8", errors="replace")
    # The budget is for the message, where json.dumps writes every non-ASCII
    # character as a 6- or 12-byte escape and each undecodable byte became
    # U+FFFD, so binary or non-Latin output can grow sixfold.
    while len(json.dumps(text)) - 2 > MAX_OUTPUT_BYTES:
        text = text[: len(text) * 3 // 4]
        cut = True
    return text, cut


def device_health() -> dict:
    health: dict = {"version": __version__, "hostname": socket.gethostname()}
    try:
        health["uptime_s"] = int(float(open("/proc/uptime").read().split()[0]))
    except OSError:
        pass
    try:
        health["load"] = [round(x, 2) for x in os.getloadavg()]
    except OSError:
        pass
    try:
        meminfo = dict(line.split(":", 1) for line in open("/proc/meminfo"))
        health["memory_mb"] = {
            "total": int(meminfo["MemTotal"].split()[0]) // 1024,
            "available": int(meminfo["MemAvailable"].split()[0]) // 1024,
        }
    except (OSError, KeyError, ValueError):
        pass
    disk = shutil.disk_usage("/")
    health["disk_gb"] = {"total": round(disk.total / 1e9, 1), "free": round(disk.free / 1e9, 1)}
    try:
        health["temperature_c"] = int(open("/sys/class/thermal/thermal_zone0/temp").read()) / 1000
    except (OSError, ValueError):
        pass
    try:
        health["model"] = open("/proc/device-tree/model").read().strip("\x00\n")
    except OSError:
        pass
    return health
