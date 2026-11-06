# Copyright (c) Meta Platforms, Inc. and affiliates.
# Licensed under the Apache License, Version 2.0 (the "License");
# you may obtain a copy at http://www.apache.org/licenses/LICENSE-2.0
# Distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND.

"""Validate wheel-backed sequential/reset parity against export-time eager data.

Requires the installed wheel and torch, not an ExecuTorch checkout or compiler.
Calibration fixtures stay outside the repository. Use wakeword_check for the
shipped synthetic compatibility corpus. No validation here establishes accuracy.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import platform
import struct
import sys
import time
from importlib import metadata
from pathlib import Path


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, fromfile_prefix_chars="@")
    parser.add_argument(
        "--library", type=Path, required=True, help="C-only signal library"
    )
    parser.add_argument("--model", type=Path, required=True)
    parser.add_argument("--parity-dir", type=Path, required=True)
    parser.add_argument("--metadata", type=Path, required=True)
    parser.add_argument(
        "--wheel", type=Path, help="Exact tested wheel file, recorded by hash"
    )
    args = parser.parse_args()
    sidecar = json.loads(args.metadata.read_text())
    if sidecar["pte_sha256"] != sha256(args.model):
        raise ValueError("Model hash does not match export metadata")
    if sidecar["backend"] != "XNNPACK" or not sidecar["mutable_histories"]:
        raise ValueError("Expected an exported stateful XNNPACK model")
    manifest = json.loads((args.parity_dir / "manifest.json").read_text())
    clips = manifest["clips"]
    frames_per_clip = manifest["frames_per_clip"]
    if (
        clips < 3
        or frames_per_clip < 30
        or set(manifest["kinds"]) != {"positive", "negative_speech", "silence"}
    ):
        raise ValueError("Need representative multi-kind streams beyond warm-up")
    frame_bytes = (args.parity_dir / "frames.f32").read_bytes()
    expected_bytes = (args.parity_dir / "expected.f32").read_bytes()
    total_frames = clips * frames_per_clip
    if (
        len(frame_bytes) != total_frames * 40 * 4
        or len(expected_bytes) != total_frames * 3 * 4
    ):
        raise ValueError("Truncated or mismatched parity fixtures")
    frames = list(struct.iter_unpack("<40f", frame_bytes))
    expected = list(struct.iter_unpack("<3f", expected_bytes))
    os.environ["MUSE_WAKEWORD_LIBRARY"] = str(args.library.resolve())
    source_root = Path(__file__).resolve().parents[2] / "src"
    sys.path.insert(0, str(source_root))
    from musegadget.wakeword import _WheelModel

    model = _WheelModel(args.model.resolve())
    errors = []
    reset_error = 0.0
    start_time = time.monotonic()
    try:
        for clip in range(clips):
            model.reset()
            start = clip * frames_per_clip
            stream = frames[start : start + frames_per_clip]
            reference = []
            for t, frame in enumerate(stream):
                actual = model.posteriors(frame)
                for got, want in zip(actual, expected[start + t]):
                    difference = abs(got - want)
                    if (
                        not math.isfinite(got)
                        or not math.isfinite(want)
                        or difference > 1e-5 + 1e-4 * abs(want)
                    ):
                        raise AssertionError(
                            f"Parity mismatch clip={clip} frame={t}: {actual} != {expected[start + t]}"
                        )
                    errors.append(difference)
                reference.append(actual)
            for frame in reversed(stream):
                model.posteriors(frame)
            model.reset()
            other = _WheelModel(args.model.resolve())
            try:
                for t, frame in enumerate(stream):
                    repeated = model.posteriors(frame)
                    fresh = other.posteriors(frame)
                    reset_error = max(
                        reset_error,
                        *(abs(a - b) for a, b in zip(repeated, reference[t])),
                    )
                    if repeated != reference[t] or fresh != reference[t]:
                        raise AssertionError(
                            f"Reset/independence mismatch clip={clip} frame={t}"
                        )
            finally:
                other.close()
    finally:
        model.close()
    # Also verify the actual PCM shim/wrapper via the shipped synthetic checker.
    from musegadget.wakeword_check import check

    synthetic_report = check(model_path=args.model.resolve())
    import torch

    report = {
        "status": "passed",
        "runtime": "ExecuTorch wheel public Runtime API",
        "executorch_version": metadata.version("executorch"),
        "torch_version": torch.__version__,
        "architecture": platform.machine(),
        "clips": clips,
        "frames": total_frames,
        "includes_warmup": True,
        "max_abs_error": max(errors),
        "mean_abs_error": sum(errors) / len(errors),
        "reset_max_abs_error": reset_error,
        "independent_instances": "bit-identical",
        "rtol": 1e-4,
        "atol": 1e-5,
        "signal_library_sha256": sha256(args.library),
        "frames_sha256": hashlib.sha256(frame_bytes).hexdigest(),
        "expected_sha256": hashlib.sha256(expected_bytes).hexdigest(),
        "synthetic_compatibility": synthetic_report,
        "validation_seconds": time.monotonic() - start_time,
    }
    if args.wheel is not None:
        report["reference_wheel_file"] = args.wheel.name
        report["reference_wheel_sha256"] = sha256(args.wheel)
    sidecar["wheel_runtime_vs_eager"] = report
    temporary = args.metadata.with_suffix(".json.tmp")
    temporary.write_text(json.dumps(sidecar, indent=2, sort_keys=True) + "\n")
    temporary.replace(args.metadata)
    print(json.dumps(report, sort_keys=True))


if __name__ == "__main__":
    main()
