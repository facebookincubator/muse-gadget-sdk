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

import asyncio
import json
import os
import re
from types import SimpleNamespace

import pytest

from musegadget import config, error_report
from musegadget.executor import Account, Executor
from musegadget.identity import Identity
from musegadget.link_client import Outcome
from musegadget.service import Service


@pytest.fixture
def state(tmp_path, monkeypatch):
    monkeypatch.setenv(config.STATE_DIR_ENV, str(tmp_path))
    monkeypatch.delenv(error_report.ENABLE_ENV, raising=False)
    boot_id = tmp_path / "boot_id"
    boot_id.write_text("boot-1\n")
    monkeypatch.setattr(error_report, "_BOOT_ID", boot_id)
    return tmp_path


def crash(version: str = "0.1.0") -> None:
    with pytest.raises(ValueError):
        with error_report.watch_run(version):
            raise ValueError("home-wifi at /home/owner/secret")


def pending_report() -> dict | None:
    message = error_report.pending_message()
    return message and message["report"]


def test_a_clean_run_reports_nothing(state):
    with error_report.watch_run("0.1.0"):
        assert pending_report() is None
    with error_report.watch_run("0.1.0"):
        assert pending_report() is None
    assert not (state / error_report.RECORD_FILE).exists()


def test_a_signal_or_exit_is_a_clean_run(state):
    for stop in (KeyboardInterrupt, SystemExit, asyncio.CancelledError):
        with pytest.raises(stop):
            with error_report.watch_run("0.1.0"):
                raise stop()
    with error_report.watch_run("0.1.0"):
        assert pending_report() is None


def test_an_escaped_exception_is_reported_by_the_next_run(state, monkeypatch):
    now = [100.0]
    monkeypatch.setattr(error_report, "time", SimpleNamespace(monotonic=lambda: now[0]))
    with pytest.raises(ValueError):
        with error_report.watch_run("0.1.0"):
            now[0] = 5421.9
            raise ValueError("home-wifi at /home/owner/secret")

    with error_report.watch_run("0.2.0"):
        message = error_report.pending_message()
    # What the exception said never leaves the device, only where it was raised.
    assert "home-wifi" not in json.dumps(message)
    assert "owner" not in json.dumps(message)
    detail = message["report"]["fault"].pop("detail")
    assert re.fullmatch(r"test_error_report\.py:\d+ in test_an_escaped_\w+", detail)
    assert message == {
        "method": "link.error_report",
        "report": {
            "schema_version": 1,
            "kind": "crash",
            "reason": "unhandled_exception",
            "consecutive_count": 1,
            "version": "0.1.0",
            "uptime_seconds": 5321,
            "fault": {"cause": "ValueError"},
        },
    }


def test_the_report_waits_through_clean_runs_until_it_is_sent(state):
    crash()
    with error_report.watch_run("0.1.0"):
        pass
    with error_report.watch_run("0.1.0"):
        assert pending_report()["consecutive_count"] == 1
        error_report.mark_sent()
        assert pending_report() is None
    assert not (state / error_report.RECORD_FILE).exists()


def test_crashes_in_a_row_are_counted_until_a_clean_run(state):
    crash()
    crash()
    with error_report.watch_run("0.1.0"):
        assert pending_report()["consecutive_count"] == 2
        error_report.mark_sent()
    crash()
    with error_report.watch_run("0.1.0"):
        assert pending_report()["consecutive_count"] == 1


def leave_a_run_behind(**changes) -> None:
    """The record of a run that never got to end."""
    run = {"boot_id": "boot-1", "pid": os.getpid(), "version": "0.1.0", "count": 0}
    config.save_json(error_report.RECORD_FILE, {"run": {**run, **changes}})


def test_a_service_killed_while_the_machine_stayed_up_is_reported(state):
    leave_a_run_behind()
    with error_report.watch_run("0.2.0"):
        assert pending_report() == {
            "schema_version": 1,
            "kind": "crash",
            "reason": "unclean_exit",
            "consecutive_count": 1,
            "version": "0.1.0",
        }


@pytest.mark.parametrize("changes", [
    {"boot_id": "boot-0"},          # the machine restarted or lost power
    {"pid": os.getppid()},          # that service is still running
    {"pid": 0},
    {"pid": "1"},
])
def test_a_run_that_did_not_crash_is_not_reported(state, changes):
    leave_a_run_behind(**changes)
    with error_report.watch_run("0.2.0"):
        assert pending_report() is None


def test_without_a_boot_id_only_exceptions_are_reported(state, monkeypatch):
    monkeypatch.setattr(error_report, "_BOOT_ID", state / "missing")
    leave_a_run_behind(boot_id="")
    with error_report.watch_run("0.1.0"):
        assert pending_report() is None
    crash()
    with error_report.watch_run("0.1.0"):
        assert pending_report()["reason"] == "unhandled_exception"


def test_turned_off_it_records_and_sends_nothing(state, monkeypatch):
    crash()
    monkeypatch.setenv(error_report.ENABLE_ENV, "0")
    before = (state / error_report.RECORD_FILE).read_bytes()
    with pytest.raises(ValueError):
        with error_report.watch_run("0.1.0"):
            assert error_report.pending_message() is None
            raise ValueError("unrecorded")
    assert (state / error_report.RECORD_FILE).read_bytes() == before


def test_an_unwritable_state_directory_does_not_stop_the_service(tmp_path, monkeypatch):
    blocked = tmp_path / "file"
    blocked.write_text("")
    monkeypatch.setenv(config.STATE_DIR_ENV, str(blocked / "state"))
    with pytest.raises(ValueError):
        with error_report.watch_run("0.1.0"):
            assert error_report.pending_message() is None
            raise ValueError("still raised")


def test_the_service_sends_the_report_once_a_session_registers(state, monkeypatch):
    crash()
    sent = []
    registers = iter([False, True, True])

    class Session:
        registered_at = None

        def __init__(self, **options):
            self.registered = asyncio.Event()

        async def send(self, message):
            sent.append(message)

        async def run(self, stop):
            if next(registers):
                self.registered.set()
                for _ in range(5):
                    await asyncio.sleep(0)
            return Outcome.CLOSED

    monkeypatch.setattr("musegadget.service.LinkSession", Session)

    async def scenario():
        service = Service(identity=Identity("02:00:00:00:00:01"), executor=Executor(Account.current()))
        vm = {"vm_id": "test", "vm_name": "test", "vm_auth_token": "unused"}
        await service._session(vm, {})      # ends before it registers
        assert sent == [] and pending_report() is not None
        await service._session(vm, {})
        assert pending_report() is None
        await service._session(vm, {})

    with error_report.watch_run("0.1.0"):
        asyncio.run(scenario())
    assert [message["method"] for message in sent] == ["link.error_report"]
    assert sent[0]["report"]["reason"] == "unhandled_exception"
