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

"""Optional local Hey Muse detector: the native runner the ESP32 firmware uses.

feed() consumes 16 kHz mono signed-16-bit little-endian PCM, including split
samples, and reports any wake while consuming the entire chunk. The work is
done by libmuse_wakeword.so (linux/wakeword/build.sh): TensorFlow Lite Micro's
microfrontend, the int8 model through ExecuTorch's ESP-NN delegate, and the
detector, compiled from esp32/components/muse_wakeword. No Python packages,
threads or network: importing this module loads nothing; construction loads
the library.
"""

from __future__ import annotations

import ctypes
import os
import stat
import sys
import threading
import warnings
from pathlib import Path


class WakeWordUnavailableError(RuntimeError):
    """The native library or the requested model is unavailable."""


class ResearchWakeWordWarning(UserWarning):
    """The bundled model was trained and measured on synthetic voices only."""


_SENSITIVITIES = {"low": 0, "default": 1, "high": 2}
_WAKE = 1  # WW_EVENT_WAKE
_SAMPLE_RATE = 16000
_MAX_FRAMES = 16000  # one second per native call
_MAX_MODEL_BYTES = 4 * 1024 * 1024


class _Config(ctypes.Structure):
    # ww_config_t in esp32/components/muse_wakeword/include/ww_runner.h
    _fields_ = [
        ("model", ctypes.c_void_p),
        ("model_len", ctypes.c_size_t),
        ("sample_rate_hz", ctypes.c_uint32),
        ("sensitivity", ctypes.c_int),
        ("vad", ctypes.c_int),
    ]


def _candidates() -> list[Path]:
    override = os.environ.get("MUSE_WAKEWORD_LIBRARY")
    if override:
        # Absolute, so ctypes loads this file: a bare name ("lib.so", which
        # is what Path makes of "./lib.so") would search the loader's paths.
        return [Path(override).expanduser().absolute()]
    return [
        # The copy linux/wakeword/setup.sh installs into its environment.
        Path(sys.prefix) / "lib" / "musegadget" / "libmuse_wakeword.so",
        # A source checkout run in place: what linux/wakeword/build.sh builds.
        Path(__file__).resolve().parents[2] / "wakeword" / "build" / "libmuse_wakeword.so",
    ]


def _read_model(path: Path) -> bytes:
    """A regular file of 1 to _MAX_MODEL_BYTES bytes, read through one open
    file, so a file swapped or grown after the check can't get past it. An
    empty buffer would mean the embedded model to the runner: never
    substitute it for a requested file."""
    # Non-blocking open, so a FIFO or device can't stall it, and the type
    # checked on that descriptor before anything is read.
    fd = os.open(path, os.O_RDONLY | os.O_NONBLOCK | getattr(os, "O_CLOEXEC", 0))
    with open(fd, "rb", closefd=True) as f:
        if not stat.S_ISREG(os.fstat(fd).st_mode):
            raise OSError("not a regular file")
        data = f.read(_MAX_MODEL_BYTES + 1)
    if not 0 < len(data) <= _MAX_MODEL_BYTES:
        raise OSError("empty" if not data else f"larger than the {_MAX_MODEL_BYTES // (1024 * 1024)} MiB limit")
    return data


def _load_library() -> ctypes.CDLL:
    failures = []
    for path in _candidates():
        if not path.is_file():
            failures.append(f"{path}: not found")
            continue
        try:
            library = ctypes.CDLL(str(path))
            library.ww_runner_create.argtypes = [ctypes.POINTER(_Config)]
            library.ww_runner_create.restype = ctypes.c_void_p
            library.ww_runner_destroy.argtypes = [ctypes.c_void_p]
            library.ww_runner_destroy.restype = None
            library.ww_runner_reset.argtypes = [ctypes.c_void_p]
            library.ww_runner_reset.restype = None
            library.ww_runner_feed.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t]
            library.ww_runner_feed.restype = ctypes.c_int
            library.muse_ww_resampler_create.argtypes = []
            library.muse_ww_resampler_create.restype = ctypes.c_void_p
            library.muse_ww_resampler_destroy.argtypes = [ctypes.c_void_p]
            library.muse_ww_resampler_destroy.restype = None
            library.muse_ww_resampler_reset.argtypes = [ctypes.c_void_p]
            library.muse_ww_resampler_reset.restype = None
            library.muse_ww_resampler_feed.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t,
                                                       ctypes.c_void_p]
            library.muse_ww_resampler_feed.restype = ctypes.c_size_t
            library.ww_runner_ok.argtypes = [ctypes.c_void_p]
            library.ww_runner_ok.restype = ctypes.c_bool
            return library
        except (OSError, AttributeError) as exc:
            failures.append(f"{path}: {exc}")
    raise WakeWordUnavailableError(
        "Hey Muse library unavailable. Build it with linux/wakeword/setup.sh, "
        "or set MUSE_WAKEWORD_LIBRARY to libmuse_wakeword.so. " + "; ".join(failures)
    )


class WakeWordDetector:
    """One serialized PCM stream: a native runner with its own state."""

    def __init__(self, model_path=None, sensitivity="default") -> None:
        if sensitivity not in _SENSITIVITIES:
            raise ValueError("sensitivity must be 'low', 'default', or 'high'")
        self._lock = threading.Lock()
        self._handle = None
        self._library = None
        self._model = None  # keeps a caller-supplied model's bytes alive
        self._odd = b""
        self._poisoned = False
        config = _Config(None, 0, _SAMPLE_RATE, _SENSITIVITIES[sensitivity], 0)
        if model_path is not None:
            path = Path(model_path).expanduser()
            try:
                data = _read_model(path)
            except OSError as exc:
                reason = exc.strerror if isinstance(exc, FileNotFoundError) else str(exc)
                raise WakeWordUnavailableError(f"Wake-word model unusable: {path}: {reason}") from exc
            self._model = ctypes.create_string_buffer(data, len(data))
            config.model = ctypes.cast(self._model, ctypes.c_void_p)
            config.model_len = len(data)
        library = _load_library()
        handle = library.ww_runner_create(ctypes.byref(config))
        if not handle:
            raise WakeWordUnavailableError(
                "The Hey Muse runner didn't start: the model doesn't match the runner"
                if model_path is not None else "The Hey Muse runner didn't start"
            )
        self._library, self._handle = library, handle
        try:
            warnings.warn(
                "Hey Muse is an experimental model trained and measured on synthetic "
                "voices, not validated on real microphone recordings. Keep opt-in.",
                ResearchWakeWordWarning,
                stacklevel=2,
            )
        except BaseException:
            self.close()
            raise

    def _check_open(self) -> None:
        if self._handle is None:
            raise RuntimeError("WakeWordDetector is closed")

    def feed(self, pcm_s16le: bytes) -> bool:
        if not isinstance(pcm_s16le, bytes):
            raise TypeError("pcm_s16le must be bytes (16 kHz mono signed-16-bit LE)")
        with self._lock:
            self._check_open()
            if self._poisoned:
                raise RuntimeError("Wake-word stream unavailable; reset required")
            data = self._odd + pcm_s16le
            usable = len(data) & ~1
            self._odd = data[usable:]
            fired = False
            view = memoryview(data)
            for start in range(0, usable, 2 * _MAX_FRAMES):
                chunk = bytes(view[start:min(usable, start + 2 * _MAX_FRAMES)])
                event = self._library.ww_runner_feed(self._handle, chunk, len(chunk) // 2)
                fired = fired or event == _WAKE
                if not self._library.ww_runner_ok(self._handle):
                    self._poisoned = True
                    raise RuntimeError("Hey Muse inference failed; reset required")
            return fired

    def reset(self) -> None:
        with self._lock:
            self._check_open()
            self._library.ww_runner_reset(self._handle)
            self._odd = b""
            if not self._library.ww_runner_ok(self._handle):
                self._poisoned = True
                raise RuntimeError("Hey Muse runner could not be reset")
            self._poisoned = False

    def close(self) -> None:
        with self._lock:
            if self._handle is not None:
                handle, self._handle = self._handle, None
                self._library.ww_runner_destroy(handle)
            self._model = None
            self._odd = b""

    def __del__(self) -> None:
        try:
            self.close()
        except Exception:
            pass

    def resampler(self) -> Resampler24To16:
        """A native 24 to 16 kHz resampler for this detector's tap: the same
        filter as wakeword_input.Pcm24To16, without its Python CPU cost."""
        with self._lock:
            self._check_open()
            return Resampler24To16(self._library)

    def __enter__(self) -> WakeWordDetector:
        with self._lock:
            self._check_open()
        return self

    def __exit__(self, exc_type, exc, traceback) -> None:
        self.close()


class Resampler24To16:
    """Streaming 2:3 resampler from the native library; chunk-independent."""

    def __init__(self, library) -> None:
        self._library = library
        self._handle = library.muse_ww_resampler_create()
        if not self._handle:
            raise MemoryError("no memory for the resampler")

    def feed(self, pcm: bytes) -> bytes:
        if len(pcm) % 2:
            raise ValueError("wake-word audio must contain whole PCM16LE samples")
        if self._handle is None:
            raise RuntimeError("resampler is closed")
        frames = len(pcm) // 2
        if frames == 0:
            return b""
        out = ctypes.create_string_buffer((frames * 2 // 3 + 2) * 2)
        written = self._library.muse_ww_resampler_feed(self._handle, pcm, frames, out)
        return out.raw[: written * 2]

    def reset(self) -> None:
        if self._handle is not None:
            self._library.muse_ww_resampler_reset(self._handle)

    def close(self) -> None:
        if self._handle is not None:
            handle, self._handle = self._handle, None
            self._library.muse_ww_resampler_destroy(handle)

    def __del__(self) -> None:
        try:
            self.close()
        except Exception:
            pass
