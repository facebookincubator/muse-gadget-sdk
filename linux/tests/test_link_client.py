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
import base64
import io
import json
import struct
import wave

import pytest

from musegadget.link_client import (
    DeviceDescription, HttpStreamError, LinkSession, MAX_CONCURRENT_INVOKES, MAX_IMAGE_BYTES,
    MessageDecoder, Outcome, RequestRejected, SideChat, acknowledgement, check_image, describe_result,
    encode_message, noise_url, printable,
)
from musegadget.noise import (
    ApplicationResponse, BodyChunk, Header, NoiseFrameDecoder, NoiseXXResponder, ServiceFrame,
    encode_noise_frames,
)
from musegadget.noise.transport import decode_request_envelope, encode_response_envelope

DEVICE = DeviceDescription(
    node_id="homelink-abcdef", display_name="pi", version="0.1.0",
    commands={"system.run": {"description": "run", "required": {}, "optional": {}}},
)


class Pipe:
    """One end of an in-memory WebSocket."""

    def __init__(self, inbox: asyncio.Queue, outbox: asyncio.Queue) -> None:
        self._inbox, self._outbox = inbox, outbox

    async def send(self, data) -> None:
        await self._outbox.put(data)

    async def recv(self):
        data = await self._inbox.get()
        if data is None:
            raise ConnectionError("closed")
        return data

    async def close(self) -> None:
        await self._outbox.put(None)


class FakeVm:
    """Just enough of the Muse VM: Noise responder plus the control stream."""

    def __init__(self, ws: Pipe) -> None:
        self.ws = ws
        self.decoder = NoiseFrameDecoder()
        self.messages = MessageDecoder()
        self.stream_id = 0

    async def handshake(self) -> None:
        responder = NoiseXXResponder()
        responder.initialize()
        await self.ws.send(responder.read_message1_and_write_message2(await self.ws.recv()))
        responder.read_message3(await self.ws.recv())
        self.send_cipher, self.recv_cipher = responder.split()

    async def next_frame(self) -> ServiceFrame:
        while True:
            plain = self.recv_cipher.decrypt_with_ad(b"", await self.ws.recv())
            assembled = self.decoder.decode(plain)
            if assembled is not None:
                return decode_request_envelope(assembled)

    async def next_message(self) -> dict:
        while True:
            frame = await self.next_frame()
            assert frame.kind == "body_chunk"
            messages = self.messages.feed(frame.value.data)
            if messages:
                return messages[0]

    async def send_frame(self, frame: ServiceFrame) -> None:
        for chunk in encode_noise_frames(encode_response_envelope(frame)):
            await self.ws.send(self.send_cipher.encrypt_with_ad(b"", chunk))

    async def accept_control_stream(self, status: int = 200) -> ServiceFrame:
        request = await self.next_frame()
        self.stream_id = request.stream_id
        await self.send_frame(ServiceFrame.response(
            self.stream_id, ApplicationResponse(status=status, end_body=status >= 400),
        ))
        return request

    async def send_message(self, message: dict) -> None:
        await self.send_frame(ServiceFrame.body_chunk(
            self.stream_id, BodyChunk(data=encode_message(message)),
        ))


def make_session(run_command, connect_log: list, **options):
    to_device, to_vm = asyncio.Queue(), asyncio.Queue()
    device_ws, vm_ws = Pipe(to_device, to_vm), Pipe(to_vm, to_device)

    async def connect(url, headers):
        connect_log.append((url, headers))
        return device_ws

    session = LinkSession(
        noise_host="gw.example", vm_id="vm 1&x", vm_auth_token="tok",
        device=DEVICE, run_command=run_command, connect=connect, **options,
    )
    return session, FakeVm(vm_ws)


def test_register_invoke_result_and_unpair():
    async def scenario():
        calls, connects = [], []

        def run_command(command, params, timeout_ms):
            calls.append((command, params, timeout_ms))
            return {"ok": True, "payload": {"stdout": "hi\n", "exit_code": 0}}

        session, vm = make_session(run_command, connects)
        stop = asyncio.Event()
        task = asyncio.ensure_future(session.run(stop))

        await vm.handshake()
        request = await vm.accept_control_stream()
        assert (request.kind, request.value.verb, request.value.path, request.value.end_body) == (
            "request", "POST", "/link-control", False)

        register = await vm.next_message()
        assert register["method"] == "link.register"
        params = register["params"]
        assert params["node_id"] == "homelink-abcdef"
        assert (params["platform"], params["device_family"]) == ("linux", "homehub")
        assert "system.run" in params["commands_v2"]
        await vm.send_message({"type": "res", "id": register["id"], "ok": True})

        await vm.send_message({
            "method": "link.invoke", "id": "inv-1", "command": "system.run",
            "params": {"command": "echo hi"}, "timeout_ms": 5000,
        })
        result = await vm.next_message()
        assert result == {"method": "link.result", "id": "inv-1", "ok": True,
                          "payload": {"stdout": "hi\n", "exit_code": 0}}
        assert calls == [("system.run", {"command": "echo hi"}, 5000)]
        assert session.registered_at is not None

        await vm.send_message({"type": "evt", "event": "link.unpaired"})
        assert await asyncio.wait_for(task, 2) is Outcome.UNPAIRED
        url, headers = connects[0]
        assert url == "wss://gw.example/v1/noise?vm_id=vm%201%26x"
        assert headers == {"Authorization": "Bearer tok"}

    asyncio.run(scenario())


def test_forbidden_control_stream():
    async def scenario():
        session, vm = make_session(lambda *a: {"ok": True}, [])
        task = asyncio.ensure_future(session.run(asyncio.Event()))
        await vm.handshake()
        await vm.accept_control_stream(status=403)
        assert await asyncio.wait_for(task, 2) is Outcome.FORBIDDEN

    asyncio.run(scenario())


def test_stop_ends_the_session():
    async def scenario():
        session, vm = make_session(lambda *a: {"ok": True}, [])
        stop = asyncio.Event()
        task = asyncio.ensure_future(session.run(stop))
        await vm.handshake()
        await vm.accept_control_stream()
        await vm.next_message()
        stop.set()
        assert await asyncio.wait_for(task, 2) is Outcome.STOPPED

    asyncio.run(scenario())


def test_decoder_handles_split_and_batched_messages():
    data = encode_message({"a": 1}) + encode_message({"b": 2}) + struct.pack("<I", 0)
    decoder = MessageDecoder()
    assert decoder.feed(data[:5]) == []
    assert decoder.feed(data[5:]) == [{"a": 1}, {"b": 2}]


def test_decoder_rejects_oversize_messages():
    with pytest.raises(ValueError):
        MessageDecoder().feed(struct.pack("<I", 1 << 30))


def test_decoder_drops_messages_that_are_not_utf8():
    garbage = b'{"a":"\xff"}'
    data = struct.pack("<I", len(garbage)) + garbage + encode_message({"b": 2})
    assert MessageDecoder().feed(data) == [{"b": 2}]


def test_vm_id_is_escaped_like_encode_uri_component():
    assert noise_url("h", "a-b_c.d!~*'()?&=") == "wss://h/v1/noise?vm_id=a-b_c.d!~*'()%3F%26%3D"


def test_send_chat_posts_a_device_attributed_message_on_the_same_session():
    async def scenario():
        session, vm = make_session(lambda *a: {"ok": True}, [])
        task = asyncio.ensure_future(session.run(asyncio.Event()))
        await vm.handshake()
        await vm.accept_control_stream()
        await vm.next_message()  # link.register

        reply = asyncio.ensure_future(session.send_chat("porch light on", "side-1"))
        request = await vm.next_frame()
        assert (request.kind, request.value.verb, request.value.path, request.value.end_body) == (
            "request", "POST", "/chat/stream", True)
        assert json.loads(request.value.body) == {
            "message": "porch light on", "output_modality": "text", "device_id": "homelink-abcdef",
            "session_id": "side-1"}
        headers = {h.key.lower(): h.value for h in request.value.headers}
        assert headers["content-type"] == "application/json"
        await vm.send_frame(ServiceFrame.response(
            request.stream_id,
            ApplicationResponse(status=200, body=b'{"accepted":true}', end_body=True),
        ))
        assert await asyncio.wait_for(reply, 2) == {
            "ok": True, "status": 200, "response": {"accepted": True}}
        task.cancel()

    asyncio.run(scenario())


@pytest.mark.parametrize("fill_budget", [False, True])
def test_drop_in_command_is_registered_and_runs_over_the_session(tmp_path, fill_budget):
    import sys

    from musegadget import commands
    from musegadget.executor import Account, Executor

    program = tmp_path / "pump.py"
    program.write_text(
        "import json, sys\n"
        "print(json.dumps({'pump': 'on' if json.load(sys.stdin)['on'] else 'off'}))\n")
    (tmp_path / "pump.set.json").write_text(json.dumps({
        "description": "Switch the garden pump.",
        "exec": [sys.executable, str(program)],
        "required": {"on": {"type": "boolean", "description": "true for on."}},
    }))
    if fill_budget:
        for i in range(4):
            (tmp_path / f"custom.a{i}.json").write_text(json.dumps({
                "description": "x" * 60000, "exec": ["/bin/true"]}))
        (tmp_path / "zzz.large.json").write_text(json.dumps({
            "description": "界" * 21000, "exec": ["/bin/true"]}, ensure_ascii=False))
    (tmp_path / "aaa.bad.json").write_text(json.dumps({
        "description": "bad", "exec": ["/bin/true"],
        "required": {"x": {"type": ["string"], "description": "d"}}}))
    current = Account.current()
    executor = Executor(Account(current.name, current.uid, current.gid, str(tmp_path)),
                        commands.load(tmp_path))

    async def scenario():
        to_device, to_vm = asyncio.Queue(), asyncio.Queue()
        device_ws, vm_ws = Pipe(to_device, to_vm), Pipe(to_vm, to_device)

        async def connect(url, headers):
            return device_ws

        session = LinkSession(
            noise_host="gw.example", vm_id="vm", vm_auth_token="tok",
            device=DeviceDescription(node_id="homelink-abcdef", display_name="pi",
                                     version="0.1.0", commands=executor.specs(
                                         node_id="homelink-abcdef", display_name="pi", version="0.1.0")),
            run_command=executor.run, connect=connect,
        )
        vm = FakeVm(vm_ws)
        task = asyncio.ensure_future(session.run(asyncio.Event()))
        await vm.handshake()
        await vm.accept_control_stream()

        register = await vm.next_message()
        spec = register["params"]["commands_v2"]["pump.set"]
        assert spec["description"] == "Switch the garden pump."
        assert "exec" not in spec
        assert "system.run" in register["params"]["commands_v2"]
        from musegadget.link_client import MAX_CONTROL_MESSAGE_BYTES
        assert len(encode_message(register)) <= MAX_CONTROL_MESSAGE_BYTES
        assert "aaa.bad" not in register["params"]["commands_v2"]
        if fill_budget:
            assert "zzz.large" not in register["params"]["commands_v2"]
            assert len(register["params"]["commands_v2"]) == 9
        await vm.send_message({"type": "res", "id": register["id"], "ok": True})

        await vm.send_message({"method": "link.invoke", "id": "inv-1", "command": "pump.set",
                               "params": {"on": True}, "timeout_ms": spec["timeout_ms"]})
        assert await asyncio.wait_for(vm.next_message(), 10) == {
            "method": "link.result", "id": "inv-1", "ok": True, "payload": {"pump": "on"}}

        await vm.send_message({"method": "link.invoke", "id": "inv-2", "command": "pump.set",
                               "params": {}})
        assert await asyncio.wait_for(vm.next_message(), 10) == {
            "method": "link.result", "id": "inv-2", "ok": False, "error": "on is required"}

        if fill_budget:
            await vm.send_message({"method": "link.invoke", "id": "inv-skipped",
                                   "command": "zzz.large", "params": {}})
            assert await asyncio.wait_for(vm.next_message(), 2) == {
                "method": "link.result", "id": "inv-skipped", "ok": False,
                "error": "unsupported command: zzz.large"}

        await vm.send_message({"type": "evt", "event": "link.unpaired"})
        assert await asyncio.wait_for(task, 2) is Outcome.UNPAIRED

    asyncio.run(scenario())


def test_each_invoke_logs_how_it_ended_but_not_what_it_ran(caplog):
    async def scenario():
        results = iter([
            {"ok": True, "payload": {"stdout": "", "exit_code": 3, "timed_out": False}},
            {"ok": False, "error": "no such file: /home/pi/secret"},
        ])
        session, vm = make_session(lambda *a: next(results), [])
        task = asyncio.ensure_future(session.run(asyncio.Event()))
        await vm.handshake()
        await vm.accept_control_stream()
        await vm.next_message()  # link.register
        for invoke_id, command in (("inv-1", "system.run"), ("inv-2", "file.read\nforged")):
            await vm.send_message({"method": "link.invoke", "id": invoke_id, "command": command,
                                   "params": {"command": "cat /home/pi/secret"}})
            await vm.next_message()
        task.cancel()

    with caplog.at_level("INFO", logger="musegadget.link_client"):
        asyncio.run(scenario())
    lines = [r.getMessage() for r in caplog.records]
    assert any(line.startswith("system.run ok, exit 3 in ") and line.endswith(" ms") for line in lines)
    assert any(line.startswith("file.read?forged failed in ") for line in lines)
    assert not any("\n" in line for line in lines)
    assert not any("secret" in line for line in lines)


@pytest.mark.parametrize("result, described", [
    ({"ok": True, "payload": {"pump": "on"}}, "ok"),
    ({"ok": True}, "ok"),
    ({"ok": True, "payload": {"exit_code": 0, "timed_out": False}}, "ok, exit 0"),
    ({"ok": True, "payload": {"exit_code": -9, "timed_out": True}}, "ok, exit -9, timed out"),
    ({"ok": False, "error": "no such file: /home/pi/secret"}, "failed"),
    ({"ok": False}, "failed"),
])
def test_describe_result(result, described):
    assert describe_result(result) == described


def test_printable_replaces_control_characters():
    assert printable("system.run") == "system.run"
    assert printable("a\nb\x1b[2Jc") == "a?b?[2Jc"


@pytest.mark.parametrize("command", [42, ["system.run"], {"name": "system.run"}])
def test_non_string_invocation_gets_correlated_failure(command):
    asyncio.run(_failed_invoke_then_healthy(command, "invalid-command"))


@pytest.mark.parametrize("failure", ["raise", "invalid-result", "unserializable-result"])
def test_callback_failure_gets_correlated_result_and_session_stays_usable(failure, caplog):
    caplog.set_level("INFO", logger="musegadget.link_client")
    asyncio.run(_failed_invoke_then_healthy("fixture.fail", failure))
    expected = {"raise": "RuntimeError", "invalid-result": "AttributeError",
                "unserializable-result": "TypeError"}[failure]
    assert f"fixture.fail failed ({expected})" in caplog.text
    assert "secret callback details" not in caplog.text
    assert "secret parameter" not in caplog.text


async def _failed_invoke_then_healthy(command, failure):
    calls = []
    unhandled = []

    def run_command(name, params, timeout):
        calls.append(name)
        if name == "fixture.fail":
            if failure == "raise":
                raise RuntimeError("secret callback details")
            if failure == "unserializable-result":
                return {"ok": True, "payload": b"secret callback details"}
            return None
        return {"ok": True}

    session, vm = make_session(run_command, [])
    loop = asyncio.get_running_loop()
    previous = loop.get_exception_handler()
    loop.set_exception_handler(lambda loop, context: unhandled.append(context))
    task = asyncio.ensure_future(session.run(asyncio.Event()))
    try:
        await vm.handshake()
        await vm.accept_control_stream()
        await vm.next_message()
        await vm.send_message({"method": "link.invoke", "id": "failed-id", "command": command,
                               "params": {"value": "secret parameter"}})
        result = await asyncio.wait_for(vm.next_message(), 2)
        assert result == {"method": "link.result", "id": "failed-id", "ok": False,
                          "error": "command must be a string" if failure == "invalid-command"
                          else "command failed"}
        if failure == "invalid-command":
            assert calls == []
        await vm.send_message({"method": "link.invoke", "id": "healthy-id",
                               "command": "fixture.ok"})
        assert await asyncio.wait_for(vm.next_message(), 2) == {
            "method": "link.result", "id": "healthy-id", "ok": True}
        await vm.send_message({"event": "link.unpaired"})
        assert await asyncio.wait_for(task, 2) is Outcome.UNPAIRED
        assert not unhandled
    finally:
        task.cancel()
        await asyncio.gather(task, return_exceptions=True)
        loop.set_exception_handler(previous)


def test_invocation_transport_failure_propagates():
    async def scenario():
        session, _ = make_session(lambda *args: {"ok": True}, [])

        async def fail_send(message):
            raise ConnectionError("transport closed")

        session.send = fail_send
        with pytest.raises(ConnectionError, match="transport closed"):
            await session._invoke({"id": "inv-1", "command": "fixture.ok"})

    asyncio.run(scenario())


def test_invocation_cancelled_while_waiting_for_slot_sends_nothing():
    async def scenario():
        session, _ = make_session(lambda *args: {"ok": True}, [])
        sent = []

        async def send(message):
            sent.append(message)

        session.send = send
        for _ in range(MAX_CONCURRENT_INVOKES):
            await session._invokes.acquire()
        task = asyncio.ensure_future(session._invoke({"id": "inv-1", "command": "fixture.ok"}))
        await asyncio.sleep(0)
        task.cancel()
        with pytest.raises(asyncio.CancelledError):
            await task
        assert not sent

    asyncio.run(asyncio.wait_for(scenario(), 2))


@pytest.mark.parametrize("failure_type", [KeyboardInterrupt, SystemExit])
def test_invocation_does_not_swallow_process_control_exceptions(failure_type):
    # Calling the coroutine directly keeps the process-control exception out
    # of asyncio's task runner while exercising the real callback boundary.
    async def scenario():
        def stop_command(*args):
            raise failure_type()

        session, _ = make_session(stop_command, [])
        with pytest.raises(failure_type):
            await session._invoke({"id": "inv-1", "command": "fixture.stop"})

    asyncio.run(scenario())


def test_rejected_registration_is_not_marked_as_registered(caplog):
    async def scenario():
        session, vm = make_session(lambda *args: {}, [])
        task = asyncio.ensure_future(session.run(asyncio.Event()))
        try:
            await vm.handshake()
            await vm.accept_control_stream()
            register = await vm.next_message()
            await vm.send_message({"type": "res", "id": register["id"],
                                   "error": {"message": "registration rejected"}})
            assert await asyncio.wait_for(task, 2) is Outcome.FORBIDDEN
            assert session.registered_at is None
            assert not session.registered.is_set()
        finally:
            task.cancel()

    asyncio.run(scenario())
    assert "link.register rejected" in caplog.text


async def registered_session():
    session, vm = make_session(lambda *args: {"ok": True}, [])
    stop = asyncio.Event()
    task = asyncio.ensure_future(session.run(stop))
    await vm.handshake()
    await vm.accept_control_stream()
    register = await vm.next_message()
    assert not session.registered.is_set()
    await vm.send_message({"type": "res", "id": register["id"], "ok": True})
    await session.registered.wait()
    return session, vm, stop, task


async def bounded_scenario(scenario):
    await asyncio.wait_for(scenario, 5)


@pytest.mark.parametrize("rejection", [{"error": "unauthorized"}, {"ok": False}])
def test_register_rejection_never_reports_ready(rejection):
    async def scenario():
        session, vm = make_session(lambda *args: {"ok": True}, [])
        task = asyncio.ensure_future(session.run(asyncio.Event()))
        await vm.handshake()
        await vm.accept_control_stream()
        register = await vm.next_message()
        await vm.send_message({"type": "res", "id": register["id"], **rejection})
        assert await task is Outcome.FORBIDDEN
        assert not session.registered.is_set()
        assert session.registered_at is None

    asyncio.run(bounded_scenario(scenario()))


async def read_upload(vm):
    request = await vm.next_frame()
    assert (request.value.verb, request.value.path, request.value.end_body) == (
        "POST", "/chat/stream", False)
    body = bytearray(request.value.body)
    chunks = 0
    while True:
        frame = await vm.next_frame()
        assert frame.kind == "body_chunk" and frame.stream_id == request.stream_id
        assert len(frame.value.data) <= 16384
        body += frame.value.data
        chunks += 1
        if frame.value.end_body:
            return request, json.loads(body), chunks


def test_largest_image_uploads_in_chunks_as_an_image_item_after_its_text():
    async def scenario():
        session, vm, stop, task = await registered_session()
        picture = b"\x89PNG\r\n\x1a\n" + bytes(MAX_IMAGE_BYTES - 8)
        reply = asyncio.ensure_future(session.send_image(
            picture, "image/png", "porch-chat", message="who is it?", filename="porch.png"))
        request, uploaded, chunks = await read_upload(vm)
        assert chunks > 100
        text, image = uploaded.pop("items")
        assert uploaded == {
            "message": "who is it?", "output_modality": "text", "device_id": "homelink-abcdef",
            "session_id": "porch-chat",
        }
        assert text == {"type": "text", "text": "who is it?"}
        assert {key: image[key] for key in ("type", "mime_type", "filename")} == {
            "type": "image", "mime_type": "image/png", "filename": "porch.png"}
        assert base64.b64decode(image["data_base64"]) == picture
        await vm.send_frame(ServiceFrame.response(request.stream_id, ApplicationResponse(
            status=200, body=b'{"result":{"message_id":"user-1"}}', end_body=True,
        )))
        assert (await reply)["ok"]
        stop.set()
        await task

    asyncio.run(asyncio.wait_for(scenario(), 30))


def test_send_image_refuses_oversized_or_non_image_input_before_sending():
    async def scenario():
        session, vm, stop, task = await registered_session()
        with pytest.raises(ValueError, match="images must be 2 MiB or less"):
            await session.send_image(bytes(MAX_IMAGE_BYTES + 1), "image/jpeg", message="hi")
        with pytest.raises(ValueError, match="JPEG, PNG, GIF or WebP"):
            await session.send_image(b"%PDF-1.7", "application/pdf", message="hi")
        with pytest.raises(ValueError, match="empty"):
            await session.send_image(b"", "image/jpeg", message="hi")
        # Nothing went out: the next request on the session is this one.
        reply = asyncio.ensure_future(session.send_image(b"GIF89a", "image/gif"))
        request = await vm.next_frame()
        uploaded = json.loads(request.value.body)
        assert [item["type"] for item in uploaded["items"]] == ["image"]
        assert "filename" not in uploaded["items"][0]
        await vm.send_frame(ServiceFrame.response(request.stream_id, ApplicationResponse(
            status=200, body=b"{}", end_body=True)))
        assert (await reply)["ok"]
        stop.set()
        await task

    asyncio.run(bounded_scenario(scenario()))


def test_check_image_accepts_exactly_the_limit_and_supported_types():
    for mime_type in ("image/jpeg", "image/png", "image/gif", "image/webp"):
        check_image(bytes(MAX_IMAGE_BYTES), mime_type)
    for mime_type in ("image/heic", "image/svg+xml", "text/plain", ""):
        with pytest.raises(ValueError):
            check_image(b"x", mime_type)


def test_twenty_second_voice_note_uploads_in_bounded_chunks():
    async def scenario():
        session, vm, stop, task = await registered_session()
        recording = io.BytesIO()
        with wave.open(recording, "wb") as wav:
            wav.setnchannels(1)
            wav.setsampwidth(2)
            wav.setframerate(16000)
            wav.writeframes(b"\x01\x00" * 320000)
        audio = recording.getvalue()
        reply = asyncio.ensure_future(session.send_voice(audio, "robot-chat"))
        request = await vm.next_frame()
        assert (request.value.verb, request.value.path, request.value.end_body) == (
            "POST", "/chat/stream", False)
        body = bytearray(request.value.body)
        chunks = 0
        while True:
            frame = await vm.next_frame()
            assert frame.kind == "body_chunk"
            assert frame.stream_id == request.stream_id
            assert len(frame.value.data) <= 16384
            body += frame.value.data
            chunks += 1
            if frame.value.end_body:
                break
        assert chunks > 40
        uploaded = json.loads(body)
        assert {key: uploaded[key] for key in ("message", "device_id", "session_id")} == {
            "message": "", "device_id": "homelink-abcdef",
            "session_id": "robot-chat",
        }
        assert "output_modality" not in uploaded
        item, = uploaded["items"]
        assert (item["type"], item["mime_type"], item["filename"]) == (
            "file", "audio/wav", "voice_note.wav")
        assert base64.b64decode(item["data_base64"]) == audio
        await vm.send_frame(ServiceFrame.response(request.stream_id, ApplicationResponse(
            status=200, body=b'{"result":{"message_id":"user-1"}}', end_body=True,
        )))
        assert await reply == {"ok": True, "status": 200, "response": {"result": {"message_id": "user-1"}}}
        stop.set()
        assert await task is Outcome.STOPPED

    asyncio.run(bounded_scenario(scenario()))


@pytest.mark.parametrize("message, items", [
    ("what did I just say?", [{"type": "text", "text": "what did I just say?"}]),
    ("", []),
    ("  \n", []),
], ids=["with_message", "without_message", "blank_message"])
def test_voice_note_sends_its_message_as_a_text_item_before_the_audio(message, items):
    async def scenario():
        session, vm, stop, task = await registered_session()
        reply = asyncio.ensure_future(session.send_voice(b"WAV", message=message))
        request = await vm.next_frame()
        uploaded = json.loads(request.value.body)
        assert uploaded["message"] == message
        assert uploaded["items"] == items + [
            {"type": "file", "mime_type": "audio/wav", "filename": "voice_note.wav",
             "data_base64": "V0FW"}]
        await vm.send_frame(ServiceFrame.response(request.stream_id, ApplicationResponse(
            status=200, body=b"{}", end_body=True)))
        assert (await reply)["ok"]
        stop.set()
        await task

    asyncio.run(bounded_scenario(scenario()))


@pytest.mark.parametrize("send, body", [
    (lambda session: session.send_chat("Hello", output_modality="voice"),
     {"message": "Hello", "device_id": "homelink-abcdef", "output_modality": "voice"}),
    (lambda session: session.send_voice(
        b"WAV", "existing-chat", message="Use concise robot replies.", output_modality="voice"),
     {"message": "Use concise robot replies.", "device_id": "homelink-abcdef",
      "session_id": "existing-chat", "output_modality": "voice",
      "items": [{"type": "text", "text": "Use concise robot replies."},
                {"type": "file", "mime_type": "audio/wav", "filename": "voice_note.wav",
                 "data_base64": "V0FW"}]}),
], ids=["send_chat", "send_voice"])
def test_request_can_ask_for_a_spoken_reply(send, body):
    async def scenario():
        session, vm, stop, task = await registered_session()
        reply = asyncio.ensure_future(send(session))
        request = await vm.next_frame()
        assert json.loads(request.value.body) == body
        await vm.send_frame(ServiceFrame.response(request.stream_id, ApplicationResponse(
            status=200, body=b"{}", end_body=True,
        )))
        assert (await reply)["ok"] is True
        stop.set()
        await task

    asyncio.run(bounded_scenario(scenario()))


def test_subscription_ready_on_headers_and_parses_split_utf8_events():
    async def scenario():
        session, vm, stop, task = await registered_session()
        subscription = session.subscribe_chat("robot-chat")
        first = asyncio.ensure_future(subscription.__anext__())
        request = await vm.next_frame()
        assert request.value.path == "/chat/subscribe"
        assert json.loads(request.value.body) == {"session_id": "robot-chat"}
        assert not session.chat_subscribed.is_set()
        await vm.send_frame(ServiceFrame.response(request.stream_id, ApplicationResponse(
            status=200, headers=[Header("Content-Type", "application/x-ndjson")], end_body=False,
        )))
        await session.chat_subscribed.wait()
        event = {"type": "event", "seq": 1, "event": "delta.text_append", "payload": {"text": "café"}}
        encoded = json.dumps(event, ensure_ascii=False).encode()
        split = encoded.index(b"\xc3") + 1
        for chunk in (b'{"type":"ack"}\n\nmalformed\n' + encoded[:split], encoded[split:] + b"\n"):
            await vm.send_frame(ServiceFrame.body_chunk(request.stream_id, BodyChunk(data=chunk)))
        assert await first == event
        final = asyncio.ensure_future(subscription.__anext__())
        await vm.send_frame(ServiceFrame.body_chunk(request.stream_id, BodyChunk(
            data=b'{"type":"event","seq":2,"event":"delta.message_done","payload":{"message_id":"reply-1"}}',
            end_body=True,
        )))
        assert await final == {"type": "event", "seq": 2, "event": "delta.message_done",
                              "payload": {"message_id": "reply-1"}}
        with pytest.raises(StopAsyncIteration):
            await subscription.__anext__()
        assert not session.chat_subscribed.is_set()
        stop.set()
        await task

    asyncio.run(bounded_scenario(scenario()))


def test_tts_yields_before_the_response_finishes_and_escapes_message_id():
    async def scenario():
        session, vm, stop, task = await registered_session()
        speech = session.stream_tts("reply 1&x")
        first = asyncio.ensure_future(speech.__anext__())
        request = await vm.next_frame()
        assert request.value.path == "/api/voice/tts-stream?message_id=reply%201%26x"
        assert {h.key.lower(): h.value for h in request.value.headers}["accept"] == "audio/mpeg"
        await vm.send_frame(ServiceFrame.response(request.stream_id, ApplicationResponse(
            status=200, body=b"first MP3 chunk", end_body=False,
        )))
        assert await first == b"first MP3 chunk"
        second = asyncio.ensure_future(speech.__anext__())
        await vm.send_frame(ServiceFrame.body_chunk(request.stream_id, BodyChunk(
            data=b"last MP3 chunk", end_body=True,
        )))
        assert await second == b"last MP3 chunk"
        with pytest.raises(StopAsyncIteration):
            await speech.__anext__()
        stop.set()
        await task

    asyncio.run(bounded_scenario(scenario()))


def test_tts_falls_back_after_404_and_cancels_the_unfinished_response():
    async def scenario():
        session, vm, stop, task = await registered_session()
        speech = session.stream_tts("reply-1")
        first = asyncio.ensure_future(speech.__anext__())
        primary = await vm.next_frame()
        await vm.send_frame(ServiceFrame.response(primary.stream_id, ApplicationResponse(
            status=404, body=b"missing", end_body=False,
        )))
        reset = await vm.next_frame()
        assert (reset.kind, reset.stream_id) == ("reset", primary.stream_id)
        fallback = await vm.next_frame()
        assert fallback.value.path == "/voice/tts-stream?message_id=reply-1"
        await vm.send_frame(ServiceFrame.response(fallback.stream_id, ApplicationResponse(
            status=200, body=b"MP3", end_body=True,
        )))
        assert await first == b"MP3"
        await speech.aclose()
        assert not session._streams
        stop.set()
        await task

    asyncio.run(bounded_scenario(scenario()))


def test_tts_http_error_has_status_without_exposing_response_body():
    async def scenario():
        session, vm, stop, task = await registered_session()
        speech = session.stream_tts("reply-1")
        first = asyncio.ensure_future(speech.__anext__())
        request = await vm.next_frame()
        await vm.send_frame(ServiceFrame.response(request.stream_id, ApplicationResponse(
            status=403, body=b"private server response", end_body=True,
        )))
        with pytest.raises(HttpStreamError) as error:
            await first
        assert error.value.status == 403
        assert "private" not in str(error.value)
        assert not session._streams
        stop.set()
        await task

    asyncio.run(bounded_scenario(scenario()))


@pytest.mark.parametrize("status, exists", [(200, True), (404, False)])
def test_opening_a_side_chat_reports_whether_muse_has_it(status, exists):
    async def scenario():
        session, vm, stop, task = await registered_session()
        lookup = asyncio.ensure_future(session.open_side_chat("robot-chat"))
        request = await vm.next_frame()
        assert (request.value.verb, request.value.path, request.value.end_body) == (
            "POST", "/chat/subscribe", True)
        assert json.loads(request.value.body) == {"session_id": "robot-chat"}
        await vm.send_frame(ServiceFrame.response(request.stream_id, ApplicationResponse(
            status=status, end_body=False,
        )))
        assert await lookup == SideChat("robot-chat", exists=exists)
        reset = await vm.next_frame()
        assert (reset.kind, reset.stream_id) == ("reset", request.stream_id)
        assert not session.chat_subscribed.is_set()
        stop.set()
        await task

    asyncio.run(bounded_scenario(scenario()))


def test_a_side_chat_muse_refuses_raises_its_status():
    async def scenario():
        session, vm, stop, task = await registered_session()
        lookup = asyncio.ensure_future(session.open_side_chat("robot-chat"))
        request = await vm.next_frame()
        await vm.send_frame(ServiceFrame.response(request.stream_id, ApplicationResponse(
            status=403, body=b"private server response", end_body=True,
        )))
        with pytest.raises(HttpStreamError) as error:
            await lookup
        assert (error.value.status, error.value.path) == (403, "/chat/subscribe")
        assert str(error.value) == "Muse request /chat/subscribe failed: HTTP 403"
        stop.set()
        await task

    asyncio.run(bounded_scenario(scenario()))


def test_disconnect_wakes_a_waiting_tts_consumer():
    async def scenario():
        session, vm, stop, task = await registered_session()
        speech = session.stream_tts("reply-1")
        first = asyncio.ensure_future(speech.__anext__())
        request = await vm.next_frame()
        await vm.send_frame(ServiceFrame.response(request.stream_id, ApplicationResponse(status=200)))
        await vm.ws.close()
        assert await task is Outcome.CLOSED
        with pytest.raises(ConnectionError, match="session ended"):
            await first
        assert not session._streams
        assert not session.registered.is_set()

    asyncio.run(bounded_scenario(scenario()))


def test_stream_buffer_limit_keeps_control_commands_responsive():
    async def scenario():
        session, vm, stop, task = await registered_session()
        opened = asyncio.Event()
        consume = asyncio.Event()

        async def slow_reader():
            async with session.stream_http("GET", "/audio") as stream:
                assert stream.status == 200
                assert [(h.key, h.value) for h in stream.headers] == [("Content-Type", "audio/mpeg")]
                opened.set()
                await consume.wait()
                return [chunk async for chunk in stream]

        reader = asyncio.ensure_future(slow_reader())
        request = await vm.next_frame()
        await vm.send_frame(ServiceFrame.response(request.stream_id, ApplicationResponse(
            status=200, headers=[Header("Content-Type", "audio/mpeg")], end_body=False,
        )))
        await opened.wait()
        for _ in range(17):
            await vm.send_frame(ServiceFrame.body_chunk(request.stream_id, BodyChunk(data=b"x" * 65536)))
        reset = await vm.next_frame()
        assert (reset.kind, reset.stream_id) == ("reset", request.stream_id)
        await vm.send_message({"method": "link.invoke", "id": "alive", "command": "device.health"})
        assert await vm.next_message() == {"method": "link.result", "id": "alive", "ok": True}
        consume.set()
        with pytest.raises(BufferError, match="consumer fell behind"):
            await reader
        stop.set()
        await task

    asyncio.run(bounded_scenario(scenario()))


def test_tts_buffers_a_burst_above_one_megabyte_in_order_for_a_paced_consumer():
    async def scenario():
        session, vm, stop, task = await registered_session()
        expected = b"".join(bytes([index]) * 65536 for index in range(40))

        async def paced_reader():
            received = bytearray()
            async for chunk in session.stream_tts("long-reply"):
                assert len(chunk) <= 16384
                received.extend(chunk)
                await asyncio.sleep(0.001)
            return received

        reader = asyncio.create_task(paced_reader())
        request = await vm.next_frame()
        await vm.send_frame(ServiceFrame.response(request.stream_id, ApplicationResponse(status=200)))
        for offset in range(0, len(expected), 65536):
            await vm.send_frame(ServiceFrame.body_chunk(request.stream_id, BodyChunk(
                data=expected[offset:offset + 65536], end_body=offset + 65536 == len(expected),
            )))
        assert await reader == expected
        stop.set()
        await task

    asyncio.run(bounded_scenario(scenario()))


def test_oversized_chat_event_is_bounded_and_cancels_subscription():
    async def scenario():
        session, vm, stop, task = await registered_session()
        subscription = session.subscribe_chat()
        first = asyncio.ensure_future(subscription.__anext__())
        request = await vm.next_frame()
        await vm.send_frame(ServiceFrame.response(request.stream_id, ApplicationResponse(status=200)))
        oversized = b'{"type":"event","payload":"' + b"x" * (256 * 1024) + b'"}\n'
        for offset in range(0, len(oversized), 32768):
            await vm.send_frame(ServiceFrame.body_chunk(request.stream_id, BodyChunk(
                data=oversized[offset:offset + 32768],
            )))
        with pytest.raises(ValueError, match="event exceeds"):
            await first
        reset = await vm.next_frame()
        assert (reset.kind, reset.stream_id) == ("reset", request.stream_id)
        assert not session.chat_subscribed.is_set()
        stop.set()
        await task

    asyncio.run(bounded_scenario(scenario()))


def test_cancelled_multiframe_send_preserves_noise_nonces_for_next_request():
    async def scenario():
        session, vm, stop, task = await registered_session()
        first_frame_sent = asyncio.Event()
        continue_sending = asyncio.Event()
        original_send = session._ws.send
        count = 0

        async def gated_send(data):
            nonlocal count
            await original_send(data)
            count += 1
            if count == 1:
                first_frame_sent.set()
                await continue_sending.wait()

        session._ws.send = gated_send

        async def upload():
            async with session.stream_http("POST", "/upload", b"x" * 180000):
                pytest.fail("cancelled upload reached its response")

        uploading = asyncio.ensure_future(upload())
        await first_frame_sent.wait()
        for _ in range(2):
            uploading.cancel()
            await asyncio.sleep(0)
            assert not uploading.done()
        continue_sending.set()
        with pytest.raises(asyncio.CancelledError):
            await uploading
        complete_request = await vm.next_frame()
        assert (complete_request.value.path, len(complete_request.value.body)) == ("/upload", 180000)
        reset = await vm.next_frame()
        assert (reset.kind, reset.stream_id) == ("reset", complete_request.stream_id)
        next_reply = asyncio.ensure_future(session.send_chat("next turn"))
        next_request = await vm.next_frame()
        assert json.loads(next_request.value.body)["message"] == "next turn"
        await vm.send_frame(ServiceFrame.response(next_request.stream_id, ApplicationResponse(
            status=200, body=b"{}", end_body=True,
        )))
        assert (await next_reply)["ok"] is True
        stop.set()
        await task

    asyncio.run(bounded_scenario(scenario()))


@pytest.mark.parametrize("worker_raises", [False, True])
def test_cancelled_invoke_holds_slot_until_worker_finishes(worker_raises):
    import threading

    async def scenario():
        loop = asyncio.get_running_loop()
        started = asyncio.Event()
        waiting = asyncio.Event()
        release = threading.Event()
        sent = []
        calls = []

        class Slots(asyncio.Semaphore):
            async def acquire(self):
                if self.locked():
                    waiting.set()
                return await super().acquire()

        slots = Slots(1)

        def command(name, *_):
            calls.append(name)
            if name == "first":
                loop.call_soon_threadsafe(started.set)
                if not release.wait(5):
                    raise AssertionError("test worker was not released")
                if worker_raises:
                    raise RuntimeError("disconnected command failed")
            return {"ok": True}

        first, _ = make_session(command, [])
        second, _ = make_session(command, [])
        first._invokes = second._invokes = slots

        async def send(message):
            sent.append(message["id"])

        first.send = second.send = send
        old = asyncio.create_task(first._invoke({"id": "old", "command": "first"}))
        new = None
        try:
            await asyncio.wait_for(started.wait(), 2)
            old.cancel()
            with pytest.raises(asyncio.CancelledError):
                await old
            assert slots.locked(), "disconnect released a slot while its thread was still running"
            new = asyncio.create_task(second._invoke({"id": "new", "command": "second"}))
            await asyncio.wait_for(waiting.wait(), 2)
            assert calls == ["first"]
            release.set()
            await asyncio.wait_for(new, 2)
            assert calls == ["first", "second"]
            assert sent == ["new"]
            assert not slots.locked()
        finally:
            release.set()
            for task in (old, new):
                if task is not None:
                    task.cancel()
            await asyncio.gather(*(t for t in (old, new) if t is not None), return_exceptions=True)

    asyncio.run(scenario())


def test_cancelling_queued_invoke_does_not_start_command_or_release_slot():
    async def scenario():
        calls = []
        session, _ = make_session(lambda *args: calls.append(args), [])
        slots = asyncio.Semaphore(0)
        session._invokes = slots
        task = asyncio.create_task(session._invoke({"id": "queued", "command": "unused"}))
        await asyncio.sleep(0)
        task.cancel()
        with pytest.raises(asyncio.CancelledError):
            await task
        assert calls == []
        assert slots.locked()

    asyncio.run(scenario())


def test_failed_executor_submission_releases_invoke_slot(monkeypatch):
    async def scenario():
        session, _ = make_session(lambda *args: {"ok": True}, [])
        session._invokes = asyncio.Semaphore(1)

        def fail(*args):
            raise RuntimeError("executor unavailable")

        monkeypatch.setattr(asyncio.get_running_loop(), "run_in_executor", fail)
        with pytest.raises(RuntimeError, match="executor unavailable"):
            await session._invoke({"id": "failed", "command": "unused"})
        assert not session._invokes.locked()

    asyncio.run(scenario())


def test_reconnect_waits_for_disconnected_commands_to_finish():
    import threading
    from musegadget.link_client import MAX_CONCURRENT_INVOKES

    async def scenario():
        loop = asyncio.get_running_loop()
        all_started = asyncio.Event()
        waiting = asyncio.Event()
        release = threading.Event()
        calls = []
        lock = threading.Lock()

        class Slots(asyncio.Semaphore):
            async def acquire(self):
                if self.locked():
                    waiting.set()
                return await super().acquire()

        slots = Slots(MAX_CONCURRENT_INVOKES)

        def command(name, *_):
            with lock:
                calls.append(name)
                if len(calls) == MAX_CONCURRENT_INVOKES:
                    loop.call_soon_threadsafe(all_started.set)
            if name == "old" and not release.wait(5):
                raise AssertionError("test workers were not released")
            return {"ok": True}

        tasks = []
        vms = []
        try:
            for cycle in range(2):
                session, vm = make_session(command, [], invoke_slots=slots)
                vms.append(vm)
                task = asyncio.create_task(session.run(asyncio.Event()))
                tasks.append(task)
                await asyncio.wait_for(vm.handshake(), 2)
                await asyncio.wait_for(vm.accept_control_stream(), 2)
                register = await asyncio.wait_for(vm.next_message(), 2)
                await vm.send_message({"type": "res", "id": register["id"], "ok": True})
                if cycle == 0:
                    for index in range(MAX_CONCURRENT_INVOKES):
                        await vm.send_message({"method": "link.invoke", "id": str(index), "command": "old"})
                    await asyncio.wait_for(all_started.wait(), 2)
                    await vm.ws.close()
                    assert await asyncio.wait_for(task, 2) is Outcome.CLOSED
                else:
                    await vm.send_message({"method": "link.invoke", "id": "new", "command": "new"})
                    await asyncio.wait_for(waiting.wait(), 2)
                    assert calls == ["old"] * MAX_CONCURRENT_INVOKES
                    release.set()
                    result = await asyncio.wait_for(vm.next_message(), 2)
                    assert result == {"method": "link.result", "id": "new", "ok": True}
                    assert calls == ["old"] * MAX_CONCURRENT_INVOKES + ["new"]
        finally:
            release.set()
            for task in tasks:
                task.cancel()
            await asyncio.gather(*tasks, return_exceptions=True)

    asyncio.run(scenario())


@pytest.mark.parametrize("failure", ["command", "send"])
def test_invoke_failure_releases_slot_for_later_commands(failure):
    async def scenario():
        def command(*args):
            if failure == "command":
                raise RuntimeError("command failed")
            return {"ok": True}

        slots = asyncio.Semaphore(1)
        session, _ = make_session(command, [], invoke_slots=slots)

        sent = []

        async def send(message):
            sent.append(message)
            raise ConnectionError("send failed")

        session.send = send
        # A command failure becomes a correlated result, so both cases reach send.
        with pytest.raises(ConnectionError, match="send failed"):
            await session._invoke({"id": "failed", "command": "test"})
        result = {"ok": False, "error": "command failed"} if failure == "command" else {"ok": True}
        assert sent == [{"method": "link.result", "id": "failed", **result}]
        await asyncio.wait_for(slots.acquire(), 1)
        assert slots.locked()
        slots.release()

    asyncio.run(scenario())


def test_worker_completion_and_waiter_cancellation_release_slot_once(monkeypatch):
    async def scenario():
        loop = asyncio.get_running_loop()
        worker = loop.create_future()
        submitted = asyncio.Event()
        slots = asyncio.Semaphore(1)
        session, _ = make_session(lambda *args: {"ok": True}, [], invoke_slots=slots)
        sent = []

        def submit(*args):
            submitted.set()
            return worker

        async def send(message):
            sent.append(message)

        monkeypatch.setattr(loop, "run_in_executor", submit)
        session.send = send
        task = asyncio.create_task(session._invoke({"id": "race", "command": "test"}))
        await asyncio.wait_for(submitted.wait(), 1)
        worker.set_result({"ok": True})
        task.cancel()
        with pytest.raises(asyncio.CancelledError):
            await task
        assert sent == []
        await asyncio.wait_for(slots.acquire(), 1)
        assert slots.locked(), "the same slot was released twice"
        slots.release()

    asyncio.run(scenario())


def test_acknowledgement_returns_what_muse_acknowledged():
    assert acknowledgement({"ok": True, "status": 200, "response": {"message_id": "m-1"}}) == {
        "message_id": "m-1"}
    assert acknowledgement({"ok": True, "status": 200, "response": {"result": {"message_id": "m-1"}}}) == {
        "result": {"message_id": "m-1"}}


def test_acknowledgement_raises_for_a_refused_request():
    with pytest.raises(RequestRejected) as rejected:
        acknowledgement({"ok": False, "status": 429, "response": {"error": "busy"}})
    assert (rejected.value.status, str(rejected.value)) == (
        429, "Muse rejected conversation request: HTTP 429")
    assert isinstance(rejected.value, ConnectionError)


@pytest.mark.parametrize("response", [None, "accepted", []])
def test_acknowledgement_requires_a_response_object(response):
    with pytest.raises(ValueError, match="^Muse did not acknowledge the request$"):
        acknowledgement({"ok": True, "status": 200, "response": response})
