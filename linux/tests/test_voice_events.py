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
import wave
from types import SimpleNamespace

import pytest

from musegadget import voice_call as voice
from musegadget.voice_audio import WavAudio
from musegadget.voice_call import (AudioChunk, PlaybackFlushed, ResponseDone, ResponseStarted,
                                   TranscriptDelta, TranscriptDone, VoiceCall, Working)
from test_streaming import chunk, connected
from test_streaming import response as reply_to
from test_voice_audio import open_audio, playback
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


@pytest.mark.parametrize("observe", [False, True], ids=["audio_only", "observed"])
@pytest.mark.parametrize("words", [said, transcript])
def test_call_keeps_five_seconds_of_held_pcm_with_transcripts(words, observe):
    async def scenario():
        async with connected(register_device=False) as (session, vm, unused):
            output = Output()
            call = VoiceCall(session, device_id="homelink-synthetic",
                             on_event=[].append if observe else None)
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


def test_held_words_have_their_own_byte_and_count_bounds():
    async def scenario():  # VoiceCall needs a running loop on Python 3.9
        call = VoiceCall(None, device_id="homelink-synthetic", on_event=[].append)
        call._reply_gate.event(speech())
        call._reply_gate.event(created("r1"))
        with pytest.raises(ValueError, match="buffer full"):
            call._words(TranscriptDone("r1", "a" * (voice.MAX_HELD_TEXT_BYTES + 1)))
        for _ in range(voice.MAX_HELD_ITEMS):
            call._words(TranscriptDelta("r1", "a"))
        with pytest.raises(ValueError, match="buffer full"):
            call._words(TranscriptDelta("r1", "a"))
        assert len(call._waiting) == voice.MAX_HELD_ITEMS

    asyncio.run(scenario())


def test_empty_transcript_deltas_are_neither_held_nor_delivered():
    async def script(vm, media, call, output, events):
        await chunk(vm, media, b"".join(map(line, [speech(), created("r1")] + [said("")] * 50)))
        await chunk(vm, media, line(said("Hi")))
        await wait_until(lambda: len(call._waiting) == 1)
        await chunk(vm, media, b"".join(map(line, [audio("p1", TONE), turn(), said("")])))
        await wait_until(lambda: output.pcm == TONE)

    events = run_call(script)
    assert [event for event in events if isinstance(event, TranscriptDelta)] == [
        TranscriptDelta("r1", "Hi")]


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


def test_a_response_that_can_still_play_keeps_its_offset_past_the_id_bound():
    tiny = b"\x10\x00" * 2
    legacy = {"type": "response.output_audio.delta", "delta": base64.b64encode(tiny).decode()}
    others = ["r%d" % n for n in range(2, voice.MAX_IDS + 3)]

    async def script(vm, media, call, output, events):
        await chunk(vm, media, line(created("r1")) + line(audio("p1", HALF)))
        await chunk(vm, media, b"".join(line(event) for response_id in others
                                        for event in (created(response_id), legacy)))
        await wait_until(lambda: len(output.pcm) == len(HALF) + len(others) * len(tiny))
        await chunk(vm, media, line(audio("p1", HALF)))
        await wait_until(lambda: len(output.pcm) == 2 * len(HALF) + len(others) * len(tiny))
        assert len(call._audio_offsets) <= voice.MAX_IDS

    chunks = [event for event in run_call(script) if isinstance(event, AudioChunk)]
    assert chunks == [AudioChunk("r1", 0, HALF), *(AudioChunk(response_id, 0, tiny)
                                                   for response_id in others),
                      AudioChunk("r1", 480, HALF)]


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


def test_a_provider_that_replaces_a_held_one_in_the_same_response_keeps_its_words():
    async def script(vm, media, call, output, events):
        await chunk(vm, media, b"".join(map(line, [
            speech("item-1"), created("r1"), audio("p1", TONE), said("Hello"), transcript("Hello"),
            audio("p2", HALF), turn("item-1", "sub-1"),
        ])))
        await wait_until(lambda: output.pcm == HALF)

    assert run_call(script) == [
        ResponseStarted("r1"),
        Working(True),
        TranscriptDelta("r1", "Hello"),
        TranscriptDone("r1", "Hello"),
        AudioChunk("r1", 0, HALF),
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


@pytest.mark.parametrize("dropped", [
    [said("lost "), audio("p2", TONE), turn("item-1", "sub-1")],
    [said("lost "), turn("item-1", "sub-1"), audio("p2", TONE)],
], ids=["audio_held_first", "audio_after_foreign_turn"])
def test_words_of_a_reply_dropped_for_another_callers_turn_stay_dropped_after_the_callers_turn(dropped):
    async def script(vm, media, call, output, events):
        await chunk(vm, media, b"".join(map(line, [speech("item-2"), created("r2")] + dropped + [
            turn("item-2", "sub-2"), transcript("lost words"), audio("p2", TONE),
            created("r3"), audio("p3", HALF), said("kept"),
        ])))
        await wait_until(lambda: output.pcm == HALF and TranscriptDelta("r3", "kept") in events)

    events = run_call(script)
    spoken = (TranscriptDelta, TranscriptDone, AudioChunk)
    assert [event for event in events if isinstance(event, spoken)] == [
        AudioChunk("r3", 0, HALF), TranscriptDelta("r3", "kept")]


def test_a_late_foreign_turn_keeps_the_words_of_the_confirmed_reply_still_playing():
    async def script(vm, media, call, output, events):
        await chunk(vm, media, b"".join(map(line, [
            speech("item-2"), turn("item-2", "sub-2"), created("r2"), audio("p2", TONE),
            turn("item-1", "sub-1"), said("still "), transcript("still here"), audio("p2", HALF),
            created("r3"), said("not this"), audio("p3", HALF),
        ])))
        await wait_until(lambda: len(output.pcm) == len(TONE) + len(HALF)
                         and ResponseStarted("r3") in events)

    events = run_call(script)
    spoken = (TranscriptDelta, TranscriptDone, AudioChunk)
    assert [event for event in events if isinstance(event, spoken)] == [
        AudioChunk("r2", 0, TONE), TranscriptDelta("r2", "still "),
        TranscriptDone("r2", "still here"), AudioChunk("r2", 960, HALF)]


def test_a_dropped_foreign_reply_does_not_silence_the_words_of_the_reply_still_playing():
    async def script(vm, media, call, output, events):
        await chunk(vm, media, b"".join(map(line, [
            speech("B"), turn("B", "SB"), created("RB"), audio("PB"),
            turn("A", "SA"), audio("PA"),  # new after another caller's turn: dropped
            said("still here"), transcript("still here"), audio("PB"),
        ])))
        await wait_until(lambda: len(output.pcm) == 2 * len(TONE))

    assert run_call(script) == [
        Working(True),
        ResponseStarted("RB"),
        AudioChunk("RB", 0, TONE),
        TranscriptDelta("RB", "still here"),
        TranscriptDone("RB", "still here"),
        AudioChunk("RB", 960, TONE),
    ]


def test_words_play_with_a_continuation_that_replaces_the_confirmed_reply():
    async def script(vm, media, call, output, events):
        await chunk(vm, media, b"".join(map(line, [
            speech("item-1"), turn("item-1", "sub-1"), created("r1"), audio("p1", TONE * 5),
            audio("p2", HALF), said("more"),
        ])))
        await wait_until(lambda: output.pcm == HALF)

    assert run_call(script) == [
        Working(True),
        ResponseStarted("r1"),
        AudioChunk("r1", 0, TONE * 5),
        PlaybackFlushed(("r1",)),
        AudioChunk("r1", 4800, HALF),
        TranscriptDelta("r1", "more"),
    ]


def test_a_reply_dropped_for_a_late_foreign_turn_does_not_silence_the_next_reply():
    async def script(vm, media, call, output, events):
        await chunk(vm, media, b"".join(map(line, [
            speech("item-2"), created("r1"), said("old"), audio("p1", TONE), created("r2"),
            turn("item-1", "sub-1"),  # drops r1's held reply
            turn("item-2", "sub-2"), said("new"), audio("p2", HALF),
        ])))
        await wait_until(lambda: len(output.pcm) == len(HALF))

    assert run_call(script) == [
        ResponseStarted("r1"),
        ResponseStarted("r2"),
        Working(True),
        TranscriptDelta("r2", "new"),
        AudioChunk("r2", 0, HALF),
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


@pytest.mark.parametrize("failure", ["stopped", "full"])
def test_rejected_playback_is_reported_as_flushed_when_run_ends(monkeypatch, failure):
    output = open_audio(monkeypatch, (24000,), (24000,))
    if failure == "stopped":
        output._output_stream.active = False
        message = "audio device stopped"
    else:
        for _ in range(8):
            output.write(TONE)
        message = "audio device playback queue full"

    async def scenario():
        async with connected(register_device=False) as (session, vm, unused):
            events = []
            call = VoiceCall(session, device_id="homelink-synthetic", on_event=events.append)
            task = asyncio.create_task(call.run(silent_source(), output))
            media, start = await admit(vm)
            await chunk(vm, media, line(created("r1")) + line(audio("p1", HALF)))
            await hangup(vm, media, start)
            with pytest.raises(OSError, match=message):
                await task
            assert call.end_call_acknowledged
            assert call.cleanup_error is None
            assert not session._requests
            assert events == [
                ResponseStarted("r1"), AudioChunk("r1", 0, HALF), PlaybackFlushed(("r1",)),
            ]

    try:
        asyncio.run(asyncio.wait_for(scenario(), 5))
    finally:
        output.close()


@pytest.mark.parametrize("preceding_audio", [b"", TONE], ids=["quiet_only", "after_tone"])
def test_playback_flush_names_queued_quiet_audio(monkeypatch, preceding_audio):
    clock = [1000.0]
    monkeypatch.setattr(voice, "time", SimpleNamespace(monotonic=lambda: clock[0]))
    output = open_audio(monkeypatch, (24000,), (24000,))

    async def scenario():
        events = []
        call = VoiceCall(None, device_id="homelink-synthetic", on_event=events.append)
        call._output = output
        if preceding_audio:
            call._queue_pcm(preceding_audio, "r1")
        task = asyncio.create_task(call._play())
        try:
            if preceding_audio:
                await wait_until(lambda: output._playback_bytes == len(preceding_audio))
                assert playback(output) == preceding_audio
                clock[0] += 0.05  # The tone ended, but its microphone settling tail remains.
            call._queue_pcm(QUIET, "r2")
            await wait_until(lambda: output._playback_bytes == len(QUIET))
            assert call._gated() == bool(preceding_audio)
            await call.barge_in()
            assert playback(output) == b"\0" * voice.FRAME_BYTES
            assert [event for event in events if isinstance(event, PlaybackFlushed)] == [
                PlaybackFlushed(("r2",)),
            ]
        finally:
            task.cancel()
            await asyncio.gather(task, return_exceptions=True)

    try:
        asyncio.run(asyncio.wait_for(scenario(), 2))
    finally:
        output.close()


def test_barge_in_keeps_written_wav_audio_without_reporting_it_flushed(monkeypatch, tmp_path):
    monkeypatch.setattr(voice, "time", SimpleNamespace(monotonic=lambda: 1000.0))
    input_path, output_path = tmp_path / "in.wav", tmp_path / "out.wav"
    with wave.open(str(input_path), "wb") as wav:
        wav.setparams((1, 2, 24000, 0, "NONE", "not compressed"))
        wav.writeframes(b"")
    events = []

    async def scenario():
        async with connected(register_device=False) as (session, vm, unused):
            output = WavAudio(input_path, output_path)
            call = VoiceCall(session, device_id="homelink-synthetic", on_event=events.append)
            task = asyncio.create_task(call.run(silent_source(), output))
            try:
                media, start = await admit(vm)
                await chunk(vm, media, line(created("r1")) + line(audio("p1", HALF)))
                await wait_until(lambda: output._output.getnframes() == 480)
                await call.barge_in()
                assert await next_event(vm) == {"type": "response.cancel", "response_id": "r1"}
                await chunk(vm, media, end=True)
                await hangup(vm, media, start, media_ended=True)
                assert await task == "remote_end"
            finally:
                task.cancel()
                await asyncio.gather(task, return_exceptions=True)
                output.close()

    asyncio.run(asyncio.wait_for(scenario(), 5))
    with wave.open(str(output_path), "rb") as wav:
        assert wav.getnframes() == 480
        assert wav.readframes(480) == HALF
    assert events == [ResponseStarted("r1"), AudioChunk("r1", 0, HALF)]


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


@pytest.mark.parametrize("consumption, flushed", [
    ("none", [PlaybackFlushed(("r1", "r2"))]),
    ("first", [PlaybackFlushed(("r2",))]),
    ("partial", [PlaybackFlushed(("r2",))]),
    ("all", []),
])
def test_playback_flush_tracks_delayed_live_audio_callbacks(monkeypatch, consumption, flushed):
    clock = [1000.0]
    monkeypatch.setattr(voice, "time", SimpleNamespace(monotonic=lambda: clock[0]))
    output = open_audio(monkeypatch, (24000,), (24000,))

    async def scenario():
        events = []
        call = VoiceCall(None, device_id="homelink-synthetic", on_event=events.append)
        call._output = output
        call._queue_pcm(HALF, "r1")
        task = asyncio.create_task(call._play())
        try:
            await wait_until(lambda: output._playback_bytes == len(HALF))
            clock[0] += 0.03
            if consumption == "first":
                assert playback(output) == HALF + b"\0" * len(HALF)
            call._queue_pcm(TONE, "r2")
            await wait_until(lambda: not call._playback)
            clock[0] += 0.05
            if consumption in ("partial", "all"):
                assert playback(output) == HALF + TONE[:960]
            if consumption == "all":
                assert playback(output) == TONE[960:] + b"\0" * len(HALF)
            await call.barge_in()
            assert playback(output) == b"\0" * voice.FRAME_BYTES
            assert [event for event in events if isinstance(event, PlaybackFlushed)] == flushed
        finally:
            task.cancel()
            await asyncio.gather(task, return_exceptions=True)

    try:
        asyncio.run(asyncio.wait_for(scenario(), 2))
    finally:
        output.close()


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


def test_caller_speech_drops_only_the_words_of_the_reply_it_cuts():
    async def script(vm, media, call, output, events):
        await chunk(vm, media, b"".join(map(line, [
            created("r1"), said("Sure "), speech("item-2"), said("thing"),
            created("r2"), said("Yes"), audio("p2", HALF), turn("item-2", "sub-2"),
        ])))
        await wait_until(lambda: output.pcm == HALF)

    assert run_call(script) == [
        ResponseStarted("r1"),
        TranscriptDelta("r1", "Sure "),
        ResponseStarted("r2"),
        Working(True),
        TranscriptDelta("r2", "Yes"),
        AudioChunk("r2", 0, HALF),
    ]


def test_barge_in_callback_failure_cancels_response_and_ends_call():
    failure = RuntimeError("synthetic observer failure")
    events = []

    def observe(event):
        events.append(event)
        if isinstance(event, PlaybackFlushed):
            raise failure

    async def scenario():
        async with connected(register_device=False) as (session, vm, unused):
            output = Output()
            call = VoiceCall(session, device_id="homelink-synthetic", on_event=observe,
                             cleanup_timeout=0.2)
            task = asyncio.create_task(call.run(silent_source(), output))
            try:
                media, start = await admit(vm)
                await chunk(vm, media, line(created("r1")) + line(audio("p1", TONE * 5)))
                await output.written.wait()
                await call.barge_in()
                assert await next_event(vm) == {"type": "response.cancel", "response_id": "r1"}
                await hangup(vm, media, start)
                with pytest.raises(RuntimeError, match="synthetic observer failure") as caught:
                    await task
                assert caught.value is failure
                assert call.end_call_acknowledged
                assert call.cleanup_error is None
                assert not session._requests
                assert [event for event in events if isinstance(event, PlaybackFlushed)] == [
                    PlaybackFlushed(("r1",)),
                ]
            finally:
                task.cancel()
                await asyncio.gather(task, return_exceptions=True)

    asyncio.run(asyncio.wait_for(scenario(), 5))


def test_observer_cancellation_while_shutdown_flushes_still_ends_the_call():
    def observe(event):
        if isinstance(event, PlaybackFlushed):
            raise asyncio.CancelledError  # e.g. reading a cancelled task's result

    async def scenario():
        async with connected(register_device=False) as (session, vm, unused):
            output = Output()
            call = VoiceCall(session, device_id="homelink-synthetic", on_event=observe,
                             cleanup_timeout=0.2)
            task = asyncio.create_task(call.run(silent_source(), output))
            media, start = await admit(vm)
            await chunk(vm, media, line(created("r1")) + line(audio("p1", TONE * 5)))
            await output.written.wait()
            task.cancel()
            await hangup(vm, media, start)
            with pytest.raises(asyncio.CancelledError):
                await task
            assert call.end_call_acknowledged
            assert not session._requests

    asyncio.run(asyncio.wait_for(scenario(), 5))


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


@pytest.mark.parametrize("observe", [False, True], ids=["audio_only", "observed"])
def test_held_fragments_play_as_whole_frames(observe):
    # Playback sleeps once per queued fragment, so held audio released in
    # tiny pieces would play slower than real time and miss the drain deadline.
    reply = TONE * 5
    fragments = [reply[i:i + 48] for i in range(0, len(reply), 48)]
    events = []
    writes = []

    class Frames(Output):
        def write(self, data):
            writes.append(len(data))
            super().write(data)

    async def scenario():
        async with connected(register_device=False) as (session, vm, unused):
            output = Frames()
            call = VoiceCall(session, device_id="homelink-synthetic",
                             on_event=events.append if observe else None)
            task = asyncio.create_task(call.run(silent_source(), output))
            media, start = await admit(vm)
            held = [speech("item-1"), created("r1")]
            for index, pcm in enumerate(fragments):
                if index % 40 == 0:
                    held.append(said(f"w{index}"))
                held.append(audio("p1", pcm))
            await chunk(vm, media, b"".join(map(line, held)))
            await wait_until(lambda: call._waiting_bytes == len(reply))
            assert len(call._waiting) == (10 if observe else 1)
            await chunk(vm, media, line(turn("item-1", "sub-1")))
            await wait_until(lambda: len(output.pcm) == len(reply))
            await chunk(vm, media, end=True)
            await hangup(vm, media, start, media_ended=True)
            assert await asyncio.wait_for(task, 2) == "remote_end"
            assert output.pcm == reply

    asyncio.run(asyncio.wait_for(scenario(), 5))
    assert writes == [voice.FRAME_BYTES] * 5
    spoken = []
    for index in range(0, len(fragments), 40):
        spoken.append(TranscriptDelta("r1", f"w{index}"))
        spoken.append(AudioChunk("r1", index * 24, reply[index * 48:(index + 40) * 48]))
    assert events == ([ResponseStarted("r1"), Working(True)] + spoken if observe else [])


def test_held_words_between_frame_boundaries_split_the_reply_where_they_fall():
    reply = TONE * 5
    words = {0: "One ", 25: "two ", 90: "three ", 170: "four"}  # said before these 48-byte fragments
    events = []
    writes = []

    class Frames(Output):
        def write(self, data):
            writes.append(len(data))
            super().write(data)

    async def scenario():
        async with connected(register_device=False) as (session, vm, unused):
            output = Frames()
            call = VoiceCall(session, device_id="homelink-synthetic", on_event=events.append)
            task = asyncio.create_task(call.run(silent_source(), output))
            media, start = await admit(vm)
            held = [speech("item-1"), created("r1")]
            for index in range(len(reply) // 48):
                if index in words:
                    held.append(said(words[index]))
                held.append(audio("p1", reply[index * 48:(index + 1) * 48]))
            await chunk(vm, media, b"".join(map(line, held)))
            await wait_until(lambda: call._waiting_bytes == len(reply))
            await chunk(vm, media, line(turn("item-1", "sub-1")))
            await wait_until(lambda: len(output.pcm) == len(reply))
            await chunk(vm, media, end=True)
            await hangup(vm, media, start, media_ended=True)
            assert await asyncio.wait_for(task, 2) == "remote_end"
            assert output.pcm == reply

    asyncio.run(asyncio.wait_for(scenario(), 5))
    assert writes == [1200, 1920, 1200, 1920, 1920, 1440]
    assert events == [
        ResponseStarted("r1"), Working(True),
        TranscriptDelta("r1", "One "), AudioChunk("r1", 0, reply[:1200]),
        TranscriptDelta("r1", "two "), AudioChunk("r1", 600, reply[1200:4320]),
        TranscriptDelta("r1", "three "), AudioChunk("r1", 2160, reply[4320:8160]),
        TranscriptDelta("r1", "four"), AudioChunk("r1", 4080, reply[8160:]),
    ]


@pytest.mark.parametrize("observe", [False, True], ids=["audio_only", "observed"])
def test_many_held_fragments_within_the_byte_limit_still_play(observe):
    reply = (TONE * 103)[:4097 * 48]  # 4,097 fragments, about 4.1 s

    async def scenario():
        async with connected(register_device=False) as (session, vm, unused):
            output = Output()
            call = VoiceCall(session, device_id="homelink-synthetic",
                             on_event=[].append if observe else None)
            task = asyncio.create_task(call.run(silent_source(), output))
            media, start = await admit(vm)
            await chunk(vm, media, line(speech("item-1")) + line(created("r1")))
            for start_at in range(0, len(reply), 48 * 200):
                part = reply[start_at:start_at + 48 * 200]
                await chunk(vm, media, b"".join(
                    line(audio("p1", part[i:i + 48])) for i in range(0, len(part), 48)))
            while call._waiting_bytes < len(reply):
                await asyncio.sleep(0.01)
            await chunk(vm, media, line(turn("item-1", "sub-1")), end=True)
            await hangup(vm, media, start, media_ended=True)
            assert await task == "remote_end"
            assert output.pcm == reply

    asyncio.run(asyncio.wait_for(scenario(), 15))


def test_audio_announced_before_a_callback_failure_is_reported_flushed():
    failure = RuntimeError("synthetic observer failure")
    events = []

    def observe(event):
        events.append(event)
        if event == TranscriptDelta("r1", "boom"):
            raise failure

    async def scenario():
        async with connected(register_device=False) as (session, vm, unused):
            call = VoiceCall(session, device_id="homelink-synthetic", on_event=observe,
                             cleanup_timeout=0.2)
            task = asyncio.create_task(call.run(silent_source(), Output()))
            try:
                media, start = await admit(vm)
                await chunk(vm, media, b"".join(map(line, [
                    speech("item-1"), created("r1"), audio("p1", TONE * 5), said("boom"),
                ])))
                await wait_until(lambda: len(call._waiting) == 2)
                await chunk(vm, media, line(turn("item-1", "sub-1")))
                await hangup(vm, media, start)
                with pytest.raises(RuntimeError, match="synthetic observer failure"):
                    await task
            finally:
                task.cancel()
                await asyncio.gather(task, return_exceptions=True)

    asyncio.run(asyncio.wait_for(scenario(), 5))
    assert events[-3:] == [AudioChunk("r1", 0, TONE * 5), TranscriptDelta("r1", "boom"),
                           PlaybackFlushed(("r1",))]


def turn_settled(submission=None, pending=None):
    event = {"type": "hatch.voice.turn_settled"}
    if submission is not None:
        event["response_message_id"] = submission
    if pending is not None:
        event["async_handoffs_pending"] = pending
    return event


def working_after(*batches):
    async def script(vm, media, call, output, events):
        for batch in batches:
            await chunk(vm, media, b"".join(map(line, batch)) + line(created("mark")))
            await wait_until(lambda: ResponseStarted("mark") in events)
            events.append("|")
            del events[events.index(ResponseStarted("mark"))]

    return [event if event == "|" else event.working
            for event in run_call(script) if event == "|" or isinstance(event, Working)]


def test_working_stops_at_a_spoken_approval_boundary_until_the_next_caller_turn():
    assert working_after(
        [turn("item-1", "sub-1")],
        [turn_settled(pending=1)],  # an approval prompt: it may name no caller turn
        [turn_settled("sub-1", pending=1)],  # still waiting on the caller
        [turn("item-2", "sub-2")],
        [turn_settled("sub-2", pending=0)],  # retires sub-1 too: nothing is pending
    ) == [True, "|", False, "|", "|", True, "|", False, "|"]


def test_a_nothing_pending_settlement_retires_turns_it_does_not_name_except_one_awaiting_its_reply():
    assert working_after(
        [turn("item-1", "sub-1"), created("r1"), done("r1"), turn("item-2", "sub-2")],
        [turn_settled(pending=0)],  # retires sub-1; keeps sub-2, whose reply hasn't finished
        [turn_settled("sub-1", pending=0)],  # a repeat changes nothing
        [turn_settled(pending=0)],  # keeps it only once
    ) == [True, "|", "|", "|", False, "|"]


def test_a_finished_reply_does_not_keep_its_turn_through_a_nothing_pending_settlement():
    reply = {"type": "response.created", "response": {"id": "r1", "submission_message_id": "sub-1"}}
    assert working_after(
        [turn("item-1", "sub-1"), reply, done("r1")],
        [turn_settled(pending=0)],
    ) == [True, "|", False, "|"]


def test_a_replayed_older_turn_does_not_become_the_turn_awaiting_its_reply():
    assert working_after(
        [turn("item-1", "sub-1"), created("r1"), done("r1"),
         turn("item-2", "sub-2"), created("r2"), done("r2")],
        [turn("item-1", "sub-1")],  # a valid replay: both replies already finished
        [turn_settled(pending=0)],
    ) == [True, "|", "|", False, "|"]


@pytest.mark.parametrize("pending", ["0", -1, 0.0, 1e0, True, None])
def test_only_plain_handoff_counts_change_working(pending):
    assert working_after(
        [turn("item-1", "sub-1"), created("r1"), done("r1")],
        [turn_settled(pending=pending)],
    ) == [True, "|", "|"]


def test_a_barge_in_flush_failure_after_the_call_ended_is_raised_to_the_caller():
    failure = RuntimeError("synthetic flush failure")

    async def scenario():
        call = VoiceCall(None, device_id="homelink-synthetic", on_event=[].append)
        call._remote_ended = True

        def fail():
            raise failure

        call._apply_reply_flush = fail
        call._barge_in_failure = asyncio.get_running_loop().create_future()
        await call.barge_in()  # a running call ends with it instead
        assert call._barge_in_failure.exception() is failure
        with pytest.raises(RuntimeError, match="synthetic flush failure"):
            await call.barge_in()  # nothing awaits the finished call's future

    asyncio.run(scenario())


class FlushFailsOnce(Output):
    def __init__(self, failure=None):
        super().__init__()
        self.failure = failure

    def flush(self):
        super().flush()
        failure, self.failure = self.failure, None
        if failure is not None:
            raise failure


@pytest.mark.parametrize("failing", ["callback", "output"])
@pytest.mark.parametrize("failure_type", [asyncio.CancelledError, KeyboardInterrupt, SystemExit,
                                          StopIteration])
def test_barge_in_routes_every_flush_failure_through_run(failing, failure_type, caplog):
    failure = failure_type("synthetic flush failure")

    def observe(event):
        if failing == "callback" and isinstance(event, PlaybackFlushed):
            raise failure

    async def scenario():
        async with connected(register_device=False) as (session, vm, unused):
            output = FlushFailsOnce(failure if failing == "output" else None)
            call = VoiceCall(session, device_id="homelink-synthetic", on_event=observe,
                             cleanup_timeout=0.2)

            async def run():
                try:
                    return await call.run(silent_source(), output)
                except BaseException as exc:
                    return exc

            task = asyncio.create_task(run())
            try:
                media, start = await admit(vm)
                await chunk(vm, media, line(created("r1")) + line(audio("p1", TONE * 5)))
                await output.written.wait()
                raised = None
                try:
                    await call.barge_in()
                except BaseException as exc:
                    raised = exc
                assert raised is None
                assert await next_event(vm) == {"type": "response.cancel", "response_id": "r1"}
                await hangup(vm, media, start)
                result = await task
                if failure_type is StopIteration:
                    assert type(result) is RuntimeError
                    assert result.__cause__ is failure
                else:
                    assert result is failure
                assert call.end_call_acknowledged
                assert call.cleanup_error is None
                assert not session._requests
                assert caplog.messages == []
            finally:
                task.cancel()
                await asyncio.gather(task, return_exceptions=True)

    asyncio.run(asyncio.wait_for(scenario(), 5))


@pytest.mark.parametrize("observe", [False, True], ids=["audio_only", "observed"])
def test_a_barge_in_flush_failure_at_any_moment_as_the_call_ends_is_raised_exactly_once(observe):
    async def outcome(k):
        failure = RuntimeError("synthetic flush failure")
        async with connected(register_device=False) as (session, vm, unused):
            output = FlushFailsOnce()
            observer = {"on_event": [].append} if observe else {}
            call = VoiceCall(session, device_id="homelink-synthetic", **observer)
            task = asyncio.create_task(call.run(silent_source(), output))
            media, start = await admit(vm)
            raised = []

            async def barge_in_as_the_call_ends():
                while call.reason != "remote_end":
                    await asyncio.sleep(0)
                for _ in range(k):
                    await asyncio.sleep(0)
                output.failure = failure
                try:
                    await call.barge_in()
                except RuntimeError as exc:
                    raised.append(exc)

            barger = asyncio.create_task(barge_in_as_the_call_ends())
            await chunk(vm, media, line(created("r1")) + line(done("r1")), end=True)
            await hangup(vm, media, start, media_ended=True)
            try:
                await asyncio.wait_for(task, 2)
            except RuntimeError as exc:
                raised.append(exc)
            await asyncio.wait_for(barger, 1)
            assert output.failure is None  # the barge-in's flush did fail
            return [exc for exc in raised if exc is failure] == [failure]

    async def scenario():
        return [k for k in range(8) if not await asyncio.wait_for(outcome(k), 5)]

    assert asyncio.run(scenario()) == []


@pytest.mark.parametrize("order", ["barge_in_then_hang_up", "hang_up_then_barge_in"])
def test_a_barge_in_flush_failure_next_to_hangup_is_not_lost(order, caplog):
    failure = OSError("synthetic flush failure")
    seen = []

    async def scenario(reply_open):
        async with connected(register_device=False) as (session, vm, unused):
            output = FlushFailsOnce()
            call = VoiceCall(session, device_id="homelink-synthetic", cleanup_timeout=0.5)
            task = asyncio.create_task(call.run(silent_source(), output))
            media, start = await admit(vm)
            await wait_until(call.ready.is_set)
            if reply_open:
                await chunk(vm, media, line(created("r1")) + line(audio("p1", TONE * 5)))
                await output.written.wait()
            output.failure = failure

            async def silence_speaker():
                try:
                    await call.barge_in()
                except OSError as exc:
                    seen.append(exc)

            if order == "barge_in_then_hang_up":
                await silence_speaker()
                task.cancel()
            else:
                task.cancel()
                await silence_speaker()
            end = await vm.next_frame()
            while end.kind == "body_chunk":  # quiet appends, and response.cancel if sent
                assert json.loads(end.value.data)["type"] in (
                    "input_audio_buffer.append", "response.cancel")
                end = await vm.next_frame()
            assert end.kind == "request" and end.value.path == "/voice/end-call"
            await reply_to(vm, end.stream_id, b'{"ok":true,"result":{"ok":true}}', end=True)
            final = await vm.next_frame()
            assert final.stream_id == media and final.value.end_body
            await chunk(vm, media, end=True)
            try:
                await task
            except asyncio.CancelledError:
                pass
            except OSError as exc:
                seen.append(exc)
            assert call.end_call_acknowledged

    for reply_open in (False, True):
        seen.clear()
        caplog.clear()
        asyncio.run(asyncio.wait_for(scenario(reply_open), 5))
        logged = [record for record in caplog.records
                  if record.exc_info and record.exc_info[1] is failure
                  or "OSError" in record.getMessage()]
        assert (len(seen), len(logged)) in [(1, 0), (0, 1)]
        assert all(record.exc_info is None for record in logged)
        assert "synthetic flush failure" not in caplog.text


def test_barge_in_failure_is_logged_when_shutdown_raises_another_error(caplog):
    async def scenario():
        async with connected(register_device=False) as (session, vm, unused):
            output = FlushFailsOnce()
            call = VoiceCall(session, device_id="homelink-synthetic", cleanup_timeout=0.5)
            task = asyncio.create_task(call.run(silent_source(), output))
            media, start = await admit(vm)
            await call.ready.wait()
            output.failure = OSError("synthetic flush failure")
            await call.barge_in()
            output.failure = asyncio.CancelledError()
            await hangup(vm, media, start)
            with pytest.raises(asyncio.CancelledError):
                await task
            assert call.end_call_acknowledged

    asyncio.run(asyncio.wait_for(scenario(), 5))
    assert caplog.messages == ["voice barge-in flush failed (OSError)"]


@pytest.mark.parametrize("callback_failure", [None, ValueError, asyncio.CancelledError])
@pytest.mark.parametrize("path", ["barge_in", "caller_speech", "shutdown"])
def test_audio_discarded_by_a_failing_output_flush_is_still_reported_flushed(path, callback_failure, caplog):
    failure = RuntimeError("synthetic flush failure")
    events = []

    def observe(event):
        events.append(event)
        if callback_failure is not None and isinstance(event, PlaybackFlushed):
            raise callback_failure("synthetic observer failure")

    async def scenario():
        async with connected(register_device=False) as (session, vm, unused):
            output = FlushFailsOnce(failure)
            call = VoiceCall(session, device_id="homelink-synthetic", on_event=observe,
                             cleanup_timeout=0.2)
            task = asyncio.create_task(call.run(silent_source(), output))
            try:
                media, start = await admit(vm)
                await chunk(vm, media, line(created("r1")) + line(audio("p1", TONE * 5)))
                await output.written.wait()
                if path == "barge_in":
                    await call.barge_in()
                    assert await next_event(vm) == {"type": "response.cancel", "response_id": "r1"}
                elif path == "caller_speech":
                    await chunk(vm, media, line(speech("item-2")))
                else:
                    task.cancel()  # shutdown's flush fails once; cleanup's flush succeeds
                await hangup(vm, media, start)
                with pytest.raises(asyncio.CancelledError if path == "shutdown" else RuntimeError) as caught:
                    await task
                if path != "shutdown":
                    assert caught.value is failure
                assert call.end_call_acknowledged
            finally:
                task.cancel()
                await asyncio.gather(task, return_exceptions=True)

    asyncio.run(asyncio.wait_for(scenario(), 5))
    assert events == [
        ResponseStarted("r1"), AudioChunk("r1", 0, TONE * 5), PlaybackFlushed(("r1",)),
    ]
    if callback_failure is not None:
        assert caplog.messages.count(
            "voice playback observer failed (%s)" % callback_failure.__name__) == 1
    assert "synthetic observer failure" not in caplog.text


class DeviceQueue(Output):
    """A sound card's queue: write() says how much is queued, flush() discards
    it and says how much. A failing flush raises before discarding anything."""

    def __init__(self, failure):
        super().__init__()
        self.queued = bytearray()
        self.failure = failure

    def write(self, data):
        super().write(data)
        self.queued.extend(data)
        return len(self.queued)

    def flush(self):
        super().flush()
        failure, self.failure = self.failure, None
        if failure is not None:
            raise failure
        discarded = len(self.queued)
        self.queued.clear()
        return discarded


@pytest.mark.parametrize("path", ["barge_in", "caller_speech", "shutdown"])
def test_queued_audio_kept_through_a_failing_flush_is_reported_when_discarded(path):
    # The failed flush keeps r1 in the output queue until shutdown or cleanup.
    failure = RuntimeError("synthetic flush failure")
    events = []

    async def scenario():
        async with connected(register_device=False) as (session, vm, unused):
            output = DeviceQueue(failure)
            call = VoiceCall(session, device_id="homelink-synthetic", on_event=events.append,
                             cleanup_timeout=0.2)
            task = asyncio.create_task(call.run(silent_source(), output))
            try:
                media, start = await admit(vm)
                await chunk(vm, media, line(created("r1")) + line(audio("p1", TONE)))
                await output.written.wait()
                if path == "barge_in":
                    await call.barge_in()
                    assert await next_event(vm) == {"type": "response.cancel", "response_id": "r1"}
                elif path == "caller_speech":
                    await chunk(vm, media, line(speech("item-2")))
                else:
                    task.cancel()  # shutdown's flush fails; cleanup's flush succeeds
                await hangup(vm, media, start)
                with pytest.raises(asyncio.CancelledError if path == "shutdown" else RuntimeError) as caught:
                    await task
                if path != "shutdown":
                    assert caught.value is failure
                assert call.end_call_acknowledged
                assert output.queued == b""
            finally:
                task.cancel()
                await asyncio.gather(task, return_exceptions=True)

    asyncio.run(asyncio.wait_for(scenario(), 5))
    assert events == [ResponseStarted("r1"), AudioChunk("r1", 0, TONE), PlaybackFlushed(("r1",))]


@pytest.mark.parametrize("words, expected", [
    ([said("Look \ud83d"), said("\ude00")],
     [TranscriptDelta("r1", "Look \ud83d"), TranscriptDelta("r1", "\ude00")]),
    ([transcript("Look \ud83d")], [TranscriptDone("r1", "Look \ud83d")]),
], ids=["split_emoji", "transcript_done"])
def test_held_surrogates_survive_provider_replacement(words, expected):
    events = []

    async def scenario():
        async with connected(register_device=False) as (session, vm, unused):
            output = Output()
            call = VoiceCall(session, device_id="homelink-synthetic", on_event=events.append)
            task = asyncio.create_task(call.run(silent_source(), output))
            media, start = await admit(vm)
            await chunk(vm, media, b"".join(map(line, [
                speech(), created("r1"), audio("p1", TONE), *words,
                audio("p2", HALF), turn(),
            ])), end=True)
            await hangup(vm, media, start, media_ended=True)
            assert await task == "remote_end"
            assert output.pcm == HALF

    asyncio.run(asyncio.wait_for(scenario(), 5))
    assert events == [
        ResponseStarted("r1"), Working(True), *expected, AudioChunk("r1", 0, HALF),
    ]


class DiscardedSamples:
    def __bool__(self):
        raise ValueError("synthetic ambiguous truth value")


class DiscardedCount(int):
    def __bool__(self):
        raise ValueError("synthetic count truth test")


def test_audio_only_call_ignores_output_flush_return_value():
    class SamplesOutput(Output):
        def flush(self):
            super().flush()
            return DiscardedSamples()

    async def scenario():
        async with connected(register_device=False) as (session, vm, unused):
            output = SamplesOutput()
            call = VoiceCall(session, device_id="homelink-synthetic")
            task = asyncio.create_task(call.run(silent_source(), output))
            media, start = await admit(vm)
            await chunk(vm, media, line(audio("p1", TONE)))
            await output.written.wait()
            await chunk(vm, media, b"".join(map(line, [
                speech("item-2"), turn("item-2", "sub-2"), audio("p2", HALF),
            ])), end=True)
            await hangup(vm, media, start, media_ended=True)
            assert await task == "remote_end"
            assert output.pcm == TONE + HALF

    asyncio.run(asyncio.wait_for(scenario(), 5))


@pytest.mark.parametrize("discarded, expected", [
    (DiscardedSamples(), []),
    (True, []),
    (1.5, []),
    (DiscardedCount(len(HALF)), [PlaybackFlushed(("r1",))]),
], ids=["samples", "bool", "float", "integral"])
def test_only_integral_output_flush_counts_report_discarded_audio(discarded, expected):
    events = []

    class BufferedOutput(Output):
        def write(self, data):
            super().write(data)
            return len(data)

        def flush(self):
            super().flush()
            return discarded

    async def scenario():
        async with connected(register_device=False) as (session, vm, unused):
            output = BufferedOutput()
            call = VoiceCall(session, device_id="homelink-synthetic", on_event=events.append)
            task = asyncio.create_task(call.run(silent_source(), output))
            media, start = await admit(vm)
            await chunk(vm, media, line(created("r1")) + line(audio("p1", HALF)))
            await output.written.wait()
            await call.barge_in()
            assert await next_event(vm) == {"type": "response.cancel", "response_id": "r1"}
            await chunk(vm, media, end=True)
            await hangup(vm, media, start, media_ended=True)
            assert await task == "remote_end"
            assert output.pcm == HALF

    asyncio.run(asyncio.wait_for(scenario(), 5))
    assert events == [ResponseStarted("r1"), AudioChunk("r1", 0, HALF), *expected]


@pytest.mark.parametrize("queued, expected", [
    (DiscardedSamples(), []),
    (True, []),
    (1.5, []),
    (DiscardedCount(len(HALF)), [PlaybackFlushed(("r1",))]),
], ids=["samples", "bool", "float", "integral"])
def test_only_integral_output_write_counts_track_queued_audio(queued, expected):
    events = []

    class BufferedOutput(Output):
        def write(self, data):
            super().write(data)
            return queued

        def flush(self):
            super().flush()
            return len(HALF)

    async def scenario():
        async with connected(register_device=False) as (session, vm, unused):
            output = BufferedOutput()
            call = VoiceCall(session, device_id="homelink-synthetic", on_event=events.append)
            task = asyncio.create_task(call.run(silent_source(), output))
            media, start = await admit(vm)
            await chunk(vm, media, line(created("r1")) + line(audio("p1", HALF)))
            await output.written.wait()
            await call.barge_in()
            assert await next_event(vm) == {"type": "response.cancel", "response_id": "r1"}
            await chunk(vm, media, end=True)
            await hangup(vm, media, start, media_ended=True)
            assert await task == "remote_end"
            assert output.pcm == HALF

    asyncio.run(asyncio.wait_for(scenario(), 5))
    assert events == [ResponseStarted("r1"), AudioChunk("r1", 0, HALF), *expected]


@pytest.mark.parametrize("response_id", [None, "r1"], ids=["unnamed", "same_id"])
def test_held_words_of_a_reply_whose_only_audio_was_replaced_are_dropped(response_id):
    async def script(vm, media, call, output, events):
        await chunk(vm, media, b"".join(map(line, [
            speech(), created(response_id), audio("p1", TONE), said("old"),
            created(response_id), audio("p2", HALF), turn(),
        ])))
        await wait_until(lambda: output.pcm == HALF)

    assert run_call(script) == [
        ResponseStarted(response_id), ResponseStarted(response_id), Working(True),
        AudioChunk(response_id, 0, HALF),
    ]


def test_held_words_of_a_reply_without_audio_survive_another_replys_drop():
    async def script(vm, media, call, output, events):
        await chunk(vm, media, b"".join(map(line, [
            speech(), created(None), said("kept"), created(None), audio("p1", TONE),
            created("r2"), audio("p2", HALF), turn(),
        ])))
        await wait_until(lambda: output.pcm == HALF)

    assert run_call(script) == [
        ResponseStarted(None), ResponseStarted(None), ResponseStarted("r2"), Working(True),
        TranscriptDelta(None, "kept"), AudioChunk("r2", 0, HALF),
    ]


def test_a_dropped_streams_trailing_silence_keeps_a_later_replys_held_words():
    async def script(vm, media, call, output, events):
        await chunk(vm, media, b"".join(map(line, [
            speech("item-1"), turn("item-0", "sub-0"), created("r1"), audio("p1", TONE),
            speech("item-2"), created("r2"), said("kept"), audio("p1", b"\0" * len(TONE)),
            created("r3"), audio("p2", TONE), audio("p3", HALF), turn("item-2", "sub-2"),
        ])))
        await wait_until(lambda: output.pcm == HALF)

    assert run_call(script) == [
        Working(True), ResponseStarted("r1"), ResponseStarted("r2"), ResponseStarted("r3"),
        TranscriptDelta("r2", "kept"), AudioChunk("r3", 0, HALF),
    ]


def test_held_words_of_an_unnamed_reply_are_dropped_with_its_replaced_audio():
    async def script(vm, media, call, output, events):
        await chunk(vm, media, b"".join(map(line, [
            speech(), {"type": "response.created"}, audio("p1", TONE), said("old"),
            {"type": "response.created"}, said("new"), audio("p2", HALF), turn(),
        ])))
        await wait_until(lambda: output.pcm == HALF)

    assert run_call(script) == [
        ResponseStarted(None), ResponseStarted(None), Working(True),
        TranscriptDelta(None, "new"), AudioChunk(None, 0, HALF),
    ]
