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
import math
import struct

import pytest

from musegadget import wakeword_input as wake
from musegadget.voice_call import FRAME_BYTES


class Detector:
    def __init__(self, fire_at=None):
        self.fire_at = fire_at
        self.received = []
        self.resets = 0

    def reset(self):
        self.resets += 1

    def feed(self, pcm):
        self.received.append(pcm)
        return len(self.received) == self.fire_at


async def source(frames):
    for frame in frames:
        yield frame


def tone(frequency, samples=4800):
    return struct.pack(
        "<" + "h" * samples,
        *(
            round(10000 * math.sin(2 * math.pi * frequency * n / 24000))
            for n in range(samples)
        )
    )


def rms(pcm):
    samples = [sample for (sample,) in struct.iter_unpack("<h", pcm)]
    return math.sqrt(sum(sample * sample for sample in samples) / len(samples))


def test_resampling_rate_chunk_parity_and_reset():
    pcm = tone(1000, 4801)
    expected = wake.Pcm24To16().feed(pcm)
    resampler = wake.Pcm24To16()
    actual = b"".join(resampler.feed(pcm[n : n + 14]) for n in range(0, len(pcm), 14))
    assert actual == expected
    assert len(expected) == 3201 * 2
    resampler.reset()
    assert resampler.feed(pcm) == expected
    with pytest.raises(ValueError):
        resampler.feed(b"\0")


def test_resampling_preserves_speech_band_and_filters_aliasing():
    low = wake.Pcm24To16().feed(tone(1000))[256:]
    high = wake.Pcm24To16().feed(tone(10000))[256:]
    assert 6800 < rms(low) < 7300
    assert rms(high) < rms(low) * 0.01


def test_no_audio_is_admitted_before_detection_and_preroll_is_original_pcm():
    async def scenario():
        feed = asyncio.Queue()
        detector = Detector(fire_at=30)

        async def microphone():
            while True:
                yield await feed.get()

        listener = wake.WakeWordInput(microphone(), detector)
        waiting = asyncio.create_task(listener.wait())
        try:
            frames = [struct.pack("<h", n) * (FRAME_BYTES // 2) for n in range(35)]
            for frame in frames[:29]:
                await feed.put(frame)
            while len(detector.received) < 29:
                await asyncio.sleep(0)
            assert not waiting.done() and not listener._queue
            await feed.put(frames[29])
            assert await waiting
            assert detector.resets == 1
            for frame in frames[30:]:
                await feed.put(frame)
            while listener._buffer_bytes < 30 * FRAME_BYTES:
                await asyncio.sleep(0)
            admitted = listener.frames()
            actual = [await admitted.__anext__() for _ in range(30)]
            assert actual == frames[5:]
            assert len(detector.received) == 30  # detection paused for the whole call
            assert all(len(pcm) == 1280 for pcm in detector.received)
            await admitted.aclose()
        finally:
            await listener.aclose()
        assert listener._task.done()

    asyncio.run(scenario())


def test_eof_without_wake_and_malformed_capture_fail_closed():
    async def scenario():
        listener = wake.WakeWordInput(source([b"\0" * FRAME_BYTES]), Detector())
        try:
            assert not await listener.wait()
            assert not listener._queue
        finally:
            await listener.aclose()
        for bad in (b"", b"\0", b"\0" * (FRAME_BYTES + 2)):
            listener = wake.WakeWordInput(source([bad]), Detector())
            try:
                with pytest.raises(ValueError):
                    await listener.wait()
            finally:
                await listener.aclose()

    asyncio.run(scenario())


def test_detector_failure_does_not_admit_a_call():
    class Broken(Detector):
        def feed(self, pcm):
            raise RuntimeError("synthetic detector error")

    async def scenario():
        listener = wake.WakeWordInput(source([b"\0" * FRAME_BYTES]), Broken())
        try:
            with pytest.raises(RuntimeError):
                await listener.wait()
            assert not listener._queue
        finally:
            await listener.aclose()

    asyncio.run(scenario())


def test_live_startup_backlog_is_bounded_and_wav_backpressure_preserves_audio():
    async def scenario():
        frames = [b"\0" * FRAME_BYTES] * 140
        listener = wake.WakeWordInput(source(frames), Detector(fire_at=1))
        try:
            assert await listener.wait()
            await listener._task
            with pytest.raises(BufferError):
                await listener.frames().__anext__()
            assert listener._buffer_bytes <= wake.MAX_BUFFER_BYTES
        finally:
            await listener.aclose()
        listener = wake.WakeWordInput(
            source(frames), Detector(fire_at=1), realtime=False
        )
        try:
            assert await listener.wait()
            actual = [frame async for frame in listener.frames()]
            assert actual == frames
        finally:
            await listener.aclose()

    asyncio.run(scenario())


def test_cancellation_closes_waiting_capture():
    async def scenario():
        closed = asyncio.Event()

        async def microphone():
            try:
                await asyncio.Event().wait()
                yield b"\0" * FRAME_BYTES
            finally:
                closed.set()

        listener = wake.WakeWordInput(microphone(), Detector())
        waiting = asyncio.create_task(listener.wait())
        await asyncio.sleep(0.01)
        waiting.cancel()
        with pytest.raises(asyncio.CancelledError):
            await waiting
        await listener.aclose()
        assert closed.is_set() and listener._task.done()
        assert not listener._queue and not listener._preroll

    asyncio.run(scenario())
