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

from musegadget import muse_api
from musegadget.executor import Account, Executor
from musegadget.identity import Identity
from musegadget.service import Backoff, Service


class FakeSession:
    registered_at = 1.0

    def __init__(self) -> None:
        self.sent: list[str] = []

    async def send_chat(self, message: str, session_id: str | None = None) -> dict:
        self.sent.append((message, session_id))
        return {"ok": True, "status": 200, "response": None}

    async def ask_chat(self, message: str, session_id: str | None = None) -> str:
        self.sent.append((message, session_id))
        return "Muse reply"


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


def test_local_ask_waits_for_and_returns_the_muse_reply():
    async def check(service, path):
        session = FakeSession()
        service._current = session
        payload = json.dumps({
            "message": "What is the weather?", "session_id": "side-1", "wait_for_reply": True,
        }).encode() + b"\n"
        reply = await ask(path, payload)
        assert reply == {"ok": True, "response": "Muse reply"}
        assert session.sent == [("What is the weather?", "side-1")]

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
