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

from musegadget.voice_call import (AudioChunk, PlaybackFlushed, ResponseDone, ResponseStarted,
                                   TranscriptDelta, TranscriptDone, VoiceCall, Working)
from test_streaming import chunk, connected
from test_voice_call import TONE, Output, admit, hangup, line
from test_voice_gate import audio, silent_source, speech, turn
from test_voice_lifecycle import wait_until

HALF = TONE[:960]
QUIET = b"\x10\x00" * 480


def created(response_id):
    return {"type": "response.created", "response": {"id": response_id}}


def done(response_id, status="completed"):
    return {"type": "response.done", "response": {"id": response_id, "status": status}}


def said(text):
    return {"type": "response.output_audio_transcript.delta", "event_id": "e", "delta": text}


def transcript(text):
    return {"type": "response.output_audio_transcript.done", "event_id": "e", "transcript": text}


def settled(submission):
    return {"type": "hatch.voice.turn_settled", "response_message_id": submission}


def run_call(script):
    events = []

    async def scenario():
        async with connected(register_device=False) as (session, vm, unused):
            output = Output()
            call = VoiceCall(session, device_id="homelink-synthetic", on_event=events.append)
            task = asyncio.create_task(call.run(silent_source(), output))
            media, start = await admit(vm)
            await script(vm, media, call, output, events)
            await chunk(vm, media, end=True)
            await hangup(vm, media, start, media_ended=True)
            assert await asyncio.wait_for(task, 2) == "remote_end"

    asyncio.run(asyncio.wait_for(scenario(), 5))
    return events


def test_events_follow_a_reply_and_audio_offsets_count_samples_per_response():
    async def script(vm, media, call, output, events):
        await chunk(vm, media, b"".join(map(line, [
            turn("item-1", "sub-1"), created("r1"), said("Hello "), audio("p1", TONE),
            said("there"), audio("p1", HALF), transcript("Hello there"), done("r1"),
            settled("sub-1"), created("r2"), audio("p2", QUIET), done("r2", "cancelled"),
        ])))
        await wait_until(lambda: len(output.pcm) == len(TONE) + len(HALF) + len(QUIET))

    events = run_call(script)
    assert events == [
        Working(True),
        ResponseStarted("r1"),
        TranscriptDelta("r1", "Hello "),
        AudioChunk("r1", 0, TONE),
        TranscriptDelta("r1", "there"),
        AudioChunk("r1", 960, HALF),
        TranscriptDone("r1", "Hello there"),
        ResponseDone("r1", "completed"),
        Working(False),
        ResponseStarted("r2"),
        AudioChunk("r2", 0, QUIET),
        ResponseDone("r2", "cancelled"),
    ]


def test_audio_keeps_the_response_it_started_in_when_the_next_response_opens():
    async def script(vm, media, call, output, events):
        await chunk(vm, media, b"".join(map(line, [
            created("r1"), audio("p1", TONE), created("r2"), audio("p1", HALF), audio("p2", HALF),
        ])))
        await wait_until(lambda: len(output.pcm) == len(TONE) + 2 * len(HALF))

    assert run_call(script) == [
        ResponseStarted("r1"),
        AudioChunk("r1", 0, TONE),
        ResponseStarted("r2"),
        AudioChunk("r1", 960, HALF),
        AudioChunk("r2", 0, HALF),
    ]


def test_held_reply_audio_is_delivered_when_its_turn_confirms_it():
    async def script(vm, media, call, output, events):
        await chunk(vm, media, b"".join(map(line, [speech("item-1"), created("r1"), audio("p1", TONE)])))
        await wait_until(lambda: bool(call._waiting_pcm))
        assert events == [ResponseStarted("r1")]
        await chunk(vm, media, line(turn("item-1", "sub-1")) + line(audio("p1", HALF)))
        await wait_until(lambda: len(output.pcm) == len(TONE) + len(HALF))

    assert run_call(script) == [
        ResponseStarted("r1"),
        Working(True),
        AudioChunk("r1", 0, TONE),
        AudioChunk("r1", 960, HALF),
    ]


def test_caller_speech_flushes_delivered_audio_and_drops_the_rest_of_the_reply():
    async def script(vm, media, call, output, events):
        await chunk(vm, media, line(created("r1")) + line(audio("p1", TONE * 5)) + line(speech("item-2")))
        await wait_until(lambda: PlaybackFlushed() in events)
        await chunk(vm, media, b"".join(map(line, [said("stale"), audio("p1", TONE), done("r1", "cancelled")])))
        await wait_until(lambda: ResponseDone("r1", "cancelled") in events)

    assert run_call(script) == [
        ResponseStarted("r1"),
        AudioChunk("r1", 0, TONE * 5),
        PlaybackFlushed(),
        TranscriptDelta("r1", "stale"),
        ResponseDone("r1", "cancelled"),
    ]


def test_unidentified_responses_and_legacy_audio_report_no_id():
    legacy = {"type": "response.output_audio.delta", "delta": base64.b64encode(HALF).decode()}

    async def script(vm, media, call, output, events):
        await chunk(vm, media, b"".join(map(line, [
            said("early"), legacy, {"type": "response.created"}, legacy,
            {"type": "response.done", "response": {"id": "r9", "status": 7}},
        ])))
        await wait_until(lambda: len(output.pcm) == len(HALF) * 2)

    assert run_call(script) == [
        TranscriptDelta(None, "early"),
        AudioChunk(None, 0, HALF),
        ResponseStarted(None),
        AudioChunk(None, 480, HALF),
        ResponseDone("r9", None),
    ]
