# Copyright (c) Meta Platforms, Inc. and affiliates.
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy at http://www.apache.org/licenses/LICENSE-2.0
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""Private POSIX FIFO handshake; client blocks for replies, never polls files.

Only the owner opens serial. Control processes open private host FIFOs, NOT the
PPK2. A token binds each command to this specific lifetime. No stale directory
is reclaimed or other process killed automatically.
"""

import json
import math
import os
import re
import secrets
import selectors
import stat
from pathlib import Path

MAX_MESSAGE_BYTES = 4096  # atomic PIPE_BUF minimum on supported POSIX systems


def dispatch(session, command):
    if not isinstance(command, dict):
        raise ValueError("command must be a JSON object")
    op = command.get("op")
    allowed = {
        "begin": {"op", "label", "duration_s"}, "end": {"op"},
        "status": {"op"}, "finish": {"op", "usb_reconnected"},
    }
    if op not in allowed or set(command) - allowed[op]:
        raise ValueError("commands: begin(label, duration_s?), end, status, finish(usb_reconnected=true)")
    if op == "begin":
        return session.begin(command.get("label"), command.get("duration_s"))
    if op == "end":
        return session.end()
    if op == "finish":
        return session.finish(usb_reconnected=command.get("usb_reconnected"))
    return session.status()


class ControlServer:
    def __init__(self, directory):
        self.directory = Path(directory)
        self.directory.mkdir(mode=0o700)  # refuse existing/stale directories
        self.token = secrets.token_hex(16)
        self.fd = None
        self.buffer = b""
        self.discarding = False
        try:
            fifo = self.directory / "commands.fifo"
            os.mkfifo(fifo, 0o600)
            self.fd = os.open(fifo, os.O_RDWR | os.O_NONBLOCK)
            # RDWR keeps the FIFO live across independent short-lived clients.
            info = {"schema_version": 1, "token": self.token, "pid": os.getpid(),
                    "commands": "commands.fifo"}
            with open(self.directory / "owner.json", "x", encoding="utf-8") as stream:
                json.dump(info, stream)
        except BaseException:
            self.close()
            raise

    def _reply(self, name, result):
        if not isinstance(name, str) or not re.fullmatch(r"reply-[0-9a-f]{32}\.fifo", name):
            return
        path = self.directory / name
        payload = (json.dumps(result, allow_nan=False, separators=(",", ":")) + "\n").encode()
        if len(payload) > MAX_MESSAGE_BYTES and result.get("ok") is True and isinstance(result.get("result"), dict) and "holding" in result["result"]:
            # Preserve actionable owner/counter/transport state even when a
            # last-record copy or long cleanup errors exceed atomic PIPE_BUF.
            # Full API/local diagnostics remain unchanged; omission is explicit.
            status = dict(result["result"])
            status["last_record"] = None
            errors = status.get("cleanup_errors", [])
            status["cleanup_errors"] = errors[:2]
            status["cleanup_errors_omitted_count"] = max(0, len(errors) - 2)
            status["control_details_truncated"] = True
            def bounded(value):
                if isinstance(value, str):
                    return value[:48]
                if isinstance(value, dict):
                    return {key: bounded(item) for key, item in value.items()}
                if isinstance(value, list):
                    return [bounded(item) for item in value]
                return value
            payload = (json.dumps({"ok": True, "result": bounded(status)}, allow_nan=False, separators=(",", ":")) + "\n").encode()
        if len(payload) > MAX_MESSAGE_BYTES:
            payload = b'{"ok":false,"error":"response exceeds handshake budget"}\n'
        fd = None
        try:
            fd = os.open(path, os.O_WRONLY | os.O_NONBLOCK | os.O_NOFOLLOW)
            if not stat.S_ISFIFO(os.fstat(fd).st_mode):
                return
            if os.write(fd, payload) != len(payload):
                raise OSError("short control reply write")
        except OSError:
            # Client timed out/disappeared. Do NOT affect PPK ownership.
            pass
        finally:
            if fd is not None:
                os.close(fd)

    def service(self, session):
        try:
            data = os.read(self.fd, MAX_MESSAGE_BYTES)
        except BlockingIOError:
            return
        for byte in data:
            if byte == 10:
                if not self.discarding:
                    self._command(self.buffer, session)
                self.buffer = b""
                self.discarding = False
            elif not self.discarding:
                if len(self.buffer) >= MAX_MESSAGE_BYTES - 1:
                    self.buffer = b""
                    self.discarding = True
                else:
                    self.buffer += bytes((byte,))

    def _command(self, payload, session):
        envelope = None
        try:
            envelope = json.loads(payload)
            if not isinstance(envelope, dict) or envelope.get("token") != self.token:
                raise ValueError("wrong session token")
            result = dispatch(session, envelope.get("command"))
            response = {"ok": True, "result": result}
        except Exception as exc:
            response = {"ok": False, "error": f"{type(exc).__name__}: {exc}"[:512]}
        if isinstance(envelope, dict):
            self._reply(envelope.get("reply"), response)

    def close(self):
        if self.fd is not None:
            os.close(self.fd)
            self.fd = None
        # Keep owner.json as provenance; absence of commands FIFO means closed.
        try:
            (self.directory / "commands.fifo").unlink()
        except FileNotFoundError:
            pass


def send_command(directory, command, timeout_s=10):
    """One atomic request; one OS-blocking wait with a finite deadline."""
    if isinstance(timeout_s, bool) or not isinstance(timeout_s, (int, float)) or not math.isfinite(timeout_s) or not 0 < timeout_s <= 120:
        raise ValueError("control timeout must be >0..120 finite seconds")
    directory = Path(directory)
    info_stat = directory.stat()
    if info_stat.st_uid != os.getuid() or info_stat.st_mode & 0o077:
        raise ValueError("control directory must be owner-only (0700)")
    with open(directory / "owner.json", encoding="utf-8") as stream:
        # Owner metadata is a small fixed object, not an unbounded file read.
        info = json.loads(stream.read(MAX_MESSAGE_BYTES))
    reply_name = f"reply-{secrets.token_hex(16)}.fifo"
    reply_path = directory / reply_name
    payload = (json.dumps({"token": info["token"], "reply": reply_name, "command": command},
                          allow_nan=False, separators=(",", ":")) + "\n").encode()
    if len(payload) > MAX_MESSAGE_BYTES:
        raise ValueError("command exceeds 4096-byte handshake budget")
    os.mkfifo(reply_path, 0o600)
    reply_fd = None
    try:
        reply_fd = os.open(reply_path, os.O_RDWR | os.O_NONBLOCK | os.O_NOFOLLOW)
        command_fd = os.open(directory / "commands.fifo", os.O_WRONLY | os.O_NONBLOCK | os.O_NOFOLLOW)
        try:
            if not stat.S_ISFIFO(os.fstat(command_fd).st_mode):
                raise ValueError("commands path is not a FIFO")
            if os.write(command_fd, payload) != len(payload):
                raise OSError("short control request write")
        finally:
            os.close(command_fd)
        with selectors.DefaultSelector() as selector:
            selector.register(reply_fd, selectors.EVENT_READ)
            if not selector.select(timeout_s):
                raise TimeoutError("owner reply timed out; command outcome UNKNOWN, do not retry begin/finish blindly; ask status")
        response = json.loads(os.read(reply_fd, MAX_MESSAGE_BYTES))
        if response.get("ok") is not True:
            raise RuntimeError(response.get("error", "control command failed"))
        return response["result"]
    finally:
        if reply_fd is not None:
            os.close(reply_fd)
        reply_path.unlink(missing_ok=True)
