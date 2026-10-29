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

import pytest

from musegadget.voice_call import FRAME_BYTES, VoiceCall
from test_streaming import chunk, connected
from test_voice_call import Output, TONE, admit, cavd, hangup, line, next_event
from test_voice_gate import audio, silent_source, speech, turn
from test_voice_lifecycle import wait_until


def committed(item):
    return {"type": "input_audio_buffer.committed", "item_id": item, "previous_item_id": None}


def transcript(item, final=False):
    kind = "completed" if final else "delta"
    return {"type": "conversation.item.input_audio_transcription." + kind,
            "item_id": item, "content_index": 0,
            "transcript" if final else "delta": "Synthetic test speech."}


def endpoint_events(commits):
    # The mapper assigns the final to the oldest committed item, whereas
    # provider speech events retain their own item identity.
    item = (commits + ["caller"])[0]
    return [transcript("caller"),
            {"type": "input_audio_buffer.speech_stopped", "item_id": "caller"},
            committed("caller"), transcript(item, final=True), turn(item),
            {"type": "response.created", "response": {"id": "mapper"}},
            audio("provider", TONE * 2), cavd(b"\0" * FRAME_BYTES),
            {"type": "response.done", "response": {"id": "mapper"}},
            {"type": "hatch.voice.turn_settled", "response_message_id": "turn"}]


@pytest.mark.parametrize("realtime", [False, True])
@pytest.mark.parametrize("quiet", ["eof", "threshold"])
def test_automatic_endpointing_preserves_caller_item_and_actual_playback(quiet, realtime):
    async def scenario():
        release = asyncio.Event()

        async def microphone():
            yield TONE
            await release.wait()
            if quiet == "threshold":
                for _ in range(5):
                    yield b"\x64\0" * 960
                await asyncio.Future()

        async with connected(register_device=False) as (session, vm, unused):
            output = Output()
            call = VoiceCall(session, device_id="homelink-synthetic", mic_threshold=500)
            task = asyncio.create_task(call.run(microphone(), output, realtime_input=realtime))
            media, start = await admit(vm)
            assert base64.b64decode((await next_event(vm))["audio"]) == TONE
            await chunk(vm, media, line(cavd(b"\0" * FRAME_BYTES)) + line(speech("caller"))
                        + line(transcript("caller")))
            await wait_until(lambda: call._reply_gate.item == "caller")
            if quiet == "threshold":
                await asyncio.sleep(0.42)
            release.set()
            commits, quiet_frames = [], 0
            while quiet_frames < 3:
                event = await next_event(vm)
                if event["type"] == "input_audio_buffer.commit":
                    commits.append("manual-item")
                    await chunk(vm, media, line(committed("manual-item")))
                else:
                    assert event["type"] == "input_audio_buffer.append"
                    assert base64.b64decode(event["audio"]) == b"\0" * FRAME_BYTES
                    quiet_frames += 1
            await chunk(vm, media, b"".join(map(line, endpoint_events(commits))), end=True)
            await hangup(vm, media, start, media_ended=True)
            assert await task == "remote_end"
            assert output.pcm == TONE * 2
            assert commits == []
            assert call._settled_turns == {"turn": "caller"}
            assert call.end_call_acknowledged and not session._requests

    asyncio.run(asyncio.wait_for(scenario(), 5))


def test_unlinked_committed_item_cannot_alias_current_speech():
    async def scenario():
        async with connected(register_device=False) as (session, vm, unused):
            output = Output()
            call = VoiceCall(session, device_id="homelink-synthetic")
            task = asyncio.create_task(call.run(silent_source(), output))
            media, start = await admit(vm)
            events = [speech("caller"), transcript("caller"), committed("manual-item")]
            events += endpoint_events(["manual-item"])
            await chunk(vm, media, b"".join(map(line, events)), end=True)
            await hangup(vm, media, start, media_ended=True)
            assert await task == "remote_end"
            assert not output.pcm
            assert call._reply_gate.blocked
            assert call.end_call_acknowledged and not session._requests

    asyncio.run(asyncio.wait_for(scenario(), 5))
