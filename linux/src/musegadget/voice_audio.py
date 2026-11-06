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
import math
import queue
import struct
import sys
import threading
import wave
from collections import deque
from contextlib import ExitStack
from operator import mul
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


# PortAudio opens hardware devices directly and never resamples, so the
# capture stream must run at a rate the microphone actually offers.
# Prefer the call's native 24 kHz; fall back to 48 kHz with software
# resampling on microphones with no 24 kHz mode.
_RESAMPLED_RATE = 48000


def _choose_capture_rate(sounddevice):
    """Pick the first candidate rate the default microphone supports.

    Probes with Pa_IsFormatSupported via sounddevice. Any probe failure
    (unsupported rate, missing device) rejects that rate. Returns None if
    no candidate passes.
    """
    for rate in (SAMPLE_RATE, _RESAMPLED_RATE):
        try:
            sounddevice.check_input_settings(
                device=None, channels=1, dtype="int16", samplerate=rate)
        except (sounddevice.PortAudioError, ValueError):
            continue
        return rate
    return None


def _lowpass_taps(count, cutoff):
    """Hann-windowed sinc low-pass; cutoff as a fraction of input Nyquist."""
    taps = []
    mid = (count - 1) / 2
    for k in range(count):
        offset = k - mid
        sinc = 1.0 if offset == 0 else math.sin(math.pi * cutoff * offset) / (math.pi * cutoff * offset)
        taps.append(sinc * (0.5 - 0.5 * math.cos(2 * math.pi * k / (count - 1))))
    total = sum(taps)
    return tuple(tap / total for tap in taps)


def _clamp16(value):
    return max(-32768, min(32767, value))


class Pcm48To24:
    """Streaming 2:1 FIR decimator; independent of input chunk boundaries."""

    def __init__(self):
        # 64 taps, cutoff 10.8 kHz (0.45 of the 24 kHz input Nyquist), just
        # below the 12 kHz Nyquist of the 24 kHz output. Reversed so each
        # oldest-first window lines up with taps[0] on its newest sample.
        self._taps = _lowpass_taps(64, 0.45)[::-1]
        self._history = [0] * 63  # previous 63 input samples, oldest first
        self._phase = 1  # every second input sample completes an output

    def feed(self, pcm: bytes) -> bytes:
        if len(pcm) % 2:
            raise ValueError("48 kHz audio must contain whole PCM16LE samples")
        # This runs in the audio callback: map/sum over slices keeps the
        # multiply-accumulate loop in C, about half the cost of a Python
        # generator per output sample.
        samples = self._history + list(struct.unpack(f"<{len(pcm) // 2}h", pcm))
        taps = self._taps
        output = [_clamp16(round(sum(map(mul, taps, samples[i:i + 64]))))
                  for i in range(self._phase, len(samples) - 63, 2)]
        self._phase = (self._phase - (len(samples) - 63)) % 2
        self._history = samples[-63:]
        return struct.pack(f"<{len(output)}h", *output)


class LiveAudio:
    """40 ms callback I/O with bounded capture and playback queues.

    Never schedule one asyncio callback per captured frame: that would leave
    an unbounded event-loop queue when it stalls. Capture retains only the
    newest 320 ms; a gated mic is consumed and discarded by VoiceCall.

    Native 24 kHz capture and playback share a duplex stream. Otherwise,
    capture runs at 48 kHz with FIR decimation and playback uses a separate
    24 kHz stream without resampling. frames() and write() always speak 24 kHz.
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
        rate = _choose_capture_rate(sounddevice)
        if rate is None:
            device = sounddevice.query_devices(kind="input")
            raise OSError(
                f"microphone {device['name']!r} supports neither 24 kHz nor 48 kHz "
                f"(native rate: {device['default_samplerate']:.0f} Hz)"
            )
        self._decimator = Pcm48To24() if rate != SAMPLE_RATE else None
        self._streams = ExitStack()
        try:
            if rate == SAMPLE_RATE:
                self._input_stream = sounddevice.RawStream(
                    samplerate=SAMPLE_RATE, blocksize=FRAME_SAMPLES, channels=1,
                    dtype="int16", callback=self._callback,
                )
                self._streams.callback(self._input_stream.close)
                self._output_stream = self._input_stream
            else:
                self._input_stream = sounddevice.RawInputStream(
                    samplerate=rate, blocksize=FRAME_SAMPLES * rate // SAMPLE_RATE, channels=1,
                    dtype="int16", callback=self._capture_callback,
                )
                self._streams.callback(self._input_stream.close)
                self._output_stream = sounddevice.RawOutputStream(
                    samplerate=SAMPLE_RATE, blocksize=FRAME_SAMPLES, channels=1,
                    dtype="int16", callback=self._playback_callback,
                )
                self._streams.callback(self._output_stream.close)
            self._input_stream.start()
            if self._output_stream is not self._input_stream:
                self._output_stream.start()
        except BaseException:
            self._streams.close()
            raise

    def _callback(self, indata, outdata, frames, timing, status) -> None:
        self._capture_callback(indata, frames, timing, status)
        self._playback_callback(outdata, frames, timing, status)

    def _capture_callback(self, indata, frames, timing, status) -> None:
        data = bytes(indata)
        if self._decimator is not None:
            data = self._decimator.feed(data)
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

    def _playback_callback(self, outdata, frames, timing, status) -> None:
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
            if not self._input_stream.active or not self._output_stream.active:
                raise OSError("audio device stopped")
            try:
                yield self._capture.get_nowait()
            except queue.Empty:
                await asyncio.sleep(0.01)

    def write(self, pcm: bytes) -> None:
        if not pcm or len(pcm) > FRAME_BYTES or len(pcm) % 2:
            raise ValueError("playback needs 1–960 PCM16 samples")
        if not self._output_stream.active:
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
            try:
                self._input_stream.abort()
            finally:
                if self._output_stream is not self._input_stream:
                    self._output_stream.abort()
        finally:
            self._streams.close()
