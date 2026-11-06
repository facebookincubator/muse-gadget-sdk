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

"""Local wake-word admission in front of the existing 24 kHz voice call.

Only the detector sees waiting audio. The voice call gets bounded pre-roll and
subsequent capture, at the original sample rate, after a detection. The detector
is paused for the entire call, including playback and remote input holds.
"""

from __future__ import annotations

import asyncio
import math
import struct
from collections import deque

from musegadget.voice_call import FRAME_BYTES, SAMPLE_RATE

PREROLL_BYTES = SAMPLE_RATE * 2
MAX_BUFFER_BYTES = SAMPLE_RATE * 2 * 4


class Pcm24To16:
    """Streaming 2:3 FIR resampler; independent of input chunk boundaries."""

    def __init__(self):
        self._filters = []
        # 63-tap Hann-windowed sinc, low-pass at 7.2 kHz before downsampling.
        for fraction in (0.0, 0.5):
            taps = []
            for k in range(63):
                offset = k + fraction - 31
                sinc = (
                    0.6
                    if offset == 0
                    else math.sin(math.pi * 0.6 * offset) / (math.pi * offset)
                )
                taps.append(sinc * (0.5 - 0.5 * math.cos(2 * math.pi * k / 62)))
            total = sum(taps)
            self._filters.append(tuple(tap / total for tap in taps))
        self.reset()

    def reset(self):
        self._history = deque([0] * 63, maxlen=63)
        self._input_index = -1
        self._output_index = 0

    def feed(self, pcm: bytes) -> bytes:
        if len(pcm) % 2:
            raise ValueError("wake-word audio must contain whole PCM16LE samples")
        output = []
        for (sample,) in struct.iter_unpack("<h", pcm):
            self._input_index += 1
            self._history.appendleft(sample)
            if self._input_index == self._output_index * 3 // 2:
                taps = self._filters[self._output_index % 2]
                value = round(
                    sum(sample * tap for sample, tap in zip(self._history, taps))
                )
                output.append(max(-32768, min(32767, value)))
                self._output_index += 1
        return struct.pack("<" + "h" * len(output), *output)


class WakeWordInput:
    """Own one capture iterator and a detector for local activation of one call.

    The caller owns the detector and audio device. Always await ``aclose`` before
    starting another listener. EOF without a wake returns False from ``wait``;
    detector/capture failures propagate rather than admitting an unguarded call.
    """

    def __init__(self, source, detector, *, realtime: bool = True):
        self._source = source
        self._detector = detector
        self._realtime = realtime
        self._room = asyncio.Event()
        self._room.set()
        self._resampler = Pcm24To16()
        self._wake = asyncio.Event()
        self._changed = asyncio.Event()
        self._queue = deque()
        self._buffer_bytes = 0
        self._preroll = deque()
        self._preroll_bytes = 0
        self._active = False
        self._eof = False
        self._error = None
        self._task = None

    async def wait(self) -> bool:
        if self._task is not None:
            raise ValueError("a wake-word listener can only be started once")
        self._detector.reset()
        self._task = asyncio.create_task(self._capture())
        await self._wake.wait()
        if self._error is not None:
            raise self._error
        return self._active

    def _enqueue(self, pcm):
        if self._buffer_bytes + len(pcm) > MAX_BUFFER_BYTES:
            raise BufferError(
                "wake-word capture exceeded the four-second admission buffer"
            )
        self._queue.append(pcm)
        self._buffer_bytes += len(pcm)
        self._changed.set()

    async def _capture(self):
        try:
            async for pcm in self._source:
                if not pcm or len(pcm) > FRAME_BYTES or len(pcm) % 2:
                    raise ValueError(
                        "input must contain 1–960 PCM16LE samples per frame"
                    )
                if self._active:
                    while (
                        not self._realtime
                        and self._buffer_bytes + len(pcm) > MAX_BUFFER_BYTES
                    ):
                        self._room.clear()
                        await self._room.wait()
                    self._enqueue(pcm)
                else:
                    self._preroll.append(pcm)
                    self._preroll_bytes += len(pcm)
                    while self._preroll_bytes > PREROLL_BYTES:
                        excess = self._preroll_bytes - PREROLL_BYTES
                        first = self._preroll.popleft()
                        if len(first) > excess:
                            self._preroll.appendleft(first[excess:])
                            self._preroll_bytes -= excess
                        else:
                            self._preroll_bytes -= len(first)
                    detected = self._detector.feed(self._resampler.feed(pcm))
                    if detected:
                        self._active = True
                        for frame in self._preroll:
                            self._enqueue(frame)
                        self._preroll.clear()
                        self._preroll_bytes = 0
                        self._wake.set()
                # WAV/fake sources need not suspend; allow admission/consumption
                # to run without buffering the entire file in one loop turn.
                await asyncio.sleep(0)
        except asyncio.CancelledError:
            raise
        except Exception as exc:
            self._error = exc
        finally:
            self._eof = True
            self._changed.set()
            self._wake.set()

    async def frames(self):
        while True:
            if self._error is not None:
                raise self._error
            if self._queue:
                pcm = self._queue.popleft()
                self._buffer_bytes -= len(pcm)
                self._room.set()
                yield pcm
            elif self._eof:
                return
            else:
                self._changed.clear()
                await self._changed.wait()

    async def aclose(self):
        async def finish():
            if self._task is not None:
                self._task.cancel()
                await asyncio.gather(self._task, return_exceptions=True)
            if hasattr(self._source, "aclose"):
                await self._source.aclose()

        cleanup = asyncio.create_task(finish())
        deadline = asyncio.create_task(asyncio.wait({cleanup}, timeout=1))
        cancelled = False
        try:
            while not deadline.done():
                try:
                    await asyncio.shield(deadline)
                except asyncio.CancelledError:
                    cancelled = True
            done, pending = deadline.result()
            if pending:
                cleanup.cancel()
                cleanup.add_done_callback(
                    lambda task: None if task.cancelled() else task.exception()
                )
                raise asyncio.TimeoutError("wake-word capture shutdown timed out")
            cleanup.result()
            if cancelled:
                raise asyncio.CancelledError
        finally:
            self._queue.clear()
            self._preroll.clear()
            self._buffer_bytes = self._preroll_bytes = 0
