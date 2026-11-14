# Copyright (c) Meta Platforms, Inc. and affiliates.
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at http://www.apache.org/licenses/LICENSE-2.0
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.

"""Export the trusted Hey Muse StreamingWakeWord checkpoint to ExecuTorch.

One exporter, two backends, selected with --backend (default: cadence):

- cadence: int8-only Cadence-backend PTQ (int8 weights and activations,
  calibrated on the training bundle's calibration_frames.pt via
  --calibration), lowered through the Cadence passes to cadence:: ops.
  The registry component registers only the kernels in a bundle
  generated at build time from the exported .pte (see
  esp32/docs/executorch.md); no bundle is committed.
  Writes, by default, ../models/hey_muse.pte, ../models/hey_muse.json,
  and the generated ../ww_frontend_config.h.
  (A portable-fp32 export measured ~69 ms per 20 ms
  frame on ESP32-S3 — 3.5x over the real-time budget — and was removed;
  the eager model here is the numerical reference for the parity check.)
- xnnpack: stateful fp32 XNNPACK export for Linux. Writes
  hey_muse_xnnpack.pte and hey_muse_xnnpack.json to --output-dir, plus
  sequential validation fixtures to --parity-dir. No quantization is
  performed; the calibration bundle is only a sequential validation
  corpus. The checkpoint's frontend must match the shared ESP header
  passed with --frontend-header; neither ESP nor Cadence output is
  rewritten. With --defer-runtime-validation, runtime validation is
  deferred to verify_parity.py.

The SDK owns the wake word application (frontend, detector, runner), so
the exporter lives with the runner. The model architecture itself comes
from an ExecuTorch checkout
(examples/models/mlperf_tiny/streaming_wakeword.py). The I/O contract:
one normalized fp32 LFBE frame (1, 40, 1, 1) in, 3 fp32 posteriors out.

The checkpoint is loaded with weights_only=False, following the training
bundle's README. Only pass checkpoints from a trusted source. Validation
streams are normalized LFBE, not raw audio. They prove numerical parity,
NOT accuracy or false-alarm rates.

Torch is imported lazily so this module stays importable (for
EXPECTED_FRONTEND and validate_frontend) without a torch install.
"""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import math
import os
import re
import struct
import subprocess
import sys
import tempfile
import types
from pathlib import Path
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    import torch

EXPECTED_FRONTEND = {
    "sample_rate": 16000,
    "window_samples": 480,
    "hop_samples": 320,
    "n_fft": 512,
    "n_mels": 40,
    "fmin": 0.0,
    "fmax": 8000.0,
    "mel_scale": "slaney",
    "log_eps": 1e-6,
}
EXPECTED_CLASSES = ["wakeword", "silence", "other"]
CAVEAT = (
    "Research/synthetic-data checkpoint, opt-in only. Clean-voice training "
    "bundle reports 78.7% recall at 33.8 false alarms/hour at threshold .95 "
    "and 51.3% recall at 3.98 false alarms/hour at .99 (fp32 streaming "
    "test); no threshold reaches <=1 false alarm/hour. "
    "These are not Linux accuracy measurements."
)


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def load_streaming_class(executorch_root: Path):
    # The model file imports EagerModelBase from the installed package. It
    # is irrelevant to the wake word architecture, so stub it if that
    # package is older than the checkout (the usual editable-install drift).
    try:
        import executorch.examples.models.model_base  # noqa: F401
    except Exception:
        stub = types.ModuleType("executorch.examples.models.model_base")
        stub.EagerModelBase = object
        sys.modules["executorch.examples.models.model_base"] = stub

    path = executorch_root / "examples/models/mlperf_tiny/streaming_wakeword.py"
    if not path.is_file():
        raise SystemExit(f"StreamingWakeWord source not found: {path}")
    spec = importlib.util.spec_from_file_location("streaming_wakeword", path)
    if spec is None or spec.loader is None:
        raise ValueError(f"Cannot load StreamingWakeWord from {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module.StreamingWakeWord


def validate_frontend_config(frontend: dict) -> None:
    for key, expected in EXPECTED_FRONTEND.items():
        actual = frontend.get(key)
        if actual != expected:
            raise SystemExit(
                f"unsupported frontend config {key}={actual!r}, expected {expected!r}; "
                "the C frontend in ww_frontend.c implements only the frozen config"
            )
    if len(frontend["norm_mean"]) != 40 or len(frontend["norm_std"]) != 40:
        raise SystemExit("norm_mean/norm_std must each have 40 entries")
    if any(float(v) == 0.0 for v in frontend["norm_std"]):
        raise SystemExit("norm_std contains a zero entry")


def load_checkpoint_model(checkpoint, executorch_root: Path):
    """Load the streaming model; returns (streaming_cls, model).

    History buffers are zeroed after loading: both backends stream frames
    after a reset, and the exported programs serialize zero histories.
    """
    config = checkpoint["config"]
    if config["model"].get("class_names") != EXPECTED_CLASSES:
        raise SystemExit(
            f"unexpected class order: {config['model'].get('class_names')!r}"
        )
    streaming_cls = load_streaming_class(executorch_root)
    model = streaming_cls(apply_softmax=True).eval()
    missing, unexpected = model.load_state_dict(
        checkpoint["model_state_dict"], strict=False
    )
    if unexpected:
        raise SystemExit(f"unexpected checkpoint keys: {unexpected}")
    non_history_missing = [k for k in missing if not k.endswith(".history")]
    if non_history_missing:
        raise SystemExit(
            f"checkpoint is missing non-history keys: {non_history_missing}"
        )
    model.reset()
    return streaming_cls, model


def model_ops(executorch_root: Path, pte_path: Path) -> list[str]:
    with tempfile.TemporaryDirectory() as td:
        out = Path(td) / "selected_operators.yaml"
        subprocess.run(
            [
                sys.executable,
                "-m",
                "codegen.tools.gen_oplist",
                f"--output_path={out}",
                f"--model_file_path={pte_path}",
            ],
            cwd=executorch_root,
            check=True,
            capture_output=True,
            text=True,
        )
        in_ops = False
        ops = []
        for line in out.read_text().splitlines():
            if line.startswith("operators:"):
                in_ops = True
                continue
            if in_ops:
                if line.startswith("  ") and not line.startswith("    "):
                    key = line.strip()
                    ops.append(key[:-1] if key.endswith(":") else key)
                elif line and not line[0].isspace():
                    break
        return sorted(set(ops))


def load_calibration_streams(
    path: Path, n_calibration_clips: int, n_parity_per_kind: int = 8
) -> tuple[list[list[torch.Tensor]], list[list[torch.Tensor]]]:
    """Split the bundle's calibration clips into calibration/parity streams.

    The bundle stores features as [clip, 40, frames] plus per-clip kind
    metadata. Selection is deterministic: per kind, in metadata order,
    the first clips calibrate and the next few are held out for the
    fp32-vs-int8 parity check. Each stream is a list of (1, 40, 1, 1)
    frames to be fed sequentially after a state reset (the model is
    stateful; observers only see realistic ranges when streamed).
    """
    import torch

    bundle = torch.load(path, map_location="cpu", weights_only=False)
    features = bundle["features"]
    metadata = bundle["metadata"]
    if features.ndim != 3 or features.shape[1] != 40:
        raise SystemExit(f"unexpected calibration features shape {features.shape}")
    if len(metadata) != features.shape[0]:
        raise SystemExit("calibration metadata/clip count mismatch")

    by_kind: dict[str, list[int]] = {}
    for i, entry in enumerate(metadata):
        by_kind.setdefault(entry["kind"], []).append(i)

    def stream(clip: int) -> list[torch.Tensor]:
        return [
            features[clip, :, t].reshape(1, 40, 1, 1)
            for t in range(features.shape[2])
        ]

    calibration: list[list[torch.Tensor]] = []
    parity: list[list[torch.Tensor]] = []
    for idxs in by_kind.values():
        n_cal = max(1, round(n_calibration_clips * len(idxs) / len(metadata)))
        calibration += [stream(i) for i in idxs[:n_cal]]
        parity += [stream(i) for i in idxs[n_cal : n_cal + n_parity_per_kind]]
    return calibration, parity


def zero_histories(module: torch.nn.Module) -> None:
    import torch

    with torch.no_grad():
        for name, buf in module.named_buffers():
            if name.endswith("history"):
                buf.zero_()


def stream_posteriors(
    forward, reset, streams: list[list[torch.Tensor]]
) -> list[torch.Tensor]:
    """Run each stream frame by frame; return per-stream [frames, 3]."""
    import torch

    out = []
    with torch.no_grad():
        for frames in streams:
            reset()
            rows = []
            for frame in frames:
                result = forward(frame)
                if isinstance(result, (tuple, list)):
                    result = result[0]
                rows.append(result.reshape(-1))
            out.append(torch.stack(rows))
    return out


def quantize_cadence_int8(model, example, args):
    """PTQ + Cadence lowering; returns (EdgeProgramManager, info dict)."""
    import torch
    from executorch.backends.cadence.aot.compiler import (
        _lower_ep_to_cadence,
        apply_pre_edge_transform_passes,
        convert_pt2,
        prepare_pt2,
    )
    from executorch.backends.cadence.aot.quantizer.quantizer import (
        CadenceDefaultQuantizer,
    )

    calibration, parity_streams = load_calibration_streams(
        args.calibration, args.calibration_clips
    )
    print(
        f"calibration: {len(calibration)} clips, "
        f"parity: {len(parity_streams)} clips"
    )

    fp32_ref = stream_posteriors(model, model.reset, parity_streams)

    quantizer = CadenceDefaultQuantizer()
    prepared = prepare_pt2(model, (example,), quantizer)
    with torch.no_grad():
        for i, frames in enumerate(calibration):
            zero_histories(prepared)
            for frame in frames:
                prepared(frame)
            if (i + 1) % 16 == 0:
                print(f"  calibrated {i + 1}/{len(calibration)} clips")
    converted = convert_pt2(prepared)

    int8_out = stream_posteriors(
        converted, lambda: zero_histories(converted), parity_streams
    )
    wake_diff = torch.cat(
        [(a[:, 0] - b[:, 0]) for a, b in zip(int8_out, fp32_ref)]
    )
    parity = {
        "clips": len(parity_streams),
        "frames": sum(len(s) for s in parity_streams),
        "wake_posterior_mean_abs_diff": float(wake_diff.abs().mean()),
        "wake_posterior_max_abs_diff": float(wake_diff.abs().max()),
        "fp32_frames_over_0.95": int(
            sum(int((b[:, 0] > 0.95).sum()) for b in fp32_ref)
        ),
        "int8_frames_over_0.95": int(
            sum(int((a[:, 0] > 0.95).sum()) for a in int8_out)
        ),
    }
    print(f"parity vs fp32: {parity}")

    with torch.no_grad():
        exported = torch.export.export(converted, (example,), strict=True)
        exported = apply_pre_edge_transform_passes(exported, quantizer)
        edge = _lower_ep_to_cadence(exported)
    info = {
        "quantization": "cadence-int8-ptq",
        "calibration_clips": len(calibration),
        "parity_vs_fp32": parity,
    }
    return edge, info


def format_float_array(name: str, values: list[float]) -> str:
    lines = [f"static const float {name}[{len(values)}] = {{"]
    for i in range(0, len(values), 4):
        chunk = ", ".join(f"{v:.9g}f" for v in values[i : i + 4])
        lines.append(f"    {chunk},")
    lines.append("};")
    return "\n".join(lines)


def write_frontend_header(path: Path, frontend: dict, checkpoint_sha: str) -> None:
    text = f"""/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/* GENERATED by tools/export_hey_muse.py from checkpoint
 * sha256:{checkpoint_sha}. Do not edit by hand. */

#pragma once

#define WW_FRONTEND_SAMPLE_RATE_HZ                                            \
   { frontend['sample_rate'] }
#define WW_FRONTEND_WINDOW_SAMPLES                                            \
   { frontend['window_samples'] }
#define WW_FRONTEND_HOP_SAMPLES                                               \
   { frontend['hop_samples'] }
#define WW_FRONTEND_N_FFT                                                     \
   { frontend['n_fft'] }
#define WW_FRONTEND_N_MELS                                                    \
   { frontend['n_mels'] }
#define WW_FRONTEND_LOG_EPS {frontend['log_eps']:.9g}f

{format_float_array('kWwFrontendNormMean', [float(v) for v in frontend['norm_mean']])
 }

 {format_float_array('kWwFrontendNormStd', [float(v) for v in frontend['norm_std']])
 }
"""
    path.write_text(text)


def validate_frontend(frontend: dict, header: Path) -> None:
    for key, expected in EXPECTED_FRONTEND.items():
        if frontend.get(key) != expected:
            raise ValueError(f"Unsupported frontend {key}={frontend.get(key)!r}")
    text = header.read_text()
    for key, name in (
        ("norm_mean", "kWwFrontendNormMean"),
        ("norm_std", "kWwFrontendNormStd"),
    ):
        match = re.search(rf"{name}\[40\]\s*=\s*\{{([^}}]+)\}}", text)
        if match is None:
            raise ValueError(f"Missing frozen normalization array {name}")
        values = [
            float(v.strip().removesuffix("f"))
            for v in match.group(1).split(",")
            if v.strip()
        ]
        expected = frontend[key]
        if (
            len(values) != 40
            or len(expected) != 40
            or any(
                not math.isfinite(float(b))
                or not math.isclose(a, float(b), rel_tol=1e-8)
                for a, b in zip(values, expected)
            )
        ):
            raise ValueError(f"Checkpoint {key} differs from frozen ESP frontend")
        if key == "norm_std" and any(v <= 0 for v in values):
            raise ValueError("Normalization standard deviations must be positive")


def select_streams(path: Path, clips_per_kind: int) -> tuple[torch.Tensor, list[str]]:
    import torch

    bundle = torch.load(path, map_location="cpu", weights_only=False)
    features = bundle["features"]
    metadata = bundle["metadata"]
    if features.ndim != 3 or features.shape[1] != 40 or len(metadata) != len(features):
        raise ValueError("Invalid calibration feature/metadata shapes")
    selected = []
    kinds = []
    counts = {}
    for i, record in enumerate(metadata):
        kind = record["kind"]
        if counts.get(kind, 0) < clips_per_kind:
            selected.append(i)
            kinds.append(kind)
            counts[kind] = counts.get(kind, 0) + 1
    if set(counts) != {"positive", "negative_speech", "silence"}:
        raise ValueError(f"Expected all three validation kinds, got {counts}")
    streams = features[selected].transpose(1, 2).contiguous().float()
    if not torch.isfinite(streams).all():
        raise ValueError("Nonfinite validation features")
    return streams, kinds


def eager_posteriors(model, streams: torch.Tensor) -> torch.Tensor:
    outputs = []
    import torch

    with torch.no_grad():
        for stream in streams:
            model.reset()
            outputs.append(
                torch.stack(
                    [
                        model(frame.reshape(1, 40, 1, 1)).reshape(3).clone()
                        for frame in stream
                    ]
                )
            )
    model.reset()
    return torch.stack(outputs)


def validate_runtime(pte: Path, streams: torch.Tensor, expected: torch.Tensor) -> dict:
    from executorch.runtime import Runtime

    runtime = Runtime.get()
    if not runtime.backend_registry.is_available("XnnpackBackend"):
        raise RuntimeError("Validation runtime was built without XNNPACK")
    actual = []
    reset_difference = 0.0
    import torch

    with torch.no_grad():
        for stream in streams:
            # The native reset contract recreates a method; test those exact semantics.
            program = runtime.load_program(pte)
            method = program.load_method("forward")
            rows = torch.stack(
                [
                    method.execute((frame.reshape(1, 40, 1, 1),))[0].reshape(3).clone()
                    for frame in stream
                ]
            )
            for frame in stream.flip(0):
                method.execute((frame.reshape(1, 40, 1, 1),))
            program = runtime.load_program(pte)
            method = program.load_method("forward")
            repeated = torch.stack(
                [
                    method.execute((frame.reshape(1, 40, 1, 1),))[0].reshape(3).clone()
                    for frame in stream
                ]
            )
            reset_difference = max(
                reset_difference, float((rows - repeated).abs().max())
            )
            actual.append(rows)
    actual = torch.stack(actual)
    torch.testing.assert_close(actual, expected, rtol=1e-4, atol=1e-5)
    if reset_difference != 0.0:
        raise AssertionError(f"Fresh/reset stream mismatch: {reset_difference}")
    difference = (actual - expected).abs()
    return {
        "clips": len(streams),
        "frames": streams.shape[0] * streams.shape[1],
        "includes_warmup": True,
        "max_abs_error": float(difference.max()),
        "mean_abs_error": float(difference.mean()),
        "reset_max_abs_error": reset_difference,
        "rtol": 1e-4,
        "atol": 1e-5,
        "runtime": "ExecuTorch Python XNNPACK",
        "status": "passed",
    }


def write_floats(path: Path, tensor: torch.Tensor) -> None:
    flat = tensor.reshape(-1).tolist()
    with path.open("wb") as output:
        for start in range(0, len(flat), 4096):
            chunk = flat[start : start + 4096]
            output.write(struct.pack(f"<{len(chunk)}f", *chunk))


def export_cadence(args, checkpoint, model, streaming_cls, et_root: Path) -> None:
    import torch

    # Use the installed ExecuTorch Python package for exir. Do not force the
    # checkout onto sys.path: an uninstalled checkout lacks package resources
    # (for example exir / _serialize / program.fbs) that the install provides.
    # The documented prerequisite is an install built from this checkout; warn
    # when the environment does not look like one.
    import executorch.exir as exir

    exir_file = Path(exir.__file__).resolve()
    if et_root not in exir_file.parents:
        print(
            f"WARNING: executorch.exir resolved outside {et_root}: {exir_file}. "
            "Install ExecuTorch from --executorch-root to align exporter/runtime.",
            file=sys.stderr,
        )

    config = checkpoint["config"]
    frontend = config["frontend"]
    model_cfg = config["model"]

    from executorch.exir import ExecutorchBackendConfig
    from executorch.exir.passes.init_mutable_pass import InitializedMutableBufferPass

    example = torch.zeros(streaming_cls.FRAME_SHAPE)
    edge, quantize_info = quantize_cadence_int8(model, example, args)
    with torch.no_grad():
        program = edge.to_executorch(
            config=ExecutorchBackendConfig(
                # Histories are zero on a fresh eager model; serialize those
                # zeros so a newly loaded method starts in the same state.
                passes=[InitializedMutableBufferPass(["history"])],
            )
        )

    args.output_dir.mkdir(parents=True, exist_ok=True)
    pte_path = args.output_dir / "hey_muse.pte"
    with pte_path.open("wb") as f:
        program.write_to_file(f)

    # The registry component registers only the kernels in a bundle
    # generated at build time from this .pte; the build refuses a bundle
    # that does not cover this model's ops. Record the ops here so the
    # bundle check has a sidecar to compare against.
    ops = model_ops(et_root, pte_path)

    checkpoint_sha = sha256_file(args.checkpoint)
    write_frontend_header(args.frontend_header, frontend, checkpoint_sha)

    et_version = (et_root / "version.txt").read_text().strip()
    try:
        import executorch.version as et_installed_version

        exporter_git_version = getattr(et_installed_version, "git_version", None)
    except Exception:
        exporter_git_version = None
    try:
        et_commit = subprocess.run(
            ["git", "-C", str(et_root), "rev-parse", "HEAD"],
            check=True,
            capture_output=True,
            text=True,
        ).stdout.strip()
    except Exception:
        et_commit = None

    sidecar = {
        "schema_version": 1,
        "model": "StreamingWakeWord",
        "wake_phrases": config.get("data", {}).get("wake_phrases", []),
        "class_names": EXPECTED_CLASSES,
        "source": args.source,
        "checkpoint_sha256": checkpoint_sha,
        "checkpoint_epoch": checkpoint.get("epoch"),
        "pte_sha256": sha256_file(pte_path),
        "pte_size": pte_path.stat().st_size,
        "architecture_executorch_version": et_version,
        "architecture_executorch_commit": et_commit,
        "exporter_git_version": exporter_git_version,
        "quantization": quantize_info["quantization"],
        "model_ops": ops,
        "frontend": {
            k: frontend[k]
            for k in (
                "sample_rate",
                "window_samples",
                "hop_samples",
                "n_fft",
                "n_mels",
                "fmin",
                "fmax",
                "mel_scale",
                "log_eps",
                "clip_frames",
            )
        },
        "receptive_field_frames": model_cfg.get("receptive_field"),
        "notes": (
            "Research/synthetic-data checkpoint; see the training bundle README "
            "for metrics and limitations. Runtime reading uses the sidecar only "
            "as provenance; the C frontend constants are regenerated alongside."
        ),
    }
    sidecar["calibration"] = {
        "file": args.calibration.name,
        "sha256": sha256_file(args.calibration),
        "clips": quantize_info["calibration_clips"],
    }
    sidecar["parity_vs_fp32"] = quantize_info["parity_vs_fp32"]
    (args.output_dir / "hey_muse.json").write_text(
        json.dumps(sidecar, indent=2, sort_keys=True) + "\n"
    )
    print(f"wrote {pte_path} ({pte_path.stat().st_size} bytes)")
    print(f"wrote {args.output_dir / 'hey_muse.json'}")
    print(f"wrote {args.frontend_header}")
    print(f"model ops ({len(ops)}): {', '.join(ops)}")


def export_xnnpack(args, checkpoint, model, streaming_cls, et_root: Path) -> None:
    import torch

    config = checkpoint["config"]
    validate_frontend(config["frontend"], args.frontend_header)
    model.reset()
    streams, kinds = select_streams(args.calibration, args.clips_per_kind)
    expected = eager_posteriors(model, streams)

    from executorch.backends.xnnpack.partition.xnnpack_partitioner import (
        XnnpackFloatingPointPartitioner,
    )
    from executorch.exir import ExecutorchBackendConfig, to_edge_transform_and_lower
    from executorch.exir.passes.init_mutable_pass import InitializedMutableBufferPass

    with torch.no_grad():
        exported = torch.export.export(
            model, (torch.zeros(streaming_cls.FRAME_SHAPE),), strict=True
        )
        # torch 2.14 preserves copy_ until functionalization exposes mutations.
        exported = exported.run_decompositions()
        mutations = list(exported.graph_signature.buffers_to_mutate.values())
        if set(mutations) != set(streaming_cls.HISTORY_NAMES):
            raise ValueError(f"History mutations lost in export: {mutations}")
        edge = to_edge_transform_and_lower(
            exported, partitioner=[XnnpackFloatingPointPartitioner()]
        )
        model.reset()
        program = edge.to_executorch(
            config=ExecutorchBackendConfig(
                passes=[InitializedMutableBufferPass(["history"])],
            )
        )
    plan = program.executorch_program.execution_plan[0]
    delegates = [delegate.id for delegate in plan.delegates]
    if not delegates or set(delegates) != {"XnnpackBackend"}:
        raise ValueError(f"Expected real XNNPACK delegates: {delegates}")
    args.output_dir.mkdir(parents=True, exist_ok=True)
    pte = args.output_dir / "hey_muse_xnnpack.pte"
    with pte.open("wb") as output:
        program.write_to_file(output)
    parity = (
        {
            "status": "pending",
            "required": "native verify_parity.py sequential/reset validation",
        }
        if args.defer_runtime_validation
        else validate_runtime(pte, streams, expected)
    )
    args.parity_dir.mkdir(parents=True, exist_ok=True)
    write_floats(args.parity_dir / "frames.f32", streams)
    write_floats(args.parity_dir / "expected.f32", expected)
    manifest = {
        "clips": len(streams),
        "frames_per_clip": streams.shape[1],
        "features_per_frame": 40,
        "posteriors_per_frame": 3,
        "kinds": kinds,
        "encoding": "little-endian float32",
    }
    (args.parity_dir / "manifest.json").write_text(
        json.dumps(manifest, indent=2) + "\n"
    )
    import executorch.version as et_version

    architecture = (
        et_root / "examples/models/mlperf_tiny/streaming_wakeword.py"
    )
    try:
        commit = subprocess.check_output(
            ["git", "-C", str(et_root), "rev-parse", "HEAD"], text=True
        ).strip()
    except subprocess.CalledProcessError:
        commit = None
    sidecar = {
        "schema_version": 1,
        "model": "StreamingWakeWord",
        "backend": "XNNPACK",
        "precision": "fp32",
        "quantization": "none",
        "experimental": True,
        "source": args.source,
        "checkpoint_sha256": sha256_file(args.checkpoint),
        "checkpoint_epoch": checkpoint.get("epoch"),
        "pte_sha256": sha256_file(pte),
        "pte_size": pte.stat().st_size,
        "architecture_sha256": sha256_file(architecture),
        "architecture_commit": commit,
        "exporter_git_version": et_version.git_version,
        "torch_version": torch.__version__,
        "class_names": EXPECTED_CLASSES,
        "wake_phrases": config.get("data", {}).get("wake_phrases", []),
        "frontend": config["frontend"],
        "frontend_header_sha256": sha256_file(args.frontend_header),
        "input_shape": list(streaming_cls.FRAME_SHAPE),
        "output_shape": [1, 3],
        "method": "forward",
        "mutable_histories": mutations,
        "histories_initialized": "zeros",
        "reset": "fresh wheel program/method",
        "delegates": delegates,
        "portable_ops": [op.name + "." + op.overload for op in plan.operators],
        "validation_data_sha256": sha256_file(args.calibration),
        "runtime_vs_eager": parity,
        "notes": CAVEAT,
        "detector": {
            "thresholds": {"low": 0.99, "default": 0.95, "high": 0.90},
            "warmup_frames": 29,
            "smooth_frames": 5,
            "cooldown_frames": 50,
        },
    }
    (args.output_dir / "hey_muse_xnnpack.json").write_text(
        json.dumps(sidecar, indent=2, sort_keys=True) + "\n"
    )
    print(json.dumps({"pte": str(pte), "size": pte.stat().st_size, "parity": parity}))


def main() -> None:
    component = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__, fromfile_prefix_chars="@")
    parser.add_argument(
        "--backend",
        choices=["cadence", "xnnpack"],
        default="cadence",
        help="Export backend: Cadence int8 PTQ for ESP32, or fp32 XNNPACK for Linux.",
    )
    parser.add_argument("--checkpoint", type=Path, required=True)
    parser.add_argument(
        "--executorch-root",
        type=Path,
        default=os.environ.get("EXECUTORCH_ROOT"),
        help="ExecuTorch checkout (or set EXECUTORCH_ROOT)",
    )
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=None,
        help="Output directory (default: <component>/models for cadence; required for xnnpack)",
    )
    parser.add_argument(
        "--frontend-header",
        type=Path,
        default=None,
        help="Frontend header (default: <component>/ww_frontend_config.h, generated, "
        "for cadence; required and validated against the checkpoint for xnnpack)",
    )
    parser.add_argument(
        "--source",
        default="clean-voice training bundle (epoch 11)",
        help="Provenance string recorded in the JSON sidecar",
    )
    parser.add_argument(
        "--calibration",
        type=Path,
        required=True,
        help="Training bundle calibration_frames.pt (cadence: PTQ calibration; "
        "xnnpack: validation corpus)",
    )
    parser.add_argument(
        "--calibration-clips",
        type=int,
        default=96,
        help="Clips to calibrate on, stratified by kind (cadence only; default 96)",
    )
    parser.add_argument("--clips-per-kind", type=int, default=8, help="xnnpack only")
    parser.add_argument(
        "--parity-dir",
        type=Path,
        default=None,
        help="xnnpack only: directory for the sequential validation fixtures",
    )
    parser.add_argument(
        "--defer-runtime-validation",
        action="store_true",
        help="xnnpack only: emit native parity fixtures and mark validation "
        "pending; run verify_parity.py before shipping",
    )
    args = parser.parse_args()

    if args.executorch_root is None:
        parser.error("--executorch-root or EXECUTORCH_ROOT is required")
    if args.backend == "xnnpack":
        if args.output_dir is None:
            parser.error("--output-dir is required for --backend xnnpack")
        if args.frontend_header is None:
            parser.error("--frontend-header is required for --backend xnnpack")
        if args.parity_dir is None:
            parser.error("--parity-dir is required for --backend xnnpack")
        if args.clips_per_kind < 1:
            parser.error("--clips-per-kind must be positive")
    else:
        if args.output_dir is None:
            args.output_dir = component / "models"
        if args.frontend_header is None:
            args.frontend_header = component / "ww_frontend_config.h"
    et_root = args.executorch_root.resolve()
    if args.backend == "cadence" and not (et_root / "version.txt").is_file():
        parser.error(f"not an ExecuTorch checkout: {et_root}")

    import torch

    if args.backend == "xnnpack":
        torch.set_num_threads(1)
    checkpoint = torch.load(args.checkpoint, map_location="cpu", weights_only=False)
    validate_frontend_config(checkpoint["config"]["frontend"])
    streaming_cls, model = load_checkpoint_model(checkpoint, et_root)
    if args.backend == "cadence":
        export_cadence(args, checkpoint, model, streaming_cls, et_root)
    else:
        export_xnnpack(args, checkpoint, model, streaming_cls, et_root)


if __name__ == "__main__":
    main()
