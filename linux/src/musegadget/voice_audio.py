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

"""WAV-first voice I/O; sounddevice/PortAudio is an optional live adapter."""

from __future__ import annotations

import asyncio
import queue
import sys
import threading
import wave
from collections import deque
from contextlib import ExitStack
from pathlib import Path

from musegadget.voice_call import FRAME_BYTES, FRAME_SAMPLES, SAMPLE_RATE


class WavAudio:
    def __init__(self, input_path, output_path) -> None:
        input_file, output_file = Path(input_path), Path(output_path)
        if (input_file.resolve() == output_file.resolve()
                or (output_file.exists() and input_file.samefile(output_file))):
            raise ValueError("input and output WAV paths must differ")
        self._files = ExitStack()
        try:
            self._input = self._files.enter_context(wave.open(str(input_path), "rb"))
            if (self._input.getnchannels(), self._input.getsampwidth(),
                    self._input.getframerate(), self._input.getcomptype()) != (1, 2, SAMPLE_RATE, "NONE"):
                raise ValueError("input WAV must be uncompressed PCM16LE mono 24 kHz")
            self._output = self._files.enter_context(wave.open(str(output_path), "wb"))
            self._output.setparams((1, 2, SAMPLE_RATE, 0, "NONE", "not compressed"))
        except BaseException:
            self._files.close()
            raise

    async def frames(self):
        while True:
            data = self._input.readframes(FRAME_SAMPLES)
            if not data:
                return
            yield data

    def write(self, pcm: bytes) -> None:
        self._output.writeframesraw(pcm)

    def flush(self) -> None:
        # Already-written samples cannot be unplayed. VoiceCall discards the
        # pending playback queue before calling this method.
        pass

    def close(self) -> None:
        self._files.close()


class LiveAudio:
    """40 ms callback I/O with bounded capture and playback queues.

    Never schedule one asyncio callback per captured frame: that would leave
    an unbounded event-loop queue when it stalls. Capture retains only the
    newest 320 ms; a gated mic is consumed and discarded by VoiceCall.
    """

    def __init__(self) -> None:
        try:
            import sounddevice
        except ImportError as exc:
            raise RuntimeError("live audio requires musegadget[voice] and PortAudio") from exc
        if sys.byteorder != "little":
            raise RuntimeError("live audio requires a little-endian host")
        self._capture = queue.Queue(maxsize=8)
        self._playback = deque()
        self._playback_bytes = 0
        self._playback_lock = threading.Lock()
        self._stream = sounddevice.RawStream(
            samplerate=SAMPLE_RATE, blocksize=FRAME_SAMPLES, channels=1,
            dtype="int16", callback=self._callback,
        )
        try:
            self._stream.start()
        except BaseException:
            self._stream.close()
            raise

    def _callback(self, indata, outdata, frames, timing, status) -> None:
        data = bytes(indata)
        try:
            self._capture.put_nowait(data)
        except queue.Full:
            try:
                self._capture.get_nowait()
            except queue.Empty:
                pass
            try:
                self._capture.put_nowait(data)
            except queue.Full:
                pass
        # The server may emit short PCM fragments. Fill the callback's whole
        # frame across fragment boundaries, retaining any remainder for next
        # time; padding every fragment separately inserts audible gaps.
        outdata[:] = b"\0" * len(outdata)
        offset = 0
        with self._playback_lock:
            while self._playback and offset < len(outdata):
                pcm = self._playback[0]
                count = min(len(pcm), len(outdata) - offset)
                outdata[offset:offset + count] = pcm[:count]
                offset += count
                self._playback_bytes -= count
                if count == len(pcm):
                    self._playback.popleft()
                else:
                    self._playback[0] = pcm[count:]

    async def frames(self):
        # Admission can take seconds: do not replay pre-call capture.
        self._clear(self._capture)
        while True:
            if not self._stream.active:
                raise OSError("audio device stopped")
            try:
                yield self._capture.get_nowait()
            except queue.Empty:
                await asyncio.sleep(0.01)

    def write(self, pcm: bytes) -> None:
        if not pcm or len(pcm) > FRAME_BYTES or len(pcm) % 2:
            raise ValueError("playback needs 1–960 PCM16 samples")
        if not self._stream.active:
            raise OSError("audio device stopped")
        with self._playback_lock:
            if self._playback_bytes + len(pcm) > 8 * FRAME_BYTES:
                raise OSError("audio device playback queue full")
            self._playback.append(pcm)
            self._playback_bytes += len(pcm)

    @staticmethod
    def _clear(q) -> None:
        while True:
            try:
                q.get_nowait()
            except queue.Empty:
                return

    def flush(self) -> None:
        with self._playback_lock:
            self._playback.clear()
            self._playback_bytes = 0

    def close(self) -> None:
        try:
            self._stream.abort()
        finally:
            self._stream.close()
