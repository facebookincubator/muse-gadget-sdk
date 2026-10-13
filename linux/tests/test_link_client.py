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
import struct

import pytest

from musegadget.link_client import (
    DeviceDescription, LinkSession, MessageDecoder, Outcome, _ChatSubscription, describe_result,
    encode_message, noise_url, printable,
)
from musegadget.noise import (
    ApplicationResponse, BodyChunk, NoiseFrameDecoder, NoiseXXResponder, ServiceFrame,
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


def make_session(run_command, connect_log: list):
    to_device, to_vm = asyncio.Queue(), asyncio.Queue()
    device_ws, vm_ws = Pipe(to_device, to_vm), Pipe(to_vm, to_device)

    async def connect(url, headers):
        connect_log.append((url, headers))
        return device_ws

    session = LinkSession(
        noise_host="gw.example", vm_id="vm 1&x", vm_auth_token="tok",
        device=DEVICE, run_command=run_command, connect=connect,
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


def test_send_chat_posts_a_message_on_the_authenticated_session():
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
def test_ask_chat_subscribes_and_returns_only_the_related_assistant_reply():
    async def scenario():
        session, vm = make_session(lambda *a: {"ok": True}, [])
        task = asyncio.ensure_future(session.run(asyncio.Event()))
        await vm.handshake()
        await vm.accept_control_stream()
        await vm.next_message()

        ask = asyncio.ensure_future(session.ask_chat("How are you?", "side-1"))
        subscription = await vm.next_frame()
        assert (subscription.kind, subscription.value.verb, subscription.value.path) == (
            "request", "POST", "/chat/subscribe")
        assert subscription.value.body == b"{}"
        headers = {h.key.lower(): h.value for h in subscription.value.headers}
        assert headers["accept"] == "application/x-ndjson"

        message = await vm.next_frame()
        assert (message.kind, message.value.verb, message.value.path) == (
            "request", "POST", "/chat/stream")
        assert json.loads(message.value.body)["message"] == "How are you?"
        await vm.send_frame(ServiceFrame.response(
            subscription.stream_id, ApplicationResponse(status=200),
        ))
        await vm.send_frame(ServiceFrame.response(
            message.stream_id,
            ApplicationResponse(
                status=200,
                body=b'{"result":{"message_id":"note-1","reply_to_message_id":"parent"}}',
                end_body=True,
            ),
        ))
        events = (
            b'{"type":"event","event":"delta.text_append","message_id":"reply-1",'
            b'"parent_message_id":"note-1","payload":{"text":"Hello "}}\n'
            b'{"type":"event","event":"delta.text_append","message_id":"unrelated",'
            b'"parent_message_id":"other","payload":{"text":"Ignored"}}\n'
            b'{"type":"event","event":"delta.text_append","message_id":"reply-1",'
            b'"parent_message_id":"reply-1","payload":{"text":"there"}}\n'
            b'{"type":"event","event":"delta.message_done","message_id":"reply-1",'
            b'"parent_message_id":"note-1","payload":{}}\n'
        )
        await vm.send_frame(ServiceFrame.body_chunk(
            subscription.stream_id, BodyChunk(data=events),
        ))
        assert await asyncio.wait_for(ask, 2) == "Hello there"
        task.cancel()

    asyncio.run(scenario())


def test_ask_chat_rejects_message_ids_with_an_unrelated_first_parent():
    async def scenario():
        session, vm = make_session(lambda *a: {"ok": True}, [])
        task = asyncio.ensure_future(session.run(asyncio.Event()))
        await vm.handshake()
        await vm.accept_control_stream()
        await vm.next_message()

        ask = asyncio.ensure_future(session.ask_chat("How are you?", "side-1"))
        subscription = await vm.next_frame()
        message = await vm.next_frame()
        await vm.send_frame(ServiceFrame.response(
            subscription.stream_id, ApplicationResponse(status=200),
        ))
        await vm.send_frame(ServiceFrame.response(
            message.stream_id,
            ApplicationResponse(
                status=200,
                body=b'{"result":{"message_id":"note-1","reply_to_message_id":"parent"}}',
                end_body=True,
            ),
        ))
        events = (
            b'{"type":"event","event":"delta.text_append","message_id":"wrong",'
            b'"parent_message_id":"other-chat","payload":{"text":"Wrong "}}\n'
            b'{"type":"event","event":"delta.text_append","message_id":"wrong",'
            b'"payload":{"text":"answer"}}\n'
            b'{"type":"event","event":"delta.message_done","message_id":"wrong",'
            b'"parent_message_id":"wrong","payload":{}}\n'
            b'{"type":"event","event":"delta.text_append","message_id":"right",'
            b'"parent_message_id":"note-1","payload":{"text":"Right answer"}}\n'
            b'{"type":"event","event":"delta.message_done","message_id":"right",'
            b'"parent_message_id":"right","payload":{}}\n'
        )
        await vm.send_frame(ServiceFrame.body_chunk(
            subscription.stream_id, BodyChunk(data=events),
        ))
        assert await asyncio.wait_for(ask, 2) == "Right answer"
        task.cancel()

    asyncio.run(scenario())


def test_chat_subscription_caps_total_buffered_event_bytes():
    async def scenario():
        subscription = _ChatSubscription()
        line = (
            b'{"type":"event","event":"delta.text_append","message_id":"reply",'
            b'"payload":{"text":"' + b"x" * (700 * 1024) + b'"}}\n'
        )
        assert len(line) < 1024 * 1024
        subscription.on_frame(ServiceFrame.body_chunk(
            1, BodyChunk(data=line + line + line),
        ))
        error = await subscription.next_event()
        assert isinstance(error, ValueError)
        assert "buffer is full" in str(error)
        assert subscription._queued_bytes == 0

    asyncio.run(scenario())


def test_ask_chat_fails_promptly_when_the_live_session_ends():
    async def scenario():
        session, vm = make_session(lambda *a: {"ok": True}, [])
        task = asyncio.ensure_future(session.run(asyncio.Event()))
        await vm.handshake()
        await vm.accept_control_stream()
        await vm.next_message()

        ask = asyncio.ensure_future(session.ask_chat("Tell me something.", "side-1"))
        subscription = await vm.next_frame()
        message = await vm.next_frame()
        await vm.send_frame(ServiceFrame.response(
            subscription.stream_id, ApplicationResponse(status=200),
        ))
        await vm.send_frame(ServiceFrame.response(
            message.stream_id,
            ApplicationResponse(
                status=200,
                body=b'{"result":{"message_id":"note-1"}}',
                end_body=True,
            ),
        ))
        await asyncio.sleep(0)
        await vm.ws.send(None)

        assert await asyncio.wait_for(task, 2) is Outcome.CLOSED
        with pytest.raises(ConnectionError, match="subscription failed"):
            await asyncio.wait_for(ask, 2)

    asyncio.run(scenario())
