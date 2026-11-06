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
from types import SimpleNamespace

import pytest

from musegadget import voice_call as voice
from musegadget.voice_call import (AudioChunk, PlaybackFlushed, ResponseDone, ResponseStarted,
                                   TranscriptDelta, TranscriptDone, VoiceCall, Working)
from test_streaming import chunk, connected
from test_voice_call import TONE, Output, admit, hangup, line, next_event
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


@pytest.mark.parametrize("words", [said, transcript])
def test_audio_only_call_keeps_five_seconds_of_held_pcm_with_transcripts(words):
    async def scenario():
        async with connected(register_device=False) as (session, vm, unused):
            output = Output()
            call = VoiceCall(session, device_id="homelink-synthetic")
            task = asyncio.create_task(call.run(silent_source(), output))
            media, start = await admit(vm)
            await chunk(vm, media, line(speech()) + line(created("r1")))
            for _ in range(125):
                await chunk(vm, media, line(audio("p1", TONE)))
            await chunk(vm, media, line(words("Hello")) + line(turn()), end=True)
            await hangup(vm, media, start, media_ended=True)
            assert await task == "remote_end"
            assert output.pcm == TONE * 125

    asyncio.run(asyncio.wait_for(scenario(), 10))


def test_audio_only_call_builds_no_events(monkeypatch):
    built = []

    def observe(event_type):
        def build(*args):
            event = event_type(*args)
            built.append(event)
            return event
        return build

    for event_type in (ResponseStarted, ResponseDone, TranscriptDelta, TranscriptDone,
                       AudioChunk, Working, PlaybackFlushed):
        monkeypatch.setattr(voice, event_type.__name__, observe(event_type))

    async def scenario():
        async with connected(register_device=False) as (session, vm, unused):
            output = Output()
            call = VoiceCall(session, device_id="homelink-synthetic")
            task = asyncio.create_task(call.run(silent_source(), output))
            media, start = await admit(vm)
            await chunk(vm, media, b"".join(map(line, [
                speech(), created("r1"), said("Hello"), transcript("Hello"),
                audio("p1", TONE), turn(), done("r1"), settled("turn"),
            ])))
            await output.written.wait()
            await call.barge_in()
            await chunk(vm, media, end=True)
            await hangup(vm, media, start, media_ended=True)
            assert await task == "remote_end"
            assert output.pcm == TONE

    asyncio.run(asyncio.wait_for(scenario(), 5))
    assert built == []


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


def test_zero_keepalive_binds_provider_to_the_response_open_at_first_audio():
    async def script(vm, media, call, output, events):
        await chunk(vm, media, b"".join(map(line, [
            created("r1"), audio("p1", b"\0" * len(TONE)), created("r2"),
            audio("p1", TONE), audio("p2", HALF),
        ])))
        await wait_until(lambda: len(output.pcm) == len(TONE) + len(HALF))

    assert run_call(script) == [
        ResponseStarted("r1"),
        ResponseStarted("r2"),
        AudioChunk("r1", 0, TONE),
        AudioChunk("r2", 0, HALF),
    ]


@pytest.mark.parametrize("observe", [False, True])
def test_held_transcript_does_not_turn_zero_keepalives_into_reply_audio(observe):
    async def scenario():
        async with connected(register_device=False) as (session, vm, unused):
            events = []
            output = Output()
            call = VoiceCall(session, device_id="homelink-synthetic",
                             on_event=events.append if observe else None)
            task = asyncio.create_task(call.run(silent_source(), output))
            media, start = await admit(vm)
            await chunk(vm, media, line(speech()) + line(created("r1")) + line(said("Hello")))
            for _ in range(126):
                await chunk(vm, media, line(audio("p1", b"\0" * len(TONE))))
            await chunk(vm, media, line(turn()), end=True)
            await hangup(vm, media, start, media_ended=True)
            assert await task == "remote_end"
            assert output.pcm == b""
            assert events == ([ResponseStarted("r1"), Working(True),
                               TranscriptDelta("r1", "Hello")] if observe else [])

    asyncio.run(asyncio.wait_for(scenario(), 5))


def test_held_reply_words_and_audio_are_delivered_when_its_turn_confirms_it():
    async def script(vm, media, call, output, events):
        await chunk(vm, media, b"".join(map(line, [
            speech("item-1"), created("r1"), said("Hi "), audio("p1", TONE),
        ])))
        await wait_until(lambda: bool(call._waiting))
        assert events == [ResponseStarted("r1")]
        await chunk(vm, media, b"".join(map(line, [turn("item-1", "sub-1"), said("there"), audio("p1", HALF)])))
        await wait_until(lambda: len(output.pcm) == len(TONE) + len(HALF))

    assert run_call(script) == [
        ResponseStarted("r1"),
        Working(True),
        TranscriptDelta("r1", "Hi "),
        AudioChunk("r1", 0, TONE),
        TranscriptDelta("r1", "there"),
        AudioChunk("r1", 960, HALF),
    ]


@pytest.mark.parametrize("replacement_pcm", [HALF, b"\0" * len(HALF)], ids=["tone", "quiet"])
def test_replacement_of_a_held_reply_keeps_the_new_responses_transcript(replacement_pcm):
    async def script(vm, media, call, output, events):
        await chunk(vm, media, b"".join(map(line, [
            speech(), created("r1"), said("old"), audio("p1", TONE),
            created("r2"), said("Olá"), transcript("Olá"), audio("p2", replacement_pcm), turn(),
        ])))
        await wait_until(lambda: len(output.pcm) == len(replacement_pcm))
        assert output.pcm == replacement_pcm
        await asyncio.sleep(len(replacement_pcm) / (voice.SAMPLE_RATE * 2))

    assert run_call(script) == [
        ResponseStarted("r1"),
        ResponseStarted("r2"),
        Working(True),
        TranscriptDelta("r2", "Olá"),
        TranscriptDone("r2", "Olá"),
        AudioChunk("r2", 0, replacement_pcm),
    ]


def test_a_held_reply_dropped_for_another_callers_turn_says_nothing():
    async def script(vm, media, call, output, events):
        await chunk(vm, media, b"".join(map(line, [
            speech("item-2"), created("r2"), said("lost "), audio("p2", TONE),
            turn("item-1", "sub-1"), said("words"), transcript("lost words"), done("r2"),
        ])))
        await wait_until(lambda: ResponseDone("r2", "completed") in events)

    assert run_call(script) == [
        ResponseStarted("r2"),
        Working(True),
        ResponseDone("r2", "completed"),
    ]


def test_caller_speech_flushes_delivered_audio_and_drops_the_rest_of_the_reply():
    async def script(vm, media, call, output, events):
        await chunk(vm, media, line(created("r1")) + line(audio("p1", TONE * 5)) + line(speech("item-2")))
        await wait_until(lambda: PlaybackFlushed(("r1",)) in events)
        await chunk(vm, media, b"".join(map(line, [
            said("stale"), audio("p1", TONE), transcript("stale"), done("r1", "cancelled"),
            turn("item-2", "sub-2"), created("r2"), said("Yes"), audio("p2", HALF),
        ])))
        await wait_until(lambda: AudioChunk("r2", 0, HALF) in events)

    assert run_call(script) == [
        ResponseStarted("r1"),
        AudioChunk("r1", 0, TONE * 5),
        PlaybackFlushed(("r1",)),
        ResponseDone("r1", "cancelled"),
        Working(True),
        ResponseStarted("r2"),
        TranscriptDelta("r2", "Yes"),
        AudioChunk("r2", 0, HALF),
    ]


@pytest.mark.parametrize("preceding_audio", [b"", TONE], ids=["quiet_only", "after_tone"])
def test_playback_flush_names_the_quiet_frame_in_flight(monkeypatch, preceding_audio):
    clock = [1000.0]
    monkeypatch.setattr(voice, "time", SimpleNamespace(monotonic=lambda: clock[0]))

    async def scenario():
        events = []
        output = Output()
        call = VoiceCall(None, device_id="homelink-synthetic", on_event=events.append)
        call._output = output
        if preceding_audio:
            call._queue_pcm(preceding_audio, "r1")
        task = asyncio.create_task(call._play())
        try:
            if preceding_audio:
                await output.written.wait()
                clock[0] += 0.05  # The tone ended, but its microphone settling tail remains.
                output.written.clear()
            call._queue_pcm(QUIET, "r2")
            await output.written.wait()
            assert output.pcm == preceding_audio + QUIET
            assert call._gated() == bool(preceding_audio)
            await call.barge_in()
            assert [event for event in events if isinstance(event, PlaybackFlushed)] == [
                PlaybackFlushed(("r2",)),
            ]
        finally:
            task.cancel()
            await asyncio.gather(task, return_exceptions=True)

    asyncio.run(asyncio.wait_for(scenario(), 2))


def test_microphone_settling_tail_is_not_reported_as_flushed_audio(monkeypatch):
    clock = [1000.0]
    monkeypatch.setattr(voice, "time", SimpleNamespace(monotonic=lambda: clock[0]))

    async def scenario():
        events = []
        output = Output()
        call = VoiceCall(None, device_id="homelink-synthetic", on_event=events.append)
        call._output = output
        call._queue_pcm(TONE, "r1")
        task = asyncio.create_task(call._play())
        try:
            await output.written.wait()
            clock[0] += 0.1
            assert call._gated()
            await call.barge_in()
            assert [event for event in events if isinstance(event, PlaybackFlushed)] == []
        finally:
            task.cancel()
            await asyncio.gather(task, return_exceptions=True)

    asyncio.run(asyncio.wait_for(scenario(), 2))


def test_barge_in_silences_the_words_of_the_reply_it_cuts_before_its_audio():
    async def script(vm, media, call, output, events):
        await chunk(vm, media, line(created("r1")) + line(said("Sure ")))
        await wait_until(lambda: TranscriptDelta("r1", "Sure ") in events)
        await call.barge_in()
        assert await next_event(vm) == {"type": "response.cancel", "response_id": "r1"}
        await chunk(vm, media, b"".join(map(line, [said("thing"), audio("p1", TONE), done("r1", "cancelled")])))
        await wait_until(lambda: ResponseDone("r1", "cancelled") in events)

    assert run_call(script) == [
        ResponseStarted("r1"),
        TranscriptDelta("r1", "Sure "),
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
