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

from musegadget import config, muse_api
from musegadget.executor import Account, Executor
from musegadget.identity import Identity
from musegadget.link_client import DeviceDescription, Outcome
from musegadget.noise import ApplicationResponse, ServiceFrame
from musegadget.service import Backoff, Service

from test_link_client import FakeVm, Pipe, bounded_scenario


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


@pytest.fixture
def paired_device(tmp_path, monkeypatch):
    monkeypatch.setenv(config.STATE_DIR_ENV, str(tmp_path))
    config.save_json(config.PAIRING_FILE, fresh_pairing())
    vm = {"is_default": True, "vm_id": "vm-1", "vm_name": "muse", "vm_auth_token": "vm-token"}
    monkeypatch.setattr(muse_api, "fetch_vms_with_status", lambda token, api: ([vm], 200))


@pytest.mark.parametrize("failure", [None, "connection", "hook"])
def test_a_subclass_runs_its_own_device_and_work_on_the_service_session(paired_device, monkeypatch, failure):
    monkeypatch.setattr("musegadget.service.BACKOFF_BASE_S", 0)
    served = []
    commands = {"lamp.on": {"description": "Turn on the lamp", "required": {}, "optional": {}}}

    class Lamp(Service):
        def describe(self):
            return DeviceDescription(self.identity.node_id, "Lamp", "2.0", commands)

        def run_command(self, name, params, timeout_ms=None):
            return {"ok": True, "payload": {"command": name, "params": params, "timeout_ms": timeout_ms}}

        async def serve(self, session, vm, stop):
            served.append(vm["vm_id"])
            if failure == "hook" and len(served) == 1:
                raise ConnectionError("temporary gadget failure")
            return await super().serve(session, vm, stop)

    async def scenario():
        to_device, to_vm = asyncio.Queue(), asyncio.Queue()
        device_ws = Pipe(to_device, to_vm)
        vm = FakeVm(Pipe(to_vm, to_device))
        connects = []

        async def connect(url, **kwargs):
            connects.append((url, kwargs["additional_headers"]))
            if failure == "connection" and len(connects) == 1:
                raise ConnectionError("temporary network failure")
            return device_ws

        monkeypatch.setattr("websockets.asyncio.client.connect", connect)
        with tempfile.TemporaryDirectory(dir="/tmp") as tmp:
            lamp = Lamp(identity=Identity("02:00:00:00:00:01"), executor=Executor(Account.current()))
            path = Path(tmp) / "mg.sock"
            server = await lamp.serve_local(path)

            async def use_gadget():
                await vm.handshake()
                await vm.accept_control_stream()
                register = await vm.next_message()
                assert register["method"] == "link.register"
                assert register["params"] == {
                    "node_id": "homelink-000001", "display_name": "Lamp", "version": "2.0",
                    "platform": "linux", "device_family": "homehub", "model_id": "linux",
                    "is_wakeup_supported": False,
                    "commands_v2": {"lamp.on": {"description": "Turn on the lamp", "required": {}, "optional": {}}},
                }
                await vm.send_message({"type": "res", "id": register["id"], "ok": True})
                await vm.send_message({
                    "method": "link.invoke", "id": "inv-1", "command": "lamp.on",
                    "params": {"brightness": 50}, "timeout_ms": 1000,
                })
                assert await vm.next_message() == {
                    "method": "link.result", "id": "inv-1", "ok": True,
                    "payload": {"command": "lamp.on", "params": {"brightness": 50}, "timeout_ms": 1000},
                }
                message = asyncio.ensure_future(ask(path, b'{"message": "hi"}\n'))
                request = await vm.next_frame()
                assert (request.value.verb, request.value.path) == ("POST", "/chat/stream")
                assert json.loads(request.value.body) == {
                    "message": "hi", "output_modality": "text", "device_id": "homelink-000001",
                }
                await vm.send_frame(ServiceFrame.response(request.stream_id, ApplicationResponse(
                    status=200, body=b'{"message_id":"user-1"}', end_body=True,
                )))
                assert await message == {"ok": True, "status": 200, "response": {"message_id": "user-1"}}
                lamp.stop()

            tasks = [asyncio.ensure_future(lamp.run()), asyncio.ensure_future(use_gadget())]
            try:
                await asyncio.gather(*tasks)
                assert await ask(path, b'{"message": "late"}\n') == {
                    "ok": False, "error": "not connected to the Muse",
                }
            finally:
                for task in tasks:
                    task.cancel()
                await asyncio.gather(*tasks, return_exceptions=True)
                server.close()
                await server.wait_closed()
        assert connects == [
            ("wss://hatch.metaaivm.com/v1/noise?vm_id=vm-1", {"Authorization": "Bearer vm-token"}),
        ] * (2 if failure == "connection" else 1)

    asyncio.run(bounded_scenario(scenario()))
    assert served == (["vm-1", "vm-1"] if failure else ["vm-1"])


def test_cancelling_a_service_does_not_reconnect(paired_device):
    async def scenario():
        started = asyncio.Event()

        class Lamp(Service):
            async def serve(self, session, vm, stop):
                started.set()
                await stop.wait()
                return Outcome.STOPPED

        lamp = Lamp(identity=Identity("02:00:00:00:00:01"), executor=Executor(Account.current()))
        task = asyncio.ensure_future(lamp.run())
        await started.wait()
        task.cancel()
        with pytest.raises(asyncio.CancelledError):
            await task

    asyncio.run(bounded_scenario(scenario()))


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


def test_a_start_with_an_sdk_token_keeps_a_fresh_token(monkeypatch):
    monkeypatch.setattr("musegadget.service.muse_api.refresh_device_token",
                        lambda *args: pytest.fail("unexpected refresh"))
    fresh = fresh_pairing()
    assert refresh_with({"sdk_token": "mgst_token"}, fresh) is fresh


def test_a_start_without_an_sdk_token_keeps_a_fresh_token(monkeypatch):
    monkeypatch.setattr("musegadget.service.muse_api.refresh_device_token",
                        lambda *args: pytest.fail("unexpected refresh"))
    fresh = fresh_pairing()
    assert refresh_with({}, fresh) is fresh


def warnings_for(sdk_token, pairing, caplog) -> list[str]:
    """Warn twice on a Service built inside the loop (Python 3.9)."""
    async def scenario():
        service = Service(identity=Identity("02:00:00:00:00:01"),
                          executor=Executor(Account.current()), sdk_token=sdk_token)
        service._warn_if_sdk_token_changed(pairing)
        service._warn_if_sdk_token_changed(pairing)

    caplog.clear()
    with caplog.at_level("WARNING", logger="musegadget.service"):
        asyncio.run(scenario())
    return [r.getMessage() for r in caplog.records]


def test_a_token_changed_since_pairing_says_to_pair_again_once(caplog):
    pairing = {**fresh_pairing(), "sdk_token_hint": "mgst_AAAAAAA"}
    assert warnings_for("mgst_BBBBBBBtoken", pairing, caplog) == [
        "paired with SDK token mgst_AAAAAAA, not this one; run `musegadget pair --force` to use it"]


def test_the_token_pairing_used_or_an_older_pairing_says_nothing(caplog):
    assert warnings_for("mgst_AAAAAAAtoken", {"sdk_token_hint": "mgst_AAAAAAA"}, caplog) == []
    assert warnings_for(None, {"sdk_token_hint": "none"}, caplog) == []
    assert warnings_for("mgst_AAAAAAAtoken", fresh_pairing(), caplog) == []


def test_api_root_uses_api_url_v2_or_the_muse_api():
    assert muse_api.api_root("https://api.example/") == "https://api.example"
    assert muse_api.api_root() == "https://api.muse.ai"


def test_reconnected_sessions_share_command_slots(monkeypatch):
    from musegadget.link_client import MAX_CONCURRENT_INVOKES, Outcome

    sessions = []

    class Session:
        registered_at = None

        def __init__(self, **options):
            sessions.append(options)

        async def run(self, stop):
            return Outcome.CLOSED

    monkeypatch.setattr("musegadget.service.LinkSession", Session)

    async def scenario():
        service = Service(identity=Identity("02:00:00:00:00:01"), executor=Executor(Account.current()))
        vm = {"vm_id": "test", "vm_name": "test", "vm_auth_token": "unused"}
        await service._session(vm, {})
        await service._session(vm, {})
        slots = sessions[0]["invoke_slots"]
        assert slots is sessions[1]["invoke_slots"]
        other = Service(identity=service.identity, executor=service.executor)
        assert other._invoke_slots is not slots
        for _ in range(MAX_CONCURRENT_INVOKES):
            await asyncio.wait_for(slots.acquire(), 1)
        assert slots.locked()
        for _ in range(MAX_CONCURRENT_INVOKES):
            slots.release()

    asyncio.run(scenario())
