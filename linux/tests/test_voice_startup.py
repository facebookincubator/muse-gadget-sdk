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
import json
import time
from types import SimpleNamespace

import pytest

from musegadget import voice_call as voice
from test_streaming import chunk, connected, response
from test_voice_call import FIXTURE, Output, TONE, cavd, hangup, line, next_event


async def initial_media(vm, event):
    request = await vm.next_frame()
    assert request.value.path.startswith("/voice/realtime?")
    start = json.loads((await vm.next_frame()).value.data)
    assert (await next_event(vm))["type"] == "hatch.voice.capability.offer"
    raw = line(event)
    # The first response record is fragmented media, not session.created/ready.
    await response(vm, request.stream_id, raw[:17])
    for offset in range(17, len(raw), 97):
        await chunk(vm, request.stream_id, raw[offset:offset + 97])
    return request.stream_id, start


def ready_event(call_id):
    return {"type": "hatch.voice.session", "call_id": call_id,
            "session_id": FIXTURE["session_id"], "thread_id": FIXTURE["thread_id"]}


def observe_validation(monkeypatch):
    validated = asyncio.Event()
    original = voice.audio_from_event

    def decode(event):
        pcm = original(event)
        if pcm is not None:
            validated.set()
        return pcm

    monkeypatch.setattr(voice, "audio_from_event", decode)
    return validated


def test_fragmented_zero_keepalive_before_ready_does_not_activate_audio(monkeypatch):
    async def scenario():
        validated = observe_validation(monkeypatch)
        source_requested = asyncio.Event()

        async def microphone():
            source_requested.set()
            yield TONE

        async with connected(register_device=False) as (session, vm, unused):
            output = Output()
            call = voice.VoiceCall(session, device_id="homelink-synthetic", cleanup_timeout=0.2)
            activity = call._activity
            task = asyncio.create_task(call.run(microphone(), output))
            try:
                media, start = await initial_media(vm, cavd(b"\0" * voice.FRAME_BYTES))
                await asyncio.wait_for(validated.wait(), 1)
                assert not call.ready.is_set() and not source_requested.is_set()
                assert not call._received_audio and not call._playback_bytes
                assert call._activity == activity and not output.pcm
                assert call._ready_deadline == call._started_mono + call.ready_timeout
                await chunk(vm, media, line(ready_event(start["call_id"])))
                assert (await next_event(vm))["type"] == "input_audio_buffer.append"
                assert source_requested.is_set()
                await chunk(vm, media, line(cavd()), end=True)
                await hangup(vm, media, start, media_ended=True)
                assert await asyncio.wait_for(task, 1) == "remote_end"
                assert output.pcm == TONE  # no startup keepalive was played
            finally:
                task.cancel()
                await asyncio.gather(task, return_exceptions=True)
    asyncio.run(scenario())


@pytest.mark.parametrize("deadline", ["ready", "overall"])
def test_startup_keepalives_do_not_renew_deadlines(monkeypatch, deadline):
    clock = [1000.0]
    monkeypatch.setattr(voice, "time", SimpleNamespace(monotonic=lambda: clock[0], time=time.time))

    async def scenario():
        validated = observe_validation(monkeypatch)
        source_requested = asyncio.Event()

        async def microphone():
            source_requested.set()
            yield TONE

        async with connected(register_device=False) as (session, vm, unused):
            output = Output()
            call = voice.VoiceCall(session, device_id="homelink-synthetic", ready_timeout=35,
                                  overall_timeout=30 if deadline == "overall" else 300)
            task = asyncio.create_task(call.run(microphone(), output))
            media, start = await initial_media(vm, cavd(b"\0" * voice.FRAME_BYTES))
            await asyncio.wait_for(validated.wait(), 1)
            original_deadline = call._ready_deadline
            validated.clear()
            clock[0] += 36
            await chunk(vm, media, line(cavd(b"\0" * voice.FRAME_BYTES)))
            await asyncio.wait_for(validated.wait(), 1)
            assert call._activity == 1000.0 and call._ready_deadline == original_deadline
            await asyncio.wait_for(hangup(vm, media, start, expect_thread=False), 1)
            if deadline == "ready":
                with pytest.raises(asyncio.TimeoutError):
                    await task
            else:
                assert await task == "overall_timeout"
            assert not call.ready.is_set() and not source_requested.is_set() and not output.pcm
    asyncio.run(scenario())


@pytest.mark.parametrize("failure", ["nonzero", "bad-base64", "odd-pcm", "bad-format", "identity"])
def test_startup_silence_exception_does_not_weaken_validation(failure):
    async def scenario():
        source_requested = asyncio.Event()

        async def microphone():
            source_requested.set()
            yield TONE

        async with connected(register_device=False) as (session, vm, unused):
            output = Output()
            call = voice.VoiceCall(session, device_id="homelink-synthetic")
            task = asyncio.create_task(call.run(microphone(), output))
            event = cavd(TONE if failure == "nonzero" else b"\0" * voice.FRAME_BYTES)
            if failure == "bad-base64":
                event["audio_delta"]["audio_frames"] = "not@base64"
            elif failure == "odd-pcm":
                event = cavd(b"\0")
            elif failure == "bad-format":
                event["audio_delta"]["format"] = {"type": "audio/pcm", "rate": 16000}
            media, start = await initial_media(vm, event)
            if failure == "identity":
                await chunk(vm, media, line(ready_event("not-the-admitted-call")))
            await asyncio.wait_for(hangup(vm, media, start, expect_thread=False), 1)
            with pytest.raises(ValueError):
                await task
            assert not call.ready.is_set() and not source_requested.is_set() and not output.pcm
    asyncio.run(scenario())
