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
import time

import pytest

from musegadget.voice_call import VoiceCall
from test_streaming import chunk, connected
from test_voice_call import (Output, TONE, admit, assert_quiet_append, cavd, hangup,
                             line, next_event, source)
from test_voice_lifecycle import wait_until


async def three_quiet_frames(vm):
    arrived = []
    for _ in range(3):
        frame = await asyncio.wait_for(vm.next_frame(), 1)
        assert_quiet_append(frame)
        arrived.append(time.monotonic())
    assert all(b - a >= 0.03 for a, b in zip(arrived, arrived[1:]))
    return arrived


def test_finite_input_keeps_paced_silence_for_endpointing():
    async def scenario():
        async with connected(register_device=False) as (session, vm, unused):
            output = Output()
            call = VoiceCall(session, device_id="homelink-synthetic", cleanup_timeout=0.2)
            task = asyncio.create_task(call.run(source(TONE), output))
            try:
                media, start = await admit(vm)
                assert base64.b64decode((await next_event(vm))["audio"]) == TONE
                last_audio = time.monotonic()
                activity = call._activity
                arrived = await three_quiet_frames(vm)
                assert arrived[0] - last_audio >= 0.03
                assert call._activity == activity and not output.pcm
                # Synthetic endpointing waits for PCM silence.
                await chunk(vm, media, line({"type": "input_audio_buffer.speech_stopped"})
                            + line(cavd()), end=True)
                await hangup(vm, media, start, media_ended=True)
                assert await task == "remote_end"
                assert output.pcm == TONE and call.end_call_acknowledged
                assert not session._requests
            finally:
                task.cancel()
                await asyncio.gather(task, return_exceptions=True)
    asyncio.run(scenario())


@pytest.mark.parametrize("deadline", ["idle", "overall"])
def test_eof_silence_does_not_renew_call_deadlines(deadline):
    async def scenario():
        async with connected(register_device=False) as (session, vm, unused):
            call = VoiceCall(session, device_id="homelink-synthetic",
                             idle_timeout=0.35 if deadline == "idle" else 10,
                             overall_timeout=0.35 if deadline == "overall" else 10)
            task = asyncio.create_task(call.run(source(TONE), Output()))
            media, start = await admit(vm)
            await next_event(vm)
            activity = call._activity
            await three_quiet_frames(vm)
            assert call._activity == activity
            await asyncio.wait_for(hangup(vm, media, start), 1)
            assert await task == deadline + "_timeout"
            assert call.end_call_acknowledged and not session._requests
    asyncio.run(scenario())


@pytest.mark.parametrize("gate", ["hold", "playback"])
@pytest.mark.parametrize("realtime", [False, True])
def test_eof_quiet_frames_respect_gates_and_cancellation(gate, realtime):
    async def scenario():
        async with connected(register_device=False) as (session, vm, unused):
            output = Output()
            call = VoiceCall(session, device_id="homelink-synthetic")
            task = asyncio.create_task(call.run(source(TONE), output, realtime_input=realtime))
            media, start = await admit(vm)
            await next_event(vm)
            assert_quiet_append(await vm.next_frame())
            if gate == "hold":
                await chunk(vm, media, line({"type": "hatch.voice.switch_input_hold", "held": True}))
                await wait_until(lambda: call.held)
            else:
                await chunk(vm, media, line(cavd(TONE * 4)))
                await asyncio.wait_for(output.written.wait(), 1)
            with pytest.raises(asyncio.TimeoutError):
                await asyncio.wait_for(vm.next_frame(), 0.09)
            if gate == "hold":
                await chunk(vm, media, line({"type": "hatch.voice.switch_input_hold", "held": False}))
            else:
                await chunk(vm, media, line({"type": "input_audio_buffer.speech_started"}))
            await three_quiet_frames(vm)
            task.cancel()
            await hangup(vm, media, start)
            with pytest.raises(asyncio.CancelledError):
                await task
            # hangup verifies that after end-call ACK the next frame is upload
            # EOS, never another silence append or commit.
            assert call.end_call_acknowledged and not session._requests
    asyncio.run(scenario())


def test_threshold_quiet_is_replaced_with_pcm_without_manual_commits():
    async def scenario():
        begin_quiet, resume_speech = asyncio.Event(), asyncio.Event()
        noise = b"\x64\0" * 960  # nonzero PCM, RMS 100, below threshold

        async def microphone():
            yield TONE
            await begin_quiet.wait()
            for _ in range(5):
                yield noise
            await resume_speech.wait()
            yield TONE

        async with connected(register_device=False) as (session, vm, unused):
            call = VoiceCall(session, device_id="homelink-synthetic", mic_threshold=500)
            task = asyncio.create_task(call.run(microphone(), Output()))
            media, start = await admit(vm)
            assert base64.b64decode((await next_event(vm))["audio"]) == TONE
            activity = call._activity
            await asyncio.sleep(0.42)  # exhaust the speech tail before quiet arrives
            begin_quiet.set()
            await three_quiet_frames(vm)
            for _ in range(2):
                assert_quiet_append(await vm.next_frame())
            assert call._activity == activity
            resume_speech.set()
            assert base64.b64decode((await next_event(vm))["audio"]) == TONE
            # Speech resumes normally; EOF returns to paced zeros without
            # manufacturing another input item via a manual commit.
            await three_quiet_frames(vm)
            await chunk(vm, media, end=True)
            await hangup(vm, media, start, media_ended=True)
            assert await task == "remote_end"
    asyncio.run(scenario())
