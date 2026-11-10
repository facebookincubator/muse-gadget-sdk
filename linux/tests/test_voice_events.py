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
import wave
from types import SimpleNamespace

import pytest

from musegadget import voice_call as voice
from musegadget.voice_audio import WavAudio
from musegadget.voice_call import (AudioChunk, PlaybackFlushed, ResponseDone, ResponseStarted,
                                   TranscriptDelta, TranscriptDone, VoiceCall, Working)
from test_streaming import chunk, connected
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
