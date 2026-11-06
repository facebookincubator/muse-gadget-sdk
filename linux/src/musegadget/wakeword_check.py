# Copyright (c) Meta Platforms, Inc. and affiliates.
# Licensed under the Apache License, Version 2.0 (the "License");
# you may obtain a copy at http://www.apache.org/licenses/LICENSE-2.0
# Distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND.

"""Check a supplied wheel against the shipped model without opening a microphone.

The reference corpus is self-authored synthetic LFBE frames, not recordings or
calibration data. Numerical compatibility is not wake-word acoustic accuracy.
"""

from __future__ import annotations

import argparse
import ctypes
import ctypes.util
import hashlib
import json
import math
import platform
import struct
import warnings
from contextlib import ExitStack
from importlib import metadata, resources
from pathlib import Path


class CompatibilityError(RuntimeError):
    """A local model, wheel, or signal-library check failed."""


def audio_prerequisites() -> dict:
    try:
        sounddevice_version = metadata.version("sounddevice")
    except metadata.PackageNotFoundError as exc:
        raise CompatibilityError(
            "Install musegadget[voice] in this environment"
        ) from exc
    try:
        library = ctypes.CDLL(
            ctypes.util.find_library("portaudio") or "libportaudio.so.2"
        )
        library.Pa_GetVersion
    except (OSError, AttributeError) as exc:
        raise CompatibilityError(
            "PortAudio is unavailable; install the distro prerequisites explicitly "
            "with setup.sh --install-system-packages"
        ) from exc
    return {"sounddevice_version": sounddevice_version, "portaudio_available": True}


def _fixtures(directory):
    try:
        manifest = json.loads(directory.joinpath("validation.json").read_text())
        frame_bytes = directory.joinpath("validation_frames.f32").read_bytes()
        expected_bytes = directory.joinpath("validation_expected.f32").read_bytes()
    except (OSError, ValueError) as exc:
        raise CompatibilityError(
            "Synthetic validation fixtures are missing or invalid"
        ) from exc
    if manifest.get("schema_version") != 1:
        raise CompatibilityError("Unsupported validation-fixture schema")
    clips, count = manifest.get("clips"), manifest.get("frames_per_clip")
    if (
        type(clips) is not int
        or type(count) is not int
        or not 1 <= clips <= 16
        or not 30 <= count <= 1000
    ):
        raise CompatibilityError(
            "Invalid or oversized sequential-validation dimensions"
        )
    if manifest.get("input_shape") != [1, 40, 1, 1] or manifest.get("output_shape") != [
        1,
        3,
    ]:
        raise CompatibilityError(
            "Validation tensor contract must be [1,40,1,1] -> [1,3]"
        )
    if (
        len(frame_bytes) != clips * count * 40 * 4
        or len(expected_bytes) != clips * count * 3 * 4
    ):
        raise CompatibilityError("Truncated or mismatched validation fixtures")
    for payload, key in (
        (frame_bytes, "frames_sha256"),
        (expected_bytes, "expected_sha256"),
    ):
        if hashlib.sha256(payload).hexdigest() != manifest.get(key):
            raise CompatibilityError(f"Validation fixture hash mismatch: {key}")
    frames = tuple(struct.iter_unpack("<40f", frame_bytes))
    expected = tuple(struct.iter_unpack("<3f", expected_bytes))
    if not all(math.isfinite(value) for row in frames + expected for value in row):
        raise CompatibilityError("Validation fixtures contain nonfinite values")
    return manifest, frames, expected


def _load_method(runtime, path):
    try:
        program = runtime.load_program(path)
        method = program.load_method("forward")
    except Exception as exc:
        raise CompatibilityError(
            "The wheel cannot load the stateful XNNPACK PTE"
        ) from exc
    if method is None:
        raise CompatibilityError("The wheel did not load the model's forward method")
    return program, method


def _execute(torch, method, frame):
    tensor = torch.tensor(frame, dtype=torch.float32).reshape(1, 40, 1, 1)
    try:
        output = method.execute((tensor,))
    except Exception as exc:
        raise CompatibilityError(
            "The wheel failed executing a sequential LFBE frame"
        ) from exc
    if (
        not isinstance(output, (list, tuple))
        or len(output) != 1
        or not isinstance(output[0], torch.Tensor)
        or tuple(output[0].shape) != (1, 3)
        or output[0].dtype != torch.float32
        or output[0].device.type != "cpu"
    ):
        raise CompatibilityError("Wheel output must be one float32 CPU [1,3] tensor")
    row = tuple(float(value) for value in output[0].reshape(-1))
    if not all(math.isfinite(value) for value in row):
        raise CompatibilityError("Wheel output contains nonfinite posteriors")
    return row


def _parity(runtime, torch, path, manifest, frames, expected):
    differences = []
    count = manifest["frames_per_clip"]
    with torch.no_grad():
        for clip in range(manifest["clips"]):
            program, method = _load_method(runtime, path)
            start = clip * count
            stream = frames[start : start + count]
            reference = []
            for index, frame in enumerate(stream):
                actual = _execute(torch, method, frame)
                for got, want in zip(actual, expected[start + index]):
                    difference = abs(got - want)
                    if difference > 1e-5 + 1e-4 * abs(want):
                        raise CompatibilityError(
                            f"Sequential parity failed at clip {clip}, frame {index}"
                        )
                    differences.append(difference)
                reference.append(actual)
            for frame in reversed(stream):
                _execute(torch, method, frame)
            del method, program
            program, method = _load_method(runtime, path)
            other_program, other_method = _load_method(runtime, path)
            for index, frame in enumerate(stream):
                if (
                    _execute(torch, method, frame) != reference[index]
                    or _execute(torch, other_method, frame) != reference[index]
                ):
                    raise CompatibilityError(
                        f"Reset/independent-stream parity failed at clip {clip}, frame {index}"
                    )
            del other_method, other_program, method, program
    return {
        "max_abs_error": max(differences),
        "mean_abs_error": sum(differences) / len(differences),
        "reset_replay": "bit-identical",
        "independent_instances": "bit-identical",
    }


def _signal_check(path):
    try:
        from musegadget.wakeword import ResearchWakeWordWarning, WakeWordDetector

        with warnings.catch_warnings():
            warnings.simplefilter("ignore", ResearchWakeWordWarning)
            with WakeWordDetector(model_path=path) as detector:
                first = detector.feed(b"\0" * 4096)
                detector.reset()
                repeated = False
                for size in (1, 319, 3, 1023, 2750):
                    repeated |= detector.feed(b"\0" * size)
                if type(first) is not bool or repeated != first:
                    raise CompatibilityError(
                        "Signal-library PCM chunk/reset check failed"
                    )
    except CompatibilityError:
        raise
    except Exception as exc:
        raise CompatibilityError(
            "The wheel-backed signal library could not process local PCM"
        ) from exc


def check(*, model_path=None, fixture_dir=None, check_audio=False) -> dict:
    try:
        import torch
        from executorch.runtime import Runtime
    except Exception as exc:
        raise CompatibilityError(
            "Cannot import the supplied wheel runtime and its Torch dependency; "
            "check the wheel's Python/platform and compiled-binding compatibility"
        ) from exc
    data = resources.files("musegadget").joinpath("data", "wakeword")
    directory = Path(fixture_dir) if fixture_dir is not None else data
    manifest, frames, expected = _fixtures(directory)
    try:
        runtime = Runtime.get()
        available = runtime.backend_registry.is_available("XnnpackBackend")
    except Exception as exc:
        raise CompatibilityError(
            "Cannot inspect the wheel runtime backend registry"
        ) from exc
    if not available:
        raise CompatibilityError("The supplied wheel has no registered XnnpackBackend")
    with ExitStack() as stack:
        path = (
            Path(model_path)
            if model_path is not None
            else stack.enter_context(
                resources.as_file(data.joinpath("hey_muse_xnnpack.pte"))
            )
        )
        try:
            model_sha = hashlib.sha256(path.read_bytes()).hexdigest()
        except OSError as exc:
            raise CompatibilityError("The wake-word PTE is unavailable") from exc
        if model_sha != manifest.get("pte_sha256"):
            raise CompatibilityError(
                "PTE hash does not match the synthetic reference corpus"
            )
        report = _parity(runtime, torch, path, manifest, frames, expected)
        _signal_check(path)
    return {
        **report,
        **(audio_prerequisites() if check_audio else {}),
        "ok": True,
        "audio_prerequisites_checked": check_audio,
        "executorch_version": metadata.version("executorch"),
        "torch_version": torch.__version__,
        "python": platform.python_version(),
        "architecture": platform.machine(),
        "backend": "XnnpackBackend",
        "pte_sha256": model_sha,
        "synthetic_clips": manifest["clips"],
        "synthetic_frames": manifest["clips"] * manifest["frames_per_clip"],
        "microphone_opened": False,
        "acoustic_accuracy_verified": False,
    }


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--check-audio",
        action="store_true",
        help="check sounddevice and PortAudio without opening a microphone",
    )
    args = parser.parse_args(argv)
    try:
        report = check(check_audio=args.check_audio)
    except CompatibilityError as exc:
        print(json.dumps({"ok": False, "error": str(exc)}))
        return 1
    except Exception as exc:
        print(
            json.dumps(
                {
                    "ok": False,
                    "error": "Local compatibility check failed",
                    "error_type": type(exc).__name__,
                }
            )
        )
        return 1
    print(json.dumps(report, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
