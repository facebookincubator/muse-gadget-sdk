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

import pytest

from musegadget.voice_call import VoiceCall
from test_streaming import connected, response, chunk
from test_voice_call import Output, TONE, admit, hangup, line, next_event
from test_voice_lifecycle import wait_until


@pytest.mark.parametrize("gate", ["hold", "hold_resume", "playback_flush", "end", "cancel"])
def test_queued_microphone_is_rechecked_under_send_lock(gate):
    async def scenario():
        async with connected(register_device=False) as (session, vm, unused):
            capture, next_capture, queued = asyncio.Event(), asyncio.Event(), asyncio.Event()

            async def source():
                await capture.wait()
                yield TONE
                await next_capture.wait()
                yield b"\x01\0" * 960
                await asyncio.Future()

            call = VoiceCall(session, device_id="homelink-synthetic")
            task = asyncio.create_task(call.run(source(), Output(), realtime_input=True))
            media, start = await admit(vm)
            original = call._event

            async def observe(event):
                if event["type"] == "input_audio_buffer.append":
                    queued.set()
                await original(event)

            call._event = observe
            await session._send_lock.acquire()
            try:
                capture.set()
                await queued.wait()
                if gate.startswith("hold"):
                    await chunk(vm, media, line({"type": "hatch.voice.switch_input_hold", "held": True}))
                    await wait_until(lambda: call.held)
                    if gate == "hold_resume":
                        await chunk(vm, media, line({"type": "hatch.voice.switch_input_hold", "held": False}))
                        await wait_until(lambda: not call.held)
                elif gate == "playback_flush":
                    from test_voice_call import cavd
                    epoch = call._mic_epoch
                    await chunk(vm, media, line(cavd(TONE)))
                    await wait_until(lambda: call._mic_epoch != epoch)
                    await chunk(vm, media, line({"type": "input_audio_buffer.speech_started"}))
                    await wait_until(lambda: not call._gated())
                elif gate == "cancel":
                    task.cancel()
                    await wait_until(lambda: call._remote_ended)
                else:
                    await chunk(vm, media, end=True)
                    await wait_until(lambda: call._remote_ended)
            finally:
                session._send_lock.release()
            if gate in ("end", "cancel"):
                await hangup(vm, media, start, media_ended=gate == "end")
                if gate == "end":
                    assert await task == "remote_end"
                else:
                    with pytest.raises(asyncio.CancelledError):
                        await task
                return
            # A following request decrypts first: the old PCM never reached the wire.
            barrier = asyncio.create_task(session.request("GET", "/synthetic-barrier"))
            frame = await vm.next_frame()
            assert frame.kind == "request" and frame.value.path == "/synthetic-barrier"
            await response(vm, frame.stream_id, b"{}", end=True)
            assert (await barrier)["ok"] and not session._closed
            if gate == "hold":
                await chunk(vm, media, line({"type": "hatch.voice.switch_input_hold", "held": False}))
                await wait_until(lambda: not call.held)
            next_capture.set()
            event = await next_event(vm)
            assert base64.b64decode(event["audio"]) == b"\x01\0" * 960
            task.cancel()
            await hangup(vm, media, start)
            with pytest.raises(asyncio.CancelledError):
                await task

    asyncio.run(asyncio.wait_for(scenario(), 5))
