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

import pytest

from musegadget.voice_call import VoiceCall
from musegadget.voice_gate import DROP, HOLD, PLAY, MAX_IDS, ReplyGate
from test_streaming import connected, chunk
from test_voice_call import Output, TONE, admit, cavd, hangup, line, next_event
from test_voice_lifecycle import wait_until


def audio(response_id, pcm=TONE):
    return dict(cavd(pcm), response_id=response_id)


def speech(item="current"):
    return {"type": "input_audio_buffer.speech_started", "item_id": item}


def turn(item="current", submission="turn"):
    return {"type": "hatch.voice.turn_started", "item_id": item, "submission_message_id": submission}


def test_interrupted_ids_never_reopen_and_pending_prefix_is_confirmed():
    gate = ReplyGate()
    assert gate.audio(audio("old")) == PLAY
    gate.interrupt()
    assert gate.audio(audio("old")) == DROP
    assert gate.audio(audio("before-speech")) == DROP
    gate.event(speech())
    assert gate.audio(audio("pending")) == HOLD
    gate.event(turn())
    assert gate.audio(audio("pending")) == PLAY
    assert gate.audio(audio("old")) == DROP
    assert gate.audio(audio("before-speech")) == DROP


def test_continuation_and_foreign_lineage_are_separated():
    gate = ReplyGate()
    gate.event(speech())
    gate.event(turn())
    assert gate.audio(audio("own")) == PLAY
    assert gate.audio(audio("continuation")) == PLAY  # An alias is not required.
    assert gate.audio(audio("own")) == DROP
    gate.take_flush()
    gate.event(turn("older", "late"))
    assert gate.blocked and gate.take_flush()[0]
    assert gate.audio(audio("continuation")) == DROP
    assert gate.audio(audio("foreign")) == DROP
    gate.event(turn("current", "alias"))
    assert gate.audio(audio("fresh")) == PLAY
    assert gate.audio(audio("foreign")) == DROP


def test_foreign_turn_drops_unconfirmed_prefix_but_not_proven_current_reply():
    gate = ReplyGate()
    gate.event(speech())
    assert gate.audio(audio("pending")) == HOLD
    gate.event(turn("older", "late"))
    assert not gate.waiting and gate.audio(audio("pending")) == DROP
    gate.event(turn())
    assert gate.audio(audio("current")) == PLAY
    gate.event(turn("older", "another-late"))
    assert gate.audio(audio("current")) == PLAY
    assert gate.audio(audio("new-unattributed")) == DROP


@pytest.mark.parametrize("missing", [None, "", "x" * 129])
def test_missing_lineage_and_provider_identity_cannot_unmute(missing):
    gate = ReplyGate()
    gate.interrupt()
    gate.event(speech(missing))
    gate.event(turn())
    assert gate.audio(audio("unattributed")) == DROP
    gate.event(speech())
    gate.event(turn())
    assert gate.audio(audio(missing)) == DROP
    assert gate.audio({"type": "response.audio.delta", "response_id": "mapper"}) == DROP


def test_old_speech_start_and_input_suppression_cannot_reselect_old_caller():
    gate = ReplyGate()
    gate.event(speech("old"))
    gate.event(speech("current"))
    gate.event(speech("old"))
    assert gate.item == "current"
    gate.event(turn())
    assert gate.audio(audio("own")) == PLAY
    gate.event({"type": "hatch.voice.input_suppressed", "item_id": "current"})
    assert not gate.item and gate.audio(audio("own")) == DROP


def test_ids_are_bounded_for_whole_call_without_eviction():
    gate = ReplyGate()
    for index in range(MAX_IDS):
        gate.audio(audio(str(index)))
        gate.interrupt()
    assert len(gate.ids) == MAX_IDS and gate.audio(audio("0")) == DROP
    with pytest.raises(ValueError, match="provider response identities"):
        gate.audio(audio("overflow"))
    for index in range(MAX_IDS):
        gate.event(speech(str(index)))
    with pytest.raises(ValueError, match="caller input identities"):
        gate.event(speech("overflow"))


def test_conflicting_active_turn_identity_is_rejected_before_gate_admission():
    async def scenario():
        call = VoiceCall(None, device_id="homelink-synthetic")
        call._track_turn(turn("old", "same-submission"))
        with pytest.raises(ValueError, match="conflicting voice turn identity"):
            call._track_turn(turn("current", "same-submission"))

    asyncio.run(scenario())


def test_mapper_response_tracking_is_separate_and_only_matching_done_closes_it():
    gate = ReplyGate()
    gate.audio(audio("provider"))
    gate.event({"type": "response.created", "response": {"id": "mapper"}})
    gate.event({"type": "response.done", "response": {"id": "older-mapper"}})
    assert gate.open_response == "mapper"
    gate.event({"type": "response.done", "response": {"id": "mapper"}})
    assert gate.open_response is None and gate.audio(audio("provider")) == PLAY


@pytest.mark.parametrize("completed", [False, True])
def test_barge_in_does_not_cancel_unknown_or_completed_mapper_response(completed):
    async def scenario():
        call = VoiceCall(None, device_id="homelink-synthetic")
        call._output = Output()
        sent = []

        async def observe(event):
            sent.append(event)

        call._event = observe
        if completed:
            call._reply_gate.event({"type": "response.created", "response": {"id": "mapper"}})
            call._reply_gate.event({"type": "response.done", "response": {"id": "mapper"}})
        await call.barge_in()
        assert not sent and call._reply_gate.fenced

    asyncio.run(scenario())


async def silent_source():
    await asyncio.Future()
    yield TONE


@pytest.mark.parametrize("interruption", ["local", "server"])
def test_real_call_drops_trailing_provider_audio_and_keeps_next_reply_prefix(interruption):
    async def scenario():
        async with connected(register_device=False) as (session, vm, unused):
            output = Output()
            call = VoiceCall(session, device_id="homelink-synthetic")
            task = asyncio.create_task(call.run(silent_source(), output))
            media, start = await admit(vm)
            await chunk(vm, media, line({"type": "response.created", "response": {"id": "mapper-old"}})
                        + line(audio("provider-old", TONE * 4)))
            await output.written.wait()
            if interruption == "local":
                await call.barge_in()
                assert await next_event(vm) == {"type": "response.cancel", "response_id": "mapper-old"}
            else:
                await chunk(vm, media, line(speech(None)))
                await wait_until(lambda: call._reply_gate.fenced)
            before = bytes(output.pcm)
            await chunk(vm, media, line(audio("provider-old")) + line(audio("unattributed")))
            await wait_until(lambda: "unattributed" in call._reply_gate.ids)
            await asyncio.sleep(0.25)  # No timer-based unmute after a silent interruption.
            assert bytes(output.pcm) == before and not call._playback
            fresh = b"\x10\x04" * 960
            await chunk(vm, media, line(speech()) + line(audio("new-provider", fresh)))
            await wait_until(lambda: bool(call._waiting_pcm))
            assert bytes(output.pcm) == before
            await chunk(vm, media, line(turn()))
            await wait_until(lambda: len(output.pcm) > len(before))
            assert bytes(output.pcm[len(before):]) == fresh
            await chunk(vm, media, line(audio("provider-old")) + line(audio("continuation", fresh)))
            await wait_until(lambda: len(output.pcm) == len(before) + len(fresh) * 2)
            task.cancel()
            await hangup(vm, media, start)
            with pytest.raises(asyncio.CancelledError):
                await task

    asyncio.run(asyncio.wait_for(scenario(), 5))


@pytest.mark.parametrize("overflow", ["waiting_audio", "provider_ids"])
def test_reply_gate_overflow_explicitly_ends_call(monkeypatch, overflow):
    async def scenario():
        async with connected(register_device=False) as (session, vm, unused):
            call = VoiceCall(session, device_id="homelink-synthetic")
            task = asyncio.create_task(call.run(silent_source(), Output()))
            media, start = await admit(vm)
            if overflow == "waiting_audio":
                monkeypatch.setattr("musegadget.voice_call.MAX_PLAYBACK_BYTES", len(TONE))
                events = [speech(), audio("pending"), audio("pending")]
            else:
                monkeypatch.setattr("musegadget.voice_gate.MAX_IDS", 2)
                events = [speech(), turn(), audio("one"), audio("two"), audio("three")]
            await chunk(vm, media, b"".join(map(line, events)))
            await hangup(vm, media, start)
            with pytest.raises(ValueError):
                await task
            assert call.end_call_acknowledged and not session._requests

    asyncio.run(asyncio.wait_for(scenario(), 5))
