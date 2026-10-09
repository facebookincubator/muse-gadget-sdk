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
    DeviceDescription, HttpStreamError, LinkSession, MessageDecoder, Outcome, RequestRejected, SideChat,
    acknowledgement, describe_result, encode_message, noise_url, printable,
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


@pytest.mark.parametrize("send, body", [
    (lambda session: session.send_chat("Hello", output_modality="voice"),
     {"message": "Hello", "device_id": "homelink-abcdef", "output_modality": "voice"}),
    (lambda session: session.send_voice(
        b"WAV", "existing-chat", message="Use concise robot replies.", output_modality="voice"),
     {"message": "Use concise robot replies.", "device_id": "homelink-abcdef",
      "session_id": "existing-chat", "output_modality": "voice",
      "items": [{"type": "file", "mime_type": "audio/wav", "filename": "voice_note.wav",
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
        assert request.value.path == "/chat/subscribe"
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


def test_acknowledgement_returns_what_muse_acknowledged():
    assert acknowledgement({"ok": True, "status": 200, "response": {"message_id": "m-1"}}) == {
        "message_id": "m-1"}


def test_acknowledgement_raises_for_a_refused_or_unacknowledged_request():
    with pytest.raises(RequestRejected) as rejected:
        acknowledgement({"ok": False, "status": 429, "response": {"error": "busy"}})
    assert (rejected.value.status, str(rejected.value)) == (
        429, "Muse rejected conversation request: HTTP 429")
    assert isinstance(rejected.value, ConnectionError)
    with pytest.raises(ValueError, match="^Muse did not acknowledge the request$"):
        acknowledgement({"ok": True, "status": 200, "response": "accepted"})
