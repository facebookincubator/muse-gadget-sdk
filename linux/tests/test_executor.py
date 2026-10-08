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

from __future__ import annotations

import base64
import hashlib
import json
import os
import stat
import time

import pytest

from musegadget import executor
from musegadget.executor import Account, Executor


@pytest.fixture
def ex(monkeypatch, tmp_path):
    src = os.path.join(os.path.dirname(__file__), "..", "src")
    monkeypatch.setenv("PYTHONPATH", os.path.abspath(src))
    account = Account.current()
    return Executor(Account(account.name, account.uid, account.gid, str(tmp_path)))


def _child_env_passes_pythonpath(monkeypatch):
    original = Executor._child_options

    def options(self):
        opts = original(self)
        opts["env"]["PYTHONPATH"] = os.environ["PYTHONPATH"]
        return opts

    monkeypatch.setattr(Executor, "_child_options", options)


def test_system_run_returns_output_and_exit_code(ex):
    result = ex.run("system.run", {"command": "echo out; echo err >&2; exit 3"})
    assert result["ok"]
    payload = result["payload"]
    assert (payload["stdout"], payload["stderr"], payload["exit_code"]) == ("out\n", "err\n", 3)
    assert not payload["timed_out"] and not payload["truncated"]


def test_system_run_defaults_to_the_account_home(ex, tmp_path):
    cwd = ex.run("system.run", {"command": "pwd -P"})["payload"]["stdout"].strip()
    assert cwd == os.path.realpath(tmp_path)


def test_system_run_times_out_and_kills_the_process_group(ex):
    result = ex.run("system.run", {"command": "sleep 30 & sleep 30", "timeout_ms": 300})
    assert result["ok"] and result["payload"]["timed_out"]
    assert result["payload"]["duration_ms"] < 5000


def test_system_run_timeout_does_not_wait_for_a_detached_process(ex):
    # A process that left the group with setsid survives the group kill and keeps the
    # output pipes open. The timeout must still return promptly.
    started = time.monotonic()
    result = ex.run("system.run", {"command": "setsid sleep 6 & sleep 30", "timeout_ms": 500})
    elapsed = time.monotonic() - started
    assert result["ok"] and result["payload"]["timed_out"]
    assert elapsed < 4


def test_system_run_returns_when_the_shell_exits_but_a_child_holds_the_pipes(ex):
    # setsid leaves the group and keeps the inherited pipes open after bash exits 0.
    # Waiting on pipe EOF reports a timeout for a command that already succeeded.
    started = time.monotonic()
    result = ex.run("system.run", {"command": "setsid sleep 30 &", "timeout_ms": 2000})
    elapsed = time.monotonic() - started
    assert result["ok"], result
    payload = result["payload"]
    assert payload["exit_code"] == 0 and not payload["timed_out"]
    assert elapsed < 1

    echo = ex.run("system.run", {"command": "echo hi"})
    assert echo["ok"]
    assert (echo["payload"]["stdout"], echo["payload"]["exit_code"]) == ("hi\n", 0)
    assert not echo["payload"]["timed_out"]

    hung = ex.run("system.run", {"command": "sleep 30", "timeout_ms": 300})
    assert hung["ok"] and hung["payload"]["timed_out"]
    assert hung["payload"]["duration_ms"] < 5000


def test_system_run_truncates_large_output(ex):
    result = ex.run("system.run", {"command": "head -c 200000 /dev/zero | tr '\\0' x"})
    payload = result["payload"]
    assert payload["truncated"]
    assert len(payload["stdout"]) == executor.MAX_OUTPUT_BYTES


def test_clip_bounds_the_json_encoded_size():
    text, cut = executor._clip(b"x" * executor.MAX_OUTPUT_BYTES)
    assert (len(text), cut) == (executor.MAX_OUTPUT_BYTES, False)
    # Undecodable bytes become U+FFFD, which json.dumps writes as a 6-byte escape.
    text, cut = executor._clip(b"\xff" * executor.MAX_OUTPUT_BYTES)
    assert cut
    assert executor.MAX_OUTPUT_BYTES // 2 < len(json.dumps(text)) - 2 <= executor.MAX_OUTPUT_BYTES


def test_system_run_requires_a_command(ex):
    assert ex.run("system.run", {}) == {"ok": False, "error": "command is required"}


def test_unknown_command(ex):
    assert ex.run("nope", {}) == {"ok": False, "error": "unsupported command: nope"}


def test_file_write_then_read_round_trip(ex, tmp_path, monkeypatch):
    _child_env_passes_pythonpath(monkeypatch)
    target = str(tmp_path / "sub" / "blob.bin")
    data = os.urandom(150_000)
    chunks = [data[i:i + 65536] for i in range(0, len(data), 65536)]
    for i, chunk in enumerate(chunks):
        last = i == len(chunks) - 1
        result = ex.run("file.write", {
            "path": target, "offset": i * 65536, "data_b64": base64.b64encode(chunk).decode(),
            "final": last, "create_parents": True,
            "sha256": hashlib.sha256(data).hexdigest() if last else None,
        })
        assert result["ok"], result
    assert result["payload"]["complete"] and open(target, "rb").read() == data

    received, offset = b"", 0
    while True:
        result = ex.run("file.read", {"path": target, "offset": offset})
        assert result["ok"], result
        received += base64.b64decode(result["payload"]["data_b64"])
        offset = result["payload"]["next_offset"]
        if result["payload"]["eof"]:
            break
    assert received == data


def test_file_write_rejects_a_bad_checksum(ex, tmp_path, monkeypatch):
    _child_env_passes_pythonpath(monkeypatch)
    target = str(tmp_path / "f")
    result = ex.run("file.write", {"path": target, "data_b64": "aGk=", "final": True, "sha256": "00"})
    assert not result["ok"] and "sha256" in result["error"]
    assert not os.path.exists(target)


def test_file_write_rejects_out_of_order_chunks(ex, tmp_path, monkeypatch):
    _child_env_passes_pythonpath(monkeypatch)
    target = str(tmp_path / "f")
    assert ex.run("file.write", {"path": target, "data_b64": "aGk="})["ok"]
    result = ex.run("file.write", {"path": target, "data_b64": "aGk=", "offset": 5})
    assert not result["ok"] and "expected offset 2" in result["error"]


def test_file_write_keeps_the_mode_of_the_file_it_replaces(ex, tmp_path, monkeypatch):
    _child_env_passes_pythonpath(monkeypatch)
    for name, mode in (("secret", 0o600), ("script.sh", 0o755)):
        target = tmp_path / name
        target.write_bytes(b"old")
        target.chmod(mode)
        result = ex.run("file.write", {"path": str(target), "data_b64": "aGk=", "final": True})
        assert result["ok"], result
        assert target.read_bytes() == b"hi"
        assert stat.S_IMODE(target.stat().st_mode) == mode, name


def test_file_write_drops_setuid_setgid_and_sticky_bits(ex, tmp_path, monkeypatch):
    _child_env_passes_pythonpath(monkeypatch)
    target = tmp_path / "tool"
    target.write_bytes(b"old")
    target.chmod(0o6755)
    result = ex.run("file.write", {"path": str(target), "data_b64": "aGk=", "final": True})
    assert result["ok"], result
    assert stat.S_IMODE(target.stat().st_mode) == 0o755


def test_file_write_keeps_the_partial_file_private_until_it_is_done(ex, tmp_path, monkeypatch):
    _child_env_passes_pythonpath(monkeypatch)
    target = tmp_path / "new"
    assert ex.run("file.write", {"path": str(target), "data_b64": "aGk="})["ok"]
    partial = tmp_path / ".new.musegadget-partial"
    assert stat.S_IMODE(partial.stat().st_mode) == 0o600
    result = ex.run("file.write", {"path": str(target), "data_b64": "aGk=", "offset": 2, "final": True})
    assert result["ok"], result
    umask = os.umask(0)
    os.umask(umask)
    assert stat.S_IMODE(target.stat().st_mode) == 0o644 & ~umask


def test_file_paths_must_be_absolute(ex, monkeypatch):
    _child_env_passes_pythonpath(monkeypatch)
    result = ex.run("file.read", {"path": "relative.txt"})
    assert result == {"ok": False, "error": "path must be an absolute path"}


def test_device_health_reports_basics(ex):
    payload = ex.run("device.health", {})["payload"]
    assert payload["version"] and payload["hostname"] and "disk_gb" in payload
