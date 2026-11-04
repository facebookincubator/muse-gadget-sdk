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

import math
import struct
import sys
from types import SimpleNamespace

import pytest

from musegadget.voice_audio import LiveAudio, Pcm48To24, _lowpass_taps
from musegadget.voice_call import FRAME_BYTES


class FakeStream:
    def __init__(self, **options):
        self.options = options
        self.active = False
        self.closed = False
        self.calls = []

    def start(self):
        self.calls.append("start")
        self.active = True

    def abort(self):
        self.calls.append("abort")
        self.active = False

    def close(self):
        self.calls.append("close")
        self.active = False
        self.closed = True


class PortAudioError(Exception):
    pass


def open_audio(monkeypatch, input_rates, output_rates):
    def checker(rates):
        def check(device, channels, dtype, samplerate):
            if samplerate not in rates:
                raise PortAudioError("Invalid sample rate")
        return check

    def output_stream(**options):
        checker(output_rates)(None, options["channels"], options["dtype"], options["samplerate"])
        return FakeStream(**options)

    monkeypatch.setitem(sys.modules, "sounddevice", SimpleNamespace(
        RawStream=output_stream, RawInputStream=FakeStream, RawOutputStream=output_stream,
        PortAudioError=PortAudioError, check_input_settings=checker(input_rates),
        query_devices=lambda kind: {"name": "USB Mic", "default_samplerate": 44100.0}))
    return LiveAudio()


@pytest.fixture(params=[(24000, 48000), (48000, 44100)])
def audio(monkeypatch, request):
    result = open_audio(monkeypatch, request.param, (24000,))
    try:
        yield result
    finally:
        result.close()


@pytest.fixture
def resampled_audio(monkeypatch):
    result = open_audio(monkeypatch, (48000, 44100), (24000,))
    try:
        yield result
    finally:
        result.close()


def playback(audio):
    output = bytearray(FRAME_BYTES)
    callback = audio._output_stream.options["callback"]
    if audio._input_stream is audio._output_stream:
        callback(b"\0" * FRAME_BYTES, output, 960, None, None)
    else:
        callback(output, 960, None, None)
    return bytes(output)


def test_capture_prefers_native_24k(monkeypatch):
    monkeypatch.setitem(sys.modules, "sounddevice", SimpleNamespace(
        RawStream=FakeStream, PortAudioError=PortAudioError,
        check_input_settings=lambda **options: None))
    audio = LiveAudio()
    try:
        assert audio._input_stream is audio._output_stream
        assert audio._input_stream.options["samplerate"] == 24000
        assert audio._input_stream.options["blocksize"] == 960
        assert audio._input_stream.calls == ["start"]
        pcm = struct.pack("<hhhh", 32767, -32768, 1000, -1000) * 240
        output = bytearray(FRAME_BYTES)
        audio._input_stream.options["callback"](pcm, output, 960, None, None)
        assert audio._capture.get_nowait() == pcm
        assert output == b"\0" * FRAME_BYTES
    finally:
        audio.close()
    assert audio._input_stream.calls == ["start", "abort", "close"]


def test_capture_falls_back_to_48k_independently_of_playback(resampled_audio):
    assert resampled_audio._input_stream.options["samplerate"] == 48000
    assert resampled_audio._input_stream.options["blocksize"] == 1920
    assert resampled_audio._output_stream.options["samplerate"] == 24000
    assert resampled_audio._output_stream.options["blocksize"] == 960


def test_capture_rejects_microphone_without_24k_or_48k(monkeypatch):
    with pytest.raises(OSError, match=r"'USB Mic' supports neither 24 kHz nor 48 kHz \(native rate: 44100 Hz\)"):
        open_audio(monkeypatch, (44100,), (24000,))


def test_resampled_capture_yields_24k_frames(resampled_audio):
    tone = struct.pack("<h", 1000) * 1920
    for _ in range(2):
        resampled_audio._input_stream.options["callback"](tone, 1920, None, None)
    resampled_audio._capture.get_nowait()
    frame = resampled_audio._capture.get_nowait()
    assert frame == struct.pack("<h", 1000) * 960  # unity DC gain once settled


def test_decimator_ignores_chunk_boundaries():
    pcm = b"".join(struct.pack("<h", (i * 997) % 20000 - 10000) for i in range(4000))
    whole = Pcm48To24().feed(pcm)
    chunked = Pcm48To24()
    pieces, offset = [], 0
    for size in (2, 6, 202, 1000, 1, 3):
        size *= 2
        pieces.append(chunked.feed(pcm[offset:offset + size]))
        offset += size
    pieces.append(chunked.feed(pcm[offset:]))
    assert b"".join(pieces) == whole


def test_decimator_matches_direct_convolution():
    # Output n is the 64-tap FIR over input samples 2n+1-63 .. 2n+1, with
    # silence before the first sample.
    pcm = [(i * 7919) % 65536 - 32768 for i in range(1001)]
    taps = _lowpass_taps(64, 0.45)
    padded = [0] * 63 + pcm
    expected = [
        max(-32768, min(32767, round(sum(t * padded[n + 63 - k] for k, t in enumerate(taps)))))
        for n in range(1, len(pcm), 2)
    ]
    decimator, out, offset = Pcm48To24(), b"", 0
    for size in (1, 3, 64, 333, 600):
        out += decimator.feed(struct.pack(f"<{size}h", *pcm[offset:offset + size]))
        offset += size
    actual = struct.unpack(f"<{len(out) // 2}h", out)
    assert len(actual) == len(expected)
    # Float summation order may move a rounding boundary by one step.
    assert max(abs(a - e) for a, e in zip(actual, expected)) <= 1


def tone_level_db(frequency):
    """Steady-state output level of a 48 kHz tone after decimation."""
    amplitude = 16000
    tone = struct.pack("<9600h", *(
        round(amplitude * math.sin(2 * math.pi * frequency * i / 48000)) for i in range(9600)))
    out = struct.unpack("<4800h", Pcm48To24().feed(tone))[200:]
    rms = math.sqrt(sum(v * v for v in out) / len(out))
    return 20 * math.log10(max(rms, 1e-9) / (amplitude / math.sqrt(2)))


def test_decimator_passes_speech_band_and_rejects_aliases():
    for frequency in (300, 1000, 4000, 8000):
        assert abs(tone_level_db(frequency)) < 0.2
    # Above the 24 kHz output's 12 kHz Nyquist a tone would fold back into
    # the speech band; the low-pass must suppress it first.
    for frequency in (13000, 15000, 20000):
        assert tone_level_db(frequency) < -50


def test_decimator_output_is_clamped_to_pcm16():
    square = struct.pack("<h", 32767) * 64 + struct.pack("<h", -32768) * 64
    out = struct.unpack("<256h", Pcm48To24().feed(square * 4))
    assert max(out) == 32767 and min(out) == -32768


def test_playback_preserves_pcm(audio):
    pcm = struct.pack("<hhhh", 32767, -32768, 1000, -1000) * 240
    audio.write(pcm)
    audio.write(pcm)
    assert playback(audio) == pcm
    assert playback(audio) == pcm
    assert audio._playback_bytes == 0


def test_partial_pcm_fragments_fill_callback_and_preserve_remainder(audio):
    first = struct.pack("<h", 1000) * 480
    second = struct.pack("<h", 2000) * 480
    audio.write(first)
    audio.write(second)
    assert playback(audio) == first + second  # no inserted half-frame silence
    assert audio._playback_bytes == 0

    first = struct.pack("<h", 1000) * 720
    second = struct.pack("<h", 2000) * 720
    audio.write(first)
    audio.write(second)
    assert playback(audio) == first + second[:480]
    assert playback(audio) == second[480:] + b"\0" * 960
    assert audio._playback_bytes == 0


def test_fragmented_playback_is_byte_bounded_and_flushable(audio):
    half = struct.pack("<h", 500) * 480
    for _ in range(16):
        audio.write(half)
    assert audio._playback_bytes == 8 * FRAME_BYTES
    with pytest.raises(OSError):
        audio.write(half)
    for _ in range(8):
        assert playback(audio) == half * 2
    assert audio._playback_bytes == 0
    audio.write(half)
    audio.flush()
    assert playback(audio) == b"\0" * FRAME_BYTES
    assert audio._playback_bytes == 0


@pytest.mark.parametrize("failure", ["duplex_start", "output_open", "input_start", "output_start"])
def test_initialization_failure_closes_opened_streams(monkeypatch, failure):
    streams = []

    class InputStream(FakeStream):
        def __init__(self, **options):
            super().__init__(**options)
            streams.append(self)

        def start(self):
            if failure == "input_start":
                raise PortAudioError("input start failed")
            super().start()

    class OutputStream(InputStream):
        def __init__(self, **options):
            if failure == "output_open":
                raise PortAudioError("output open failed")
            super().__init__(**options)

        def start(self):
            if failure == "output_start":
                raise PortAudioError("output start failed")
            super().start()

    class DuplexStream(InputStream):
        def start(self):
            raise PortAudioError("duplex start failed")

    def check_input_settings(**options):
        if failure != "duplex_start" and options["samplerate"] == 24000:
            raise PortAudioError("Invalid sample rate")

    monkeypatch.setitem(sys.modules, "sounddevice", SimpleNamespace(
        RawStream=DuplexStream, RawInputStream=InputStream, RawOutputStream=OutputStream,
        PortAudioError=PortAudioError, check_input_settings=check_input_settings))
    with pytest.raises(PortAudioError):
        LiveAudio()
    assert len(streams) == (1 if failure in ("duplex_start", "output_open") else 2)
    assert all(stream.closed and not stream.active for stream in streams)
    assert all(stream.calls.count("close") == 1 for stream in streams)
