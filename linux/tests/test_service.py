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
import stat
import tempfile
import time
from pathlib import Path

import pytest

from musegadget import config, muse_api, service as service_module
from musegadget.executor import Account, Executor
from musegadget.identity import Identity
from musegadget.service import Backoff, Service
from musegadget.link_client import Outcome


class FakeSession:
    registered_at = 1.0

    def __init__(self) -> None:
        self.sent: list[str] = []

    async def send_chat(self, message: str, session_id: str | None = None) -> dict:
        self.sent.append((message, session_id))
        return {"ok": True, "status": 200, "response": None}


async def ask(path: Path, payload: bytes) -> dict:
    reader, writer = await asyncio.open_unix_connection(str(path))
    writer.write(payload)
    await writer.drain()
    reply = json.loads(await reader.readline())
    writer.close()
    return reply


def run_with_socket(check) -> None:
    async def scenario():
        # AF_UNIX paths are short on macOS, so avoid pytest's long tmp_path.
        with tempfile.TemporaryDirectory(dir="/tmp") as tmp:
            path = Path(tmp) / "mg.sock"
            service = Service(identity=Identity("02:00:00:00:00:01"),
                              executor=Executor(Account.current()))
            server = await service.serve_local(path)
            try:
                await check(service, path)
            finally:
                server.close()

    asyncio.run(scenario())


def test_local_message_is_forwarded_to_the_live_session():
    async def check(service, path):
        assert stat.S_IMODE(os.stat(path).st_mode) == 0o660
        session = FakeSession()
        service._current = session
        reply = await ask(path, json.dumps({"message": "hello from the ring"}).encode() + b"\n")
        assert reply["ok"] and session.sent == [("hello from the ring", None)]

    run_with_socket(check)


def test_local_message_can_target_a_side_chat():
    async def check(service, path):
        session = FakeSession()
        service._current = session
        sid = "4f6c7ff7-3406-4a35-8ec9-907f61fc43f3"
        assert (await ask(path, json.dumps({"message": "ring", "session_id": sid}).encode() + b"\n"))["ok"]
        assert session.sent == [("ring", sid)]
        bad = await ask(path, b'{"message": "ring", "session_id": "../x"}\n')
        assert not bad["ok"] and len(session.sent) == 1

    run_with_socket(check)


def test_local_message_fails_cleanly_when_not_connected():
    async def check(service, path):
        reply = await ask(path, b'{"message": "hi"}\n')
        assert reply == {"ok": False, "error": "not connected to the Muse"}

    run_with_socket(check)


def test_local_message_must_be_non_empty_text():
    async def check(service, path):
        service._current = FakeSession()
        for payload in (b'{"message": ""}\n', b'{"text": "hi"}\n', b'[1]\n'):
            assert not (await ask(path, payload))["ok"]
        reply = await ask(path, b"not json\n")
        assert not reply["ok"] and "JSONDecodeError" in reply["error"]

    run_with_socket(check)


def test_backoff_doubles_to_a_ceiling_and_honours_the_floor():
    backoff = Backoff()
    assert [backoff.next_delay() for _ in range(7)] == [2, 4, 8, 16, 32, 60, 60]
    backoff.reset()
    backoff.floor = 15
    assert backoff.next_delay() == 15


def test_backoff_stays_at_the_ceiling_through_a_long_outage():
    backoff = Backoff()
    for _ in range(1100):  # 2 ** 1024 no longer fits a float
        delay = backoff.next_delay()
    assert delay == 60


def refresh_with(service_kwargs, pairing, calls=1):
    """Run _maybe_refresh `calls` times on a Service built inside the loop (Python 3.9)."""
    async def scenario():
        service = Service(identity=Identity("02:00:00:00:00:01"),
                          executor=Executor(Account.current()), **service_kwargs)
        result = pairing
        for _ in range(calls):
            result = await service._maybe_refresh(result)
        return result
    return asyncio.run(scenario())


def fresh_pairing() -> dict:
    return {"access_token": "a", "refresh_token": "r", "access_token_saved_at": int(time.time())}


def test_a_start_with_an_sdk_token_refreshes_once_to_report_it(tmp_path, monkeypatch):
    monkeypatch.setenv("MUSEGADGET_STATE_DIR", str(tmp_path))
    calls = []

    def refresh(refresh_token, node_id, base_url, sdk_token):
        calls.append(sdk_token)
        return {"access_token": "new-a", "refresh_token": "new-r"}, 200

    monkeypatch.setattr("musegadget.service.muse_api.refresh_device_token", refresh)
    pairing = refresh_with({"sdk_token": "mgst_token"}, fresh_pairing(), calls=2)
    assert calls == ["mgst_token"]
    assert pairing["access_token"] == "new-a"


def test_a_start_without_an_sdk_token_keeps_a_fresh_token(monkeypatch):
    monkeypatch.setattr("musegadget.service.muse_api.refresh_device_token",
                        lambda *args: pytest.fail("unexpected refresh"))
    fresh = fresh_pairing()
    assert refresh_with({}, fresh) is fresh


def test_a_rejected_sdk_token_report_keeps_the_pairing(tmp_path, monkeypatch):
    monkeypatch.setenv("MUSEGADGET_STATE_DIR", str(tmp_path))
    monkeypatch.setattr("musegadget.service.muse_api.refresh_device_token",
                        lambda *args: (None, 401))
    fresh = fresh_pairing()
    (tmp_path / "pairing.json").write_text(json.dumps(fresh))
    assert refresh_with({"sdk_token": "mgst_token"}, fresh) is fresh
    assert (tmp_path / "pairing.json").exists()


def test_api_root_uses_api_url_v2_or_the_muse_api():
    assert muse_api.api_root("https://api.example/") == "https://api.example"
    assert muse_api.api_root() == "https://api.muse.ai"


# Drive the production run loop with synthetic API responses and a recorded
# stop-aware delay. asyncio's watchdog keeps using its real monotonic clock.
def run_auth_cycles(tmp_path, monkeypatch, statuses, outcomes=(), refresh_status=200):
    monkeypatch.setenv(config.STATE_DIR_ENV, str(tmp_path))
    config.save_json(config.PAIRING_FILE, fresh_pairing())
    trace = []

    async def scenario():
        service = Service(identity=Identity("02:00:00:00:00:01"),
                          executor=Executor(Account.current()))
        loop = asyncio.get_running_loop()
        pending_statuses = iter(statuses)
        pending_outcomes = iter(outcomes)

        def fetch(token, api):
            status = next(pending_statuses)
            trace.append(("fetch", token, status))
            if len([x for x in trace if x[0] == "fetch"]) == len(statuses) and status == 401:
                loop.call_soon_threadsafe(service.stop)
            return ([{"is_default": True}] if status == 200 else []), status

        def refresh(*args):
            trace.append(("refresh", refresh_status))
            return ({"access_token": "new-a", "refresh_token": "new-r"}
                    if refresh_status == 200 else None), refresh_status

        async def sleep(seconds):
            trace.append(("sleep", seconds))
            await asyncio.sleep(0)

        async def session(vm, pairing):
            result = next(pending_outcomes)
            trace.append(("session", result[0]))
            return result

        monkeypatch.setattr(muse_api, "fetch_vms_with_status", fetch)
        monkeypatch.setattr(muse_api, "refresh_device_token", refresh)
        monkeypatch.setattr(service, "_sleep", sleep)
        monkeypatch.setattr(service, "_session", session)
        await asyncio.wait_for(service.run(), 3)

    asyncio.run(scenario())
    return trace


def test_repeated_rejections_after_successful_refresh_are_paced(tmp_path, monkeypatch, caplog):
    trace = run_auth_cycles(tmp_path, monkeypatch, [401] * 4)
    assert [x[2] for x in trace if x[0] == "fetch"] == [401] * 4
    # The first refresh gets one immediate retry. Later rejected refreshed
    # tokens must encounter a positive delay before another fetch/refresh cycle.
    fetch_indices = [i for i, event in enumerate(trace) if event[0] == "fetch"]
    for left, right in zip(fetch_indices[1:], fetch_indices[2:]):
        assert any(event[0] == "sleep" and event[1] >= service_module.AUTH_BACKOFF_MIN_S
                   for event in trace[left + 1:right]), trace
    assert "new-a" not in caplog.text and "new-r" not in caplog.text


def test_temporary_token_rejection_retries_immediately_with_the_saved_token(tmp_path, monkeypatch):
    trace = run_auth_cycles(tmp_path, monkeypatch, [401, 200], [(Outcome.STOPPED, 0)])
    assert [x[1] for x in trace if x[0] == "fetch"] == ["a", "new-a"]
    assert [x for x in trace if x[0] == "refresh"] == [("refresh", 200)]
    assert not [x for x in trace if x[0] == "sleep"]


def test_healthy_session_resets_rejection_backoff(tmp_path, monkeypatch):
    trace = run_auth_cycles(tmp_path, monkeypatch, [401] * 6 + [200, 401, 401, 200],
                            [(Outcome.CLOSED, service_module.HEALTHY_SESSION_S), (Outcome.STOPPED, 0)])
    assert [x[1] for x in trace if x[0] == "sleep"] == [15, 15, 15, 16, 32, 2, 15]


@pytest.mark.parametrize("refresh_status, retained", [(401, False), (503, True)])
def test_forced_refresh_failure_keeps_revocation_distinct_from_transient_failure(
    tmp_path, monkeypatch, refresh_status, retained,
):
    trace = run_auth_cycles(tmp_path, monkeypatch, [401], refresh_status=refresh_status)
    assert [x[1] for x in trace if x[0] == "sleep"] == [service_module.TOKEN_RETRY_S]
    assert (tmp_path / config.PAIRING_FILE).exists() is retained
    if retained:
        assert config.load_json(config.PAIRING_FILE)["access_token"] == "a"


def test_stop_wakes_the_real_rejection_delay_without_more_api_calls(tmp_path, monkeypatch):
    monkeypatch.setenv(config.STATE_DIR_ENV, str(tmp_path))
    config.save_json(config.PAIRING_FILE, fresh_pairing())
    fetches = []

    async def scenario():
        service = Service(identity=Identity("02:00:00:00:00:01"),
                          executor=Executor(Account.current()))
        entered = asyncio.Event()
        original_sleep = service._sleep

        async def sleep(seconds):
            assert seconds >= service_module.AUTH_BACKOFF_MIN_S
            entered.set()
            await original_sleep(seconds)

        def fetch(*args):
            fetches.append(1)
            assert len(fetches) < 10, "unpaced rejection loop"
            return [], 401

        monkeypatch.setattr(muse_api, "fetch_vms_with_status", fetch)
        monkeypatch.setattr(muse_api, "refresh_device_token",
                            lambda *args: ({"access_token": "new-a", "refresh_token": "new-r"}, 200))
        monkeypatch.setattr(service, "_sleep", sleep)
        task = asyncio.ensure_future(service.run())
        try:
            await asyncio.wait_for(entered.wait(), 1)
            assert len(fetches) == 2
            service.stop()
            await asyncio.wait_for(task, 1)
            assert len(fetches) == 2
        finally:
            service.stop()
            task.cancel()
            await asyncio.gather(task, return_exceptions=True)

    asyncio.run(scenario())


def test_age_based_refresh_keeps_pairing_and_observes_retry_cooldown(tmp_path, monkeypatch):
    monkeypatch.setenv(config.STATE_DIR_ENV, str(tmp_path))
    calls = []
    fresh = fresh_pairing()
    fresh["access_token_saved_at"] -= service_module.TOKEN_REFRESH_AGE_S + 1

    def refresh(*args):
        calls.append(1)
        return None, 503

    monkeypatch.setattr(muse_api, "refresh_device_token", refresh)
    assert refresh_with({}, fresh, calls=2) is fresh
    assert calls == [1]


def test_repeated_rejection_backoff_is_bounded_in_the_run_loop(tmp_path, monkeypatch):
    trace = run_auth_cycles(tmp_path, monkeypatch, [401] * 10)
    delays = [x[1] for x in trace if x[0] == "sleep"]
    assert len(delays) == 9
    assert all(service_module.AUTH_BACKOFF_MIN_S <= delay <= service_module.BACKOFF_MAX_S
               for delay in delays)
    assert delays[-1] == service_module.BACKOFF_MAX_S
