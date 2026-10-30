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

import struct
import sys
from types import SimpleNamespace

import pytest

from musegadget.voice_audio import LiveAudio
from musegadget.voice_call import FRAME_BYTES


class FakeStream:
    def __init__(self, **options):
        self.active = False

    def start(self):
        self.active = True

    def abort(self):
        self.active = False

    def close(self):
        self.active = False


@pytest.fixture
def audio(monkeypatch):
    monkeypatch.setitem(sys.modules, "sounddevice", SimpleNamespace(RawStream=FakeStream))
    result = LiveAudio()
    try:
        yield result
    finally:
        result.close()


def playback(audio):
    output = bytearray(FRAME_BYTES)
    audio._callback(b"\0" * FRAME_BYTES, output, 960, None, None)
    return bytes(output)


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
