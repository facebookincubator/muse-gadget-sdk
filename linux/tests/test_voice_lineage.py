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
from musegadget.voice_gate import DROP, PLAY, identifier
from test_streaming import connected, chunk
from test_voice_call import Output, admit, hangup, line
from test_voice_gate import audio, speech, turn, silent_source
from test_voice_lifecycle import wait_until


def settled(submission):
    return {"type": "hatch.voice.turn_settled", "response_message_id": submission}


def accept(call, event):
    if call._track_turn(event):
        call._reply_gate.event(event)
        return True
    return False


def test_settled_submission_cannot_be_relabelled_to_unlock_waiting_pcm():
    async def scenario():
        async with connected(register_device=False) as (session, vm, unused):
            output = Output()
            call = VoiceCall(session, device_id="homelink-synthetic")
            task = asyncio.create_task(call.run(silent_source(), output))
            media, start = await admit(vm)
            await chunk(vm, media, b"".join(map(line, [speech("A"), turn("A", "S"), settled("S"),
                                                       speech("B"), audio("B-provider")])))
            await wait_until(lambda: bool(call._waiting_pcm))
            assert call._settled_turns == {"S": "A"} and not call._reply_gate.confirmed
            await chunk(vm, media, line(turn("B", "S")))
            await hangup(vm, media, start)
            with pytest.raises(ValueError, match="conflicting voice turn identity"):
                await task
            assert not output.pcm and not call._reply_gate.confirmed
            assert call.end_call_acknowledged and not session._requests

    asyncio.run(asyncio.wait_for(scenario(), 5))


def test_valid_settled_replays_still_block_foreign_lineage_and_confirm_current_aliases():
    async def scenario():
        call = VoiceCall(None, device_id="homelink-synthetic")
        gate = call._reply_gate
        gate.event(speech("A"))
        assert accept(call, turn("A", "S-A"))
        assert call._track_turn(settled("S-A"))
        gate.event(speech("B"))
        assert accept(call, turn("B", "S-B"))
        assert call._track_turn(settled("S-B"))
        assert gate.audio(audio("B-reply")) == PLAY
        assert gate.audio(audio("B-continuation")) == PLAY
        assert accept(call, turn("A", "S-A"))  # Valid older replay still informs the gate.
        assert gate.blocked and gate.audio(audio("B-continuation")) == DROP
        assert not call._active_turns
        assert accept(call, turn("B", "S-B"))  # Accepted need not mean newly active.
        assert not gate.blocked and not call._active_turns
        assert gate.audio(audio("B-next")) == PLAY
        assert accept(call, turn("B", "B-alias"))
        assert call._active_turns == {"B-alias": "B"}
        assert accept(call, turn("B", "B-alias"))
        assert call._active_turns == {"B-alias": "B"}

    asyncio.run(scenario())


@pytest.mark.parametrize("field", ["submission_message_id", "item_id"])
@pytest.mark.parametrize("invalid", ["é" * 65, "界" * 43, "embedded\0nul", "\ud800"])
def test_lifecycle_and_gate_share_utf8_byte_and_nul_identity_bounds(field, invalid):
    async def scenario():
        call = VoiceCall(None, device_id="homelink-synthetic")
        event = turn()
        event[field] = invalid
        assert identifier(invalid) is None
        with pytest.raises(ValueError, match="invalid voice turn identity"):
            call._track_turn(event)
        assert not call._active_turns
        assert not call._track_turn(settled(invalid))

    asyncio.run(scenario())


@pytest.mark.parametrize("valid", ["é" * 64, "界" * 42, "a" * 128])
def test_valid_multibyte_identities_and_replays_keep_the_same_lineage(valid):
    async def scenario():
        call = VoiceCall(None, device_id="homelink-synthetic")
        assert identifier(valid) == valid
        call._reply_gate.event(speech(valid))
        assert accept(call, turn(valid, valid))
        assert call._reply_gate.confirmed
        assert call._track_turn(settled(valid))
        assert call._settled_turns == {valid: valid}
        assert accept(call, turn(valid, valid)) and not call._active_turns

    asyncio.run(scenario())


def test_more_than_64_settled_aliases_preserve_lineage():
    async def scenario():
        call = VoiceCall(None, device_id="homelink-synthetic")
        for index in range(65):
            assert call._track_turn(turn("item", str(index)))
            assert call._track_turn(settled(str(index)))
        assert len(call._settled_turns) == 65
        assert not call._active_turns
        assert call._track_turn(turn("item", "0"))
        with pytest.raises(ValueError, match="conflicting voice turn identity"):
            call._track_turn(turn("new-item", "0"))

    asyncio.run(scenario())


def test_settled_lineage_bound_fails_closed_without_forgetting_ids(monkeypatch):
    async def scenario():
        monkeypatch.setattr("musegadget.voice_call.MAX_SETTLED_TURNS", 2)
        call = VoiceCall(None, device_id="homelink-synthetic")
        for index in range(2):
            assert call._track_turn(turn("item", str(index)))
            assert call._track_turn(settled(str(index)))
        assert call._track_turn(turn("item", "0"))  # Valid replay consumes no slot.
        assert call._track_turn(turn("item", "overflow"))
        with pytest.raises(ValueError, match="too many settled voice turns"):
            call._track_turn(settled("overflow"))
        assert call._settled_turns == {"0": "item", "1": "item"}
        with pytest.raises(ValueError, match="conflicting voice turn identity"):
            call._track_turn(turn("new-item", "0"))

    asyncio.run(scenario())
