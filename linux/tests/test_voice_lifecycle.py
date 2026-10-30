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
import time
from types import SimpleNamespace

import pytest

from musegadget import voice_call as voice
from test_streaming import chunk, connected, response
from test_voice_call import Output, TONE, admit, cavd, hangup, line, next_event, quiet_for, source


def test_operation_errors_keep_media_and_input_open(caplog):
    errors = [
        {"type": "server_error", "code": "voice_floor_queue_full"},
        {"type": "invalid_request_error", "code": "visual_context_unavailable"},
        {"type": "invalid_request_error", "code": "unsupported_session_update"},
        {"type": "invalid_request_error", "code": "invalid_visual_context"},
        {"type": "backend_error"},
        {"code": "future_operation_warning"},
    ]

    async def scenario():
        async with connected(register_device=False) as (session, vm, unused):
            output = Output()
            call = voice.VoiceCall(session, device_id="homelink-synthetic", half_duplex=False)
            task = asyncio.create_task(call.run(source(TONE, TONE), output))
            media, start = await admit(vm)
            assert (await next_event(vm))["type"] == "input_audio_buffer.append"
            await chunk(vm, media, b"".join(line({"type": "error", "error": {
                **error, "message": "synthetic-private-content"}}) for error in errors))
            assert (await next_event(vm))["type"] == "input_audio_buffer.append"
            await chunk(vm, media, line(cavd()), end=True)
            await hangup(vm, media, start, media_ended=True)
            assert await asyncio.wait_for(task, 1) == "remote_end"
            assert output.pcm == TONE
            assert call.advisory_error_count == len(errors)
    asyncio.run(scenario())
    assert "synthetic-private-content" not in caplog.text


@pytest.mark.parametrize("error", [
    {"type": "backend_unavailable"}, {"type": "invalid_request_error"},
])
def test_pre_readiness_errors_remain_fatal(error):
    async def scenario():
        async with connected(register_device=False) as (session, vm, unused):
            call = voice.VoiceCall(session, device_id="homelink-synthetic")
            task = asyncio.create_task(call.run(source(TONE), Output()))
            media, start = await admit(vm, ready=False, extra=[{
                "type": "error", "error": {**error, "message": "synthetic"}}])
            await hangup(vm, media, start, expect_thread=False)
            with pytest.raises(ConnectionError):
                await task
            assert call.advisory_error_count == 0
    asyncio.run(scenario())


async def wait_until(predicate):
    async def wait():
        while not predicate():
            await asyncio.sleep(0)
    await asyncio.wait_for(wait(), 1)


def test_silent_work_survives_idle_and_old_settlement_cannot_clear_new_work(monkeypatch):
    clock = [1000.0]
    monkeypatch.setattr(voice, "time", SimpleNamespace(monotonic=lambda: clock[0], time=time.time))

    async def scenario():
        async with connected(register_device=False) as (session, vm, unused):
            call = voice.VoiceCall(session, device_id="homelink-synthetic", idle_timeout=45,
                                   overall_timeout=300)
            task = asyncio.create_task(call.run(source(TONE), Output()))
            media, start = await admit(vm)
            await next_event(vm)
            await next_event(vm)

            async def started(message, item):
                await chunk(vm, media, line({"type": "hatch.voice.turn_started",
                    "submission_message_id": message, "item_id": item}))
                await wait_until(lambda: message in call._active_turns)

            async def settled(message):
                await chunk(vm, media, line({"type": "hatch.voice.turn_settled",
                                             "response_message_id": message}))

            await started("root-a", "input-a")
            clock[0] += 60  # silent Companion work, longer than the default 45s idle
            await quiet_for(vm, 0.15)
            assert not task.done()
            await started("root-b", "input-b")
            await started("alias-a", "input-a")
            await settled("root-a")  # an older boundary must not clear the later alias
            await wait_until(lambda: "root-a" not in call._active_turns)
            assert call._active_turns == {"root-b": "input-b", "alias-a": "input-a"}
            await settled("alias-a")
            await wait_until(lambda: "alias-a" not in call._active_turns)
            assert call._active_turns == {"root-b": "input-b"}
            await settled(None)  # unknown/legacy settlement cannot erase current work
            await settled("unrelated")
            clock[0] += 60
            await quiet_for(vm, 0.15)
            assert not task.done()
            await settled("root-b")
            await wait_until(lambda: not call._active_turns)
            # Duplicated old start/settlement neither reopens work nor clears a
            # newer generation. Idle gets a fresh window after real settlement.
            await chunk(vm, media, line({"type": "hatch.voice.turn_started",
                "submission_message_id": "root-a", "item_id": "input-a"}))
            await settled("root-a")
            clock[0] += 44
            await quiet_for(vm, 0.15)
            clock[0] += 2
            await hangup(vm, media, start)
            assert await task == "idle_timeout"
    asyncio.run(scenario())


def test_active_work_cannot_bypass_overall_deadline(monkeypatch):
    clock = [1000.0]
    monkeypatch.setattr(voice, "time", SimpleNamespace(monotonic=lambda: clock[0], time=time.time))

    async def scenario():
        async with connected(register_device=False) as (session, vm, unused):
            call = voice.VoiceCall(session, device_id="homelink-synthetic", overall_timeout=90)
            task = asyncio.create_task(call.run(source(TONE), Output()))
            media, start = await admit(vm)
            await next_event(vm)
            await next_event(vm)
            await chunk(vm, media, line({"type": "hatch.voice.turn_started",
                "submission_message_id": "root", "item_id": "input"}))
            await wait_until(lambda: bool(call._active_turns))
            clock[0] += 91
            await hangup(vm, media, start)
            assert await task == "overall_timeout"
    asyncio.run(scenario())


def test_acknowledged_end_call_survives_slow_media_eof_by_reset():
    async def scenario():
        async with connected(register_device=False) as (session, vm, unused):
            call = voice.VoiceCall(session, device_id="homelink-synthetic", cleanup_timeout=0.05)
            task = asyncio.create_task(call.run(source(TONE), Output()))
            media, start = await admit(vm)
            await next_event(vm)
            await next_event(vm)
            await chunk(vm, media, line({"type": "conversation.item.added", "item": {
                "type": "session_control", "name": "stop", "call_id": "synthetic-control"}}))
            request = await vm.next_frame()
            assert request.value.path == "/voice/end-call"
            await response(vm, request.stream_id, b'{"ok":true,"result":{"ok":true}}', end=True)
            assert (await vm.next_frame()).value.end_body
            # Deliberately omit response EOS; application ACK is already final.
            reset = await asyncio.wait_for(vm.next_frame(), 1)
            assert reset.kind == "reset" and reset.stream_id == media
            assert await task == "remote_stop"
            assert call.end_call_acknowledged and call.media_drain_incomplete
            assert call.cleanup_error is None and not session._requests
    asyncio.run(scenario())


def test_missing_end_call_ack_is_still_a_failure():
    async def scenario():
        async with connected(register_device=False) as (session, vm, unused):
            call = voice.VoiceCall(session, device_id="homelink-synthetic", cleanup_timeout=0.05)
            task = asyncio.create_task(call.run(source(TONE), Output()))
            media, start = await admit(vm)
            await next_event(vm)
            await next_event(vm)
            await chunk(vm, media, end=True)
            request = await vm.next_frame()
            assert request.value.path == "/voice/end-call"
            with pytest.raises(asyncio.TimeoutError):
                await asyncio.wait_for(task, 1)
            assert not call.end_call_acknowledged
            assert not session._requests
    asyncio.run(scenario())
