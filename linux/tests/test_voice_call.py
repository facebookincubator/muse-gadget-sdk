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

import asyncio
import base64
import json
import math
import struct
import time
import wave
from datetime import datetime
from pathlib import Path
from urllib.parse import parse_qs, urlsplit

import pytest

from musegadget.voice_audio import WavAudio
from musegadget.voice_call import FRAME_BYTES, MAX_AUDIO_BYTES, VoiceCall, audio_from_event
from test_streaming import chunk, connected, response

FIXTURE = json.loads((Path(__file__).parent / "vectors/realtime_voice.json").read_text())
TONE = b"".join(struct.pack("<h", int(2000 * math.sin(2 * math.pi * 440 * n / 24000)))
                for n in range(960))


def line(event):
    return json.dumps(event).encode() + b"\n"


def cavd(pcm=TONE):
    return {"type": "continuous_audio_video.delta", "audio_delta": {
        "audio_frames": base64.b64encode(pcm).decode(), "format": FIXTURE["audio_format"],
        "num_channels": 1, "num_frames": len(pcm) // 2}}


class Output:
    def __init__(self):
        self.pcm = bytearray()
        self.written = asyncio.Event()
        self.flushed = asyncio.Event()

    def write(self, data):
        self.pcm.extend(data)
        self.written.set()

    def flush(self):
        self.flushed.set()


async def source(*frames):
    for pcm in frames:
        yield pcm


async def admit(vm, *, extra=(), ready=True, wrong_id=False, device_id="homelink-synthetic"):
    request = await vm.next_frame()
    assert request.value.path.startswith("/voice/realtime?")
    assert not request.value.end_body
    frame = await vm.next_frame()
    assert frame.stream_id == request.stream_id and not frame.value.end_body
    start = json.loads(frame.value.data)
    assert start["device"] == "mcu-wearable"
    assert start["device_id"] == device_id
    assert parse_qs(urlsplit(request.value.path).query) == {"call_id": [start["call_id"]]}
    assert start["type"] == "hatch.voice.call.start"
    offer = await next_event(vm)
    assert offer == {"type": "hatch.voice.capability.offer", "end_call_stop": 1}
    records = [FIXTURE["events"][0]]
    if ready:
        records.append({"type": "hatch.voice.session",
                        "call_id": "wrong" if wrong_id else start["call_id"],
                        "session_id": FIXTURE["session_id"], "thread_id": FIXTURE["thread_id"]})
    records.extend(extra)
    raw = b"".join(map(line, records))
    # Fragment readiness and media at unrelated Noise/body boundaries.
    await response(vm, request.stream_id, raw[:7])
    for offset in range(7, len(raw), 97):
        await chunk(vm, request.stream_id, raw[offset:offset + 97])
    return request.stream_id, start


async def next_event(vm):
    frame = await vm.next_frame()
    assert frame.kind == "body_chunk" and not frame.value.end_body
    return json.loads(frame.value.data)


def assert_quiet_append(frame):
    assert frame.kind == "body_chunk" and not frame.value.end_body
    event = json.loads(frame.value.data)
    assert event["type"] == "input_audio_buffer.append"
    assert base64.b64decode(event["audio"], validate=True) == b"\0" * FRAME_BYTES


async def quiet_for(vm, seconds):
    deadline = asyncio.get_running_loop().time() + seconds
    while True:
        remaining = deadline - asyncio.get_running_loop().time()
        if remaining <= 0:
            return
        try:
            frame = await asyncio.wait_for(vm.next_frame(), remaining)
        except asyncio.TimeoutError:
            return
        assert_quiet_append(frame)


async def hangup(vm, media_id, start, *, status=200, media_ended=False, expect_thread=True):
    end = await vm.next_frame()
    while end.kind == "body_chunk":
        assert end.stream_id == media_id
        assert_quiet_append(end)  # EOF keeps a quiet microphone alive until hangup.
        end = await vm.next_frame()
    # The upload remains open until the explicit end-call acknowledgment.
    assert end.kind == "request" and end.value.path == "/voice/end-call"
    body = json.loads(end.value.body)
    assert body["call_id"] == start["call_id"]
    assert datetime.fromisoformat(body["call_ended_at"]) > datetime.fromisoformat(body["call_started_at"])
    if expect_thread:
        assert body["thread_id"] == FIXTURE["thread_id"]
    await response(vm, end.stream_id, b'{"ok":true,"result":{"ok":true}}', status=status, end=True)
    final = await vm.next_frame()
    if status != 200:
        assert final.kind == "reset"
        return
    assert final.stream_id == media_id and final.value.end_body
    if not media_ended:
        await chunk(vm, media_id, end=True)


def test_actual_noise_duplex_wav_audio_and_cancelled_response_is_not_call_end(tmp_path):
    input_path, output_path = tmp_path / "in.wav", tmp_path / "out.wav"
    with wave.open(str(input_path), "wb") as wav:
        wav.setparams((1, 2, 24000, 0, "NONE", "not compressed"))
        wav.writeframes(TONE * 2)

    async def scenario():
        async with connected(register_device=False) as (session, vm, unused):
            audio = WavAudio(input_path, output_path)
            call = VoiceCall(session, device_id="homelink-synthetic", half_duplex=False)
            task = asyncio.create_task(call.run(audio.frames(), audio))
            try:
                media, start = await admit(vm)
                first = await next_event(vm)
                assert first["type"] == "input_audio_buffer.append"
                assert base64.b64decode(first["audio"]) == TONE
                sent_at = time.monotonic()
                # Downlink precedes upload EOS and even the second append.
                events = [cavd(), *FIXTURE["events"][1:],
                          {"type": "response.output_audio.delta", "delta": base64.b64encode(TONE).decode()}]
                raw = b"".join(map(line, events))
                for offset in range(0, len(raw), 97):
                    await chunk(vm, media, raw[offset:offset + 97])
                assert (await next_event(vm))["type"] == "input_audio_buffer.append"
                assert time.monotonic() - sent_at >= 0.03
                assert not task.done()
                await chunk(vm, media, end=True)
                await hangup(vm, media, start, media_ended=True)
                assert await asyncio.wait_for(task, 2) == "remote_end"
                assert not session._requests
            finally:
                task.cancel()
                await asyncio.gather(task, return_exceptions=True)
                audio.close()
    asyncio.run(scenario())
    with wave.open(str(output_path), "rb") as wav:
        assert (wav.getnchannels(), wav.getsampwidth(), wav.getframerate()) == (1, 2, 24000)
        assert wav.readframes(wav.getnframes()) == TONE * 2


@pytest.mark.parametrize("failure", ["source", "cancel", "server", "identity", "truncated", "oversize"])
def test_hangup_on_errors_and_cancellation_preserves_original_failure(failure):
    async def broken_source():
        if failure == "source":
            raise RuntimeError("synthetic source failure")
        await asyncio.Future()
        yield TONE

    async def scenario():
        async with connected(register_device=False) as (session, vm, unused):
            output = Output()
            call = VoiceCall(session, device_id="homelink-synthetic", cleanup_timeout=0.5)
            task = asyncio.create_task(call.run(broken_source(), output))
            media, start = await admit(vm, wrong_id=failure == "identity")
            if failure == "cancel":
                await asyncio.wait_for(call.ready.wait(), 1)
                task.cancel()
            elif failure == "server":
                await chunk(vm, media, line({"type": "error", "error": {
                    "code": "session_dead", "message": "synthetic"}}))
            elif failure == "truncated":
                await chunk(vm, media, b'{"type":', end=True)
            elif failure == "oversize":
                await chunk(vm, media, b"x" * (1024 * 1024 + 1))
            # A teardown HTTP failure must not replace any of the above.
            await hangup(vm, media, start, status=503, expect_thread=failure != "identity")
            expected = {"source": RuntimeError, "cancel": asyncio.CancelledError,
                        "server": ConnectionError}.get(failure, ValueError)
            with pytest.raises(expected):
                await asyncio.wait_for(task, 2)
            assert call.cleanup_error is not None
            assert not session._requests
    asyncio.run(scenario())


def test_hold_gates_input_and_speech_start_flushes_playback():
    async def scenario():
        async with connected(register_device=False) as (session, vm, unused):
            output = Output()
            call = VoiceCall(session, device_id="homelink-synthetic")
            task = asyncio.create_task(call.run(source(TONE), output))
            media, start = await admit(vm, extra=[{"type": "hatch.voice.switch_input_hold", "held": True}])
            await asyncio.wait_for(call.ready.wait(), 1)
            with pytest.raises(asyncio.TimeoutError):
                await asyncio.wait_for(vm.next_frame(), 0.06)
            assert call.held
            await chunk(vm, media, line({"type": "hatch.voice.switch_input_hold", "held": False}))
            assert (await next_event(vm))["type"] == "input_audio_buffer.append"
            await chunk(vm, media, line(cavd(TONE * 8)))
            await asyncio.wait_for(output.written.wait(), 1)
            await chunk(vm, media, line({"type": "input_audio_buffer.speech_started"}))
            await asyncio.wait_for(output.flushed.wait(), 1)
            assert call._playback_bytes == 0 and call._audible_frames == 0
            await chunk(vm, media, line({"type": "conversation.item.added", "item": {
                "type": "session_control", "name": "stop", "call_id": start["call_id"], "arguments": "{}"}}))
            await hangup(vm, media, start)
            assert await asyncio.wait_for(task, 1) == "remote_stop"
            assert len(output.pcm) < len(TONE) * 8
    asyncio.run(scenario())


def test_silent_keepalives_neither_gate_mic_nor_extend_idle():
    async def scenario():
        async with connected(register_device=False) as (session, vm, unused):
            output = Output()
            call = VoiceCall(session, device_id="homelink-synthetic", idle_timeout=0.12)
            task = asyncio.create_task(call.run(source(TONE), output))
            silence = cavd(b"\0" * FRAME_BYTES)
            media, start = await admit(vm, extra=[silence] * 4)
            assert (await next_event(vm))["type"] == "input_audio_buffer.append"

            async def keepalive():
                while True:
                    await chunk(vm, media, line(silence))
                    await asyncio.sleep(0.02)

            pings = asyncio.create_task(keepalive())
            try:
                await asyncio.wait_for(hangup(vm, media, start), 1)
                assert await task == "idle_timeout"
                assert not output.pcm
            finally:
                pings.cancel()
                await asyncio.gather(pings, return_exceptions=True)
    asyncio.run(scenario())


def test_missing_readiness_times_out_without_audio_and_still_hangs_up():
    async def scenario():
        async with connected(register_device=False) as (session, vm, unused):
            call = VoiceCall(session, device_id="homelink-synthetic", ready_timeout=0.05)
            task = asyncio.create_task(call.run(source(TONE), Output()))
            media, start = await admit(vm, ready=False)
            await hangup(vm, media, start, expect_thread=False)
            with pytest.raises(asyncio.TimeoutError):
                await task
    asyncio.run(scenario())


def test_stream_fenced_stop_drains_audio_and_handles_control_once():
    async def scenario():
        async with connected(register_device=False) as (session, vm, unused):
            output = Output()
            call = VoiceCall(session, device_id="homelink-synthetic")
            task = asyncio.create_task(call.run(source(TONE), output))
            media, start = await admit(vm)
            assert (await next_event(vm))["type"] == "input_audio_buffer.append"
            stop = {"type": "conversation.item.added", "item": {
                "type": "session_control", "name": "stop", "call_id": "synthetic-control", "arguments": "{}"}}
            reply_audio = TONE + b"\0" * FRAME_BYTES + TONE
            await chunk(vm, media + 100, line(stop))  # unrelated stream cannot end this call
            await chunk(vm, media, line(cavd(reply_audio)))
            await asyncio.wait_for(output.written.wait(), 1)
            assert not task.done()
            assert stop["item"]["call_id"] != start["call_id"]
            await chunk(vm, media, line(stop) * 2)  # duplicate causes only one end-call
            await hangup(vm, media, start)
            assert await asyncio.wait_for(task, 1) == "remote_stop"
            assert output.pcm == reply_audio
    asyncio.run(scenario())


def test_overall_deadline_ends_an_active_call():
    async def forever():
        while True:
            yield TONE

    async def scenario():
        async with connected(register_device=False) as (session, vm, unused):
            call = VoiceCall(session, device_id="homelink-synthetic", overall_timeout=0.15)
            task = asyncio.create_task(call.run(forever(), Output()))
            media, start = await admit(vm)
            while True:
                frame = await vm.next_frame()
                if frame.kind == "request":
                    break
                assert json.loads(frame.value.data)["type"] == "input_audio_buffer.append"
            assert frame.value.path == "/voice/end-call"
            await response(vm, frame.stream_id, b'{"ok":true,"result":{"ok":true}}', end=True)
            assert (await vm.next_frame()).value.end_body
            await chunk(vm, media, end=True)
            assert await asyncio.wait_for(task, 1) == "overall_timeout"
    asyncio.run(scenario())


def test_voice_only_transport_preserves_running_service_for_same_identity():
    async def scenario():
        async with connected() as (service, control_vm, unused):
            executed = []

            def command(name, params, timeout):
                executed.append(name)
                return {"ok": True, "payload": {"service": "original"}}

            service._run_command = command

            async def invoke(index):
                await control_vm.send_message({"method": "link.invoke", "id": str(index),
                                               "command": "device.health", "params": {}})
                result = await asyncio.wait_for(control_vm.next_message(), 1)
                assert result["ok"] and result["payload"]["service"] == "original"

            await invoke(1)
            async with connected(register_device=False) as (voice_session, voice_vm, unused):
                assert voice_session._device.node_id == service._device.node_id
                assert voice_session.registered_at is None
                with pytest.raises(ValueError):
                    await voice_session.wait_registered()
                call = VoiceCall(voice_session, device_id=service._device.node_id)
                task = asyncio.create_task(call.run(source(TONE), Output()))
                # The very first request is voice admission, never link-control
                # or link.register (which would steal the service's live slot).
                media, start = await admit(voice_vm, device_id=service._device.node_id)
                await next_event(voice_vm)
                await next_event(voice_vm)
                await invoke(2)
                await chunk(voice_vm, media, end=True)
                await hangup(voice_vm, media, start, media_ended=True)
                assert await asyncio.wait_for(task, 1) == "remote_end"
            await invoke(3)  # same original connection after voice exit; no restore
            assert executed == ["device.health"] * 3
            assert service.registered_at is not None and not service._closed
    asyncio.run(scenario())


@pytest.mark.parametrize("stalled", [False, True])
def test_repeated_cancellation_during_source_finalizer_still_ends_call(stalled):
    async def scenario():
        finalizing, release = asyncio.Event(), asyncio.Event()

        async def delayed_source():
            try:
                yield TONE
                await asyncio.Future()
            finally:
                finalizing.set()
                await release.wait()

        async with connected(register_device=False) as (session, vm, unused):
            call = VoiceCall(session, device_id="homelink-synthetic", cleanup_timeout=0.1)
            task = asyncio.create_task(call.run(delayed_source(), Output()))
            media, start = await admit(vm)
            await next_event(vm)
            task.cancel()
            await asyncio.wait_for(finalizing.wait(), 1)
            task.cancel()  # while worker join is blocked, before end-call starts
            if not stalled:
                release.set()
            await asyncio.wait_for(hangup(vm, media, start), 2)
            with pytest.raises(asyncio.CancelledError):
                await asyncio.wait_for(task, 1)
            assert not session._requests
            if stalled:
                assert isinstance(call.cleanup_error, asyncio.TimeoutError)
            else:
                assert call.cleanup_error is None
    asyncio.run(scenario())


def test_wav_output_cannot_truncate_hardlinked_input(tmp_path):
    import os

    original = tmp_path / "input.wav"
    alias = tmp_path / "alias.wav"
    with wave.open(str(original), "wb") as wav:
        wav.setparams((1, 2, 24000, 0, "NONE", "not compressed"))
        wav.writeframes(TONE)
    os.link(original, alias)
    before = original.read_bytes()
    with pytest.raises(ValueError):
        WavAudio(original, alias)
    assert original.read_bytes() == before


@pytest.mark.parametrize("event", [
    {"type": "response.output_audio.delta", "delta": "@not base64"},
    {"type": "response.output_audio.delta", "delta": "AA=="},
    cavd(b"\0" * (MAX_AUDIO_BYTES + 2)),
    {"type": "continuous_audio_video.delta", "audio_delta": {
        "audio_frames": "AAAA", "format": {"type": "audio/pcm", "rate": 16000}}},
    {"type": "continuous_audio_video.delta", "audio_delta": {
        "audio_frames": "AAAA", "num_channels": 2}},
])
def test_malformed_or_oversized_audio_is_rejected_before_playback(event):
    with pytest.raises(ValueError):
        audio_from_event(event)
