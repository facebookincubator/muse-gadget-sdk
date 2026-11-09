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

import pytest

from test_streaming import connected, response


@pytest.mark.parametrize("send, expected", [
    (lambda session, options: session.send_chat("Hello", "side-chat", **options),
     {"message": "Hello", "device_id": "homelink-abcdef", "session_id": "side-chat",
      "output_modality": "text"}),
    (lambda session, options: session.send_voice(
        b"WAV", "side-chat", message="Describe the sound.", output_modality="text", **options),
     {"message": "Describe the sound.", "device_id": "homelink-abcdef",
      "session_id": "side-chat", "output_modality": "text",
      "items": [{"type": "text", "text": "Describe the sound."},
                {"type": "file", "mime_type": "audio/wav", "filename": "voice_note.wav",
                 "data_base64": "V0FW"}]}),
], ids=["dictation", "voice_note"])
def test_spoken_device_hint_applies_only_to_the_requested_turn(send, expected):
    async def scenario():
        async with connected() as (session, vm, unused):
            for options, body in [
                ({"device": "mcu-wearable"}, {**expected, "device": "mcu-wearable"}),
                ({}, expected),
                ({"device": None}, expected),
            ]:
                reply = asyncio.create_task(send(session, options))
                request = await vm.next_frame()
                assert (request.kind, request.value.verb, request.value.path,
                        request.value.end_body) == ("request", "POST", "/chat/stream", True)
                assert json.loads(request.value.body) == body
                await response(vm, request.stream_id, b'{"accepted":true}', end=True)
                assert await reply == {
                    "ok": True, "status": 200, "response": {"accepted": True}}

    asyncio.run(asyncio.wait_for(scenario(), 5))
