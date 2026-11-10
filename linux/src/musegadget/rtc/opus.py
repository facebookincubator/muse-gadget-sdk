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

"""Opus encode/decode through the system libopus (ctypes; no Python package)."""

from __future__ import annotations

import ctypes
import ctypes.util

_APPLICATION_VOIP = 2048
_SET_BITRATE = 4002
# Interpreters with their own loader (e.g. a vendored Python) may not search
# the distribution's library directories.
_SYSTEM_PATHS = (
    "/usr/lib64/libopus.so.0",
    "/usr/lib/x86_64-linux-gnu/libopus.so.0",
    "/usr/lib/aarch64-linux-gnu/libopus.so.0",
    "/usr/lib/arm-linux-gnueabihf/libopus.so.0",
    "/opt/homebrew/lib/libopus.dylib",
    "/usr/local/lib/libopus.dylib",
)


class OpusUnavailable(RuntimeError):
    pass


def _load() -> ctypes.CDLL:
    candidates = [ctypes.util.find_library("opus"), "libopus.so.0", "libopus.dylib", *_SYSTEM_PATHS]
    for name in filter(None, candidates):
        try:
            lib = ctypes.CDLL(name)
            break
        except OSError:
            continue
    else:
        raise OpusUnavailable("libopus not found; install it (e.g. apt install libopus0)")
    lib.opus_encoder_create.restype = ctypes.c_void_p
    lib.opus_encoder_create.argtypes = [ctypes.c_int32, ctypes.c_int, ctypes.c_int, ctypes.POINTER(ctypes.c_int)]
    lib.opus_encode.restype = ctypes.c_int32
    lib.opus_encode.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int, ctypes.c_char_p, ctypes.c_int32]
    lib.opus_encoder_destroy.argtypes = [ctypes.c_void_p]
    # Variadic: declare only the fixed parameters (required on Apple arm64).
    lib.opus_encoder_ctl.argtypes = [ctypes.c_void_p, ctypes.c_int]
    lib.opus_decoder_create.restype = ctypes.c_void_p
    lib.opus_decoder_create.argtypes = [ctypes.c_int32, ctypes.c_int, ctypes.POINTER(ctypes.c_int)]
    lib.opus_decode.restype = ctypes.c_int
    lib.opus_decode.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int32, ctypes.c_char_p, ctypes.c_int, ctypes.c_int]
    lib.opus_decoder_destroy.argtypes = [ctypes.c_void_p]
    return lib


_lib: ctypes.CDLL | None = None


def ensure_available() -> None:
    """Raise OpusUnavailable now rather than mid-call."""
    _opus()


def _opus() -> ctypes.CDLL:
    global _lib
    if _lib is None:
        _lib = _load()
    return _lib


class OpusEncoder:
    """Mono 48 kHz, 20 ms frames of PCM16 little-endian."""

    FRAME_SAMPLES = 960

    def __init__(self, bitrate: int = 32000) -> None:
        err = ctypes.c_int()
        self._enc = _opus().opus_encoder_create(48000, 1, _APPLICATION_VOIP, ctypes.byref(err))
        if err.value or not self._enc:
            raise OpusUnavailable(f"opus_encoder_create failed ({err.value})")
        _opus().opus_encoder_ctl(self._enc, _SET_BITRATE, ctypes.c_int32(bitrate))
        self._out = ctypes.create_string_buffer(1500)

    def encode(self, pcm: bytes) -> bytes:
        n = _opus().opus_encode(self._enc, pcm, self.FRAME_SAMPLES, self._out, len(self._out))
        if n < 0:
            raise RuntimeError(f"opus_encode failed ({n})")
        return self._out.raw[:n]

    def close(self) -> None:
        if self._enc:
            _opus().opus_encoder_destroy(self._enc)
            self._enc = None


class OpusDecoder:
    """Decodes to mono 48 kHz PCM16 little-endian."""

    def __init__(self) -> None:
        err = ctypes.c_int()
        self._dec = _opus().opus_decoder_create(48000, 1, ctypes.byref(err))
        if err.value or not self._dec:
            raise OpusUnavailable(f"opus_decoder_create failed ({err.value})")
        self._out = ctypes.create_string_buffer(5760 * 2)

    def decode(self, packet: bytes | None) -> bytes:
        n = _opus().opus_decode(self._dec, packet, len(packet) if packet else 0, self._out, 5760, 0)
        if n < 0:
            raise RuntimeError(f"opus_decode failed ({n})")
        return self._out.raw[: n * 2]

    def close(self) -> None:
        if self._dec:
            _opus().opus_decoder_destroy(self._dec)
            self._dec = None
