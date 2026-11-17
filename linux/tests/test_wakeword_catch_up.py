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

"""A wake-started call catches up on the capture queued before it was ready."""

import asyncio
import base64
import json
import struct

import pytest

from musegadget import wakeword_input as wake
from musegadget.voice_call import FRAME_BYTES, VoiceCall
from test_streaming import connected
from test_voice_call import Output, admit


class Detector:
    def reset(self):
        pass

    def feed(self, pcm):
        return True  # wake on the first frame


@pytest.mark.parametrize("half_duplex", [True, False])
def test_backlog_after_a_wake_drains_in_order(half_duplex):
    async def scenario():
        # Distinct, speech-loud frames so the order and content can be checked.
        frames = [struct.pack("<h", 1000 + n) * (FRAME_BYTES // 2) for n in range(150)]

        async def microphone():
            # Two seconds arrive before the call can take any, then capture
            # continues at real time.
            for index, frame in enumerate(frames):
                if index >= 50:
                    await asyncio.sleep(0.04)
                yield frame

        listener = wake.WakeWordInput(microphone(), Detector())
        assert await listener.wait()
        while listener.backlog_bytes < 50 * FRAME_BYTES:
            await asyncio.sleep(0.005)
        sent = []
        try:
            async with connected(register_device=False) as (session, vm, unused):
                call = VoiceCall(session, device_id="homelink-synthetic",
                                 half_duplex=half_duplex, cleanup_timeout=0.2)
                task = asyncio.create_task(call.run(listener, Output(), realtime_input=True))
                media, start = await admit(vm)

                async def read():
                    while True:
                        frame = await vm.next_frame()
                        if frame.kind != "body_chunk" or frame.stream_id != media:
                            continue
                        event = json.loads(frame.value.data)
                        if event.get("type") == "input_audio_buffer.append":
                            sent.append(base64.b64decode(event["audio"]))

                reader = asyncio.create_task(read())
                try:
                    # Two seconds queued, sent at three times real time while
                    # one more frame arrives every 40 ms: drained in about 1 s.
                    await asyncio.sleep(2.0)
                    assert listener.backlog_bytes <= FRAME_BYTES
                    # Nothing queued was dropped or reordered.
                    assert sent == frames[:len(sent)] and len(sent) >= 60
                finally:
                    task.cancel()
                    await asyncio.gather(task, return_exceptions=True)
                    reader.cancel()
                    await asyncio.gather(reader, return_exceptions=True)
        finally:
            await listener.aclose()

    asyncio.run(scenario())
