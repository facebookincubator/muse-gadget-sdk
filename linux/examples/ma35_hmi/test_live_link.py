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

from live_link import (
    DeviceDescription, LinkSession, MessageDecoder, Outcome, ReplyCollector, describe_result,
    encode_message, noise_url, parse_chat_event, printable,
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
        self.subscriptions: asyncio.Queue = asyncio.Queue()
        self.stash: list[ServiceFrame] = []

    async def handshake(self) -> None:
        responder = NoiseXXResponder()
        responder.initialize()
        await self.ws.send(responder.read_message1_and_write_message2(await self.ws.recv()))
        responder.read_message3(await self.ws.recv())
        self.send_cipher, self.recv_cipher = responder.split()

    async def _raw_frame(self) -> ServiceFrame:
        while True:
            plain = self.recv_cipher.decrypt_with_ad(b"", await self.ws.recv())
            assembled = self.decoder.decode(plain)
            if assembled is not None:
                return decode_request_envelope(assembled)

    async def next_frame(self) -> ServiceFrame:
        """Next frame other than the session's own /chat/subscribe requests."""
        while True:
            if self.stash:
                return self.stash.pop(0)
            frame = await self._raw_frame()
            if self._is_subscribe(frame):
                self.subscriptions.put_nowait(frame)
                continue
            return frame

    @staticmethod
    def _is_subscribe(frame: ServiceFrame) -> bool:
        return frame.kind == "request" and frame.value.path == "/chat/subscribe"

    async def next_subscription(self) -> ServiceFrame:
        """Next /chat/subscribe request, setting aside any other frame meanwhile."""
        while self.subscriptions.empty():
            frame = await self._raw_frame()
            if self._is_subscribe(frame):
                self.subscriptions.put_nowait(frame)
            else:
                self.stash.append(frame)
        return self.subscriptions.get_nowait()

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

    with caplog.at_level("INFO", logger="live_link"):
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


def _event(name, message_id, reply_to="", seq=1, **fields):
    return json.dumps({"type": "event", "seq": seq, "payload": {
        "event": name, "message_id": message_id, "reply_to_message_id": reply_to, **fields}}
    ).encode() + b"\n"


async def _start(vm_session):
    session, vm = vm_session
    task = asyncio.ensure_future(session.run(asyncio.Event()))
    await vm.handshake()
    await vm.accept_control_stream()
    await vm.next_message()  # link.register
    return session, vm, task


def test_session_subscribes_to_replies_once_with_an_empty_body():
    async def scenario():
        session, vm, task = await _start(make_session(lambda *a: {"ok": True}, []))
        sub = await asyncio.wait_for(vm.next_subscription(), 2)
        assert (sub.value.verb, sub.value.path, sub.value.body, sub.value.end_body) == (
            "POST", "/chat/subscribe", b"{}", True)
        headers = {h.key.lower(): h.value for h in sub.value.headers}
        assert headers["accept"] == "application/x-ndjson"
        assert headers["content-type"] == "application/json"
        task.cancel()

    asyncio.run(scenario())


def test_send_chat_wait_reply_returns_the_matching_answer_from_the_subscription():
    async def scenario():
        session, vm, task = await _start(make_session(lambda *a: {"ok": True}, []))
        sub = await asyncio.wait_for(vm.next_subscription(), 2)
        # First row is an acknowledgement, not an event.
        await vm.send_frame(ServiceFrame.response(sub.stream_id, ApplicationResponse(
            status=200, body=b'{"type":"ack"}\n', end_body=False)))

        reply = asyncio.ensure_future(session.send_chat("hi", wait_reply=5))
        note = await vm.next_frame()
        assert note.value.path == "/chat/stream"
        # Replayed history, then our note, then the answer in deltas (reply_to empty).
        await vm.send_frame(ServiceFrame.body_chunk(sub.stream_id, BodyChunk(
            data=_event("message.assistant", "m0", "", seq=1, text="stale"), end_body=False)))
        await vm.send_frame(ServiceFrame.body_chunk(sub.stream_id, BodyChunk(
            data=_event("message.user", "note-1", "", seq=2)
                 + _event("delta.text_append", "m1", "", seq=2, text="Hel"), end_body=False)))
        await vm.send_frame(ServiceFrame.response(
            note.stream_id,
            ApplicationResponse(status=200, body=b'{"message_id":"note-1"}', end_body=True)))
        await vm.send_frame(ServiceFrame.body_chunk(sub.stream_id, BodyChunk(
            data=_event("delta.text_append", "m1", "", seq=3, text="lo")
                 + _event("delta.text_append", "m1", "", seq=3, text="lo")  # repeated seq
                 + _event("delta.message_done", "m1", "", seq=4), end_body=False)))
        result = await asyncio.wait_for(reply, 2)
        assert result["reply"] == "Hello"
        assert result["response"] == {"message_id": "note-1"}
        task.cancel()

    asyncio.run(scenario())


def test_send_chat_wait_reply_in_a_side_chat_subscribes_with_its_id():
    async def scenario():
        session, vm, task = await _start(make_session(lambda *a: {"ok": True}, []))
        await asyncio.wait_for(vm.next_subscription(), 2)  # the main-chat one

        reply = asyncio.ensure_future(session.send_chat("hi", "side-1", wait_reply=5))
        side = await asyncio.wait_for(vm.next_subscription(), 2)
        assert json.loads(side.value.body) == {"session_id": "side-1"}
        note = await vm.next_frame()
        await vm.send_frame(ServiceFrame.response(
            note.stream_id, ApplicationResponse(status=200, body=b'{"message_id":"n"}', end_body=True)))
        await vm.send_frame(ServiceFrame.body_chunk(side.stream_id, BodyChunk(
            data=_event("message.user", "n", "") + _event("message.assistant", "m", "", text="done"),
            end_body=False)))
        assert (await asyncio.wait_for(reply, 2))["reply"] == "done"
        reset = await vm.next_frame()
        assert (reset.kind, reset.stream_id) == ("reset", side.stream_id)
        task.cancel()

    asyncio.run(scenario())


def test_send_chat_wait_reply_times_out_with_no_reply():
    async def scenario():
        session, vm, task = await _start(make_session(lambda *a: {"ok": True}, []))
        reply = asyncio.ensure_future(session.send_chat("hi", wait_reply=0.2))
        note = await vm.next_frame()
        await vm.send_frame(ServiceFrame.response(
            note.stream_id, ApplicationResponse(status=200, body=b'{"message_id":"n"}', end_body=True)))
        result = await asyncio.wait_for(reply, 2)
        assert result["ok"] and result["reply"] is None
        task.cancel()

    asyncio.run(scenario())


def test_a_refused_subscription_is_retried_without_hammering(monkeypatch):
    monkeypatch.setattr("live_link.SUBSCRIBE_RETRY_S", 0.05)

    async def scenario():
        session, vm, task = await _start(make_session(lambda *a: {"ok": True}, []))
        first = await asyncio.wait_for(vm.next_subscription(), 2)
        await vm.send_frame(ServiceFrame.response(
            first.stream_id, ApplicationResponse(status=502, end_body=True)))
        second = await asyncio.wait_for(vm.next_subscription(), 2)
        assert second.stream_id != first.stream_id
        task.cancel()

    asyncio.run(scenario())


def test_chat_events_are_flattened_from_the_payload():
    row = _event("message.assistant", "m", "n", display_text="shown", content="raw")
    assert parse_chat_event(row) == {
        "event": "message.assistant", "message_id": "m", "reply_to": "n", "text": "shown", "seq": 1}
    assert parse_chat_event(b"not json") is None
    assert parse_chat_event(b'{"type": "heartbeat"}') is None


def test_reply_collector_follows_the_note_not_reply_to_and_skips_replayed_history():
    collector = ReplyCollector()
    feed = lambda *a, **k: collector.feed(parse_chat_event(_event(*a, **k)))
    # History replayed on subscribe, and an event that arrives before the ack.
    assert feed("message.user", "old-note", "") is None
    assert feed("message.assistant", "old-answer", "", text="stale") is None
    assert feed("message.user", "note-1", "") is None
    assert collector.set_note_id("note-1") is None
    # Streamed answer: reply_to is empty, parent_message_id is the message itself.
    assert feed("delta.message_start", "a1", "", parent_message_id="a1") is None
    assert feed("delta.text_append", "a1", "", parent_message_id="a1", text="Reply ") is None
    assert feed("delta.text_append", "a1", "", parent_message_id="a1", text="works.") is None
    assert feed("delta.message_done", "a1", "", parent_message_id="a1") == "Reply works."


def test_reply_collector_ignores_an_answer_that_names_another_note():
    collector = ReplyCollector()
    collector.set_note_id("n1")
    assert collector.feed(parse_chat_event(_event("message.assistant", "x", "other", text="no"))) is None
    assert collector.feed(parse_chat_event(_event("message.assistant", "y", "n1", text="yes"))) == "yes"
