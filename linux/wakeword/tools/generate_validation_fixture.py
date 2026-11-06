# Copyright (c) Meta Platforms, Inc. and affiliates.
# Licensed under the Apache License, Version 2.0.

"""Generate self-authored normalized feature fixtures and eager posteriors.

These artificial LFBE-shaped sequences contain no audio, recordings, or
calibration samples. They validate wheel compatibility, not model accuracy.
Only pass a trusted checkpoint; loading uses weights_only=False.
"""

from __future__ import annotations

import argparse
import json
import math
import sys
from pathlib import Path

# The single exporter lives with the ESP wake-word component.
sys.path.insert(
    0,
    str(
        Path(__file__).resolve().parents[3]
        / "esp32/components/muse_wakeword/tools"
    ),
)

from export_hey_muse import load_streaming_class, sha256_file, write_floats


def main() -> None:
    import torch

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--checkpoint", type=Path, required=True)
    parser.add_argument("--executorch-root", type=Path, required=True)
    parser.add_argument("--model", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()
    torch.set_num_threads(1)
    checkpoint = torch.load(args.checkpoint, map_location="cpu", weights_only=False)
    model = load_streaming_class(args.executorch_root)(apply_softmax=True).eval()
    missing, unexpected = model.load_state_dict(
        checkpoint["model_state_dict"], strict=False
    )
    if unexpected or any(not name.endswith(".history") for name in missing):
        raise ValueError(f"Checkpoint mismatch: {missing}, {unexpected}")
    frames = torch.tensor(
        [
            [[0.0 for _ in range(40)] for _ in range(100)],
            [
                [
                    0.75 * math.sin(0.17 * t + 0.13 * m)
                    + 0.25 * math.cos(0.03 * (t + 1) * (m + 1))
                    for m in range(40)
                ]
                for t in range(100)
            ],
            [
                [((t * 7 + m * 13) % 31 - 15) / 15.0 for m in range(40)]
                for t in range(100)
            ],
        ],
        dtype=torch.float32,
    )
    outputs = []
    with torch.no_grad():
        for stream in frames:
            model.reset()
            outputs.append(
                torch.stack(
                    [
                        model(frame.reshape(1, 40, 1, 1)).reshape(3).clone()
                        for frame in stream
                    ]
                )
            )
    expected = torch.stack(outputs)
    if not torch.isfinite(expected).all():
        raise ValueError("Nonfinite eager outputs")
    args.output_dir.mkdir(parents=True, exist_ok=True)
    frame_path = args.output_dir / "validation_frames.f32"
    expected_path = args.output_dir / "validation_expected.f32"
    write_floats(frame_path, frames)
    write_floats(expected_path, expected)
    manifest = {
        "schema_version": 1,
        "purpose": "Synthetic wheel compatibility fixture, not accuracy evaluation",
        "provenance": "Self-authored arithmetic LFBE-shaped sequences; no recordings or calibration data",
        "encoding": "little-endian float32",
        "clips": 3,
        "frames_per_clip": 100,
        "features_per_frame": 40,
        "posteriors_per_frame": 3,
        "input_shape": [1, 40, 1, 1],
        "output_shape": [1, 3],
        "kinds": ["zeros", "sinusoid", "modular_ramp"],
        "class_names": ["wakeword", "silence", "other"],
        "frames_file": frame_path.name,
        "expected_file": expected_path.name,
        "frames_sha256": sha256_file(frame_path),
        "expected_sha256": sha256_file(expected_path),
        "pte_sha256": sha256_file(args.model),
        "checkpoint_sha256": sha256_file(args.checkpoint),
        "checkpoint_epoch": checkpoint.get("epoch"),
        "eager_torch_version": torch.__version__,
        "atol": 1e-5,
        "rtol": 1e-4,
        "includes_warmup": True,
        "reset": "fresh program/method before each independent stream",
    }
    (args.output_dir / "validation.json").write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n"
    )
    print(json.dumps(manifest, sort_keys=True))


if __name__ == "__main__":
    main()
