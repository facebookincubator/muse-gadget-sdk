# Copyright (c) Meta Platforms, Inc. and affiliates.
# Licensed under the Apache License, Version 2.0 (the "License");
# you may obtain a copy at http://www.apache.org/licenses/LICENSE-2.0
# Distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND.

"""Check the native Hey Muse library without opening a microphone.

Runs the runner on self-made test signals (silence and tones, no
recordings), in different chunk sizes and across a reset, and checks that it
answers identically each time. Numerical consistency is not wake-word
acoustic accuracy.
"""

from __future__ import annotations

import argparse
import ctypes
import ctypes.util
import json
import math
import platform
import struct
import warnings
from importlib import metadata


class CompatibilityError(RuntimeError):
    """The native library or the audio prerequisites failed a check."""


def audio_prerequisites() -> dict:
    try:
        sounddevice_version = metadata.version("sounddevice")
    except metadata.PackageNotFoundError as exc:
        raise CompatibilityError("Install musegadget[voice] in this environment") from exc
    try:
        library = ctypes.CDLL(ctypes.util.find_library("portaudio") or "libportaudio.so.2")
        library.Pa_GetVersion
    except (OSError, AttributeError) as exc:
        raise CompatibilityError(
            "PortAudio is unavailable; install the distro prerequisites explicitly "
            "with setup.sh --install-system-packages"
        ) from exc
    return {"sounddevice_version": sounddevice_version, "portaudio_available": True}


def _signal(seconds=4.0, rate=16000) -> bytes:
    samples = []
    for i in range(int(seconds * rate)):
        t = i / rate
        tone = 6000 * math.sin(2 * math.pi * (300 + 200 * t) * t)
        samples.append(int(tone if int(t * 2) % 2 else 0))
    return struct.pack("<%dh" % len(samples), *samples)


def _detector_check() -> dict:
    from musegadget.wakeword import ResearchWakeWordWarning, WakeWordDetector

    pcm = _signal()
    with warnings.catch_warnings():
        warnings.simplefilter("ignore", ResearchWakeWordWarning)
        detector = WakeWordDetector()
    try:
        results = []
        for size in (640, 1, 3, 1023, 32000):
            detector.reset()
            fired = False
            for start in range(0, len(pcm), size):
                fired |= detector.feed(pcm[start:start + size])
            results.append(fired)
        if len(set(results)) != 1 or results[0]:
            raise CompatibilityError("The runner answered differently for the same audio")
    finally:
        detector.close()
    return {"chunk_and_reset_check": "consistent", "test_signal_wakes": 0}


def check(*, check_audio=False) -> dict:
    try:
        report = _detector_check()
    except CompatibilityError:
        raise
    except Exception as exc:
        raise CompatibilityError(f"The Hey Muse library could not run: {exc}") from exc
    return {
        **report,
        **(audio_prerequisites() if check_audio else {}),
        "ok": True,
        "audio_prerequisites_checked": check_audio,
        "python": platform.python_version(),
        "architecture": platform.machine(),
        "microphone_opened": False,
        "acoustic_accuracy_verified": False,
    }


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check-audio", action="store_true",
                        help="check sounddevice and PortAudio without opening a microphone")
    args = parser.parse_args(argv)
    try:
        report = check(check_audio=args.check_audio)
    except CompatibilityError as exc:
        print(json.dumps({"ok": False, "error": str(exc)}))
        return 1
    except Exception as exc:
        print(json.dumps({"ok": False, "error": "Local check failed", "error_type": type(exc).__name__}))
        return 1
    print(json.dumps(report, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
